#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include "libslic3r/Print.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/GCodeReader.hpp"

#include "test_data.hpp"

#include <algorithm>
#include <regex>

using namespace Slic3r;
using namespace Slic3r::Test;

std::regex perimeters_regex("G1 X[-0-9.]* Y[-0-9.]* E[-0-9.]* ; perimeter");
std::regex infill_regex("G1 X[-0-9.]* Y[-0-9.]* E[-0-9.]* ; infill");
std::regex skirt_regex("G1 X[-0-9.]* Y[-0-9.]* E[-0-9.]* ; skirt");

TEST_CASE("Snapmaker 2.0 exports finalize native header metadata", "[PrintGCode][SnapmakerHeader]")
{
    Print print;
    Model model;
    print.is_BBL_printer() = false;
    const double retraction = GENERATE(0.0, 0.8);
    Test::init_print({TestMesh::cube_20x20x20}, print, model,
                     {{"printer_model", "Snapmaker A350"}, {"machine_start_gcode", ""}, {"remaining_times", true},
                      {"retraction_length", std::to_string(retraction)}, {"retract_length_toolchange", "2.5"},
                      {"nozzle_temperature_initial_layer", "215"}, {"curr_bed_type", "High Temp Plate"},
                      {"hot_plate_temp_initial_layer", "55"}});
    const std::string output = Test::gcode(print);

    REQUIRE(output.find(";Header Start\n;file_total_lines: ") == 0);
    const std::string prefix = ";Header Start\n;file_total_lines: ";
    const size_t line_count = std::stoull(output.substr(prefix.size()));
    CHECK(line_count == static_cast<size_t>(std::count(output.begin(), output.end(), '\n')));
    const size_t header_end = output.find(";Header End\n");
    REQUIRE(header_end != std::string::npos);
    const std::string header = output.substr(0, header_end + std::string(";Header End\n").size());
    INFO(header);
    CHECK(header_end < output.find("; HEADER_BLOCK_START\n"));
    const size_t time = output.find(";estimated_time(s): ");
    REQUIRE(time < header_end);
    CHECK(std::stoull(output.substr(time + std::string(";estimated_time(s): ").size())) > 0);
    CHECK(output.find(";layer_number: ") < header_end);
    CHECK(output.find("_GP_SNAPMAKER_PRINTING_TIME_PLACEHOLDER") == std::string::npos);
    auto value = [&header](const std::string& key) {
        const std::string prefix = ";" + key + ": ";
        const size_t pos = header.find(prefix);
        REQUIRE(pos != std::string::npos);
        return std::stod(header.substr(pos + prefix.size()));
    };
    // GcodeParser.checkParamsRequired() needs a line count AND an explicitly supplied retraction distance, including zero.
    CHECK(value("file_total_lines") > 0);
    CHECK_THAT(value("Extruder 0 Retraction Distance"), WithinAbs(retraction, 0.001));
    CHECK_THAT(value("Extruder 0 Switch Retraction Distance"), WithinAbs(2.5, 0.001));
    CHECK(value("nozzle_temperature(\xC2\xB0" "C)") == 215);
    CHECK(value("build_plate_temperature(\xC2\xB0" "C)") == 55);
    for (const char axis : std::string("xyz")) {
        const std::string suffix = std::string(1, axis) + "(mm)";
        CHECK(value("min_" + suffix) <= value("max_" + suffix));
    }
    CHECK_THAT(value("max_z(mm)"), WithinAbs(20.0, 0.01));
}

