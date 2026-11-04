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

#include "LayoutManager.hpp"
#include "../Plater.hpp"        // H1 2.2: F3 q bridges + reslice/update_restart public faces\n
#include "../MainFrame.hpp"      // H1 2.2: typed inject MainFrame* (select_tab)
#include "../FilamentGroupDialog.hpp"  // 阶段 I：页面切换自动切片前的耗材分组确认
#include "../FlowTypeHelper.hpp"       // 阶段 I：高流量判定与体积类型同步
#include "ProjectSession.hpp"    // H1 2.2: typed inject ProjectSession& (session predicates)\n
#include "SlicingController.hpp" // H1 2.2: typed inject SlicingController& (slicing coupling)\n
#include "SceneRefreshCoordinator.hpp"  // H1 2.2: late_bind refresh\n

#include <algorithm>
#include <utility>

#include <wx/aui/aui.h>          // wxAuiManager/wxAuiFloatingFrame/wxAUI_DOCK_RIGHT（原 Plater.cpp:82）

#include "../GUI_Preview.hpp"       // View3D/Preview/AssembleView 完整类型（同头）+ GLCanvas3D
#include "../MainFrame.hpp"         // MainFrame::tp3DEditor/tpPreview（select_tab 槽实参，G2 cpp-only 先例）
#include "../Selection.hpp"         // 跨视图选中迁移（set_current_panel）
#include "../Gizmos/GLGizmosManager.hpp"
#include "../Widgets/StateColor.hpp"// apply_color_mode darkModeColorFor
#include "../I18N.hpp"              // _u8L（collapse_sidebar tooltip）
#include "NotificationHub.hpp"      // m_hub：is_dark()/nm()->set_sidebar_collapsed/set_in_preview/invalid_all_plate_thumbnails
#include "../NotificationManager.hpp"  // nm() 解引用完整类型（set_in_preview/set_sidebar_collapsed）
#include "../GUI_ObjectList.hpp"    // obj_list 槽 ->update_selections
#include "../PartPlate.hpp"         // partplate_list 槽：get_curr_plate/select_plate/get_current_fff_print
#include "libslic3r/AppConfig.hpp"   // app_config 槽：get/get_bool/set
#include "libslic3r/PresetBundle.hpp"// preset_bundle 槽：full_config/filament_presets

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/Model.hpp"          // Model::setExtruderParams/setPrintSpeedTable（select_view_3D）

namespace Slic3r {
namespace GUI {

class FloatFrame : public wxAuiFloatingFrame
{
public:
    FloatFrame(wxWindow* parent, wxAuiManager* ownerMgr, const wxAuiPaneInfo& pane, std::function<void(wxFrame*)> apply_dark_ui)
        : wxAuiFloatingFrame(parent, ownerMgr, pane)
    {
        if (apply_dark_ui)
            apply_dark_ui(this);          // 原 GUI_App 全局 UpdateFrameDarkUI(this)（宿主槽等价改写）
    }
};

class AuiMgr : public wxAuiManager
{
public:
    std::function<void(wxFrame*)> apply_dark_ui;     // LayoutManager ctor 装配（FloatFrame 暗色 UI 原行为；UpdateFrameDarkUI 签名 wxFrame*）

    AuiMgr() : wxAuiManager(){}

