// SlicingController — Plater 切片域服务实现（阶段 D2 纯搬移，手册 §3）。
// ⚠ 本文件禁止 include Plater/GUI_App 宿主头——上下文只能经 Host 进入（AGENTS.md）。
// 由 generate_controller.py 从 Plater.cpp 机械搬移生成；逐函数 diff 非替换类差异应 = 0。
#include "SlicingController.hpp"
#include "../Plater.hpp"                   // H1 2.3: q faces + F3 bridges + publicized faces
#include "../MainFrame.hpp"                // H1 2.3: m_main_frame->update_slice_print_status
#include "../GUI.hpp"                      // H1 2.3: show_error 直调
#include "../RemovableDriveManager.hpp"    // H1 2.3: removable_drive_manager()->set_exporting_finished 完整型（b23_6 C2027）
#include "../Jobs/Worker.hpp"              // H1 2.3: stop_queue 直调
#include "NotificationHub.hpp"             // H1 2.3: m_hub（warnings 族 + nm()）
#include "SnapshotService.hpp"             // H1 2.3: m_snapshots.take_snapshot
#include "PlateController.hpp"             // H1 2.3: m_plates.on_plate_selected（plate_select_event 直排）
#include "ProjectSession.hpp"              // H1 2.3: late_bind session
#include "ExportSendCoordinator.hpp"       // H1 2.3: late_bind ex
#include "ArrangementService.hpp"          // H1 2.3: late_bind arrange
#include "MixingGateKeeper.hpp"            // H1 2.3: late_bind gate_keeper
#include "CalibrationFlow.hpp"             // H1 2.3: late_bind calib
#include "LayoutManager.hpp"               // H1 2.3: late_bind layout
#include "SceneRefreshCoordinator.hpp"     // H1 2.3: late_bind refresh

#include "../BackgroundSlicingProcess.hpp"  // BackgroundSlicingProcess / SlicingProcessCompletedEvent / EVT_PROCESS_COMPLETED
#include "../PartPlate.hpp"
#include "../SelectMachine.hpp"      // SelectMachineDialog 完整类型（update_print_required_data 对话框转发）
#include "../MsgDialog.hpp"                  // RichMessageDialog（内存警戒对话框）                 // PartPlateList / PartPlate / PlateBBoxData
#include "../GUI_Preview.hpp"               // View3D / Preview
#include "../GLCanvas3D.hpp"                // GLCanvas3D / ThumbnailsParams / ThumbnailsList / Camera
#include "../GUI_ObjectList.hpp"            // ObjectList
#include "../MsgDialog.hpp"
#include "../NotificationManager.hpp"
#include "../I18N.hpp"                      // _u8L / _L
#include "../wxExtensions.hpp"              // into_u8
#include "../PlateSettingsDialog.hpp"       // PlateNameEditDialog（select_plate_by_hover_id 重命名分支）
#include "../FilamentGroupDialog.hpp"       // 阶段 I：切片前的耗材分组确认
#include "../FlowTypeHelper.hpp"            // 阶段 I：高流量判定与体积类型同步
#include "../../Utils/SnapLogClient.hpp"                   // SNAP_LOG_BATCH（如置于他头随编译修正）
#include "libslic3r/Print.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Platform.hpp"           // PlatformFlavor（经 Host 判定）
#include "libslic3r/Format/bbs_3mf.hpp"
#include "boost/log/trivial.hpp"

#include <boost/format.hpp>

namespace Slic3r {

extern const wxEventTypeTag<SlicingProcessCompletedEvent> EVT_PROCESS_COMPLETED;  // 定义于 Slic3r 层，Plater.cpp 同库定义
namespace GUI {

namespace app {

// 原 priv 枚举镜像（Plater.cpp priv::UpdateBackgroundProcessReturnState）——值必须一致（G 阶段收口）
enum : unsigned int {
    UPDATE_BACKGROUND_PROCESS_INVALID        = 0x0001,
    UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE  = 0x0002,
    UPDATE_BACKGROUND_PROCESS_RESTART        = 0x0004,
    UPDATE_BACKGROUND_PROCESS_FORCE_RESTART  = 0x0008,
    UPDATE_BACKGROUND_PROCESS_FORCE_EXPORT   = 0x0010,
};

// EVT_PROCESS_COMPLETED 等事件常量定义于 Plater.cpp（同库链接）——extern 引用

SlicingController::SlicingController(Plater* q, PartPlateList& partplate_list, Model& model, DynamicPrintConfig* config,
                                     NotificationHub& hub, SnapshotService& snapshots, PlateController& plates,
                                     Worker& worker, MainFrame* main_frame,
                                     Print& fff_print, SLAPrint& sla_print, GCodeProcessorResult& gcode_result)
    : q(q), m_partplate_list(partplate_list), m_model(model), m_config(config), m_hub(hub)
    , m_snapshots(snapshots), m_plates(plates), m_worker(worker), m_main_frame(main_frame)
    , m_fff_print(fff_print), m_sla_print(sla_print), m_gcode_result(gcode_result)
{
}

void SlicingController::late_bind(ProjectSession& session, ExportSendCoordinator& ex, ArrangementService& arrange,
                                  MixingGateKeeper& gate_keeper, CalibrationFlow& calib, LayoutManager& layout,
                                  SceneRefreshCoordinator& refresh)
{
    m_session     = &session;
    m_ex          = &ex;
    m_arrange     = &arrange;
    m_gate_keeper = &gate_keeper;
    m_calib       = &calib;
    m_layout      = &layout;
    m_refresh     = &refresh;
}

void SlicingController::call_after_fn(std::function<void()> fn)
{
    q->CallAfter([fn]() { fn(); });
}

void SlicingController::schedule_background_process() {
    m_refresh->clear_delayed_error_message();
    // Trigger the timer event after 0.5s
    m_background_process_timer.Start(500, wxTIMER_ONE_SHOT);
    // Notify the Canvas3D that something has changed, so it may invalidate some of the layer editing stuff.
    m_layout->view3D->get_canvas3d()->set_config(m_config);
}

bool SlicingController::can_current_plate_be_sliced() const {
    const PartPlate *plate = m_partplate_list.get_curr_plate();
    return plate && plate->can_slice() && !m_gate_keeper->has_incompatible_mixed_filament_in_use();
}

unsigned int SlicingController::update_background_process(bool force_validation, bool postpone_error_messages, bool switch_print) {
    // bitmap of enum UpdateBackgroundProcessReturnState
    unsigned int return_state = 0;

    // If the update_background_process() was not called by the timer, kill the timer,
    // so the update_restart_background_process() will not be called again in vain.
    m_background_process_timer.Stop();
    // Update the "out of print bed" state of ModelInstances.
    m_refresh->update_print_volume_state();
    // Apply new config to the possibly running background task.
    bool               was_running = m_background_process.running();
    //BBS: add the switch print logic before Print::Apply
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": enter, force_validation=%1% postpone_error_messages=%2%, switch_print=%3%, was_running=%4%")%force_validation %postpone_error_messages %switch_print %was_running;
    if (switch_print)
    {
        //BBS: update the current print to the current plate
        m_partplate_list.update_slice_context_to_current_plate(m_background_process);
        m_layout->preview->update_gcode_result(m_partplate_list.get_current_slice_result());
    }
    Print::ApplyStatus invalidated = m_background_process.apply(m_model, q->preset_bundle()->full_config());
    q->notify_filament_compatibility_after_apply();

    if ((invalidated == Print::APPLY_STATUS_CHANGED) || (invalidated == Print::APPLY_STATUS_INVALIDATED))
        // BBS: add only gcode mode
        m_session->set_only_gcode(false);

    //BBS: add slicing related logs
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": background process apply result=%1%")%invalidated;
    if (m_background_process.empty())
        m_layout->view3D->get_canvas3d()->reset_sequential_print_clearance();

    if (invalidated == Print::APPLY_STATUS_INVALIDATED) {
        //BBS: update current plater's slicer result to invalid
        m_background_process.get_current_plate()->update_slice_result_valid_state(false);

        //no need, should be done in m_background_process.apply
        //m_background_process.get_current_gcode_result()->reset();
        // Reset m_layout->preview canvases. If the print has been invalidated, the m_layout->preview canvases will be cleared.
        // Otherwise they will be just refreshed.
        if (m_layout->preview != nullptr) {
            // If the m_layout->preview is not visible, the following line just invalidates the m_layout->preview,
            // but the G-code paths or SLA m_layout->preview are calculated first once the m_layout->preview is made visible.
            m_refresh->reset_gcode_toolpaths();
            m_layout->preview->reload_print();
        }
        // In FDM mode, we need to reload the 3D scene because of the wipe tower m_layout->preview box.
        // In SLA mode, we need to reload the 3D scene every time to show the support structures.
        if (m_session->printer_technology == ptSLA || (m_session->printer_technology == ptFFF && m_config->opt_bool("enable_prime_tower")))
            return_state |= UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE;

        m_hub.nm()->set_slicing_progress_hidden();
    }
    else {
        if (m_layout->preview && m_layout->preview->get_reload_paint_after_background_process_apply()) {
            m_layout->preview->set_reload_paint_after_background_process_apply(false);
            m_layout->preview->reload_print();
        }
    }

