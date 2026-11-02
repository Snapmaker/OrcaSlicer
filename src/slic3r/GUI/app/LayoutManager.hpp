// LayoutManager — 布局/视图/主题服务（阶段 G3）。
// 承接：priv 18 成员（Plater.cpp:478-535 原位）+ 9 内联判定族（:574-594）+ 15 函数 +
// save_window_layout/attach_views 新抽（手册 §2.2）；FloatFrame/AuiMgr 类定义随迁 cpp
// （FloatFrame 原体调 wxGetApp().UpdateFrameDarkUI —— 宿主能力经 update_frame_dark_ui 槽注入，
// 维持「服务 cpp 零 wxGetApp」护栏）。
// 依赖规则（B1/B2/B3/G1/G2 护栏延续）：本 hpp 禁 include 宿主头（Plater/GUI_App/MainFrame）；
// 宿主能力经 LayoutManagerHost 槽；通知经 NotificationHub& typed 注入（C/G1/G2 先例）。
// 勘误（手册 §0 / proposal）：路线指引 §4.7 "顶层成员迁移会动 Plater.hpp 私有区" 实测不成立
// ——18 成员原全在 priv（Plater.cpp），本阶段 hpp 仅加前向声明。
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <wx/panel.h>             // wxPanel（current_panel/panels 值语义）
#include <wx/string.h>            // wxString（m_default_window_layout）

#include "../3DBed.hpp"              // Bed3D（值成员，逐字；app/ 相对路径——GUI/ 非 include 根，CalibrationFlow.cpp 同款）
#include "../Camera.hpp"             // Camera（值成员，逐字）
#include "../GLToolbar.hpp"          // GLToolbar（值成员，逐字；BackgroundTexture 同头）
#include "../Mouse3DController.hpp"  // Mouse3DController（值成员，逐字）
#include "../Sidebar.hpp"            // Sidebar::DockingState（get_sidebar_docking_state 签名）
#include "libslic3r/PrintConfig.hpp"  // PrinterTechnology（printer_technology 槽签名）

class wxWindow;
class wxAuiManager;

namespace Slic3r {
class AppConfig;
class Model;
class PresetBundle;

namespace GUI {
class MainFrame;

class AuiMgr;          // 定义随迁 LayoutManager.cpp（原 Plater.cpp:449-458）
class AssembleView;
class GLCanvas3D;
class ObjectList;
class PartPlateList;
class Plater;
class Preview;
class Sidebar;
class View3D;

namespace app {
class NotificationHub;
class ProjectSession;
class SceneRefreshCoordinator;
class SlicingController;

// H1 2.2: LayoutManagerHost removed - F2 ctor typed inject (session/slicing/partplate_list/model/main_frame)
// + refresh late_bind; F3 via q bridges (preset_bundle/app_config/is_editor/app_update_ui_from_settings/dark_mode/is_gcode_viewer/update_frame_dark_ui) + reslice/plater_alive via q.


class LayoutManager {
public:
    LayoutManager(Plater* q, NotificationHub& hub, ProjectSession& session, SlicingController& slicing,
                  PartPlateList& partplate_list, Model& model, MainFrame* main_frame);
    void late_bind(SceneRefreshCoordinator& refresh);   // H1 2.2: refresh declared after this service, bind in plater_init
    ~LayoutManager();                                   // unique_ptr<AuiMgr> 完整类型在 cpp

    // —— 视图/面板成员（原 priv 逐字：指针/容器公有，迁移期最小 diff，G5 收紧；D3）——
    wxPanel*  current_panel{ nullptr };
    std::vector<wxPanel*> panels;
    Sidebar*  sidebar{ nullptr };                       // ctor 接管原 priv init（new Sidebar(q)）
    struct SidebarLayout                                  // 原 priv :483-488 逐字
    {
        bool is_enabled{false};
        bool is_collapsed{false};
        bool show{false};
    } sidebar_layout;
    View3D*   view3D;                                   // 逐字（priv 原样无初始化器；attach_views 于 priv ctor body 赋值）
    Preview*  preview;                                  // 逐字（同上）
    AssembleView* assemble_view { nullptr };            // 逐字
    bool first_enter_assemble{ true };                  // 逐字
    bool show_render_statistic_dialog{ false };         // 逐字
    bool show_wireframe{ false };                       // 逐字
    bool wireframe_enabled{ true };                     // 逐字
    Bed3D bed;                                          // 值成员（View3D/Preview/AssembleView 构造引用源）
    Camera camera;                                      // 值成员
    Mouse3DController mouse3d_controller;               // 值成员
    GLToolbar collapse_toolbar;                         // 值成员（ctor init：GLToolbar::Normal,"Collapse"）
    wxString m_default_window_layout;                   // 逐字（priv ctor body 写入）
#if ENABLE_ENVIRONMENT_MAP
    GLTexture environment_texture;                      // 条件块整迁（纪律 7；ENABLE_ENVIRONMENT_MAP=0）
#endif

