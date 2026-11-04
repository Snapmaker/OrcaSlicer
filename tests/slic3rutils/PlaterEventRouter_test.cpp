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
    REQUIRE(Router::plan_right_click_menu_kind(true, true, true, false, false, false, false, false, false) == MenuKind::Default);
    REQUIRE(Router::plan_right_click_menu_kind(true, true, false, false, false, false, false, false, false) == MenuKind::None);
    REQUIRE(Router::plan_right_click_menu_kind(true, false, true, true, false, false, false, false, false) == MenuKind::AssembleMultiSelection);
    REQUIRE(Router::plan_right_click_menu_kind(true, false, true, false, false, false, false, false, false) == MenuKind::MultiSelection);
    REQUIRE(Router::plan_right_click_menu_kind(false, true, false, false, false, false, false, false, false) == MenuKind::None);
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, false, true, false, false, false, false) == MenuKind::SlaObjectMenu);
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, true, false, true, false, false, false) == MenuKind::AssembleObjectMenu);
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, true, false, false, true, false, false) == MenuKind::AssemblePartMenu);
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, true, false, false, false, false, false) == MenuKind::AssembleMultiSelection);
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, false, false, true, false, false, false) == MenuKind::ObjectMenu);
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, false, false, false, true, true, false) == MenuKind::TextPartMenu);
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, false, false, false, true, false, true) == MenuKind::SvgPartMenu);
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, false, false, false, true, false, false) == MenuKind::PartMenu);
    REQUIRE(Router::plan_right_click_menu_kind(false, false, false, false, false, false, false, false, false) == MenuKind::MultiSelection);
}

TEST_CASE("plan_tab_selection veto sidebar and monitor guard", "[PlaterEventRouter]")
{
    const int tp3D = 0, tpPreview = 1, tpOther = 2;
    {
        const auto plan = Router::plan_tab_selection(false, tp3D, tp3D, tpPreview, false, false);
        REQUIRE(plan.veto_skip == true);
        REQUIRE(plan.sidebar_show == false);
        REQUIRE(plan.install_hint == false);
    }
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
    {
        const auto plan = Router::plan_tab_selection(true, tpOther, tp3D, tpPreview, true, false);
        REQUIRE(plan.veto_skip == true);
        REQUIRE(plan.install_hint == true);
    }
    {
        const auto plan = Router::plan_tab_selection(true, tpOther, tp3D, tpPreview, true, true);
        REQUIRE(plan.veto_skip == false);
        REQUIRE(plan.install_hint == false);
    }
}
