#ifndef slic3r_GLGizmoCadFillet_hpp_
#define slic3r_GLGizmoCadFillet_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"

#include "libslic3r/BRep/CadEdit.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Point.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace Slic3r {
class ModelVolume;

namespace GUI {

class MeshRaycaster;

// Exact CAD edits of one part: fillet, chamfer and shell on the part's B-rep (libslic3r
// BRep/CadEdit.hpp), as opposed to the Edit gizmo's bevel, which works on the triangles.
//
// FLOW
//   * Sourcing. On opening, the part's CAD body is found: the one it already carries (a
//     previous exact edit or conversion), else - for a part imported from STEP whose mesh is
//     unedited - the exact shape re-read from the source file. Anything else offers
//     "Convert to CAD body" (BRep::cad_body_from_mesh, coplanar triangles merged into planar
//     faces), with a warning for big meshes and a hard limit.
//   * Picking. The body's edges are drawn over the part. Hover lights one up (screen-space
//     distance to its polyline, hidden edges rejected against a raycast of the body's own
//     tessellation); a click toggles it, or its whole tangent chain when "Tangent chain" is on;
//     Shift+click on a face toggles all of the face's edges. In Shell mode faces are picked.
//   * Preview / Apply. Both run on the plater's worker. Preview swaps the result into the
//     render volume; Apply takes ONE undo snapshot and swaps the volume's mesh (transform,
//     name, settings and source kept, painted data cleared with a notice), and the new body
//     stays attached to the volume so the next operation stacks on it and STEP export writes
//     it exactly. A failure leaves the model untouched and says why in the panel.
//
// Not in the toolbar: opened from the part's right-click menu or the Edit gizmo's panel, the
// way Simplify is opened.
class GLGizmoCadFillet : public GLGizmoBase
{
public:
    GLGizmoCadFillet(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);
    ~GLGizmoCadFillet() override;

    void data_changed(bool is_serializing) override;
    bool on_mouse(const wxMouseEvent &mouse_event) override;

    bool        wants_enter_leave_snapshots() const override { return true; }
    std::string get_gizmo_entering_text() const override;
    std::string get_gizmo_leaving_text() const override;

protected:
    bool               on_init() override;
    std::string        on_get_name() const override;
    void               on_render() override;
    void               on_render_input_window(float x, float y, float bottom_limit) override;
    bool               on_is_activable() const override;
    bool               on_is_selectable() const override { return false; }
    void               on_set_state() override;
    CommonGizmosDataID on_get_requirements() const override;
    std::string        get_dock_key() const override { return "cadfillet"; }

private:
    enum class Mode : int { Fillet = 0, Chamfer = 1, Shell = 2 };
    enum class Stage {
        NoPart,          // nothing (or not exactly one part) selected
        Loading,         // a worker is sourcing the body / building its topology
        NeedsConversion, // a mesh with no body: offer "Convert to CAD body"
        Ready,           // body and topology loaded: pick and apply
        Failed,          // the body could not be read
    };

    // --- session ---
    ModelVolume *selected_volume(int &object_idx, int &volume_idx) const;
    void         attach_to_selection();
    void         detach();
    bool         volume_alive() const;
    Transform3d  volume_trafo() const;
    double       mesh_scale() const;
    BRep::TessellationParams apply_tessellation() const;

    // --- worker ---
    struct JobOutput;
    // Run `work` on the plater's worker, then `done` on the UI thread - unless the gizmo was
    // re-attached or closed in between, which drops the result.
    bool run_job(const std::string &status, std::function<void(JobOutput &)> work, std::function<void(JobOutput &)> done);
    void load_body_async(std::shared_ptr<const BRep::CadBody> body, bool from_step_source);
    void convert_async();
    void run_operation_async(bool apply);
    void on_body_loaded(JobOutput &out);

    // --- the operation ---
    std::string operation_key() const;
    void        clear_preview();
    void        show_preview_mesh(const indexed_triangle_set &its);
    void        commit(const BRep::CadOpResult &result, std::unique_ptr<BRep::CadTopology> topo);
    void        commit_conversion(std::shared_ptr<const BRep::CadBody> body);

    // --- picking ---
    void update_hover(const Vec2d &mouse_pos, bool shift_down);
    int  pick_edge(const Vec2d &mouse_pos, bool has_hit, const Vec3d &hit_world) const;
    void toggle_edges(const std::vector<int> &edges);
    void clear_selection();

    // --- rendering ---
    void rebuild_edge_models();
    void build_tubes(const std::vector<int> &edges, float radius, GLModel &model) const;
    void build_faces(const std::vector<int> &faces, GLModel &model) const;
    void render_overlays();

    // Session identity.
    ModelVolume *m_volume{nullptr};
    ObjectID     m_volume_id;
    int          m_object_idx{-1};
    int          m_volume_idx{-1};
    uint64_t     m_token{0};

    Stage                                m_stage{Stage::NoPart};
    std::shared_ptr<const BRep::CadBody> m_body;
    bool                                 m_body_from_step{false};
    std::unique_ptr<BRep::CadTopology>   m_topo;
    std::unique_ptr<MeshRaycaster>       m_raycaster;
    std::string                          m_load_error; // why the body is not there
    int                                  m_mesh_triangles{0};

    bool        m_job_running{false};
    std::string m_job_status;

    // Options.
    Mode  m_mode{Mode::Fillet};
    float m_radius{1.f};
    float m_distance{1.f};
    float m_thickness{2.f};
    bool  m_tangent_chain{true};

    // Selection.
    std::set<int> m_sel_edges;
    std::set<int> m_sel_faces;
    int           m_hover_edge{-1};
    int           m_hover_face{-1};
    std::vector<int> m_hover_edges; // what a click would toggle
    bool          m_hover_shift{false};
    Vec2d         m_last_mouse{Vec2d::Zero()};

    // Result of the last Preview / failed Apply.
    std::shared_ptr<BRep::CadOpResult> m_preview;
    std::shared_ptr<BRep::CadTopology> m_preview_topo;
    std::string                        m_preview_key;
    bool                               m_preview_shown{false};
    std::string                        m_last_error;
    std::string                        m_last_info;

    // Render models (mesh coordinates, drawn with the volume's transform).
    GLModel m_edges_model;
    GLModel m_hover_model;
    GLModel m_selected_model;
    GLModel m_hover_face_model;
    GLModel m_selected_faces_model;
    bool    m_models_dirty{true};
    float   m_tube_radius{0.05f};

    std::map<std::string, wxString> m_desc;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoCadFillet_hpp_
