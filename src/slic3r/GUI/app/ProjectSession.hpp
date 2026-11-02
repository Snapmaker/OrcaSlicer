// ProjectSession — Plater 项目会话服务（阶段 E1 纯搬移，手册《Plater阶段E1-项目ProjectSession抽取实施方案》）。
// hpp：wx 仅以指针/值类型出现；依赖规则（AGENTS.md）：本目录禁止 include Plater/GUI_App 宿主头，
// q 与 priv 上下文只能经 Host 进入；SLAImportDialog 因其头文件含 Plater.hpp 同禁（经 Host 槽惰性创建）。
#pragma once

#include <functional>
#include <regex>
#include <string>
#include <vector>

#include <wx/string.h>                  // wxString（get/set_project_filename 等签名）
#include <wx/colour.h>                  // wxColour（next_color_for_filament 槽）

#include <boost/filesystem/path.hpp>    // boost::filesystem::path（m_project_folder / load_files 签名）
#include "libslic3r/Point.hpp"          // Vec2d/Vec3d（bed 触点槽）
#include "libslic3r/ProjectTask.hpp"    // BBLProject（按值成员需完整类型）
#include "../ProjectDirtyStateManager.hpp"  // ProjectDirtyStateManager（物理持有，勘误 4/手册 §3）

class wxWindow;
class wxPanel;                          // wx 全局命名空间（勿置入 Slic3r::GUI——E13 同类遮蔽）
class wxArrayString;

namespace Slic3r {
class Model;
class ModelObject;
class DynamicPrintConfig;
class AppConfig;
class PresetBundle;
class NetworkAgent;
class BackgroundSlicingProcess;   // 定义在 Slic3r（非 Slic3r::GUI）——误置于 GUI 会遮蔽真实类（C2079）
enum class SaveStrategy : int;          // 完整定义在 libslic3r/Format/bbs_3mf.hpp（cpp 侧 include）
enum class LoadStrategy : int;
namespace GUI {
class Plater;
class MainFrame;
class PartPlateList;
class View3D;
class Preview;
class ObjectList;
class NotificationManager;
class SLAImportDialog;
class GLCanvas3D;
class Selection;
class Job;

namespace app {
class SnapshotService;                  // 构造注入 typed 引用（D4 决策）
class PlateController;
class ExportSendCoordinator;
class LayoutManager;
class MixingGateKeeper;
class NotificationHub;
class SceneRefreshCoordinator;
class SelectionQuery;
class SlicingController;
// H1 2.3: ProjectSessionHost removed (79 assembled slots + 1 dead decl; 9 zero-consumer slots deleted per C.4 discipline)
// - F2 ctor inject: slicing/partplate_list/hub/plates/main_frame; late_bind(ex/selq/layout/refresh/gate_keeper :907>902)
// - F3 via q bridges x17 + load_auxiliary_files publicized; dialog_parent->m_q; plater_alive dead-deleted

class ProjectSession
{
public:
    ProjectSession(Plater* q, Model* model, DynamicPrintConfig* config, SnapshotService& snapshots,
                   SlicingController& slicing, PartPlateList& partplate_list, NotificationHub& hub,
                   PlateController& plates, MainFrame* main_frame);
    void late_bind(ExportSendCoordinator& ex, SelectionQuery& selq, LayoutManager& layout,
                   SceneRefreshCoordinator& refresh, MixingGateKeeper& gate_keeper);   // H1 2.3
    ~ProjectSession();
    // H2 收口（用户授权接口扩展 2026-10-23）：模型对象访问器——消费方脱离 Plater::model() 门面
    Model& model() const;   // cpp 定义（Model 前向声明——解引用需完整型）


    // —— 纯函数（单测面；原 Plater 静态组 :609/:615/:625）——
    static bool has_illegal_filename_characters(const wxString& wxs_name);
    static bool has_illegal_filename_characters(const std::string& name);
    static void show_illegal_characters_warning(wxWindow* parent);