    if ((invalidated != Print::APPLY_STATUS_UNCHANGED || force_validation) && ! m_background_process.empty()) {
        // The delayed error message is no more valid.
        m_refresh->clear_delayed_error_message();
        // The state of the Print changed, and it is non-zero. Let's validate it and give the user feedback on errors.

        //BBS: add is_warning logic
        StringObjectException warning;
        //BBS: refine seq-print logic
        Polygons polygons;
        std::vector<std::pair<Polygon, float>> height_polygons;
        StringObjectException err = m_background_process.validate(&warning, &polygons, &height_polygons);
        // update string by type
        q->post_process_string_object_exception(err);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": validate err=%1%, warning=%2%")%err.string%warning.string;

        if (err.string.empty()) {
            // Pass a warning from validation and either show a notification,
            // or hide the old one.
            m_hub.process_validation_warning(warning);

            // Validate passed, but also check filament temp mixing as a final
            // guard. check_filament_temp_mixing() reads directly from preset
            // configs and catches cases that Print::validate() may miss (e.g.
            // wall_filament changes that haven't propagated to PrintRegions yet).
            bool filament_ok = m_gate_keeper->sync_filament_temp_mixing_notification();
            bool cold_ok    = m_gate_keeper->sync_cold_plate_notification();
            if (filament_ok && cold_ok) {
                m_partplate_list.get_curr_plate()->update_apply_result_invalid(false);
                m_hub.nm()->set_all_slicing_errors_gray(true);
                m_hub.nm()->close_notification_of_type(NotificationType::ValidateError);
            }
            else {
                return_state |= UPDATE_BACKGROUND_PROCESS_INVALID;
            }
            // flow_ratio_zero sync MUST run after the blanket close_notification_of_type
            // (ValidateError) above - otherwise our banner gets closed immediately.
            // Its return value gates UPDATE_BACKGROUND_PROCESS_INVALID so that
            // restart_background_process() refuses to start the slice when the
            // plate is blocked (Preview-tab switch / reslice path).
            const bool flow_ratio_ok = m_gate_keeper->sync_flow_ratio_zero_notification();
            if (!flow_ratio_ok)
                return_state |= UPDATE_BACKGROUND_PROCESS_INVALID;
            if (filament_ok && cold_ok && flow_ratio_ok && invalidated != Print::APPLY_STATUS_UNCHANGED && q->background_processing_enabled())
                return_state |= UPDATE_BACKGROUND_PROCESS_RESTART;

            if (m_session->printer_technology == ptFFF) {
                m_layout->view3D->get_canvas3d()->reset_sequential_print_clearance();
                m_layout->view3D->get_canvas3d()->set_as_dirty();
                m_layout->view3D->get_canvas3d()->request_extra_frame();
            }
        }
        else {
            m_partplate_list.get_curr_plate()->update_apply_result_invalid(true);
            // The print is not valid.
            // Show error as notification.
            m_hub.nm()->push_validate_error_notification(err);
            //also update the warnings
            m_hub.process_validation_warning(warning);
            m_gate_keeper->sync_filament_temp_mixing_notification();
            m_gate_keeper->sync_flow_ratio_zero_notification();
            m_gate_keeper->sync_cold_plate_notification();
            return_state |= UPDATE_BACKGROUND_PROCESS_INVALID;
            if (m_session->printer_technology == ptFFF) {
                const Print* print = m_background_process.fff_print();
                //Polygons polygons;
                //if (print->config().print_sequence == PrintSequence::ByObject)
                //    Print::sequential_print_clearance_valid(*print, &polygons);
                m_layout->view3D->get_canvas3d()->set_sequential_print_clearance_visible(true);
                m_layout->view3D->get_canvas3d()->set_sequential_print_clearance_render_fill(true);
                m_layout->view3D->get_canvas3d()->set_sequential_print_clearance_polygons(polygons, height_polygons);
            }
        }
    }
    else if (!m_refresh->has_delayed_error_message()) {
        // Reusing the old state.
        return_state |= UPDATE_BACKGROUND_PROCESS_INVALID;
    }

    //actualizate warnings
    if (invalidated != Print::APPLY_STATUS_UNCHANGED || m_background_process.empty()) {
        if (m_background_process.empty()) {
            m_hub.process_validation_warning({});
            m_gate_keeper->sync_filament_temp_mixing_notification();
            m_gate_keeper->sync_flow_ratio_zero_notification();
            m_gate_keeper->sync_cold_plate_notification();
        }
        m_hub.actualize_slicing_warnings(*m_background_process.current_print());
        m_hub.actualize_object_warnings(*m_background_process.current_print());
        q->set_show_warning_dialog(false);
        q->set_process_completed_with_error(-1);
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: was_running = %2%, running %3%, invalidated=%4%, return_state=%5%, internal_cancel=%6%")
        % __LINE__ % was_running % m_background_process.running() % invalidated % return_state % m_background_process.is_internal_cancelled();
    if (was_running && ! m_background_process.running() && (return_state & UPDATE_BACKGROUND_PROCESS_RESTART) == 0) {
        if (invalidated != Print::APPLY_STATUS_UNCHANGED || m_background_process.is_internal_cancelled())
        {
            // The background processing was killed and it will not be restarted.
            // Post the "canceled" callback message, so that it will be processed after any possible pending status bar update messages.
            SlicingProcessCompletedEvent evt(EVT_PROCESS_COMPLETED, 0,
                SlicingProcessCompletedEvent::Cancelled, nullptr);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%, post an EVT_PROCESS_COMPLETED to main, status %2%")%__LINE__ %evt.status();
            wxQueueEvent(q, evt.Clone());
        }
    }

    if ((return_state & UPDATE_BACKGROUND_PROCESS_INVALID) != 0)
    {
        // Validation of the background data failed.
        //BBS: add slice&&print status update logic
        m_main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, false);

        q->set_process_completed_with_error(m_partplate_list.get_curr_plate_index());
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: set to process_completed_with_error, return_state=%2%")%__LINE__%return_state;
    }
    else
    {
        // Background data is valid.
        if ((return_state & UPDATE_BACKGROUND_PROCESS_RESTART) != 0 ||
            (return_state & UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE) != 0 )
            m_hub.nm()->set_slicing_progress_hidden();

        //BBS: add slice&&print status update logic
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: background data valid, return_state=%2%")%__LINE__%return_state;
        PartPlate* cur_plate = m_background_process.get_current_plate();
        if (m_background_process.finished() && cur_plate && cur_plate->is_slice_result_valid())
        {
            //ready_to_slice = false;
            m_main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, false);
        }
        else if (!m_background_process.empty() &&
                 !m_background_process.running()) /* Do not update buttons if background process is running
                                                 * This condition is important for SLA mode especially,
                                                 * when this function is called several times during calculations
                                                 * */
        {
            if (cur_plate && cur_plate->can_slice() && !m_gate_keeper->has_incompatible_mixed_filament_in_use()) {
                //ready_to_slice = true;
                m_main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, true);
                q->set_process_completed_with_error(-1);
            }
            else {
                //ready_to_slice = false;
                m_main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, false);
                q->set_process_completed_with_error(m_partplate_list.get_curr_plate_index());
            }
        }
#if 0
        // 阶段 E2：引用已删按钮文案死状态的注释行一并清除（commit ③）

        //const wxString slice_string = m_background_process.running() && wxGetApp().get_mode() == comSimple ?
        //                              _L("Slicing") + dots : _L("Slice now");
        //sidebar->set_btn_label(ActionButtonType::abReslice, slice_string);

        //if (m_background_process.finished())
        //    show_action_buttons(false);
        //else if (!m_background_process.empty() &&
        //         !m_background_process.running()) /* Do not update buttons if background process is running
        //                                         * This condition is important for SLA mode especially,
        //                                         * when this function is called several times during calculations
        //                                         * */
        //    show_action_buttons(true);
#endif
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: exit, return_state=%2%")%__LINE__%return_state;
    return return_state;
}

bool SlicingController::restart_background_process(unsigned int state) {
    if (!m_arrange->worker_idle()) {
        // Avoid a race condition
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: ui jobs running, return false")%__LINE__;
        return false;
    }

    if ( ! m_background_process.empty() &&
         (state & UPDATE_BACKGROUND_PROCESS_INVALID) == 0 &&
         ( ((state & UPDATE_BACKGROUND_PROCESS_FORCE_RESTART) != 0 && ! m_background_process.finished()) ||
           (state & UPDATE_BACKGROUND_PROCESS_FORCE_EXPORT) != 0 ||
           (state & UPDATE_BACKGROUND_PROCESS_RESTART) != 0 ) ) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: print is valid, try to start it now")%__LINE__;
        // The print is valid and it can be started.
        if (m_background_process.start()) {
            if (!q->get_show_warning_dialog())
                handle_slicing_began();
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: start successfully")%__LINE__;
            return true;
        }
    }
    else if (m_background_process.empty()) {
        PartPlate* cur_plate = m_background_process.get_current_plate();
        if (cur_plate->is_slice_result_valid() && ((state & UPDATE_BACKGROUND_PROCESS_FORCE_RESTART) != 0)) {
            if (m_background_process.start()) {
                if (!q->get_show_warning_dialog())
                    handle_slicing_began();
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: start successfully")%__LINE__;
                return true;
            }
        }
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: not started")%__LINE__;
    return false;
}

