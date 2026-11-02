#ifndef slic3r_Plater_hpp_
#define slic3r_Plater_hpp_

#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <boost/filesystem/path.hpp>

#include <wx/panel.h>
// BBS
#include <wx/notebook.h>

#include "Selection.hpp"
#include "MixedColorMatchHelpers.hpp"

#include "libslic3r/enum_bitmask.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "Jobs/Job.hpp"
#include "Jobs/Worker.hpp"
#include "app/MixingTypes.hpp"   // 阶段 B3：门控类型上提（Plater 类内 using 别名保持公有 API 名）
#include "app/SnapshotService.hpp"   // 阶段 H2 1.1（§5.3 别名原形）：快照替身类完整型——hpp 禁服务头纪律的注册例外（无 wx 轻头）
#include "GUI_App.hpp"
#include "Sidebar.hpp"
#include "Jobs/PrintJob.hpp"
#include "Jobs/SendJob.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintBase.hpp"
#include "Camera.hpp"   // H2 3.1 顶部瘦身：Camera::EType（原经 PartPlate.hpp 传递链）
#include "libslic3r/Print.hpp"   // H2 3.1 顶部瘦身：m_fff_print 值成员（原经 PartPlate.hpp 传递链）
#include "libslic3r/SLAPrint.hpp"   // 阶段 H1（路径 A）：m_sla_print 值成员上收后消费 TU 需完整类型

#include "libslic3r/calib.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/FlushVolCalc.hpp"

#define FILAMENT_SYSTEM_COLORS_NUM      16

class wxButton;
class ScalableButton;
class wxScrolledWindow;
class wxString;
class ComboBox;
class Button;

namespace Slic3r {

class BuildVolume;
enum class BuildVolume_Type : unsigned char;
class Model;
class ModelObject;
class ModelInstance;
class Print;
class SLAPrint;
//BBS: add partplatelist and SlicingStatusEvent
class PartPlateList;
class SlicingStatusEvent;
enum SLAPrintObjectStep : unsigned int;
enum class ConversionType : int;
class Ams;

using ModelInstancePtrs = std::vector<ModelInstance*>;


namespace UndoRedo {
    class Stack;
    enum class SnapshotType : unsigned char;
    struct Snapshot;
}

namespace GUI {

namespace app {
class ArrangeRequest;
class OrientRequest;
class SnapshotService;
class NotificationHub;
class PlateController;
class SlicingController;
class ProjectSession;
class ExportSendCoordinator;
class SelectionQuery;
class ObjectEditCommands;
class ArrangementService;
class MixingGateKeeper;
class FilamentService;
class CalibrationFlow;
class LayoutManager;
class SceneRefreshCoordinator;
class PlaterEventRouter;
}   // F3/G1/G3/G5/H1：前置声明（hpp 禁 include 服务头——调用方 cpp 各自 include；H2 1.1 起唯一例外：
    // SnapshotService.hpp 直含——§5.3 别名原形所需完整型，无 wx 轻头）

class MainFrame;
class ConfigOptionsGroup;
class ObjectSettings;
class ObjectLayers;
class ObjectList;
class GLCanvas3D;
class GLTexture;
class Mouse3DController;
class NotificationManager;
class DailyTipsWindow;
struct Camera;
class GLToolbar;
class PlaterPresetComboBox;
class PartPlateList;
class MenuFactory;
class BoostThreadWorker;
class SLAImportDialog;
template <class T> class PlaterWorker;

using t_optgroups = std::vector <std::shared_ptr<ConfigOptionsGroup>>;

class Plater;

#define EVT_PUBLISHING_START        1
#define EVT_PUBLISHING_STOP         2

//BBS: add EVT_SLICING_UPDATE declare here
wxDECLARE_EVENT(EVT_SCHEDULE_BACKGROUND_PROCESS, SimpleEvent);
wxDECLARE_EVENT(EVT_SLICING_UPDATE, Slic3r::SlicingStatusEvent);
wxDECLARE_EVENT(EVT_PUBLISH,        wxCommandEvent);
wxDECLARE_EVENT(EVT_OPEN_PLATESETTINGSDIALOG,        wxCommandEvent);
wxDECLARE_EVENT(EVT_REPAIR_MODEL,        wxCommandEvent);
wxDECLARE_EVENT(EVT_FILAMENT_COLOR_CHANGED,        wxCommandEvent);
wxDECLARE_EVENT(EVT_INSTALL_PLUGIN_NETWORKING,        wxCommandEvent);
wxDECLARE_EVENT(EVT_INSTALL_PLUGIN_HINT,        wxCommandEvent);
wxDECLARE_EVENT(EVT_UPDATE_PLUGINS_WHEN_LAUNCH,        wxCommandEvent);
wxDECLARE_EVENT(EVT_PREVIEW_ONLY_MODE_HINT,        wxCommandEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_COLOR_MODE_CHANGED,   SimpleEvent);
wxDECLARE_EVENT(EVT_PRINT_FROM_SDCARD_VIEW,   SimpleEvent);
wxDECLARE_EVENT(EVT_CREATE_FILAMENT, SimpleEvent);
wxDECLARE_EVENT(EVT_MODIFY_FILAMENT, SimpleEvent);
wxDECLARE_EVENT(EVT_FILAMENT_USAGE_CHANGED, SimpleEvent);
wxDECLARE_EVENT(EVT_ADD_FILAMENT, SimpleEvent);
wxDECLARE_EVENT(EVT_DEL_FILAMENT, SimpleEvent);
using ColorEvent = Event<wxColour>;
wxDECLARE_EVENT(EVT_ADD_CUSTOM_FILAMENT, ColorEvent);
const wxString DEFAULT_PROJECT_NAME = "Untitled";


class Plater: public wxPanel
{
public:
    using fs_path = boost::filesystem::path;
    // 阶段 B3：门控类型定义迁 GUI/app/MixingTypes.hpp；以下别名保持 Plater::X 公有 API 名不变
    using FilamentTempMixingState = app::FilamentTempMixingState;
    using FilamentTempMixingDetail = app::FilamentTempMixingDetail;
    using PlateMixingInfo = app::PlateMixingInfo;
    using FlowRatioZeroDetail = app::FlowRatioZeroDetail;
    using ColdPlateCompatState = app::ColdPlateCompatState;
    using ColdPlateCompatResult = app::ColdPlateCompatResult;

