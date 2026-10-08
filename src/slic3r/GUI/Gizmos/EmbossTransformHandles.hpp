#ifndef slic3r_EmbossTransformHandles_hpp_
#define slic3r_EmbossTransformHandles_hpp_

#include "GLGizmoBase.hpp"
#include "GLGizmoRotate.hpp"
#include "slic3r/GUI/EmbossFreeTransform.hpp"

#include <array>
#include <optional>
#include <string>

namespace Slic3r::GUI {

// Free 3D move and rotate handles of the Text and SVG tools (GLGizmoEmboss, GLGizmoSVG; the traced
// Image is an SVG part). Owner request 2026-10-07: a part that landed badly was hard to lift off or
// turn, the tools only had the surface drag and the ring that turns the part in its own plane.
//
// The handles live inside the host tool, which stays the only active gizmo:
//  - three arrows move the part along its own X / Y / Z (Z = the emboss direction, off / into the
//    surface), or along the world axes;
//  - two rings turn it around its own X and Y. The host's existing in-plane ring (hover id 0) is
//    the third ring, around Z.
// The host forwards hover ids 1-5 (Id), the drag and the rendering. A finished drag reports what it
// did (Result); the host takes the undo snapshot and decides what that means for the projection on
// the surface (EmbossFreeTransform.hpp). The handles are drawn outside the part's outline, so the
// surface drag keeps the whole outline (#377); an arrow that would sit over it is hidden.
class EmbossTransformHandles : public GLGizmoBase
{
public:
    // Hover ids of the handles in the host. 0 is the host's in-plane rotation ring.
    enum Id : int { MoveX = 1, MoveY = 2, MoveZ = 3, RotateX = 4, RotateY = 5 };
    static bool is_handle(int id) { return MoveX <= id && id <= RotateY; }
    static bool is_move(int id) { return MoveX <= id && id <= MoveZ; }
    static bool is_rotate(int id) { return RotateX <= id && id <= RotateY; }

    explicit EmbossTransformHandles(GLCanvas3D &parent);

    // Arrows along the world axes instead of the part's own. The rings always turn around the part's
    // own axes.
    void set_world_axes(bool world) { m_world_axes = world; }
    bool is_world_axes() const { return m_world_axes; }

    // Handles shown at all (the user's choice, and a single part of an object is edited).
    void set_visible(bool visible);
    bool is_visible() const { return m_visible; }

    // The host registers / unregisters its picking raycasters (its own ring); the handles follow
    // when they are visible.
    void on_host_register();
    void on_host_unregister();

    // Hover id of the host (-1 nothing, 0 its ring, 1-5 a handle).
    void set_host_hover_id(int id);

    // Drag of the handle `id` (a host hover id, is_handle()). start_drag() returns false when there
    // is no single part to drag.
    bool start_drag(int id);
    void drag(const UpdateData &data);
    struct Result
    {
        int    id       = -1;
        bool   rotation = false;
        Vec3d  displacement{ Vec3d::Zero() }; // world [mm], moves
        Vec3d  axis{ Vec3d::UnitZ() };        // world, turns
        double angle    = 0.;                 // [rad], turns
        Vec3d  normal{ Vec3d::UnitZ() };      // the part's emboss direction (world) at the start
    };
    // Ends the drag. The volume transformation is already applied to the scene, not to the model.
    std::optional<Result> stop_drag();
    bool is_handle_dragging() const { return m_drag_id >= 0; }

    std::string get_tooltip() const override;

    // The user's choice to show the handles (app config, shared by the Text and SVG tools).
    bool is_enabled() const { return m_enabled; }

    // Part of the tool window: how the part sits on the surface, and the handle options.
    // `distance` [mm] is shown for Placement::Floating. `reattach_pending`: a handle turned "Use
    // surface" off and the next surface drag turns it on again. Returns true when an option changed.
    bool draw_options(ImGuiWrapper &imgui, EmbossFreeTransform::Placement placement, double distance, bool reattach_pending, float tooltip_width);

protected:
    bool        on_init() override;
    std::string on_get_name() const override { return ""; }
    void        on_set_state() override;
    void        on_render() override;
    void        on_register_raycasters_for_picking() override;
    void        on_unregister_raycasters_for_picking() override;

private:
    struct Frame
    {
        Transform3d matrix{ Transform3d::Identity() }; // arrows: orthonormal axes, origin at the centre
        Vec3d       normal{ Vec3d::UnitZ() };          // the part's emboss direction (world, unit)
        double      ring_radius = 0.;
        double      arrow_length = 0.;
    };
    std::optional<Frame> calc_frame() const;
    void sync_picking();
    void set_arrow_picking_active(int i, bool active);
    void render_arrow_lines(const Frame &frame, const std::array<bool, 3> &shown);

    // Rings around the part's own X and Y (Z is the host's in-plane ring).
    std::array<GLGizmoRotate, 2> m_rings;

    bool m_enabled         = true;
    bool m_world_axes      = false;
    bool m_visible         = false;
    bool m_host_registered = false;
    bool m_registered      = false;

    struct ArrowLine
    {
        GLModel model;
        Vec3f   from{ Vec3f::Zero() };
        Vec3f   to{ Vec3f::Zero() };
    };
    std::array<ArrowLine, 3> m_lines;

    // running drag
    int         m_drag_id = -1;
    int         m_object_idx = -1;
    int         m_instance_idx = -1;
    int         m_volume_idx = -1;
    Transform3d m_start_instance{ Transform3d::Identity() };
    Transform3d m_start_volume{ Transform3d::Identity() };
    Vec3d       m_start_normal{ Vec3d::UnitZ() };
    Vec3d       m_axis{ Vec3d::UnitZ() };
    Vec3d       m_start_point{ Vec3d::Zero() };
    Vec3d       m_pivot{ Vec3d::Zero() };
    double      m_start_ring_angle = 0.;
    double      m_distance = 0.;
    double      m_angle    = 0.;
};

} // namespace Slic3r::GUI

#endif // slic3r_EmbossTransformHandles_hpp_
