#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentCliGates.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

#include <algorithm>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

SCENARIO("PrintObject: Perimeter generation", "[PrintObject]") {
    GIVEN("20mm cube and default config") {
        WHEN("make_perimeters() is called")  {
            Slic3r::Print print;
            // Pin the Slic3r-era geometry this scenario was written against:
            // 0.5 + 65*0.3 = 20mm -> 66 layers, and 3 classic perimeter loops.
            Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
                { "sparse_infill_density",               0 },
                { "nozzle_diameter",            0.6 },
                { "layer_height",               0.3 },
                { "initial_layer_print_height", 0.5 },
                { "wall_loops",                 3 }
            });
			const PrintObject &object = *print.objects().front();
			THEN("67 layers exist in the model") {
                REQUIRE(object.layers().size() == 66);
            }
            THEN("Every layer in region 0 has 1 island of perimeters") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.entities.size() == 1);
            }
            THEN("Every layer in region 0 has 3 paths in its perimeters list.") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.items_count() == 3);
            }
        }
    }
}

SCENARIO("Print: Skirt generation", "[Print]") {
    GIVEN("20mm cube and default config") {
        WHEN("Skirts is set to 2 loops")  {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
            	{ "skirt_height", 	1 },
        		{ "skirt_distance", 1 },
        		{ "skirt_loops", 		2 }
            });
            THEN("Skirt Extrusion collection has 2 loops in it") {
                REQUIRE(print.skirt().items_count() == 2);
                REQUIRE(print.skirt().flatten().entities.size() == 2);
            }
        }
    }
}

SCENARIO("Print: Changing number of solid surfaces does not cause all surfaces to become internal.", "[Print]") {
    GIVEN("sliced 20mm cube and config with top_solid_surfaces = 2 and bottom_solid_surfaces = 1") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
		config.set_deserialize_strict({
			{ "top_shell_layers",		2 },
			{ "bottom_shell_layers",	1 },
			{ "layer_height",			0.25 }, // get a known number of layers
			{ "initial_layer_print_height",		0.25 }
			});
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::Test::init_print({TestMesh::cube_20x20x20}, print, model, config);
        // Precondition: Ensure that the model has 2 solid top layers (39, 38)
        // and one solid bottom layer (0).
		auto test_is_solid_infill = [&print](size_t obj_id, size_t layer_id) {
		    const Layer &layer = *(print.objects().at(obj_id)->get_layer((int)layer_id));
		    // iterate over all of the regions in the layer
		    for (const LayerRegion *region : layer.regions()) {
		        // for each region, iterate over the fill surfaces
		        for (const Surface &surface : region->fill_surfaces.surfaces)
		            CHECK(surface.is_solid());
		    }
		};
        print.process();
        test_is_solid_infill(0,  0); // should be solid
        test_is_solid_infill(0, 79); // should be solid
        test_is_solid_infill(0, 78); // should be solid
        WHEN("Model is re-sliced with top_solid_layers == 3") {
			config.set("top_shell_layers", 3);
			print.apply(model, config);
            print.process();
            THEN("Print object does not have 0 solid bottom layers.") {
                test_is_solid_infill(0, 0);
            }
            AND_THEN("Print object has 3 top solid layers") {
                test_is_solid_infill(0, 79);
                test_is_solid_infill(0, 78);
                test_is_solid_infill(0, 77);
            }
        }
    }
}

SCENARIO("Print: Brim generation", "[Print]") {
    GIVEN("20mm cube and default config, 1mm first layer width") {
        WHEN("Brim is set to 3mm")  {
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
	        	{ "initial_layer_line_width", 	1 },
	        	{ "brim_width", 					3 },
	        	// brim_type defaults to auto_brim in this fork, which ignores
	        	// brim_width for a shape that needs no brim (a plain cube).
	        	{ "brim_type", 					"outer_only" }
	        });
            THEN("Brim Extrusion collection has 2 loops in it") {
            // FORK BEHAVIOUR: Brim.cpp:385 quantises the requested brim_width DOWN to an
            // EVEN number of flow widths - floor(brim_width / flowWidth / 2) * flowWidth * 2 -
            // so the loop count is 2*floor(width / (2*flow)), not the upstream width/flow.
            // flow 1mm: 3mm -> 2 loops (not 3), 6mm -> 6; flow 0.5mm: 6mm -> 12 (not 14).
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 2);
            }
        }
        WHEN("Brim is set to 6mm")  {
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
	        	{ "initial_layer_line_width", 	1 },
	        	{ "brim_width", 					6 },
	        	{ "brim_type", 					"outer_only" }
	        });
            THEN("Brim Extrusion collection has 6 loops in it") {
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 6);
            }
        }
        WHEN("Brim is set to 6mm, extrusion width 0.5mm")  {
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
	        	{ "initial_layer_line_width", 	1 },
	        	{ "brim_width", 					6 },
	        	{ "brim_type", 					"outer_only" },
	        	{ "initial_layer_line_width", 	0.5 }
	        });
			print.process();
            THEN("Brim Extrusion collection has 12 loops in it") {
            // FORK BEHAVIOUR: Brim.cpp:385 quantises the requested brim_width DOWN to an
            // EVEN number of flow widths - floor(brim_width / flowWidth / 2) * flowWidth * 2 -
            // so the loop count is 2*floor(width / (2*flow)), not the upstream width/flow.
            // flow 1mm: 3mm -> 2 loops (not 3), 6mm -> 6; flow 0.5mm: 6mm -> 12 (not 14).
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 12);
            }
        }
    }
}