    Plater(wxWindow *parent, MainFrame *main_frame);
    Plater(Plater &&) = delete;
    Plater(const Plater &) = delete;
    Plater &operator=(Plater &&) = delete;
    Plater &operator=(const Plater &) = delete;
    ~Plater();

    bool Show(bool show = true);

    void set_plater_dirty(bool is_dirty);
    void update_project_dirty_from_presets();
    void reset_project_dirty_initial_presets();
#if ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW
    void render_project_state_debug_window() const;
#endif // ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW

    Sidebar& sidebar();
    const Model& model() const;
    Model& model();
    const Print& fff_print() const;
    Print& fff_print();

    int new_project(bool skip_confirm = false, bool silent = false, const wxString& project_name = wxString());
    // BBS: save & backup
    //BBS download project by project id
    void import_model_id(wxString download_info);
    void request_model_download(wxString url);
    void request_download_project(std::string project_id);
    // BBS: check snapshot
    bool up_to_date(bool saved, bool backup);



    // SoftFever
    void calib_pa(const Calib_Params& params);
    void calib_temp(const Calib_Params& params);
    void calib_max_vol_speed(const Calib_Params& params);
    void calib_retraction(const Calib_Params& params);
    void calib_VFA(const Calib_Params& params);
    void calib_input_shaping_freq(const Calib_Params& params);
    void calib_input_shaping_damp(const Calib_Params& params);
    void calib_junction_deviation(const Calib_Params& params);

    BuildVolume_Type get_build_volume_type() const;

    //BBS: add only gcode mode
    bool only_gcode_mode();

    //BBS: add only gcode mode
    bool using_exported_file();

    // BBS
    wxString get_project_name();
    void update_all_plate_thumbnails(bool force_update = false);
    void invalid_all_plate_thumbnails();
    void force_update_all_plate_thumbnails();

    static wxColour get_next_color_for_filament();
    static wxString get_slice_warning_string(GCodeProcessorResult::SliceWarning& warning);


    // BBS: restore
    std::vector<size_t> load_files(const std::vector<boost::filesystem::path>& input_files, LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig,  bool ask_multi = false);
    // To be called when providing a list of files to the GUI slic3r on command line.
    std::vector<size_t> load_files(const std::vector<std::string>& input_files, LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig,  bool ask_multi = false);
    // to be called on drag and drop
    bool load_files(const wxArrayString& filenames);


    void update(bool conside_update_flag = false, bool force_background_processing_update = false);
    //BBS

