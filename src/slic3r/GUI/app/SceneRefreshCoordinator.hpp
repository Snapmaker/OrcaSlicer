// SceneRefreshCoordinator — 刷新调度服务（阶段 G4）。
// 承接：priv 4 成员（m_need_update/delayed_scene_refresh/delayed_error_message/skip_thumbnail_invalid——
// 末者原在 Plater.hpp:828 私有区，勘误 2）+ 14 函数 + UpdateParams enum（Plater.cpp:523-527，勘误 6），
// 逐字迁入（槽消费等价改写豁免：宿主解引用→SceneRefreshHost 槽 / q->force_update_all_plate_thumbnails
// →m_hub typed / q->is_preview_shown→current_panel==preview / assert &p->model→&m_host.model()）。
// 依赖规则（B1…G3 护栏延续）：本 hpp 禁 include 宿主头（Plater/GUI_App/MainFrame/SlicingController——
// asio 链陷阱）；宿主能力经 SceneRefreshHost 槽；thumbnail 全量刷新经 NotificationHub& typed（C 先例，D1）。
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"   // PrinterTechnology（update/changed_object 触点 + plan_* 纯函数）

class wxPanel;

namespace Slic3r {
class Model;
class ModelObject;
class Print;

namespace GUI {

class AssembleView;
class GLCanvas3D;
class MainFrame;
class PartPlateList;
class Plater;
class Preview;
class View3D;

namespace app {
class LayoutManager;
class NotificationHub;
class ObjectEditCommands;
class ProjectSession;
class SlicingController;
// H1 2.3: SceneRefreshHost removed (21 slots typed, all ctor inject — refresh is last-declared :930, no late_bind)
// - layout faces via m_layout (view3d/preview/assemble_view/current_panel/current_canvas/update_sidebar/printable_height)
// - slicing faces via m_slicing; session printer_technology via m_session; update/restart bg process via q (publicized)
// - notify_object_config_changed new q bridge; can_current_plate_be_sliced publicized; ensure_layers_tool_consistency inlined


class SceneRefreshCoordinator {
public:
    SceneRefreshCoordinator(NotificationHub& hub, Plater* q, Model& model, PartPlateList& partplate_list,
                            SlicingController& slicing, ProjectSession& session, LayoutManager& layout,
                            ObjectEditCommands& obj_cmds, MainFrame* main_frame);   // H1 2.3：全 ctor 注入

    // —— 原 priv UpdateParams enum（Plater.cpp:523-527 逐字随迁；facade :4016 不再引用 priv 路径）——
    enum class UpdateParams {
        FORCE_FULL_SCREEN_REFRESH          = 1,
        FORCE_BACKGROUND_PROCESSING_UPDATE = 2,
        POSTPONE_VALIDATION_ERROR_MESSAGE  = 4,
    };

    // —— 刷新调度主入口（原 priv::update :1958-1995 逐字，槽消费）——
    void update(unsigned int flags = 0);
    // 原 Plater::update(bool,bool) facade 门控（:4014-4025 语义逐字——门控随服务，手册 §0.1）
    void update_gate(bool conside_update_flag, bool force_background_processing_update);

    // —— 场景/视图刷新族（原 priv 函数逐字）——
    void selection_changed();
    void object_list_changed();
    void update_print_volume_state();
    void update_fff_scene();
    void update_fff_scene_only_shells(bool only_shells = true);   // 缺省参随原 priv 声明（:803 原位）
    void update_sla_scene();
    void set_current_canvas_as_dirty();
    void unbind_canvas_event_handlers();
    void reset_canvas_volumes();
    void update_preview_bottom_toolbar();
    void reset_gcode_toolpaths();

    // —— changed_object 族（原 Plater 公有 facade 内联体 :5168-5225 体迁入，D6）——
    void changed_object(ModelObject& object);
    void changed_object(int obj_idx);
    void changed_objects(const std::vector<size_t>& object_idxs);

    // —— 旗标/延迟量（原 priv 4 成员 accessor，语义逐字）——
    bool need_update() const { return m_need_update; }                    // 原 priv inline :507
    void set_need_update(bool need_update) { m_need_update = need_update; } // 原 priv inline :508
    void set_delayed_scene_refresh(bool v) { m_delayed_scene_refresh = v; }   // 写点唯一（D3：slicing_host 槽改指）
    bool consume_delayed_scene_refresh()                                    // 读清原子（:3200-3201 逐字，D3）
    {
        const bool v = m_delayed_scene_refresh;
        m_delayed_scene_refresh = false;
        return v;
    }
    void clear_delayed_error_message() { m_delayed_error_message.clear(); }              // 原 :1051/:1623
    bool has_delayed_error_message() const { return !m_delayed_error_message.empty(); }  // 原 :1052
    std::string take_delayed_error_message()                                             // move-out（:658-659 壳消费，D5）
    {
        std::string msg = std::move(m_delayed_error_message);
        m_delayed_error_message.clear();
        return msg;
    }
    bool skip_thumbnail_invalid() const { return m_skip_thumbnail_invalid; }             // 原 hpp:828 读（:989 槽）
    void set_skip_thumbnail_invalid(bool v) { m_skip_thumbnail_invalid = v; }            // 原 hpp:828 写（:1242 槽）

    // —— 纯函数三件（D7；无头单测直调，G3 D7 先例；服务体消费以钉住语义）——
    struct UpdateFlagsPlan { bool force_bg_restart{false}; bool full_refresh{false}; bool postpone_errors{false}; };
    struct ChangedObjectAction { bool restart_bg{false}; bool reload_scene{false}; };
    struct UpdateGatePlan { bool call{false}; bool clear_need_update{false}; };
    static UpdateFlagsPlan plan_update_flags(PrinterTechnology technology, unsigned int flags);
    static ChangedObjectAction plan_changed_object_action(PrinterTechnology technology);
    static UpdateGatePlan plan_update_gate(bool need_update, bool conside_update_flag);

private:
    NotificationHub& m_hub;                             // typed：force_update_all_plate_thumbnails（C 先例，D1）
    Plater*                     q{nullptr};
    Model&                      m_model;
    PartPlateList&              m_partplate_list;
    SlicingController&          m_slicing;
    ProjectSession&             m_session;
    LayoutManager&              m_layout;
    ObjectEditCommands&         m_obj_cmds;
    MainFrame*                  m_main_frame{nullptr};
    bool m_need_update{false};                          // 原 priv :468
    bool m_delayed_scene_refresh{false};                // 原 priv :489
    std::string m_delayed_error_message;                // 原 priv :490
    bool m_skip_thumbnail_invalid{false};               // 原 Plater.hpp:828（勘误 2）
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
