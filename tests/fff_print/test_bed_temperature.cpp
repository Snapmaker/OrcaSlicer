#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <regex>
#include <sstream>
#include <vector>
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

// All paths: the brim follows the object's wall filament, so that filament raises the first layer bed temperature.
// With wall_loops = 0 no brim is generated and the wall filament never prints, so it must not count.
TEST_CASE("The brim filament counts only when a brim is printed", "[BedTemperature]")
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
        // Infill uses extruder 1 (0-based 0, 35C); walls and brim use extruder 2 (0-based 1, 65C).
        { "brim_width", 5 },
        { "brim_type", "outer_only" },
        { "wall_filament", 2 },
        { "sparse_infill_filament", 1 },
        { "solid_infill_filament", 1 }
    });

    SECTION("With walls the brim and walls print with filament 2") {
        config.set_deserialize_strict({ { "wall_loops", 1 } });
        std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
        REQUIRE(first_layer_bed_temp(gcode) == 65);
        REQUIRE(later_layer_bed_temp(gcode) == 60);
    }
    SECTION("Without walls there is no brim and filament 2 never prints") {
        config.set_deserialize_strict({ { "wall_loops", 0 } });
        std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
        REQUIRE(gcode.find("\nT1") == std::string::npos);
        REQUIRE(first_layer_bed_temp(gcode) == 35);
        REQUIRE(later_layer_bed_temp(gcode) == 30);
    }
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

// A filament that starts above the first layer, such as a PETG support interface under a PLA part, never touches
// the bed. Heating the bed to its temperature would soften the filament the part stands on, so only filaments
// printed on the first layer count.
TEST_CASE("A support interface filament above the first layer does not raise the bed temperature", "[BedTemperature]")
{
    // A 30 mm cap on a 10 mm stem prints with filament 1 (bed 55 first layer, 50 after) and is supported from the
    // bed: the support with filament 1, the interface under the cap with filament 2 (bed 80).
    TriangleMesh mesh = make_cube(10, 10, 10);
    mesh.translate(10, 10, 0);
    TriangleMesh cap = make_cube(30, 30, 2);
    cap.translate(0, 0, 10);
    mesh.merge(cap);

    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_deserialize_strict({
        { "curr_bed_type", "High Temp Plate" },
        { "filament_diameter", "1.75,1.75" },
        { "hot_plate_temp", "50,80" },
        { "hot_plate_temp_initial_layer", "55,80" },
        { "enable_prime_tower", 0 },
        { "enable_support", 1 },
        { "support_filament", 1 },
        { "support_interface_filament", 2 },
        { "skirt_loops", 0 },
        { "brim_type", "no_brim" },
        { "machine_start_gcode", "M190 S{bed_temperature_initial_layer_single}" }
    });
    std::string gcode = Slic3r::Test::slice({mesh}, config);
    REQUIRE(gcode.find("\nT1") != std::string::npos);

    std::vector<int> bed_temps;
    std::istringstream lines(gcode);
    for (std::string line; std::getline(lines, line);)
        if (line.rfind("M140 S", 0) == 0 || line.rfind("M190 S", 0) == 0)
            bed_temps.push_back(std::stoi(line.substr(6)));
    REQUIRE_FALSE(bed_temps.empty());
    CHECK(bed_temps.front() == 55);
    CHECK(std::find(bed_temps.begin(), bed_temps.end(), 50) != bed_temps.end());
    CHECK(std::find(bed_temps.begin(), bed_temps.end(), 80) == bed_temps.end());
}