unsigned int SlicingController::update_restart_background_process(bool force_update_scene, bool force_update_preview) {
    bool switch_print = true;
    //BBS: judge whether can switch print or not
    if ((m_partplate_list.get_plate_count() > 1) && !m_background_process.can_switch_print())
    {
        //can not switch print currently
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": plate count %1%, can not switch") % m_partplate_list.get_plate_count();
        switch_print = false;
    }
    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = this->update_background_process(false, false, switch_print);
    if (force_update_scene || (state & UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE) != 0)
        m_layout->view3D->reload_scene(false);

    if (force_update_preview)
        m_layout->preview->reload_print();
    this->restart_background_process(state);
    return state;
}

void SlicingController::handle_slicing_update(SlicingStatusEvent &evt) {
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": event_type %1%, percent %2%, text %3%") % evt.GetEventType() % evt.status.percent % evt.status.text;
    //BBS: add slice project logic
    std::string title_text = _u8L("Slicing");
    evt.status.text = title_text + evt.status.text;
    if (evt.status.percent >= 0) {
         if (!m_arrange->worker_idle()) {
            // Avoid a race condition
            return;
        }

        m_hub.nm()->set_slicing_progress_percentage(evt.status.text, (float)evt.status.percent / 100.0f);

        // update slicing percent
        PartPlateList& plate_list = m_partplate_list;
        //slicing parallel, only update if percent is greater than before
        if (evt.status.percent > plate_list.get_curr_plate()->get_slicing_percent())
            plate_list.get_curr_plate()->update_slicing_percent(evt.status.percent);
    }

    if (evt.status.flags & (PrintBase::SlicingStatus::RELOAD_SCENE | PrintBase::SlicingStatus::RELOAD_SLA_SUPPORT_POINTS)) {
        switch (m_session->printer_technology) {
        case ptFFF:
            //BBS: add slice project logic, only display shells at the beginning
            if (!m_slice_all || (m_cur_slice_plate == (m_partplate_list.get_plate_count() - 1)))
                //m_refresh->update_fff_scene();
                m_refresh->update_fff_scene_only_shells(true);
            break;
        case ptSLA:
            // If RELOAD_SLA_SUPPORT_POINTS, then the SLA gizmo is updated (reload_scene calls update_gizmos_data)
            if (m_layout->view3D->is_dragging())
                m_refresh->set_delayed_scene_refresh(true);
            else
                m_refresh->update_sla_scene();
            break;
        default: break;
        }
    } else if (evt.status.flags & PrintBase::SlicingStatus::RELOAD_SLA_PREVIEW) {
        // Update the SLA m_layout->preview. Only called if not RELOAD_SLA_SUPPORT_POINTS, as the block above will refresh the m_layout->preview anyways.
        m_layout->preview->reload_print();
    }

    if (evt.status.flags & (PrintBase::SlicingStatus::UPDATE_PRINT_STEP_WARNINGS | PrintBase::SlicingStatus::UPDATE_PRINT_OBJECT_STEP_WARNINGS)) {
        // Update notification center with warnings of object_id and its warning_step.
        ObjectID object_id = evt.status.warning_object_id;
        int warning_step = evt.status.warning_step;
        PrintStateBase::StateWithWarnings state;
        ModelObject const * model_object = nullptr;

        //BBS: add partplate related logic, use the print in background process
        if (evt.status.flags & PrintBase::SlicingStatus::UPDATE_PRINT_STEP_WARNINGS) {
            state = m_session->printer_technology == ptFFF ?
                m_background_process.fff_print()->step_state_with_warnings(static_cast<PrintStep>(warning_step)) :
                m_background_process.sla_print()->step_state_with_warnings(static_cast<SLAPrintStep>(warning_step));
        } else if (m_session->printer_technology == ptFFF) {
            const PrintObject *print_object = m_background_process.fff_print()->get_object(object_id);
            if (print_object) {
                state = print_object->step_state_with_warnings(static_cast<PrintObjectStep>(warning_step));
                model_object = print_object->model_object();
            }
        } else {
            const SLAPrintObject *print_object = m_background_process.sla_print()->get_object(object_id);
            if (print_object) {
                state = print_object->step_state_with_warnings(static_cast<SLAPrintObjectStep>(warning_step));
                model_object = print_object->model_object();
            }
        }
        // Now process state.warnings.
        for (auto const& warning : state.warnings) {
            if (warning.current) {
                NotificationManager::NotificationLevel notif_level = NotificationManager::NotificationLevel::WarningNotificationLevel;
                if (evt.status.message_type == PrintStateBase::SlicingNotificationType::SlicingReplaceInitEmptyLayers || evt.status.message_type == PrintStateBase::SlicingNotificationType::SlicingEmptyGcodeLayers) {
                    notif_level = NotificationManager::NotificationLevel::SeriousWarningNotificationLevel;
                }
                m_hub.nm()->push_slicing_warning_notification(warning.message, false, model_object, object_id, warning_step, warning.message_id, notif_level);
                m_hub.add_warning(warning, object_id.id);
            }
        }
    }
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format("exit.");
}

void SlicingController::handle_slicing_completed(wxCommandEvent & evt) {
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": event_type %1%, string %2%") % evt.GetEventType() % evt.GetString();

    if (GUI::wxGetApp().is_recreating_gui())
        return;

    //BBS: add slice project logic
    if (m_slice_all && (m_cur_slice_plate < (m_partplate_list.get_plate_count() - 1))) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format("slicing all, finished plate %1%, will continue next.")%m_cur_slice_plate;
        return;
    }

    if (m_layout->view3D->is_dragging()) // updating scene now would interfere with the gizmo dragging
        m_refresh->set_delayed_scene_refresh(true);
    else {
        if (m_session->printer_technology == ptFFF) {
            //BBS: only reload shells
            m_refresh->update_fff_scene_only_shells(false);
            //m_refresh->update_fff_scene();
        }
        else
            m_refresh->update_sla_scene();
    }
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format("exit.");
}

void SlicingController::handle_export_began() {
    if (q->get_show_warning_dialog())
        m_hub.warnings_dialog();
}

void SlicingController::handle_export_finished() {
#if 0
    //BBS: also export 3mf to the same directory for debugging
    std::string gcode_path_str(evt.GetString().ToUTF8().data());
    fs::path gcode_path(gcode_path_str);

    if (q) {
        q->export_3mf(gcode_path.replace_extension(".3mf"), SaveStrategy::Silence); // BBS: silence
    }
#endif
}

void SlicingController::handle_slicing_began() {
    if(!m_slice_timing_active)
        m_slice_start_time    = std::chrono::steady_clock::now();
    m_slice_timing_active = true;
    m_hub.clear_warnings();
    m_hub.nm()->close_notification_of_type(NotificationType::SignDetected);
    m_hub.nm()->close_notification_of_type(NotificationType::ExportFinished);
    bool is_first_plate = m_cur_slice_plate == 0;
    bool slice_all = m_session->only_gcode_mode() ? m_slice_all_only_has_gcode : m_slice_all;
    bool need_change_dailytips = !(slice_all && !is_first_plate);
    m_hub.nm()->set_slicing_progress_began();
    m_hub.nm()->update_slicing_notif_dailytips(need_change_dailytips);
}