    // Get the worker handling the UI jobs (arrange, fill bed, etc...)
    // Here is an example of starting up an ad-hoc job:
    //    queue_job(
    //        get_ui_job_worker(),
    //        [](Job::Ctl &ctl) {
    //            // Executed in the worker thread
    //
    //            CursorSetterRAII cursor_setter{ctl};
    //            std::string msg = "Running";
    //
    //            ctl.update_status(0, msg);
    //            for (int i = 0; i < 100; i++) {
    //                usleep(100000);
    //                if (ctl.was_canceled()) break;
    //                ctl.update_status(i + 1, msg);
    //            }
    //            ctl.update_status(100, msg);
    //        },
    //        [](bool, std::exception_ptr &e) {
    //            // Executed in UI thread after the work is done
    //
    //            try {
    //                if (e) std::rethrow_exception(e);
    //            } catch (std::exception &e) {
    //                BOOST_LOG_TRIVIAL(error) << e.what();
    //            }
    //            e = nullptr;
    //        });
    // This would result in quick run of the progress indicator notification
    // from 0 to 100. Use replace_job() instead of queue_job() to cancel all
    // pending jobs.
    Worker& get_ui_job_worker();
    const Worker & get_ui_job_worker() const;

    //BBS: add no_slice logic
    void select_view_3D(const std::string& name, bool no_slice = true);

    void reload_paint_after_background_process_apply();
    bool is_preview_shown() const;
    bool is_view3D_shown() const;



    void enable_sidebar(bool enabled);
    bool is_sidebar_collapsed() const;
    void collapse_sidebar(bool collapse);
    Sidebar::DockingState get_sidebar_docking_state() const;

    void reset_window_layout();

    // Called after the Preferences dialog is closed and the program settings are saved.
    // Update the UI based on the current preferences.

    //BBS
    void select_curr_plate_all();
    void remove_curr_plate_all();

    void select_all();
    void reset(bool apply_presets_change = false);
    //BBS: return int for various result
    //BBS: trigger a restore project event
    void trigger_restore_project(int skip_confirm = 0);
    void remove_selected();
    void increase_instances(size_t num = 1);
    void decrease_instances(size_t num = 1);
    void fill_bed_with_instances();
    void scale_selection_to_fit_print_volume();
    void apply_cut_object_to_model(size_t init_obj_idx, const ModelObjectPtrs& cut_objects);

    void send_to_printer(bool isall = false);
    void export_gcode(bool prefer_removable);
    static TriangleMesh combine_mesh_fff(const ModelObject& mo, int instance_id, std::function<void(const std::string&)> notify_func = {});
    //BBS: remove amf
    //void export_amf();
    //BBS add extra param for exporting 3mf silence
    // BBS: backup
    int export_3mf(const boost::filesystem::path& output_path = boost::filesystem::path(), SaveStrategy strategy = SaveStrategy::Default, int export_plate_idx = -1, Export3mfProgressFn proFn = nullptr);

    //BBS
    void publish_project();

    void reload_all_from_disk();
    bool reslice();
    void record_slice_preset(std::string action);
    void reslice_SLA_supports(const ModelObject &object, bool postpone_error_messages = false);
    void reslice_SLA_hollowing(const ModelObject &object, bool postpone_error_messages = false);
    void reslice_SLA_until_step(SLAPrintObjectStep step, const ModelObject &object, bool postpone_error_messages = false);   // H2 2.6 恢复：真逻辑体非纯转发

    void clear_before_change_mesh(int obj_idx);
    void changed_mesh(int obj_idx);

    void changed_object(ModelObject &object);
    void changed_object(int obj_idx);
    void schedule_background_process(bool schedule = true);
    /* -1: send current gcode if not specified
     * -2: send all gcode to target machine */
    int send_gcode(int plate_idx = -1, Export3mfProgressFn proFn = nullptr);
    int export_config_3mf(int plate_idx = -1, Export3mfProgressFn proFn = nullptr);
    //BBS jump to nonitor after print job finished
    void send_calibration_job_finished(wxCommandEvent &evt);
    void print_job_finished(wxCommandEvent &evt);
    void send_job_finished(wxCommandEvent& evt);
    void publish_job_finished(wxCommandEvent& evt);
    void open_platesettings_dialog(wxCommandEvent& evt);
	void eject_drive();

    void take_snapshot(const std::string &snapshot_name);
    //void take_snapshot(const wxString &snapshot_name);
    void take_snapshot(const std::string &snapshot_name, UndoRedo::SnapshotType snapshot_type);
    //void take_snapshot(const wxString &snapshot_name, UndoRedo::SnapshotType snapshot_type);

    void undo();
    void redo();
    bool undo_redo_string_getter(const bool is_undo, int idx, const char** out_text);
    void undo_redo_topmost_string_getter(const bool is_undo, std::string& out_text);
    bool search_string_getter(int idx, const char** label, const char** tooltip);
    // For the memory statistics.
    // Enter / leave the Gizmos specific Undo / Redo stack. To be used by the SLA support point editing gizmo.
    void enter_gizmos_stack();
    // BBS: return false if not changed
    bool leave_gizmos_stack();