    // 公共静态 helper（原 Plater.cpp 文件局部 static；priv::can_reload_from_disk 复用；E1 迁入）
    static std::vector<std::pair<int, int>> reloadable_volumes(const Model &model, const Selection &selection);

    // —— 脏状态组（原 priv :913-943 / up_to_date :6953 / update_title_dirty_status :6384）——
    void set_plater_dirty(bool is_dirty);            // 原 priv:913
    bool is_project_dirty() const;                   // 原 priv:914
    bool is_presets_dirty() const;                   // 原 priv:915
    void update_project_dirty_from_presets();        // 原 priv:916-921（put_other_changes() 备份副作用序冻结，手册陷阱 6）
    int  save_project_if_dirty(const wxString& reason); // 原 priv:922-941（get_export_file_path → Host ask_export_path，缝合）
    void reset_project_dirty_after_save();           // 原 priv:942（m_snapshots typed 注入）
    void reset_project_dirty_initial_presets();      // 原 priv:943
    bool up_to_date(bool saved, bool backup);        // 原 priv:6953-6965（单测面）
    void update_dirty_from_stack(bool m);            // snapshot_host.update_dirty_from_stack 改接入口
    void update_title_dirty_status();                // 原 priv:6384

    // —— 文件名/路径组（原 priv :6314/:6403/:6363/:6369）——
    wxString get_project_filename(const wxString& extension = wxEmptyString) const;
    void     set_project_filename(const wxString& filename);   // 尾段写 m_preview_only_filename（手册陷阱 9：commit ① 经 Host，② 后直读）
    wxString get_project_name() const;
    void     set_project_name(const wxString& project_name);

    // —— 模式标志组（hpp 六成员门面；commit ① 经 Host 桥接，② 后直读自有成员）——
    bool        only_gcode_mode() const;
    void        set_only_gcode(bool only_gcode);
    bool        using_exported_file() const;
    void        set_using_exported_file(bool exported_file);
    bool        is_loading_project() const;
    void        set_loading_project(bool loading);
    std::string get_preview_only_filename() const;
    int         valid_plates_count() const;
    void        set_valid_plates_count(int count);
    const wxString& get_last_loaded_gcode() const;
    void        reset_last_loaded_gcode();

    // —— 生命周期组（原 Plater 本体 :7098/:7189/:7315/:9595/:9634，门面一行转发）——
    int  new_project(bool skip_confirm = false, bool silent = false, const wxString& project_name = wxEmptyString);
    void load_project(const wxString& filename = wxEmptyString, const wxString& originfile = "-");
    int  save_project(bool saveAs = false);
    int  close_with_confirm(std::function<bool(bool)> second_check = std::function<bool(bool)>());
    void trigger_restore_project(int skip_confirm = 0);

    // —— 加载组（原 priv :2446/:3548 + 门面 :8765/:8789/:9182/:9374/:9411/:9422/:7631/:8798/:8601/:8611/:8730）——
    // LoadStrategy 完整定义在 libslic3r/Format/bbs_3mf.hpp（hpp 仅前置声明，默认实参由 priv 转发壳给足）
    std::vector<size_t> load_files(const std::vector<boost::filesystem::path>& input_files, LoadStrategy strategy, bool ask_multi);
    std::vector<size_t> load_files(const std::vector<std::string>& input_files, LoadStrategy strategy, bool ask_multi);
    bool load_files(const class wxArrayString& filenames);
    std::vector<size_t> load_files_core(const std::vector<boost::filesystem::path>& input_files, LoadStrategy strategy, bool ask_multi); // 原 priv::load_files ≈1,100 行主体
    std::vector<size_t> load_model_objects(const std::vector<class ModelObject*>& model_objects, bool allow_negative_z = false, bool split_object = false); // ModelObjectPtrs = std::vector<ModelObject*>（typedef 免 include Model.hpp）
    bool open_3mf_file(const boost::filesystem::path& file_path);
    int  get_3mf_file_count(std::vector<boost::filesystem::path> paths);
    void add_file();
    void add_model(bool imperial_units, std::string fname);
    bool preview_zip_archive(const boost::filesystem::path& archive_path);
    void load_gcode();
    void load_gcode(const wxString& filename);
    void reload_gcode_from_disk();