void SlicingController::handle_process_completed(SlicingProcessCompletedEvent &evt) {
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": enter, m_ignore_event %1%, status %2%")%m_ignore_event %evt.status();
    //BBS:ignore cancel event for some special case
    if (m_ignore_event)
    {
        m_ignore_event = false;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": ignore this event %1%") % evt.status();
        return;
    }
    //BBS: add project slice logic
    bool is_finished = !m_slice_all || (m_cur_slice_plate == (m_partplate_list.get_plate_count() - 1));

    {
        if (m_slice_timing_active) {
            auto end_time    = std::chrono::steady_clock::now();
            auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - m_slice_start_time).count();
            auto timess      = duration_ms;
            if (evt.cancelled()) {
                BOOST_LOG_TRIVIAL(info) << "Slicing cancelled after " << duration_ms << " ms";
                m_slice_start_time    = {};
                m_slice_timing_active = false;
            } else if (evt.error())
            {
                m_slice_start_time    = {};
                m_slice_timing_active = false; 
            }
            else if (is_finished && evt.finished())
            {
                //auto strTime = get_works_time(duration_ms);
                //auto slice_time = BP_SLICE_DURATION_TIME + std::string(":") + strTime;
                //sentryReportLog(SENTRY_LOG_TRACE, slice_time, BP_SLICE_DURATION);

                m_slice_start_time    = {};
                m_slice_timing_active = false; 
            }
        }
    }
    //BBS: slice .gcode.3mf file related logic, assign is_finished again
    bool only_has_gcode_need_preview = false;
    auto plate_list = m_partplate_list.get_plate_list();
    bool has_print_instances = false;
    for (auto plate : plate_list)
        has_print_instances = has_print_instances || plate->has_printable_instances();
    if (m_model.objects.empty() && !has_print_instances)
        only_has_gcode_need_preview = true;
    if (only_has_gcode_need_preview && m_slice_all_only_has_gcode) {
        is_finished = (m_cur_slice_plate == (m_partplate_list.get_plate_count() - 1));
        if (is_finished)
            m_slice_all_only_has_gcode = false;
    }

    // Stop the background task, wait until the thread goes into the "Idle" state.
    // At this point of time the thread should be either finished or canceled,
    // so the following call just confirms, that the produced data were consumed.
    m_background_process.stop();
    if (m_slice_timing_active && !m_background_process.running()) 
    {
        if (evt.cancelled() || evt.error()) {
            m_slice_start_time    = {};
            m_slice_timing_active = false;
        }
    }
    m_hub.nm()->set_slicing_progress_export_possible();

    // Reset the "export G-code path" name, so that the automatic background processing will be enabled again.
    m_background_process.reset_export();
    // This bool stops showing export finished notification even when process_completed_with_error is false
    bool has_error = false;
    if (evt.error()) {
        auto message = evt.format_error_message();
        if (evt.critical_error()) {
            if (q->get_tracking_popup_menu()) {
                // We don't want to pop-up a message box when tracking a pop-up menu.
                // We postpone the error message instead.
                q->set_tracking_popup_menu_error_message(message.first);
            } else {
                show_error(q, message.first, message.second.size() != 0 && message.second[0] != 0);
                m_hub.nm()->set_slicing_progress_hidden();
            }
        } else {
            std::vector<const ModelObject *> ptrs;
            for (auto oid : message.second)
            {
                const PrintObject *print_object = m_background_process.fff_print()->get_object(ObjectID(oid));
                if (print_object) { ptrs.push_back(print_object->model_object()); }
            }
            m_hub.nm()->push_slicing_error_notification(message.first, ptrs);
        }
        if (evt.invalidate_plater())
        {
            // BBS
#if 0
            const wxString invalid_str = _L("Invalid data");
            for (auto btn : { ActionButtonType::abReslice, ActionButtonType::abSendGCode, ActionButtonType::abExport })
                sidebar->set_btn_label(btn, invalid_str);
#endif
            q->set_process_completed_with_error(m_partplate_list.get_curr_plate_index());;
        }
        has_error = true;
        is_finished = true;
        SNAP_LOG_BATCH(Error, "slice failed", {"eventName","slice_failed"}, {"reason", evt.format_error_message().first});
    }
    if (evt.cancelled()) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", cancel event, status: %1%") % evt.status();
        m_hub.nm()->set_slicing_progress_canceled(_u8L("Slicing Canceled"));
        is_finished = true;
    }

    //BBS: set the current plater's slice result to valid
    if (!m_background_process.empty())
        m_background_process.get_current_plate()->update_slice_result_valid_state(evt.success());

    //BBS: update the action button according to the current plate's status
    bool ready_to_slice = !m_partplate_list.get_curr_plate()->is_slice_result_valid();

    // BBS
#if 0
    this->sidebar->show_sliced_info_sizer(evt.success());
#endif

    // This updates the "Slice now", "Export G-code", "Arrange" buttons status.
    // Namely, it refreshes the "Out of print bed" property of all the ModelObjects, and it enables
    // the "Slice now" and "Export G-code" buttons based on their "out of bed" status.
    //BBS: remove this update here, will be updated in update_fff_scene later
    //this->object_list_changed();

    // refresh m_layout->preview
    if (m_layout->view3D->is_dragging()) // updating scene now would interfere with the gizmo dragging
        m_refresh->set_delayed_scene_refresh(true);
    else {
        if (m_session->printer_technology == ptFFF) {
            if (is_finished)
                m_refresh->update_fff_scene();
        }
        else
            m_refresh->update_sla_scene();
    }

    //BBS: add slice&&print status update logic
    if (evt.cancelled()) {
        /*if (wxGetApp().get_mode() == comSimple)
            sidebar->set_btn_label(ActionButtonType::abReslice, "Slice now");
        show_action_buttons(true);*/
        ready_to_slice = true;
        //m_main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, true, true);

        //BBS
        if (m_ex->is_publishing()) {
            m_ex->publishing_cancel();
        }
    } else {
        if((ready_to_slice) || (wxGetApp().get_mode() == comSimple)) {
            //this means the current plate is not the slicing plate
            //show_action_buttons(ready_to_slice);
            //m_main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, ready_to_slice, true);
        }
        if (!m_ex->exporting_active() && !has_error) {
            m_hub.nm()->stop_delayed_notifications_of_type(NotificationType::ExportOngoing);
            m_hub.nm()->close_notification_of_type(NotificationType::ExportOngoing);
        }
        // If writing to removable drive was scheduled, show notification with eject button
        if (m_ex->exporting_to_removable() && !has_error) {
            //show_action_buttons(ready_to_slice);
            m_main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, ready_to_slice, true);
            m_hub.nm()->push_exporting_finished_notification(m_ex->get_last_output_path(), m_ex->get_last_output_dir_path(),
                // Don't offer the "Eject" button on ChromeOS, the Linux side has no control over it.
                (platform_flavor() != PlatformFlavor::LinuxOnChromium));
            q->removable_drive_manager()->set_exporting_finished(true);
        }else
        if (m_ex->exporting_to_local() && !has_error)
            m_hub.nm()->push_exporting_finished_notification(m_ex->get_last_output_path(), m_ex->get_last_output_dir_path(), false);

        // BBS, Generate calibration thumbnail for current plate
        if (!has_error && m_layout->preview) {
            // generate calibration data
            /* BBS generate calibration data by printer
            m_layout->preview->reload_print();
            ThumbnailData* calibration_data = &m_partplate_list.get_curr_plate()->cali_thumbnail_data;
            const ThumbnailsParams calibration_params = { {}, false, true, true, true, m_partplate_list.get_curr_plate_index() };
            generate_calibration_thumbnail(*calibration_data, PartPlate::cali_thumbnail_width, PartPlate::cali_thumbnail_height, calibration_params);
            m_layout->preview->get_canvas3d()->m_refresh->reset_gcode_toolpaths();*/

            // generate bbox data
            PlateBBoxData* plate_bbox_data = &m_partplate_list.get_curr_plate()->cali_bboxes_data;
            *plate_bbox_data = generate_first_layer_bbox();
        }
    }

    m_ex->set_not_exporting();


    // BBS stop publishing if error occur
    //if (m_ex->is_publishing()) {
    //    GCodeProcessorResult *gcode_result = m_background_process.get_current_gcode_result();
    //    m_publish_dlg->UpdateStatus(_L("Error occurred during slicing"), -1, false);
    //    // if toolpath is outside
    //    if (!gcode_result || gcode_result->toolpath_outside) {
    //        m_ex->set_is_publishing(false);
    //    }
    //}


    if (is_finished)
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":finished, reload print soon");
        // success (not when is_finished was forced true by error/cancel above).
        if (!has_error && !evt.cancelled() && evt.success()) {
            SNAP_LOG_BATCH(Info, "slice completed", {"eventName","slice_completed"});
        }
        m_slice_all = false;
        m_is_slicing = false;
        // Refresh the button right after m_is_slicing clears: in the cancelled branch
        // above, get_enable_slice_status() still sees m_is_slicing == true and would
        // re-disable the button instead of re-enabling it.
        if (evt.cancelled()) {
            this->m_main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, true, true);
        }
        m_layout->preview->reload_print(false);
        /* BBS if in publishing progress */
        if (m_ex->is_publishing()) {
            if (m_ex->publishing_dlg_active()) {
                if (m_ex->publish_dialog_shown()) {
                    m_ex->publish_project();
                } else {
                    m_ex->set_is_publishing(false);
                }
            }
        }
        q->set_plater_drop_target();
    }
    else
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":slicing all, plate %1% finished, start next slice...")%m_cur_slice_plate;
        m_cur_slice_plate = q->find_next_sliceable_plate_for_slice_all(m_cur_slice_plate + 1);
        if (m_cur_slice_plate < 0)
        {
            m_is_slicing = false;
            return;
        }

        q->Freeze();
        q->select_plate(m_cur_slice_plate);
        m_partplate_list.select_plate_view();
        int ret = q->start_next_slice();
        if (ret) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":slicing all, plate %1% can not be sliced, will stop")%m_cur_slice_plate;
            m_is_slicing = false;
        }
        //not the last plate
        m_refresh->update_fff_scene_only_shells(true);
        q->Thaw();
        if (m_ex->is_publishing()) {
            if (m_ex->publishing_dlg_active()) {
                wxString msg = wxString::Format(_L("Slicing Plate %d"), m_cur_slice_plate + 1);
                int percent  = 70 * m_cur_slice_plate / m_partplate_list.get_plate_count();
                m_ex->publishing_update_status(msg, percent);
            }
        }
    }
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(", exit.");
}

