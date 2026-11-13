#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/calib.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <set>
#include <cstdlib>
#include <sstream>

using namespace Slic3r;

namespace {

// Config carrying only the options find_optimal_PA_speed reads.
DynamicPrintConfig speed_config(double nozzle_diameter, double outer_wall_speed, double max_vol_speed)
{
    DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
    c.set_key_value("nozzle_diameter", new ConfigOptionFloats{ nozzle_diameter });
    c.set_key_value("outer_wall_speed", new ConfigOptionFloats{ outer_wall_speed });
    c.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{ max_vol_speed });
    return c;
}

// Full config for the PA pattern. width/layer_height are absolute values so
// get_abs_value() returns them verbatim; gcode_flavor is pinned to Klipper so
// set_pressure_advance emits "SET_PRESSURE_ADVANCE ADVANCE=<k>" (greppable).
DynamicPrintConfig make_pa_pattern_config()
{
    DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
    c.set_key_value("nozzle_diameter", new ConfigOptionFloats{ 0.4 });
    c.set_key_value("line_width", new ConfigOptionFloatOrPercent(0.5, false));
    c.set_key_value("initial_layer_line_width", new ConfigOptionFloatOrPercent(0.5, false));
    c.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    c.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.25));
    c.set_key_value("wall_loops", new ConfigOptionInt(3));
    c.set_key_value("outer_wall_speed", new ConfigOptionFloats{ 200.0 });
    c.set_key_value("outer_wall_acceleration", new ConfigOptionFloats{ 5000.0 });
    c.set_key_value("initial_layer_speed", new ConfigOptionFloats{ 30.0 });
    c.set_key_value("filament_flow_ratio", new ConfigOptionFloats{ 1.0 });
    c.set_key_value("gcode_flavor", new ConfigOptionEnum<GCodeFlavor>(gcfKlipper));
    return c;
}

// A Model that owns a single cube ModelObject with one instance (both are
// required by CalibPressureAdvancePattern's refresh_setup()).
struct CubeFixture {
    Model        model;
    ModelObject *object;

    CubeFixture()
    {
        object = model.add_object();
        object->add_volume(make_cube(10., 10., 2.));
        object->add_instance();
    }
};

// Calib_Params for a PA pattern ladder.
Calib_Params pa_params(double start, double end, double step)
{
    Calib_Params p;
    p.mode  = CalibMode::Calib_PA_Pattern;
    p.start = start;
    p.end   = end;
    p.step  = step;
    return p;
}

// Collect the distinct pressure-advance values emitted into the generated gcode
// (Klipper flavor: "SET_PRESSURE_ADVANCE ADVANCE=<k>"). Repeated occurrences of
// the same k (start is re-emitted at the frame/numbering/end) collapse via std::set.
std::vector<double> extract_pa_values(const CustomGCode::Info &info)
{
    std::set<double> values;
    const std::string marker = "ADVANCE=";
    for (const CustomGCode::Item &item : info.gcodes) {
        const std::string &g = item.extra;
        size_t pos = 0;
        while ((pos = g.find(marker, pos)) != std::string::npos) {
            const char *start = g.c_str() + pos + marker.size();
            char       *end   = nullptr;
            double      v     = std::strtod(start, &end);
            if (end == start) { // malformed "ADVANCE=" with no number after it: skip
                ++pos;
                continue;
            }
            values.insert(v);
            pos = static_cast<size_t>(end - g.c_str()); // end >= start > g.c_str(), so safe
        }
    }
    return std::vector<double>(values.begin(), values.end());
}