    void on_filaments_change(size_t extruders_count);
    void on_filaments_delete(size_t extruders_count, size_t filament_id, int replace_filament_id = -1, const std::vector<unsigned char>& is_mixed_snapshot = {});
    bool confirm_auto_generated_gradients(size_t num_physical);
    // BBS
    void on_bed_type_change(BedType bed_type);
    void on_config_change(const DynamicPrintConfig &config);
    /// @brief Check whether high-temperature and low-temperature filaments are mixed on the current plate.
    /// @return True if compatible; false if high/low temperature materials are mixed.
    /// @brief Check whether high-temperature and low-temperature filaments are mixed on a specific plate.
    /// @param plate_index Plate index to check.
    /// @return True if compatible or plate index is invalid; false if high/low temperature materials are mixed.
    /// @brief Same as above, plus fills `detail` with the 1-based used slots on the plate grouped by
    ///        high/low temperature. `detail` is only meaningful when this overload returns false
    ///        (i.e. a mixing conflict exists). On compatible / invalid plates, `detail` is cleared.
    /// @brief Get high/low temperature material mixing state for the current plate.
    /// @return Current plate material mixing state.
    /// @brief Get high/low temperature material mixing state for a specific plate.
    /// @param plate_index Plate index to check.
    /// @return Plate material mixing state.
    /// @brief Check whether a specific plate is blocked by high/low temperature material mixing.
    /// @param plate_index Plate index to check.
    /// @return True if slicing this plate is blocked; otherwise false.
    /// @brief Cached variant for hot paths (per-frame plate-thumbnail rendering).
    ///        Refreshed once per sync_filament_temp_mixing_notification() call (which already
    ///        runs at every state-changing event), so reads are O(1). Returns false for any
    ///        index not yet populated by a sync (safe default — same as "not blocked").
    /// @param plate_index Plate index to check.
    /// @return True if the cached state for this plate is BlockedError; otherwise false.
    /// @brief Check whether slice-all has at least one plate that can be sliced.
    /// @return True if any plate can be sliced and is not blocked by material mixing.
    bool has_sliceable_plate_for_slice_all();