// Orca #15924: inner-outer-inner (IOI) wall order. After the first layer, a 3-wall island is
// supposed to print the second internal wall, then the outer wall, then the first internal wall.
// Arachne's old centreline-distance test ignored variable width, so a widened odd centre line on a
// narrow wall was not grouped with its neighbours and the outer wall printed first.
namespace {

std::vector<int> island_wall_insets(const ExtrusionEntity *island)
{
    std::vector<int> insets;
    auto take = [&](const ExtrusionEntity *entity) {
        if (entity->inset_idx >= 0)
            insets.push_back(entity->inset_idx);
    };
    if (island->is_collection()) {
        for (const ExtrusionEntity *entity : static_cast<const ExtrusionEntityCollection *>(island)->entities)
            take(entity);
    } else {
        take(island);
    }
    return insets;
}

std::string insets_to_string(const std::vector<int> &insets)
{
    std::ostringstream os;
    for (size_t i = 0; i < insets.size(); ++i) {
        if (i)
            os << ',';
        os << insets[i];
    }
    return os.str();
}

TriangleMesh thin_ring(double wall_mm, double height_mm = 1.2)
{
    // Square-section ring: a hole plus an outer contour, matching the upstream thin-ring case.
    const double inner = 8.0;
    const double outer = inner + wall_mm;
    std::vector<Vec2d> profile{{inner, 0.}, {outer, 0.}, {outer, height_mm}, {inner, height_mm}};
    return TriangleMesh(its_make_revolved(profile, 64));
}

std::vector<int> sandwich_core(const std::vector<int> &insets)
{
    std::vector<int> core;
    for (int inset : insets) {
        if (inset == 0 || inset == 1 || inset == 2)
            core.push_back(inset);
    }
    return core;
}

bool has_insets_0_1_2(const std::vector<int> &insets)
{
    bool has0 = false, has1 = false, has2 = false;
    for (int inset : insets) {
        has0 = has0 || inset == 0;
        has1 = has1 || inset == 1;
        has2 = has2 || inset == 2;
    }
    return has0 && has1 && has2;
}

int count_ioi_sandwiches(const Print &print)
{
    int sandwiches = 0;
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.layer_count() > 1);
    for (const Layer *layer : object.layers()) {
        if (layer->id() == 0)
            continue; // IOI is disabled on the first layer
        for (const LayerRegion *region : layer->regions()) {
            for (const ExtrusionEntity *island : region->perimeters.entities) {
                const std::vector<int> insets = island_wall_insets(island);
                if (!has_insets_0_1_2(insets))
                    continue;
                const std::vector<int> core = sandwich_core(insets);
                CAPTURE(layer->id(), insets_to_string(insets), insets_to_string(core));
                // Inner-outer-inner prints the second internal wall (inset 2) before the outer wall.
                REQUIRE_FALSE(core.empty());
                REQUIRE(core.front() == 2);
                ++sandwiches;
            }
        }
    }
    return sandwiches;
}

} // namespace

TEST_CASE("Inner-outer-inner wall order starts with the second internal wall on a cube", "[PrintObject][IOI]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({Slic3r::make_cube(20., 20., 1.2)}, print, {
        { "wall_generator",             wall_generator },
        { "wall_sequence",              "inner-outer-inner wall" },
        { "wall_loops",                 3 },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "nozzle_diameter",            0.4 },
        { "line_width",                 0.4 },
        { "outer_wall_line_width",      0.4 },
        { "inner_wall_line_width",      0.4 },
        { "only_one_wall_top",          0 },
        { "sparse_infill_density",      0 },
        { "enable_support",             0 },
        { "brim_width",                 0 },
        { "detect_overhang_wall",       0 },
        { "offset_layers",              0 },
        { "precise_outer_wall",         0 },
        { "spiral_mode",                0 }
    });

    REQUIRE(count_ioi_sandwiches(print) > 0);
}

TEST_CASE("Arachne inner-outer-inner wall order holds on a narrow wall", "[PrintObject][IOI][Arachne]")
{
    // Upstream #15924 failed on a thin RING (outer contour + hole), not a solid strip. A solid
    // strip is one island; sandwich reordering still fires even when the width-aware touching
    // test misses the widened centre line. A ring has two outers, so grouping has to attach the
    // odd centre line or one side prints outer-first.
    //
    // Discriminator (old centreline test vs this PR, 0.4 mm line, 5 walls):
    //   1.6 mm  — fewer than three insets, sandwich never runs
    //   1.8 mm  — sandwich still fires without the width-aware test
    //   2.0 mm  — FAILS without the fix (first wall is inset 0), PASSES with it
    const double wall_mm = 2.0;
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({thin_ring(wall_mm)}, print, {
        { "wall_generator",             "arachne" },
        { "wall_sequence",              "inner-outer-inner wall" },
        { "wall_loops",                 5 },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "nozzle_diameter",            0.4 },
        { "line_width",                 0.4 },
        { "outer_wall_line_width",      0.4 },
        { "inner_wall_line_width",      0.4 },
        { "only_one_wall_top",          0 },
        { "sparse_infill_density",      0 },
        { "enable_support",             0 },
        { "brim_width",                 0 },
        { "detect_overhang_wall",       0 },
        { "offset_layers",              0 },
        { "precise_outer_wall",         0 },
        { "spiral_mode",                0 }
    });

    REQUIRE(count_ioi_sandwiches(print) > 0);
}

TEST_CASE("outer_wall_filament is counted by slicing and matches the Prepare plate helper", "[Print][WipeTower]")
{
    // Prepare's plate set used to miss outer_wall_filament, so the estimate had depth 0 while
    // Print::extruders() still listed both filaments and built a real tower. Lock them together:
    // volume extruder 1 plus resolve_outer_wall_filament must equal print.extruders() as 1-based
    // project ids. A SINGLE apply (the CLI / GUI Slice-all path) must keep the tower: Print::apply
    // re-checks enable_prime_tower after regions exist.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4, 0.4, 0.4};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    config.option<ConfigOptionBool>("enable_prime_tower")->value   = true;
    config.option<ConfigOptionBool>("enable_support")->value       = false;
    config.option<ConfigOptionBool>("spiral_mode")->value          = false;
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    config.option<ConfigOptionBool>("purge_in_prime_tower")->value           = false;
    config.option<ConfigOptionInt>("outer_wall_filament")->value   = 2;
    config.option<ConfigOptionInt>("wall_filament")->value         = 1;
    config.option<ConfigOptionInt>("sparse_infill_filament")->value = 1;
    config.option<ConfigOptionInt>("solid_infill_filament")->value  = 1;
    config.option<ConfigOptionFloats>("wipe_tower_x")->values       = {15.};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values       = {15.};
    config.option<ConfigOptionFloat>("prime_tower_width")->value    = 35.;
    config.set_key_value("printer_model", new ConfigOptionString("Snapmaker U1"));

    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);

    REQUIRE(print.default_region_config().outer_wall_filament.value == 2);
    REQUIRE(print.extruders() == std::vector<unsigned int>{0, 1});
    REQUIRE(print.has_wipe_tower());
    REQUIRE(print.config().enable_prime_tower.value);

    std::vector<int> prepare_ids;
    append_object_plate_filament_ids(*model.objects.front(), config, prepare_ids);
    std::sort(prepare_ids.begin(), prepare_ids.end());
    prepare_ids.erase(std::unique(prepare_ids.begin(), prepare_ids.end()), prepare_ids.end());
    prepare_ids.erase(std::remove(prepare_ids.begin(), prepare_ids.end(), 0), prepare_ids.end());

    std::vector<int> slice_ids;
    for (unsigned int e : print.extruders())
        slice_ids.push_back(int(e) + 1);
    REQUIRE(prepare_ids == slice_ids);

    const std::string gcode = Test::gcode(print);
    REQUIRE(gcode.find("WIPE_TOWER_START") != std::string::npos);
    REQUIRE(gcode.find("CP TOOLCHANGE") != std::string::npos);
}

