#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "libslic3r/GCode/SmallAreaInfillFlowCompensator.hpp"

using namespace Slic3r;

// Configure the CSV compensation model (each string is one "eLength,flowComp" point).
static void init_config(GCodeConfig &config, const std::vector<std::string> &model)
{
    config.small_area_infill_flow_compensation_model.values = model;
}

SCENARIO("SmallAreaInfillFlowCompensator interpolates the CSV flow model", "[SAIFC]") {
    GIVEN("a valid model (0,0.4) (2,0.7) (5,1.0)") {
        GCodeConfig config;
        init_config(config, { "0,0.4", "2,0.7", "5,1.0" });
        SmallAreaInfillFlowCompensator comp(config);

        THEN("a line at an interior node gets the exact compensation factor") {
            REQUIRE_THAT(comp.modify_flow(2.0, 1.0, erSolidInfill), WithinRel(0.7, 1e-6));
        }

        THEN("a line between nodes gets the interpolated factor") {
            // Natural cubic spline through (0,0.4),(2,0.7),(5,1.0):
            //   S(x) = 0.4 + 0.16x - 0.0025x^3          on [0,2]  -> S(1.0) = 0.5575
            //   S(x) = 0.7 + 0.13(x-2) - 0.015(x-2)^2 + 0.0016667(x-2)^3  on [2,5] -> S(3.5) = 0.866875
            CHECK_THAT(comp.modify_flow(1.0, 1.0, erSolidInfill), WithinRel(0.5575, 1e-6));
            CHECK_THAT(comp.modify_flow(3.5, 1.0, erSolidInfill), WithinRel(0.866875, 1e-6));
        }
    }
}

SCENARIO("SmallAreaInfillFlowCompensator caps compensation at the tail of the model", "[SAIFC]") {
    GIVEN("a valid model (0,0.4) (2,0.7) (5,1.0)") {
        GCodeConfig config;
        init_config(config, { "0,0.4", "2,0.7", "5,1.0" });
        SmallAreaInfillFlowCompensator comp(config);

        THEN("the last node is exactly 1.0") {
            REQUIRE_THAT(comp.modify_flow(5.0, 1.0, erSolidInfill), WithinRel(1.0, 1e-6));
        }

        THEN("a length beyond the model is not compensated (tail = 1.0)") {
            CHECK_THAT(comp.modify_flow(5.01, 1.0, erSolidInfill), WithinRel(1.0, 1e-6));
            CHECK_THAT(comp.modify_flow(100.0, 1.0, erSolidInfill), WithinRel(1.0, 1e-6));
        }

        THEN("a zero-length line is not compensated") {
            REQUIRE_THAT(comp.modify_flow(0.0, 1.0, erSolidInfill), WithinRel(1.0, 1e-6));
        }
    }
}

SCENARIO("SmallAreaInfillFlowCompensator only compensates solid-infill roles", "[SAIFC]") {
    GIVEN("a valid model") {
        GCodeConfig config;
        init_config(config, { "0,0.4", "2,0.7", "5,1.0" });
        SmallAreaInfillFlowCompensator comp(config);

        THEN("solid-infill / top-solid / bottom-surface roles are compensated") {
            CHECK_THAT(comp.modify_flow(2.0, 1.0, erSolidInfill), WithinRel(0.7, 1e-6));
            CHECK_THAT(comp.modify_flow(2.0, 1.0, erTopSolidInfill), WithinRel(0.7, 1e-6));
            CHECK_THAT(comp.modify_flow(2.0, 1.0, erBottomSurface), WithinRel(0.7, 1e-6));
        }

        THEN("perimeter roles are left unchanged") {
            CHECK_THAT(comp.modify_flow(2.0, 1.0, erPerimeter), WithinRel(1.0, 1e-6));
            CHECK_THAT(comp.modify_flow(2.0, 1.0, erExternalPerimeter), WithinRel(1.0, 1e-6));
            CHECK_THAT(comp.modify_flow(2.0, 1.0, erOverhangPerimeter), WithinRel(1.0, 1e-6));
        }

        THEN("non-solid infill roles are left unchanged") {
            CHECK_THAT(comp.modify_flow(2.0, 1.0, erInternalInfill), WithinRel(1.0, 1e-6));
            CHECK_THAT(comp.modify_flow(2.0, 1.0, erBridgeInfill), WithinRel(1.0, 1e-6));
            CHECK_THAT(comp.modify_flow(2.0, 1.0, erInternalBridgeInfill), WithinRel(1.0, 1e-6));
            CHECK_THAT(comp.modify_flow(2.0, 1.0, erGapFill), WithinRel(1.0, 1e-6));
            CHECK_THAT(comp.modify_flow(2.0, 1.0, erIroning), WithinRel(1.0, 1e-6));
        }
    }
}

