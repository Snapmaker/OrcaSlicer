#include <catch2/catch_test_macros.hpp>

#include "libslic3r/LocalZOrderOptimizer.hpp"
#include "libslic3r/LocalZ/LocalZSchedule.hpp"
#include "libslic3r/LocalZ/LocalZMaskPreparation.hpp"
#include "libslic3r/ClipperUtils.hpp"

using namespace Slic3r;

TEST_CASE("Local-Z schedule completes lower ready planes before reusing a higher tool", "[LocalZ][Schedule]")
{
    const std::vector<LocalZSchedulePass> passes{{0, 1, 0.20, 1, 1, {0}},
                                                 {1, 0, 0.10, 1, 0, {2}},
                                                 {0, 0, 0.08, 1, 0, {1}},
                                                 {1, 1, 0.20, 1, 1, {0}}};
    const auto                            schedule = build_local_z_schedule(passes, 0);
    REQUIRE(schedule.phases.size() == 3);
    CHECK(schedule.phases[0].extruder_id == 1);
    CHECK(schedule.phases[0].passes == std::vector<size_t>{2});
    CHECK(schedule.phases[1].extruder_id == 2);
    CHECK(schedule.phases[1].passes == std::vector<size_t>{1});
    CHECK(schedule.phases[2].extruder_id == 0);
    CHECK(schedule.phases[2].passes == std::vector<size_t>{0, 3});
    CHECK(schedule.final_extruder == 0);
    CHECK_FALSE(schedule.used_dependency_fallback);
}

TEST_CASE("Local-Z dependencies wait for every tool in a pass", "[LocalZ][Schedule]")
{
    const auto schedule = build_local_z_schedule({{0, 0, 0.1, 1, 0, {0, 1}}, {0, 1, 0.1, 1, 1, {0}}}, 0);
    REQUIRE(schedule.phases.size() == 3);
    CHECK(schedule.phases[0].passes == std::vector<size_t>{0});
    CHECK(schedule.phases[1].extruder_id == 1);
    CHECK(schedule.phases[1].passes == std::vector<size_t>{0});
    CHECK(schedule.phases[2].passes == std::vector<size_t>{1});
}

TEST_CASE("Local-Z height ordering retains active and next-plane tools", "[LocalZ][Schedule]")
{
    const auto schedule = build_local_z_schedule({{0, 2, 0.2, 0, 0, {1}}, {0, 0, 0.1, 0, 0, {0, 1, 2}}}, 0);
    REQUIRE(schedule.phases.size() == 4);
    CHECK(schedule.phases[0].extruder_id == 0);
    CHECK(schedule.phases[1].extruder_id == 2);
    CHECK(schedule.phases[2].extruder_id == 1);
    CHECK(schedule.phases[3].extruder_id == 1);
    CHECK(schedule.phases[3].passes == std::vector<size_t>{0});
    const auto empty = build_local_z_schedule({{0, 0, 0.1, 1, 0, {}}}, 2);
    CHECK(empty.phases.empty());
    CHECK(empty.final_extruder == 2);
}

TEST_CASE("Local-Z prepared masks preserve fixed paint and guard mixed boundaries", "[LocalZ][Masks]")
{
    const auto rectangle = [](double x0, double x1) {
        return ExPolygons{
            ExPolygon(Polygon({Point::new_scale(x0, 0), Point::new_scale(x1, 0), Point::new_scale(x1, 2), Point::new_scale(x0, 2)}))};
    };
    SubLayerPlan plan;
    plan.painted_masks_by_extruder       = {rectangle(0, 2), {}};
    plan.fixed_painted_masks_by_extruder = {{}, rectangle(1, 2)};
    const auto prepared                  = prepare_local_z_pass_masks(plan);
    REQUIRE(prepared.by_extruder.size() == 2);
    CHECK(prepared.by_extruder[1] == plan.fixed_painted_masks_by_extruder[1]);
    CHECK(intersection_ex(prepared.by_extruder[0], prepared.by_extruder[1]).empty());
    CHECK(!intersection_ex(prepared.by_extruder[0], rectangle(-0.05, 0)).empty());
    CHECK(prepared.raw_polygon_count == 2);
    CHECK(plan.painted_masks_by_extruder[0] == rectangle(0, 2));
}

TEST_CASE("Local-Z bucket ordering starts with the active extruder when possible", "[LocalZ][GCode]")
{
    const std::vector<unsigned int> ordered =
        LocalZOrderOptimizer::order_bucket_extruders({1, 2}, 2, 1);

    REQUIRE(ordered == std::vector<unsigned int>{2, 1});
}

TEST_CASE("Local-Z pass groups keep a matching active extruder bucket first", "[LocalZ][GCode]")
{
    const std::vector<size_t> ordered =
        LocalZOrderOptimizer::order_pass_group({{1}, {2}}, 2);

    REQUIRE(ordered == std::vector<size_t>{1, 0});
}

TEST_CASE("Local-Z pass groups keep original order when no bucket matches the active extruder", "[LocalZ][GCode]")
{
    const std::vector<size_t> ordered =
        LocalZOrderOptimizer::order_pass_group({{1}, {2}, {3}}, 0);

    REQUIRE(ordered == std::vector<size_t>{0, 1, 2});
}
