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
    {
        const auto plan = Layout::plan_sidebar_visibility(make_state(true, false, true), true, false);
        REQUIRE(plan.should_show == true);
        REQUIRE(plan.change == false);
    }
    {
        const auto plan = Layout::plan_sidebar_visibility(make_state(true, true, true), true, false);
        REQUIRE(plan.should_show == false);
        REQUIRE(plan.change == true);
    }
    {
        const auto plan = Layout::plan_sidebar_visibility(make_state(false, false, true), true, false);
        REQUIRE(plan.should_show == false);
        REQUIRE(plan.change == true);
    }
    {
        const auto plan = Layout::plan_sidebar_visibility(make_state(false, false, true), false, false);
        REQUIRE(plan.change == false);
    }
    {
        const auto plan = Layout::plan_sidebar_visibility(make_state(true, true, true), true, true);
        REQUIRE(plan.should_show == false);
        REQUIRE(plan.change == true);
    }
    {
        const auto plan = Layout::plan_sidebar_visibility(make_state(true, false, false), true, true);
        REQUIRE(plan.should_show == false);
        REQUIRE(plan.change == true);
    }
    {
        const auto plan = Layout::plan_sidebar_visibility(make_state(true, false, true), true, true);
        REQUIRE(plan.should_show == true);
        REQUIRE(plan.change == false);
    }
}

TEST_CASE("docking_state_from three-state mapping", "[LayoutManager]")
{
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