bool SlicingController::guard_before_slice_plate()
{
    return m_gate_keeper->guard_before_slice_plate();
}

void SlicingController::on_action_slice_plate() {
    if (true) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received slice plate event\n" ;
        //BBS update extruder params and speed table before slicing
        const Slic3r::DynamicPrintConfig& config = q->preset_bundle()->full_config();
        auto& print = m_partplate_list.get_current_fff_print();
        auto print_config = print.config();
        int numExtruders = (int)q->preset_bundle()->filament_presets.size();

        Model::setExtruderParams(config, numExtruders);
        Model::setPrintSpeedTable(config, print_config);
        m_slice_all = false;

        if (!m_gate_keeper->guard_before_slice_plate())
            return;

        // Slice first, then switch to m_layout->preview regardless of cancel,
        // so the user stays on the m_layout->preview page.
        bool slice_cancelled = !reslice();
        if (slice_cancelled) {
            // User chose "No" in the memory warning dialog.
            // Load only shells (no gcode toolpaths) to avoid OOM.
            m_refresh->update_fff_scene_only_shells(true);
        }
        q->select_view_3D("Preview");
    }
}

void SlicingController::on_action_slice_all() {
    if (true) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received slice project event\n" ;
        //BBS update extruder params and speed table before slicing
        const Slic3r::DynamicPrintConfig& config = q->preset_bundle()->full_config();
        auto& print = m_partplate_list.get_current_fff_print();
        auto print_config = print.config();
        int numExtruders = (int)q->preset_bundle()->filament_presets.size();

        Model::setExtruderParams(config, numExtruders);
        Model::setPrintSpeedTable(config, print_config);

        if (!m_gate_keeper->guard_before_slice_all())
            return;

        m_slice_all = true;
        m_slice_all_only_has_gcode = true;
        m_cur_slice_plate = q->find_next_sliceable_plate_for_slice_all(0);
        if (m_cur_slice_plate < 0)
        {
            m_slice_all = false;
            return;
        }
        //select plate
        q->select_plate(m_cur_slice_plate);
        bool slice_cancelled = !reslice();
        if (slice_cancelled) {
            m_slice_all = false;
            m_slice_all_only_has_gcode = false;
        }
        //BBS: wish to select all plates stats item
        if (!m_ex->is_publishing())
            q->select_view_3D("Preview");
        if (!slice_cancelled)
            m_layout->preview->get_canvas3d()->_update_select_plate_toolbar_stats_item(true);
    }
}

int SlicingController::update_print_required_data(Slic3r::DynamicPrintConfig config, Slic3r::Model model, Slic3r::PlateDataPtrs plate_data_list, std::string file_name, std::string file_path) {
    return m_ex->select_machine_dlg()->update_print_required_data(config, model, plate_data_list, file_name, file_path);
}

void SlicingController::handle_slice_button_status(bool enable) {
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ": enable = "<<enable<<"\n";
    if (!m_background_process.running())
        m_main_frame->update_slice_print_status(MainFrame::eEventObjectUpdate, enable);
}

void SlicingController::generate_thumbnail(ThumbnailData& data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params, int camera_type, bool use_top_view, bool for_picking, bool ban_light) {
    m_layout->view3D->get_canvas3d()->render_thumbnail(data, w, h, thumbnail_params, (Camera::EType)camera_type, use_top_view, for_picking, ban_light);
}

std::vector<ThumbnailData> SlicingController::generate_thumbnails(const ThumbnailsParams& params, int camera_type) {
    std::vector<ThumbnailData> thumbnails;
    for (const Vec2d& size : params.sizes) {
        thumbnails.push_back(ThumbnailData());
        Point isize(size); // round to ints
        generate_thumbnail(thumbnails.back(), isize.x(), isize.y(), params, camera_type);
        if (!thumbnails.back().is_valid())
            thumbnails.pop_back();
    }
    return thumbnails;
}

void SlicingController::generate_calibration_thumbnail(ThumbnailData& data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params) {
    m_layout->preview->get_canvas3d()->render_calibration_thumbnail(data, w, h, thumbnail_params);
}

PlateBBoxData SlicingController::generate_first_layer_bbox() {
    PlateBBoxData bboxdata;
    std::vector<BBoxData>& id_bboxes = bboxdata.bbox_objs;
    BoundingBoxf bbox_all;
    auto                   print = m_background_process.fff_print();
    auto curr_plate = m_partplate_list.get_curr_plate();
    auto curr_plate_seq = curr_plate->get_real_print_seq();
    bboxdata.is_seq_print = (curr_plate_seq == PrintSequence::ByObject);
    bboxdata.first_extruder = print->get_tool_ordering().first_extruder();
    bboxdata.bed_type       = bed_type_to_gcode_string(print->config().curr_bed_type.value);
    // get nozzle diameter
    auto opt_nozzle_diameters = print->config().option<ConfigOptionFloats>("nozzle_diameter");
    if (opt_nozzle_diameters != nullptr)
        bboxdata.nozzle_diameter = float(opt_nozzle_diameters->get_at(bboxdata.first_extruder));
    //PrintObjectPtrs objects;
    //if (m_session->printer_technology == ptFFF) {
    //    objects = m_background_process.fff_print()->objects().vector();
    //}
    //else {
    //    objects = m_background_process.sla_print()->objects();
    //}
    auto objects = print->objects();
    auto orig = m_partplate_list.get_curr_plate()->get_origin();
    Vec2d orig2d = { orig[0], orig[1] };

    BBoxData data;
    for (auto obj : objects)
    {
        auto bb_scaled = obj->get_first_layer_bbox(data.area, data.layer_height, data.name);
        auto bb = unscaled(bb_scaled);
        bb.min -= orig2d;
        bb.max -= orig2d;
        bbox_all.merge(bb);
        data.area *= (SCALING_FACTOR * SCALING_FACTOR); // unscale area
        data.id = obj->id().id;
        data.bbox = { bb.min.x(),bb.min.y(),bb.max.x(),bb.max.y() };
        id_bboxes.emplace_back(data);
    }

    // add wipe tower bounding box
    if (print->has_wipe_tower()) {
        auto   wt_corners = print->first_layer_wipe_tower_corners();
        // when loading gcode.3mf, wipe tower info may not be correct
        if (!wt_corners.empty()) {
            BoundingBox bb_scaled = {wt_corners[0], wt_corners[2]};
            auto        bb        = unscaled(bb_scaled);
            bb.min -= orig2d;
            bb.max -= orig2d;
            bbox_all.merge(bb);
            data.name = "wipe_tower";
            data.id   = m_partplate_list.get_curr_plate()->get_index() + 1000;
            data.bbox = {bb.min.x(), bb.min.y(), bb.max.x(), bb.max.y()};
            id_bboxes.emplace_back(data);
        }
    }

    bboxdata.bbox_all = { bbox_all.min.x(),bbox_all.min.y(),bbox_all.max.x(),bbox_all.max.y() };
    return bboxdata;
}