TEST_CASE("a single-filament plate still has no wipe tower after one apply", "[Print][WipeTower]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(1);
    config.set_num_filaments(1);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4};
    config.option<ConfigOptionBool>("enable_prime_tower")->value   = true;
    config.option<ConfigOptionBool>("enable_support")->value       = false;
    config.option<ConfigOptionInt>("outer_wall_filament")->value   = 1;

    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);
    REQUIRE(print.extruders().size() == 1);
    REQUIRE_FALSE(print.config().enable_prime_tower.value);
}

TEST_CASE("Precise Seam helper does not turn the prime tower on", "[Print][PreciseSeam]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4, 0.4, 0.4};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    config.option<ConfigOptionBool>("enable_prime_tower")->value   = true;
    config.option<ConfigOptionBool>("enable_support")->value       = false;
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    config.option<ConfigOptionBool>("purge_in_prime_tower")->value           = false;
    config.option<ConfigOptionFloats>("wipe_tower_x")->values      = {15.};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values      = {15.};
    config.option<ConfigOptionFloat>("prime_tower_width")->value   = 35.;
    config.set_key_value("printer_model", new ConfigOptionString("Snapmaker U1"));

    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);
    ModelVolume *helper = model.objects.front()->add_volume(make_cube(2., 2., 2.));
    helper->set_type(ModelVolumeType::PRECISE_SEAM_LEFT);
    helper->config.set("extruder", 3);
    helper->config.set("wall_filament", 4);
    print.apply(model, config);

    REQUIRE(helper->is_precise_seam());
    CHECK_FALSE(volume_contributes_feature_filaments(*helper));

    std::vector<int> plate_ids;
    append_object_plate_filament_ids(*model.objects.front(), config, plate_ids);
    std::sort(plate_ids.begin(), plate_ids.end());
    plate_ids.erase(std::unique(plate_ids.begin(), plate_ids.end()), plate_ids.end());
    CHECK(std::find(plate_ids.begin(), plate_ids.end(), 3) == plate_ids.end());
    CHECK(std::find(plate_ids.begin(), plate_ids.end(), 4) == plate_ids.end());
    REQUIRE(print.extruders().size() == 1);
    REQUIRE_FALSE(print.has_wipe_tower());
    REQUIRE_FALSE(print.config().enable_prime_tower.value);
}

TEST_CASE("BBL two-volume filament slice still has a tower after one apply", "[Print][WipeTower][BBLIdentity]")
{
    // Volume extruders are visible as soon as objects exist, so a single apply of a 2-volume
    // BBL plate must match main: the post-region normalize is a no-op when used_filaments>1.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionBool>("enable_prime_tower")->value   = true;
    config.option<ConfigOptionBool>("enable_support")->value       = false;
    config.option<ConfigOptionBool>("spiral_mode")->value          = false;
    config.option<ConfigOptionFloats>("wipe_tower_x")->values      = {15.};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values      = {15.};
    config.option<ConfigOptionFloat>("prime_tower_width")->value   = 35.;
    config.set_deserialize_strict({{"brim_type", "no_brim"}, {"skirt_loops", "0"}, {"wipe_tower_wall_type", "rectangle"}});

    Print print;
    Model model;
    ModelObject *first = model.add_object();
    first->name        = "cube-a.stl";
    first->add_volume(mesh(TestMesh::cube_20x20x20));
    first->add_instance()->set_offset(Vec3d(80., 40., 0.));
    first->ensure_on_bed();
    ModelObject *second = model.add_object();
    second->name        = "cube-b.stl";
    second->add_volume(mesh(TestMesh::cube_20x20x20));
    second->add_instance()->set_offset(Vec3d(120., 40., 0.));
    second->ensure_on_bed();
    second->volumes.front()->config.set("extruder", 2);

    print.apply(model, config);
    print.is_BBL_printer() = true;
    print.set_status_silent();

    REQUIRE(print.extruders() == std::vector<unsigned int>{0, 1});
    REQUIRE(print.has_wipe_tower());
    REQUIRE(print.config().enable_prime_tower.value);

    const std::string gcode = Test::gcode(print);
    REQUIRE(gcode.find("CP TOOLCHANGE") != std::string::npos);
    if (const char *path = std::getenv("BBL_GCODE_OUT")) {
        std::ofstream out(path, std::ios::binary);
        out << gcode;
    }
}

