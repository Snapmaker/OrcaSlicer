// SnapshotService — Plater 快照域服务（阶段 C 纯搬移，手册《Plater阶段C-快照与通知服务抽取实施方案》§3.1）。
// 全文件无 wx：仅依赖 slic3r/Utils/UndoRedo.hpp（实测无 wx 依赖，可直接进单测）。
// 依赖规则（手册 §10.1 / AGENTS.md）：本目录禁止 include Plater/GUI_App 宿主头，q 与 priv 上下文只能经 Host 进入。
#pragma once

#include <string>
#include <vector>

#include "slic3r/Utils/UndoRedo.hpp"   // Slic3r::UndoRedo::Stack / Snapshot / SnapshotType
                                        // （snapshot_modifies_project 同文件；PrinterTechnology/ConfigOptionMode 经 libslic3r/Config.hpp 可见）

namespace Slic3r {
class Model;
class PresetBundle;
class AppConfig;
class BackgroundSlicingProcess;   // 定义在 Slic3r（非 Slic3r::GUI）——误置于 GUI 会遮蔽真实类（C2079）
namespace GUI {
class GLCanvas3D;
class ObjectList;
class View3D;
class Preview;
class AssembleView;
class PartPlateList;
class Plater;

namespace app {
class LayoutManager;
class ProjectSession;
class SceneRefreshCoordinator;
class SlicingController;

// 阶段 H1 2.2：SnapshotHost 消亡——F2 ctor typed 注入（model/partplate_list）+ late_bind
// （slicing/session/layout/refresh——声明序晚于本服务 :825；m_single 经 SingleSnapshot*& 引注入）；
// F3（preset_bundle/app_config/get_mode/load_current_presets/save_mode）q 桥接；
// layers_height_allowed/show_delayed_error_message 走 q 公有化面。

class SnapshotService
{
public:
    // 服务原生抑制 RAII：undo_redo_to 内部替身（原 Plater.cpp:7910 的宿主 SuppressSnapshots）
    class SuppressScope {
    public:
        explicit SuppressScope(SnapshotService& s) : m_s(s) { m_s.suppress_snapshots(); }
        // 阶段 H2 1.1：Plater 嵌套类 SuppressSnapshots 的替身形态（Plater* 入参；cpp 承载，经 friend 取服务引用）
        explicit SuppressScope(Plater* plater);
        ~SuppressScope() { m_s.allow_snapshots(); }
        SuppressScope(const SuppressScope&) = delete;
        SuppressScope& operator=(const SuppressScope&) = delete;
    private:
        SnapshotService& m_s;
    };

    // RAII wrapper for taking an Undo / Redo snapshot while disabling the snapshot taking by the methods called from inside this snapshot.
    // 阶段 H2 1.1：Plater 嵌套类 TakeSnapshot 的替身（Plater.hpp §5.3 using 别名原形）——体随嵌套版逐字对齐，cpp 承载
    // （解引用 Plater* 需完整型）；suppress/allow 走 Plater 私有面，经 friend 放行。
    class Scope {
    public:
        Scope(Plater* plater, const std::string& snapshot_name);
        Scope(Plater* plater, const std::string& snapshot_name, UndoRedo::SnapshotType snapshot_type);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        Plater* m_plater;
    };

    // BBS: limit to single snapshot taking by the methods called from inside
    // this snapshot.
    // 阶段 H2 1.1：Plater 嵌套类 SingleSnapshot 的替身——token 语义逐字（check 返回 false = 本次快照已被消费）。
    class SingleScope {
    public:
        SingleScope(Plater* plater);
        ~SingleScope();
        bool check(bool modify);
    private:
        Plater* m_plater;
        bool    token = false;
        bool    modify = false;
    };

    SnapshotService(Plater* q, Model& model, PartPlateList& partplate_list);
    void late_bind(SlicingController& slicing, ProjectSession& session, LayoutManager& layout,
                   SceneRefreshCoordinator& refresh);   // 阶段 H1 2.2：声明序晚于本服务，plater_init 内补注

    // —— 无 wx 核心（单测面）——
    UndoRedo::Stack& undo_redo_stack()         { assert(m_active != nullptr); return *m_active; }
    UndoRedo::Stack& undo_redo_stack_main()    { return m_main; }
    void suppress_snapshots()                  { ++m_prevent; }
    void allow_snapshots()                     { --m_prevent; }
    bool inside_snapshot_capture() const       { return m_prevent != 0; }
    void enter_gizmos_stack();
    bool leave_gizmos_stack();
    int  get_active_snapshot_index();
    bool undo_redo_string_getter(bool is_undo, int idx, const char** out_text);
    void undo_redo_topmost_string_getter(bool is_undo, std::string& out_text);

    // —— 流程（wx 副作用经 Host）——
    void take_snapshot(const std::string& snapshot_name,
                       UndoRedo::SnapshotType snapshot_type = UndoRedo::SnapshotType::Action);
    void undo();
    void redo();
    void undo_redo_to(size_t time_to_load);
    void undo_redo_to(std::vector<UndoRedo::Snapshot>::const_iterator it_snapshot);
    void update_after_undo_redo(const UndoRedo::Snapshot& snapshot, bool temp_snapshot_was_taken = false);

private:
    // —— 阶段 H1 2.2：typed 注入（ctor + late_bind）——
    Plater*                     m_q{nullptr};
    Model&                      m_model;
    PartPlateList&              m_partplate_list;
    SlicingController*          m_slicing{nullptr};   // late_bind
    ProjectSession*             m_session{nullptr};   // late_bind
    LayoutManager*              m_layout{nullptr};    // late_bind
    SceneRefreshCoordinator*    m_refresh{nullptr};   // late_bind
    UndoRedo::Stack m_main;
    UndoRedo::Stack m_gizmos;
    UndoRedo::Stack* m_active = &m_main;
    int         m_prevent = 0;
    std::string m_last_fff_printer_profile_name;      // ← 原 priv:1338
    std::string m_last_sla_printer_profile_name;      // ← 原 priv:1339
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
