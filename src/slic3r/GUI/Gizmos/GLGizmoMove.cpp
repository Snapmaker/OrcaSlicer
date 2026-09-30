#include "GLGizmoMove.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
//BBS: GUI refactor
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <imgui/imgui.h>


#include <glad/gl.h>

#include <wx/utils.h>

namespace Slic3r {
namespace GUI {

#if ENABLE_FIXED_GRABBER
const double GLGizmoMove3D::Offset = 50.0;
#else
const double GLGizmoMove3D::Offset = 10.0;
#endif

//BBS: GUI refactor: add obj manipulation
GLGizmoMove3D::GLGizmoMove3D(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id, GizmoObjectManipulation* obj_manipulation)
    : GLGizmoBase(parent, icon_filename, sprite_id)
    //BBS: GUI refactor: add obj manipulation
    , m_object_manipulation(obj_manipulation)
{}

std::string GLGizmoMove3D::get_tooltip() const
{
    const Selection& selection = m_parent.get_selection();
    bool show_position = selection.is_single_full_instance();
    const Vec3d& position = selection.get_bounding_box().center();

    if (m_hover_id == 0 || m_grabbers[0].dragging)
        return "X: " + format(show_position ? position(0) : m_displacement(0), 2);
    else if (m_hover_id == 1 || m_grabbers[1].dragging)
        return "Y: " + format(show_position ? position(1) : m_displacement(1), 2);
    else if (m_hover_id == 2 || m_grabbers[2].dragging)
        return "Z: " + format(show_position ? position(2) : m_displacement(2), 2);
    else
        return "";
}

bool GLGizmoMove3D::on_mouse(const wxMouseEvent &mouse_event) {
    if (m_snap_enabled && on_mouse_snap(mouse_event))
        return true;
    return use_grabbers(mouse_event);
}

void GLGizmoMove3D::data_changed(bool is_serializing) {
    m_grabbers[2].enabled = !m_parent.get_selection().is_wipe_tower();
    change_cs_by_selection();
    // A face picked on an object that is no longer the selection is meaningless.
    if (m_snap_state == SnapState::Idle && (m_snap_face.valid || m_snap_hover.valid) && (!snap_available() || snap_face_volume() == nullptr))
        snap_reset();
}

bool GLGizmoMove3D::on_init()
{
    for (int i = 0; i < 3; ++i) {
        m_grabbers.push_back(Grabber());
        m_grabbers.back().extensions = GLGizmoBase::EGrabberExtension::PosZ;
    }

    m_grabbers[0].angles = { 0.0, 0.5 * double(PI), 0.0 };
    m_grabbers[1].angles = { -0.5 * double(PI), 0.0, 0.0 };

    m_shortcut_key = WXK_CONTROL_M;

    return true;
}

std::string GLGizmoMove3D::on_get_name() const
{
    if (!on_is_activable() && m_state == EState::Off) {
        return _u8L("Move") + ":\n" + _u8L("Please select at least one object.");
    } else {
        return _u8L("Move");
    }
}

bool GLGizmoMove3D::on_is_activable() const
{
    return !m_parent.get_selection().is_empty();
}

void GLGizmoMove3D::on_set_state() {
    // Snap mode stays switched on between openings, the picked face does not.
    snap_reset();
    if (get_state() == On) {
        m_last_selected_obejct_idx = -1;
        m_last_selected_volume_idx = -1;
        change_cs_by_selection();
    }
}

void GLGizmoMove3D::on_start_dragging()
{
    assert(m_hover_id != -1);

    m_displacement = Vec3d::Zero();
    const BoundingBoxf3& box = m_parent.get_selection().get_bounding_box();
    m_starting_drag_position = m_grabbers[m_hover_id].matrix * m_grabbers[m_hover_id].center;
    m_starting_box_center = box.center();
    m_starting_box_bottom_center = box.center();
    m_starting_box_bottom_center(2) = box.min(2);
}

void GLGizmoMove3D::on_stop_dragging()
{
    m_parent.do_move(L("Gizmo-Move"));
    m_displacement = Vec3d::Zero();
}

void GLGizmoMove3D::on_dragging(const UpdateData& data)
{
    if (m_hover_id == 0)
        m_displacement.x() = calc_projection(data);
    else if (m_hover_id == 1)
        m_displacement.y() = calc_projection(data);
    else if (m_hover_id == 2)
        m_displacement.z() = calc_projection(data);
        
    Selection &selection = m_parent.get_selection();
    TransformationType trafo_type;
    trafo_type.set_relative();
    switch (wxGetApp().obj_manipul()->get_coordinates_type())
    {
    case ECoordinatesType::Instance: { trafo_type.set_instance(); break; }
    case ECoordinatesType::Local: { trafo_type.set_local(); break; }
    default: { break; }
    }
    selection.translate(m_displacement, trafo_type);
}

void GLGizmoMove3D::on_render()
{
    const Selection& selection = m_parent.get_selection();

    // Drawn against the scene depth, before it is cleared for the grabbers.
    if (m_snap_enabled)
        render_snap_faces();

    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));

