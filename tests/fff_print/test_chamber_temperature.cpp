#include <catch2/catch.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Print.hpp"

#include <string>

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {
// Number of G-code lines that start with `needle` (the trailing config dump repeats the start G-code
// as a comment, so a plain substring count would double-count it).
size_t count_of(const std::string& gcode, const std::string& needle)
{
    size_t n = 0;
    size_t pos = 0;
    while (pos < gcode.size()) {
        size_t eol = gcode.find('\n', pos);
        if (eol == std::string::npos)
            eol = gcode.size();
        if (gcode.compare(pos, needle.size(), needle) == 0)
            ++n;
        pos = eol + 1;
    }
    return n;
}
} // namespace

TEST_CASE("chamber_minimal_temperature is a defined filament option", "[ChamberTemperature]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE(config.has("chamber_minimal_temperature"));
    REQUIRE(config.option<ConfigOptionInts>("chamber_minimal_temperature")->get_at(0) == 0);

    // It is part of the filament preset, next to the target chamber temperature.
    bool in_filament_options = false;
    for (const std::string& key : Preset::filament_options())
        in_filament_options |= key == "chamber_minimal_temperature";
    REQUIRE(in_filament_options);
}

TEST_CASE("chamber_minimal_temperature is exposed to the start G-code", "[ChamberTemperature]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "chamber_minimal_temperature", "40" },
        { "machine_start_gcode", "; chamber_min=[chamber_minimal_temperature]" }
    });
    const std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
    REQUIRE(gcode.find("; chamber_min=40") != std::string::npos);
}

TEST_CASE("Automatic chamber M191 is skipped when the start G-code sets the chamber temperature", "[ChamberTemperature]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "activate_chamber_temp_control", "1" },
        { "chamber_temperature", "45" }
    });

    SECTION("start G-code without M141/M191: the automatic M191 is emitted once")
    {
        config.set_deserialize_strict({ { "machine_start_gcode", "G28" } });
        const std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
        REQUIRE(count_of(gcode, "M191 S45") == 1);
    }

    SECTION("start G-code with its own M191: no automatic M191")
    {
        config.set_deserialize_strict({ { "machine_start_gcode", "G28\nM191 S35" } });
        const std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
        REQUIRE(count_of(gcode, "M191 S45") == 0);
        REQUIRE(count_of(gcode, "M191 S35") == 1);
    }

    SECTION("start G-code with its own M141: no automatic M191")
    {
        config.set_deserialize_strict({ { "machine_start_gcode", "G28\nM141 S35" } });
        const std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
        REQUIRE(count_of(gcode, "M191 S45") == 0);
        REQUIRE(count_of(gcode, "M141 S35") == 1);
    }
}
