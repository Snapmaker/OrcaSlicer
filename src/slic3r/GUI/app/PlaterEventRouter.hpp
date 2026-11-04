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



class PlaterEventRouter {
public:
    PlaterEventRouter(Plater* q, NotificationHub& hub,
                      SlicingController& slicing, ProjectSession& session,
                      ExportSendCoordinator& ex, PlateController& plates,
                      ObjectEditCommands& obj_cmds, LayoutManager& layout,
                      SceneRefreshCoordinator& refresh, SnapshotService& snapshots,
                      MainFrame* main_frame, MenuFactory& menus, PartPlateList& partplate_list,
                      Model& model, std::atomic<bool>& filament_usage_sync_pending);
    ~PlaterEventRouter();

    void install();     // 原 priv ctor Bind 区 138 处（逐字；wxEVT_TIMER bind 例外留守——SlicingController 定时器域）
    void uninstall();   // 对称 Unbind
    void rebind();      // uninstall + install

    enum class RightClickMenuKind {
        None, Default, MultiSelection, AssembleMultiSelection,
        ObjectMenu, SlaObjectMenu, AssembleObjectMenu, AssemblePartMenu,
        TextPartMenu, SvgPartMenu, PartMenu,
    };
    static RightClickMenuKind plan_right_click_menu_kind(
        bool no_object, bool clicked_empty, bool selection_empty, bool panel_is_assemble,
        bool sla, bool some_full_instances, bool is_part,
        bool model_volume_text, bool model_volume_svg);
    struct TabSelectionPlan { bool veto_skip{false}; bool sidebar_show{false}; bool install_hint{false}; };
    //   sidebar_show = new_sel==tp3d || new_sel==tp_preview；
    static TabSelectionPlan plan_tab_selection(bool is_tabpanel_event, int new_sel, int tp3d, int tp_preview,
                                               bool monitor_guard_active, bool has_agent);

private:
    struct Impl;                                        // 事件签名类型困 Plater.hpp/BackgroundSlicingProcess.hpp——cpp 承载
    std::unique_ptr<Impl> m_impl;
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