    virtual wxAuiFloatingFrame* CreateFloatingFrame(wxWindow* parent, const wxAuiPaneInfo& p) override
    {
        return new FloatFrame(parent, this, p, apply_dark_ui);
    }
};

namespace app {

LayoutManager::LayoutManager(Plater* q, NotificationHub& hub, ProjectSession& session, SlicingController& slicing,
                             PartPlateList& partplate_list, Model& model, MainFrame* main_frame)
    : sidebar(new Sidebar(q))                        // 原 priv init list（Plater.cpp:984）逐字承接
    , collapse_toolbar(GLToolbar::Normal, "Collapse")// 原 priv init list（:986）逐字承接
    , m_q(q)
    , m_hub(hub)
    , m_aui_mgr(new AuiMgr())
    , m_session(session)
    , m_slicing(slicing)
    , m_partplate_list(partplate_list)
    , m_model(model)
    , m_main_frame(main_frame)
{
    m_aui_mgr->apply_dark_ui = [this](wxFrame* w) {    // 派生指针；签名随 update_frame_dark_ui 槽（UpdateFrameDarkUI 收 wxFrame*）
        m_q->update_frame_dark_ui(w);
    };
}

void LayoutManager::late_bind(SceneRefreshCoordinator& refresh)
{
    m_refresh = &refresh;
}

LayoutManager::~LayoutManager() = default;

bool LayoutManager::is_preview_shown() const { return current_panel == preview; }
bool LayoutManager::is_preview_loaded() const { return preview->is_loaded(); }
bool LayoutManager::is_view3D_shown() const { return current_panel == view3D; }
bool LayoutManager::is_assemble_view_show() const { return current_panel == assemble_view; }

bool LayoutManager::are_view3D_labels_shown() const { return (current_panel == view3D) && view3D->get_canvas3d()->are_labels_shown(); }
void LayoutManager::show_view3D_labels(bool show) { if (current_panel == view3D) view3D->get_canvas3d()->show_labels(show); }

bool LayoutManager::is_view3D_overhang_shown() const { return (current_panel == view3D) && view3D->get_canvas3d()->is_overhang_shown(); }
void LayoutManager::show_view3D_overhang(bool show)
{
    if (current_panel == view3D) view3D->get_canvas3d()->show_overhang(show);
}

bool LayoutManager::is_view3D_layers_editing_enabled() const { return (current_panel == view3D) && view3D->get_canvas3d()->is_layers_editing_enabled(); }

wxAuiManager* LayoutManager::aui() { return m_aui_mgr.get(); }   // 基类指针（AuiMgr 完整类型在本 cpp）


LayoutManager::SidebarVisPlan LayoutManager::plan_sidebar_visibility(const SidebarLayout& st, bool pane_shown, bool pane_floating)
{
    if (!st.is_enabled)
        return {false, pane_shown};                  // 禁用态：显示中即需隐藏（原 update_sidebar disabled 分支）
    const bool should_hide = st.is_collapsed || (pane_floating && !st.show);
    const bool should_show = !should_hide;
    return {should_show, should_show != pane_shown};
}

Sidebar::DockingState LayoutManager::docking_state_from(bool is_enabled, bool is_floating, bool docked_right)
{
    if (!is_enabled)
        return Sidebar::None;
    if (is_floating)
        return Sidebar::None;
    return docked_right ? Sidebar::Right : Sidebar::Left;
}

LayoutManager::PanelKind LayoutManager::pick_canvas_kind(PanelKind current, bool exclude_preview)
{
    if (current == PanelKind::View3D)
        return PanelKind::View3D;
    if (!exclude_preview && current == PanelKind::Preview)
        return PanelKind::Preview;
    if (current == PanelKind::Assemble)
        return PanelKind::Assemble;
    return PanelKind::View3D;                        // 原 :3869 fallback "BBS default set to view3D"
}

LayoutManager::PanelKind LayoutManager::kind_from_view_name(const std::string& name)
{
    if (name == "3D")
        return PanelKind::View3D;
    if (name == "Preview")
        return PanelKind::Preview;
    if (name == "Assemble")
        return PanelKind::Assemble;
    return PanelKind::None;
}

LayoutManager::PanelKind LayoutManager::kind_of(const wxPanel* panel) const
{
    if (panel == view3D)
        return PanelKind::View3D;
    if (panel == preview)
        return PanelKind::Preview;
    if (panel == assemble_view)
        return PanelKind::Assemble;
    return PanelKind::None;
}


void LayoutManager::attach_views(View3D* v3d, Preview* prev, AssembleView* assemble)
{
    view3D = v3d;
    preview = prev;
    assemble_view = assemble;
    panels.push_back(view3D);                        // 原 :1566-1568 逐字
    panels.push_back(preview);
    panels.push_back(assemble_view);
}

void LayoutManager::save_window_layout()             // 原 reset() 尾 :2479-2488 逐字（app_config 改槽）
{
    // Save window layout
    if (sidebar_layout.is_enabled) {
        // Reset show state
        auto& sidebar_pane = m_aui_mgr->GetPane(sidebar);
        if (!sidebar_layout.is_collapsed && !sidebar_pane.IsShown()) {
            sidebar_pane.Show();
        }
        auto layout = m_aui_mgr->SavePerspective();
        m_q->app_config()->set("window_layout", layout.utf8_string());
    }
}


void LayoutManager::select_view(const std::string& direction)   // 原 :2027-2043
{
    if (current_panel == view3D) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << "select view3D";
        view3D->select_view(direction);
        m_q->app_update_ui_from_settings();
    }
    else if (current_panel == preview) {
        BOOST_LOG_TRIVIAL(info) << "select preview";
        preview->select_view(direction);
        m_q->app_update_ui_from_settings();
    }
    else if (current_panel == assemble_view) {
        BOOST_LOG_TRIVIAL(info) << "select assemble view";
        assemble_view->select_view(direction);
    }
}

void LayoutManager::apply_free_camera_correction(bool apply/* = true*/)   // 原 :2066-2075（get_config → app_config 槽）
{
    bool use_perspective_camera = m_q->app_config()->get("use_perspective_camera").compare("true") == 0;
    if (use_perspective_camera)
        camera.set_type(Camera::EType::Perspective);
    else
        camera.set_type(Camera::EType::Ortho);
    if (apply && m_q->app_config()->get_bool("use_free_camera"))
        camera.recover_from_free_camera();
}

//BBS: add no slice option
void LayoutManager::select_view_3D(const std::string& name, bool no_slice)   // 原 :2078-2112
{
    const PanelKind kind = kind_from_view_name(name);   // 等价改写 ④：if-链名比较 → kind 判定
    if (kind == PanelKind::View3D) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << "select view3D";
        if (m_session.only_gcode_mode() || m_session.using_exported_file()) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("goto preview page when loading gcode/exported_3mf");
        }
        set_current_panel(view3D, no_slice);
        m_main_frame->select_tab(size_t(MainFrame::tp3DEditor));
    }
    else if (kind == PanelKind::Preview) {
        BOOST_LOG_TRIVIAL(info) << "select preview";
        //BBS update extruder params and speed table before slicing
        const Slic3r::DynamicPrintConfig& config = m_q->preset_bundle()->full_config();
        auto& print = m_partplate_list.get_current_fff_print();
        auto print_config = print.config();
        int numExtruders = m_q->preset_bundle()->filament_presets.size();

        Model::setExtruderParams(config, numExtruders);
        Model::setPrintSpeedTable(config, print_config);
        set_current_panel(preview, no_slice);
        m_main_frame->select_tab(size_t(MainFrame::tpPreview));
    }
    else if (kind == PanelKind::Assemble) {
        BOOST_LOG_TRIVIAL(info) << "select assemble view";
        set_current_panel(assemble_view, no_slice);
        m_main_frame->select_tab(size_t(MainFrame::tp3DEditor));
    }