TEST_CASE("turning the tower on clears independent_support_layer_height before slicing params", "[Print][WipeTower]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4, 0.4, 0.4};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    config.option<ConfigOptionBool>("enable_prime_tower")->value   = true;
    config.option<ConfigOptionBool>("enable_support")->value       = true;
    config.option<ConfigOptionBool>("independent_support_layer_height")->value = true;
    config.option<ConfigOptionBool>("spiral_mode")->value          = false;
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    config.option<ConfigOptionBool>("purge_in_prime_tower")->value           = false;
    config.option<ConfigOptionInt>("outer_wall_filament")->value   = 2;
    config.option<ConfigOptionInt>("wall_filament")->value         = 1;
    config.option<ConfigOptionFloat>("layer_height")->value        = 0.2;
    config.option<ConfigOptionFloat>("support_top_z_distance")->value = 0.15;
    config.option<ConfigOptionFloats>("wipe_tower_x")->values       = {15.};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values       = {15.};
    config.option<ConfigOptionFloat>("prime_tower_width")->value    = 35.;
    config.set_key_value("printer_model", new ConfigOptionString("Snapmaker U1"));

    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);

    REQUIRE(print.has_wipe_tower());
    REQUIRE(print.config().enable_prime_tower.value);
    REQUIRE_FALSE(print.config().independent_support_layer_height.value);
    const SlicingParameters &sp = print.objects().front()->slicing_parameters();
    REQUIRE(sp.valid);
    // islh true would leave the 0.15 mm gap; clearing it rounds to layer_height 0.2.
    REQUIRE(sp.gap_support_object == Approx(0.2));

    const std::string gcode = Test::gcode(print);
    REQUIRE(gcode.find("WIPE_TOWER_START") != std::string::npos);
}

TEST_CASE("BBL AMS slot 3 single object is filament 3 only", "[Print][WipeTower][BBLIdentity]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4, 0.4, 0.4};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    config.option<ConfigOptionBool>("enable_prime_tower")->value   = true;
    config.option<ConfigOptionBool>("enable_support")->value       = false;
    config.option<ConfigOptionBool>("spiral_mode")->value          = false;
    config.option<ConfigOptionFloats>("wipe_tower_x")->values      = {15.};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values      = {15.};
    config.option<ConfigOptionFloat>("prime_tower_width")->value   = 35.;
    config.set_deserialize_strict({{"brim_type", "no_brim"}, {"skirt_loops", "0"}, {"wipe_tower_wall_type", "rectangle"}});

    Print print;
    Model model;
    ModelObject *object = model.add_object();
    object->name        = "cube-ams3.stl";
    object->add_volume(mesh(TestMesh::cube_20x20x20));
    object->add_instance()->set_offset(Vec3d(80., 40., 0.));
    object->ensure_on_bed();
    object->config.set("extruder", 3);

    print.apply(model, config);
    print.is_BBL_printer() = true;
    print.set_status_silent();

    std::vector<int> plate_ids;
    append_object_plate_filament_ids(*model.objects.front(), config, plate_ids);
    std::sort(plate_ids.begin(), plate_ids.end());
    plate_ids.erase(std::unique(plate_ids.begin(), plate_ids.end()), plate_ids.end());
    plate_ids.erase(std::remove(plate_ids.begin(), plate_ids.end(), 0), plate_ids.end());
    REQUIRE(plate_ids == std::vector<int>{3});
    REQUIRE(print.extruders() == std::vector<unsigned int>{2});
    REQUIRE_FALSE(print.has_wipe_tower());

    const std::string gcode = Test::gcode(print);
    if (const char *path = std::getenv("BBL_GCODE_SLOT3_OUT")) {
        std::ofstream out(path, std::ios::binary);
        out << gcode;
    }
}

namespace {

std::string mixed_ab_definition()
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, {"#FF0000", "#00FF00"});
    mgr.mixed_filaments().front().manual_pattern = MixedFilamentManager::normalize_manual_pattern("12");
    return mgr.serialize_custom_entries();
}

unsigned int mixed_ab_virtual_id()
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, {"#FF0000", "#00FF00"});
    return mgr.filament_id_from_mixed_index(0, 2);
}

DynamicPrintConfig two_filament_config(bool by_object, bool mixed_walls)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        {"nozzle_diameter",            "0.4,0.4"},
        {"filament_diameter",          "1.75,1.75"},
        {"enable_prime_tower",         "0"},
        {"enable_support",             "0"},
        {"sparse_infill_density",      "0"},
        {"layer_height",               "0.3"},
        {"initial_layer_print_height", "0.3"},
        {"skirt_loops",                "0"},
        {"brim_type",                  "no_brim"},
        {"print_sequence",             by_object ? "by object" : "by layer"},
        {"wall_loops",                 "2"},
        {"gcode_comments",             "1"},
        {"single_extruder_multi_material", "1"},
    });
    config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#00FF00"};
    if (mixed_walls) {
        const unsigned int virtual_id = mixed_ab_virtual_id();
        config.set_deserialize_strict({
            {"wall_filament",          std::to_string(virtual_id)},
            {"sparse_infill_filament", std::to_string(virtual_id)},
            {"solid_infill_filament",  std::to_string(virtual_id)},
        });
        config.set("mixed_filament_definitions", mixed_ab_definition());
    } else {
        config.set_deserialize_strict({
            {"wall_filament",          "1"},
            {"sparse_infill_filament", "1"},
            {"solid_infill_filament",  "1"},
        });
    }
    return config;
}

std::string mixed_ac_pattern13_definition()
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, {"#FF0000", "#00FF00", "#0000FF"});
    mgr.mixed_filaments().front().manual_pattern = MixedFilamentManager::normalize_manual_pattern("13");
    return mgr.serialize_custom_entries();
}

unsigned int mixed_ac_virtual_id()
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, {"#FF0000", "#00FF00", "#0000FF"});
    return mgr.filament_id_from_mixed_index(0, 3);
}

DynamicPrintConfig three_filament_config(bool by_object)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    config.set_deserialize_strict({
        {"nozzle_diameter",            "0.4,0.4,0.4"},
        {"filament_diameter",          "1.75,1.75,1.75"},
        {"enable_prime_tower",         "0"},
        {"enable_support",             "0"},
        {"sparse_infill_density",      "0"},
        {"layer_height",               "0.3"},
        {"initial_layer_print_height", "0.3"},
        {"skirt_loops",                "0"},
        {"brim_type",                  "no_brim"},
        {"print_sequence",             by_object ? "by object" : "by layer"},
        {"wall_loops",                 "2"},
        {"gcode_comments",             "1"},
        {"single_extruder_multi_material", "1"},
        {"wall_filament",              "1"},
        {"sparse_infill_filament",     "1"},
        {"solid_infill_filament",      "1"},
    });
    config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#00FF00", "#0000FF"};
    config.set("mixed_filament_definitions", mixed_ac_pattern13_definition());
    return config;
}