    const auto &[box, box_trafo]    = selection.get_bounding_box_in_current_reference_system();
    m_bounding_box                  = box;
    m_center                        = box_trafo.translation();
    if (m_object_manipulation) {
        m_object_manipulation->cs_center = box_trafo.translation();
    }
    const Transform3d base_matrix   = box_trafo;
    float space_size = 20.f *INV_ZOOM;

    for (int i = 0; i < 3; ++i) {
        m_grabbers[i].matrix = base_matrix;
    }

    const Vec3d zero = Vec3d::Zero();

    // x axis
    m_grabbers[0].center = {m_bounding_box.max.x() + space_size, 0, 0};
    // y axis
    m_grabbers[1].center = {0, m_bounding_box.max.y() + space_size,0};
    // z axis
    m_grabbers[2].center = {0,0, m_bounding_box.max.z() + space_size};

    for (int i = 0; i < 3; ++i) {
        m_grabbers[i].color       = AXES_COLOR[i];
        m_grabbers[i].hover_color = AXES_HOVER_COLOR[i];
    }
    glsafe(::glLineWidth((m_hover_id != -1) ? 2.0f : 1.5f));

    auto render_grabber_connection = [this, &zero](unsigned int id) {
        if (m_grabbers[id].enabled) {
            //if (!m_grabber_connections[id].model.is_initialized() || !m_grabber_connections[id].old_center.isApprox(center)) {
                m_grabber_connections[id].old_center = m_grabbers[id].center;
                m_grabber_connections[id].model.reset();

                GLModel::Geometry init_data;
                init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
                init_data.color = AXES_COLOR[id];
                init_data.reserve_vertices(2);
                init_data.reserve_indices(2);

                // vertices
                init_data.add_vertex((Vec3f)zero.cast<float>());
                init_data.add_vertex((Vec3f)m_grabbers[id].center.cast<float>());

                // indices
                init_data.add_line(0, 1);

                m_grabber_connections[id].model.init_from(std::move(init_data));
            //}

            glLineStipple(1, 0x0FFF);
            glEnable(GL_LINE_STIPPLE);
            m_grabber_connections[id].model.render();
            glDisable(GL_LINE_STIPPLE);
        }
    };

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader != nullptr) {
        shader->start_using();
        const Camera& camera = wxGetApp().plater()->get_camera();
        shader->set_uniform("view_model_matrix", camera.get_view_matrix() * base_matrix);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());

        // draw axes
        for (unsigned int i = 0; i < 3; ++i) {
            render_grabber_connection(i);
        }

        shader->stop_using();
    }
	
	// draw grabbers
    render_grabbers(box);

    if (m_object_manipulation->is_instance_coordinates()) {
        shader = wxGetApp().get_shader("flat");
        if (shader != nullptr) {
            shader->start_using();
            const Camera& camera = wxGetApp().plater()->get_camera();

            Geometry::Transformation cur_tran;
            if (auto mi = m_parent.get_selection().get_selected_single_intance()) {
                cur_tran = mi->get_transformation();
            } else {
                cur_tran = selection.get_first_volume()->get_instance_transformation();
            }

            shader->set_uniform("view_model_matrix", camera.get_view_matrix() * cur_tran.get_matrix());
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());

            render_cross_mark(Vec3f::Zero(), true);

            shader->stop_using();
        }
    }
}

