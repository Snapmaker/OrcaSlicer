#pragma once
#ifndef slic3r_GLGizmoLocalSubdivision_hpp_
#define slic3r_GLGizmoLocalSubdivision_hpp_

#include "GLGizmoPainterBase.hpp"

#include "slic3r/GUI/GLModel.hpp"
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

    // The base painter requires a single full instance, but this gizmo only needs
    // the selection to belong to one instance of one object: the object/part context
    // menu can be invoked with just one volume of a multi-volume object selected
    // (e.g. right after repairing that part), and the per-volume preview machinery
    // already handles the unselected parts. Multi-object and multi-instance
    // selections are still refused, as the data path assumes a single instance.
    bool on_is_activable() const override;

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
    void on_opening() override { m_subdiv_level = 0; m_param_area_max_height = 0.f; }
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
        // Why the growth stopped below the requested level (valid when
        // failed_level >= 0).
        TriangleMeshDeal::SubdivisionFailure failure_reason = TriangleMeshDeal::SubdivisionFailure::None;
        GLModel                   model;            // non-region geometry, drawn by the gizmo with the volume color
        GLModel                   model_region;     // region-only geometry, colored like the level-0 painting
        bool                      hidden = false;   // the original volume is hidden while the preview is shown
    };
    void        update_preview();
    void        restore_preview();
    void        render_preview_meshes();
    // GLVolume of the previewed volume inside the selected object instance. Looks
    // over ALL canvas volumes, not just the selected ones: with a partial-volume
    // selection the painted volume may lie outside the selection.
    const GLVolume* matching_glvolume(const Preview& p) const;
    // Whether the given model part belongs to the current selection. Parts outside
    // a partial-volume selection are hidden for the whole session.
    bool is_selected_volume(const ModelVolume* mv) const;
    // Painting hits on unselected parts (possible through the object-wide raycaster
    // while those parts are hidden) must not survive the stroke: their transient
    // selection is cleared so no preview or apply ever sees it.
    void clear_painting_on_hidden_parts();
    // Per-volume gate for the inherited selector draw: skip exactly the volumes
    // whose draw render_preview_meshes replaces (conditions mirrored), so
    // unpainted siblings and failed previews keep their original rendering.
    bool        render_selector_for_volume(const ModelVolume* mv) const override;
    // Failure notification text (the only reachable cause is the non-manifold
    // guard). apply selects the "Subdivision failed" family, otherwise the
    // "stopped at level" family.
    wxString    subdivision_failure_text(int lowest_achieved, bool apply, int* reason_key_out = nullptr) const;

    wchar_t m_current_tool = 0;
    bool m_detect_geometry_edge = true;
    std::map<std::string, wxString> m_desc;

    static const int MaxSubdivLevel = 3;
    int  m_subdiv_level  = 0;   // wanted subdivision level (slider)
    bool m_preview_dirty = true; // painted facets changed, the caches must be rebuilt
    int  m_shown_level   = 0;   // level whose geometry is currently displayed (0 = original)
    int  m_selected_facet_count = 0; // painted original-face count shown in the panel; refreshed while m_preview_dirty
    // (Requested, achieved) level pair of the last "subdivision stopped early"
    // notification; repaints that fail the same way do not re-notify.
    std::pair<int, int> m_last_failed_levels = {0, 0};
    int                 m_last_failed_reason  = 0; // 0 none, 1 non-manifold, 2 degenerate, 3 mixed - the last notified cause
    bool                m_notified_no_subdiv_faces = false; // "no subdividable faces" told once per selection
    // Tallest parameter layout drawn so far this session; shorter layouts are
    // padded up to it so switching tools never changes the panel height. Reset
    // when the gizmo opens.
    float m_param_area_max_height = 0.f;
    std::vector<Preview> m_previews;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoLocalSubdivision_hpp_