// Extract the (X, Y) of every wall-start move in one layer's gcode. Each wall is
// drawn by two "Print pattern wall" G1 lines (up-right then down-right); the
// down-right one lands on the wall start, so take every 2nd line.
std::vector<std::pair<double, double>> extract_wall_starts(const std::string &gcode)
{
    std::vector<double> xs, ys;
    std::istringstream  iss(gcode);
    std::string         line;
    while (std::getline(iss, line)) {
        if (line.find("Print pattern wall") == std::string::npos)
            continue;
        size_t xp = line.find(" X");
        size_t yp = line.find(" Y");
        if (xp == std::string::npos || yp == std::string::npos)
            continue;

        const char *xstart = line.c_str() + xp + 2;
        const char *ystart = line.c_str() + yp + 2;
        char       *xend   = nullptr;
        char       *yend   = nullptr;
        double      x      = std::strtod(xstart, &xend);
        double      y      = std::strtod(ystart, &yend);
        // Fail loudly on a malformed coordinate rather than silently pushing 0.
        REQUIRE(xend != xstart);
        REQUIRE(yend != ystart);
        xs.push_back(x);
        ys.push_back(y);
    }
    std::vector<std::pair<double, double>> starts;
    for (size_t i = 1; i < xs.size(); i += 2)
        starts.emplace_back(xs[i], ys[i]);
    return starts;
}

} // namespace

// ---------------------------------------------------------------------------
// find_optimal_PA_speed (max volumetric speed clamp)
// ---------------------------------------------------------------------------

SCENARIO("Calib find_optimal_PA_speed clamps to the max volumetric speed", "[Calib]") {
    GIVEN("a high wall speed and a low max volumetric speed") {
        // nozzle 0.4, line 0.5, layer 0.2 -> mm3_per_mm = 0.2*(0.5 - 0.2*(1-pi/4))
        // = 0.09141593 mm^3/mm. 10 mm^3/s / 0.09141593 = 109.39 -> floor 109.
        DynamicPrintConfig config = speed_config(0.4, 200.0, 10.0);

        WHEN("the optimal speed is computed") {
            float speed = CalibPressureAdvance::find_optimal_PA_speed(config, 0.5, 0.2, 0);

            THEN("it is clamped below the wall speed by the volumetric limit") {
                REQUIRE_THAT(speed, WithinRel(109.0, 1e-6));
                REQUIRE(speed < 200.0f); // proves the volumetric clamp (not the wall speed) bound the result
            }
        }
    }
}

SCENARIO("Calib find_optimal_PA_speed keeps the wall speed when below the volumetric limit", "[Calib]") {
    GIVEN("a wall speed above the 100 floor and a generous max volumetric speed") {
        DynamicPrintConfig config = speed_config(0.4, 150.0, 100.0);

        WHEN("the optimal speed is computed") {
            float speed = CalibPressureAdvance::find_optimal_PA_speed(config, 0.5, 0.2, 0);

            THEN("the wall speed is used unchanged") {
                REQUIRE_THAT(speed, WithinRel(150.0, 1e-6));
            }
        }
    }
}

SCENARIO("Calib find_optimal_PA_speed floors to the 100 minimum", "[Calib]") {
    GIVEN("a wall speed below 100") {
        DynamicPrintConfig config = speed_config(0.4, 50.0, 100.0);

        WHEN("the optimal speed is computed") {
            float speed = CalibPressureAdvance::find_optimal_PA_speed(config, 0.5, 0.2, 0);

            THEN("the general suggested minimum of 100 is used") {
                REQUIRE_THAT(speed, WithinRel(100.0, 1e-6));
            }
        }
    }
}

SCENARIO("Calib find_optimal_PA_speed falls back to auto extrusion width for a non-positive line width", "[Calib]") {
    GIVEN("line_width <= 0") {
        // auto width = 1.125 * nozzle = 0.45 -> mm3_per_mm = 0.2*(0.45 - 0.042920367)
        // = 0.08141593. 8 / 0.08141593 = 98.26 -> floor 98.
        DynamicPrintConfig config = speed_config(0.4, 200.0, 8.0);

        WHEN("the optimal speed is computed") {
            float speed = CalibPressureAdvance::find_optimal_PA_speed(config, 0.0, 0.2, 0);

            THEN("the auto width clamps the speed") {
                REQUIRE_THAT(speed, WithinRel(98.0, 1e-6));
                REQUIRE(speed < 200.0f);
            }
        }
    }
}