void GLGizmoMove3D::on_register_raycasters_for_picking()
{
    // the gizmo grabbers are rendered on top of the scene, so the raytraced picker should take it into account
    m_parent.set_raycaster_gizmos_on_top(true);
}

void GLGizmoMove3D::on_unregister_raycasters_for_picking()
{
    m_parent.set_raycaster_gizmos_on_top(false);
}

//BBS: add input window for move
void GLGizmoMove3D::on_render_input_window(float x, float y, float bottom_limit)
{
    if (m_object_manipulation)
        m_object_manipulation->do_render_move_window(m_imgui, "Move", x, y, bottom_limit, this);
}


double GLGizmoMove3D::calc_projection(const UpdateData& data) const
{
    double projection = 0.0;

    const Vec3d starting_vec = m_starting_drag_position - m_starting_box_center;
    const double len_starting_vec = starting_vec.norm();
    if (len_starting_vec != 0.0) {
        const Vec3d mouse_dir = data.mouse_ray.unit_vector();
        // finds the intersection of the mouse ray with the plane parallel to the camera viewport and passing throught the starting position
        // use ray-plane intersection see i.e. https://en.wikipedia.org/wiki/Line%E2%80%93plane_intersection algebric form
        // in our case plane normal and ray direction are the same (orthogonal view)
        // when moving to perspective camera the negative z unit axis of the camera needs to be transformed in world space and used as plane normal
        const Vec3d inters = data.mouse_ray.a + (m_starting_drag_position - data.mouse_ray.a).dot(mouse_dir) * mouse_dir;
        // vector from the starting position to the found intersection
        const Vec3d inters_vec = inters - m_starting_drag_position;

        // finds projection of the vector along the staring direction
        projection = inters_vec.dot(starting_vec.normalized());
    }

    if (wxGetKeyState(WXK_SHIFT))
        projection = m_snap_step * (double)std::round(projection / m_snap_step);

    return projection;
}

void GLGizmoMove3D::change_cs_by_selection() {
    int          obejct_idx, volume_idx;
    ModelVolume *model_volume = m_parent.get_selection().get_selected_single_volume(obejct_idx, volume_idx);
    if (m_last_selected_obejct_idx == obejct_idx && m_last_selected_volume_idx == volume_idx) {
        return;
    }
    m_last_selected_obejct_idx = obejct_idx;
    m_last_selected_volume_idx = volume_idx;
    if (m_parent.get_selection().is_multiple_full_object()) {
        m_object_manipulation->set_use_object_cs(false);
    }
    else if (model_volume) {
         m_object_manipulation->set_use_object_cs(true);
    } else {
        m_object_manipulation->set_use_object_cs(false);
    }
    if (m_object_manipulation->get_use_object_cs()) {
        m_object_manipulation->set_coordinates_type(ECoordinatesType::Instance);
    } else {
        m_object_manipulation->set_coordinates_type(ECoordinatesType::World);
    }
}


// ---------------------------------------------------------------------------------------------
// EdgeSlicer: Snap face to surface
// ---------------------------------------------------------------------------------------------