    wxAuiManager* aui();                                // m_aui_mgr 接管（基类指针——AuiMgr 完整类型仅在 cpp，
                                                        // 宿主侧（Plater.cpp 含 wx/aui）经基类调用 SetManagedWindow/AddPane 等即可）

    // —— 新抽（priv ctor/reset 迁出）——
    void attach_views(View3D* v3d, Preview* prev, AssembleView* assemble);   // 原 :1566-1568 panels.push_back×3 + 指针落位
    void save_window_layout();                                               // 原 reset() 尾 :2479-2488

    // —— 视图切换（原 priv 函数逐字，宿主解引用改槽）——
    void select_view(const std::string& direction);
    void select_view_3D(const std::string& name, bool no_slice = true);
    void select_next_view_3D();
    void apply_free_camera_correction(bool apply = true);
    void set_current_panel(wxPanel* panel, bool no_slice = true);
    GLCanvas3D* get_current_canvas3D(bool exclude_preview = false);

    // —— AUI 侧栏（原 priv 函数逐字）——
    void enable_sidebar(bool enabled);
    void collapse_sidebar(bool collapse);
    void update_sidebar(bool force_update = false);
    void reset_window_layout();
    Sidebar::DockingState get_sidebar_docking_state();

    // —— 主题/rescale（拆分转发的服务体 = 原序列布局前缀；D4）——
    void msw_rescale();
    void sys_color_changed();
    void apply_color_mode();
    void update_ui_from_settings();

    // —— 工具栏/视图查询 ——
    bool init_collapse_toolbar(const GLTexture* shared_background_texture = nullptr);
    bool has_assemble_view() const;

    // —— 原 priv 内联判定族（:574-594 逐字；体在 cpp——View3D/Preview 在本 hpp 仅前向声明，
    //    指针比较/解引用需完整类型，与原 priv 位于 Plater.cpp 同理）——
    bool is_preview_shown() const;
    bool is_preview_loaded() const;
    bool is_view3D_shown() const;
    bool is_assemble_view_show() const;
    bool are_view3D_labels_shown() const;
    void show_view3D_labels(bool show);
    bool is_view3D_overhang_shown() const;
    void show_view3D_overhang(bool show);
    bool is_view3D_layers_editing_enabled() const;

    // —— 面板种类（视图切换/画布判定的可测纯核；D7）——
    enum class PanelKind { None, View3D, Preview, Assemble };
    struct SidebarVisPlan { bool should_show{false}; bool change{false}; };

    // 纯函数（无 wx 依赖，无头单测直调；手册 §6 用例 A/B）
    static SidebarVisPlan plan_sidebar_visibility(const SidebarLayout& st, bool pane_shown, bool pane_floating);
    static Sidebar::DockingState docking_state_from(bool is_enabled, bool is_floating, bool docked_right);
    static PanelKind pick_canvas_kind(PanelKind current, bool exclude_preview);
    static PanelKind kind_from_view_name(const std::string& name);
    PanelKind kind_of(const wxPanel* panel) const;      // 实例方法（比对成员指针）

private:
    Plater* m_q{ nullptr };                             // q 持有（E2 持 q 先例：action_callback/相机同步等价改写）
    NotificationHub& m_hub;                             // typed（is_dark/nm()->set_sidebar_collapsed/set_in_preview/invalid_all_plate_thumbnails）
    // H1 2.2: typed inject (ctor + late_bind)
    ProjectSession&     m_session;
    SlicingController&  m_slicing;
    PartPlateList&      m_partplate_list;
    Model&              m_model;
    MainFrame*          m_main_frame{nullptr};
    SceneRefreshCoordinator* m_refresh{nullptr};   // late_bind
    std::unique_ptr<AuiMgr> m_aui_mgr;                  // 原 priv 值成员 AuiMgr 化堆（类含 wxGetApp 触点，禁入 hpp；D2）
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
