// ObjectEditCommands — Plater 对象编辑命令服务（阶段 F1 纯搬移，手册《Plater阶段F1-对象编辑ObjectEditCommands与SelectionQuery抽取实施方案》）。
// hpp：wx 零依赖；依赖规则（AGENTS.md）：本目录禁止 include Plater/GUI_App 宿主头，
// q 与 priv 上下文只能经 Host 进入；已落地服务一律构造注入 typed（design D2）。
#pragma once

#include <string>
#include <vector>

#include "libslic3r/libslic3r.h"   // Axis（mirror 签名；enum 值语义）

class wxWindow;

namespace Slic3r {
class Model;
class ModelObject;
class DynamicPrintConfig;
class PresetBundle;
class BuildVolume;
enum class ConversionType : int;   // 定义于 libslic3r/Model.hpp:719（前瞻声明须带相同底层类型）

namespace GUI {
class Plater;
class PartPlateList;
class ObjectList;
class View3D;
class GLCanvas3D;
class Selection;
class Worker;

namespace app {
class LayoutManager;
class SceneRefreshCoordinator;
class SelectionQuery;
class ProjectSession;
class SlicingController;
class NotificationHub;
class SnapshotService;

// 阶段 H1 2.2：ObjectEditCommandsHost 消亡——F2 ctor typed 注入（partplate_list/worker）+ layout/refresh late_bind；
// F1（notify_filament_usage_changed/is_shown/dialog_parent/show_clone_dialog）m_q 直调；F3 preset_bundle q 桥接。

// 对象编辑命令 + 谓词唯一归属；选中状态查询经 SelectionQuery&（typed），不重复持有。
class ObjectEditCommands {
public:
    ObjectEditCommands(Plater* q, Model* model, DynamicPrintConfig* config,
                       SelectionQuery& selq, ProjectSession& session,
                       SlicingController& slicing, NotificationHub& notifications,
                       SnapshotService& snapshots, PartPlateList& partplate_list, Worker& worker);
    void late_bind(LayoutManager& layout, SceneRefreshCoordinator& refresh);   // 阶段 H1 2.2：声明序晚于本服务，plater_init 内补注

    // —— 删除组 ——
    void remove(size_t obj_idx);
    bool delete_object_from_model(size_t obj_idx, bool refresh_immediately = true);
    void delete_all_objects_from_model();
    void remove_selected();

    // —— 变换组 ——
    void center_selection();
    void drop_selection();
    void mirror(Axis axis);
    void split_object();
    void split_volume();
    void scale_selection_to_fit_print_volume();
    void convert_unit(ConversionType conv_type);
    void set_selected_visible(bool visible);
    void exit_gizmo();

    // —— 剪贴板/克隆组 ——
    void cut_selection_to_clipboard();
    void copy_selection_to_clipboard();
    void paste_from_clipboard();
    void clone_selection();
    void apply_cut_object_to_model(size_t obj_idx, const std::vector<ModelObject*>& new_objects);

    // —— 网格/gizmo 组 ——
    void clear_before_change_mesh(int obj_idx);
    void changed_mesh(int obj_idx);
    void reset_all_gizmos();

    // —— 动作分发（wx 事件签名由 priv 转发壳吸收，服务不持 wx 类型）——
    void select_all();
    void deselect_all();
    void on_object_select();
    void on_action_split_objects();
    void on_action_split_volumes();
    void on_repair_model();
    void on_action_layersediting();

    // —— 谓词组（19 个）——
    bool can_delete() const;
    bool can_delete_all() const;
    bool can_fix_through_netfabb() const;
    bool can_simplify() const;
    bool can_increase_instances() const;
    bool can_decrease_instances() const;
    bool can_split(bool to_objects) const;
    bool can_split_to_objects() const;
    bool can_split_to_volumes() const;
    bool can_set_instance_to_object() const;
    bool can_fillcolor() const;
#if ENABLE_ENHANCED_PRINT_VOLUME_FIT
    bool can_scale_to_print_volume() const;
#endif // ENABLE_ENHANCED_PRINT_VOLUME_FIT
    bool can_mirror() const;
    bool can_layers_editing() const;
    bool layers_height_allowed() const;
    bool can_cut_to_clipboard() const;
    bool can_copy_to_clipboard() const;
    bool can_paste_from_clipboard() const;
    bool can_replace_with_stl() const;
    bool can_reload_from_disk() const;

private:
    // 原嵌套版 TakeSnapshot 等价 RAII：take_snapshot → suppress_snapshots，析构 allow_snapshots（语义逐字）
    class TakeSnapshotScope {
    public:
        TakeSnapshotScope(SnapshotService& snapshots, const std::string& name);   // 定义在 cpp（hpp 不完整类型）
        ~TakeSnapshotScope();
    private:
        SnapshotService& m_snapshots;
    };

    Plater*             m_q{nullptr};
    Model*              m_model{nullptr};
    DynamicPrintConfig* m_config{nullptr};
    SelectionQuery&     m_selq;
    ProjectSession&     m_session;
    SlicingController&  m_slicing;
    NotificationHub&    m_notifications;
    SnapshotService&    m_snapshots;
    PartPlateList& m_partplate_list;
    Worker&         m_worker;
    LayoutManager*          m_layout{nullptr};   // late_bind
    SceneRefreshCoordinator* m_refresh{nullptr};  // late_bind
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
