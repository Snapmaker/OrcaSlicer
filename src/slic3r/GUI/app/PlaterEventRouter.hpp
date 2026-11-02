// PlaterEventRouter — 事件路由服务（阶段 G5）。
// 承接：priv ctor 138 处 Bind 的注册/卸载/重挂（install/uninstall/rebind——主文档 §5.2
// "事件路由挂载 3 件"）+ 50 个 on_* 事件处理器逐字迁入；priv 解散（28 成员上收 Plater 直属 +
// 126 方法四去向）后事件分派唯一归属本服务。
// 依赖规则（B1…G4 护栏延续）：本 hpp 禁 include 宿主头（Plater/GUI_App/MainFrame——asio 链陷阱）
// 且零 wx 必要 include（wxString 仅签名值形参，wx/string.h 轻头）——**Impl pimpl 化**：
// 事件签名类型（SlicingStatusEvent 困 BackgroundSlicingProcess.hpp、18 个 EVT_* 常量困 Plater.hpp）
// 全部留在 cpp（勘误登记：手册 §3 草案"处理器入类"调整为 Impl 承载，bodies 逐字不变）。
// Router cpp 零 wxGetApp（陷阱 8）：wxGetApp/main_frame 触点全经 PlaterEventRouterHost 槽。
#pragma once

#include <atomic>
#include <memory>

#include <functional>

#include <wx/defs.h>              // wx 基础宏（先于 event）
#include <wx/event.h>             // wxDECLARE_EVENT/wxBinding 事件标签
#include <wx/string.h>            // international_url 槽值形参

class wxWindow;

namespace Slic3r {
class AppConfig;
class DeviceManager;
class Model;
class PresetBundle;
class PresetUpdater;

namespace GUI {

class MainFrame;
class MenuFactory;
class ObjectList;
class ParamsDialog;
class PartPlateList;
class Plater;
class RemovableDriveManager;
class Tab;

// 10 个 EVT_* 常量 wxDEFINE_EVENT 于 Plater.cpp TU 内（:253-270 区）——此处 wxDECLARE_EVENT
// 外部化（勘误 8：commit ① 在 Plater.cpp define 前补同款声明 10 行，仅声明零行为），
// Router cpp 与 Plater 装配点共享同一事件标签（GUI ns 与 define 同域）。
wxDECLARE_EVENT(EVT_SLICING_COMPLETED, wxCommandEvent);
wxDECLARE_EVENT(EVT_EXPORT_BEGAN, wxCommandEvent);
wxDECLARE_EVENT(EVT_EXPORT_FINISHED, wxCommandEvent);
wxDECLARE_EVENT(EVT_DOWNLOAD_PROJECT, wxCommandEvent);
wxDECLARE_EVENT(EVT_IMPORT_MODEL_ID, wxCommandEvent);
wxDECLARE_EVENT(EVT_PRINT_FINISHED, wxCommandEvent);
wxDECLARE_EVENT(EVT_SEND_CALIBRATION_FINISHED, wxCommandEvent);
wxDECLARE_EVENT(EVT_SEND_FINISHED, wxCommandEvent);
wxDECLARE_EVENT(EVT_PUBLISH_FINISHED, wxCommandEvent);
wxDECLARE_EVENT(EVT_RESTORE_PROJECT, wxCommandEvent);

namespace app {
class ExportSendCoordinator;
class LayoutManager;
class NotificationHub;
class ObjectEditCommands;
class PlateController;
class ProjectSession;
class SceneRefreshCoordinator;
class SlicingController;
class SnapshotService;

// 阶段 H1 2.2：PlaterEventRouterHost 消亡——F2 ctor typed 注入（main_frame/menus/partplate_list/model/filament_usage_sync_pending）+ reset_all_gizmos 公有化；
// F3×16 改 q 桥接（is_editor/is_gcode_viewer/preset_bundle/app_config/obj_list/get_tab/params_dialog/page_http_port/international_url/start_download/preset_updater/device_manager/removable_drive_manager/keyboard_shortcuts）+ plater 槽即 m_q。


class PlaterEventRouter {
public:
    // typed 服务注入（F1 m_obj_cmds 六服务先例；消费面以 50 处理器实测为准）
    PlaterEventRouter(Plater* q, NotificationHub& hub,
                      SlicingController& slicing, ProjectSession& session,
                      ExportSendCoordinator& ex, PlateController& plates,
                      ObjectEditCommands& obj_cmds, LayoutManager& layout,
                      SceneRefreshCoordinator& refresh, SnapshotService& snapshots,
                      MainFrame* main_frame, MenuFactory& menus, PartPlateList& partplate_list,
                      Model& model, std::atomic<bool>& filament_usage_sync_pending);
    ~PlaterEventRouter();

    // —— 事件路由挂载 3 件（主文档 §5.2）——
    void install();     // 原 priv ctor Bind 区 138 处（逐字；wxEVT_TIMER bind 例外留守——SlicingController 定时器域）
    void uninstall();   // 对称 Unbind
    void rebind();      // uninstall + install

    // —— 纯函数两件（D7；无头单测直调，G3/G4 先例）——
    enum class RightClickMenuKind {
        None, Default, MultiSelection, AssembleMultiSelection,
        ObjectMenu, SlaObjectMenu, AssembleObjectMenu, AssemblePartMenu,
        TextPartMenu, SvgPartMenu, PartMenu,
    };
    // on_right_click 菜单分派判定核（以现体分支为准——勘误登记：手册 §3 四参草案定稿为九参）
    static RightClickMenuKind plan_right_click_menu_kind(
        bool no_object, bool clicked_empty, bool selection_empty, bool panel_is_assemble,
        bool sla, bool some_full_instances, bool is_part,
        bool model_volume_text, bool model_volume_svg);
    struct TabSelectionPlan { bool veto_skip{false}; bool sidebar_show{false}; bool install_hint{false}; };
    // on_tab_selection_changing 判定核（以现体分支为准）：
    //   !is_tabpanel_event → veto_skip（调用方 e.Skip()+return）；
    //   sidebar_show = new_sel==tp3d || new_sel==tp_preview；
    //   Monitor 守卫（monitor_guard ∧ ¬has_agent）→ veto + install_hint
    static TabSelectionPlan plan_tab_selection(bool is_tabpanel_event, int new_sel, int tp3d, int tp_preview,
                                               bool monitor_guard_active, bool has_agent);

private:
    struct Impl;                                        // 事件签名类型困 Plater.hpp/BackgroundSlicingProcess.hpp——cpp 承载
    std::unique_ptr<Impl> m_impl;
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
