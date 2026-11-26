#pragma once
#ifndef slic3r_GLGizmoLocalSubdivision_hpp_
#define slic3r_GLGizmoLocalSubdivision_hpp_

#include "GLGizmoPainterBase.hpp"

#include "slic3r/GUI/GLModel.hpp"
#include "libslic3r/PreviewBudgetGate.hpp"
#include "libslic3r/TriangleMeshDeal.hpp"

#include <utility>

namespace Slic3r {
namespace GUI {

// Painting-style gizmo used to select a group of facets of the selected object
// and subdivide exactly those facets with TriangleMeshDeal::smooth_triangle_mesh_local.
// The facet selection is transient: it lives only while the gizmo is open and is
// consumed by the "Subdivide" button.
//
// A "subdivision level" slider shows a live preview: the rendered geometry of the
// painted volumes is swapped for the subdivided mesh, while painting keeps hitting
// the original mesh (raycasting uses the ModelVolume, rendering the GLVolume).
// The model itself stays untouched until "Subdivide" is pressed.
class GLGizmoLocalSubdivision : public GLGizmoPainterBase
{
public:
    GLGizmoLocalSubdivision(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    void render_painter_gizmo() override;
    // The facet selection is transient, so the gizmo session must not leave steps on
    // the undo stack: after applying, one Ctrl+Z returns straight to the original mesh.
    bool gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down) override;
    bool wants_enter_leave_snapshots() const override { return false; }

protected:
    void on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;
    PainterGizmoType get_painter_type() const override;
    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;
    std::string get_gizmo_entering_text() const override { return _u8L("Entering Local subdivision"); }
    std::string get_gizmo_leaving_text() const override { return _u8L("Leaving Local subdivision"); }
    std::string get_action_snapshot_name() const override { return _u8L("Local subdivision facet selection"); }

    // Left button paints the selection; the right button erases it.
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType::NONE; }
    ColorRGBA get_cursor_sphere_right_button_color() const override { return { 0.f, 0.f, 0.f, 0.25f }; }

    // Opened from the object context menu only, never from the toolbar.
    bool on_is_selectable() const override { return false; }

    // Finer circle brush than the painting gizmos default.
    static const constexpr float CursorRadiusMin = 0.1f;
    const float get_cursor_radius_min() const override { return CursorRadiusMin; }

private:
    bool on_init() override;
    // The selection is transient, so nothing is stored on the ModelVolume. A finished
    // paint stroke ends in update_model_object(), used here to invalidate the preview.
    void update_model_object() override { m_preview_dirty = true; }
    void update_from_model_object(bool first_update = false) override;
    // Every time the gizmo is opened, start without a subdivision preview and
    // recalibrate the fixed panel height from scratch.
    void on_opening() override { m_subdiv_level = 0; m_param_area_max_height = 0.f; m_budget_flow.reset(); }
    void on_shutdown() override;

    void apply_subdivision();
    void close();