    // —— 阶段 H1 2.1：F3 桥接（服务经 q 调用；一律单行转发，H2 门面瘦身统一评审）——
    PresetBundle* preset_bundle();
    AppConfig* app_config();
    Tab* get_tab(int type);
    std::string get_mode_str();
    NetworkAgent* get_agent();
    void select_tab_3d_editor();
    void params_panel_switch_to_object();
    DeviceManager* device_manager();
    bool is_editor();
    bool is_gcode_viewer();
    ParamsDialog* params_dialog();
    boost::asio::ip::port_type page_http_port();
    wxString international_url(const wxString& origin_url);
    void start_download(const std::string& url);
    PresetUpdater* preset_updater();
    RemovableDriveManager* removable_drive_manager();
    void keyboard_shortcuts();
    void app_update_ui_from_settings();
    bool dark_mode();
    void update_frame_dark_ui(wxFrame* frame);
    wxString inf_dialog_context();
    bool is_user_login();
    void show_down_net_plugin_dlg();
    void publish_model(Model& model, SaveStrategy strategy);
    int get_process_completed_with_error();
    void set_show_warning_dialog(bool v);
    // H1 2.3: ProjectSessionHost 消亡 F3 桥接（单行转发）+ load_auxiliary_files 公有化
    void run_sla_import_job();
    int determine_load_type(std::string filename, std::string override_setting);
    void handle_newer_3mf_schema(DynamicPrintConfig& config_loaded);
    void read_binary_stl(const std::string& filename, std::string& model_id, std::string& code);
    int check_and_keep_current_preset_changes(const wxString& caption, const wxString& header, int buttons, bool* postponed);
    bool check_and_save_current_preset_changes(const wxString& caption, const wxString& header);
    void load_project_dialog(wxString& input_file);
    void update_saved_preset_from_current_preset();
    void import_model_dialog(wxArrayString& files);
    bool can_load_project();
    void import_zip_dialog(wxString& input_file);
    void load_gcode_dialog(wxString& input_file);
    wxString model_file_wildcards();
    void load_current_presets(bool show_sure_modification_dialog, bool show_unsaved_dialog);   // 2 参重载；无参版见上
    void update_nozzle_settings_after();
    void trigger_restore_project_evt(int skip_confirm);
    void request_model_download_evt(const wxString& url);
    void load_auxiliary_files();
    // H1 2.3: SlicingControllerHost 消亡 F3/成员桥接 + 3 件公有化
    void set_process_completed_with_error(int v);
    bool get_show_warning_dialog();
    bool get_tracking_popup_menu();
    void set_tracking_popup_menu_error_message(const std::string& msg);
    void set_plater_drop_target();
    void notify_filament_compatibility_after_apply();
    bool background_processing_enabled() const;
    int start_next_slice();
    void notify_object_config_changed();
    bool can_current_plate_be_sliced() const;
    unsigned int update_background_process(bool force_validation = false, bool postpone_error_messages = false, bool switch_print = true);
    bool restart_background_process(unsigned int state);
    void generate_thumbnail(ThumbnailData& data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params,
        Camera::EType camera_type, bool use_top_view = false, bool for_picking = false, bool ban_light = false);
    void generate_calibration_thumbnail(ThumbnailData& data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params);
    PlateBBoxData generate_first_layer_bbox();
    void reset_all_gizmos();
    unsigned int update_restart_background_process(bool force_scene_update, bool force_preview_update);
    ConfigOptionMode get_mode();
    void load_current_presets();
    void save_mode(int mode);
    bool single_blocks(bool cond);
    bool layers_height_allowed() const;
    void show_delayed_error_message();
    void get_connect_host(std::shared_ptr<PrintHost>& h);
    /// @brief Unified per-plate sliceability predicate.
    /// @details Combines PartPlate::can_slice() with the GUI-layer blockers
    ///          (filament temp mixing, cold-plate incompatibility, and future
    ///          flow_ratio_zero) so plate-toolbar rendering, slice-all iteration,
    ///          and MainFrame button gating cannot drift. Add new blockers as
    ///          early-returns inside this function; do not branch on them at
    ///          individual call sites. Mirrors has_sliceable_plate_for_slice_all's
    ///          non-const contract (relies on is_plate_blocked_by_filament_temp_mixing,
    ///          which is historical non-const).
    /// @param[in] plate_index 0-based plate index.
    /// @return true iff the plate exists, can_slice(), and is not blocked by any GUI-layer blocker.
    bool is_plate_sliceable(int plate_index);
    /// @brief Find the next plate that can be sliced by slice-all.
    /// @param start_plate_index First plate index to check.
    /// @return Plate index if found; otherwise -1.
    int find_next_sliceable_plate_for_slice_all(int start_plate_index);
    /// Sync notification state with current filament temp mixing status.
    /// Returns true if slicing is allowed, false if high/low temperature mixing blocks slicing.
    bool sync_filament_temp_mixing_notification();
    /// @brief Same as above, plus fills `detail` with the 1-based slots of offending filaments.
    ///        `detail` is only meaningful when this overload returns false. On valid plates, `detail` is cleared.
    /// @brief Check whether a specific plate is blocked because one of its used filaments has flow ratio == 0.
    /// @param plate_index Plate index to check.
    /// @return True if slicing this plate is blocked; otherwise false.
    /// Sync notification state with current flow-ratio-zero status.
    /// Returns true if slicing is allowed, false if any used filament has flow ratio == 0.
    bool sync_flow_ratio_zero_notification();
    /// @brief Compute the per-plate cold-plate compatibility state in a single pass.
    /// @details Collects used filament slots once and walks them once, producing
    ///          {state, unsupported filaments, TPU flag} in one traversal.
    /// @param[in] plate_index 0-based plate index.
    /// @return Result struct; state is Compatible when curr_bed_type != btCSP or plate is empty.
    /// @brief Returns true if the plate is blocked from slicing by cold-plate incompatibility.
    /// @param[in] plate_index 0-based plate index.
    /// @brief Sync (close + re-push) cold-plate notifications for the current plate.
    /// @return True if slicing is allowed on current plate after sync.
    bool sync_cold_plate_notification();
    /// Check and guard filament temp mixing before slicing all plates.
    /// @brief Show confirmation dialog for allowed high/low temperature mixing before slice.
    /// @return True if slicing can proceed; false if blocked or user cancelled.
    /// @brief Confirm warning-level high/low temperature material mixing before slicing all plates.
    /// @return True if slice-all can continue; otherwise false.
    /// Queue a single UI sync after filament preset/assignment/plate usage changes.
    void notify_filament_usage_changed();
    // On activating the parent window.
    std::vector<std::string> get_extruder_colors_from_plater_config(const GCodeProcessorResult* const result = nullptr, bool include_mixed = true) const;
    std::vector<std::string> get_colors_for_color_print(const GCodeProcessorResult* const result = nullptr) const;

    void update_menus();
    // BBS
    //void show_action_buttons(const bool is_ready_to_slice) const;


    wxString get_export_gcode_filename(const wxString& extension = wxEmptyString, bool only_filename = false, bool export_all = false) const;
    void update_print_error_info(int code, std::string msg, std::string extra);
    void notify_vhl_dithering_conflict(bool local_z_enabled);
    bool has_incompatible_mixed_filament_in_use() const;


