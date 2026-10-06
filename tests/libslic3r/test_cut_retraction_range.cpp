#include <catch2/catch.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "../fff_print/test_data.hpp"

#include <regex>
#include <string>
#include <vector>

using namespace Slic3r;

// retraction_distances_when_cut used to be limited to Bambu's cutter range [10, 18]. Other vendors
// ship values outside it: Anycubic 0 (no cutter), Creality SPARKX i7 28 and K2 30. The CLI aborted
// with "not in range [10,18]" and the GUI clamped typed values. The range is now [0, 100], and
// 0 means "no cut retraction": nothing may emit a long-retraction move for a 0 mm distance, even
// with long_retractions_when_cut / enable_long_retraction_when_cut switched on.

namespace {

// A config dump at the head (Bambu) or tail (others) echoes change_filament_gcode verbatim, both
// branches included; cut it out before scanning.
std::string body_of(const std::string &gcode)
{
    static const std::string begin_tag = "; CONFIG_BLOCK_START";
    static const std::string end_tag   = "; CONFIG_BLOCK_END";
    std::string body = gcode;
    for (size_t b; (b = body.find(begin_tag)) != std::string::npos;) {
        const size_t e = body.find(end_tag, b);
        body.erase(b, e == std::string::npos ? std::string::npos : e + end_tag.size() - b);
    }
    return body;
}

size_t count_matches(const std::string &text, const std::string &pattern)
{
    const std::regex re(pattern);
    return size_t(std::distance(std::sregex_iterator(text.begin(), text.end(), re), std::sregex_iterator()));
}

// Two filaments, a toolchange on every layer (walls on filament 1, infill on filament 2). The
// probe mirrors the two shapes the shipped Bambu templates use: the scalar pair
// (H2D / H2S / P2S: {if long_retraction_when_cut} ... E-{retraction_distance_when_cut}) and the
// arrays indexed by the outgoing filament (X1 / P1 / A1: {if long_retractions_when_cut[previous_extruder]}).
DynamicPrintConfig two_filament_config(const std::string &distances, const std::string &long_on, int enable_level)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        { "change_filament_gcode",
          "{if long_retraction_when_cut}\n"
          "; CUT SCALAR E-{retraction_distance_when_cut}\n"
          "{else}\n"
          "; CUT SCALAR OFF\n"
          "{endif}\n"
          "{if previous_extruder >= 0}{if long_retractions_when_cut[previous_extruder]}\n"
          "; CUT ARRAY E-{retraction_distances_when_cut[previous_extruder]}\n"
          "{else}\n"
          "; CUT ARRAY OFF\n"
          "{endif}{endif}" },
        { "enable_long_retraction_when_cut", enable_level },
        { "long_retractions_when_cut", long_on },
        { "retraction_distances_when_cut", distances },
        { "layer_height", 0.3 },
        { "initial_layer_print_height", 0.3 },
        { "wall_loops", 1 },
        { "wall_filament", 1 },
        { "sparse_infill_filament", 2 },
        { "solid_infill_filament", 2 },
        { "sparse_infill_density", "25%" },
        { "top_shell_layers", 0 },
        { "bottom_shell_layers", 0 },
        { "enable_prime_tower", false },
        { "enable_support", false },
        { "skirt_loops", 0 },
    });
    return config;
}

std::string slice_cube(const DynamicPrintConfig &config)
{
    return body_of(Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, config));
}

} // namespace

