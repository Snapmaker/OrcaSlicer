// SceneRefreshCoordinator — 刷新调度服务实现（阶段 G4）。
// 迁移源：Plater.cpp priv 14 函数（手册 §2.2，逐字）+ Plater 公有 facade 内联体 changed_object 族
// （:5168-5225，D6）；宿主解引用改 SceneRefreshHost 槽 / NotificationHub& typed。
// 等价改写豁免口径（逐字审计）：①宿主解引用→槽 ②q->is_preview_shown→current_panel==preview（LayoutManager
// 同名内联原体）③q->force_update_all_plate_thumbnails→m_hub（Plater facade 原体一行 p->notification_hub()）④
// update/changed_object 分支判定抽 plan_update_flags/plan_changed_object_action 纯函数（D7，服务体消费钉语义）
// ⑤assert &p->model→&m_model（D6）⑥dead 块（#ifdef SUPPORT_AUTOCENTER/#if 0）逐字保留——宏全仓未定义，
// 预处理消除，零编译影响（手册 §2.4 勘误 6 同批登记）。
// Windows/asio 前置块必须置于一切 include 之前（G2 勘误 10：hpp 链 wx/defs.h 先引入 windows.h）。
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

#include "SceneRefreshCoordinator.hpp"

#include <cassert>
#include <utility>

