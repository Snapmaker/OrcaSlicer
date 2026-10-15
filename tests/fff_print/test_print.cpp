#include <catch2/catch_test_macros.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/GCodeReader.hpp"

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

namespace {
void check_support_tower_gcode(Print& print, unsigned int support_tool, bool expect_tower, bool support_on_tower)
{
    const std::string gcode = Test::gcode(print);
    GCodeReader       reader;
    reader.apply_config(print.config());
    unsigned int tool = 0;
    std::string  role;
    size_t       interface_moves = 0, tower_moves = 0, support_tower_moves = 0, wrong_interface_moves = 0;
    reader.parse_buffer(gcode, [&](GCodeReader& reader, const GCodeReader::GCodeLine& line) {
        const std::string_view cmd = line.cmd();
        if (cmd.size() > 1 && cmd.front() == 'T' && std::isdigit(cmd[1]))
            tool = std::stoul(std::string(cmd.substr(1)));
        if (line.comment().substr(0, 5) == "TYPE:")
            role = line.comment().substr(5);
        else if (line.comment().substr(0, 10) == " FEATURE: ")
            role = line.comment().substr(10);
        if (!line.extruding(reader) || !(line.has_x() || line.has_y()))
            return;
        if (role == "Support interface") {
            ++interface_moves;
            wrong_interface_moves += tool != support_tool;
        }
        if (role == "Prime tower") {
            ++tower_moves;
            support_tower_moves += tool == support_tool;
        }
    });
    CHECK(interface_moves > 0);
    CHECK(wrong_interface_moves == 0);
    CHECK((tower_moves > 0) == expect_tower);
    CHECK((support_tower_moves > 0) == support_on_tower);
}
} // namespace

TEST_CASE("Prime tower can exclude support-only filaments", "[PrimeTower]")
{
    for (bool include_support : {true, false}) {
        CAPTURE(include_support);
        DynamicPrintConfig config = Test::default_print_config();
        config.set_deserialize_strict({
            {"enable_prime_tower", true},
            {"prime_tower_include_support", include_support},
            {"single_extruder_multi_material", false},
            {"nozzle_diameter", "0.4,0.4"},
            {"filament_diameter", "1.75,1.75"},
            {"enable_support", true},
            {"support_filament", 1},
            {"support_interface_filament", 2},
            {"support_interface_top_layers", 2},
            {"support_type", "normal(auto)"},
            {"flush_volumes_matrix", "0,140,140,0"},
        });
        config.set_num_extruders(2);
        config.set_num_filaments(2);
        Print print;
        print.is_BBL_printer() = false;
        Model model;
        Test::init_print({TestMesh::overhang}, print, model, config);
        CHECK(print.has_wipe_tower() == include_support);
        CHECK(print.is_prime_tower_extruder(0));
        CHECK(print.is_prime_tower_extruder(1) == include_support);
        print.process();
        CHECK(print.wipe_tower_data().tool_ordering.has_wipe_tower() == include_support);
        if (!include_support) {
            CHECK(print.wipe_tower_data().tool_changes.empty());
            bool has_interface = false;
            for (const auto* layer : print.objects().front()->support_layers()) {
                const auto paths = layer->support_fills.flatten();
                for (const auto* path : paths.entities)
                    has_interface |= path->role() == erSupportMaterialInterface;
            }
            CHECK(has_interface);
        }
        check_support_tower_gcode(print, 1, include_support, include_support);
        if (!include_support) {
            config.set_deserialize_strict("prime_tower_include_support", "1");
            print.apply(model, config);
            check_support_tower_gcode(print, 1, true, true);
        }
    }
}

TEST_CASE("Prime tower keeps model filaments and shared-nozzle purging", "[PrimeTower]")
{
    for (int support_tool : {0, 1, 2}) {
        for (bool priming : {false, true}) {
            CAPTURE(support_tool, priming);
            const int          model_tool       = (support_tool + 1) % 3;
            const int          other_model_tool = (support_tool + 2) % 3;
            DynamicPrintConfig config           = Test::default_print_config();
            config.set_deserialize_strict({
                {"enable_prime_tower", true},
                {"prime_tower_include_support", false},
                {"single_extruder_multi_material", false},
                {"single_extruder_multi_material_priming", priming},
                {"nozzle_diameter", "0.4,0.4,0.4"},
                {"filament_diameter", "1.75,1.75,1.75"},
                {"enable_support", true},
                {"support_filament", model_tool + 1},
                {"support_interface_filament", support_tool + 1},
                {"support_interface_top_layers", 2},
                {"support_type", "normal(auto)"},
                {"wipe_tower_filament", support_tool + 1},
                {"flush_volumes_matrix", "0,140,140,140,0,140,140,140,0"},
            });
            config.set_num_extruders(3);
            config.set_num_filaments(3);
            Print print;
            print.is_BBL_printer() = false;
            Model model;
            Test::init_print({TestMesh::overhang, TestMesh::cube_20x20x20}, print, model, config);
            model.objects.front()->config.set_key_value("extruder", new ConfigOptionInt(model_tool + 1));
            model.objects.back()->config.set_key_value("extruder", new ConfigOptionInt(other_model_tool + 1));
            print.apply(model, config);
            CHECK(print.has_wipe_tower());
            CHECK(print.is_prime_tower_extruder(model_tool));
            CHECK(print.is_prime_tower_extruder(other_model_tool));
            CHECK_FALSE(print.is_prime_tower_extruder(support_tool));
            print.process();
            REQUIRE_FALSE(print.wipe_tower_data().tool_changes.empty());
            for (const auto& layer : print.wipe_tower_data().tool_changes)
                for (const auto& change : layer)
                    if (!change.gcode.empty())
                        CHECK(change.new_tool != support_tool);
            check_support_tower_gcode(print, support_tool, true, false);

            config.set_deserialize_strict("single_extruder_multi_material", "1");
            print.apply(model, config);
            CHECK(print.is_prime_tower_extruder(support_tool));
            CHECK(print.has_wipe_tower());
            config.set_deserialize_strict("single_extruder_multi_material", "0");
            model.objects.front()->config.set_key_value("extruder", new ConfigOptionInt(support_tool + 1));
            print.apply(model, config);
            CHECK(print.is_prime_tower_extruder(support_tool));
        }
    }
}
