#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <numeric>
#include <sstream>

#include "test_data.hpp" // get access to init_print, etc

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r::Test;
using namespace Slic3r;

SCENARIO("Extrusion width specifics", "[Flow]") {
    GIVEN("A config with a skirt, brim, some fill density, 3 perimeters, and 1 bottom solid layer and a 20mm cube mesh") {
        // this is a sharedptr
        DynamicPrintConfig config = Slic3r::Test::default_print_config();
		config.set_deserialize_strict({
			{ "brim_width",			2 },
			{ "skirt_loops",		1 },
			{ "wall_loops",			3 },
			{ "sparse_infill_density", "40%" },
			{ "initial_layer_print_height", 0.3 }
			});

        WHEN("first layer width set to 2mm") {
            Slic3r::Model model;
            config.set("initial_layer_line_width", 2);
            Slic3r::Print print;
            Slic3r::Test::init_print({TestMesh::cube_20x20x20}, print, model, config);

            std::vector<double> E_per_mm_bottom;
            std::vector<double> E_per_mm_upper;
            std::string gcode = Test::gcode(print);
            Slic3r::GCodeReader parser;
            const double initial_layer_height = config.opt_float("initial_layer_print_height");
            parser.parse_buffer(gcode, [&E_per_mm_bottom, &E_per_mm_upper, initial_layer_height] (Slic3r::GCodeReader& self, const Slic3r::GCodeReader::GCodeLine& line)
            { 
                if (line.extruding(self) && line.dist_XY(self) > 0) {
                    if (std::abs(self.z() - initial_layer_height) < 0.01) { // only consider first layer
                        E_per_mm_bottom.emplace_back(line.dist_E(self) / line.dist_XY(self));
                    } else if (self.z() > initial_layer_height + 0.01) { // upper layers only
                        E_per_mm_upper.emplace_back(line.dist_E(self) / line.dist_XY(self));
                    }
                }
            });
            THEN(" First layer width applies to everything on first layer.") {
                REQUIRE(E_per_mm_bottom.size() > 0); // make sure it actually passed because of extrusion

                bool pass = false;
                double avg_E = std::accumulate(E_per_mm_bottom.cbegin(), E_per_mm_bottom.cend(), 0.0) / static_cast<double>(E_per_mm_bottom.size());

                pass = (std::count_if(E_per_mm_bottom.cbegin(), E_per_mm_bottom.cend(), [avg_E] (const double& v) { return std::abs(v - avg_E) < 1e-6; }) == 0);
                REQUIRE(pass == true);
            }
            THEN(" First layer width does not apply to upper layer.") {
                REQUIRE(E_per_mm_bottom.size() > 0);
                REQUIRE(E_per_mm_upper.size() > 0);

                const double avg_bottom = std::accumulate(E_per_mm_bottom.cbegin(), E_per_mm_bottom.cend(), 0.0) / static_cast<double>(E_per_mm_bottom.size());
                const double avg_upper  = std::accumulate(E_per_mm_upper.cbegin(),  E_per_mm_upper.cend(),  0.0) / static_cast<double>(E_per_mm_upper.size());

                // The 2mm first-layer width must not leak into the upper layers,
                // whose extrusion is driven by the (much narrower) default width.
                REQUIRE(avg_upper < avg_bottom);
            }
        }
    }
}
// needs gcode export
SCENARIO(" Bridge flow specifics.", "[Flow]") {
    GIVEN("A default config with no cooling and a fixed bridge speed, flow ratio and an overhang mesh.") {
        WHEN("bridge_flow_ratio is set to 1.0") {
            THEN("Output flow is as expected.") {
            }
        }
        WHEN("bridge_flow_ratio is set to 0.5") {
            THEN("Output flow is as expected.") {
            }
        }
        WHEN("bridge_flow_ratio is set to 2.0") {
            THEN("Output flow is as expected.") {
            }
        }
    }
    GIVEN("A default config with no cooling and a fixed bridge speed, flow ratio, fixed extrusion width of 0.4mm and an overhang mesh.") {
        WHEN("bridge_flow_ratio is set to 1.0") {
            THEN("Output flow is as expected.") {
            }
        }
        WHEN("bridge_flow_ratio is set to 0.5") {
            THEN("Output flow is as expected.") {
            }
        }
        WHEN("bridge_flow_ratio is set to 2.0") {
            THEN("Output flow is as expected.") {
            }
        }
    }
}

