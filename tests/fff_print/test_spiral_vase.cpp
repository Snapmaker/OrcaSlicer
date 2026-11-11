#include "catch_main.hpp"

#include <cstdio>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/SpiralVase.hpp"

using namespace Slic3r;

// Fill the spiral-vase fields of a PrintConfig. Relative E is used throughout so
// transition-layer E tapering is active.
static void init_vase_config(FullPrintConfig &config, bool smooth, bool relative_e,
                             float start_ratio = 0.5f, float finish_ratio = 0.5f)
{
    config.spiral_mode_smooth.value          = smooth;
    config.use_relative_e_distances.value    = relative_e;
    config.spiral_starting_flow_ratio.value  = start_ratio;
    config.spiral_finishing_flow_ratio.value = finish_ratio;
    config.z_offset.value                    = 0.0f;
}

// A single vase layer with a leading Z move and a two-segment straight extrusion
// path (x1 -> x2 along Y=0). Z is absolute and increments per layer.
static std::string layer_at(float z, float x1, float x2)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "G1 Z%.1f F600\nG1 X%.0f Y0 E0.5 F600\nG1 X%.0f Y0 E0.5 F600\n",
                  z, x1, x2);
    return std::string(buf);
}

SCENARIO("SpiralVase disabled passes gcode through unchanged", "[SpiralVase]") {
    GIVEN("a disabled vase (never enabled)") {
        FullPrintConfig config = FullPrintConfig::defaults();
        init_vase_config(config, false, true);
        SpiralVase vase(config);

        WHEN("a layer is processed") {
            std::string in  = layer_at(0.2f, 10.f, 20.f);
            std::string out = vase.process_layer(in, false);

            THEN("the output is byte-identical to the input") {
                REQUIRE(out == in);
            }
        }
    }
}

SCENARIO("SpiralVase ramps Z and E on the transition layer", "[SpiralVase]") {
    GIVEN("a relative-E vase with start ratio 0.5") {
        FullPrintConfig config = FullPrintConfig::defaults();
        init_vase_config(config, false, true, 0.5f, 0.5f);
        SpiralVase vase(config);
        vase.enable(true); // first enabled layer is the transition layer

        WHEN("the transition layer is processed") {
            std::string out = vase.process_layer(layer_at(0.2f, 10.f, 20.f), false);

            THEN("Z ramps from the layer base and E ramps up from half flow") {
                const std::string expected =
                    "G1 Z0.000 F600\n"
                    "G1 Z0.100 X10 Y0 E0.37500 F600\n"
                    "G1 Z0.200 X20 Y0 E0.50000 F600\n";
                REQUIRE(out == expected);
            }
        }
    }
}

SCENARIO("SpiralVase keeps full flow on non-transition layers", "[SpiralVase]") {
    GIVEN("a vase already past the transition layer") {
        FullPrintConfig config = FullPrintConfig::defaults();
        init_vase_config(config, false, true, 0.5f, 0.5f);
        SpiralVase vase(config);

        vase.enable(true);             // first layer -> transition
        vase.process_layer(layer_at(0.2f, 10.f, 20.f), false);
        vase.enable(true);             // subsequent layer -> non-transition

        WHEN("a subsequent layer is processed") {
            std::string out = vase.process_layer(layer_at(0.4f, 10.f, 20.f), false);

            THEN("E is not tapered (full flow) while Z still ramps") {
                const std::string expected =
                    "G1 Z0.200 F600\n"
                    "G1 Z0.300 X10 Y0 E0.5 F600\n"
                    "G1 Z0.400 X20 Y0 E0.5 F600\n";
                REQUIRE(out == expected);
            }
        }
    }
}

SCENARIO("SpiralVase ramps E down on the last layer", "[SpiralVase]") {
    GIVEN("a relative-E vase with finish ratio 0.5") {
        FullPrintConfig config = FullPrintConfig::defaults();
        init_vase_config(config, false, true, 0.5f, 0.5f);
        SpiralVase vase(config);

        vase.enable(true);
        vase.process_layer(layer_at(0.2f, 10.f, 20.f), false);
        vase.enable(true);

        WHEN("the last layer is processed") {
            std::string out = vase.process_layer(layer_at(0.4f, 10.f, 20.f), true);

            THEN("a ramped-down transition layer is appended at constant Z") {
                const std::string expected =
                    "G1 Z0.200 F600\n"
                    "G1 Z0.300 X10 Y0 E0.5 F600\n"
                    "G1 Z0.400 X20 Y0 E0.5 F600\n"
                    "G1 X10 Y0 E0.37500 F600\n"
                    "G1 X20 Y0 E0.25000 F600\n";
                REQUIRE(out == expected);
            }
        }
    }
}

