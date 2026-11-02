// SceneRefreshCoordinator 单测（阶段 G4）：无 wx 应用态，静态纯函数直调（零服务构造、零 wx——手册 §6 用例 A/B）。
// 覆盖：A update 旗标判定——plan_update_flags 矩阵（FFF/SLA × FORCE_BG/FORCE_FULL/POSTPONE）；
//       B 门控与动作分派——plan_update_gate 三态（need∧conside→call+clear / ¬conside→call 不清 / ¬need∧conside→skip）
//         + plan_changed_object_action 两态（SLA→restart_bg / FFF→reload_scene）。
// 无头限制（同 F1/G1/G2/G3 先例）：八张 Host 槽编排、update() 全链路、delayed 延迟量时序由手测 §6 六条兜底。
// Windows/asio 前置块置于一切 include 之前（G2 勘误 10：hpp 链 PrintConfig.hpp 先引入 windows.h）。
// TEST_CASE 名 ASCII（E9 教训）。
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
    // ① FFF + 0 → 不强制后台重启；两旗标位不传递
    {
        const auto plan = Coord::plan_update_flags(ptFFF, 0);
        REQUIRE(plan.force_bg_restart == false);
        REQUIRE(plan.full_refresh == false);
        REQUIRE(plan.postpone_errors == false);
    }
    // ② SLA + 0 → 强制后台重启（原 :1968 tech==ptSLA 短路）
    {
        const auto plan = Coord::plan_update_flags(ptSLA, 0);
        REQUIRE(plan.force_bg_restart == true);
        REQUIRE(plan.full_refresh == false);
        REQUIRE(plan.postpone_errors == false);
    }
    // ③ FFF + FORCE_BACKGROUND_PROCESSING_UPDATE → 强制后台重启（旗标置位路径）
    {
        const auto plan = Coord::plan_update_flags(ptFFF, (unsigned int)UpdateParams::FORCE_BACKGROUND_PROCESSING_UPDATE);
        REQUIRE(plan.force_bg_restart == true);
    }
    // ④ SLA + FORCE_BG → 仍强制（短路 ∨ 旗标，不翻转）
    {
        const auto plan = Coord::plan_update_flags(ptSLA, (unsigned int)UpdateParams::FORCE_BACKGROUND_PROCESSING_UPDATE);
        REQUIRE(plan.force_bg_restart == true);
    }
    // ⑤ FORCE_FULL_SCREEN_REFRESH 独立传递（FFF，不牵动重启位）
    {
        const auto plan = Coord::plan_update_flags(ptFFF, (unsigned int)UpdateParams::FORCE_FULL_SCREEN_REFRESH);
        REQUIRE(plan.force_bg_restart == false);
        REQUIRE(plan.full_refresh == true);
    }
    // ⑥ POSTPONE_VALIDATION_ERROR_MESSAGE 独立位（FFF+全旗标：三位互不干扰）
    {
        const unsigned all = (unsigned int)UpdateParams::FORCE_FULL_SCREEN_REFRESH
            | (unsigned int)UpdateParams::FORCE_BACKGROUND_PROCESSING_UPDATE
            | (unsigned int)UpdateParams::POSTPONE_VALIDATION_ERROR_MESSAGE;
        const auto plan = Coord::plan_update_flags(ptFFF, all);
        REQUIRE(plan.force_bg_restart == true);
        REQUIRE(plan.full_refresh == true);
        REQUIRE(plan.postpone_errors == true);
    }
    // ⑦ SLA + POSTPONE → 重启置位 + postpone 传递（后台重启抑制校验弹窗路径，原 :1972）
    {
        const auto plan = Coord::plan_update_flags(ptSLA, (unsigned int)UpdateParams::POSTPONE_VALIDATION_ERROR_MESSAGE);
        REQUIRE(plan.force_bg_restart == true);
        REQUIRE(plan.postpone_errors == true);
        REQUIRE(plan.full_refresh == false);
    }
}

TEST_CASE("plan_update_gate three states and plan_changed_object_action two states", "[SceneRefreshCoordinator]")
{
    // —— plan_update_gate 三态（原 Plater::update facade :4014-4025 门控语义）——
    // ① need ∧ conside → 调用并清旗标
    {
        const auto gate = Coord::plan_update_gate(true, true);
        REQUIRE(gate.call == true);
        REQUIRE(gate.clear_need_update == true);
    }
    // ② ¬conside → 调用但不清旗标（else 分支无条件调用）
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
    // ③ ¬need ∧ conside → 跳过（不调用、不清旗标）
    {
        const auto gate = Coord::plan_update_gate(false, true);
        REQUIRE(gate.call == false);
        REQUIRE(gate.clear_need_update == false);
    }

    // —— plan_changed_object_action 两态（原 changed_object :5175 分支）——
    // SLA → 走后台重启（update_restart_background_process）
    {
        const auto action = Coord::plan_changed_object_action(ptSLA);
        REQUIRE(action.restart_bg == true);
        REQUIRE(action.reload_scene == false);
    }
    // FFF → 走视图重载（view3D reload_scene）
    {
        const auto action = Coord::plan_changed_object_action(ptFFF);
        REQUIRE(action.restart_bg == false);
        REQUIRE(action.reload_scene == true);
    }
}
