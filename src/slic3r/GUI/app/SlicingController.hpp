// SlicingController — Plater 切片域服务（阶段 D2 纯搬移，手册《Plater阶段D2-切片SlicingController抽取实施方案》）。
// hpp：wx 仅以指针/前向声明出现；依赖规则见 AGENTS.md（禁 include Plater/GUI_App 宿主头）。
// Host 回调中标注（E/G/B 过渡）者，在对应服务落地后改接新服务——接口不变。
#pragma once

// 阶段 H1（勘误 13）：头自包含化——windows 前置保护 BSP→PrintHost→asio 链（CP_ACP），
// 使任意 TU 任意顺序包含本头均安全（ExportSendCoordinator.hpp 同款前置先例）。
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winnls.h>
#endif

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <chrono>

#include <wx/timer.h>
#include <wx/string.h>                  // wxString（publishing_update_status 签名）                  // wxTimer（background_process_timer 随迁）

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"  // ThumbnailData/ThumbnailsParams/PlateBBoxData（Slic3r 层）   // PrinterTechnology / DynamicPrintConfig
#include "libslic3r/Print.hpp"         // Print（按值成员需完整类型）
#include "libslic3r/SLAPrint.hpp"      // SLAPrint
#include "libslic3r/GCode/GCodeProcessor.hpp"  // GCodeProcessorResult
#include "../BackgroundSlicingProcess.hpp"     // BackgroundSlicingProcess（随迁成员；含事件类型）
#include "../../Utils/SnapLogClient.hpp"   // SNAP_LOG_BATCH
#include "libslic3r/PrintBase.hpp"     // PrintBase / PrintStateBase::Warning / StringObjectException
#include "ArrangeRequest.hpp"          // F3：arrange/orient_facade 槽改型签名（app 内部头，纯值）

class wxWindow;
class wxEvent;
class wxCommandEvent;                 // wx 全局命名空间（勿置入 Slic3r::GUI——E13 同类遮蔽）

namespace Slic3r {
class CalibPressureAdvancePattern;
class Model;
class ModelObject;
class BackgroundSlicingProcess;
class SlicingStatusEvent;
class SlicingProcessCompletedEvent;      // 定义在 Slic3r（非 Slic3r::GUI）——D1 E13 教训
class GCodeProcessorResult;
class Print;
class SLAPrint;
namespace GUI {
extern const wxEventTypeTag<SlicingProcessCompletedEvent> EVT_PROCESS_COMPLETED;
class SelectMachineDialog;
class GLCanvas3D;
class PartPlateList;
class View3D;
class Preview;
class ObjectList;
class NotificationManager;
class Plater;
class MainFrame;
class Worker;

namespace app {
class ArrangementService;
class CalibrationFlow;
class ExportSendCoordinator;
class LayoutManager;
class MixingGateKeeper;
class NotificationHub;
class PlateController;
class ProjectSession;
class SceneRefreshCoordinator;
class SnapshotService;
// H1 2.3: SlicingControllerHost removed (111 anchors/110 lines; 13 dead slots + host_ready deleted per C.4)
// - F2 ctor inject: q/partplate_list/model/config/hub/snapshots/plates/worker/main_frame/fff_print/sla_print/gcode_result
// - late_bind(session/ex/arrange/gate_keeper/calib/layout/refresh); queue/freeze/thaw/show_error via q (wxWindow faces)
// - 13 dead slots: timer_owner/is_exporting_to_local/is_exporting_to_removable/is_not_exporting/set_exporting_status/
//   process_completed_event_type/update_print_required_data_dialog/get_mode/call_after_canvas_dirty/gui_is_recreating/
//   render_canvas3d/main_frame_update_slice_status_raw/view3d_is_dragging


class SlicingController
{
public:
    SlicingController(Plater* q, PartPlateList& partplate_list, Model& model, DynamicPrintConfig* config,
                      NotificationHub& hub, SnapshotService& snapshots, PlateController& plates,
                      Worker& worker, MainFrame* main_frame,
                      Print& fff_print, SLAPrint& sla_print, GCodeProcessorResult& gcode_result);
    void late_bind(ProjectSession& session, ExportSendCoordinator& ex, ArrangementService& arrange,
                   MixingGateKeeper& gate_keeper, CalibrationFlow& calib, LayoutManager& layout,
                   SceneRefreshCoordinator& refresh);   // H1 2.3

    // —— 所有权成员访问器（priv 过渡期/G 前使用）——
    BackgroundSlicingProcess& background_process() { return m_background_process; }
    Print&        fff_print()            { return m_fff_print; }
    SLAPrint&     sla_print()            { return m_sla_print; }
    GCodeProcessorResult& gcode_result() { return m_gcode_result; }
    bool  is_slicing() const              { return m_is_slicing; }
    bool  slice_all() const               { return m_slice_all; }
    int   cur_slice_plate() const         { return m_cur_slice_plate; }

    // —— 无 wx 决策核心（单测面）——
    bool can_current_plate_be_sliced() const;
    bool guard_before_slice_plate();                  // 阶段 I：页面切换自动切片复用同一前置守卫

