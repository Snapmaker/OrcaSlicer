#include <catch2/catch_all.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/calib.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

// The width-resolution getters are protected; expose them so the resolution can be asserted directly.
struct PaPatternProbe : public CalibPressureAdvancePattern
{
    using CalibPressureAdvancePattern::CalibPressureAdvancePattern;
    using CalibPressureAdvancePattern::line_width;
    using CalibPressureAdvancePattern::line_width_first_layer;
};

} // namespace

TEST_CASE("Zero calibration line width resolves to a positive default", "[Calib][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"line_width", "0"},
        {"initial_layer_line_width", "0"},
    });

    Model model;
    model.add_object("cube", "", make_cube(20, 20, 20))->add_instance();

    Calib_Params params;
    params.mode = CalibMode::Calib_PA_Pattern;

    PaPatternProbe pattern(params, config, /* is_bbl_machine */ true, *model.objects.front(), Vec3d(0, 0, 0));

    REQUIRE(pattern.line_width() > 0.);
    REQUIRE(pattern.line_width_first_layer() > 0.);
}

namespace {

struct EndState { double final_e; double max_e; };

EndState simulate_absolute_e(const std::string &gcode)
{
    double final_e = 0.;
    double max_e   = 0.;

    std::istringstream lines(gcode);
    std::string        line;
    while (std::getline(lines, line)) {
        std::istringstream words(line);
        std::string        op;
        if (!(words >> op))
            continue;
        if (op != "G1" && op != "G0" && op != "G92")
            continue;

        std::string word;
        while (words >> word) {
            if (word.size() >= 2 && word[0] == 'E') {
                final_e = std::stod(word.substr(1));
                max_e   = std::max(max_e, final_e);
                break;
            }
        }
    }

    return {final_e, max_e};
}

} // namespace

TEST_CASE("PA pattern resets the extruder after the final layer in absolute E mode", "[Calib][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"use_relative_e_distances", "0"},
        {"line_width", "0.45"},
        {"initial_layer_line_width", "0.45"},
    });

    Model model;
    model.add_object("cube", "", make_cube(20, 20, 20))->add_instance();

    Calib_Params params;
    params.mode  = CalibMode::Calib_PA_Pattern;
    params.start = 0.;
    params.end   = 0.08;
    params.step  = 0.002;

    CalibPressureAdvancePattern pattern(params, config, /* is_bbl_machine */ false, *model.objects.front(), Vec3d(0, 0, 0));
    const CustomGCode::Info     info = pattern.generate_custom_gcodes(config, /* is_bbl_machine */ false, *model.objects.front(),
                                                                      Vec3d(0, 0, 0));

    std::string gcode;
    for (const CustomGCode::Item &item : info.gcodes)
        gcode += item.extra;

    const EndState state = simulate_absolute_e(gcode);

    REQUIRE(state.max_e > 1.);
    REQUIRE_THAT(state.final_e, Catch::Matchers::WithinAbs(0., 1e-9));
}

// CalibUtils::calib_max_vol_speed and CalibUtils::calib_VFA live in libslic3r_gui and need presets plus a
// connected device, so they cannot run here. This pins the contract they depend on: an override merged into a
// config built from FullPrintConfig::defaults() has to carry the option type PrintConfig.cpp defines for the key.
TEST_CASE("Device calibration overrides match the option types of their keys", "[Calib][Regression]")
{
    const std::pair<const char *, ConfigOptionType> expected[] = {
        {"slow_down_layer_time", coFloats},
        {"outer_wall_line_width", coFloatOrPercent},
        {"enable_overhang_speed", coBools},
        {"filament_max_volumetric_speed", coFloats},
    };
    for (const auto &[key, type] : expected) {
        const ConfigOptionDef *opt_def = print_config_def.get(key);
        REQUIRE(opt_def != nullptr);
        REQUIRE(opt_def->type == type);
    }

    auto merge = [](const std::string &key, ConfigOption *opt) {
        DynamicPrintConfig overrides;
        overrides.set_key_value(key, opt);
        DynamicPrintConfig full_config;
        full_config.apply(FullPrintConfig::defaults());
        full_config.apply(overrides);
        return full_config;
    };

    SECTION("the former option types are rejected by the merge") {
        REQUIRE_THROWS_AS(merge("slow_down_layer_time", new ConfigOptionInts{0}), ConfigurationError);
        REQUIRE_THROWS_AS(merge("outer_wall_line_width", new ConfigOptionFloat(0.7)), ConfigurationError);
    }

    SECTION("the option types written by the device calibrations merge and keep their value") {
        DynamicPrintConfig cfg = merge("slow_down_layer_time", new ConfigOptionFloats{0.0});
        REQUIRE(cfg.option<ConfigOptionFloats>("slow_down_layer_time")->values == std::vector<double>{0.0});

        cfg = merge("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.7, false));
        const auto *width = cfg.option<ConfigOptionFloatOrPercent>("outer_wall_line_width");
        REQUIRE(width->value == Catch::Approx(0.7));
        REQUIRE_FALSE(width->percent);

        cfg = merge("enable_overhang_speed", new ConfigOptionBoolsNullable({false}));
        REQUIRE(cfg.option<ConfigOptionBools>("enable_overhang_speed")->get_at(0) == false);

        cfg = merge("filament_max_volumetric_speed", new ConfigOptionFloats{50});
        REQUIRE(cfg.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{50.});
    }
}