bool SlicingController::reslice() {
    m_partplate_list.set_filament_group_dirty(false);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: enter, process_completed_with_error=%2%")%__LINE__ %q->get_process_completed_with_error();
    // There is "invalid data" button instead "slice now"
    if (q->get_process_completed_with_error() == m_partplate_list.get_curr_plate_index())
    {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": process_completed_with_error, return directly");
        m_refresh->reset_gcode_toolpaths();
        return true;
    }

    // In case SLA gizmo is in editing mode, refuse to continue
    // and notify user that he should leave it first.
    if (m_layout->view3D->get_canvas3d()->get_gizmos_manager().is_in_editing_mode(true))
        return true;
    
    // Stop the running (and queued) UI jobs and only proceed if they actually
    // get stopped.
    unsigned timeout_ms = 10000;
    if (!stop_queue(m_worker, (unsigned)timeout_ms)) {
        BOOST_LOG_TRIVIAL(error) << "Could not stop UI job within "
                                 << timeout_ms << " milliseconds timeout!";
        return true;
    }

    // Orca: regenerate CalibPressureAdvancePattern custom G-code to apply changes
    if (m_model.calib_pa_pattern.get()) {
        m_calib->_calib_pa_pattern_gen_gcode();
    }

    if (m_session->printer_technology == ptSLA) {
        for (auto& object : m_model.objects)
            if (object->sla_points_status == sla::PointsStatus::NoPoints)
                object->sla_points_status = sla::PointsStatus::Generating;
    }

    //FIXME Don't reslice if export of G-code or sending to OctoPrint is running.
    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = update_background_process(true);
    if (state & UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE)
        m_layout->view3D->reload_scene(false);
    // If the SLA processing of just a single object's supports is running, restart slicing for the whole object.
    if (m_session->printer_technology == ptFFF && m_gate_keeper->has_incompatible_mixed_filament_in_use()) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": incompatible mixed filament in use, blocking slice";
        m_hub.nm()->push_notification(
            NotificationType::FilamentIncompatibleMixed,
            NotificationManager::NotificationLevel::ErrorNotificationLevel,
            into_u8(_L("Mixed filaments contain incompatible material types. Please correct the mixed filaments settings before slicing.")));
        m_refresh->reset_gcode_toolpaths();
        return true;
    }

    // Runtime memory guard: register a callback that fires DURING slicing
    // when available physical memory drops below the threshold (PrintBase.hpp).
    // The guard checks every 500ms at the 138 throw_if_canceled() checkpoints.
    m_layout->preview->set_skip_toolpath_preview(false);
    if (m_session->printer_technology == ptFFF) {
        Print* print_ptr = m_background_process.fff_print();
        if (print_ptr) {
            print_ptr->set_memory_guard_callback([this]() -> bool {
                auto promise = std::make_shared<std::promise<bool>>();
                auto future  = promise->get_future();

                call_after_fn([this, promise]() {
                    wxString msg = _L("Available system memory is critically low during slicing. "
                                      "Continuing may cause the application to freeze or crash.")
                        + "\n\n"
                        + _L("Do you want to continue slicing?")
                        + "\n\n"
                        + "\n- "
                        + _L("Select \"Yes\" to attempt slicing, but the software may lag or freeze.")
                        + "\n- "
                        + _L("Select \"No\" to terminate the slicing task immediately.");
                    bool result = true;
                    if (q) {
                        RichMessageDialog dlg(q, msg,
                            _L("Memory Usage Warning"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
                        dlg.SetYesNoLabels(_L("Yes, Continue"), _L("No, Stop"));
                        result = (dlg.ShowModal() == wxID_YES);
                    }
                    if (result) {
                        // Skip toolpath m_layout->preview to reduce memory usage on
                        // the subsequent load_toolpaths / load_shells phase.
                        m_layout->preview->set_skip_toolpath_preview(true);
                    } else {
                        // User chose to cancel: aggressively free the partial
                        // slicing data to reclaim memory before the m_layout->preview
                        // page loads any rendering buffers.
                        m_layout->preview->set_skip_toolpath_preview(true);
                        GCodeProcessorResult* gcode_res = m_layout->preview->get_gcode_result();
                        if (gcode_res != nullptr) {
                            gcode_res->moves.clear();
                            gcode_res->moves.shrink_to_fit();
                        }
                    }
                    promise->set_value(result);
                });

                return future.get();
            });
        }
    }

    this->m_background_process.set_task(PrintBase::TaskParams());
    // Only restarts if the state is valid.
    //BBS: jusdge the result
    bool result = restart_background_process(state | UPDATE_BACKGROUND_PROCESS_FORCE_RESTART);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: restart background,state=%2%, result=%3%")%__LINE__%state %result;
    if ((state & UPDATE_BACKGROUND_PROCESS_INVALID) != 0)
    {
        //BBS: add logs
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": state %1% is UPDATE_BACKGROUND_PROCESS_INVALID, can not slice") % state;
        m_refresh->update_fff_scene_only_shells(true);
        return true;
    }

    if ((!result) && m_slice_all && (m_cur_slice_plate < (m_partplate_list.get_plate_count() - 1)))
    {
        //slice next
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": in slicing all, current plate %1% already sliced, skip to next") % m_cur_slice_plate ;
        SlicingProcessCompletedEvent evt(EVT_PROCESS_COMPLETED, 0,
            SlicingProcessCompletedEvent::Finished, nullptr);
        // Post the "complete" callback message, so that it will slice the next plate soon
        wxQueueEvent(q, evt.Clone());
        m_is_slicing = true;
        if (m_cur_slice_plate == 0)
            m_refresh->reset_gcode_toolpaths();
        return true;
    }

    if (result) {
        m_is_slicing = true;
    }

    bool clean_gcode_toolpaths = true;
    // BBS
    if (m_background_process.running())
    {
        //p->ready_to_slice = false;
        m_main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, false);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": background process is running, m_is_slicing is true");
    }
    else if (!m_background_process.empty() && !m_background_process.idle()) {
        //p->show_action_buttons(true);
        //p->ready_to_slice = true;
        m_main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, true);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": background process changes to not_idle, set ready_to_slice back to true");
    }
    else {
        //BBS: add reset logic for empty plate
        PartPlate * current_plate = m_background_process.get_current_plate();

        if (!current_plate->has_printable_instances()) {
            clean_gcode_toolpaths = true;
            current_plate->update_slice_result_valid_state(false);
        }
        else {
            clean_gcode_toolpaths = false;
            current_plate->update_slice_result_valid_state(true);
        }
        m_main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, false);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": background process in idle state, use previous result, clean_gcode_toolpaths=%1%")%clean_gcode_toolpaths;
    }

    if (clean_gcode_toolpaths)
        m_refresh->reset_gcode_toolpaths();

    m_layout->preview->reload_print(!clean_gcode_toolpaths);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": finished, started slicing for plate %1%") % m_partplate_list.get_curr_plate_index();

    q->record_slice_preset("slicing");
    return true;
}

int SlicingController::start_next_slice() {
    // Stop arrange and (or) optimize rotation tasks.
    //this->stop_jobs();

    if (m_gate_keeper->is_plate_blocked_by_filament_temp_mixing(m_partplate_list.get_curr_plate_index())
        || m_gate_keeper->is_plate_blocked_by_cold_plate(m_partplate_list.get_curr_plate_index()))
    {
        m_gate_keeper->sync_filament_temp_mixing_notification();
        m_gate_keeper->sync_cold_plate_notification();
        if (m_slice_all)
        {
            SlicingProcessCompletedEvent evt(EVT_PROCESS_COMPLETED, 0,
                    SlicingProcessCompletedEvent::Finished, nullptr);
            wxQueueEvent(q, evt.Clone());
            return 0;
        }
        q->set_process_completed_with_error(m_partplate_list.get_curr_plate_index());
        return -1;
    }

    // Second blocker: flow_ratio == 0. NOTE: this uses first-blocker
    // early-return - if temp_mixing above already blocked the plate, this
    // branch never runs and only the temp_mixing banner is shown. Blocking
    // itself still takes effect (single-plate returns -1; slice_all posts
    // Finished to skip the current plate). If both banners must be shown
    // simultaneously, hoist m_gate_keeper->sync_flow_ratio_zero_notification() into the
    // temp_mixing branch before its early return.
    if (m_gate_keeper->is_plate_blocked_by_flow_ratio_zero(m_partplate_list.get_curr_plate_index()))
    {
        m_gate_keeper->sync_flow_ratio_zero_notification();
        if (m_slice_all)
        {
            SlicingProcessCompletedEvent evt(EVT_PROCESS_COMPLETED, 0,
                    SlicingProcessCompletedEvent::Finished, nullptr);
            wxQueueEvent(q, evt.Clone());
            return 0;
        }
        q->set_process_completed_with_error(m_partplate_list.get_curr_plate_index());
        return -1;
    }

    //FIXME Don't reslice if export of G-code or sending to OctoPrint is running.
    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = update_background_process(true, false, false);
    if (state & UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE)
        m_layout->view3D->reload_scene(false);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": update_background_process returns %1%")%state;
    if (m_session->printer_technology == ptFFF && m_gate_keeper->has_incompatible_mixed_filament_in_use()) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": incompatible mixed filament in use, blocking slice";
        m_hub.nm()->push_notification(
            NotificationType::FilamentIncompatibleMixed,
            NotificationManager::NotificationLevel::ErrorNotificationLevel,
            into_u8(_L("Mixed filaments contain incompatible material types. Please correct the mixed filaments settings before slicing.")));

        return -1;
    }
    if (!m_partplate_list.get_curr_plate()->can_slice()) {
        q->set_process_completed_with_error(m_partplate_list.get_curr_plate_index());
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": found invalidated apply in update_background_process.");
        return -1;
    }

    // Only restarts if the state is valid.
    bool result = restart_background_process(state | UPDATE_BACKGROUND_PROCESS_FORCE_RESTART);
    if (!result)
    {
        //slice next
        SlicingProcessCompletedEvent evt(EVT_PROCESS_COMPLETED, 0,
                SlicingProcessCompletedEvent::Finished, nullptr);
        // Post the "complete" callback message, so that it will slice the next plate soon
        wxQueueEvent(q, evt.Clone());
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": restart_background_process returns %1%")%result;

    return 0;
}