SCENARIO("SmallAreaInfillFlowCompensator scales the extrusion amount by the factor", "[SAIFC]") {
    GIVEN("a valid model (0,0.4) (2,0.7) (5,1.0)") {
        GCodeConfig config;
        init_config(config, { "0,0.4", "2,0.7", "5,1.0" });
        SmallAreaInfillFlowCompensator comp(config);

        THEN("the input extrusion dE is multiplied by the compensation factor") {
            REQUIRE_THAT(comp.modify_flow(2.0, 2.0, erSolidInfill), WithinRel(1.4, 1e-6));
        }
    }
}

SCENARIO("SmallAreaInfillFlowCompensator rejects malformed models", "[SAIFC]") {
    THEN("a model whose first length is not 0 is rejected (flow unchanged)") {
        GCodeConfig config;
        init_config(config, { "1,0.4", "2,0.7", "5,1.0" });
        SmallAreaInfillFlowCompensator comp(config);
        REQUIRE_THAT(comp.modify_flow(2.0, 1.0, erSolidInfill), WithinRel(1.0, 1e-6));
    }

    THEN("a model whose second point has length 0 is rejected (only the first may be 0)") {
        GCodeConfig config;
        init_config(config, { "0,0.4", "0,0.7", "5,1.0" });
        SmallAreaInfillFlowCompensator comp(config);
        REQUIRE_THAT(comp.modify_flow(2.0, 1.0, erSolidInfill), WithinRel(1.0, 1e-6));
    }

    THEN("a model whose final factor is not 1.0 is rejected") {
        GCodeConfig config;
        init_config(config, { "0,0.4", "2,0.7", "5,0.9" });
        SmallAreaInfillFlowCompensator comp(config);
        REQUIRE_THAT(comp.modify_flow(2.0, 1.0, erSolidInfill), WithinRel(1.0, 1e-6));
    }

    THEN("a model with non-increasing lengths is rejected") {
        GCodeConfig config;
        init_config(config, { "0,0.4", "5,0.7", "2,1.0" });
        SmallAreaInfillFlowCompensator comp(config);
        REQUIRE_THAT(comp.modify_flow(2.0, 1.0, erSolidInfill), WithinRel(1.0, 1e-6));
    }

    THEN("a model with an unparseable length is rejected") {
        GCodeConfig config;
        init_config(config, { "0,0.4", "abc,0.7", "5,1.0" });
        SmallAreaInfillFlowCompensator comp(config);
        REQUIRE_THAT(comp.modify_flow(2.0, 1.0, erSolidInfill), WithinRel(1.0, 1e-6));
    }

    THEN("a model with an unparseable factor is rejected") {
        GCodeConfig config;
        init_config(config, { "0,0.4", "2,xyz", "5,1.0" });
        SmallAreaInfillFlowCompensator comp(config);
        REQUIRE_THAT(comp.modify_flow(2.0, 1.0, erSolidInfill), WithinRel(1.0, 1e-6));
    }
}