    //BBS update selection
    sidebar->obj_list()->update_selections();
    m_refresh->selection_changed();

    apply_free_camera_correction(false);
}

void LayoutManager::select_next_view_3D()            // 原 :2114-2123
{
    if (current_panel == view3D)
        m_main_frame->select_tab(size_t(MainFrame::tpPreview));
    else if (current_panel == preview)
        m_main_frame->select_tab(size_t(MainFrame::tp3DEditor));
//    else if (current_panel == assemble_view)
//        set_current_panel(view3D);
}

GLCanvas3D* LayoutManager::get_current_canvas3D(bool exclude_preview)   // 原 :3852-3874（判定核抽 pick_canvas_kind）
{
    // During destruction, these pointers may be null or point to destroyed objects
    // Add null checks to prevent crashes during shutdown
    const PanelKind picked = pick_canvas_kind(kind_of(current_panel), exclude_preview);
    switch (picked) {
    case PanelKind::View3D:
        if (view3D)
            return view3D->get_canvas3d();
        break;
    case PanelKind::Preview:
        if (preview)
            return preview->get_canvas3d();
        break;
    case PanelKind::Assemble:
        if (assemble_view)
            return assemble_view->get_canvas3d();
        break;
    case PanelKind::None:
        break;
    }

    //BBS default set to view3D, but check if it's still valid
    if (view3D)
        return view3D->get_canvas3d();

    return nullptr;
}