std::string export_print_gcode(Print &print)
{
    print.set_status_silent();
    print.process();
    const boost::filesystem::path out = scratch_path(".gcode");
    print.export_gcode(out.string(), nullptr, nullptr);
    boost::nowide::ifstream in(out.string());
    std::string             gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    boost::nowide::remove(out.string().c_str());
    return gcode;
}

// Strip the lines that legitimately differ across runs (generation timestamp and M73
// estimates) so two exports of the same job can be compared byte-for-byte.
std::string strip_gcode_timestamps(const std::string &gcode)
{
    std::string out;
    out.reserve(gcode.size());
    size_t pos = 0;
    while (pos < gcode.size()) {
        const size_t eol  = gcode.find('\n', pos);
        const size_t end  = eol == std::string::npos ? gcode.size() : eol + 1;
        const std::string line = gcode.substr(pos, end - pos);
        const bool volatile_line =
            line.find("; generated by") != std::string::npos ||
            line.find("M73") != std::string::npos ||
            line.find("estimated") != std::string::npos ||
            line.find("total estimated time") != std::string::npos;
        if (!volatile_line)
            out += line;
        pos = end;
    }
    return out;
}

size_t count_toolchange(const std::string &gcode, unsigned int extruder_id)
{
    const std::string needle = "T" + std::to_string(extruder_id);
    size_t            count  = 0;
    size_t            pos    = 0;
    while (pos < gcode.size()) {
        const size_t eol  = gcode.find('\n', pos);
        const size_t end  = eol == std::string::npos ? gcode.size() : eol;
        std::string  line = gcode.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        // "T<n>" or "T<n> ; change extruder" — comments are optional when gcode_comments is off.
        if (line.compare(0, needle.size(), needle) == 0 &&
            (line.size() == needle.size() || line[needle.size()] == ' ' || line[needle.size()] == ';'))
            ++count;
        pos = end == gcode.size() ? gcode.size() : end + 1;
    }
    return count;
}

struct AutoGenerateGuard
{
    AutoGenerateGuard() : previous(MixedFilamentManager::auto_generate_enabled())
    {
        MixedFilamentManager::set_auto_generate_enabled(true);
    }
    ~AutoGenerateGuard() { MixedFilamentManager::set_auto_generate_enabled(previous); }
    bool previous;
};

struct SupportExtrusionHit
{
    double   z    = 0.;
    unsigned tool = 0;
};

std::vector<SupportExtrusionHit> parse_support_extrusions(const std::string &gcode)
{
    std::vector<SupportExtrusionHit> hits;
    double                           z    = 0.;
    unsigned                         tool = 0;
    size_t                           pos  = 0;
    while (pos < gcode.size()) {
        const size_t      eol  = gcode.find('\n', pos);
        const size_t      end  = eol == std::string::npos ? gcode.size() : eol;
        std::string       line = gcode.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if ((line.compare(0, 2, "G0") == 0 || line.compare(0, 2, "G1") == 0) &&
            (line.size() == 2 || line[2] == ' ' || line[2] == 'X' || line[2] == 'Y' || line[2] == 'Z' || line[2] == 'F' ||
             line[2] == 'E')) {
            const size_t zpos = line.find('Z');
            if (zpos != std::string::npos && zpos + 1 < line.size()) {
                char *endptr = nullptr;
                const double parsed = std::strtod(line.c_str() + zpos + 1, &endptr);
                if (endptr != line.c_str() + zpos + 1)
                    z = parsed;
            }
        }
        if (!line.empty() && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1]))) {
            tool = unsigned(std::strtoul(line.c_str() + 1, nullptr, 10));
        }
        if (line.find("support material") != std::string::npos)
            hits.push_back({z, tool});
        pos = end == gcode.size() ? gcode.size() : end + 1;
    }
    return hits;
}

bool support_hit_at_z(const std::vector<SupportExtrusionHit> &hits, double print_z, unsigned expected_tool)
{
    for (const SupportExtrusionHit &hit : hits) {
        if (std::abs(hit.z - print_z) < 0.05 && hit.tool == expected_tool)
            return true;
    }
    return false;
}

std::vector<SupportExtrusionHit> parse_wall_infill_extrusions(const std::string &gcode)
{
    std::vector<SupportExtrusionHit> hits;
    double                           z    = 0.;
    unsigned                         tool = 0;
    size_t                           pos  = 0;
    while (pos < gcode.size()) {
        const size_t      eol  = gcode.find('\n', pos);
        const size_t      end  = eol == std::string::npos ? gcode.size() : eol;
        std::string       line = gcode.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if ((line.compare(0, 2, "G0") == 0 || line.compare(0, 2, "G1") == 0) &&
            (line.size() == 2 || line[2] == ' ' || line[2] == 'X' || line[2] == 'Y' || line[2] == 'Z' || line[2] == 'F' ||
             line[2] == 'E')) {
            const size_t zpos = line.find('Z');
            if (zpos != std::string::npos && zpos + 1 < line.size()) {
                char *endptr = nullptr;
                const double parsed = std::strtod(line.c_str() + zpos + 1, &endptr);
                if (endptr != line.c_str() + zpos + 1)
                    z = parsed;
            }
        }
        if (!line.empty() && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1]))) {
            tool = unsigned(std::strtoul(line.c_str() + 1, nullptr, 10));
        }
        const size_t comment = line.find(';');
        if (comment != std::string::npos && line.compare(0, 2, "G1") == 0 && line.find('E') < comment) {
            const std::string desc = line.substr(comment);
            const bool wall_or_infill =
                (desc.find("perimeter") != std::string::npos || desc.find("infill") != std::string::npos) &&
                desc.find("width") == std::string::npos && desc.find("move to") == std::string::npos;
            if (wall_or_infill)
                hits.push_back({z, tool});
        }
        pos = end == gcode.size() ? gcode.size() : end + 1;
    }
    return hits;
}

bool wall_infill_hit_at_z_on_scheduled(const std::vector<SupportExtrusionHit> &hits,
                                       double                                  print_z,
                                       const LayerTools                       &lt)
{
    bool saw = false;
    for (const SupportExtrusionHit &hit : hits) {
        if (std::abs(hit.z - print_z) > 0.05)
            continue;
        saw = true;
        if (!lt.has_extruder(hit.tool))
            return false;
    }
    return saw;
}

} // namespace