SCENARIO("SpiralVase filters retractions at layer change", "[SpiralVase]") {
    GIVEN("an enabled vase") {
        FullPrintConfig config = FullPrintConfig::defaults();
        init_vase_config(config, false, true, 0.5f, 0.5f);
        SpiralVase vase(config);
        vase.enable(true);

        WHEN("the layer contains a leading retraction") {
            const std::string gcode =
                "G1 Z0.2 F600\n"
                "G1 E-0.5 F600\n"
                "G1 X10 Y0 E0.5 F600\n"
                "G1 X20 Y0 E0.5 F600\n";
            std::string out = vase.process_layer(gcode, false);

            THEN("the retraction is not present in the output") {
                REQUIRE(out.find("E-0.5") == std::string::npos);
            }
        }
    }
}

SCENARIO("SpiralVase interpolates XY on the smooth spiral", "[SpiralVase]") {
    GIVEN("a smooth-spiral vase with a large smoothing distance") {
        FullPrintConfig config = FullPrintConfig::defaults();
        init_vase_config(config, true, true, 0.5f, 0.5f);
        SpiralVase vase(config);
        vase.set_max_xy_smoothing(1000.f);

        vase.enable(true);
        vase.process_layer(layer_at(0.2f, 10.f, 20.f), false); // seed previous layer
        vase.enable(true);

        WHEN("the next layer (shifted in Y) is processed") {
            std::string out = vase.process_layer(
                "G1 Z0.4 F600\nG1 X10 Y2 E0.5 F600\nG1 X20 Y2 E0.5 F600\n", false);

            THEN("the extrusion XY is blended toward the previous layer and E rescaled") {
                // The first segment (factor ≈ 0.505, a diagonal move) interpolates Y=2
                // toward the previous layer's Y=0 -> Y1.010, and E is rescaled by the
                // length change. The last segment (factor 1.0) keeps its XY but still
                // rescales E.
                const std::string expected =
                    "G1 Z0.200 F600\n"
                    "G1 Z0.301 X10.000 Y1.010 E0.49278 F600\n"
                    "G1 Z0.400 X20.000 Y2.000 E0.50245 F600\n";
                REQUIRE(out == expected);
            }
        }
    }
}

SCENARIO("SpiralVase skips smoothing beyond the threshold", "[SpiralVase]") {
    GIVEN("a smooth vase with a tiny smoothing distance and a plain vase") {
        FullPrintConfig cfg_smooth = FullPrintConfig::defaults();
        init_vase_config(cfg_smooth, true, true, 0.5f, 0.5f);
        SpiralVase smooth(cfg_smooth);
        smooth.set_max_xy_smoothing(0.5f); // dist=2 > 0.5 -> no interpolation

        FullPrintConfig cfg_plain = FullPrintConfig::defaults();
        init_vase_config(cfg_plain, false, true, 0.5f, 0.5f);
        SpiralVase plain(cfg_plain);

        const std::string layer1 = "G1 Z0.2 F600\nG1 X10 Y0 E0.5 F600\nG1 X20 Y0 E0.5 F600\n";
        const std::string layer2 = "G1 Z0.4 F600\nG1 X10 Y2 E0.5 F600\nG1 X20 Y2 E0.5 F600\n";

        smooth.enable(true);
        smooth.process_layer(layer1, false);
        smooth.enable(true);
        plain.enable(true);
        plain.process_layer(layer1, false);
        plain.enable(true);

        WHEN("the shifted second layer is processed") {
            std::string out_smooth = smooth.process_layer(layer2, false);
            std::string out_plain  = plain.process_layer(layer2, false);

            THEN("the smooth mode degrades to plain (no interpolation)") {
                REQUIRE(out_smooth == out_plain);
            }
        }
    }
}

SCENARIO("SpiralVase disables transition taper with absolute E", "[SpiralVase]") {
    GIVEN("an absolute-E vase with start ratio 0.5") {
        FullPrintConfig config = FullPrintConfig::defaults();
        init_vase_config(config, false, /*relative_e=*/false, 0.5f, 0.5f);
        SpiralVase vase(config);
        vase.enable(true); // would be the transition layer

        WHEN("the layer is processed") {
            std::string out = vase.process_layer(
                "G1 Z0.2 F600\nG1 X10 Y0 E0.5 F600\nG1 X20 Y0 E1 F600\n", false);

            THEN("E is not tapered, but Z still ramps normally") {
                const std::string expected =
                    "G1 Z0.000 F600\n"
                    "G1 Z0.100 X10 Y0 E0.5 F600\n"
                    "G1 Z0.200 X20 Y0 E1 F600\n";
                REQUIRE(out == expected);
            }
        }
    }
}