void LayoutManager::enable_sidebar(bool enabled)     // 原 :2125-2132
{
    if (m_session.only_gcode_mode())
        enabled = false;

    sidebar_layout.is_enabled = enabled;
    update_sidebar();
}

void LayoutManager::collapse_sidebar(bool collapse)  // 原 :2134-2150
{
    if (m_session.only_gcode_mode())
        return;

    sidebar_layout.is_collapsed = collapse;

    // Now update the tooltip in the toolbar.
    std::string new_tooltip = collapse
                              ? _u8L("Expand sidebar")
                              : _u8L("Collapse sidebar");
    new_tooltip += " [" + _u8L("Shift+") + _u8L("Tab") + "]";
    int id = collapse_toolbar.get_item_id("collapse_sidebar");
    collapse_toolbar.set_tooltip(id, new_tooltip);

    update_sidebar();
}

void LayoutManager::update_sidebar(bool force_update)   // 原 :2152-2178（判定核抽 plan_sidebar_visibility）
{
    auto& sidebar_pane = m_aui_mgr->GetPane(sidebar);
    if (!sidebar_pane.IsOk() || this->current_panel == nullptr) {
        return;
    }
    bool needs_update = force_update;

    const SidebarVisPlan plan = plan_sidebar_visibility(sidebar_layout, sidebar_pane.IsShown(), sidebar_pane.IsFloating());
    if (!sidebar_layout.is_enabled) {
        if (plan.change) {
            sidebar_pane.Hide();
            needs_update = true;
        }
    } else {
        // Only hide if collapsed or is floating and is not 3d view
        if (plan.change) {
            sidebar_pane.Show(plan.should_show);
            needs_update = true;
        }
    }

    if (needs_update) {
        m_hub.nm()->set_sidebar_collapsed(sidebar_pane.IsShown());
        m_aui_mgr->Update();
    }
}

void LayoutManager::reset_window_layout()            // 原 :2180-2185
{
    m_aui_mgr->LoadPerspective(m_default_window_layout, false);
    sidebar_layout.is_collapsed = false;
    update_sidebar(true);
}

Sidebar::DockingState LayoutManager::get_sidebar_docking_state()   // 原 :2187-2198（判定核抽 docking_state_from）
{
    const auto& pane = m_aui_mgr->GetPane(sidebar);
    return docking_state_from(sidebar_layout.is_enabled, pane.IsFloating(), pane.dock_direction == wxAUI_DOCK_RIGHT);
}


void LayoutManager::set_current_panel(wxPanel* panel, bool no_slice)
{
    if (std::find(panels.begin(), panels.end(), panel) == panels.end())
        return;

    // Prepare and preview both render the by-object yellow warning; re-evaluate
    // it against the current plate on every page switch. Skip the initial call
    // from the priv constructor: Plater::p is not assigned until that ctor
    // returns, and the sync dereferences it.
    if (current_panel != nullptr)
        m_hub.sync_print_seq_warning_notification();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": current_panel %1%, new_panel %2%")%current_panel%panel;
#ifdef __WXMAC__
    bool force_render = (current_panel != nullptr);
