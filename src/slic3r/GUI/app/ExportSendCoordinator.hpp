#pragma once

#include <functional>
#include <string>

#include <wx/string.h>                       // wxString（get_export_file/get_export_gcode_filename 签名）
#include <boost/filesystem/path.hpp>         // boost::filesystem::path（export_3mf / get_export_file_path）
#include "libslic3r/Format/bbs_3mf.hpp"      // SaveStrategy / Export3mfProgressFn（二者同在此头）
#include "libslic3r/GCode/ThumbnailData.hpp" // ThumbnailData / ThumbnailsParams / PlateBBoxData（缩略图槽签名）
#include "../Jobs/PrintJob.hpp"              // PrintPrepareData（m_print_job_data 值成员；自包含，仅前置声明 Plater）
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX                 // 防 min/max 宏泄漏（EdgeGrid/FanMover 的 std::max 受害实测）
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN      // 防 winsock.h 拖入与 asio 的 winsock2 冲突（ws2def sockaddr 重定义实测）
#endif
#include <windows.h>                         // MB_YESNO/MessageBox（get_export_file __WXMSW__ 分支）；asio（PrintHost→Http）需 windows.h 先行
#endif
#include "../../Utils/PrintHost.hpp"         // PrintHostJob（export_gcode 3-arg 上传重载签名；Slic3r 层，wx 轻；asio 需 windows.h 先行）

class wxWindow;
class wxCommandEvent;                        // wx 全局命名空间（勿置入 Slic3r::GUI——E13 同类遮蔽）

namespace Slic3r {
class Model;
class ModelObject;
class DynamicPrintConfig;
class AppConfig;
class PresetBundle;
class NetworkAgent;
class TriangleMesh;
class SLAPrint;                   // 定义在 Slic3r（libslic3r；sla_print 槽签名）
class BackgroundSlicingProcess;   // 定义在 Slic3r（非 Slic3r::GUI）——误置于 GUI 会遮蔽真实类（C2079，E13 教训）
class DeviceManager;              // 同上：定义在 Slic3r 层（getDeviceManager 返回 Slic3r::DeviceManager*）
namespace GUI {
class Plater;
class MainFrame;
class PartPlateList;
class View3D;
class Preview;
class NotificationManager;
class Selection;
class SelectMachineDialog;
class SendMultiMachinePage;
class SendToPrinterDialog;
class PublishDialog;
class RemovableDriveManager;

namespace app {
class NotificationHub;
class CalibrationFlow;
class LayoutManager;
class ProjectSession;
class SceneRefreshCoordinator;
class SelectionQuery;
class SlicingController;                        // EX→PS typed 构造注入（design D1，目标图 EX-->PS 边）

// H1 2.3: ExportSendCoordinatorHost removed (batch-2 accidentally dropped its assembly; re-typed here first of batch 3)
// - F2 ctor inject: slicing/partplate_list/hub/main_frame; late_bind(selq/layout/refresh/calib)
// - F1 dialog_parent/plater_alive via q; F3 via q bridges (preset_bundle/app_config/agent/removable_drive_manager/device_manager
//   + new: inf_dialog_context/is_user_login/show_down_net_plugin_dlg)
// - priv-only faces publicized on Plater: update_background_process/restart_background_process/generate_thumbnail/
//   generate_calibration_thumbnail/generate_first_layer_bbox/publish_model(new bridge to local publish())/get_process_completed_with_error/set_show_warning_dialog


class ExportSendCoordinator {
public:
    ExportSendCoordinator(Plater* q, Model* model, DynamicPrintConfig* config, app::ProjectSession& session,
                          SlicingController& slicing, PartPlateList& partplate_list, NotificationHub& hub, MainFrame* main_frame);
    void late_bind(SelectionQuery& selq, LayoutManager& layout, SceneRefreshCoordinator& refresh, CalibrationFlow& calib);   // H1 2.3
    ~ExportSendCoordinator();

    void export_gcode(bool prefer_removable);
    void export_gcode(boost::filesystem::path output_path, bool output_path_on_removable_media);                            // 2-arg（原 priv 第一 overload；门面 export_gcode(bool) 尾部调用）
    void export_gcode(boost::filesystem::path output_path, bool output_path_on_removable_media, PrintHostJob upload_job);   // 3-arg 上传重载（原 priv 第二 overload；send_gcode_legacy 上传分支）
    void export_gcode_3mf(bool export_all = false);
    void export_core_3mf();
    void export_stl(bool extended, bool selection_only, bool multi_stls);
    void export_toolpaths_to_obj() const;
    bool has_toolpaths_to_export() const;
    static TriangleMesh combine_mesh_fff(const ModelObject& mo, int instance_id, std::function<void(const std::string&)> notify_func = {});
    int  export_3mf(const boost::filesystem::path& output_path = boost::filesystem::path(), SaveStrategy strategy = SaveStrategy::Default, int export_plate_idx = -1, Export3mfProgressFn proFn = nullptr);
    int  export_config_3mf(int plate_idx, Export3mfProgressFn proFn = nullptr);