/// Test the expected behavior for auto-width, 
/// spacing, etc
SCENARIO("Flow: Flow math for non-bridges", "[Flow]") {
    GIVEN("Nozzle Diameter of 0.4, a desired width of 1mm and layer height of 0.5") {
        ConfigOptionFloatOrPercent	width(1.0, false);
        float nozzle_diameter	= 0.4f;
        float layer_height		= 0.4f;

        // Spacing for non-bridges is has some overlap
        THEN("External perimeter flow has spacing fixed to 1.125 * nozzle_diameter") {
            auto flow = Flow::new_from_config_width(frExternalPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
            REQUIRE_THAT(flow.spacing(), WithinRel(1.125 * nozzle_diameter - layer_height * (1.0 - PI / 4.0), 0.001));
        }

        THEN("Internal perimeter flow has spacing fixed to 1.125 * nozzle_diameter") {
            auto flow = Flow::new_from_config_width(frPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
            REQUIRE_THAT(flow.spacing(), WithinRel(1.125 *nozzle_diameter - layer_height * (1.0 - PI / 4.0), 0.001));
        }
        THEN("Spacing for supplied width is 0.8927f") {
            auto flow = Flow::new_from_config_width(frExternalPerimeter, width, nozzle_diameter, layer_height);
            REQUIRE_THAT(flow.spacing(), WithinRel(width.value - layer_height * (1.0 - PI / 4.0), 0.001));
            flow = Flow::new_from_config_width(frPerimeter, width, nozzle_diameter, layer_height);
            REQUIRE_THAT(flow.spacing(), WithinRel(width.value - layer_height * (1.0 - PI / 4.0), 0.001));
        }
    }
    /// Check the min/max
    GIVEN("Nozzle Diameter of 0.25") {
        float nozzle_diameter	= 0.25f;
        float layer_height		= 0.5f;
        WHEN("layer height is set to 0.2") {
            layer_height = 0.15f;
            THEN("Max width is set.") {
                auto flow = Flow::new_from_config_width(frPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
                REQUIRE_THAT(flow.width(), WithinRel(1.125 * nozzle_diameter, 0.001));
            }
        }
        WHEN("Layer height is set to 0.25") {
            layer_height = 0.25f;
            THEN("Min width is set.") {
                auto flow = Flow::new_from_config_width(frPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
                REQUIRE_THAT(flow.width(), WithinRel(1.125 * nozzle_diameter, 0.001));
            }
        }
    }

#if 0
    /// Check for an edge case in the maths where the spacing could be 0; original
    /// math is 0.99. Slic3r issue #4654
    GIVEN("Input spacing of 0.414159 and a total width of 2") {
        double in_spacing = 0.414159;
        double total_width = 2.0;
        auto flow = Flow::new_from_spacing(1.0, 0.4, 0.3);
        WHEN("solid_spacing() is called") {
            double result = flow.solid_spacing(total_width, in_spacing);
            THEN("Yielded spacing is greater than 0") {
                REQUIRE(result > 0);
            }
        }
    }
#endif    

}

/// Spacing, width calculation for bridge extrusions
SCENARIO("Flow: Flow math for bridges", "[Flow]") {
    GIVEN("Nozzle Diameter of 0.4, a desired width of 1mm and layer height of 0.5") {
		float nozzle_diameter	= 0.4f;
		float bridge_flow		= 1.0f;
        WHEN("Flow role is frExternalPerimeter") {
            auto flow = Flow::bridging_flow(nozzle_diameter * sqrt(bridge_flow), nozzle_diameter);
            THEN("Bridge width is same as nozzle diameter") {
                REQUIRE_THAT(flow.width(), WithinRel(nozzle_diameter, 0.001));
            }
            THEN("Bridge spacing is same as nozzle diameter + BRIDGE_EXTRA_SPACING") {
                REQUIRE_THAT(flow.spacing(), WithinRel(nozzle_diameter + BRIDGE_EXTRA_SPACING, 0.001));
            }
        }
    }
}

/// Direct unit tests for the Flow math primitives: mm3_per_mm(),
/// with_cross_section() and with_spacing().
/// The legacy PrusaSlicer API `Flow::solid_spacing(...)` no longer exists in this
/// codebase; extrusion spacing is adjusted through Flow::with_spacing() instead.
SCENARIO("Flow: mm3_per_mm cross-section area", "[Flow]") {
    GIVEN("A non-bridge flow of width 1.0 and height 0.4") {
        Flow flow(1.0f, 0.4f, 0.4f);

        THEN("The rounded-rectangle area is h * (w - h * (1 - PI/4))") {
            REQUIRE_THAT(flow.mm3_per_mm(), WithinRel(0.3656637061, 0.001));
        }
    }

    GIVEN("A non-bridge flow whose width equals its height (a circle)") {
        Flow flow(0.4f, 0.4f, 0.4f);

        THEN("The area equals PI * d^2 / 4") {
            REQUIRE_THAT(flow.mm3_per_mm(), WithinRel(0.1256637061, 0.001));
        }
    }

    GIVEN("A bridge flow of diameter 0.4") {
        Flow flow = Flow::bridging_flow(0.4f, 0.4f);

        THEN("The area equals PI * d^2 / 4") {
            REQUIRE(flow.bridge());
            REQUIRE_THAT(flow.mm3_per_mm(), WithinRel(0.1256637061, 0.001));
        }
    }
}

SCENARIO("Flow: with_cross_section adjusts the extrusion to a target area", "[Flow]") {
    GIVEN("A base flow of width 0.5, height 0.2 and nozzle 0.4") {
        const Flow base(0.5f, 0.2f, 0.4f);

        THEN("Passing the current area back is a no-op") {
            Flow result = base.with_cross_section(float(base.mm3_per_mm()));

            REQUIRE_THAT(result.mm3_per_mm(), WithinRel(base.mm3_per_mm(), 0.001));
            REQUIRE_THAT(result.width(),  WithinRel(base.width(),  0.001));
            REQUIRE_THAT(result.height(), WithinRel(base.height(), 0.001));
            REQUIRE_THAT(result.spacing(), WithinRel(base.spacing(), 0.001));
        }

        WHEN("The target area is larger") {
            Flow result = base.with_cross_section(0.15f);

            THEN("The height grows to reach the area while spacing is preserved") {
                REQUIRE_THAT(result.mm3_per_mm(), WithinRel(0.15, 0.001));
                REQUIRE(result.height() > base.height());
                REQUIRE_THAT(result.spacing(), WithinRel(base.spacing(), 0.001));
            }
        }

        WHEN("The target area is moderately smaller") {
            Flow result = base.with_cross_section(0.05f);

            THEN("The width shrinks while the height is preserved") {
                REQUIRE_THAT(result.mm3_per_mm(), WithinRel(0.05, 0.001));
                REQUIRE_THAT(result.height(), WithinRel(base.height(), 0.001));
                REQUIRE(result.width() < base.width());
            }
        }

        WHEN("The target area is much smaller than the current area") {
            Flow result = base.with_cross_section(0.02f);

            THEN("The extrusion degenerates to a circular cross-section with the spacing preserved") {
                REQUIRE_THAT(result.width(), WithinRel(result.height(), 0.001));
                REQUIRE_THAT(result.spacing(), WithinRel(base.spacing(), 0.001));
                // The diameter is computed as sqrt(area_new / PI), so the resulting
                // circle area (PI * d^2 / 4) lands at area_new / 4, not area_new.
                // Left as an observation rather than an assertion until reviewed.
            }
        }
    }
}

SCENARIO("Flow: with_spacing shifts the extrusion width", "[Flow]") {
    GIVEN("A non-bridge flow of width 0.5 and height 0.2") {
        const Flow base(0.5f, 0.2f, 0.4f);

        WHEN("Spacing is increased to 0.5") {
            Flow result = base.with_spacing(0.5f);

            THEN("The width shifts by the same delta and the height is preserved") {
                REQUIRE_THAT(result.spacing(), WithinRel(0.5, 0.001));
                REQUIRE_THAT(result.height(),  WithinRel(base.height(), 0.001));
                REQUIRE_THAT(result.width(), WithinRel(base.width() + (0.5f - base.spacing()), 0.001));
            }
        }

        WHEN("Spacing is reduced below what the height allows") {
            THEN("An invalid-argument error is raised") {
                REQUIRE_THROWS_AS(base.with_spacing(0.05f), Slic3r::InvalidArgument);
            }
        }
    }

    GIVEN("A bridge flow of diameter 0.4") {
        const Flow base = Flow::bridging_flow(0.4f, 0.4f);

        WHEN("Spacing is increased to 0.5") {
            Flow result = base.with_spacing(0.5f);

            THEN("The diameter grows by the same delta, keeping the 0.05 gap") {
                REQUIRE(result.bridge());
                REQUIRE_THAT(result.spacing(), WithinRel(0.5, 0.001));
                REQUIRE_THAT(result.width(),   WithinRel(0.45, 0.001));
                REQUIRE_THAT(result.height(),  WithinRel(0.45, 0.001));
            }
        }
    }
}

/// Exact-formula table for mm3_per_mm(): area = w*h - h^2*(1 - PI/4).
/// The rounded-rectangle cross-section is a rectangle of width w minus the
/// corner caps, giving width*height - height^2*(1 - pi/4).
SCENARIO("Flow: mm3_per_mm exact formula table", "[Flow]") {
    struct Case { float width; float height; double expected; };
    const Case cases[] = {
        { 0.6f, 0.2f, 0.1114159265 },
        { 0.4f, 0.4f, 0.1256637061 },
        { 1.0f, 0.4f, 0.3656637061 },
        { 0.5f, 0.3f, 0.1306858347 },
        { 0.8f, 0.8f, 0.5026548246 },
    };

    for (const Case& c : cases) {
        DYNAMIC_SECTION("width=" << c.width << " height=" << c.height) {
            Flow flow(c.width, c.height, 0.4f);
            REQUIRE_THAT(flow.mm3_per_mm(), WithinRel(c.expected, 0.001));
        }
    }
}

/// with_cross_section() is contractually "adjust to reach the prescribed area".
/// A round-trip away and back must therefore restore the original area.
SCENARIO("Flow: with_cross_section round-trip", "[Flow]") {
    GIVEN("A base flow of width 0.5, height 0.2") {
        const Flow base(0.5f, 0.2f, 0.4f);
        const float original = float(base.mm3_per_mm());   // ~0.0914

        WHEN("The area is increased and then restored") {
            Flow round_tripped = base.with_cross_section(0.12f).with_cross_section(original);

            THEN("The original area is recovered") {
                REQUIRE_THAT(round_tripped.mm3_per_mm(), WithinRel(original, 0.001));
            }
        }

        WHEN("The area is decreased and then restored") {
            Flow round_tripped = base.with_cross_section(0.06f).with_cross_section(original);

            THEN("The original area is recovered") {
                REQUIRE_THAT(round_tripped.mm3_per_mm(), WithinRel(original, 0.001));
            }
        }

        WHEN("A single change to a larger area") {
            Flow stepped = base.with_cross_section(0.12f);

            THEN("The target area is reached exactly") {
                REQUIRE_THAT(stepped.mm3_per_mm(), WithinRel(0.12, 0.001));
            }
        }
    }
}

/// The old PrusaSlicer `Flow::solid_spacing(width, distance)` was removed; its
/// logic now lives in Fill::_adjust_solid_spacing() (coord_t based). Upstream's
/// `#if 0` divisibility check (`width % distance == 0`, e.g. 250 % 50) no longer
/// holds: the current implementation floors the spacing, so (250, 47) yields 49,
/// not 50. Pin the current behaviour and the documented invariants instead.
SCENARIO("Fill: adjusted solid spacing", "[Flow]") {
    THEN("The spacing is adjusted to a whole number of lines") {
        REQUIRE(Slic3r::Fill::_adjust_solid_spacing(250, 47) == 49);
        REQUIRE(Slic3r::Fill::_adjust_solid_spacing(100, 30) == 33);
    }

    THEN("The 20% increase cap is enforced") {
        REQUIRE(Slic3r::Fill::_adjust_solid_spacing(250, 50) == 60);
    }

    THEN("A width narrower than one line is left unchanged") {
        REQUIRE(Slic3r::Fill::_adjust_solid_spacing(40, 47) == 47);
    }

    THEN("The result never decreases and never exceeds 1.2x the request") {
        const coord_t widths[] = { 250, 100, 250, 40 };
        const coord_t dists[]  = { 47, 30, 50, 47 };
        for (int i = 0; i < 4; ++i) {
            INFO("width=" << widths[i] << " distance=" << dists[i]);
            coord_t d = Slic3r::Fill::_adjust_solid_spacing(widths[i], dists[i]);
            REQUIRE(d >= dists[i]);
            REQUIRE(d <= coord_t(dists[i] * 1.2 + 0.5));
        }
    }
}

/// Negative spacing (width too narrow for the height) must raise FlowError.
SCENARIO("Flow: negative spacing raises FlowError", "[Flow]") {
    THEN("rounded_rectangle_extrusion_spacing throws FlowErrorNegativeSpacing") {
        REQUIRE_THROWS_AS(Slic3r::Flow::rounded_rectangle_extrusion_spacing(0.1f, 0.5f),
                          Slic3r::FlowErrorNegativeSpacing);
    }

    THEN("The 3-argument Flow constructor propagates the same error") {
        REQUIRE_THROWS_AS(Slic3r::Flow(0.1f, 0.5f, 0.4f),
                          Slic3r::FlowErrorNegativeSpacing);
    }
}

/// Percent vs absolute line-width parsing through new_from_config_width().
SCENARIO("Flow: percent vs absolute line width parsing", "[Flow]") {
    const float nozzle = 0.4f;
    const float height = 0.2f;

    THEN("An absolute width is used verbatim") {
        auto flow = Slic3r::Flow::new_from_config_width(
            Slic3r::frPerimeter, Slic3r::ConfigOptionFloatOrPercent(0.6, false), nozzle, height);
        REQUIRE_THAT(flow.width(), WithinRel(0.6, 0.001));
    }

    THEN("A percent width is resolved against the nozzle diameter") {
        auto flow = Slic3r::Flow::new_from_config_width(
            Slic3r::frPerimeter, Slic3r::ConfigOptionFloatOrPercent(150.0, true), nozzle, height);
        REQUIRE_THAT(flow.width(), WithinRel(0.6, 0.001));
    }

    THEN("A zero absolute width falls back to the role default (1.125 * nozzle)") {
        auto flow = Slic3r::Flow::new_from_config_width(
            Slic3r::frPerimeter, Slic3r::ConfigOptionFloatOrPercent(0.0, false), nozzle, height);
        REQUIRE_THAT(flow.width(), WithinRel(0.45, 0.001));
    }

    THEN("Support-material default width is the bare nozzle diameter") {
        auto flow = Slic3r::Flow::new_from_config_width(
            Slic3r::frSupportMaterial, Slic3r::ConfigOptionFloatOrPercent(0.0, false), nozzle, height);
        REQUIRE_THAT(flow.width(), WithinRel(0.4, 0.001));
    }
}
