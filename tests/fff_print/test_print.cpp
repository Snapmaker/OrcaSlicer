#include <catch2/catch_test_macros.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

SCENARIO("PrintObject: Perimeter generation", "[PrintObject]") {
    GIVEN("20mm cube and default config") {
        WHEN("make_perimeters() is called")  {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, { { "sparse_infill_density", 0 } });
			const PrintObject &object = *print.objects().front();
			THEN("100 layers exist in the model") {
                REQUIRE(object.layers().size() == 100);
            }
            THEN("Every layer in region 0 has 1 island of perimeters") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.entities.size() == 1);
            }
			THEN("Every layer in region 0 has 2 paths in its perimeters list.") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.items_count() == 2);
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
                { "skirt_loops",   2 }
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
        Slic3r::DynamicPrintConfig config = Slic3r::Test::default_print_config();
		config.set_deserialize_strict({
			{ "top_shell_layers",		2 },
			{ "bottom_shell_layers",	1 },
			{ "layer_height",			0.25 }, // get a known number of layers
			{ "initial_layer_print_height", 0.25 }
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
			config.set_deserialize_strict("top_shell_layers", "3");
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
                { "brim_type",                     "outer_only" },
	        	{ "brim_width", 					3 }
	        });
            THEN("Brim Extrusion collection has 2 loops in it") {
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
                { "brim_type",                     "outer_only" },
	        	{ "brim_width", 					6 }
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
                { "brim_type",                     "outer_only" },
	        	{ "brim_width", 					6 },
	        	{ "initial_layer_line_width", 	0.5 }
	        });
			print.process();
            THEN("Brim Extrusion collection has 12 loops in it") {
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 12);
            }
        }
    }
}

// Peak Z of the first object printed in by-object G-code: the highest extrusion before Z drops back
// to the first layer, which with by-object printing only happens at an object change.
static double first_object_peak_z(const std::string &gcode)
{
    double peak         = 0.0;
    double running_peak = 0.0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
        if (peak != 0.0 || !line.extruding(self)) return; // ignore travels (e.g. start-gcode Z lift)
        if (running_peak > 1.0 && self.z() < 1.0)
            peak = running_peak;
        else
            running_peak = std::max(running_peak, static_cast<double>(self.z()));
    });
    return peak;
}

// 20 mm boxes of the given heights, in list order, 80 mm apart in X at the same Y, so they share a
// Y band but are far enough apart for the toolhead radius rule. The rod is lowered to 25 mm, so a
// 30 mm box reaches above it and a 10 mm one does not. init_print() arranges the objects, so they
// are placed afterwards and the model re-applied.
static void place_boxes_in_one_y_band(const std::vector<double> &heights, Print &print, Model &model)
{
    std::vector<TriangleMesh> meshes;
    for (double height : heights)
        meshes.push_back(TriangleMesh(its_make_cube(20, 20, height)));
    DynamicPrintConfig config = Test::default_print_config();
    config.set_deserialize_strict({
        { "print_sequence",                   "by object" },
        { "extruder_clearance_height_to_rod", 25 },
        { "skirt_loops",                      0 },
        { "layer_height",                     0.2 },
        { "initial_layer_print_height",       0.2 },
        { "z_hop",                            0 },
        { "layer_change_gcode",               "G92 E0\n" }, // validate() relative-E reset
        // validate() has one warning slot and later checks overwrite it; keep the default
        // accelerations from triggering the acceleration warning.
        { "default_acceleration",             0 }
    });
    Test::init_print(std::move(meshes), print, model, config);
    for (size_t i = 0; i < model.objects.size(); ++i) {
        ModelInstance *instance = model.objects[i]->instances.front();
        instance->set_offset(Vec3d(90 + 80 * i, 90, instance->get_offset().z()));
    }
    print.apply(model, config);
}

TEST_CASE("Sequential printing accepts a taller object listed first and over the rod height", "[Print]")
{
    Print print;
    Model model;
    place_boxes_in_one_y_band({ 30, 10 }, print, model);

    // The print no longer follows the object list, so the user is told about the taller object,
    // which is held back to clear the rod, and the notification jumps to it.
    StringObjectException warning;
    const StringObjectException error = print.validate(&warning);
    INFO(error.string);
    REQUIRE(error.string.empty());
    CHECK_THAT(warning.string, Catch::Matchers::ContainsSubstring("is printed later than its place in the object list"));
    CHECK(warning.object == print.model().objects.front());
    CHECK(warning.string.find('\n') == std::string::npos); // only the taller object is named
}