namespace {

// Facets sharing the seed facet's plane (within a small angle and distance), reached through
// shared edges. On a curved surface this is just the seed facet.
std::vector<size_t> coplanar_region(const indexed_triangle_set& its, const std::vector<Vec3i32>& neighbors, size_t seed)
{
    static constexpr size_t MAX_REGION    = 200000;
    static constexpr double COS_TOLERANCE = 0.99985; // ~1 degree
    static constexpr double DIST_TOLERANCE = 0.01;   // mm, mesh coordinates

    auto facet_normal = [&its](size_t f) -> Vec3d {
        const Vec3i32& t = its.indices[f];
        const Vec3d a = its.vertices[t[0]].cast<double>();
        const Vec3d n = (its.vertices[t[1]].cast<double>() - a).cross(its.vertices[t[2]].cast<double>() - a);
        const double len = n.norm();
        return len > 0. ? Vec3d(n / len) : Vec3d(Vec3d::Zero());
    };

    std::vector<size_t> region;
    if (seed >= its.indices.size())
        return region;
    const Vec3d n0 = facet_normal(seed);
    const Vec3d p0 = its.vertices[its.indices[seed][0]].cast<double>();
    region.push_back(seed);
    if (n0 == Vec3d::Zero() || neighbors.size() != its.indices.size())
        return region;

    std::vector<char>   visited(its.indices.size(), 0);
    std::vector<size_t> stack{ seed };
    visited[seed] = 1;
    region.clear();
    while (!stack.empty() && region.size() < MAX_REGION) {
        const size_t f = stack.back();
        stack.pop_back();
        region.push_back(f);
        for (int k = 0; k < 3; ++k) {
            const int nb = neighbors[f][k];
            if (nb < 0 || size_t(nb) >= visited.size() || visited[nb])
                continue;
            visited[nb] = 1;
            if (facet_normal(nb).dot(n0) < COS_TOLERANCE)
                continue;
            bool on_plane = true;
            for (int j = 0; j < 3 && on_plane; ++j)
                on_plane = std::abs(n0.dot(its.vertices[its.indices[nb][j]].cast<double>() - p0)) <= DIST_TOLERANCE;
            if (on_plane)
                stack.push_back(size_t(nb));
        }
    }
    return region;
}

Vec3d transform_normal(const Transform3d& trafo, const Vec3d& normal)
{
    return (trafo.linear().inverse().transpose() * normal).normalized();
}

// World transform that puts `face_point` onto `target_point` with `face_normal` pointing into
// the target surface (opposite `target_normal`), then spins by `spin` around the target normal.
Transform3d snap_delta(const Vec3d& face_point, const Vec3d& face_normal, const Vec3d& target_point, const Vec3d& target_normal, double spin)
{
    const Vec3d into_surface = -target_normal.normalized();
    Transform3d delta = Transform3d::Identity();
    delta.translate(target_point);
    delta.rotate(Eigen::AngleAxisd(spin, into_surface));
    delta.rotate(Eigen::Quaterniond::FromTwoVectors(face_normal.normalized(), into_surface));
    delta.translate(-face_point);
    return delta;
}

} // namespace

bool GLGizmoMove3D::snap_available() const
{
    const Selection& selection = m_parent.get_selection();
    return m_parent.get_canvas_type() == GLCanvas3D::ECanvasType::CanvasView3D &&
           !selection.is_empty() && !selection.is_wipe_tower() &&
           (selection.is_single_full_instance() || selection.is_single_volume_or_modifier());
}