TEST_CASE("Snapmaker dual headers retain distinct tool retraction metadata", "[PrintGCode][SnapmakerHeader]")
{
    Print print;
    Model model;
    print.is_BBL_printer() = false;
    Test::init_print({TestMesh::cube_20x20x20}, print, model,
                     {{"printer_model", "Snapmaker A350 Dual"}, {"machine_start_gcode", ""},
                      {"nozzle_diameter", "0.4,0.4"}, {"filament_diameter", "1.75,1.75"}, {"filament_type", "PLA,PLA"},
                      {"retraction_length", "0.6,1.2"}, {"retract_length_toolchange", "2,3"},
                      {"nozzle_temperature_initial_layer", "210,220"}});
    const std::string output = Test::gcode(print);
    const std::string header = output.substr(0, output.find(";Header End\n"));
    CHECK(header.find(";Extruder 0 Retraction Distance: 0.600\n") != std::string::npos);
    CHECK(header.find(";Extruder 1 Retraction Distance: 1.200\n") != std::string::npos);
    CHECK(header.find(";Extruder 0 Switch Retraction Distance: 2.000\n") != std::string::npos);
    CHECK(header.find(";Extruder 1 Switch Retraction Distance: 3.000\n") != std::string::npos);
    CHECK(header.find(";nozzle_temperature(\xC2\xB0" "C): 210\n") != std::string::npos);
    CHECK(header.find(";nozzle_1_temperature(\xC2\xB0" "C): 220\n") != std::string::npos);
}

TEST_CASE("Other Snapmaker models do not acquire the SM2 header", "[PrintGCode][SnapmakerHeader]")
{
    Print print;
    Model model;
    print.is_BBL_printer() = false;
    const std::string printer_model = GENERATE("Snapmaker U1", "Snapmaker Artisan");
    Test::init_print({TestMesh::cube_20x20x20}, print, model, {{"printer_model", printer_model}, {"machine_start_gcode", ""}});
    const std::string output = Test::gcode(print);
    CHECK(output.find(";Header Start\n") == std::string::npos);
    CHECK(output.find(";Extruder 0 Retraction Distance:") == std::string::npos);
}