bool SlicingController::is_export_gcode_scheduled() const {
    return m_background_process.is_export_scheduled();
}

void SlicingController::suppress_background_process(const bool stop_background_process) {
    if (stop_background_process)
        m_background_process_timer.Stop();

    m_suppressed_update = true;
}

void SlicingController::apply_background_progress() {
    PartPlate* part_plate = m_partplate_list.get_curr_plate();
    int plate_index = m_partplate_list.get_curr_plate_index();
    bool result_valid = part_plate->is_slice_result_valid();
    //always apply the current plate's print
    Print::ApplyStatus invalidated = m_background_process.apply(m_model, q->preset_bundle()->full_config());
    q->notify_filament_compatibility_after_apply();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: plate %2%, after apply, invalidated= %3%, previous result_valid %4% ") % __LINE__ % plate_index % invalidated % result_valid;
    if (invalidated & PrintBase::APPLY_STATUS_INVALIDATED)
    {
        part_plate->update_slice_result_valid_state(false);
        //p->ready_to_slice = true;
        if (can_current_plate_be_sliced())
            m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, true);
        else
            m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false);
    }
}

int SlicingController::select_plate(int plate_index, bool need_slice) {
    int ret;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: plate %2%, need_slice %3% ")%__LINE__ %plate_index  %need_slice;
    m_snapshots.take_snapshot("select partplate!");
    ret = m_partplate_list.select_plate(plate_index);
    if (!ret) {
        if (m_layout->is_view3D_shown())
            wxGetApp().plater()->canvas3D()->render();
    }

    if ((!ret) && (m_background_process.can_switch_print()))
    {
        //select successfully
        m_partplate_list.update_slice_context_to_current_plate(m_background_process);
        m_layout->preview->update_gcode_result(m_partplate_list.get_current_slice_result());
        m_refresh->update_print_volume_state();

        PartPlate* part_plate = m_partplate_list.get_curr_plate();
        bool result_valid = part_plate->is_slice_result_valid();
        PrintBase* print = nullptr;
        GCodeResult* gcode_result = nullptr;
        Print::ApplyStatus invalidated;

        part_plate->get_print(&print, &gcode_result, NULL);

        //always apply the current plate's print
        invalidated = m_background_process.apply(m_model, q->preset_bundle()->full_config());
        q->notify_filament_compatibility_after_apply();
        bool model_fits, validate_err;

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: plate %2%, after apply, invalidated= %3%, previous result_valid %4% ")%__LINE__ %plate_index  %invalidated %result_valid;
        if (result_valid)
        {
            if (m_layout->is_preview_shown())
            {
                if (need_slice) { //from m_layout->preview's thumbnail
                    if ((invalidated & PrintBase::APPLY_STATUS_INVALIDATED) || (gcode_result->moves.empty())){
                        if (invalidated & PrintBase::APPLY_STATUS_INVALIDATED)
                            part_plate->update_slice_result_valid_state(false);
                        q->set_process_completed_with_error(-1);
                        m_slice_all = false;
                        m_refresh->reset_gcode_toolpaths();
                        if (!m_gate_keeper->guard_before_slice_plate())
                            return ret;
                        if (!reslice())
                            return ret;
                    }
                    else {
                        validate_current_plate(model_fits, validate_err);
                        //just refresh_print
                        m_layout->preview->refresh_print();
                        m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false, true);
                    }
                }
                else {// from multiple slice's next
                    //do nothing
                }
            }
            else
            {
                validate_current_plate(model_fits, validate_err);
                if (invalidated & PrintBase::APPLY_STATUS_INVALIDATED)
                {
                    part_plate->update_slice_result_valid_state(false);
                    // BBS
                    //p->show_action_buttons(true);
                    //p->ready_to_slice = true;
                    if (can_current_plate_be_sliced())
                        m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, true);
                    else
                        m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false);
                }
                else
                {
                    // BBS
                    //p->show_action_buttons(false);
                    //p->ready_to_slice = false;
                    m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false);

                    m_layout->preview->refresh_print();
                }
            }
        }
        else
        {
            //check inside status
            //model_fits = m_layout->view3D->get_canvas3d()->check_volumes_outside_state() != ModelInstancePVS_Partly_Outside;
            //bool validate_err = false;
            validate_current_plate(model_fits, validate_err);
            if (model_fits && !validate_err) {
                q->set_process_completed_with_error(-1);
            }
            else {
                q->set_process_completed_with_error(m_partplate_list.get_curr_plate_index());
            }
            if (m_layout->is_preview_shown())
            {
                if (need_slice)
                {
                    //q->set_process_completed_with_error(-1);
                    m_slice_all = false;
                    m_refresh->reset_gcode_toolpaths();
                    if (model_fits && !validate_err)
                    {
                        if (!m_gate_keeper->guard_before_slice_plate())
                            return ret;
                        if (!reslice())
                            return ret;
                    }
                    else
                    {
                        m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false);
                        //sometimes the previous print's sliced result is still valid, but the newly added object is laid over the boundary
                        //then the print toolpath will be shown, so we should not refresh print here, only onload shell
                        //m_layout->preview->refresh_print();
                        m_refresh->update_fff_scene_only_shells(true);
                    }
                }
                else {
                    //p->ready_to_slice = false;
                    m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false);
                    m_layout->preview->refresh_print();
                }
            }
            else
            {
                //validate_current_plate(model_fits, validate_err);
                //check inside status
                /*if (model_fits && !validate_err){
                    q->set_process_completed_with_error(-1);
                }
                else {
                    q->set_process_completed_with_error(m_partplate_list.get_curr_plate_index());
                }*/

                // BBS: don't show action buttons
                //p->show_action_buttons(true);
                //p->ready_to_slice = true;
                if (model_fits && part_plate->has_printable_instances()
                    && can_current_plate_be_sliced())
                {
                    //m_layout->view3D->get_canvas3d()->post_event(Event<bool>(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, true));
                    m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, true);
                }
                else
                {
                    //m_layout->view3D->get_canvas3d()->post_event(Event<bool>(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, false));
                    m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false);
                }
            }
        }
    }

    SimpleEvent event(EVT_GLCANVAS_PLATE_SELECT);
    { SimpleEvent e(EVT_GLCANVAS_PLATE_SELECT); m_plates.on_plate_selected(); }
    m_gate_keeper->sync_filament_temp_mixing_notification();
    m_gate_keeper->sync_flow_ratio_zero_notification();
    m_gate_keeper->sync_cold_plate_notification();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: plate %2%, return %3%")%__LINE__ %plate_index %ret;
    return ret;
}

int SlicingController::select_sliced_plate(int plate_index) {
    int ret = 0;
    BOOST_LOG_TRIVIAL(info) << "select_sliced_plate plate_idx=" << plate_index;

    if (GUI::FlowType::any_nozzle_high_flow()) {
        if (m_partplate_list.is_filament_group_dirty()) {
            bool all_high_flow = GUI::FlowType::distinct_nozzle_flow_type_count() < 2;
            GUI::FilamentGroupDialog dlg(q, all_high_flow);
            if (dlg.ShowModal() != wxID_OK)
                return 0;
        }
    } else {
        GUI::FlowType::sync_filament_volume_types_for_slice();
    }

    q->Freeze();
    ret = select_plate(plate_index, true);
    if (ret)
    {
        BOOST_LOG_TRIVIAL(error) << "select_plate error for plate_idx=" << plate_index;
        q->Thaw();
        return -1;
    }
    m_partplate_list.select_plate_view();
    q->Thaw();

    return ret;
}