#endif // __WXMAC__

    //BBS: add slice logic when switch to preview page
    auto do_reslice = [this, no_slice]() {
            // see: Plater::priv::object_list_changed()
            // FIXME: it may be better to have a single function making this check and let it be called wherever needed
            bool export_in_progress = m_slicing.background_process().is_export_scheduled();
            bool model_fits = this->view3D->get_canvas3d()->check_volumes_outside_state() != ModelInstancePVS_Partly_Outside;
            //BBS: add partplate logic
            PartPlate * current_plate = m_partplate_list.get_curr_plate();
            bool only_has_gcode_need_preview = false;
            bool current_has_print_instances = current_plate->has_printable_instances();
            if (current_plate->is_slice_result_valid() && m_model.objects.empty() && !current_has_print_instances)
                only_has_gcode_need_preview = true;
            bool slice_cancelled = false;

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": from set_current_panel, no_slice %1%, export_in_progress %2%, model_fits %3%, is_slicing %4%")%no_slice%export_in_progress%model_fits%m_slicing.is_slicing();

            if (!no_slice && !m_model.objects.empty() && !export_in_progress && model_fits && current_has_print_instances
                && !current_plate->is_slice_result_valid())
            {
                //if already running in background, not relice here
                //BBS: add more judge for slicing
                if (!m_slicing.background_process().running() && !m_slicing.is_slicing())
                {
                    m_slicing.set_slice_all(false);
                    // Page-switch auto-slice runs the same pre-slice guard as the
                    // slice button so blocking errors (filament temp mixing, cold
                    // plate) surface here too.
                    bool guard_passed = m_slicing.guard_before_slice_plate();
                    bool grouping_confirmed = true;
                    if (guard_passed && GUI::FlowType::any_nozzle_high_flow()) {
                        bool all_high_flow = GUI::FlowType::distinct_nozzle_flow_type_count() < 2;
                        GUI::FilamentGroupDialog dlg(m_q, all_high_flow);
                        grouping_confirmed = dlg.ShowModal() == wxID_OK;
                    } else if (guard_passed) {
                        GUI::FlowType::sync_filament_volume_types_for_slice();
                    }
                    if (guard_passed && grouping_confirmed)
                        slice_cancelled = !m_slicing.reslice();
                    else {
                        slice_cancelled = true;
                        if (wxGetApp().mainframe != nullptr)
                            wxGetApp().mainframe->update_slice_print_status(MainFrame::eEventSliceUpdate, true);
                    }
               }
                else {
                    //reset current plate to the slicing plate
                    int plate_index = m_slicing.background_process().get_current_plate()->get_index();
                    m_partplate_list.select_plate(plate_index);
                    m_hub.sync_print_seq_warning_notification();
                }
            }
            else if (only_has_gcode_need_preview)
            {
                m_slicing.set_slice_all(false);
                slice_cancelled = !m_q->reslice();
            }
            //BBS: process empty plate, reset previous toolpath
            else
            {
                //if (!m_slicing.slice_all())
                if (!current_has_print_instances)
                    preview->get_canvas3d()->reset_gcode_toolpaths();   // 原 priv::reset_gcode_toolpaths 内联展开（留守 G4）
                //this->q->refresh_print();
                if (!preview->get_canvas3d()->is_initialized())
                {
                    preview->get_canvas3d()->render(true);
                }
            }
            //TODO: turn off this switch currently
            /*auto canvas_w = float(preview->get_canvas3d()->get_canvas_size().get_width());
            auto canvas_h = float(preview->get_canvas3d()->get_canvas_size().get_height());
            Point screen_center(canvas_w/2, canvas_h/2);
            auto center_point = preview->get_canvas3d()->_mouse_to_3d(screen_center);
            center_point(2) = 0.f;
            if (!current_plate->contains(center_point))
                this->partplate_list.select_plate_view();*/

            // keeps current gcode preview, if any
            if (slice_cancelled) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": slicing cancelled by user, showing shells only";
                m_refresh->update_fff_scene_only_shells();
            }
            else if (m_slicing.slice_all()) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": slicing all, just reload shells");
                m_refresh->update_fff_scene_only_shells();
            }
            else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": single slice, reload print");
                if (model_fits)
                    this->preview->reload_print(true);
                else
                    m_refresh->update_fff_scene_only_shells();
            }

            preview->set_as_dirty();
        };

    // Add sidebar and toolbar collapse logic
    if (panel == view3D || panel == preview) {
        this->enable_sidebar(!m_session.only_gcode_mode());
    }
    if (panel == preview) {
        if (m_session.only_gcode_mode()) {
            preview->get_canvas3d()->enable_select_plate_toolbar(false);
        } else if (m_session.using_exported_file() && (m_session.valid_plates_count() <= 1)) {
            preview->get_canvas3d()->enable_select_plate_toolbar(false);
        } else {
            preview->get_canvas3d()->enable_select_plate_toolbar(true);
        }
    }
    else {
        preview->get_canvas3d()->enable_select_plate_toolbar(false);
    }

    if (current_panel == panel)
    {
        //BBS: add slice logic when switch to preview page
        //BBS: add only gcode mode
        if (!m_session.only_gcode_mode() && (current_panel == preview) && (m_q->is_editor())) {
            do_reslice();
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": the same panel, exit");
        return;
    }

    //BBS: wish to reset all plates stats item selected state when back to View3D Tab
    preview->get_canvas3d()->reset_select_plate_toolbar_selection();

    wxPanel* old_panel = current_panel;
