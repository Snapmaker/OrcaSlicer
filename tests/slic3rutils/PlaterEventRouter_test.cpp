// PlaterEventRouter 单测（阶段 G5）：无 wx 应用态，静态纯函数直调（零服务构造、零 wx——手册 §6 用例 A/B）。
// 覆盖：A 右键菜单分派核——plan_right_click_menu_kind 矩阵（无对象/空点/多选空/对象 × assemble/SLA/
//       full_instance/part/text/svg 分支）；
//       B tab 切换判定核——plan_tab_selection（子控件 veto / sidebar 显隐 / Monitor 无插件 veto+hint）。
// 无头限制（同 F1…G4 先例）：138 处 Bind 实际挂载、事件到达顺序由手测 §6 十组兜底。
// Windows/asio 前置块置于一切 include 之前（G2 勘误 10）。TEST_CASE 名 ASCII（E9 教训）。
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

#include "slic3r/GUI/app/PlaterEventRouter.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using Router = app::PlaterEventRouter;
using MenuKind = Router::RightClickMenuKind;

TEST_CASE("plan_right_click_menu_kind dispatch matrix", "[PlaterEventRouter]")
{
    // ① 无对象 + 空点 + 选中集空 → default 菜单
    REQUIRE(Router::plan_right_click_menu_kind(true, true, true, false, false, false, false, false, false) == MenuKind::Default);
    // ② 无对象 + 空点 + 选中集非空 → 不弹（None）
    REQUIRE(Router::plan_right_click_menu_kind(true, true, false, false, false, false, false, false, false) == MenuKind::None);
    // ③ 无对象 + 非空点 → assemble 面板分支
    REQUIRE(Router::plan_right_click_menu_kind(true, false, true, true, false, false, false, false, false) == MenuKind::AssembleMultiSelection);
    REQUIRE(Router::plan_right_click_menu_kind(true, false, true, false, false, false, false, false, false) == MenuKind::MultiSelection);
    // ④ 有对象 + 空点 → 不弹
    REQUIRE(Router::plan_right_click_menu_kind(false, true, false, false, false, false, false, false, false) == MenuKind::None);
    // ⑤ 有对象 + SLA → sla_object_menu（面板无关）
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, false, true, false, false, false, false) == MenuKind::SlaObjectMenu);
    // ⑥ 有对象 + FFF + assemble 面板：full_instance / part / 多选三分支
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, true, false, true, false, false, false) == MenuKind::AssembleObjectMenu);
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, true, false, false, true, false, false) == MenuKind::AssemblePartMenu);
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, true, false, false, false, false, false) == MenuKind::AssembleMultiSelection);
    // ⑦ 有对象 + FFF + 3D 面板：full_instance → object；part 按 volume 文本/svg 三分支；多选 → multi
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, false, false, true, false, false, false) == MenuKind::ObjectMenu);
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, false, false, false, true, true, false) == MenuKind::TextPartMenu);
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, false, false, false, true, false, true) == MenuKind::SvgPartMenu);
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, false, false, false, true, false, false) == MenuKind::PartMenu);
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, false, false, false, false, false, false) == MenuKind::MultiSelection);
}

TEST_CASE("plan_tab_selection veto sidebar and monitor guard", "[PlaterEventRouter]")
{
    const int tp3D = 0, tpPreview = 1, tpOther = 2;
    // ① 子控件事件 → veto_skip（调用方 e.Skip()+return，不显隐）
    {
        const auto plan = Router::plan_tab_selection(false, tp3D, tp3D, tpPreview, false, false);
        REQUIRE(plan.veto_skip == true);
        REQUIRE(plan.sidebar_show == false);
        REQUIRE(plan.install_hint == false);
    }
    // ② tabpanel 事件 → 不 veto；3D/Preview 显 sidebar，其他页隐藏
    {
        const auto plan = Router::plan_tab_selection(true, tp3D, tp3D, tpPreview, false, false);
        REQUIRE(plan.veto_skip == false);
        REQUIRE(plan.sidebar_show == true);
    }
    {
        const auto plan = Router::plan_tab_selection(true, tpPreview, tp3D, tpPreview, false, false);
        REQUIRE(plan.sidebar_show == true);
    }
    {
        const auto plan = Router::plan_tab_selection(true, tpOther, tp3D, tpPreview, false, false);
        REQUIRE(plan.sidebar_show == false);
    }
    // ③ Monitor 守卫激活 + 无 agent → veto + 弹安装提示
    {
        const auto plan = Router::plan_tab_selection(true, tpOther, tp3D, tpPreview, true, false);
        REQUIRE(plan.veto_skip == true);
        REQUIRE(plan.install_hint == true);
    }
    // ④ Monitor 守卫激活 + 有 agent → 不 veto（走设备页刷新路径）
    {
        const auto plan = Router::plan_tab_selection(true, tpOther, tp3D, tpPreview, true, true);
        REQUIRE(plan.veto_skip == false);
        REQUIRE(plan.install_hint == false);
    }
}