    // Live preview of the subdivided region, one entry per painted model part volume.
    // The surroundings are shown by swapping the GLVolume's own GPU model to the
    // non-region geometry, so the canvas renders them through its normal pipeline
    // (identical look to level 0); only the region is drawn by the gizmo itself.
    struct Preview {
        ObjectID                            object_id;  // owning object id at cache build time
        ModelVolume*                        volume = nullptr; // the model part being previewed
        int                                 gl_volume_idx = -1; // full index into ModelObject::volumes, matches GLVolume::volume_idx()
        std::shared_ptr<const TriangleMesh> origin;      // mesh identity at cache build time
        size_t                              facet_count = 0;  // painted original facets
        std::vector<uint32_t>     next_facets;      // selection for the next subdivision step
        std::vector<indexed_triangle_set> meshes;   // [0] original, [n] = n-th subdivision level (bare sets, no stats)
        std::vector<size_t>       region_counts;    // region faces of each level (front of the face list)
        // Level whose computation failed for the current cached inputs (-1 when all
        // requested levels were computed). The caches are rebuilt - and the record
        // cleared - whenever the selection or the mesh changes, so an unchanged
        // failing selection (e.g. non-manifold edges) is not recomputed every frame.
        int                       failed_level = -1;
        // Why the growth stopped below the requested level (valid when failed_level >= 0).
        TriangleMeshDeal::SubdivisionFailure failure_reason = TriangleMeshDeal::SubdivisionFailure::None;
        GLModel                   model;            // non-region geometry, drawn by the gizmo with the volume color
        GLModel                   model_region;     // region-only geometry, colored like the level-0 painting
        bool                      hidden = false;   // the original volume is hidden while the preview is shown
    };
    // One frame of the live preview: updates the caches from the current
    // selection and level. Returns a judgeable outcome - callers that may
    // apply the caches (apply_subdivision) proceed only on Ready, which means
    // the caches reflect the current inputs. prompt_mode selects whether the
    // budget dialog may be shown (the panel asks; an apply-time forced
    // recompute never shows one - it is authorized by a recorded
    // confirmation instead).
    PreviewUpdateOutcome update_preview(BudgetPromptMode prompt_mode);
    void                 restore_preview();
    void                 render_preview_meshes();
    // Rebuilds the preview caches from the current painting state and grows
    // them level by level to m_subdiv_level; also raises the "stopped early"
    // notification. Returns false when it could not run (never in practice:
    // per-preview failures are recorded in the caches and reported here).
    bool                 compute_preview_caches(ModelObject* mo, bool& changed);
    // Cheap budget gate run BEFORE any subdivision computation: estimates the face
    // count of the requested level from the current selection and asks the user
    // when it would exceed LocalSubdivisionFaceBudget (soft limit, Yes continues).
    // Returns false when the user cancelled, true otherwise (also true when the
    // estimate is within the budget or there is nothing selected).
    bool        confirm_face_budget(int level);
    // Counts the current selection and the meshes it sits on for the budget
    // estimate (no subdivision is run).
    void        gather_budget_inputs(size_t &painted, size_t &total_faces) const;
    // Shows the over-budget confirmation dialog (reentrancy-guarded).
    bool        ask_face_budget_dialog();
    // Failure notification text matching the recorded per-preview reasons: the
    // specific cause when all failed previews share one, the combined wording when
    // they mix. apply selects the "Subdivision failed" family, otherwise the
    // "stopped at level" family.
    wxString    subdivision_failure_text(int lowest_achieved, bool apply) const;

    wchar_t m_current_tool = 0;
    bool m_detect_geometry_edge = true;
    std::map<std::string, wxString> m_desc;

    static const int MaxSubdivLevel = 3;
    // Face-count budget shared by the live preview and the applied subdivision:
    // both ask the user before spending the computation beyond this size.
    static constexpr size_t LocalSubdivisionFaceBudget = 1000000;
    int  m_subdiv_level  = 0;   // wanted subdivision level (slider)
    bool m_preview_dirty = true; // painted facets changed, the caches must be rebuilt
    int  m_shown_level   = 0;   // level whose geometry is currently displayed (0 = original)
    // Budget gate of the live preview: the state machine remembers a decline
    // only for the (level, selection size) it was raised for, so a selection
    // that shrinks back under the budget unblocks the preview automatically
    // (still over: silent block, no dialog spam), and a level change
    // re-evaluates from scratch. The flow additionally carries the one-shot
    // apply-time confirmation: a forced recompute is authorized exactly once
    // for exactly the inputs the confirmation was given for, and update_preview
    // reports a judgeable outcome so apply only ever submits caches that were
    // actually recomputed for the current selection.
    PreviewBudgetFlow m_budget_flow;
    // Reentrancy guard: a modal budget dialog pumps events, which may repaint the
    // canvas and re-enter update_preview while the dialog is still open.
    bool m_awaiting_budget_confirmation = false;
    int  m_selected_facet_count = 0; // painted original-face count shown in the panel; refreshed while m_preview_dirty
    // (Requested, achieved) level pair of the last "subdivision stopped early"
    // notification; repaints that fail the same way do not re-notify.
    std::pair<int, int> m_last_failed_levels = {0, 0};
    // Tallest parameter layout drawn so far this session; shorter layouts are
    // padded up to it so switching tools never changes the panel height. Reset
    // when the gizmo opens.
    float m_param_area_max_height = 0.f;
    std::vector<Preview> m_previews;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoLocalSubdivision_hpp_
