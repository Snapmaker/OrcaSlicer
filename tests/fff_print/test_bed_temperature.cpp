#include <catch2/catch_all.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Print.hpp"

#include <regex>
#include <string>

#include "test_helpers.hpp"

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
    // multifilament_config() is the only way to get a real 2-filament print here: the filament
    // count is derived from filament_diameter, which it sizes (along with the distinct colours and
    // the flush matrix). DynamicPrintConfig::set_num_filaments() is a no-op in this codebase
    // (PrintConfigDef never calls init_filament_option_keys(), so filament_option_keys() is empty),
    // and with a single configured filament Print::apply clamps every feature filament id back to 1.
    // The feature selectors are the *_filament_id keys, where 0 means "inherit".
    DynamicPrintConfig config = multifilament_config(2, {
        { "curr_bed_type", "High Temp Plate" },
        { "hot_plate_temp", "30,60" },
        { "hot_plate_temp_initial_layer", "35,65" },
        // Walls print with filament 1 (low bed temp 35C), infill and solid surfaces with filament 2
        // (65C), so the first layer already carries both filaments.
        { "outer_wall_filament_id", 1 },
        { "inner_wall_filament_id", 1 },
        { "sparse_infill_filament_id", 2 },
        { "internal_solid_filament_id", 2 },
        { "top_surface_filament_id", 2 },
        { "bottom_surface_filament_id", 2 }
    });
    std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
    REQUIRE(first_layer_bed_temp(gcode) == 65);
    REQUIRE(later_layer_bed_temp(gcode) == 60);
}

// The brim follows the wall filament: with the walls (and therefore the brim) on the hot 65C
// filament, the first layer's bed temperature must be the max, even though the object's infill and
// solid surfaces print with the cold 35C filament and the walls are not the first printing filament.
// NOTE: the btfHighestTemp formula maxes over the filaments in the first layer's *tool ordering*,
// which is driven by the layer's extrusion entities. A filament that only a brim would introduce
// (wall_loops = 0, i.e. no perimeter entities at all) is not part of that set, so the object keeps
// one wall loop here.
TEST_CASE("Brim/wall filament raises the bed temperature", "[BedTemperature]")
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "curr_bed_type", "High Temp Plate" },
        { "hot_plate_temp", "30,60" },
        { "hot_plate_temp_initial_layer", "35,65" },
        { "wall_loops", 1 },
        { "brim_width", 5 },
        { "brim_type", "auto_brim" },
        { "outer_wall_filament_id", 2 },
        { "inner_wall_filament_id", 2 },
        { "sparse_infill_filament_id", 1 },
        { "internal_solid_filament_id", 1 },
        { "top_surface_filament_id", 1 },
        { "bottom_surface_filament_id", 1 }
    });
    std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
    REQUIRE(first_layer_bed_temp(gcode) == 65);
    REQUIRE(later_layer_bed_temp(gcode) == 60);
}

// Placeholder: the machine_start_gcode single-value placeholder expands to the max bed temperature
// (this is the path used by the Snapmaker U1 start gcode).
TEST_CASE("bed_temperature_initial_layer_single expands to the max", "[BedTemperature]")
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "curr_bed_type", "High Temp Plate" },
        { "hot_plate_temp", "30,60" },
        { "hot_plate_temp_initial_layer", "35,65" },
        { "outer_wall_filament_id", 1 },
        { "inner_wall_filament_id", 1 },
        { "sparse_infill_filament_id", 2 },
        { "internal_solid_filament_id", 2 },
        { "top_surface_filament_id", 2 },
        { "bottom_surface_filament_id", 2 },
        { "machine_start_gcode", "M190 S{bed_temperature_initial_layer_single}" }
    });
    std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
    // start gcode already sets the temperature, so no additional M190 is emitted by the slicer.
    REQUIRE(gcode.find("M190 S65") != std::string::npos);
}