    const Selection& get_selection() const;
    int get_selected_object_idx();
    GLCanvas3D* canvas3D();
    const GLCanvas3D * canvas3D() const;
    GLCanvas3D* get_current_canvas3D(bool exclude_preview = false);
    GLCanvas3D* get_view3D_canvas3D();
    GLCanvas3D* get_preview_canvas3D();
    GLCanvas3D* get_assmeble_canvas3D();

    void arrange(const app::ArrangeRequest& request);   // F3：原无参 + 旧隐式写点前置，一并消亡（app:: 限定——子命名空间不参与非限定查找）
    void orient(const app::OrientRequest& request);
    void find_new_position(const ModelInstancePtrs  &instances);
    //BBS: add print job releated functions
    void get_print_job_data(PrintPrepareData* data);
    int get_send_calibration_finished_event();
    int get_print_finished_event();
    int get_send_finished_event();

    void set_current_canvas_as_dirty();

    PrinterTechnology   printer_technology() const;
    const DynamicPrintConfig * config() const;
    bool                set_printer_technology(PrinterTechnology printer_technology);

    //BBS

    //BBS: add clone logic
    void clone_selection();
    void search(bool plater_is_active, Preset::Type  type, wxWindow *tag, TextInput *etag, wxWindow *stag);
    void optimize_rotation();
    // find all empty cells on the plate and won't overlap with exclusion areas
    static std::vector<Vec2f> get_empty_cells(const Vec2f step);

    //BBS:
    void fill_color(int extruder_id);

    bool can_add_model() const;
    bool can_add_plate() const;
    //BBS
    bool can_undo() const;
    bool can_redo() const;
#if ENABLE_ENHANCED_PRINT_VOLUME_FIT
#endif // ENABLE_ENHANCED_PRINT_VOLUME_FIT

    //BBS:
    bool has_assmeble_view() const;

    void msw_rescale();
    void sys_color_changed();

    // BBS
#if 0
    bool init_view_toolbar();
    void enable_view_toolbar(bool enable);
#endif


    const Camera& get_camera() const;
    Camera& get_camera();

    //BBS: partplate list related functions
    PartPlateList& get_partplate_list();
    //BBS: select the plate by index
    int select_plate(int plate_index, bool need_slice = false);
    //BBS: update progress result
    //BBS: select the plate by hover_id
    //BBS: delete the plate, index= -1 means the current plate
    int delete_plate(int plate_index = -1);
    int duplicate_plate(int plate_index = -1);
    //BBS: select the sliced plate by index
    int select_sliced_plate(int plate_index);
    //BBS: set bed positions
    void set_bed_position(Vec2d& pos);
    //BBS: is the background process slicing currently
    //BBS: update slicing context
    void update_slicing_context_to_current_partplate();
    //BBS: show object info
    void show_object_info();
    //BBS
    //BBS: post process string object exception strings by warning types
    void post_process_string_object_exception(StringObjectException &err);

#if ENABLE_ENVIRONMENT_MAP
    void init_environment_texture();
    unsigned int get_environment_texture_id() const;
#endif // ENABLE_ENVIRONMENT_MAP

    const BuildVolume& build_volume() const;

    // BBS
    //const GLToolbar& get_view_toolbar() const;
    //GLToolbar& get_view_toolbar();

    const GLToolbar& get_collapse_toolbar() const;
    GLToolbar& get_collapse_toolbar();


#if 0
    void update_partplate();
#endif

    void post_slice_state_change_update();

    const Mouse3DController& get_mouse3d_controller() const;
    Mouse3DController& get_mouse3d_controller();

    //BBS: add bed exclude area
	void set_bed_shape() const;
    void set_bed_shape(const Pointfs& shape, const Pointfs& exclude_area, const double printable_height, const std::string& custom_texture, const std::string& custom_model, bool force_as_custom = false) const;

	const NotificationManager* get_notification_manager() const;
	NotificationManager* get_notification_manager();
    DailyTipsWindow* get_dailytips() const;
    //BBS: show message in status bar
    void show_status_message(std::string s);

    void init_notification_manager();

    void bring_instance_forward();

    // >0 while cleanup_unused_filaments_after_batch_match is looping physical
    // deletions: suppresses per-deletion panel rebuild + painting remap (both
    // run once after the loop). Single source of truth — Sidebar queries this
    // instead of keeping its own flag.
    // 阶段 G1：耗材/AMS/批量删除编排服务（Sidebar 门面转发用；唯一实现归属 GUI/app/FilamentService）
    app::FilamentService& filament_service();
    void update_title_dirty_status();