    int  send_gcode(int plate_idx = -1, Export3mfProgressFn proFn = nullptr);
    void send_gcode_legacy(int plate_idx = -1, Export3mfProgressFn proFn = nullptr, bool use_3mf = false);
    void send_to_printer(bool isall = false);
    void send_gcode_finish(wxString name);
    void get_print_job_data(PrintPrepareData* data);
    void update_print_error_info(int code, std::string msg, std::string extra);   // 三路分发（Jobs/PrintJob·SendJob 消费；门面零改动）

    void publish_project();                     // 空壳随迁（勘误 2；SlicingController 调用点行为不变）
    bool show_publish_dialog(bool show);
    void handle_publish(bool publishing_start); // priv 壳 bool 投影入口（design D2）
    void update_publish_dialog_status(wxString& msg, int percent);

    boost::filesystem::path get_export_file_path(int file_type);
    wxString get_export_file(int file_type);
    wxString get_export_gcode_filename(const wxString& extension, bool only_filename, bool export_all) const;
    static wxString join_gcode_filename(const boost::filesystem::path& project_folder, const wxString& project_name,
                                        const wxString& plate_index_str, const wxString& extension,
                                        bool only_filename, bool export_all,
                                        const wxString& untitled_object_name = {}); // design D7 纯函数（folder 空分支的 model 依赖段经第 7 参注入）

    void handle_print_plate();
    void handle_print_all();
    void handle_print_plate_from_sdcard();
    void handle_send_to_multi_machine();
    void install_network_plugin();
    void update_plugin_when_launch();
    void show_install_plugin_hint();
    void handle_print_job_finished(wxCommandEvent& evt);
    void handle_send_job_finished(wxCommandEvent& evt);
    void handle_publish_job_finished(wxCommandEvent& evt);
    void handle_send_calibration_job_finished(wxCommandEvent& evt);
    void notify_send_to_sdcard_color_mode();    // priv::on_change_color_mode 触点（执行期补充：SendToPrinterDialog 暗色同步）
    void notify_sys_color_changed();            // priv::sys_color_changed 触点（执行期补充：SelectMachineDialog 主题同步）

    bool exporting_active() const;
    bool is_exporting_to_removable() const;
    bool is_exporting_to_local() const;
    bool is_not_exporting() const;
    bool exporting_to_removable() const;
    bool exporting_to_local() const;
    void set_not_exporting();
    void set_exporting_status(int status);
    bool is_publishing() const;
    void set_is_publishing(bool v);
    void publishing_update_status(const wxString& msg, int percent);
    void publishing_cancel();
    bool publishing_dlg_active() const;
    bool publish_dialog_shown() const;
    SelectMachineDialog* select_machine_dlg();  // 懒创建（原 priv 装配 lambda 语义）
    const std::string& get_last_output_path() const { return m_last_output_path; }
    const std::string& get_last_output_dir_path() const { return m_last_output_dir_path; }

private:
    SlicingController&          m_slicing;
    PartPlateList&              m_partplate_list;
    NotificationHub&            m_hub;
    MainFrame*                  m_main_frame{nullptr};
    SelectionQuery*             m_selq{nullptr};            // late_bind
    LayoutManager*              m_layout{nullptr};          // late_bind
    SceneRefreshCoordinator*    m_refresh{nullptr};         // late_bind
    CalibrationFlow*            m_calib{nullptr};           // late_bind
    Plater*             q{nullptr};
    Model*              model{nullptr};
    DynamicPrintConfig* config{nullptr};
    app::ProjectSession& m_session;

    enum ExportingStatus {
        NOT_EXPORTING = 0,
        EXPORTING_TO_REMOVABLE,
        EXPORTING_TO_LOCAL
    };
    ExportingStatus     m_exporting_status{NOT_EXPORTING};
    std::string         m_last_output_path;
    std::string         m_last_output_dir_path;
    bool                m_is_publishing{false};
    PrintPrepareData    m_print_job_data;
    SelectMachineDialog*  m_select_machine_dlg{nullptr};
    SendMultiMachinePage* m_send_multi_dlg{nullptr};
    SendToPrinterDialog*  m_send_to_sdcard_dlg{nullptr};
    PublishDialog*        m_publish_dlg{nullptr};

    void hide_select_machine_dlg();
    void enter_prepare_mode();
    void hide_send_to_printer_dlg();
};
} } } // namespace Slic3r::GUI::app