SCENARIO("SpiralVase handles an empty layer", "[SpiralVase]") {
    GIVEN("an enabled vase") {
        FullPrintConfig config = FullPrintConfig::defaults();
        init_vase_config(config, false, true, 0.5f, 0.5f);
        SpiralVase vase(config);
        vase.enable(true);

        WHEN("an empty layer is processed") {
            std::string out = vase.process_layer("", false);

            THEN("the output is empty without crashing") {
                REQUIRE(out.empty());
            }
        }
    }
}

SCENARIO("SpiralVase filters zero-length extrusions", "[SpiralVase]") {
    GIVEN("an enabled vase") {
        FullPrintConfig config = FullPrintConfig::defaults();
        init_vase_config(config, false, true, 0.5f, 0.5f);
        SpiralVase vase(config);
        vase.enable(true);

        WHEN("the layer has a purge move (E without XY)") {
            const std::string gcode =
                "G1 Z0.2 F600\n"
                "G1 E0.1 F600\n"        // zero-XY extrusion (purge)
                "G1 X10 Y0 E0.5 F600\n"
                "G1 X20 Y0 E0.5 F600\n";
            std::string out = vase.process_layer(gcode, false);

            THEN("the purge move is filtered out") {
                // "E0.1" is chosen to not collide with the transition taper
                // values (E0.37500 / E0.50000) of the surrounding extrusions.
                REQUIRE(out.find("E0.1") == std::string::npos);
            }
        }
    }
}

SCENARIO("SpiralVase drops travel moves", "[SpiralVase]") {
    GIVEN("an enabled vase") {
        FullPrintConfig config = FullPrintConfig::defaults();
        init_vase_config(config, false, true, 0.5f, 0.5f);
        SpiralVase vase(config);
        vase.enable(true);

        WHEN("the layer has a travel move (XY without extrusion)") {
            const std::string gcode =
                "G1 Z0.2 F600\n"
                "G1 X5 Y0 F600\n"        // travel: XY, no E
                "G1 X10 Y0 E0.5 F600\n"
                "G1 X20 Y0 E0.5 F600\n";
            std::string out = vase.process_layer(gcode, false);

            THEN("the travel move is not emitted") {
                REQUIRE(out.find("X5") == std::string::npos);
            }
        }
    }
}

SCENARIO("SpiralVase re-triggers the transition after re-enable", "[SpiralVase]") {
    GIVEN("a vase that is enabled, disabled, then re-enabled") {
        FullPrintConfig config = FullPrintConfig::defaults();
        init_vase_config(config, false, true, 0.5f, 0.5f);
        SpiralVase vase(config);

        vase.enable(true);
        vase.process_layer(layer_at(0.2f, 10.f, 20.f), false); // transition

        vase.enable(false);
        std::string passthrough_in = layer_at(0.4f, 10.f, 20.f);

        WHEN("a layer is processed while disabled") {
            std::string out = vase.process_layer(passthrough_in, false);
            THEN("it passes through unchanged") {
                REQUIRE(out == passthrough_in);
            }
        }

        AND_WHEN("the next layer is processed after re-enable") {
            vase.enable(true); // re-enable -> transition again
            std::string out = vase.process_layer(layer_at(0.6f, 10.f, 20.f), false);
            THEN("the transition taper is re-applied") {
                REQUIRE(out.find("E0.375") != std::string::npos);
            }
        }
    }
}

SCENARIO("SpiralVase applies a non-zero z offset", "[SpiralVase]") {
    GIVEN("a vase configured with z_offset 0.1") {
        FullPrintConfig config = FullPrintConfig::defaults();
        init_vase_config(config, false, true, 0.5f, 0.5f);
        config.z_offset.value = 0.1f;
        SpiralVase vase(config);
        vase.enable(true);

        WHEN("the first layer is processed") {
            // Reader starts at Z = z_offset = 0.1, so a Z0.2 move is only +0.1.
            std::string out = vase.process_layer(layer_at(0.2f, 10.f, 20.f), false);

            THEN("the Z ramp is offset by 0.1") {
                const std::string expected =
                    "G1 Z0.100 F600\n"
                    "G1 Z0.150 X10 Y0 E0.37500 F600\n"
                    "G1 Z0.200 X20 Y0 E0.50000 F600\n";
                REQUIRE(out == expected);
            }
        }
    }
}