TEST_CASE("Sequential printing prints the shorter object first when the taller one is over the rod height", "[Print]")
{
    Print print;
    Model model;
    place_boxes_in_one_y_band({ 30, 10 }, print, model);

    REQUIRE_THAT(first_object_peak_z(Test::gcode(print)), Catch::Matchers::WithinAbs(10.0, 0.3));
}

TEST_CASE("Sequential printing keeps a list order that already clears the rod", "[Print]")
{
    Print print;
    Model model;
    place_boxes_in_one_y_band({ 10, 30 }, print, model);

    StringObjectException warning;
    const StringObjectException error = print.validate(&warning);
    INFO(error.string);
    REQUIRE(error.string.empty());
    CHECK(warning.string.empty());
    REQUIRE_THAT(first_object_peak_z(Test::gcode(print)), Catch::Matchers::WithinAbs(10.0, 0.3));
}

TEST_CASE("Sequential printing still rejects two objects over the rod height that share a Y band", "[Print]")
{
    Print print;
    Model model;
    place_boxes_in_one_y_band({ 30, 30 }, print, model);

    CHECK_THAT(print.validate().string, Catch::Matchers::ContainsSubstring("too tall"));
}

TEST_CASE("Sequential printing keeps copies of one object in their list order", "[Print]")
{
    // Every copy of an object shares its object index, so ordering by that alone leaves the copies to
    // the sort, which shuffles them once there are enough. Nothing here comes near the rod: 200 copies
    // of a part lower than the nozzle, on a grid, so nothing needs reordering.
    std::vector<TriangleMesh> meshes;
    meshes.push_back(TriangleMesh(its_make_cube(5, 5, 2)));
    DynamicPrintConfig config = Test::default_print_config();
    config.set_deserialize_strict({
        { "print_sequence",     "by object" },
        { "skirt_loops",        0 },
        { "layer_change_gcode", "G92 E0\n" }, // validate() relative-E reset
        // validate() has one warning slot and later checks overwrite it; keep the default
        // accelerations from triggering the acceleration warning.
        { "default_acceleration", 0 }
    });
    Print print;
    Model model;
    Test::init_print(std::move(meshes), print, model, config);
    ModelObject *object = model.objects.front();
    const double z      = object->instances.front()->get_offset().z();
    while (object->instances.size() < 200)
        object->add_instance();
    for (size_t k = 0; k < object->instances.size(); ++k)
        object->instances[k]->set_offset(Vec3d(10. + 12. * double(k % 15), 10. + 12. * double(k / 15), z));
    print.apply(model, config);

    StringObjectException warning;
    const StringObjectException error = print.validate(&warning);
    INFO(error.string);
    REQUIRE(error.string.empty());
    CHECK(warning.string.find("is printed later than its place in the object list") == std::string::npos);
    // validate() records the print order on the print's own copy of the model.
    const ModelInstancePtrs &copies = print.model().objects.front()->instances;
    CHECK(std::is_sorted(copies.begin(), copies.end(),
                         [](const ModelInstance *l, const ModelInstance *r) { return l->arrange_order < r->arrange_order; }));
}

TEST_CASE("Sequential printing still lets objects below the rod use the rod height when the lid is lower", "[Print]")
{
    // Some stock profiles put the lid below the rod, such as the Creality Ender-3 V3 KE (lid 34 mm,
    // rod 47 mm). An object that shares a Y band with a later one is checked against the rod height
    // instead of the lid, so two 40 mm boxes side by side in X still slice.
    std::vector<TriangleMesh> meshes;
    for (size_t i = 0; i < 2; ++i)
        meshes.push_back(TriangleMesh(its_make_cube(20, 20, 40)));
    DynamicPrintConfig config = Test::default_print_config();
    config.set_deserialize_strict({
        { "print_sequence",                   "by object" },
        { "extruder_clearance_height_to_lid", 34 },
        { "extruder_clearance_height_to_rod", 47 },
        { "skirt_loops",                      0 },
        { "layer_change_gcode",               "G92 E0\n" } // validate() relative-E reset
    });
    Print print;
    Model model;
    Test::init_print(std::move(meshes), print, model, config);
    // init_print() arranges the objects, so place them side by side in X and re-apply.
    for (size_t i = 0; i < model.objects.size(); ++i)
        model.objects[i]->instances.front()->set_offset(Vec3d(90 + 80 * i, 90, 0));
    print.apply(model, config);

    const std::string error = print.validate().string;
    INFO(error);
    CHECK(error.empty());
}