bool GLGizmoMove3D::snap_raycast(bool on_selection, SurfaceHit& hit) const
{
    const Selection& selection = m_parent.get_selection();
    const GLVolume*  first     = selection.get_first_volume();
    if (first == nullptr)
        return false;
    const bool    volume_mode = selection.is_single_volume_or_modifier();
    const Camera& camera      = wxGetApp().plater()->get_camera();
    const Vec2d   mouse_pos   = m_parent.get_local_mouse_position();
    const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;

    double closest = std::numeric_limits<double>::max();
    for (size_t i = 0; i < volumes.size(); ++i) {
        const GLVolume* v = volumes[i];
        if (v == nullptr || !v->mesh_raycaster || v->is_wipe_tower || v->volume_idx() < 0)
            continue;
        // In object mode the whole instance moves, so all of its parts count as the selection.
        const bool in_selection = volume_mode ? selection.contains_volume((unsigned int) i) :
                                                (v->object_idx() == first->object_idx() && v->instance_idx() == first->instance_idx());
        if (in_selection != on_selection)
            continue;
        if (on_selection ? (!volume_mode && v->is_modifier) : (!v->is_active || v->plate_focus_hidden || v->is_modifier))
            continue;

        const Transform3d trafo = v->world_matrix();
        Vec3f  position, normal;
        size_t facet = 0;
        if (!v->mesh_raycaster->closest_hit(mouse_pos, trafo, camera, position, normal, nullptr, &facet))
            continue;
        const Vec3d  world_point = trafo * position.cast<double>();
        const double dist        = (camera.get_position() - world_point).squaredNorm();
        if (dist < closest) {
            closest          = dist;
            hit.volume_idx   = int(i);
            hit.facet        = facet;
            hit.mesh_point   = position.cast<double>();
            hit.mesh_normal  = normal.cast<double>().normalized();
            hit.point        = world_point;
            hit.normal       = transform_normal(trafo, hit.mesh_normal);
        }
    }
    return closest < std::numeric_limits<double>::max();
}

const GLVolume* GLGizmoMove3D::snap_face_volume() const
{
    if (!m_snap_face.valid)
        return nullptr;
    const Selection&    selection = m_parent.get_selection();
    const GLVolumePtrs& volumes   = m_parent.get_volumes().volumes;
    const GLVolume*     first     = selection.get_first_volume();
    if (first == nullptr || first->object_idx() != m_snap_face.object_idx || first->instance_idx() != m_snap_face.instance_idx)
        return nullptr;
    for (size_t i = 0; i < volumes.size(); ++i) {
        const GLVolume* v = volumes[i];
        if (v != nullptr && v->mesh_raycaster && v->object_idx() == m_snap_face.object_idx && v->instance_idx() == m_snap_face.instance_idx &&
            v->volume_idx() == m_snap_face.volume_idx)
            return (!selection.is_single_volume_or_modifier() || selection.contains_volume((unsigned int) i)) ? v : nullptr;
    }
    return nullptr;
}

bool GLGizmoMove3D::snap_face_world(Vec3d& point, Vec3d& normal) const
{
    const GLVolume* v = snap_face_volume();
    if (v == nullptr)
        return false;
    const Transform3d trafo = v->world_matrix();
    point  = trafo * m_snap_face.mesh_point;
    normal = transform_normal(trafo, m_snap_face.mesh_normal);
    return true;
}

void GLGizmoMove3D::snap_set_face(SnapFace& face, const SurfaceHit& hit)
{
    const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;
    if (hit.volume_idx < 0 || hit.volume_idx >= int(volumes.size()))
        return;
    const GLVolume* v = volumes[hit.volume_idx];
    face.valid        = true;
    face.object_idx   = v->object_idx();
    face.instance_idx = v->instance_idx();
    face.volume_idx   = v->volume_idx();
    face.mesh_point   = hit.mesh_point;
    face.mesh_normal  = hit.mesh_normal;
    face.region.reset();

    std::vector<size_t> region;
    const indexed_triangle_set* its = v->mesh_raycaster->get_aabb_mesh().get_triangle_mesh();
    if (its != nullptr && hit.facet < its->indices.size()) {
        std::vector<Vec3i32>& neighbors = m_snap_neighbors[its];
        if (neighbors.size() != its->indices.size())
            neighbors = its_face_neighbors(*its);
        region = coplanar_region(*its, neighbors, hit.facet);

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(3 * region.size());
        init_data.reserve_indices(3 * region.size());
        unsigned int id = 0;
        for (size_t f : region) {
            for (int j = 0; j < 3; ++j)
                init_data.add_vertex((Vec3f) its->vertices[its->indices[f][j]]);
            init_data.add_triangle(id, id + 1, id + 2);
            id += 3;
        }
        face.region.init_from(std::move(init_data));
    }
    if (&face == &m_snap_hover) {
        m_snap_hover_facet = hit.facet;
        m_snap_hover_region_facets = std::move(region);
        std::sort(m_snap_hover_region_facets.begin(), m_snap_hover_region_facets.end());
    }
}