SCENARIO("Calib find_optimal_PA_speed selects the per-filament max volumetric speed", "[Calib]") {
    GIVEN("two filaments with different max volumetric speeds and a high wall speed") {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats{ 0.4 });
        config.set_key_value("outer_wall_speed", new ConfigOptionFloats{ 500.0 });
        config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{ 10.0, 20.0 });

        THEN("filament 0 clamps to its own 10 mm^3/s limit") {
            // 10 / 0.09141593 = 109.39 -> 109.
            REQUIRE_THAT(CalibPressureAdvance::find_optimal_PA_speed(config, 0.5, 0.2, 0), WithinRel(109.0, 1e-6));
        }
        THEN("filament 1 clamps to its own 20 mm^3/s limit") {
            // 20 / 0.09141593 = 218.78 -> 218.
            REQUIRE_THAT(CalibPressureAdvance::find_optimal_PA_speed(config, 0.5, 0.2, 1), WithinRel(218.0, 1e-6));
        }
    }
}

// ---------------------------------------------------------------------------
// CalibPressureAdvancePattern (K ladder + spacing formula)
// ---------------------------------------------------------------------------

SCENARIO("Calib PA pattern emits a K ladder covering start through end", "[Calib]") {
    GIVEN("start=0, end=0.03, step=0.005 (end exactly on a rung)") {
        // num_patterns = ceil((0.03-0)/0.005 + 1) = 7.
        DynamicPrintConfig config = make_pa_pattern_config();
        CubeFixture         fixture;
        Calib_Params        params = pa_params(0.0, 0.03, 0.005);
        CalibPressureAdvancePattern pattern(params, config, false, *fixture.object, Vec3d::Zero());

        WHEN("the pattern gcode is generated") {
            CustomGCode::Info info = pattern.generate_custom_gcodes(config, false, *fixture.object, Vec3d::Zero());
            auto              k    = extract_pa_values(info);

            THEN("7 distinct K values are emitted, each start + i*step") {
                REQUIRE(k.size() == 7);
                // std::set sorts, so k is ascending; verify every rung, not just the ends.
                for (size_t i = 0; i < k.size(); ++i)
                    CHECK_THAT(k[i], WithinRel(0.0 + i * 0.005, 1e-9));
            }
        }
    }
}

SCENARIO("Calib PA pattern K ladder overshoots to cover a non-exact end", "[Calib]") {
    GIVEN("start=0, end=0.02, step=0.006 (end not on a rung)") {
        // num_patterns = ceil((0.02-0)/0.006 + 1) = ceil(4.333) = 5; last rung 0.024 >= end.
        DynamicPrintConfig config = make_pa_pattern_config();
        CubeFixture         fixture;
        Calib_Params        params = pa_params(0.0, 0.02, 0.006);
        CalibPressureAdvancePattern pattern(params, config, false, *fixture.object, Vec3d::Zero());

        WHEN("the pattern gcode is generated") {
            CustomGCode::Info info = pattern.generate_custom_gcodes(config, false, *fixture.object, Vec3d::Zero());
            auto              k    = extract_pa_values(info);

            THEN("5 rungs are each start + i*step, the last reaching the end") {
                REQUIRE(k.size() == 5);
                for (size_t i = 0; i < k.size(); ++i)
                    CHECK_THAT(k[i], WithinRel(0.0 + i * 0.006, 1e-9));
            }
        }
    }
}

SCENARIO("Calib PA pattern K ladder supports a non-zero start", "[Calib]") {
    GIVEN("start=0.01, end=0.03, step=0.005") {
        // num_patterns = ceil((0.03-0.01)/0.005 + 1) = 5.
        DynamicPrintConfig config = make_pa_pattern_config();
        CubeFixture         fixture;
        Calib_Params        params = pa_params(0.01, 0.03, 0.005);
        CalibPressureAdvancePattern pattern(params, config, false, *fixture.object, Vec3d::Zero());

        WHEN("the pattern gcode is generated") {
            CustomGCode::Info info = pattern.generate_custom_gcodes(config, false, *fixture.object, Vec3d::Zero());
            auto              k    = extract_pa_values(info);

            THEN("the rungs run 0.01, 0.015, 0.02, 0.025, 0.03") {
                REQUIRE(k.size() == 5);
                for (size_t i = 0; i < k.size(); ++i)
                    CHECK_THAT(k[i], WithinRel(0.01 + i * 0.005, 1e-9));
            }
        }
    }
}