//#if BBL_HAS_FIRST_PAGE
    if (!old_panel) {
        //BBS: only switch to the first panel when visible
        panel->Show();
        //dynamic_cast<View3D *>(panel)->get_canvas3d()->render();
        if (!panel->IsShownOnScreen())
            return;
    }
//#endif
    current_panel = panel;

    // to reduce flickering when changing view, first set as visible the new current panel
    for (wxPanel* p : panels) {
        if (p == current_panel) {
#ifdef __WXMAC__
            // On Mac we need also to force a render to avoid flickering when changing view
            if (force_render) {
                if (p == view3D)
                    dynamic_cast<View3D*>(p)->get_canvas3d()->render();
                else if (p == preview)
                    dynamic_cast<Preview*>(p)->get_canvas3d()->render();
            }
#endif // __WXMAC__
            p->Show();
        }
    }
    // then set to invisible the other
    for (wxPanel* p : panels) {
        if (p != current_panel)
            p->Hide();
    }

    update_sidebar(true);

    if (true) {
        Camera& cam = m_q->get_camera();               // 原 GUI_App 全局 plater()->get_camera()（单例同指，等价改写 ①）
        if (old_panel == preview || old_panel == view3D) {
            view3D->get_canvas3d()->get_camera().load_camera_view(cam);
        } else if (old_panel == assemble_view) {
            assemble_view->get_canvas3d()->get_camera().load_camera_view(cam);
        }
        if (current_panel == view3D || current_panel == preview) {
            cam.load_camera_view(view3D->get_canvas3d()->get_camera());
        }
        else if (current_panel == assemble_view) {
            cam.load_camera_view(assemble_view->get_canvas3d()->get_camera());
        }
    }

    if (current_panel == view3D) {
        if (old_panel == preview)
            preview->get_canvas3d()->unbind_event_handlers();
        else if (old_panel == assemble_view) {
            assemble_view->get_canvas3d()->unbind_event_handlers();

            GLCanvas3D* assemble_canvas = assemble_view->get_canvas3d();
            Selection::IndicesList select_idxs = assemble_canvas->get_selection().get_volume_idxs();
            Selection& view3d_selection = view3D->get_canvas3d()->get_selection();
            view3d_selection.clear();
            for (unsigned int idx : select_idxs) {
                auto v = assemble_canvas->get_selection().get_volume(idx);
                auto real_idx = view3d_selection.query_real_volume_idx_from_other_view(v->object_idx(), v->instance_idx(), v->volume_idx());
                if (real_idx >= 0) {
                    view3d_selection.add(real_idx, false);
                }
            }
        }

        view3D->get_canvas3d()->bind_event_handlers();

        if (view3D->is_reload_delayed()) {
            // Delayed loading of the 3D scene.
            if (m_session.printer_technology == ptSLA) {
                // Update the SLAPrint from the current Model, so that the reload_scene()
                // pulls the correct data.
                m_q->update_restart_background_process(true, false);
            } else
                view3D->reload_scene(true);
        }

        // sets the canvas as dirty to force a render at the 1st idle event (wxWidgets IsShownOnScreen() is buggy and cannot be used reliably)
        view3D->set_as_dirty();
        // reset cached size to force a resize on next call to render() to keep imgui in synch with canvas size
        view3D->get_canvas3d()->reset_old_size();
        // BBS
        //view_toolbar.select_item("3D");
        if (m_hub.nm() != nullptr)
            m_hub.nm()->set_in_preview(false);
    }
    else if (current_panel == preview) {
        m_hub.invalid_all_plate_thumbnails();          // 原 q->invalid_all_plate_thumbnails()（门面直通 hub，等价改写）
        if (old_panel == view3D)
            view3D->get_canvas3d()->unbind_event_handlers();
        else if (old_panel == assemble_view)
            assemble_view->get_canvas3d()->unbind_event_handlers();

        preview->get_canvas3d()->bind_event_handlers();

        GLGizmosManager& gizmos = view3D->get_canvas3d()->get_gizmos_manager();
        if (gizmos.is_running()) {
            gizmos.reset_all_states();
            gizmos.update_data();
        }

        if (m_q->is_editor()) {
            // see: Plater::priv::object_list_changed()
            // FIXME: it may be better to have a single function making this check and let it be called wherever needed
            /*bool export_in_progress = this->slicing().background_process().is_export_scheduled();
            bool model_fits = view3D->get_canvas3d()->check_volumes_outside_state() != ModelInstancePVS_Partly_Outside;
            //BBS: add partplate logic
            PartPlate* current_plate = partplate_list.get_curr_plate();
            if (!no_slice && !model.objects.empty() && !export_in_progress && model_fits && current_plate->has_printable_instances()) {
                preview->get_canvas3d()->init_gcode_viewer();
                // BBS
                //if already running in background, not relice here
                if (!this->slicing().background_process().running())
                {
                    slicing().set_slice_all(false);
                    this->q->reslice();
                }
            }
            // keeps current gcode preview, if any
            preview->reload_print(true);

            preview->set_as_dirty();*/
            if (m_q->is_editor() && !m_session.only_gcode_mode())
                do_reslice();
        }

        // reset cached size to force a resize on next call to render() to keep imgui in synch with canvas size
        preview->get_canvas3d()->reset_old_size();
        // BBS
        //view_toolbar.select_item("Preview");
        if (m_hub.nm() != nullptr)
            m_hub.nm()->set_in_preview(true);
    }
    else if (current_panel == assemble_view) {
        if (old_panel == view3D) {
            view3D->get_canvas3d()->unbind_event_handlers();
        }
        else if (old_panel == preview)
            preview->get_canvas3d()->unbind_event_handlers();

        assemble_view->get_canvas3d()->bind_event_handlers();
        assemble_view->reload_scene(true);

        if (old_panel == view3D) {
            GLCanvas3D* view3D_canvas = view3D->get_canvas3d();
            Selection::IndicesList select_idxs = view3D_canvas->get_selection().get_volume_idxs();
            Selection& assemble_selection = assemble_view->get_canvas3d()->get_selection();
            assemble_selection.clear();
            for (unsigned int idx : select_idxs) {
                auto v        = view3D_canvas->get_selection().get_volume(idx);
                auto real_idx = assemble_selection.query_real_volume_idx_from_other_view(v->object_idx(), v->instance_idx(), v->volume_idx());
                if (real_idx >= 0) {
                    assemble_selection.add(real_idx, false);
                }
            }
        }

        // BBS set default view and zoom
        if (first_enter_assemble) {
            m_q->get_camera().requires_zoom_to_volumes = true;   // 等价改写 ①（原 GUI_App 全局 plater()->get_camera()）
            first_enter_assemble = false;
        }

        assemble_view->set_as_dirty();
        // BBS
        //view_toolbar.select_item("Assemble");
    }

    current_panel->SetFocusFromKbd();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": successfully, exit");
}


