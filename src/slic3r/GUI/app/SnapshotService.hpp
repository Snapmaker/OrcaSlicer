#pragma once

#include <string>
#include <vector>

#include "slic3r/Utils/UndoRedo.hpp"   // Slic3r::UndoRedo::Stack / Snapshot / SnapshotType

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


class SnapshotService
{
public:
    class SuppressScope {
    public:
        explicit SuppressScope(SnapshotService& s) : m_s(s) { m_s.suppress_snapshots(); }
        explicit SuppressScope(Plater* plater);
        ~SuppressScope() { m_s.allow_snapshots(); }
        SuppressScope(const SuppressScope&) = delete;
        SuppressScope& operator=(const SuppressScope&) = delete;
    private:
        SnapshotService& m_s;
    };

    // RAII wrapper for taking an Undo / Redo snapshot while disabling the snapshot taking by the methods called from inside this snapshot.
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

    void take_snapshot(const std::string& snapshot_name,
                       UndoRedo::SnapshotType snapshot_type = UndoRedo::SnapshotType::Action);
    void undo();
    void redo();
    void undo_redo_to(size_t time_to_load);
    void undo_redo_to(std::vector<UndoRedo::Snapshot>::const_iterator it_snapshot);
    void update_after_undo_redo(const UndoRedo::Snapshot& snapshot, bool temp_snapshot_was_taken = false);

private:
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