SCENARIO("Calib PA pattern spacing formula and flow value", "[Calib]") {
    GIVEN("line 0.5, layer 0.2, wall_loops 3, outer_wall_speed 200") {
        // spacing = line_width - layer_height*(1 - pi/4) = 0.5 - 0.2*0.214601837 = 0.457079633;
        // spacing_angle = spacing/sin(45deg) = 0.646408216; spacing_first_layer = 0.5 - 0.25*0.214601837 = 0.446349541.
        // print_size_x = num_patterns*(wall_count-1)*spacing_angle + (num_patterns-1)*(2+line_width)
        //                + cos(45deg)*30 + spacing_first_layer*wall_count + pattern_shift.
        DynamicPrintConfig config = make_pa_pattern_config();
        CubeFixture         fixture;
        Calib_Params        params = pa_params(0.0, 0.03, 0.005);
        CalibPressureAdvancePattern pattern(params, config, false, *fixture.object, Vec3d::Zero());

        THEN("flow_val equals speed * mm3_per_mm * flow_ratio") {
            // 200 * 0.2*(0.5 - 0.2*(1-pi/4)) * 1.0 = 18.283185307179586.
            REQUIRE_THAT(pattern.flow_val(), WithinRel(18.283185307179586, 1e-6));
        }
        THEN("print_size_x matches the spacing formula") {
            REQUIRE_THAT(pattern.print_size_x(), WithinRel(48.99466615852109, 1e-6));
        }
        THEN("print_size_y matches the frame plus numbering tab") {
            // 2*sin(45deg)*30 + max_numbering_height(11) + 2*1 + 0.5 = 55.926406871.
            REQUIRE_THAT(pattern.print_size_y(), WithinRel(55.92640687119285, 1e-6));
        }
    }
}

SCENARIO("Calib PA pattern G1 wall coordinates follow the spacing formula", "[Calib]") {
    GIVEN("line 0.5, layer 0.2, wall_loops 3, 7 patterns") {
        DynamicPrintConfig config = make_pa_pattern_config();
        CubeFixture         fixture;
        Calib_Params        params = pa_params(0.0, 0.03, 0.005);
        CalibPressureAdvancePattern pattern(params, config, false, *fixture.object, Vec3d::Zero());

        WHEN("the pattern gcode is generated") {
            CustomGCode::Info info = pattern.generate_custom_gcodes(config, false, *fixture.object, Vec3d::Zero());

            THEN("on the first layer, wall-start X deltas alternate between wall and pattern spacing") {
                REQUIRE_FALSE(info.gcodes.empty());
                auto starts = extract_wall_starts(info.gcodes.front().extra);
                // 7 patterns x 3 walls = 21 wall starts on the first layer.
                REQUIRE(starts.size() == 21);

                // line_spacing_angle = line_spacing / sin(45deg) = 0.457079633/0.707106781
                // = 0.646408216; X is quantized to 3 decimals, so the delta reads .646/.647
                // (max quantization error 0.0006).
                const double wall_delta = 0.6464082156198468;
                for (size_t i = 1; i < starts.size(); ++i) {
                    double delta = starts[i].first - starts[i - 1].first;
                    if (i % 3 == 0)
                        // between patterns: m_pattern_spacing(2) + line_width(0.5) = 2.5 exact
                        CHECK_THAT(delta, WithinAbs(2.5, 1e-6));
                    else
                        // within a pattern: adjacent walls are line_spacing_angle apart
                        CHECK_THAT(delta, WithinAbs(wall_delta, 0.0008));
                }
            }
        }
    }
}
