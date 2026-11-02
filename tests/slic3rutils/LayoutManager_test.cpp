// LayoutManager 单测（阶段 G3）：无 wx 应用态，静态纯函数直调（零服务构造、零 wx——手册 §6 用例 A/B）。
// 覆盖：A 布局状态机——plan_sidebar_visibility 六态矩阵 + docking_state_from 三态映射；
//       B 面板切换判定——pick_canvas_kind（含 exclude_preview/fallback 分支）+ kind_from_view_name 四态。
// 无头限制（同 F1/G1/G2 先例）：AUI pane 显隐全链路、视图切换编排、相机同步由手测 §6 九条兜底。
// Windows/asio 前置块置于一切 include 之前（G2 勘误 10：LayoutManager.hpp 链 wx/panel.h 先引入 windows.h）。
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

#include "slic3r/GUI/app/LayoutManager.hpp"

#include <string>

using namespace Slic3r;
using namespace Slic3r::GUI;
using Layout = app::LayoutManager;
using SidebarLayout = Layout::SidebarLayout;
using PanelKind = Layout::PanelKind;

namespace {

SidebarLayout make_state(bool is_enabled, bool is_collapsed, bool show)
{
    SidebarLayout st;
    st.is_enabled  = is_enabled;
    st.is_collapsed = is_collapsed;
    st.show        = show;
    return st;
}

} // namespace

TEST_CASE("plan_sidebar_visibility state machine matrix", "[LayoutManager]")
{
    // ① enabled + 展开 + pane 显示（停靠）→ 保持显示，零变更
    {
        const auto plan = Layout::plan_sidebar_visibility(make_state(true, false, true), true, false);
        REQUIRE(plan.should_show == true);
        REQUIRE(plan.change == false);
    }
    // ② enabled + 折叠 + pane 显示 → 收起（needs_update）
    {
        const auto plan = Layout::plan_sidebar_visibility(make_state(true, true, true), true, false);
        REQUIRE(plan.should_show == false);
        REQUIRE(plan.change == true);
    }
    // ③ disabled + pane 显示 → 隐藏（change=true 即"需隐藏"）
    {
        const auto plan = Layout::plan_sidebar_visibility(make_state(false, false, true), true, false);
        REQUIRE(plan.should_show == false);
        REQUIRE(plan.change == true);
    }
    // ④ disabled + pane 已隐 → 不动
    {
        const auto plan = Layout::plan_sidebar_visibility(make_state(false, false, true), false, false);
        REQUIRE(plan.change == false);
    }
    // ⑤ enabled + 折叠 + 浮动 + show 旗标真 → 仍收起（is_collapsed 优先，原 :2166 语义）
    {
        const auto plan = Layout::plan_sidebar_visibility(make_state(true, true, true), true, true);
        REQUIRE(plan.should_show == false);
        REQUIRE(plan.change == true);
    }
    // ⑥ enabled + 展开 + 浮动 + show 旗标假 → 隐藏（floating && !show，原 :2166 语义）
    {
        const auto plan = Layout::plan_sidebar_visibility(make_state(true, false, false), true, true);
        REQUIRE(plan.should_show == false);
        REQUIRE(plan.change == true);
    }
    // ⑦ enabled + 展开 + 浮动 + show 旗标真 → 显示
    {
        const auto plan = Layout::plan_sidebar_visibility(make_state(true, false, true), true, true);
        REQUIRE(plan.should_show == true);
        REQUIRE(plan.change == false);
    }
}

TEST_CASE("docking_state_from three-state mapping", "[LayoutManager]")
{
    // 禁用即 None（原 :2188）；浮动即 None（原 :2193）；右停靠 Right / 其余 Left（原 :2197）
    REQUIRE(Layout::docking_state_from(false, false, true)  == Sidebar::None);
    REQUIRE(Layout::docking_state_from(true,  true,  true)  == Sidebar::None);
    REQUIRE(Layout::docking_state_from(true,  false, true)  == Sidebar::Right);
    REQUIRE(Layout::docking_state_from(true,  false, false) == Sidebar::Left);
}

TEST_CASE("pick_canvas_kind exclude_preview/fallback branches", "[LayoutManager]")
{
    REQUIRE(Layout::pick_canvas_kind(PanelKind::View3D,   false) == PanelKind::View3D);
    REQUIRE(Layout::pick_canvas_kind(PanelKind::View3D,   true)  == PanelKind::View3D);
    REQUIRE(Layout::pick_canvas_kind(PanelKind::Preview,  false) == PanelKind::Preview);
    REQUIRE(Layout::pick_canvas_kind(PanelKind::Preview,  true)  == PanelKind::View3D);   // exclude_preview → 回退 view3D（原 :3860）
    REQUIRE(Layout::pick_canvas_kind(PanelKind::Assemble, false) == PanelKind::Assemble);
    REQUIRE(Layout::pick_canvas_kind(PanelKind::Assemble, true)  == PanelKind::Assemble); // assemble 不受 exclude 影响（原 :3864）
    REQUIRE(Layout::pick_canvas_kind(PanelKind::None,     false) == PanelKind::View3D);   // fallback（原 :3869）
}

TEST_CASE("kind_from_view_name four-kind mapping", "[LayoutManager]")
{
    REQUIRE(Layout::kind_from_view_name("3D")       == PanelKind::View3D);
    REQUIRE(Layout::kind_from_view_name("Preview")  == PanelKind::Preview);
    REQUIRE(Layout::kind_from_view_name("Assemble") == PanelKind::Assemble);
    REQUIRE(Layout::kind_from_view_name("Unknown")  == PanelKind::None);
}