void LayoutManager::msw_rescale()                    // 原 Plater::msw_rescale :5865 布局前缀
{
    preview->msw_rescale();
    view3D->get_canvas3d()->msw_rescale();
    sidebar->msw_rescale();
}

void LayoutManager::sys_color_changed()              // 原 Plater::sys_color_changed :5879 布局前缀
{
    preview->sys_color_changed();
    sidebar->sys_color_changed();
}

void LayoutManager::apply_color_mode()               // 原 :3551-3564（dark_mode 改槽）
{
    const bool is_dark         = m_q->dark_mode();
    wxColour   orca_color      = wxColour(59, 68, 70);//wxColour(ColorRGBA::ORCA().r_uchar(), ColorRGBA::ORCA().g_uchar(), ColorRGBA::ORCA().b_uchar());
    orca_color                 = is_dark ? StateColor::darkModeColorFor(orca_color) : StateColor::lightModeColorFor(orca_color);
    wxColour sash_color = is_dark ? wxColour(38, 46, 48) : wxColour(206, 206, 206);
    m_aui_mgr->GetArtProvider()->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_COLOUR, sash_color);
    m_aui_mgr->GetArtProvider()->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_TEXT_COLOUR, *wxWHITE);
    m_aui_mgr->GetArtProvider()->SetColour(wxAUI_DOCKART_SASH_COLOUR, sash_color);
    m_aui_mgr->GetArtProvider()->SetColour(wxAUI_DOCKART_BORDER_COLOUR, is_dark ? *wxBLACK : wxColour(165, 165, 165));

    if (sidebar)
        sidebar->update_color_mix_panel();
}

