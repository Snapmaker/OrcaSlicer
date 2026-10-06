#include <catch2/catch.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// Bambu Lab A2L layer change G-code: "M201 N1 Y[curr_y_acceleration_limit]". The variable comes from
// Bambu Studio's bed-slinger mass model (machine_max_force_Y / machine_bed_mass_Y); without it every
// A2L preset failed to slice ("custom G-code fails to parse", CLI return code -100).

namespace {

DynamicPrintConfig mass_model_config(double force_y, double bed_mass_y)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "machine_max_acceleration_y", "20000,20000" },
        { "filament_density",           "1.24" },
        { "sparse_infill_density",      "100%" },
        { "skirt_loops",                "0" },
        { "brim_type",                  "no_brim" },
    });
    config.set_key_value("layer_change_gcode",
                         new ConfigOptionString("G92 E0\nM201 N1 Y[curr_y_acceleration_limit]\n;MASS {curr_accumulated_mass} {curr_layer_mass}"));
    config.set_key_value("machine_max_force_Y", new ConfigOptionFloat(force_y));
    config.set_key_value("machine_bed_mass_Y", new ConfigOptionFloat(bed_mass_y));
    return config;
}

// Real G-code lines only: the config block repeats the template with escaped newlines.
const std::string LIMIT_LINE = std::string(1, '\n') + "M201 N1 Y";
const std::string MASS_LINE  = std::string(1, '\n') + ";MASS ";

std::vector<double> numbers_after(const std::string &gcode, const std::string &prefix)
{
    std::vector<double> out;
    size_t              pos = 0;
    while ((pos = gcode.find(prefix, pos)) != std::string::npos) {
        pos += prefix.size();
        out.emplace_back(std::stod(gcode.substr(pos, gcode.find_first_of(" \n", pos) - pos)));
    }
    return out;
}

} // namespace

TEST_CASE("curr_y_acceleration_limit falls with the printed mass on a bed slinger", "[GCode][Placeholders][A2L]")
{
    // The A2L's numbers: 29 N Y drive, 2700 g bed.
    const std::string   gcode  = Test::slice({ TestMesh::cube_20x20x20 }, mass_model_config(29., 2700.));
    const std::vector<double> limits = numbers_after(gcode, LIMIT_LINE);
    const std::vector<double> masses = numbers_after(gcode, MASS_LINE);
    REQUIRE(limits.size() > 10);
    REQUIRE(masses.size() == limits.size());
    // F / (bed + part), capped by the configured limit, which is also the answer while nothing is
    // printed yet (the first layer change). The part only grows, so the limit only falls.
    CHECK(masses.front() == Approx(0.).margin(0.5));
    for (size_t i = 0; i < limits.size(); ++i) {
        const double expected = masses[i] > 0. ? std::min(20000., 29e6 / (2700. + masses[i])) : 20000.;
        CHECK(limits[i] == Approx(expected).epsilon(1e-4));
        if (i > 0) {
            CHECK(masses[i] > masses[i - 1]);
            CHECK(limits[i] < limits[i - 1] + 1e-6);
        }
    }
    CHECK(limits.back() < 29e6 / 2700.);
    // The model's keys are part of this printer's config block.
    CHECK(gcode.find("; machine_max_force_Y = 29") != std::string::npos);
    CHECK(gcode.find("; machine_bed_mass_Y = 2700") != std::string::npos);
}

TEST_CASE("curr_y_acceleration_limit is the machine Y limit when no mass model is set", "[GCode][Placeholders][A2L]")
{
    const std::string   gcode  = Test::slice({ TestMesh::cube_20x20x20 }, mass_model_config(0., 0.));
    const std::vector<double> limits = numbers_after(gcode, LIMIT_LINE);
    REQUIRE(limits.size() > 10);
    for (double limit : limits)
        CHECK(limit == Approx(20000.));
    // A printer without the model keeps the config block it had before the keys existed.
    CHECK(gcode.find("machine_max_force_Y") == std::string::npos);
    CHECK(gcode.find("machine_bed_mass_Y") == std::string::npos);
}