TEST_CASE("Sequential printing counts only the part of an object above the rod height", "[Print]")
{
    // Each object is a 2 mm slab carrying one fin on its front (-Y) or back (+Y) edge, with the rod at
    // 25 mm. The slabs share a Y band, but the rod can only hit the fins. Each fin is checked against
    // the other slab's footprint widened by half of extruder_clearance_radius, not against the other
    // fin: facing away from each other, neither fin is within that range; facing each other, both are,
    // and no print order works. With one of each, only the first slab's fin is, so the second slab has to
    // print first.
    enum class Fins { FacingAway, FacingEachOther, OneOfEach };
    const Fins fins = GENERATE(Fins::FacingAway, Fins::FacingEachOther, Fins::OneOfEach);
    // A raised object (snap to the bed turned off) whose own height is below the rod but whose top is
    // above it must get the same rule.
    const bool raised = GENERATE(false, true);
    // A per-object skirt that reaches the rod is a wall around the whole footprint, so the rod can hit
    // all of it: the plate is refused whichever way the fins face, as it was before this rule. That is
    // a draft shield (as tall as the object) or a skirt whose skirt_height reaches the rod.
    enum class Skirt { None, DraftShield, Tall };
    const Skirt skirt = GENERATE(Skirt::None, Skirt::DraftShield, Skirt::Tall);
    CAPTURE(int(fins), raised, int(skirt));

    const double fin_height = raised ? 20. : 29.5;
    const double raise_by   = raised ? 10. : 0.;
    auto slab_with_fin = [fin_height](bool fin_at_back) {
        TriangleMesh slab(its_make_cube(40, 30, 2));
        TriangleMesh fin(its_make_cube(8, 2, fin_height));
        fin.translate(16.f, fin_at_back ? 28.f : 0.f, 0.f);
        slab.merge(fin);
        return slab;
    };
    // The first slab sits further back (+Y), so its front fin faces the second slab.
    std::vector<TriangleMesh> meshes;
    meshes.push_back(slab_with_fin(fins == Fins::FacingAway));
    meshes.push_back(slab_with_fin(fins == Fins::FacingEachOther));

    DynamicPrintConfig config = Test::default_print_config();
    config.set_deserialize_strict({
        { "print_sequence",                   "by object" },
        { "extruder_clearance_height_to_rod", 25 },
        { "skirt_loops",                      0 },
        { "layer_change_gcode",               "G92 E0\n" } // validate() relative-E reset
    });
    if (skirt == Skirt::DraftShield)
        config.set_deserialize_strict({
            { "draft_shield", "enabled" },
            { "skirt_type",   "perobject" },
            { "skirt_loops",  1 }
        });
    else if (skirt == Skirt::Tall)
        config.set_deserialize_strict({
            { "skirt_type",   "perobject" },
            { "skirt_loops",  1 },
            { "skirt_height", 160 } // layers, well above the 25 mm rod at the default max layer height
        });
    Print print;
    Model model;
    Test::init_print(std::move(meshes), print, model, config);
    // init_print() arranges the objects, so place them (raised, if asked) and re-apply.
    const Vec2d centers[] = { Vec2d(60, 100), Vec2d(190, 60) };
    for (size_t i = 0; i < 2; ++i) {
        ModelInstance *instance = model.objects[i]->instances.front();
        instance->set_offset(Vec3d(centers[i].x(), centers[i].y(), instance->get_offset().z() + raise_by));
    }
    print.apply(model, config);
    if (raised) {
        REQUIRE(print.objects().front()->height() <= scale_(25.));
        REQUIRE(print.objects().front()->max_z() > scale_(25.));
    }

    const std::string error = print.validate().string;
    INFO(error);
    const bool refused = fins == Fins::FacingEachOther || skirt != Skirt::None;
    CHECK((error.find("too tall") != std::string::npos) == refused);
    if (fins == Fins::OneOfEach && !refused) {
        // validate() records the print order on the print's own copy of the model.
        const ModelObjectPtrs &objects = print.model().objects;
        CHECK(objects[1]->instances.front()->arrange_order < objects[0]->instances.front()->arrange_order);
    }
}