void GLGizmoMove3D::snap_update_hover()
{
    SurfaceHit hit;
    if (m_hover_id != -1 || !snap_raycast(true, hit)) {
        if (m_snap_hover.valid) {
            m_snap_hover.valid = false;
            m_snap_hover.region.reset();
            m_parent.set_as_dirty();
        }
        return;
    }
    const GLVolume* v = m_parent.get_volumes().volumes[hit.volume_idx];
    const bool same_region = m_snap_hover.valid && m_snap_hover.object_idx == v->object_idx() &&
                             m_snap_hover.instance_idx == v->instance_idx() && m_snap_hover.volume_idx == v->volume_idx() &&
                             std::binary_search(m_snap_hover_region_facets.begin(), m_snap_hover_region_facets.end(), hit.facet);
    if (!same_region) {
        snap_set_face(m_snap_hover, hit);
        m_parent.set_as_dirty();
    }
}

bool GLGizmoMove3D::on_mouse_snap(const wxMouseEvent& mouse_event)
{
    if (!snap_available()) {
        if (m_snap_state != SnapState::Idle || m_snap_face.valid || m_snap_hover.valid)
            snap_reset();
        return false;
    }

    if (mouse_event.Moving()) {
        snap_update_hover();
        return false;
    }

    if (mouse_event.LeftDown()) {
        // Grabbers keep working, Ctrl/Cmd+click still edits the selection, Shift/Alt stay with the camera.
        if (m_hover_id != -1 || mouse_event.CmdDown() || mouse_event.ShiftDown() || mouse_event.AltDown())
            return false;
        SurfaceHit hit;
        if (!snap_raycast(true, hit))
            return false;
        if (!m_snap_face.valid)
            snap_set_face(m_snap_face, hit);
        m_snap_press_hit = hit;
        m_snap_press_pos = m_parent.get_local_mouse_position();
        m_snap_state     = SnapState::Pressed;
        m_parent.set_as_dirty();
        return true;
    }

    if (mouse_event.Dragging() && mouse_event.LeftIsDown() && m_snap_state != SnapState::Idle) {
        if (m_snap_state == SnapState::Pressed) {
            // A click picks a face, only a real drag moves the object.
            if ((m_parent.get_local_mouse_position() - m_snap_press_pos).norm() < 4.0)
                return true;
            snap_begin_drag();
        }
        if (m_snap_state == SnapState::Dragging)
            snap_update_drag();
        return true;
    }

    if (mouse_event.LeftUp() && m_snap_state != SnapState::Idle) {
        if (m_snap_state == SnapState::Pressed)
            snap_set_face(m_snap_face, m_snap_press_hit);
        else if (m_snap_moved)
            snap_commit();
        m_snap_state = SnapState::Idle;
        m_snap_moved = false;
        m_parent.set_as_dirty();
        return true;
    }

    return false;
}

void GLGizmoMove3D::snap_begin_drag()
{
    const GLVolume* first = m_parent.get_selection().get_first_volume();
    if (first == nullptr || !snap_face_world(m_snap_start_point, m_snap_start_normal)) {
        m_snap_state = SnapState::Idle;
        return;
    }
    m_snap_start_instance = first->get_instance_transformation().get_matrix();
    m_snap_start_volume   = first->get_volume_transformation().get_matrix();
    m_snap_spin           = 0.0;
    m_snap_has_target     = false;
    m_snap_moved          = false;
    m_snap_state          = SnapState::Dragging;
    // The hover preview would trail behind the moving object.
    m_snap_hover.valid = false;
    m_snap_hover.region.reset();
}