void SlicingController::validate_current_plate(bool& model_fits, bool& validate_error) {
    model_fits = m_layout->view3D->get_canvas3d()->check_volumes_outside_state() != ModelInstancePVS_Partly_Outside;
    validate_error = false;
    if (m_session->printer_technology == ptFFF) {
        std::string plater_text = _u8L("An object is laid over the boundary of plate or exceeds the height limit.\n"
                    "Please solve the problem by moving it totally on or off the plate, and confirming that the height is within the build volume.");;
        StringObjectException warning;
        Polygons polygons;
        std::vector<std::pair<Polygon, float>> height_polygons;
        StringObjectException err = m_background_process.validate(&warning, &polygons, &height_polygons);
        // update string by type
        q->post_process_string_object_exception(err);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": validate err=%1%, warning=%2%, model_fits %3%")%err.string%warning.string %model_fits;

        if (err.string.empty()) {
            // Print::validate() passed, but GUI-layer blockers (filament temp
            // mixing, cold plate incompatibility) may still be active. Their
            // ValidateError notifications must survive this close-all — gate it.
            const int curr_plate_idx = m_partplate_list.get_curr_plate_index();
            const bool blocked_by_mixing = m_gate_keeper->is_plate_blocked_by_filament_temp_mixing(curr_plate_idx);
            const bool blocked_by_cold    = m_gate_keeper->is_plate_blocked_by_cold_plate(curr_plate_idx);

            m_partplate_list.get_curr_plate()->update_apply_result_invalid(false);
            m_hub.nm()->set_all_slicing_errors_gray(true);
            if (!blocked_by_mixing && !blocked_by_cold) {
                m_hub.nm()->close_notification_of_type(NotificationType::ValidateError);
            }

            // Pass a warning from validation and either show a notification,
            // or hide the old one.
            m_hub.process_validation_warning(warning);
            m_layout->view3D->get_canvas3d()->reset_sequential_print_clearance();
            m_layout->view3D->get_canvas3d()->set_as_dirty();
            m_layout->view3D->get_canvas3d()->request_extra_frame();
        }
        else {
            // The print is not valid.
            m_partplate_list.get_curr_plate()->update_apply_result_invalid(true);
            // Show error as notification.
            m_hub.nm()->push_validate_error_notification(err);
            m_hub.process_validation_warning(warning);
            //model_fits = false;
            validate_error = true;
            m_layout->view3D->get_canvas3d()->set_sequential_print_clearance_visible(true);
            m_layout->view3D->get_canvas3d()->set_sequential_print_clearance_render_fill(true);
            m_layout->view3D->get_canvas3d()->set_sequential_print_clearance_polygons(polygons, height_polygons);
        }

        if (!model_fits) {
            m_hub.nm()->push_plater_error_notification(plater_text);
        }
        else {
            m_hub.nm()->close_plater_error_notification(plater_text);
        }

        m_gate_keeper->sync_filament_temp_mixing_notification();
        m_gate_keeper->sync_flow_ratio_zero_notification();
        m_gate_keeper->sync_cold_plate_notification();
    }

    PartPlate* part_plate = m_partplate_list.get_curr_plate();
    part_plate->update_slice_ready_status(model_fits);

    return;
}

int SlicingController::select_plate_by_hover_id(int hover_id, bool right_click, bool isModidyPlateName) {
    int ret;
    int action, plate_index;

    plate_index = hover_id / PartPlate::GRABBER_COUNT;
    action      = isModidyPlateName ? PartPlate::PLATE_NAME_HOVER_ID : hover_id % PartPlate::GRABBER_COUNT;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": enter, hover_id %1%, plate_index %2%, action %3%")%hover_id % plate_index %action;
    if (action == 0)
    {
        //select plate
        ret = m_partplate_list.select_plate(plate_index);
        if (!ret) {
            SimpleEvent event(EVT_GLCANVAS_PLATE_SELECT);
            { SimpleEvent e(EVT_GLCANVAS_PLATE_SELECT); m_plates.on_plate_selected(); }
        }
        if ((!ret)&&(m_background_process.can_switch_print()))
        {
            //select successfully
            m_partplate_list.update_slice_context_to_current_plate(m_background_process);
            m_layout->preview->update_gcode_result(m_partplate_list.get_current_slice_result());
            m_refresh->update_print_volume_state();

            PartPlate* part_plate = m_partplate_list.get_curr_plate();
            bool result_valid = part_plate->is_slice_result_valid();
            PrintBase* print = nullptr;
            GCodeResult* gcode_result = nullptr;
            Print::ApplyStatus invalidated;

            part_plate->get_print(&print, &gcode_result, NULL);
            //always apply the current plate's print
            invalidated = m_background_process.apply(m_model, q->preset_bundle()->full_config());
            q->notify_filament_compatibility_after_apply();
            bool model_fits, validate_err;
            validate_current_plate(model_fits, validate_err);

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: after apply, invalidated= %2%, previous result_valid %3% ")%__LINE__ % invalidated %result_valid;
            if (result_valid)
            {
                if (invalidated & PrintBase::APPLY_STATUS_INVALIDATED)
                {
                    //bool model_fits, validate_err;
                    //validate_current_plate(model_fits, validate_err);
                    part_plate->update_slice_result_valid_state(false);

                    // BBS
                    //p->show_action_buttons(true);
                    //p->ready_to_slice = true;
                    if (can_current_plate_be_sliced())
                        m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, true);
                    else
                        m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false);
                }
                else
                {
                    // BBS
                    //p->show_action_buttons(false);
                    //validate_current_plate(model_fits, validate_err);
                    //p->ready_to_slice = false;
                    m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false);

                    m_layout->preview->refresh_print();
                }
            }
            else
            {
                //check inside status
                if (model_fits && !validate_err){
                    q->set_process_completed_with_error(-1);
                }
                else {
                    q->set_process_completed_with_error(m_partplate_list.get_curr_plate_index());
                }

                // BBS: don't show action buttons
                //p->show_action_buttons(true);
                //p->ready_to_slice = true;
                if (model_fits && part_plate->has_printable_instances()
                    && can_current_plate_be_sliced())
                {
                    //m_layout->view3D->get_canvas3d()->post_event(Event<bool>(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, true));
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": will set can_slice to true");
                    m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, true);
                }
                else
                {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": will set can_slice to false, has_printable_instances %1%")%part_plate->has_printable_instances();
                    //m_layout->view3D->get_canvas3d()->post_event(Event<bool>(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, false));
                    m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false);
                }
            }
        }
    }
    else if ((action == 1)&&(!right_click))
    {
        //delete plate
        ret = q->delete_plate(plate_index);
    }
    else if ((action == 2)&&(!right_click))
    {
        //arrange the plate
        //m_snapshots.take_snapshot("select_orient partplate");
        ret = q->select_plate(plate_index);
        if (!ret)
        {
            m_arrange->orient(OrientRequest::current_plate());   // F3：写点+调用合并为一次 Request 调用（hover action 2）
        }
        else
        {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "can not select plate %1%" << plate_index;
            ret = -1;
        }
    }
    else if ((action == 3)&&(!right_click))
    {
        //arrange the plate
        //m_snapshots.take_snapshot("select_arrange partplate");
        ret = q->select_plate(plate_index);
        if (!ret)
        {
            if (m_arrange->claim_arrange_running()) {
                m_arrange->arrange(ArrangeRequest::current_plate());   // F3：写点+调用合并（hover action 3，claim 之后）
            }
        }
        else
        {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "can not select plate %1%" << plate_index;
            ret = -1;
        }
    }
    else if ((action == 4)&&(!right_click))
    {
        //lock the plate
        m_snapshots.take_snapshot("lock partplate");
        ret = m_partplate_list.lock_plate(plate_index, !m_partplate_list.is_locked(plate_index));
    }
    else if ((action == 5)&&(!right_click))
    {
        // set the plate type
        ret = q->select_plate(plate_index);
        if (!ret) {
            { wxCommandEvent evt(EVT_OPEN_PLATESETTINGSDIALOG); evt.SetInt(plate_index); evt.SetEventObject(q); wxPostEvent(q, evt); }

            this->schedule_background_process();
        } else {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "can not select plate %1%" << plate_index;
            ret = -1;
        }
    }
    else if ((action == 6) && (!right_click)) {
        // set the plate type
        ret = q->select_plate(plate_index);
        if (!ret) {
            PlateNameEditDialog dlg(q, wxID_ANY, _L("Edit Plate Name"));
            PartPlate *         curr_plate = m_partplate_list.get_curr_plate();

            wxString curr_plate_name = from_u8(curr_plate->get_plate_name());
            dlg.set_plate_name(curr_plate_name);

            int result=dlg.ShowModal();
            if (result == wxID_YES) {
                wxString dlg_plate_name = dlg.get_plate_name();
                curr_plate->set_plate_name(dlg_plate_name.ToUTF8().data());
            }
        } else {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "can not select plate %1%" << plate_index;
            ret = -1;
        }
    } else if ((action == 7) && (!right_click)) {
        // move plate to the front
        m_snapshots.take_snapshot("move plate to the front");
        ret = m_partplate_list.move_plate_to_index(plate_index,0);
        m_partplate_list.update_slice_context_to_current_plate(m_background_process);
        m_layout->preview->update_gcode_result(m_partplate_list.get_current_slice_result());
        m_layout->sidebar->obj_list()->reload_all_plates();
        m_partplate_list.update_plates();
        m_refresh->update(0);
        m_partplate_list.select_plate(0);
        m_hub.sync_print_seq_warning_notification();
    }

    else
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "invalid action %1%, with right_click=%2%" << action << right_click;
        ret = -1;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: return %2%")%__LINE__ % ret;
    return ret;
}

void SlicingController::update_slicing_context_to_current_partplate() {
    m_partplate_list.update_slice_context_to_current_plate(m_background_process);
    m_layout->preview->update_gcode_result(m_partplate_list.get_current_slice_result());
}

void SlicingController::post_slice_state_change_update() {
    m_partplate_list.invalid_all_slice_result();
    m_refresh->reset_gcode_toolpaths();
    m_main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, true, false);
    call_after_fn([this]() {
        GLCanvas3D* canvas = m_layout->get_current_canvas3D();
        if (canvas) {
            canvas->set_as_dirty();
            canvas->request_extra_frame();
        }
    });
}


} // namespace app
} // namespace GUI
} // namespace Slic3r
