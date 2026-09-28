#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

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
