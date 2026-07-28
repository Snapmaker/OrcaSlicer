#include <catch2/catch_test_macros.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

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
    double first_object_peak_z = 0.0;
    double running_peak        = 0.0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
        if (first_object_peak_z != 0.0 || !line.extruding(self)) return; // ignore travels (e.g. start-gcode Z lift)
        if (running_peak > 1.0 && self.z() < 1.0)
            first_object_peak_z = running_peak;
        else
            running_peak = std::max(running_peak, static_cast<double>(self.z()));
    });
    return first_object_peak_z;
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