    // —— 调度/状态机 ——
    void schedule_background_process();
    void suppress_background_process(const bool stop_background_process);
    void set_suppress_update(bool v)              { m_suppressed_update = v; }
    bool suppress_update_active() const           { return m_suppressed_update; }
    bool update_timer_running() const             { return m_background_process_timer.IsRunning(); }
    void start_update_timer()                     { m_background_process_timer.Start(500, wxTIMER_ONE_SHOT); }
    void stop_update_timer()                      { m_background_process_timer.Stop(); }
    void init_update_timer(wxWindow* owner)       { m_background_process_timer.SetOwner(owner, 0); }
    bool slice_all_only_has_gcode() const         { return m_slice_all_only_has_gcode; }
    void set_slice_all(bool v)                    { m_slice_all = v; }
    void set_slice_all_only_has_gcode(bool v)     { m_slice_all_only_has_gcode = v; }
    void set_cur_slice_plate(int v)               { m_cur_slice_plate = v; }
    void set_is_slicing(bool v)                   { m_is_slicing = v; }
    unsigned int update_background_process(bool force_validation = false, bool postpone_error_messages = false, bool switch_print = false);
    unsigned int update_restart_background_process(bool force_update_scene = false, bool force_update_preview = false);
    bool restart_background_process(unsigned int state);
    void apply_background_progress();
    bool reslice();
    int  start_next_slice();

    // —— 状态查询 ——
    bool is_background_process_slicing() const { return m_is_slicing; }
    bool is_slicing_in_progress() const        { return m_is_slicing || m_background_process.running(); }
    bool is_export_gcode_scheduled() const;
    void post_slice_state_change_update();

    // —— 事件逻辑（Bind 接线留守 priv，priv 转发壳调用）——
    void handle_slicing_update(SlicingStatusEvent &evt);
    void handle_slicing_completed(wxCommandEvent & evt);
    void handle_slicing_began();
    void handle_process_completed(SlicingProcessCompletedEvent &evt);
    void handle_export_began();
    void handle_export_finished();
    void handle_slice_button_status(bool enable);

    // —— 切片动作 ——
    void on_action_slice_plate();
    void on_action_slice_all();
    int  select_plate(int plate_index, bool need_slice);
    int  select_sliced_plate(int plate_index);
    int  select_plate_by_hover_id(int hover_id, bool right_click, bool isModidyPlateName);
    void validate_current_plate(bool& model_fits, bool& validate_error);
    void update_slicing_context_to_current_partplate();
    int  update_print_required_data(Slic3r::DynamicPrintConfig config, Slic3r::Model model, Slic3r::PlateDataPtrs plate_data_list, std::string file_name, std::string file_path);

    // —— 缩略图/首层框（渲染经 Host 视图）——
    void generate_thumbnail(class ThumbnailData& data, unsigned int w, unsigned int h, const class ThumbnailsParams& thumbnail_params, int camera_type, bool use_top_view = false, bool for_picking = false, bool ban_light = false);
    std::vector<ThumbnailData> generate_thumbnails(const class ThumbnailsParams& params, int camera_type);
    void generate_calibration_thumbnail(class ThumbnailData& data, unsigned int w, unsigned int h, const class ThumbnailsParams& thumbnail_params);
    PlateBBoxData generate_first_layer_bbox();

    void on_select_bed_type_reslice_tail();   // 原 on_select_bed_type 的 reslice 尾段（config 段留守 priv）

private:
    Plater*                     q{nullptr};
    PartPlateList&              m_partplate_list;
    Model&                      m_model;
    DynamicPrintConfig*         m_config{nullptr};
    NotificationHub&            m_hub;
    SnapshotService&            m_snapshots;
    PlateController&            m_plates;
    Worker&                     m_worker;
    MainFrame*                  m_main_frame{nullptr};
    Print&                      m_fff_print;
    SLAPrint&                   m_sla_print;
    GCodeProcessorResult&       m_gcode_result;
    ProjectSession*             m_session{nullptr};         // late_bind
    ExportSendCoordinator*      m_ex{nullptr};              // late_bind
    ArrangementService*         m_arrange{nullptr};         // late_bind
    MixingGateKeeper*           m_gate_keeper{nullptr};     // late_bind
    CalibrationFlow*            m_calib{nullptr};           // late_bind
    LayoutManager*              m_layout{nullptr};          // late_bind
    SceneRefreshCoordinator*    m_refresh{nullptr};         // late_bind
    void call_after_fn(std::function<void()> fn);           // 原 Host 槽转正（§5.1：cpp 定义）

    // —— 原 priv 13 成员 ——
    bool                 m_ignore_event{false};
    bool                 m_slice_all{false};
    bool                 m_is_slicing{false};
    bool                 m_slice_all_only_has_gcode{false};
    BackgroundSlicingProcess m_background_process;
    bool                 m_suppressed_update{false};       // ← suppressed_backround_processing_update
    int                  m_cur_slice_plate{0};
    std::chrono::steady_clock::time_point m_slice_start_time;
    bool                 m_slice_timing_active = false;
    wxTimer              m_background_process_timer;       // owner 经 Host SetOwner
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
