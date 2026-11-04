#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winnls.h>              // CP_ACP
#endif

#include "slic3r/GUI/app/SceneRefreshCoordinator.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using Coord = app::SceneRefreshCoordinator;
using UpdateParams = Coord::UpdateParams;

TEST_CASE("plan_update_flags flag matrix across technologies", "[SceneRefreshCoordinator]")
{
    {
        const auto plan = Coord::plan_update_flags(ptFFF, 0);
        REQUIRE(plan.force_bg_restart == false);
        REQUIRE(plan.full_refresh == false);
        REQUIRE(plan.postpone_errors == false);
    }
    {
        const auto plan = Coord::plan_update_flags(ptSLA, 0);
        REQUIRE(plan.force_bg_restart == true);
        REQUIRE(plan.full_refresh == false);
        REQUIRE(plan.postpone_errors == false);
    }
    {
        const auto plan = Coord::plan_update_flags(ptFFF, (unsigned int)UpdateParams::FORCE_BACKGROUND_PROCESSING_UPDATE);
        REQUIRE(plan.force_bg_restart == true);
    }
    {
        const auto plan = Coord::plan_update_flags(ptSLA, (unsigned int)UpdateParams::FORCE_BACKGROUND_PROCESSING_UPDATE);
        REQUIRE(plan.force_bg_restart == true);
    }
    {
        const auto plan = Coord::plan_update_flags(ptFFF, (unsigned int)UpdateParams::FORCE_FULL_SCREEN_REFRESH);
        REQUIRE(plan.force_bg_restart == false);
        REQUIRE(plan.full_refresh == true);
    }
    {
        const unsigned all = (unsigned int)UpdateParams::FORCE_FULL_SCREEN_REFRESH
            | (unsigned int)UpdateParams::FORCE_BACKGROUND_PROCESSING_UPDATE
            | (unsigned int)UpdateParams::POSTPONE_VALIDATION_ERROR_MESSAGE;
        const auto plan = Coord::plan_update_flags(ptFFF, all);
        REQUIRE(plan.force_bg_restart == true);
        REQUIRE(plan.full_refresh == true);
        REQUIRE(plan.postpone_errors == true);
    }
    {
        const auto plan = Coord::plan_update_flags(ptSLA, (unsigned int)UpdateParams::POSTPONE_VALIDATION_ERROR_MESSAGE);
        REQUIRE(plan.force_bg_restart == true);
        REQUIRE(plan.postpone_errors == true);
        REQUIRE(plan.full_refresh == false);
    }
}

TEST_CASE("plan_update_gate three states and plan_changed_object_action two states", "[SceneRefreshCoordinator]")
{
    {
        const auto gate = Coord::plan_update_gate(true, true);
        REQUIRE(gate.call == true);
        REQUIRE(gate.clear_need_update == true);
    }
    {
        const auto gate = Coord::plan_update_gate(true, false);
        REQUIRE(gate.call == true);
        REQUIRE(gate.clear_need_update == false);
    }
    {
        const auto gate = Coord::plan_update_gate(false, false);
        REQUIRE(gate.call == true);
        REQUIRE(gate.clear_need_update == false);
    }
    {
        const auto gate = Coord::plan_update_gate(false, true);
        REQUIRE(gate.call == false);
        REQUIRE(gate.clear_need_update == false);
    }

    {
        const auto action = Coord::plan_changed_object_action(ptSLA);
        REQUIRE(action.restart_bg == true);
        REQUIRE(action.reload_scene == false);
    }
    {
        const auto action = Coord::plan_changed_object_action(ptFFF);
        REQUIRE(action.restart_bg == false);
        REQUIRE(action.reload_scene == true);
    }
}