    // RAII guard for batch physical deletion: bumps batch_physical_deletion()
    // on construction, decrements on destruction. Used by the batch-match
    // cleanup loop to skip per-deletion rebuilds/remaps; exception-safe.
    class BatchPhysicalDeletionGuard
    {
        Plater &m_plater;
    public:
        // H2 2.6：inc/dec 门面已删——体走组合根 filament_svc（cpp 定义；q 消费面 BatchMatchCoordinator 保留）
        explicit BatchPhysicalDeletionGuard(Plater &p);
        ~BatchPhysicalDeletionGuard();
        BatchPhysicalDeletionGuard(const BatchPhysicalDeletionGuard &) = delete;
        BatchPhysicalDeletionGuard &operator=(const BatchPhysicalDeletionGuard &) = delete;
    };

    // 阶段 H2 1.1（§5.3 形态）：嵌套三件套（原 :795 SuppressSnapshots / :811 TakeSnapshot / :845 SingleSnapshot）
    // 体迁 app::SnapshotService（token 语义逐字；Plater* 入参不变），别名保持旧调用点语法——
    // 22 文件 91 引用分簇改写至 SnapshotService:: 原生名前的编译稳定点。
    using TakeSnapshot      = app::SnapshotService::Scope;          // app:: 限定——子命名空间不参与非限定查找（F3 先例）
    using SingleSnapshot    = app::SnapshotService::SingleScope;
    using SuppressSnapshots = app::SnapshotService::SuppressScope;



    void toggle_show_wireframe();
    bool is_show_wireframe() const;
    void enable_wireframe(bool status);
    bool is_wireframe_enabled() const;

	// Wrapper around wxWindow::PopupMenu to suppress error messages popping out while tracking the popup menu.
	bool PopupMenu(wxMenu *menu, const wxPoint& pos = wxDefaultPosition);
    bool PopupMenu(wxMenu *menu, int x, int y) { return this->PopupMenu(menu, wxPoint(x, y)); }

    //BBS: add popup logic for table object
    bool PopupObjectTable(int object_id, int volume_id, const wxPoint& position);
    //BBS: popup selection at default position
    bool PopupObjectTableBySelection();

    // get same Plater/ObjectList menus
    wxMenu* plate_menu();
    wxMenu* object_menu();
    wxMenu* part_menu();
    wxMenu* text_part_menu();
    wxMenu* svg_part_menu();
    wxMenu* sla_object_menu();
    wxMenu* default_menu();
    wxMenu* instance_menu();
    wxMenu* layer_menu();
    wxMenu* multi_selection_menu();
    wxMenu*     filament_action_menu(int active_filament_id);
    int     GetPlateIndexByRightMenuInLeftUI();
    void    SetPlateIndexByRightMenuInLeftUI(int);
    static bool has_illegal_filename_characters(const wxString& name);
    static bool has_illegal_filename_characters(const std::string& name);
    static void show_illegal_characters_warning(wxWindow* parent);


    // 阶段 F2：抢占锁释放端门面（旗标与 claim 唯一归属 app::ArrangementService）
    void release_arrange_running();

    bool is_loading_project() const;

private:
    // —— 阶段 H1（路径 A）：priv 解散——服务/PartPlateList unique_ptr + 前向声明（勘误 13 头链隔离）——
    // 成员声明序承 priv(:463-848) 原行序，逆序析构次序不变；撞名成员四件 m_ 改名（勘误 14）。
    MainFrame *main_frame = nullptr;                                                   // priv :463
    std::atomic<bool> filament_usage_sync_pending{false};                              // priv :464
    std::unique_ptr<MenuFactory> menus;                                                // priv :467
    Slic3r::DynamicPrintConfig *m_config = nullptr;   // FIXME: leak?（H1 改名避让公有 config()）  // priv :472
    Slic3r::Model m_model;                                                             // priv :473
    std::unique_ptr<PartPlateList> partplate_list;                                     // priv :478
    // —— 阶段 H2 2.0：15 服务 unique_ptr 所有权迁组合根 GUI_App（design 决策 9；构造留守 plater_init；
    // ~Plater shutdown_services 复现逆序析构）——服务 accessor 声明保留，cpp 体改 wxGetApp() 转发。——
    std::unique_ptr<PlaterWorker<BoostThreadWorker>> m_worker;                         // priv :501
    SLAImportDialog *m_sla_import_dlg = nullptr;                                       // priv :502
    int process_completed_with_error { -1 }; //-1 means no error                       // priv :800
    Slic3r::Print m_fff_print;                                                         // priv :828
    Slic3r::SLAPrint m_sla_print;                                                      // priv :829
    Slic3r::GCodeProcessorResult gcode_result;                                         // priv :830
    SingleSnapshot *m_single = nullptr;                                                // priv :844
    bool show_warning_dialog { false };                                                // priv :848