SCENARIO( "PrintGCode basic functionality", "[PrintGCode]") {
    GIVEN("A default configuration and a print test object") {
        WHEN("the output is executed with no support material") {
            Slic3r::Print print;
            Slic3r::Model model;
            Slic3r::Test::init_print({TestMesh::cube_20x20x20}, print, model, {
                { "layer_height",					0.2 },
                { "initial_layer_print_height",		0.2 },
                { "initial_layer_line_width",	0 },
                { "gcode_comments",					true },
                { "machine_start_gcode",			"" }
                });
            std::string gcode = Slic3r::Test::gcode(print);
            THEN("Some text output is generated.") {
                REQUIRE(gcode.size() > 0);
            }
            THEN("Exported text contains slic3r version") {
                REQUIRE(gcode.find("generated by Snapmaker Orca") != std::string::npos);
            }
            //THEN("Exported text contains git commit id") {
            //    REQUIRE(gcode.find("; Git Commit") != std::string::npos);
            //    REQUIRE(gcode.find(SLIC3R_BUILD_ID) != std::string::npos);
            //}
            THEN("Exported text contains extrusion statistics.") {
                REQUIRE(gcode.find("; external perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; solid infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; top infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; support material extrusion width") == std::string::npos);
                REQUIRE(gcode.find("; first layer extrusion width") == std::string::npos);
            }
            THEN("Exported text does not contain cooling markers (they were consumed)") {
                REQUIRE(gcode.find(";_EXTRUDE_SET_SPEED") == std::string::npos);
            }

            THEN("GCode preamble is emitted.") {
                REQUIRE(gcode.find("G21") != std::string::npos);
            }

            THEN("Config options emitted for print config, default region config, default object config") {
                REQUIRE(gcode.find("; first_layer_temperature") != std::string::npos);
                REQUIRE(gcode.find("; layer_height") != std::string::npos);
                REQUIRE(gcode.find("; sparse_infill_density") != std::string::npos);
            }
            THEN("Infill is emitted.") {
                std::smatch has_match;
                REQUIRE(std::regex_search(gcode, has_match, infill_regex));
            }
            THEN("Perimeters are emitted.") {
				std::smatch has_match;
                REQUIRE(std::regex_search(gcode, has_match, perimeters_regex));
            }
            THEN("Skirt is emitted.") {
                std::smatch has_match;
                REQUIRE(std::regex_search(gcode, has_match, skirt_regex));
            }
            THEN("final Z height is 20mm") {
                double final_z = 0.0;
                GCodeReader reader;
                reader.apply_config(print.config());
                reader.parse_buffer(gcode, [&final_z] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
                    final_z = std::max<double>(final_z, static_cast<double>(self.z())); // record the highest Z point we reach
                });
                REQUIRE_THAT(final_z, WithinRel(20.4, 0.001));
            }
        }
        WHEN("output is executed with complete objects and two differently-sized meshes") {
            Slic3r::Print print;
            Slic3r::Model model;
            Slic3r::Test::init_print({TestMesh::cube_20x20x20,TestMesh::cube_20x20x20}, print, model, {
                { "initial_layer_line_width",    0 },
                { "initial_layer_print_height",     0.3 },
                { "layer_height",                   0.2 },
                { "enable_support",                 false },
                { "raft_layers",                    0 },
                { "print_sequence",                 "by object" },
                { "gcode_comments",                 true },
                { "printing_by_object_gcode",       "; between-object-gcode" }
                });
            std::string gcode = Slic3r::Test::gcode(print);
            THEN("Some text output is generated.") {
                REQUIRE(gcode.size() > 0);
            }
            THEN("Infill is emitted.") {
                std::smatch has_match;
                REQUIRE(std::regex_search(gcode, has_match, infill_regex));
            }
            THEN("Perimeters are emitted.") {
                std::smatch has_match;
                REQUIRE(std::regex_search(gcode, has_match, perimeters_regex));
            }
            THEN("Skirt is emitted.") {
                std::smatch has_match;
                REQUIRE(std::regex_search(gcode, has_match, skirt_regex));
            }
            THEN("Between-object-gcode is emitted.") {
                REQUIRE(gcode.find("; between-object-gcode") != std::string::npos);
            }
            THEN("final Z height is 20.4mm") {
                double final_z = 0.0;
                GCodeReader reader;
                reader.apply_config(print.config());
                reader.parse_buffer(gcode, [&final_z] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
                    final_z = std::max(final_z, static_cast<double>(self.z())); // record the highest Z point we reach
                });
                REQUIRE_THAT(final_z, WithinRel(20.5, 0.001));
            }
            THEN("Z height resets on object change") {
                double final_z = 0.0;
                bool reset = false;
                GCodeReader reader;
                reader.apply_config(print.config());
                reader.parse_buffer(gcode, [&final_z, &reset] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
                    if (final_z > 0 && std::abs(self.z() - 0.3) < 0.01 ) { // saw higher Z before this, now it's lower
                        reset = true;
                    } else {
                        final_z = std::max(final_z, static_cast<double>(self.z())); // record the highest Z point we reach
                    }
                });
                REQUIRE(reset == true);
            }
            THEN("Shorter object is printed before taller object.") {
                double final_z = 0.0;
                bool reset = false;
                GCodeReader reader;
                reader.apply_config(print.config());
                reader.parse_buffer(gcode, [&final_z, &reset] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
                    if (final_z > 0 && std::abs(self.z() - 0.3) < 0.01 ) { 
                        reset = (final_z > 20.0);
                    } else {
                        final_z = std::max(final_z, static_cast<double>(self.z())); // record the highest Z point we reach
                    }
                });
                REQUIRE(reset == true);
            }
        }
        WHEN("the output is executed with support material") {
            std::string gcode = ::Test::slice({TestMesh::cube_20x20x20}, {
                { "initial_layer_line_width",    0 },
                { "enable_support",                 true },
                { "raft_layers",                    3 },
                { "gcode_comments",                 true }
                });
            THEN("Some text output is generated.") {
                REQUIRE(gcode.size() > 0);
            }
            THEN("Exported text contains extrusion statistics.") {
                REQUIRE(gcode.find("; external perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; solid infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; top infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; support material extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; first layer extrusion width") == std::string::npos);
            }
            THEN("Raft is emitted.") {
                REQUIRE(gcode.find("; raft") != std::string::npos);
            }
        }
        WHEN("the output is executed with a separate first layer extrusion width") {
			std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
                { "initial_layer_line_width", "0.5" }
                });
            THEN("Some text output is generated.") {
                REQUIRE(gcode.size() > 0);
            }
            THEN("Exported text contains extrusion statistics.") {
                REQUIRE(gcode.find("; external perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; solid infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; top infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; support material extrusion width") == std::string::npos);
                REQUIRE(gcode.find("; first layer extrusion width") != std::string::npos);
            }
        }
        WHEN("Cooling is enabled and the fan is disabled.") {
			std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
				{ "slow_down_for_layer_cooling",    true },
                { "close_fan_the_first_x_layers",  5 }
                });
            THEN("GCode to disable fan is emitted."){
                REQUIRE(gcode.find("M106 S0") != std::string::npos);
            }
        }
        WHEN("end_gcode exists with layer_num and layer_z") {
			std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
				{ "machine_end_gcode",      "; Layer_num [layer_num]\n; Layer_z [layer_z]" },
                { "layer_height",           0.1 },
                { "initial_layer_print_height", 0.1 }
                });
            THEN("layer_num and layer_z are processed in the end gcode") {
                REQUIRE(gcode.find("; Layer_num 199") != std::string::npos);
                REQUIRE(gcode.find("; Layer_z 20") != std::string::npos);
            }
        }
        WHEN("current_extruder exists in start_gcode") {
            {
				std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
					{ "machine_start_gcode", "; Extruder [current_extruder]" }
                });
                THEN("current_extruder is processed in the start gcode and set for first extruder") {
                    REQUIRE(gcode.find("; Extruder 0") != std::string::npos);
                }
            }
			{
                DynamicPrintConfig config = Slic3r::Test::default_print_config();
                config.set_num_extruders(4);
                config.set_deserialize_strict({
                    { "machine_start_gcode",            "; Extruder [current_extruder]" },
                    { "infill_extruder",                2 },
                    { "solid_infill_extruder",          2 },
                    { "perimeter_extruder",             2 },
                    { "support_material_extruder",      2 },
                    { "support_material_interface_extruder", 2 }
                });
                std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
                THEN("current_extruder is processed in the start gcode before tool changes") {
                    REQUIRE(gcode.find("; Extruder 0") != std::string::npos);
                }
            }
        }

        WHEN("layer_num represents the layer's index from z=0") {
			std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20, TestMesh::cube_20x20x20 }, {
				{ "print_sequence",                 "by object" },
                { "gcode_comments",                 true },
                { "before_layer_change_gcode",      ";Layer:[layer_num] ([layer_z] mm)" },
                { "layer_height",                   0.1 },
                { "initial_layer_print_height",     0.1 }
                });
			// End of the 1st object.
            std::string token = ";Layer:199 ";
			size_t pos = gcode.find(token);
			THEN("First and second object last layer is emitted") {
				// First object
				REQUIRE(pos != std::string::npos);
				pos += token.size();
				REQUIRE(pos < gcode.size());
				double z = 0;
				REQUIRE((sscanf(gcode.data() + pos, "(%lf mm)", &z) == 1));
				REQUIRE_THAT(z, WithinRel(20., 0.001));
				// Second object
				pos = gcode.find(";Layer:399 ", pos);
				REQUIRE(pos != std::string::npos);
				pos += token.size();
				REQUIRE(pos < gcode.size());
				REQUIRE((sscanf(gcode.data() + pos, "(%lf mm)", &z) == 1));
				REQUIRE_THAT(z, WithinRel(20., 0.001));
			}
        }
    }
}