void GLGizmoMove3D::snap_apply(const Transform3d& world_delta)
{
    Selection&      selection = m_parent.get_selection();
    const GLVolume* first     = selection.get_first_volume();
    if (first == nullptr)
        return;
    const unsigned int object_idx   = (unsigned int) first->object_idx();
    const unsigned int instance_idx = (unsigned int) first->instance_idx();
    if (selection.is_single_volume_or_modifier()) {
        // The part moves inside its object: express the world delta in the instance frame.
        const Transform3d new_volume = m_snap_start_instance.inverse() * world_delta * m_snap_start_instance * m_snap_start_volume;
        selection.rotate(object_idx, instance_idx, (unsigned int) first->volume_idx(), new_volume);
    } else
        selection.rotate(object_idx, instance_idx, world_delta * m_snap_start_instance);
    m_parent.set_as_dirty();
}

void GLGizmoMove3D::snap_update_drag()
{
    SurfaceHit target;
    if (!snap_raycast(false, target))
        return; // off every other object: stay where the last surface put it
    m_snap_last_target = target;
    m_snap_has_target  = true;
    snap_apply(snap_delta(m_snap_start_point, m_snap_start_normal, target.point, target.normal, m_snap_spin));
    m_snap_moved = true;
}

bool GLGizmoMove3D::on_mouse_wheel_snap(const wxMouseEvent& evt)
{
    if (!m_snap_enabled || m_snap_state != SnapState::Dragging)
        return false;
    const double notches = (double) evt.GetWheelRotation() / (double) std::max(1, evt.GetWheelDelta());
    const double step    = evt.ShiftDown() ? 5.0 : 15.0;
    m_snap_spin += notches * step * PI / 180.0;
    if (m_snap_has_target) {
        snap_apply(snap_delta(m_snap_start_point, m_snap_start_normal, m_snap_last_target.point, m_snap_last_target.normal, m_snap_spin));
        m_snap_moved = true;
    }
    return true;
}

void GLGizmoMove3D::snap_commit()
{
    Selection&      selection = m_parent.get_selection();
    const GLVolume* first     = selection.get_first_volume();
    if (first == nullptr)
        return;

    if (selection.is_single_volume_or_modifier()) {
        m_parent.do_move(L("Snap to surface"));
        return;
    }

    // Written straight to the ModelInstance rather than through GLCanvas3D::do_move(): its
    // "fix flying instances" pass would drop an object snapped onto another one back to the bed.
    // Same approach as the Measure gizmo's assembly fit.
    const int object_idx   = first->object_idx();
    const int instance_idx = first->instance_idx();
    Model&    model        = wxGetApp().plater()->model();
    if (object_idx < 0 || object_idx >= (int) model.objects.size())
        return;
    ModelObject* model_object = model.objects[object_idx];
    if (instance_idx < 0 || instance_idx >= (int) model_object->instances.size())
        return;

    wxGetApp().plater()->take_snapshot(L("Snap to surface"));
    model_object->instances[instance_idx]->set_transformation(first->get_instance_transformation());
    model_object->invalidate_bounding_box();
    selection.notify_instance_update(object_idx, instance_idx);
    wxGetApp().obj_list()->update_info_items(size_t(object_idx));
    wxGetApp().obj_list()->update_plate_values_for_items();
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_INSTANCE_MOVED));
}

void GLGizmoMove3D::snap_spin_in_place(double angle)
{
    const GLVolume* first = m_parent.get_selection().get_first_volume();
    Vec3d point, normal;
    if (first == nullptr || m_snap_state != SnapState::Idle || !snap_face_world(point, normal))
        return;
    m_snap_start_instance = first->get_instance_transformation().get_matrix();
    m_snap_start_volume   = first->get_volume_transformation().get_matrix();
    Transform3d delta = Transform3d::Identity();
    delta.translate(point);
    delta.rotate(Eigen::AngleAxisd(angle, normal));
    delta.translate(-point);
    snap_apply(delta);
    snap_commit();
}