// S1: ByObject + mixed virtual wall_filament used to SIGSEGV in GCode::needs_retraction
// (writer().extruder() dangling because Print::extruders() clamped the virtual id to 0, so
// GCodeWriter never registered the mixed component-B physical extruder).
TEST_CASE("ByObject mixed virtual wall filament exports with physical toolchanges", "[Print][MixedFilament][GCode]")
{
    REQUIRE(mixed_ab_virtual_id() == 3);

    const bool by_object = GENERATE(true, false);
    DYNAMIC_SECTION((by_object ? "by object" : "by layer"))
    {
        Print print;
        Model model;
        init_print({TestMesh::cube_20x20x20}, print, model, two_filament_config(by_object, true));
        REQUIRE(print.mixed_filament_manager().is_mixed(3, 2));

        // Export first: on main this SIGSEGVs in GCode::needs_retraction before any
        // post-export check can run. Print::extruders() is asserted afterwards.
        std::string gcode;
        REQUIRE_NOTHROW(gcode = export_print_gcode(print));
        REQUIRE_FALSE(gcode.empty());

        const std::vector<unsigned int> used = print.extruders();
        REQUIRE(std::find(used.begin(), used.end(), 0u) != used.end());
        REQUIRE(std::find(used.begin(), used.end(), 1u) != used.end());
        if (const char *dir = std::getenv("DUMP_GCODE_DIR")) {
            boost::nowide::ofstream dump(std::string(dir) + (by_object ? "/mixed_byobject.gcode" : "/mixed_bylayer.gcode"));
            dump << strip_gcode_timestamps(gcode);
        }
        const size_t t0 = count_toolchange(gcode, 0);
        const size_t t1 = count_toolchange(gcode, 1);
        INFO("T0=" << t0 << " T1=" << t1);
        REQUIRE(t0 + t1 >= 2);
        REQUIRE(t0 >= 1);
        REQUIRE(t1 >= 1);
    }
}

TEST_CASE("Non-mixed two-filament G-code is unchanged by mixed-id expansion", "[Print][GCode]")
{
    const bool by_object = GENERATE(true, false);
    DYNAMIC_SECTION((by_object ? "by object" : "by layer"))
    {
        Print print;
        Model model;
        init_print({TestMesh::cube_20x20x20}, print, model, two_filament_config(by_object, false));
        const std::vector<unsigned int> used = print.extruders();
        REQUIRE(used == std::vector<unsigned int>{0});

        std::string gcode;
        REQUIRE_NOTHROW(gcode = export_print_gcode(print));
        REQUIRE_FALSE(gcode.empty());
        REQUIRE(count_toolchange(gcode, 0) >= 1);
        REQUIRE(count_toolchange(gcode, 1) == 0);
        REQUIRE(count_toolchange(gcode, 2) == 0);
        REQUIRE_FALSE(strip_gcode_timestamps(gcode).empty());
        if (const char *dir = std::getenv("DUMP_GCODE_DIR")) {
            boost::nowide::ofstream dump(std::string(dir) + (by_object ? "/nonmixed_byobject.gcode" : "/nonmixed_bylayer.gcode"));
            dump << strip_gcode_timestamps(gcode);
        }
    }
}

// Pattern "13" names physical 1 and 3. Expanding those tokens in Print::extruders(), or
// unioning the later object's ToolOrdering, is each enough to register T0 and T2. Component B
// (filament 2) is unused and must stay out of Print::extruders() — over-including it would
// trip temp-compat / CLI hard-block / bed-max / tower validation.
TEST_CASE("ByObject later object mixed pattern 13 exports T0 and T2", "[Print][MixedFilament][GCode]")
{
    REQUIRE(mixed_ac_virtual_id() == 4);

    Print print;
    Model model;
    DynamicPrintConfig config = three_filament_config(true);
    init_print({TestMesh::cube_20x20x20, TestMesh::cube_20x20x20}, print, model, config);
    REQUIRE(model.objects.size() == 2);
    REQUIRE(print.mixed_filament_manager().is_mixed(4, 3));

    model.objects[1]->volumes[0]->config.set_key_value("wall_filament", new ConfigOptionInt(4));
    model.objects[1]->volumes[0]->config.set_key_value("sparse_infill_filament", new ConfigOptionInt(4));
    model.objects[1]->volumes[0]->config.set_key_value("solid_infill_filament", new ConfigOptionInt(4));
    print.apply(model, config);
    print.validate();

    const std::vector<unsigned int> used = print.extruders();
    REQUIRE(std::find(used.begin(), used.end(), 0u) != used.end());
    REQUIRE(std::find(used.begin(), used.end(), 2u) != used.end());
    REQUIRE(std::find(used.begin(), used.end(), 1u) == used.end());

    std::string gcode;
    REQUIRE_NOTHROW(gcode = export_print_gcode(print));
    REQUIRE_FALSE(gcode.empty());
    INFO("T0=" << count_toolchange(gcode, 0) << " T2=" << count_toolchange(gcode, 2));
    REQUIRE(count_toolchange(gcode, 0) >= 1);
    REQUIRE(count_toolchange(gcode, 2) >= 1);
    REQUIRE(count_toolchange(gcode, 1) == 0);
}

TEST_CASE("ByObject mixed support filament exports with physical toolchanges", "[Print][MixedFilament][GCode]")
{
    DynamicPrintConfig config = two_filament_config(true, true);
    config.set_deserialize_strict({
        {"wall_filament",                 "1"},
        {"sparse_infill_filament",        "1"},
        {"solid_infill_filament",         "1"},
        {"enable_support",                "1"},
        {"support_type",                  "normal(auto)"},
        {"support_filament",               "3"},
        {"support_interface_filament",     "3"},
        {"support_on_build_plate_only",    "0"},
    });

    Print print;
    Model model;
    init_print({TestMesh::overhang}, print, model, config);
    REQUIRE(print.mixed_filament_manager().is_mixed(3, 2));

    const std::vector<unsigned int> used = print.extruders();
    REQUIRE(std::find(used.begin(), used.end(), 0u) != used.end());
    REQUIRE(std::find(used.begin(), used.end(), 1u) != used.end());

    std::string gcode;
    REQUIRE_NOTHROW(gcode = export_print_gcode(print));
    REQUIRE_FALSE(gcode.empty());
    INFO("T0=" << count_toolchange(gcode, 0) << " T1=" << count_toolchange(gcode, 1));
    REQUIRE(count_toolchange(gcode, 0) >= 1);
    REQUIRE(count_toolchange(gcode, 1) >= 1);
}

