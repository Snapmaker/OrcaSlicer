#ifndef slic3r_GLGizmoMove_hpp_
#define slic3r_GLGizmoMove_hpp_

#include "GLGizmoBase.hpp"
//BBS: add size adjust related
#include "GizmoObjectManipulation.hpp"

#include <map>
#include <vector>


namespace Slic3r {
namespace GUI {

//BBS: GUI refactor: add object manipulation
class GizmoObjectManipulation;
class GLGizmoMove3D : public GLGizmoBase
{
    static const double Offset;

    Vec3d m_displacement{ Vec3d::Zero() };
    Vec3d m_center{ Vec3d::Zero() };
    BoundingBoxf3 m_bounding_box;
    double m_snap_step{ 1.0 };
    Vec3d m_starting_drag_position{ Vec3d::Zero() };
    Vec3d m_starting_box_center{ Vec3d::Zero() };
    Vec3d m_starting_box_bottom_center{ Vec3d::Zero() };

    struct GrabberConnection
    {
        GLModel model;
        Vec3d old_center{ Vec3d::Zero() };
    };
    std::array<GrabberConnection, 3> m_grabber_connections;

    //BBS: add size adjust related
    GizmoObjectManipulation* m_object_manipulation;

    // EdgeSlicer: "Snap face to surface" (Blender's face project snapping with align rotation).
    // The user picks one face of the selected object or part, then drags; the picked face is kept
    // flush against whatever other object is under the cursor and the selection rotates so the face
    // follows the target surface's normal. The mouse wheel spins it around that normal mid-drag.
    struct SurfaceHit
    {
        int         volume_idx{ -1 };   // index into GLCanvas3D::get_volumes()
        size_t      facet{ 0 };
        Vec3d       mesh_point{ Vec3d::Zero() };  // mesh coords of the hit volume
        Vec3d       mesh_normal{ Vec3d::Zero() };
        Vec3d       point{ Vec3d::Zero() };       // world coords
        Vec3d       normal{ Vec3d::Zero() };
    };
    struct SnapFace
    {
        bool        valid{ false };
        // identifies the GLVolume the face was picked on; GLVolume indices do not survive reload_scene()
        int         object_idx{ -1 };
        int         instance_idx{ -1 };
        int         volume_idx{ -1 };
        Vec3d       mesh_point{ Vec3d::Zero() };
        Vec3d       mesh_normal{ Vec3d::Zero() };
        GLModel     region;             // coplanar facets around the picked one, mesh coords
    };
    enum class SnapState { Idle, Pressed, Dragging };

    bool        m_snap_enabled{ false };
    SnapFace    m_snap_face;
    SnapFace    m_snap_hover;           // face under the cursor on the selection, drawn as a preview
    size_t      m_snap_hover_facet{ 0 };
    std::vector<size_t> m_snap_hover_region_facets;
    SnapState   m_snap_state{ SnapState::Idle };
    Vec2d       m_snap_press_pos{ Vec2d::Zero() };
    SurfaceHit  m_snap_press_hit;
    bool        m_snap_moved{ false };
    double      m_snap_spin{ 0.0 };
    SurfaceHit  m_snap_last_target;
    bool        m_snap_has_target{ false };
    Vec3d       m_snap_start_point{ Vec3d::Zero() };  // picked face at drag start, world coords
    Vec3d       m_snap_start_normal{ Vec3d::Zero() };
    Transform3d m_snap_start_instance{ Transform3d::Identity() };
    Transform3d m_snap_start_volume{ Transform3d::Identity() };
    // face adjacency per mesh, keyed by the mesh pointer and checked against its facet count
    std::map<const indexed_triangle_set*, std::vector<Vec3i32>> m_snap_neighbors;

public:
    //BBS: add obj manipulation logic
    //GLGizmoMove3D(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    GLGizmoMove3D(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id, GizmoObjectManipulation* obj_manipulation);
    virtual ~GLGizmoMove3D() = default;

    double get_snap_step(double step) const { return m_snap_step; }

    // EdgeSlicer: Snap face to surface UI (drawn inside the Move panel) and mid-drag wheel spin.
    void render_snap_to_surface_ui(ImGuiWrapper* imgui, float wrap_width);
    bool on_mouse_wheel_snap(const wxMouseEvent& evt);
    void set_snap_step(double step) { m_snap_step = step; }

    std::string get_tooltip() const override;

    /// <summary>
    /// Postpone to Grabber for move
    /// </summary>
    /// <param name="mouse_event">Keep information about mouse click</param>
    /// <returns>Return True when use the information otherwise False.</returns>
    bool on_mouse(const wxMouseEvent &mouse_event) override;

    /// <summary>
    /// Detect reduction of move for wipetover on selection change
    /// </summary>
    void data_changed(bool is_serializing) override;
protected:
    bool on_init() override;
    std::string on_get_name() const override;
    bool on_is_activable() const override;
    virtual void on_set_state() override;
    void on_start_dragging() override;
    void on_stop_dragging() override;
    void on_dragging(const UpdateData& data) override;
    void on_render() override;
    void on_register_raycasters_for_picking() override;
    void on_unregister_raycasters_for_picking() override;
    //BBS: GUI refactor: add object manipulation
    virtual void on_render_input_window(float x, float y, float bottom_limit);

private:
    double calc_projection(const UpdateData& data) const;
    void   change_cs_by_selection(); //cs mean Coordinate System

    bool snap_available() const;
    bool on_mouse_snap(const wxMouseEvent& mouse_event);
    bool snap_raycast(bool on_selection, SurfaceHit& hit) const;
    bool snap_face_world(Vec3d& point, Vec3d& normal) const;
    const GLVolume* snap_face_volume() const;
    void snap_set_face(SnapFace& face, const SurfaceHit& hit);
    void snap_update_hover();
    void snap_begin_drag();
    void snap_apply(const Transform3d& world_delta);
    void snap_update_drag();
    void snap_commit();
    void snap_spin_in_place(double angle);
    void snap_reset();
    void render_snap_faces();
private:
    int m_last_selected_obejct_idx, m_last_selected_volume_idx;
};



} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoMove_hpp_