void GLGizmoMove3D::snap_reset()
{
    m_snap_state = SnapState::Idle;
    m_snap_moved = false;
    m_snap_has_target = false;
    m_snap_face.valid = false;
    m_snap_face.region.reset();
    m_snap_hover.valid = false;
    m_snap_hover.region.reset();
    m_snap_hover_region_facets.clear();
    m_snap_neighbors.clear();
}

void GLGizmoMove3D::render_snap_faces()
{
    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    const Camera&       camera  = wxGetApp().plater()->get_camera();
    const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;

    auto find_volume = [&volumes](const SnapFace& face) -> const GLVolume* {
        for (const GLVolume* v : volumes)
            if (v != nullptr && v->object_idx() == face.object_idx && v->instance_idx() == face.instance_idx && v->volume_idx() == face.volume_idx)
                return v;
        return nullptr;
    };

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-2.0f, -2.0f));
    glsafe(::glDisable(GL_CULL_FACE));
    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    auto render_face = [&](SnapFace& face, const ColorRGBA& color) {
        if (!face.valid || !face.region.is_initialized())
            return;
        const GLVolume* v = find_volume(face);
        if (v == nullptr)
            return;
        shader->set_uniform("view_model_matrix", camera.get_view_matrix() * v->world_matrix());
        face.region.set_color(color);
        face.region.render();
    };
    // While dragging the picked face is pressed against the target, so only the hover is useful before.
    if (m_snap_state != SnapState::Dragging)
        render_face(m_snap_hover, GLGizmoBase::FLATTEN_HOVER_COLOR);
    render_face(m_snap_face, ColorRGBA(0.92f, 0.50f, 0.26f, 0.8f));

    shader->stop_using();
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));
}

void GLGizmoMove3D::render_snap_to_surface_ui(ImGuiWrapper* imgui, float wrap_width)
{
    ImGui::Separator();
    bool enabled = m_snap_enabled;
    if (imgui->checkbox(_L("Snap face to surface"), enabled)) {
        m_snap_enabled = enabled;
        snap_reset();
        m_parent.set_as_dirty();
    }
    if (ImGui::IsItemHovered())
        imgui->tooltip(_L("Pick a face of the selected object or part, then drag it onto another object. The face stays flush "
                          "with the surface under the cursor and the selection turns to follow it. An object snapped onto another "
                          "one is not dropped back to the bed; merge them into one object to keep them together."),
                       wrap_width);
    if (!m_snap_enabled)
        return;

    if (!snap_available()) {
        imgui->text_wrapped(_L("Select a single object or part to snap it."), wrap_width);
        return;
    }
    if (!m_snap_face.valid) {
        imgui->text_wrapped(_L("Click a face of the selected object to use as the contact face."), wrap_width);
        return;
    }
    imgui->text_wrapped(_L("Drag onto another object's surface. Scroll while dragging to spin (Shift for finer steps). "
                           "Click another face to change the contact face."),
                        wrap_width);
    ImGui::AlignTextToFramePadding();
    imgui->text(_L("Spin"));
    ImGui::SameLine();
    if (imgui->button(wxString::FromUTF8("-15\xC2\xB0")))
        snap_spin_in_place(-15.0 * PI / 180.0);
    ImGui::SameLine();
    if (imgui->button(wxString::FromUTF8("+15\xC2\xB0")))
        snap_spin_in_place(15.0 * PI / 180.0);
    ImGui::SameLine();
    if (imgui->button(_L("Clear face"))) {
        m_snap_face.valid = false;
        m_snap_face.region.reset();
        m_parent.set_as_dirty();
    }
}

} // namespace GUI
} // namespace Slic3r