#include "../GUI_Preview.hpp"       // View3D/Preview/AssembleView 完整类型（同头）+ GLCanvas3D（G2/G3 cpp-only 先例）
#include "../MainFrame.hpp"         // MainFrame::eEventObjectUpdate（update_slice_print_status 槽 int 实参 cast）
#include "../PartPlate.hpp"         // partplate_list 槽：get_curr_plate/has_printable_instances/get_shape
#include "NotificationHub.hpp"      // m_hub：force_update_all_plate_thumbnails（update preview shown 分支，D1）
#include "../Plater.hpp"             // H1 2.3: q faces（update/restart bg process + notify 桥接 + 公有化面）
#include "SlicingController.hpp"     // H1 2.3: m_slicing（schedule/is_export_scheduled/fff_print）
#include "ProjectSession.hpp"        // H1 2.3: m_session.printer_technology
#include "LayoutManager.hpp"         // H1 2.3: m_layout（view3d/preview/assemble_view/current_panel 族）
#include "ObjectEditCommands.hpp"    // H1 2.3: m_obj_cmds.on_action_layersediting（ensure_layers 直排）
#include "libslic3r/Model.hpp"      // Model/ModelObject/SINKING_Z_THRESHOLD/ModelInstancePVS_Partly_Outside
#include "libslic3r/BuildVolume.hpp"  // BuildVolume（update_print_volume_state）

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace GUI {

namespace app {

SceneRefreshCoordinator::SceneRefreshCoordinator(NotificationHub& hub, Plater* q, Model& model, PartPlateList& partplate_list,
                                                 SlicingController& slicing, ProjectSession& session, LayoutManager& layout,
                                                 ObjectEditCommands& obj_cmds, MainFrame* main_frame)
    : m_hub(hub), q(q), m_model(model), m_partplate_list(partplate_list), m_slicing(slicing)
    , m_session(session), m_layout(layout), m_obj_cmds(obj_cmds), m_main_frame(main_frame)
{
}

// —— 纯函数三件（D7；判定语义 = 原 update/changed_object/update facade 头部分支逐字）——

SceneRefreshCoordinator::UpdateFlagsPlan SceneRefreshCoordinator::plan_update_flags(PrinterTechnology technology, unsigned int flags)
{
    UpdateFlagsPlan plan;
    plan.force_bg_restart = technology == ptSLA || (flags & (unsigned int)UpdateParams::FORCE_BACKGROUND_PROCESSING_UPDATE);
    plan.full_refresh     = flags & (unsigned int)UpdateParams::FORCE_FULL_SCREEN_REFRESH;
    plan.postpone_errors  = flags & (unsigned int)UpdateParams::POSTPONE_VALIDATION_ERROR_MESSAGE;
    return plan;
}

SceneRefreshCoordinator::ChangedObjectAction SceneRefreshCoordinator::plan_changed_object_action(PrinterTechnology technology)
{
    ChangedObjectAction action;
    action.restart_bg  = technology == ptSLA;   // 原 changed_object :5175 SLA 分支
    action.reload_scene = technology != ptSLA;  // 原 FFF 分支（view3D reload_scene）
    return action;
}

SceneRefreshCoordinator::UpdateGatePlan SceneRefreshCoordinator::plan_update_gate(bool need_update, bool conside_update_flag)
{
    UpdateGatePlan plan;
    if (!conside_update_flag) {          // 原 facade :4023-4024 else 分支：无条件调用、不清旗标
        plan.call = true;
        plan.clear_need_update = false;
    } else if (need_update) {            // 原 :4017-4021：调用并清旗标
        plan.call = true;
        plan.clear_need_update = true;
    } else {                             // 原 :4016-4022 else 空：跳过
        plan.call = false;
        plan.clear_need_update = false;
    }
    return plan;
}

// —— 原 priv::update（Plater.cpp:1958-1995 逐字，槽消费）——

void SceneRefreshCoordinator::update(unsigned int flags)
{
    // the following line, when enabled, causes flickering on NVIDIA graphics cards
//    wxWindowUpdateLocker freeze_guard(q);
#ifdef SUPPORT_AUTOCENTER
    if (get_config("autocenter") == "true")
        model.center_instances_around_point(this->layout().bed.build_volume().bed_center());
#endif

    unsigned int update_status = 0;
    const UpdateFlagsPlan plan = plan_update_flags(m_session.printer_technology, flags);
    if (plan.force_bg_restart)
        // Update the SLAPrint from the current Model, so that the reload_scene()
        // pulls the correct data.
        update_status = q->update_background_process(false, plan.postpone_errors);
    //BBS TODO reload_scene
    m_layout.view3D->reload_scene(false, plan.full_refresh);
    m_layout.preview->reload_print();
    //BBS assemble view
    m_layout.assemble_view->reload_scene(false, flags);

    wxPanel* current = m_layout.current_panel;
    if (current && current == m_layout.preview) {   // 原 q->is_preview_shown()（LayoutManager::is_preview_shown 原体等价）
        m_hub.force_update_all_plate_thumbnails();  // 原 q->force_update_all_plate_thumbnails()（facade 原体= notification_hub().同名，D1 typed）
        //update_fff_scene_only_shells(true);
    }

    if (plan.force_bg_restart)
        q->restart_background_process(update_status);
    else
        m_slicing.schedule_background_process();

    // BBS
#if 0
    if (get_config("autocenter") == "true" && this->layout().sidebar->obj_manipul()->IsShown())
        this->layout().sidebar->obj_manipul()->UpdateAndShow(true);
#endif

    m_layout.update_sidebar();
}

// —— 原 Plater::update(bool,bool) facade 门控（:4014-4025 语义逐字，门控随服务 §0.1）——

void SceneRefreshCoordinator::update_gate(bool conside_update_flag, bool force_background_processing_update)
{
    unsigned int flag = force_background_processing_update ? (unsigned int)UpdateParams::FORCE_BACKGROUND_PROCESSING_UPDATE : 0;
    const UpdateGatePlan gate = plan_update_gate(m_need_update, conside_update_flag);
    if (gate.call)
        update(flag);
    if (gate.clear_need_update)
        m_need_update = false;                      // 原 p->set_need_update(false)
}

// —— 原 priv::selection_changed（:2167-2186，槽消费；layers 分支整段封装，陷阱 9）——

void SceneRefreshCoordinator::selection_changed()
{
    // if the selection is not valid to allow for layer editing, we need to turn off the tool if it is running
    if (!q->layers_height_allowed() && m_layout.view3D->is_layers_editing_enabled()) {
        m_obj_cmds.on_action_layersediting();
    }

    // forces a frame render to update the view (to avoid a missed update if, for example, the context menu appears)
    GLCanvas3D* canvas = m_layout.get_current_canvas3D();
    if (canvas) {
        if (canvas->get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
            if (AssembleView* assemble = m_layout.assemble_view)
                assemble->render();
        } else {
            if (View3D* view3d = m_layout.view3D)
                view3d->render();
        }
    }
}

// —— 原 priv::object_list_changed（:2188-2207，槽消费）——

void SceneRefreshCoordinator::object_list_changed()
{
    const bool export_in_progress = m_slicing.background_process().is_export_scheduled(); // || ! send_gcode_file.empty());
    // XXX: is this right?
    //const bool model_fits = layout().view3D->get_canvas3d()->check_volumes_outside_state() == ModelInstancePVS_Inside;
    const bool model_fits = m_layout.view3D->get_canvas3d()->check_volumes_outside_state() != ModelInstancePVS_Partly_Outside;

    PartPlate* part_plate = m_partplate_list.get_curr_plate();

    // BBS
    //layout().sidebar->enable_buttons(!model.objects.empty() && !export_in_progress && model_fits && part_plate->has_printable_instances());
    bool can_slice = !m_model.objects.empty() && !export_in_progress && model_fits && part_plate->has_printable_instances()
        && q->can_current_plate_be_sliced();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": can_slice %1%, model_fits= %2%, export_in_progress %3%, has_printable_instances %4% ")%can_slice %model_fits %export_in_progress %part_plate->has_printable_instances();
    m_main_frame->update_slice_print_status((MainFrame::SlicePrintEventType)int(MainFrame::eEventObjectUpdate), can_slice);

    q->notify_filament_compatibility_after_apply();

    q->notify_object_config_changed();
}

// —— 原 priv::update_print_volume_state（:2328-2334，槽消费；bed 高度经 printable_height 膨胀槽）——

void SceneRefreshCoordinator::update_print_volume_state()
{
    //BBS: use the plate's bounding box instead of the bed's
    PartPlate* pp = m_partplate_list.get_curr_plate();
    BuildVolume build_volume(pp->get_shape(), m_layout.bed.build_volume().printable_height());
    m_model.update_print_volume_state(build_volume);
}

// —— 原 priv::update_fff_scene（:2428-2436，槽消费）——

void SceneRefreshCoordinator::update_fff_scene()
{
    if (m_layout.preview != nullptr)
        m_layout.preview->reload_print();
    // In case this was MM print, wipe tower bounding box on 3D tab might need redrawing with exact depth:
    m_layout.view3D->reload_scene(true);
    //BBS: add assemble view related logic
    m_layout.assemble_view->reload_scene(true);
}

// —— 原 priv::update_fff_scene_only_shells（:2439-2455，槽消费）——

void SceneRefreshCoordinator::update_fff_scene_only_shells(bool only_shells)
{
    if (m_layout.preview != nullptr)
    {
        const Print* current_print = m_slicing.background_process().fff_print();
        if (current_print)
        {
            //this->layout().preview->reset_shells();
            m_layout.preview->load_shells(*current_print);
        }
    }

    if (!only_shells) {
        m_layout.view3D->reload_scene(true);
        m_layout.assemble_view->reload_scene(true);
    }
}

// —— 原 priv::update_sla_scene（:2457-2463，槽消费；延迟旗标随服务清零，D3）——

void SceneRefreshCoordinator::update_sla_scene()
{
    // Update the SLAPrint from the current Model, so that the reload_scene()
    // pulls the correct data.
    m_delayed_scene_refresh = false;
    q->update_restart_background_process(true, true);
}

// —— 原 priv::set_current_canvas_as_dirty（:3357-3365，槽消费；指针比较，G3 pick 同款）——

void SceneRefreshCoordinator::set_current_canvas_as_dirty()
{
    if (m_layout.current_panel == m_layout.view3D)
        m_layout.view3D->set_as_dirty();
    else if (m_layout.current_panel == m_layout.preview)
        m_layout.preview->set_as_dirty();
    else if (m_layout.current_panel == m_layout.assemble_view)
        m_layout.assemble_view->set_as_dirty();
}

// —— 原 priv::unbind_canvas_event_handlers（:3369-3379，槽消费）——

void SceneRefreshCoordinator::unbind_canvas_event_handlers()
{
    if (m_layout.view3D != nullptr)
        m_layout.view3D->get_canvas3d()->unbind_event_handlers();

    if (m_layout.preview != nullptr)
        m_layout.preview->get_canvas3d()->unbind_event_handlers();

    if (m_layout.assemble_view != nullptr)
        m_layout.assemble_view->get_canvas3d()->unbind_event_handlers();
}

// —— 原 priv::reset_canvas_volumes（:3381-3388，槽消费）——

void SceneRefreshCoordinator::reset_canvas_volumes()
{
    if (m_layout.view3D != nullptr)
        m_layout.view3D->get_canvas3d()->reset_volumes();

    if (m_layout.preview != nullptr)
        m_layout.preview->get_canvas3d()->reset_volumes();
}

// —— 原 priv::update_preview_bottom_toolbar（:3392-3395，空体逐字——主文档 §6.3 拆法实测为空，随 G4 收编注销）——

void SceneRefreshCoordinator::update_preview_bottom_toolbar()
{
    ;
}

// —— 原 priv::reset_gcode_toolpaths（:3404-3407，槽消费）——

void SceneRefreshCoordinator::reset_gcode_toolpaths()
{
    m_layout.preview->get_canvas3d()->reset_gcode_toolpaths();
}

// —— 原 Plater::changed_object 族（:5168-5225 公有 facade 内联体体迁，D6）——

void SceneRefreshCoordinator::changed_object(ModelObject& object)
{
    assert(object.get_model() == &m_model); // is object from same model?（D6 等价改写：&p->model→&m_model）
    object.invalidate_bounding_box();

    // recenter and re - align to Z = 0
    object.ensure_on_bed(m_session.printer_technology != ptSLA);

    const ChangedObjectAction action = plan_changed_object_action(m_session.printer_technology);
    if (action.restart_bg) {
        // Update the SLAPrint from the current Model, so that the reload_scene()
        // pulls the correct data, update the 3D scene.
        q->update_restart_background_process(true, false);
    } else
        m_layout.view3D->reload_scene(false);

    // update print
    m_slicing.schedule_background_process();

    // Check outside bed
    GLCanvas3D* canvas = m_layout.get_current_canvas3D();
    if (canvas)
        canvas->requires_check_outside_state();
}

void SceneRefreshCoordinator::changed_object(int obj_idx)
{
    if (obj_idx < 0)
        return;
    ModelObject* object = m_model.objects[obj_idx];
    if (object == nullptr)
        return;
    changed_object(*object);
}

void SceneRefreshCoordinator::changed_objects(const std::vector<size_t>& object_idxs)
{
    if (object_idxs.empty())
        return;

    for (size_t obj_idx : object_idxs) {
        if (obj_idx < m_model.objects.size()) {
            if (m_model.objects[obj_idx]->min_z() >= SINKING_Z_THRESHOLD)
                // re - align to Z = 0
                m_model.objects[obj_idx]->ensure_on_bed();
        }
    }
    if (m_session.printer_technology == ptSLA) {
        // Update the SLAPrint from the current Model, so that the reload_scene()
        // pulls the correct data, update the 3D scene.
        q->update_restart_background_process(true, false);
    }
    else {
        m_layout.view3D->reload_scene(false);
        m_layout.view3D->get_canvas3d()->update_instance_printable_state_for_objects(object_idxs);
    }

    // update print
    m_slicing.schedule_background_process();
}

} // namespace app
} // namespace GUI
} // namespace Slic3r