TEST_CASE("Auto mixed support with A/B layer heights matches ToolOrdering", "[Print][MixedFilament][GCode]")
{
    AutoGenerateGuard auto_mixed;
    const bool        by_object = GENERATE(true, false);
    DYNAMIC_SECTION((by_object ? "by object" : "by layer"))
    {
        DynamicPrintConfig config = two_filament_config(by_object, false);
        config.set_deserialize_strict({
            {"enable_support",             "1"},
            {"support_type",               "normal(auto)"},
            {"support_filament",           "3"},
            {"support_interface_filament", "3"},
            {"support_on_build_plate_only","0"},
            {"mixed_color_layer_height_a", "0.3"},
            {"mixed_color_layer_height_b", "0.6"},
        });

        Print print;
        Model model;
        init_print({TestMesh::overhang}, print, model, config);
        REQUIRE(print.mixed_filament_manager().is_mixed(3, 2));
        REQUIRE_FALSE(print.mixed_filament_manager().mixed_filaments().front().custom);

        std::string gcode;
        REQUIRE_NOTHROW(gcode = export_print_gcode(print));
        REQUIRE_FALSE(gcode.empty());
        const std::vector<SupportExtrusionHit> hits = parse_support_extrusions(gcode);

        const PrintObject &object = *print.objects().front();
        REQUIRE_FALSE(object.support_layers().empty());
        ToolOrdering ordering = by_object ? ToolOrdering(object, (unsigned int) -1) : print.tool_ordering();

        bool saw_height_cycle_disagreement = false;
        size_t filled_support_layers       = 0;
        for (const SupportLayer *sl : object.support_layers()) {
            if (sl == nullptr || sl->support_fills.entities.empty())
                continue;
            ++filled_support_layers;
            const LayerTools  &lt     = ordering.tools_for_layer(sl->print_z);
            const unsigned int via_to = lt.resolve_mixed_1based_at(3, float(sl->print_z), float(sl->height), &object);
            REQUIRE(via_to >= 1);
            REQUIRE(lt.has_extruder(via_to - 1));

            unsigned int via_direct = 3;
            if (lt.mixed_mgr != nullptr)
                via_direct = lt.mixed_mgr->resolve(3, lt.num_physical, lt.layer_index, float(sl->print_z), float(sl->height), false, &object);
            if (via_direct != via_to)
                saw_height_cycle_disagreement = true;

            INFO("print_z=" << sl->print_z << " scheduled_tool=" << (via_to - 1) << " direct=" << (via_direct >= 1 ? via_direct - 1 : -1)
                            << " hits=" << hits.size());
            REQUIRE(support_hit_at_z(hits, sl->print_z, via_to - 1));
        }
        REQUIRE(filled_support_layers >= 1);
        REQUIRE(saw_height_cycle_disagreement);
    }
}

TEST_CASE("Non-mixed multi-object ByObject G-code dump", "[Print][GCode]")
{
    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20, TestMesh::cube_20x20x20}, print, model, two_filament_config(true, false));
    REQUIRE(print.extruders() == std::vector<unsigned int>{0});

    std::string gcode;
    REQUIRE_NOTHROW(gcode = export_print_gcode(print));
    REQUIRE_FALSE(gcode.empty());
    REQUIRE(count_toolchange(gcode, 0) >= 1);
    REQUIRE(count_toolchange(gcode, 1) == 0);
    if (const char *dir = std::getenv("DUMP_GCODE_DIR")) {
        boost::nowide::ofstream dump(std::string(dir) + "/nonmixed_byobject_multi.gcode");
        dump << strip_gcode_timestamps(gcode);
    }
}

// Catch2 treats "[n]" in a test name as a tag, so keep the name free of brackets.
TEST_CASE("ByObject mixed pattern bracket-3 token exports T0 and T2", "[Print][MixedFilament][GCode]")
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, {"#FF0000", "#00FF00", "#0000FF"});
    mgr.mixed_filaments().front().manual_pattern = MixedFilamentManager::normalize_manual_pattern("1[3]");
    REQUIRE(mgr.filament_id_from_mixed_index(0, 3) == 4);

    DynamicPrintConfig config = three_filament_config(true);
    config.set("mixed_filament_definitions", mgr.serialize_custom_entries());
    config.set_deserialize_strict({
        {"wall_filament",          "4"},
        {"sparse_infill_filament", "4"},
        {"solid_infill_filament",  "4"},
    });

    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);
    REQUIRE(print.mixed_filament_manager().is_mixed(4, 3));
    const std::vector<unsigned int> used = print.extruders();
    REQUIRE(std::find(used.begin(), used.end(), 0u) != used.end());
    REQUIRE(std::find(used.begin(), used.end(), 2u) != used.end());
    REQUIRE(std::find(used.begin(), used.end(), 1u) == used.end());

    std::string gcode;
    REQUIRE_NOTHROW(gcode = export_print_gcode(print));
    REQUIRE_FALSE(gcode.empty());
    REQUIRE(count_toolchange(gcode, 0) >= 1);
    REQUIRE(count_toolchange(gcode, 2) >= 1);
    REQUIRE(count_toolchange(gcode, 1) == 0);
}