void LayoutManager::update_ui_from_settings()        // 原 :2205-2213 逐字
{
    apply_free_camera_correction();

    view3D->get_canvas3d()->update_ui_from_settings();
    preview->get_canvas3d()->update_ui_from_settings();

    sidebar->update_ui_from_settings();
}


bool LayoutManager::init_collapse_toolbar(const GLTexture* shared_background_texture)          // 原 :3897-3942（is_gcode_viewer 改槽；plater() 改 q）
{
    if (m_q->is_gcode_viewer())
        return true;

    BackgroundTexture::Metadata background_data;
    background_data.filename = m_hub.is_dark() ? "toolbar_background_dark.png" : "toolbar_background.png";
    background_data.left = 16;
    background_data.top = 16;
    background_data.right = 16;
    background_data.bottom = 16;

    if (collapse_toolbar.get_items_count() > 0) {
        if (shared_background_texture != nullptr)
            collapse_toolbar.init_shared_background(background_data, shared_background_texture);
        // already initialized
        return true;
    }

    if (shared_background_texture == nullptr || !collapse_toolbar.init_shared_background(background_data, shared_background_texture)) {
        if (!collapse_toolbar.init(background_data))
            return false;
    }

    collapse_toolbar.set_layout_type(GLToolbar::Layout::Vertical);
    collapse_toolbar.set_horizontal_orientation(GLToolbar::Layout::HO_Right);
    collapse_toolbar.set_vertical_orientation(GLToolbar::Layout::VO_Top);
    collapse_toolbar.set_border(4.0f);
    collapse_toolbar.set_separator_size(4);
    collapse_toolbar.set_gap_size(2);

    collapse_toolbar.del_all_item();

    GLToolbarItem::Data item;

    item.name = "collapse_sidebar";
    // set collapse svg name
    item.icon_filename = "collapse.svg";
    item.sprite_id = 0;
    item.left.action_callback = [this]() {          // 原 [] + GUI_App 全局 plater()（单例同指，等价改写 ①；持 q 先例）
        m_q->collapse_sidebar(!m_q->is_sidebar_collapsed());
    };

    if (!collapse_toolbar.add_item(item))
        return false;

    // Now "collapse" sidebar to current state. This is done so the tooltip
    // is updated before the toolbar is first used.
    m_q->collapse_sidebar(m_q->is_sidebar_collapsed());
    return true;
}

bool LayoutManager::has_assemble_view() const        // 原 :3970-3988（model 改槽）
{
    for (auto object: m_model.objects)
    {
        for (auto instance : object->instances)
            if (instance->is_assemble_initialized())
                return true;

        int part_cnt = 0;
        for (auto volume : object->volumes) {
            if (volume->is_model_part())
                part_cnt++;
        }

        if (part_cnt > 1)
            return true;
    }
    return false;
}

} // namespace app
} // namespace GUI
} // namespace Slic3r