    // —— 导入/替换/下载组（原 :8571/:8583/:8592/:7361/:7601/:7606/:4562/:4489/:4632/:5073）——
    void import_zip_archive();
    void import_sl1_archive();
    void extract_config_from_project();
    void import_model_id(wxString download_info);
    void download_project(const wxString& project_id);
    void request_model_download(wxString url);
    void replace_with_stl();
    bool replace_volume_with_stl(int object_idx, int volume_idx, const boost::filesystem::path& new_path, const std::string& snapshot);
    void reload_from_disk();
    void reload_all_from_disk();

    // —— Bind handler 函数体（wx 事件签名由 priv 转发壳吸收，载荷以普通参数传入；G 的 Router 收口）——
    void on_action_open_project();
    void on_action_request_model_id(const wxString& model_id);
    void on_action_download_project(const wxString& project_id);
    void show_preview_only_hint();

    // —— 物理迁入成员（唯一归属；priv 侧词边界 grep = 0 守恒）——
    ProjectDirtyStateManager dirty_state;                   // 原 priv:868（自包含状态类，全仓无指针持有）
    PrinterTechnology        printer_technology = ptFFF;    // 原 priv:827（值成员无指针锚定）
    BBLProject               project;                       // 原 priv:1262
    boost::filesystem::path  m_project_folder;              // 原 priv:1281
    wxString                 m_project_name;                // 原 priv:1284
    size_t                   m_saved_timestamp  = 0;        // 原 priv:1298（up_to_date 时间戳）
    size_t                   m_backup_timestamp = 0;        // 原 priv:1299
    // hpp 六成员（commit ② 落位；原 Plater.hpp :880/:882/:883/:885/:886/:887）
    wxString     m_last_loaded_gcode;
    bool         m_only_gcode { false };
    bool         m_exported_file { false };
    bool         m_loading_project { false };
    std::string  m_preview_only_filename;
    int          m_valid_plates_count { 0 };
    // hpp 六成员于 commit ② 落位于此（m_only_gcode/m_exported_file/m_loading_project/

private:
    // 文件类型判定 static regex（原 priv:897-901 声明 / :1308-1312 定义，随迁）
    static const std::regex pattern_bundle;
    static const std::regex pattern_3mf;
    static const std::regex pattern_zip_amf;
    static const std::regex pattern_any_amf;
    static const std::regex pattern_prusa;

    SlicingController&          m_slicing;
    PartPlateList&              m_partplate_list;
    NotificationHub&            m_hub;
    PlateController&            m_plates;
    MixingGateKeeper*           m_gate_keeper{nullptr};     // late_bind（:907 晚声明）
    MainFrame*                  m_main_frame{nullptr};
    ExportSendCoordinator*      m_ex{nullptr};              // late_bind
    SelectionQuery*             m_selq{nullptr};            // late_bind
    LayoutManager*              m_layout{nullptr};          // late_bind
    SceneRefreshCoordinator*    m_refresh{nullptr};         // late_bind
    Plater*             m_q = nullptr;          // 仅存指针（禁 include 宿主头，永不解引用；析构期防御经 m_host.plater_alive）
    Model*              m_model = nullptr;      // 勘误 1：model/config 成员留守 priv（地址被 PartPlate 指针网锚定），注入引用消费
    DynamicPrintConfig* m_config = nullptr;
    SnapshotService&    m_snapshots;            // D4：typed 注入（up_to_date/reset_after_save 摸撤销栈）
    SLAImportDialog*    m_sla_import_dlg = nullptr; // 惰性创建（m_host.create_sla_import_dialog）
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