// ByLayer shares LayerTools across objects. This plate is taller first, shorter
// second. Stamping every entry in collect_extruders let the later shorter object
// overwrite the taller object's layer_index above the short top, so mixed
// walls/infill resolved to a tool that was not in layer_tools.extruders.
TEST_CASE("ByLayer mixed walls on a tall-then-short plate stay on scheduled tools", "[Print][MixedFilament][GCode]")
{
    REQUIRE(mixed_ab_virtual_id() == 3);

    DynamicPrintConfig config = two_filament_config(false, true);
    config.set_deserialize_strict({{"sparse_infill_density", "20"}});

    Print print;
    Model model;
    init_print({make_cube(20., 20., 20.), make_cube(20., 20., 6.)}, print, model, config);
    REQUIRE(print.objects().size() == 2);
    REQUIRE(print.mixed_filament_manager().is_mixed(3, 2));

    std::string gcode;
    REQUIRE_NOTHROW(gcode = export_print_gcode(print));
    REQUIRE_FALSE(gcode.empty());

    const PrintObject &tall = *print.objects().front();
    const PrintObject &shrt = *print.objects().back();
    REQUIRE_FALSE(tall.layers().empty());
    REQUIRE_FALSE(shrt.layers().empty());
    const double short_top = shrt.layers().back()->print_z;
    REQUIRE(tall.layers().back()->print_z > short_top + 0.5);

    const ToolOrdering                    &ordering = print.tool_ordering();
    const std::vector<SupportExtrusionHit> hits     = parse_wall_infill_extrusions(gcode);
    REQUIRE_FALSE(hits.empty());

    size_t layers_above_short = 0;
    for (const Layer *layer : tall.layers()) {
        if (layer == nullptr || layer->print_z <= short_top + 0.05)
            continue;
        bool has_walls_or_infill = false;
        for (const LayerRegion *region : layer->regions()) {
            if (region != nullptr && region->has_extrusions()) {
                has_walls_or_infill = true;
                break;
            }
        }
        if (!has_walls_or_infill)
            continue;
        ++layers_above_short;
        const LayerTools &lt = ordering.tools_for_layer(layer->print_z);
        INFO("print_z=" << layer->print_z << " layer_index=" << lt.layer_index);
        REQUIRE(wall_infill_hit_at_z_on_scheduled(hits, layer->print_z, lt));
    }
    REQUIRE(layers_above_short >= 1);
}

TEST_CASE("Exporting a sliced print again gives the same G-code", "[Print][GCode][Regression]")
{
    const int instances = GENERATE(1, 3);
    CAPTURE(instances);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"layer_change_gcode", "G92 E0"},
        {"skirt_loops", "0"},
        {"brim_type", "no_brim"},
        // ipadstand is ~98 mm in X; 3 copies need a plate wider than the default 200 mm.
        {"printable_area", "0x0,400x0,400x400,0x400"},
    });
    auto check_reexport = [&](TestMesh mesh) {
        Model        model  = Test::model("reexport", Test::mesh(mesh));
        ModelObject *object = model.objects.front();
        const double spacing = (mesh == TestMesh::ipadstand) ? 110. : 40.;
        for (int i = 1; i < instances; ++i)
            object->add_instance()->set_offset(Vec3d(spacing * i, 0., 0.));
        object->ensure_on_bed();

        Print print;
        print.auto_assign_extruders(object);
        print.apply(model, config);
        const StringObjectException err = print.validate();
        INFO(err.string);
        REQUIRE(err.string.empty());
        print.set_status_silent();

        const std::string first  = strip_gcode_timestamps(Test::gcode(print));
        const std::string second = strip_gcode_timestamps(Test::gcode(print));

        // Shows the first differing line on failure.
        const size_t diff       = std::mismatch(first.begin(), first.end(), second.begin(), second.end()).first - first.begin();
        const size_t line_start = diff == 0 ? 0 : first.rfind('\n', diff - 1) + 1;
        const size_t line_end   = first.find('\n', diff);
        INFO("first export:  " << first.substr(line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start));
        INFO("second export: " << second.substr(line_start, second.find('\n', diff) == std::string::npos ?
                                                                 std::string::npos :
                                                                 second.find('\n', diff) - line_start));
        REQUIRE(first == second);
        REQUIRE(first.size() == second.size());
    };

    SECTION("infill reversed by chaining") {
        // Upstream Orca #16025 uses ipadstand + gyroid so chain_and_reorder actually
        // reverses infill. The cube fixture passed on main and did not exercise that hunk.
        config.set_deserialize_strict({
            {"sparse_infill_pattern", "gyroid"},
            {"sparse_infill_density", "20"},
        });
        check_reexport(TestMesh::ipadstand);
    }
    SECTION("support reversed by chaining") {
        config.set_deserialize_strict({
            {"enable_support", "1"},
            {"support_interface_pattern", "concentric"},
        });
        check_reexport(TestMesh::overhang);
    }
}

namespace {

std::shared_ptr<MultiNozzleUtils::LayeredNozzleGroupResult> make_single_nozzle_group()
{
    std::vector<MultiNozzleUtils::NozzleInfo> nozzles(1);
    nozzles[0].diameter    = "0.4";
    nozzles[0].volume_type = nvtStandard;
    nozzles[0].extruder_id = 0;
    nozzles[0].group_id    = 0;
    auto created = MultiNozzleUtils::LayeredNozzleGroupResult::create({0}, nozzles, {0u});
    REQUIRE(created);
    return std::make_shared<MultiNozzleUtils::LayeredNozzleGroupResult>(std::move(*created));
}

} // namespace

TEST_CASE("Replacing the nozzle grouping result recasts the layered cache and bumps generation", "[Print]")
{
    Print print;
    REQUIRE(print.get_layered_nozzle_group_result() == nullptr);
    const size_t generation0 = print.config_index_generation();

    print.set_nozzle_group_result(nullptr);
    REQUIRE(print.config_index_generation() != generation0);
    REQUIRE(print.get_layered_nozzle_group_result() == nullptr);

    auto grouped = make_single_nozzle_group();
    const size_t generation1 = print.config_index_generation();
    print.set_nozzle_group_result(grouped);
    REQUIRE(print.config_index_generation() != generation1);
    REQUIRE(print.get_layered_nozzle_group_result() == grouped);
    REQUIRE(print.get_layered_nozzle_group_result().get() == print.get_nozzle_group_result().get());
}

TEST_CASE("A flow-variant change clears the layered nozzle-group cache", "[Print]")
{
    Print              print;
    Model              model;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    init_print({TestMesh::cube_20x20x20}, print, model, config);

    print.set_nozzle_group_result(make_single_nozzle_group());
    REQUIRE(print.get_layered_nozzle_group_result() != nullptr);
    const size_t generation = print.config_index_generation();

    config.set_deserialize_strict({{"filament_volume_type", FLOW_MODE_HIGH_FLOW}});
    print.apply(model, config);
    REQUIRE(print.config_index_generation() != generation);
    CHECK(print.get_layered_nozzle_group_result() == nullptr);
}