    // —— 服务 accessor（hpp 仅声明；unique_ptr<前向类型> 不内联解引用——erratum §5.1）——
    app::SnapshotService& snapshots() const;
    app::NotificationHub& notification_hub() const;
    app::PlateController& plates() const;
    app::SlicingController& slicing() const;
    app::ProjectSession& session() const;
    app::ExportSendCoordinator& ex() const;
    app::SelectionQuery& selq() const;
    app::ObjectEditCommands& obj_cmds() const;
    app::ArrangementService& arrange_svc() const;
    app::MixingGateKeeper& gate_keeper() const;
    app::FilamentService& filament_svc() const;
    app::CalibrationFlow& calibration_svc() const;
    app::LayoutManager& layout() const;
    app::SceneRefreshCoordinator& refresh() const;
    app::PlaterEventRouter& router() const;
    NotificationManager* notification_manager() const;

    // —— priv-only 方法（公有区无同名声明者方入此块；内联体一律 cpp 定义化）——
    void plater_init();              // 原 priv ctor 整体折叠
    std::string get_config(const std::string &key) const;
    BoundingBoxf bed_shape_bb() const;
    BoundingBox scaled_bed_shape_bb() const;
    std::vector<size_t> load_model_objects(const ModelObjectPtrs& model_objects, bool allow_negative_z = false, bool split_object = false);
    fs::path get_export_file_path(GUI::FileType file_type);
    wxString get_export_file(GUI::FileType file_type);
    const Selection& get_curr_selection();
    int get_selected_volume_idx() const;
    bool is_slicing_in_progress() const;
    enum UpdateBackgroundProcessReturnState {
        UPDATE_BACKGROUND_PROCESS_RESTART = 1,
        UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE = 2,
        UPDATE_BACKGROUND_PROCESS_INVALID = 4,
        UPDATE_BACKGROUND_PROCESS_FORCE_RESTART = 8,
        UPDATE_BACKGROUND_PROCESS_FORCE_EXPORT = 16,
    };
    void export_gcode(fs::path output_path, bool output_path_on_removable_media);
    void export_gcode(fs::path output_path, bool output_path_on_removable_media, PrintHostJob upload_job);
    bool replace_volume_with_stl(int object_idx, int volume_idx, const fs::path& new_path, const std::string& snapshot = "");
    bool show_publish_dlg(bool show = true);
    void update_publish_dialog_status(wxString &msg, int percent = -1);
    ThumbnailsList generate_thumbnails(const ThumbnailsParams& params, Camera::EType camera_type);
    bool confirm_auto_generated_gradients(wxWindow *parent, size_t num_physical);   // 2 参私有重载；公有 1 参版见公有区
    void set_project_name(const wxString& project_name);

    // Set true during PopupMenu() tracking to suppress immediate error message boxes.
    // The error messages are collected to m_tracking_popup_menu_error_message instead and these error messages
    // are shown after the pop-up dialog closes.
    bool 	 m_tracking_popup_menu = false;
    wxString m_tracking_popup_menu_error_message;

    // 阶段 G4：skip_thumbnail_invalid 迁 app::SceneRefreshCoordinator（hpp 私有区唯一非 priv 成员——勘误 2）

    void suppress_snapshots();
    void allow_snapshots();
    // BBS: single snapshot
    void single_snapshots_enter(SingleSnapshot *single);
    void single_snapshots_leave(SingleSnapshot *single);
    // BBS: add project slice related functions

    // 阶段 G2：_calib_pa_* 辅助与 cut_horizontal 迁 app::CalibrationFlow（私有块删除；
    // slicing_host._calib_pa_pattern_gen_gcode 改指 calibration_svc()）。

    friend class SuppressBackgroundProcessingUpdate;
    friend class PlaterDropTarget;
    // 阶段 H2 1.1：快照替身 Scope/SingleScope（app::SnapshotService 嵌套）经 Plater* 逐字承接嵌套版语义——
    // take_snapshot 公有可及；suppress_snapshots/allow_snapshots/single_snapshots_enter/leave 私有面经 friend 放行。
    friend class app::SnapshotService::Scope;
    friend class app::SnapshotService::SingleScope;
    friend class app::SnapshotService::SuppressScope;
};

class SuppressBackgroundProcessingUpdate
{
public:
    SuppressBackgroundProcessingUpdate();
    ~SuppressBackgroundProcessingUpdate();
private:
    bool m_was_scheduled;
};

std::vector<int> get_min_flush_volumes(const DynamicPrintConfig& full_config);

} // namespace GUI
} // namespace Slic3r

#endif