TEST_CASE("retraction_distances_when_cut accepts vendor values outside Bambu's 10-18 mm", "[CutRetraction]")
{
    const ConfigOptionDef *def = print_config_def.get("retraction_distances_when_cut");
    REQUIRE(def != nullptr);
    CHECK(def->min == 0.);
    CHECK(def->max == 100.);
    // The per-filament override copies the range from the printer option.
    const ConfigOptionDef *fdef = print_config_def.get("filament_retraction_distances_when_cut");
    REQUIRE(fdef != nullptr);
    CHECK(fdef->min == 0.);
    CHECK(fdef->max == 100.);

    for (const char *value : { "0", "10", "18", "28", "30", "100" }) {
        DYNAMIC_SECTION("value " << value)
        {
            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_deserialize_strict({ { "retraction_distances_when_cut", value },
                                            { "filament_retraction_distances_when_cut", value } });
            const auto errors = config.validate(true);
            CHECK(errors.find("retraction_distances_when_cut") == errors.end());
            CHECK(errors.find("filament_retraction_distances_when_cut") == errors.end());
        }
    }

    SECTION("negative and absurd values are still rejected")
    {
        for (const char *value : { "-1", "101" }) {
            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_deserialize_strict({ { "retraction_distances_when_cut", value } });
            const auto errors = config.validate(true);
            CHECK(errors.find("retraction_distances_when_cut") != errors.end());
        }
    }
}

TEST_CASE("Cut retraction of 0 mm emits no long-retraction move; 10 and 30 mm do", "[CutRetraction]")
{
    SECTION("printer-level switch on, 10 mm: every toolchange carries the cut retraction")
    {
        const std::string gcode = slice_cube(two_filament_config("10,10", "1,1", 1));
        CHECK(count_matches(gcode, "; CUT SCALAR E-10\n") >= 2);
        CHECK(count_matches(gcode, "; CUT ARRAY E-10\n") >= 2);
        CHECK(count_matches(gcode, "; CUT SCALAR OFF\n") == 0);
    }
    SECTION("30 mm (Creality K2) slices and is emitted as is")
    {
        const std::string gcode = slice_cube(two_filament_config("30,30", "1,1", 1));
        CHECK(count_matches(gcode, "; CUT SCALAR E-30\n") >= 2);
        CHECK(count_matches(gcode, "; CUT ARRAY E-30\n") >= 2);
    }
    SECTION("printer-level switch on, 0 mm: no cut retraction anywhere")
    {
        const std::string gcode = slice_cube(two_filament_config("0,0", "1,1", 1));
        CHECK(count_matches(gcode, "; CUT SCALAR OFF\n") >= 2);
        CHECK(count_matches(gcode, "; CUT SCALAR E-") == 0);
        CHECK(count_matches(gcode, "; CUT ARRAY E-") == 0);
    }
    SECTION("per-filament: the 0 mm filament never retracts, the 10 mm one does")
    {
        const std::string gcode = slice_cube(two_filament_config("0,10", "1,1", 1));
        CHECK(count_matches(gcode, "; CUT SCALAR E-0\n") == 0);
        CHECK(count_matches(gcode, "; CUT ARRAY E-0\n") == 0);
        CHECK(count_matches(gcode, "; CUT SCALAR E-10\n") >= 1);
        CHECK(count_matches(gcode, "; CUT ARRAY E-10\n") >= 1);
    }
    SECTION("filament-level override (enable_long_retraction_when_cut = 2) with a 0 mm filament distance")
    {
        // The SPARKX i8 shape: the printer level allows per-filament control, the filament switches
        // the feature on but carries 0 mm. That must not produce a cut retraction either.
        DynamicPrintConfig config = two_filament_config("10,10", "0,0", 2);
        config.set_deserialize_strict({ { "filament_long_retractions_when_cut", "1,1" },
                                        { "filament_retraction_distances_when_cut", "0,0" } });
        const std::string gcode = slice_cube(config);
        CHECK(count_matches(gcode, "; CUT SCALAR OFF\n") >= 2);
        CHECK(count_matches(gcode, "; CUT SCALAR E-") == 0);
        CHECK(count_matches(gcode, "; CUT ARRAY E-") == 0);
    }
    SECTION("filament-level override with 10 mm still retracts")
    {
        DynamicPrintConfig config = two_filament_config("18,18", "0,0", 2);
        config.set_deserialize_strict({ { "filament_long_retractions_when_cut", "1,1" },
                                        { "filament_retraction_distances_when_cut", "10,10" } });
        const std::string gcode = slice_cube(config);
        CHECK(count_matches(gcode, "; CUT SCALAR E-10\n") >= 2);
        CHECK(count_matches(gcode, "; CUT ARRAY E-10\n") >= 2);
    }
}
