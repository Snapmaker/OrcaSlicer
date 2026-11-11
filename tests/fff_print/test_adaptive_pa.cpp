#include "catch_main.hpp"

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/AdaptivePAProcessor.hpp"
#include "libslic3r/GCode/PchipInterpolatorHelper.hpp"
#include "libslic3r/GCode/AdaptivePAInterpolator.hpp"

using namespace Slic3r; // AdaptivePAProcessor

// PchipInterpolatorHelper and AdaptivePAInterpolator live in the global namespace.

// A valid model: 2 accelerations (5000/10000) x 2 flow rates (0.5/1.0 mm^3/s).
static const char *kModelCSV =
    "0.05,0.5,5000\n"
    "0.10,1.0,5000\n"
    "0.08,0.5,10000\n"
    "0.15,1.0,10000\n";

// --- Scope: PCHIP interpolation -------------------------------------------------

SCENARIO("PCHIP reproduces node values exactly", "[AdaptivePA]") {
    GIVEN("a PCHIP model over y = x^2") {
        PchipInterpolatorHelper p;
        p.setData({ 0.0, 1.0, 2.0, 3.0 }, { 0.0, 1.0, 4.0, 9.0 });

        THEN("each node interpolates to its exact y") {
            REQUIRE(p.interpolate(0.0) == 0.0);
            REQUIRE(p.interpolate(1.0) == 1.0);
            REQUIRE(p.interpolate(2.0) == 4.0);
            REQUIRE(p.interpolate(3.0) == 9.0);
        }
        THEN("an interior point exercises the Hermite coefficients") {
            // PCHIP is C1 but not C2: 2.21875 (not the true 2.25) is the PCHIP value.
            REQUIRE_THAT(p.interpolate(1.5), Catch::Matchers::WithinRel(2.21875, 1e-9));
        }
    }
}

SCENARIO("PCHIP clamps at the boundaries", "[AdaptivePA]") {
    GIVEN("a PCHIP model") {
        PchipInterpolatorHelper p;
        p.setData({ 0.0, 1.0, 2.0 }, { 5.0, 10.0, 20.0 });

        THEN("out-of-range inputs clamp to the end values") {
            REQUIRE(p.interpolate(-1.0) == 5.0);   // left clamp
            REQUIRE(p.interpolate(3.0) == 20.0);   // right clamp
        }
    }
}

SCENARIO("PCHIP degenerates to linear for two points", "[AdaptivePA]") {
    GIVEN("two collinear points") {
        PchipInterpolatorHelper p;
        p.setData({ 0.0, 10.0 }, { 0.0, 20.0 }); // y = 2x

        THEN("the midpoint is the linear midpoint") {
            REQUIRE(p.interpolate(5.0) == 10.0);
        }
    }
}

SCENARIO("PCHIP rejects invalid input", "[AdaptivePA]") {
    GIVEN("mismatched or too-few points") {
        PchipInterpolatorHelper p;
        THEN("setData throws std::invalid_argument") {
            REQUIRE_THROWS_AS(p.setData({ 1.0 }, { 1.0 }), std::invalid_argument);       // < 2 points
            REQUIRE_THROWS_AS(p.setData({ 1.0, 2.0 }, { 1.0 }), std::invalid_argument);  // size mismatch
        }
    }
}

// --- Scope: CSV parse return codes ------------------------------------------------

SCENARIO("AdaptivePAInterpolator parses valid CSV", "[AdaptivePA]") {
    GIVEN("a valid 2-acceleration, 2-flow model") {
        AdaptivePAInterpolator interp;
        int rc = interp.parseAndSetData(kModelCSV);

        THEN("parse succeeds and node / middle-accel interpolation is exact") {
            REQUIRE(rc == 0);
            REQUIRE(interp.isInitialised());
            REQUIRE_THAT(interp(0.5, 5000), Catch::Matchers::WithinRel(0.05, 1e-9));  // flow node
            REQUIRE_THAT(interp(0.5, 7500), Catch::Matchers::WithinRel(0.065, 1e-9)); // middle accel
        }
    }
}

SCENARIO("AdaptivePAInterpolator reports parse errors", "[AdaptivePA]") {
    GIVEN("a CSV with a non-numeric field") {
        AdaptivePAInterpolator interp;
        int rc = interp.parseAndSetData("abc,0.5,5000\n");

        THEN("parse returns -1") {
            REQUIRE(rc == -1);
        }
    }
}

SCENARIO("AdaptivePAInterpolator empty CSV is a fake success", "[AdaptivePA]") {
    GIVEN("an empty CSV string") {
        AdaptivePAInterpolator interp;
        int rc = interp.parseAndSetData("");

        THEN("parse returns 0 but there is no data to interpolate") {
            // FIXME: "fake success" — empty CSV returns 0 and isInitialised()==true, but
            // the interpolator holds no data and operator() returns -1. Pinned as a known
            // quirk; consider returning -1 from parseAndSetData for empty/insufficient input.
            REQUIRE(rc == 0);                // the "fake success" trap
            REQUIRE(interp.isInitialised()); // flag is still set
            REQUIRE(interp(0.5, 5000) == -1.0); // but operator() fails
        }
    }
}