// Two 2 mm slabs side by side in X, each with one fin above the rod, the fins facing away from each
// other so only the fins can meet the rod. init_print() arranges the objects, so they are placed
// afterwards, the first slab further back (+Y), and the model re-applied.
static void init_slabs_with_fins_facing_away(double fin_height, double low_fin_height, Print &print, Model &model,
                                             const DynamicPrintConfig &config)
{
    auto slab = [&](bool fin_at_back) {
        TriangleMesh mesh(its_make_cube(40, 30, 2));
        TriangleMesh fin(its_make_cube(8, 2, fin_height));
        fin.translate(16.f, fin_at_back ? 28.f : 0.f, 0.f);
        mesh.merge(fin);
        if (low_fin_height > 0.) {
            // A second, lower fin on the opposite edge, facing the other slab.
            TriangleMesh low_fin(its_make_cube(8, 2, low_fin_height));
            low_fin.translate(16.f, fin_at_back ? 0.f : 28.f, 0.f);
            mesh.merge(low_fin);
        }
        return mesh;
    };
    std::vector<TriangleMesh> meshes;
    meshes.push_back(slab(true));
    meshes.push_back(slab(false));
    Test::init_print(std::move(meshes), print, model, config);
    const Vec2d centers[] = { Vec2d(60, 100), Vec2d(190, 60) };
    for (size_t i = 0; i < 2; ++i) {
        ModelInstance *instance = model.objects[i]->instances.front();
        instance->set_offset(Vec3d(centers[i].x(), centers[i].y(), instance->get_offset().z()));
    }
    print.apply(model, config);
}

TEST_CASE("Sequential printing counts a raft toward what reaches the rod", "[Print]")
{
    // Each slab has a tall fin facing away from the other slab and a lower fin facing it. Without a raft
    // the lower fin stays under the rod and the plate validates; a raft lifts it above the rod, and then
    // the lower fins meet.
    const bool raft = GENERATE(false, true);
    CAPTURE(raft);

    DynamicPrintConfig config = Test::default_print_config();
    config.set_deserialize_strict({
        { "print_sequence",                   "by object" },
        { "extruder_clearance_height_to_rod", 25 },
        { "skirt_loops",                      0 },
        { "raft_layers",                      raft ? 3 : 0 },
        { "layer_change_gcode",               "G92 E0\n" } // validate() relative-E reset
    });
    Print print;
    Model model;
    init_slabs_with_fins_facing_away(29.5, 24.8, print, model, config);
    if (raft)
        REQUIRE(print.objects().front()->slicing_parameters().object_print_z_min > 0.2);

    const std::string error = print.validate().string;
    INFO(error);
    CHECK((error.find("too tall") != std::string::npos) == raft);
}

TEST_CASE("Sequential printing keeps the rod height for an object only its raft lifts above the rod", "[Print]")
{
    // With the lid below the rod, an object that shares a Y band with a later one is checked against the
    // rod height instead of the lid. The fins face away from each other and stay below the rod on their
    // own; only the raft lifts them above it. The too-tall check compares the top without the raft, so the
    // slabs keep the footprint test and the rod height, and the plate still validates.
    DynamicPrintConfig config = Test::default_print_config();
    config.set_deserialize_strict({
        { "print_sequence",                   "by object" },
        { "extruder_clearance_height_to_lid", 20 },
        { "extruder_clearance_height_to_rod", 25 },
        { "skirt_loops",                      0 },
        { "raft_layers",                      3 },
        { "layer_change_gcode",               "G92 E0\n" } // validate() relative-E reset
    });
    Print print;
    Model model;
    init_slabs_with_fins_facing_away(24.8, 0., print, model, config);
    REQUIRE(24.8 + print.objects().front()->slicing_parameters().object_print_z_min > 25.);

    const std::string error = print.validate().string;
    INFO(error);
    CHECK(error.empty());
}

TEST_CASE("Sequential printing applies an instance's rotation to the part above the rod", "[Print]")
{
    // The fins face away from each other in the meshes, but each instance is turned 180 degrees about its
    // own center, so on the bed they face each other and no print order works.
    DynamicPrintConfig config = Test::default_print_config();
    config.set_deserialize_strict({
        { "print_sequence",                   "by object" },
        { "extruder_clearance_height_to_rod", 25 },
        { "skirt_loops",                      0 },
        { "layer_change_gcode",               "G92 E0\n" } // validate() relative-E reset
    });
    Print print;
    Model model;
    init_slabs_with_fins_facing_away(29.5, 0., print, model, config);
    for (ModelObject *object : model.objects) {
        ModelInstance *instance = object->instances.front();
        const Vec3d    center   = object->instance_bounding_box(0).center();
        const Vec3d    offset   = instance->get_offset();
        instance->set_rotation(Vec3d(0., 0., PI));
        // A half turn about the instance origin moves the object too; shift it back so it turns in place.
        instance->set_offset(Vec3d(2. * center.x() - offset.x(), 2. * center.y() - offset.y(), offset.z()));
        REQUIRE((object->instance_bounding_box(0).center() - center).norm() < EPSILON);
    }
    print.apply(model, config);

    const std::string error = print.validate().string;
    INFO(error);
    CHECK(error.find("too tall") != std::string::npos);
}

