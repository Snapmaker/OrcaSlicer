#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Print.hpp"

#include <regex>
#include <string>

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {
// Extract the S value of the first M190 (first-layer bed temperature, emitted after machine_start_gcode).
int first_layer_bed_temp(const std::string& gcode)
{
    std::smatch m;
    if (std::regex_search(gcode, m, std::regex(R"(M190 S(\d+))")))
        return std::stoi(m[1]);
    return -1;
}

// Extract the S value of the M140 emitted at the first-to-second layer transition.
int later_layer_bed_temp(const std::string& gcode)
{
    std::smatch m;
    if (std::regex_search(gcode, m, std::regex(R"(M140 S(\d+))")))
        return std::stoi(m[1]);
    return -1;
}
} // namespace

// Regression: with a single filament the emitted bed temperature is the value of that filament
// (identical to the pre-change behavior, which always used the first printing extruder).
TEST_CASE("Single filament bed temperature is unchanged", "[BedTemperature]")
{
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "curr_bed_type", "High Temp Plate" }, // btPEI -> hot_plate_temp
        { "hot_plate_temp", "45" },
        { "hot_plate_temp_initial_layer", "55" }
    });
    std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
    REQUIRE(first_layer_bed_temp(gcode) == 55);
    REQUIRE(later_layer_bed_temp(gcode) == 45);
}

// Two compatible filaments (PLA-like 65C + TPU-like 35C). The bed temperature shall always take the
// higher value, even if the first printing extruder (wall) is the low-temperature one.
TEST_CASE("Mixed print uses the highest bed temperature", "[BedTemperature]")
{
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    // Filament count is data-driven like a loaded preset (Preset.cpp derives it from
    // filament_diameter); DynamicPrintConfig::set_num_filaments does not resize these vectors.
    config.set_deserialize_strict({
        { "curr_bed_type", "High Temp Plate" },
        { "filament_diameter", "1.75,1.75" }, // two filaments, preset-style
        { "hot_plate_temp", "30,60" },
        { "hot_plate_temp_initial_layer", "35,65" },
        // Wall printed by extruder 1 (0-based 0, low bed temp 35C), infill by extruder 2 (0-based 1, 65C).
        { "wall_filament", 1 },
        { "sparse_infill_filament", 2 },
        { "solid_infill_filament", 2 }
    });
    std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
    REQUIRE(first_layer_bed_temp(gcode) == 65);
    REQUIRE(later_layer_bed_temp(gcode) == 60);
}

// All paths: with wall_loops = 0 the brim-introduced wall_filament extruder must be included in the
// max, even though it is not the first printing extruder of the object.
TEST_CASE("Brim-introduced extruder is covered when wall_loops is zero", "[BedTemperature]")
{
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    // Filament count is data-driven like a loaded preset (Preset.cpp derives it from
    // filament_diameter); DynamicPrintConfig::set_num_filaments does not resize these vectors.
    config.set_deserialize_strict({
        { "curr_bed_type", "High Temp Plate" },
        { "filament_diameter", "1.75,1.75" }, // two filaments, preset-style
        { "hot_plate_temp", "30,60" },
        { "hot_plate_temp_initial_layer", "35,65" },
        // No walls. Infill uses extruder 1 (0-based 0, 35C); the brim follows wall_filament
        // (extruder 2, 0-based 1, 65C) and must raise the bed temperature.
        { "wall_loops", 0 },
        { "brim_width", 5 },
        { "brim_type", "auto_brim" },
        { "wall_filament", 2 },
        { "sparse_infill_filament", 1 },
        { "solid_infill_filament", 1 }
    });
    std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
    REQUIRE(first_layer_bed_temp(gcode) == 65);
    REQUIRE(later_layer_bed_temp(gcode) == 60);
}

// Placeholder: the machine_start_gcode single-value placeholder expands to the max bed temperature
// (this is the path used by the Snapmaker U1 start gcode).
TEST_CASE("bed_temperature_initial_layer_single expands to the max", "[BedTemperature]")
{
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    // Filament count is data-driven like a loaded preset (Preset.cpp derives it from
    // filament_diameter); DynamicPrintConfig::set_num_filaments does not resize these vectors.
    config.set_deserialize_strict({
        { "curr_bed_type", "High Temp Plate" },
        { "filament_diameter", "1.75,1.75" }, // two filaments, preset-style
        { "hot_plate_temp", "30,60" },
        { "hot_plate_temp_initial_layer", "35,65" },
        { "wall_filament", 1 },
        { "sparse_infill_filament", 2 },
        { "solid_infill_filament", 2 },
        { "machine_start_gcode", "M190 S{bed_temperature_initial_layer_single}" }
    });
    std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
    // start gcode already sets the temperature, so no additional M190 is emitted by the slicer.
    REQUIRE(gcode.find("M190 S65") != std::string::npos);
}

namespace {
// Two filaments: filament 1 (PLA-like, 60C first layer / 55C later), filament 2 (PETG-like, 80C / 75C).
// The object prints with filament 1, support and support interface are assigned to filament 2.
DynamicPrintConfig support_on_second_filament_config()
{
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_deserialize_strict({
        { "curr_bed_type", "High Temp Plate" },
        { "filament_diameter", "1.75,1.75" }, // two filaments, preset-style
        { "hot_plate_temp", "55,75" },
        { "hot_plate_temp_initial_layer", "60,80" },
        { "wall_filament", 1 },
        { "sparse_infill_filament", 1 },
        { "solid_infill_filament", 1 },
        { "enable_support", 1 },
        { "support_filament", 2 },
        { "support_interface_filament", 2 },
        { "enable_prime_tower", 0 },
        { "machine_start_gcode", "; bed_single = {bed_temperature_initial_layer_single}" }
    });
    return config;
}
} // namespace

// Regression (Snapmaker/OrcaSlicer#1073): support enabled on another filament, but the model needs no
// support, so the support filament prints nothing and must not raise the bed temperature.
TEST_CASE("Support filament without generated support does not raise the bed temperature", "[BedTemperature]")
{
    DynamicPrintConfig config = support_on_second_filament_config();
    std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
    REQUIRE(gcode.find("\nT1") == std::string::npos); // filament 2 never prints
    REQUIRE(gcode.find("; bed_single = 60") != std::string::npos);
    REQUIRE(first_layer_bed_temp(gcode) == 60);
    REQUIRE(later_layer_bed_temp(gcode) == 55);
}

// Guard: when support is actually generated with the other filament, it is still counted.
TEST_CASE("Support filament with generated support still raises the bed temperature", "[BedTemperature]")
{
    // A 30 x 30 mm cap on a 10 mm tall stem: the cap overhang needs support.
    TriangleMesh mesh = make_cube(10, 10, 10);
    mesh.translate(10, 10, 0);
    TriangleMesh cap = make_cube(30, 30, 2);
    cap.translate(0, 0, 10);
    mesh.merge(cap);

    DynamicPrintConfig config = support_on_second_filament_config();
    std::string gcode = Slic3r::Test::slice({mesh}, config);
    REQUIRE(gcode.find("\nT1") != std::string::npos); // support prints with filament 2
    REQUIRE(gcode.find("; bed_single = 80") != std::string::npos);
    REQUIRE(first_layer_bed_temp(gcode) == 80);
    REQUIRE(later_layer_bed_temp(gcode) == 75);
}