SCENARIO("AdaptivePAInterpolator returns the value for a single acceleration", "[AdaptivePA]") {
    GIVEN("a model with a single acceleration") {
        AdaptivePAInterpolator interp;
        int rc = interp.parseAndSetData("0.05,0.5,5000\n0.10,1.0,5000\n");

        THEN("the single-acceleration branch returns the flow-interpolated value") {
            REQUIRE(rc == 0);
            REQUIRE_THAT(interp(0.5, 5000), Catch::Matchers::WithinRel(0.05, 1e-9));  // flow node
            REQUIRE_THAT(interp(0.75, 5000), Catch::Matchers::WithinRel(0.075, 1e-9)); // linear midpoint
        }
    }
}

// --- Scope: AdaptivePAProcessor ------------------------------------------------

// Build a GCode + AdaptivePAProcessor with adaptive PA enabled for tool 0, using the
// given model CSV, then hand the processor to a callback (GCode must outlive it).
template <typename Fn>
static void with_processor(const std::string &model, double fallback_pa, Fn &&fn)
{
    FullPrintConfig config = FullPrintConfig::defaults();
    config.adaptive_pressure_advance.values        = { true };
    config.enable_pressure_advance.values          = { true };
    config.adaptive_pressure_advance_model.values  = { model };
    config.adaptive_pressure_advance_bridges.values = { 0.09 };
    config.pressure_advance.values                 = { fallback_pa };
    config.gcode_flavor.value                      = gcfMarlinFirmware;

    GCode gcodegen;
    gcodegen.apply_print_config(config);
    gcodegen.writer().set_extruders({ 0 });
    gcodegen.writer().set_extruder(0);
    Slic3r::AdaptivePAProcessor proc(gcodegen, { 0 });
    fn(proc);
}

SCENARIO("AdaptivePAProcessor injects the correct pressure advance on PA_CHANGE", "[AdaptivePA]") {
    GIVEN("a processor with a valid model and a PA_CHANGE-tagged layer") {
        with_processor(kModelCSV, 0.05, [](Slic3r::AdaptivePAProcessor &proc) {
            WHEN("a tagged layer is processed") {
                // MM3MM:0.005 x 100mm/s = 0.5 mm^3/s, the model's first flow node ->
                // predicted PA = 0.05 at accel 5000.
                std::string out = proc.process_layer(
                    "; PA_CHANGE:T0 MM3MM:0.005 ACCEL:5000 BR:0 RC:0 OV:0\n"
                    "G1 F6000\n"
                    "G1 X10 Y0 E1\n");

                THEN("M900 K0.05 is injected (not just any M900)") {
                    REQUIRE(out.find("M900 K0.05") != std::string::npos);
                }
            }
        });
    }
}

SCENARIO("AdaptivePAProcessor suppresses duplicate pressure advance", "[AdaptivePA]") {
    GIVEN("a processor and two identical PA_CHANGE tags") {
        with_processor(kModelCSV, 0.05, [](Slic3r::AdaptivePAProcessor &proc) {
            WHEN("two identical PA_CHANGE tags are processed") {
                std::string out = proc.process_layer(
                    "; PA_CHANGE:T0 MM3MM:0.005 ACCEL:5000 BR:0 RC:0 OV:0\n"
                    "G1 F6000\n"
                    "G1 X10 Y0 E1\n"
                    "; PA_CHANGE:T0 MM3MM:0.005 ACCEL:5000 BR:0 RC:0 OV:0\n"
                    "G1 F6000\n"
                    "G1 X20 Y0 E1\n");

                THEN("only one pressure-advance command is emitted") {
                    size_t first = out.find("M900 K");
                    REQUIRE(first != std::string::npos);
                    REQUIRE(out.find("M900 K", first + 1) == std::string::npos);
                }
            }
        });
    }
}

SCENARIO("AdaptivePAProcessor falls back to the default pressure advance on bad model", "[AdaptivePA]") {
    GIVEN("a processor whose model fails to parse") {
        with_processor("abc,0.5,5000\n", 0.03, [](Slic3r::AdaptivePAProcessor &proc) {
            WHEN("a tagged layer is processed") {
                std::string out = proc.process_layer(
                    "; PA_CHANGE:T0 MM3MM:0.005 ACCEL:5000 BR:0 RC:0 OV:0\n"
                    "G1 F6000\n"
                    "G1 X10 Y0 E1\n");

                THEN("the fallback pressure_advance (0.03) is used, distinct from the model value") {
                    REQUIRE(out.find("M900 K0.03") != std::string::npos);
                }
            }
        });
    }
}

SCENARIO("AdaptivePAProcessor uses the bridge PA override", "[AdaptivePA]") {
    GIVEN("a processor and a BR:1 (bridge) PA_CHANGE tag") {
        with_processor(kModelCSV, 0.05, [](Slic3r::AdaptivePAProcessor &proc) {
            WHEN("a bridge-tagged layer is processed") {
                std::string out = proc.process_layer(
                    "; PA_CHANGE:T0 MM3MM:0.005 ACCEL:5000 BR:1 RC:0 OV:0\n"
                    "G1 F6000\n"
                    "G1 X10 Y0 E1\n");

                THEN("the adaptive_pressure_advance_bridges value (0.09) is used") {
                    REQUIRE(out.find("M900 K0.09") != std::string::npos);
                }
            }
        });
    }
}
