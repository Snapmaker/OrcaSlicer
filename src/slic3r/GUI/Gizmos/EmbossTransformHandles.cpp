#include "EmbossTransformHandles.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/SurfaceDrag.hpp"
#include "slic3r/GUI/EmbossFreeTransform.hpp"
#include "slic3r/GUI/EmbossPicking.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"

#include <imgui/imgui.h>
#include <glad/gl.h>
#include <wx/utils.h>

#include <cmath>

namespace Slic3r::GUI {

namespace {

// GLGizmoRotate::Offset and GLGizmoRotate::GrabberOffset: the rings are drawn at the bounding sphere
// radius plus this many mm, their grabbers that fraction of the radius further out.
constexpr double EMBOSS_HANDLES_RING_OFFSET  = 5.0;
constexpr double EMBOSS_HANDLES_RING_GRABBER = 0.15;
// Move arrows: the line starts this many grabber sizes beyond the ring grabbers, the cone sits at
// the end of a line this many grabber sizes long.
constexpr double EMBOSS_HANDLES_ARROW_GAP    = 1.0;
constexpr double EMBOSS_HANDLES_ARROW_LENGTH = 4.0;
// SHIFT while moving snaps to multiples of this [mm] (as the Move tool does).
constexpr double EMBOSS_HANDLES_MOVE_SNAP = 1.0;

// app config: handles shown ("0" = off, anything else on), arrows along the world axes ("1")
const char *const EMBOSS_HANDLES_CONFIG_KEY       = "emboss_3d_handles";
const char *const EMBOSS_HANDLES_WORLD_CONFIG_KEY = "emboss_3d_handles_world";

// Starting angle of the X and Y rings: their grabbers sit in the part's plane (the X ring's at -Y,
// the Y ring's at -X), clear of the host's ring grabber (+Y) and of the arrows (+X, +Y).
constexpr double EMBOSS_HANDLES_RING_ANGLE[2] = { 0., 1.5 * PI };

Vec3d emboss_handles_unit(const Vec3d &v, const Vec3d &fallback)
{
    const double len = v.norm();
    return len > 0. ? Vec3d(v / len) : fallback;
}

} // namespace

EmbossTransformHandles::EmbossTransformHandles(GLCanvas3D &parent)
    : GLGizmoBase(parent, "", -1)
    , m_rings({ GLGizmoRotate(parent, GLGizmoRotate::X), GLGizmoRotate(parent, GLGizmoRotate::Y) })
{
    m_rings[0].set_group_id(RotateX);
    m_rings[1].set_group_id(RotateY);
    for (GLGizmoRotate &ring : m_rings)
        ring.set_force_local_coordinate(true);
}

bool EmbossTransformHandles::on_init()
{
    for (int i = 0; i < 3; ++i) {
        m_grabbers.push_back(Grabber());
        m_grabbers.back().extensions = GLGizmoBase::EGrabberExtension::PosZ;
    }
    // cones along X and Y (the cone points along its Z), as in GLGizmoMove3D
    m_grabbers[0].angles = { 0.0, 0.5 * double(PI), 0.0 };
    m_grabbers[1].angles = { -0.5 * double(PI), 0.0, 0.0 };

    for (size_t i = 0; i < m_rings.size(); ++i) {
        if (!m_rings[i].init())
            return false;
        m_rings[i].set_highlight_color(AXES_COLOR[i]);
        m_rings[i].set_angle(EMBOSS_HANDLES_RING_ANGLE[i]);
    }

    if (const AppConfig *config = wxGetApp().app_config; config != nullptr) {
        // on unless switched off
        m_enabled    = config->get(EMBOSS_HANDLES_CONFIG_KEY) != "0";
        m_world_axes = config->get(EMBOSS_HANDLES_WORLD_CONFIG_KEY) == "1";
    }
    return true;
}

bool EmbossTransformHandles::draw_options(ImGuiWrapper &imgui, EmbossFreeTransform::Placement placement, double distance,
                                          bool reattach_pending, float tooltip_width)
{
    using EmbossFreeTransform::Placement;

    std::string line;
    bool        away = false;
    switch (placement) {
    case Placement::Object: break;
    case Placement::Projected: line = _u8L("Placement: projected onto the surface"); break;
    case Placement::OnSurface: line = _u8L("Placement: on the surface"); break;
    case Placement::Floating:
        line = GUI::format(_u8L("Placement: %1% mm from the surface"), Slic3r::string_printf("%.2f", distance));
        break;
    case Placement::Tilted: line = _u8L("Placement: tilted against the surface"); away = true; break;
    case Placement::Free: line = _u8L("Placement: free, not on a surface"); away = true; break;
    }
    if (!line.empty()) {
        // wrap at the window edge: the window sizes itself to its other rows
        ImGui::PushTextWrapPos(0.f);
        ImGuiWrapper::text(line);
        if (away)
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_GREY_LIGHT,
                                       reattach_pending ?
                                           _u8L("Drag it over the surface to put it back; \"Use surface\" then turns on again.") :
                                           _u8L("Drag it over the surface to put it back."));
        ImGui::PopTextWrapPos();
    }

    bool changed = false;
    bool enabled = m_enabled;
    if (imgui.bbl_checkbox(_L("Move and rotate handles"), enabled)) {
        m_enabled = enabled;
        changed   = true;
        if (AppConfig *config = wxGetApp().app_config; config != nullptr)
            config->set(EMBOSS_HANDLES_CONFIG_KEY, m_enabled ? "1" : "0");
    }
    if (ImGui::IsItemHovered())
        imgui.tooltip(_u8L("Arrows move the part along its own axes; the blue one lifts it off the surface or pushes it in. "
                           "Rings turn it around its own axes. Lifting, pushing or tilting a part that uses the surface "
                           "turns \"Use surface\" off; a drag over the surface puts it back.").c_str(),
                      tooltip_width);
    if (m_enabled) {
        bool world = m_world_axes;
        if (imgui.bbl_checkbox(_L("Arrows along the world axes"), world)) {
            m_world_axes = world;
            changed      = true;
            if (AppConfig *config = wxGetApp().app_config; config != nullptr)
                config->set(EMBOSS_HANDLES_WORLD_CONFIG_KEY, m_world_axes ? "1" : "0");
        }
        if (ImGui::IsItemHovered())
            imgui.tooltip(_u8L("Move along the X, Y and Z of the printer instead of the part's own axes. The rings always "
                               "turn around the part's own axes.").c_str(),
                          tooltip_width);
    }
    if (changed)
        m_parent.set_as_dirty();
    return changed;
}

void EmbossTransformHandles::on_set_state()
{
    for (GLGizmoRotate &ring : m_rings)
        ring.set_state(m_state);
    if (m_state == Off)
        m_drag_id = -1;
}

void EmbossTransformHandles::set_visible(bool visible)
{
    if (m_visible == visible)
        return;
    m_visible = visible;
    sync_picking();
}

void EmbossTransformHandles::on_host_register()
{
    m_host_registered = true;
    sync_picking();
}

void EmbossTransformHandles::on_host_unregister()
{
    m_host_registered = false;
    sync_picking();
}

void EmbossTransformHandles::sync_picking()
{
    const bool wanted = m_host_registered && m_visible;
    if (wanted == m_registered)
        return;
    // Grabbers create their raycasters on the next render, under the id set here.
    if (wanted)
        register_raycasters_for_picking();
    else
        unregister_raycasters_for_picking();
    m_registered = wanted;
    m_parent.set_as_dirty();
}

void EmbossTransformHandles::on_register_raycasters_for_picking()
{
    // GLGizmoBase gave the arrows the ids 0-2; the host's ring owns 0, so move them to 1-3.
    for (size_t i = 0; i < m_grabbers.size(); ++i)
        m_grabbers[i].register_raycasters_for_picking(MoveX + int(i));
    for (GLGizmoRotate &ring : m_rings)
        ring.register_raycasters_for_picking();
}

void EmbossTransformHandles::on_unregister_raycasters_for_picking()
{
    for (GLGizmoRotate &ring : m_rings)
        ring.unregister_raycasters_for_picking();
}

void EmbossTransformHandles::set_host_hover_id(int id)
{
    if (m_drag_id >= 0)
        return; // keep the dragged handle highlighted
    m_hover_id = is_move(id) ? id - MoveX : -1;
    m_rings[0].set_hover_id(id == RotateX ? 0 : -1);
    m_rings[1].set_hover_id(id == RotateY ? 0 : -1);
}

std::optional<EmbossTransformHandles::Frame> EmbossTransformHandles::calc_frame() const
{
    const Selection &selection = m_parent.get_selection();
    if (selection.is_empty() || selection.volumes_count() != 1)
        return {};

    // The part's own axes from its world matrix (without the 3MF fix): Z is the emboss direction,
    // X along the text line. Made orthonormal, so scaled or skewed parts still get square arrows.
    const Transform3d world  = world_matrix_fixed(selection);
    const Matrix3d    linear = world.linear();
    Frame frame;
    frame.normal = emboss_handles_unit(linear.inverse().transpose() * Vec3d::UnitZ(), Vec3d::UnitZ());
    if (m_world_axes)
        frame.matrix = Transform3d::Identity();
    else {
        Vec3d x = linear.col(0);
        x -= frame.normal * frame.normal.dot(x);
        x = emboss_handles_unit(x, frame.normal.unitOrthogonal());
        const Vec3d y = frame.normal.cross(x);
        frame.matrix = Transform3d::Identity();
        frame.matrix.linear().col(0) = x;
        frame.matrix.linear().col(1) = y;
        frame.matrix.linear().col(2) = frame.normal;
    }

    // Same centre and radius as the rings (GLGizmoRotate::init_data_from_selection).
    const std::pair<Vec3d, double> sphere = selection.get_bounding_sphere();
    frame.matrix.translation()            = sphere.first;
    frame.ring_radius                     = EMBOSS_HANDLES_RING_OFFSET + sphere.second;
    frame.arrow_length = frame.ring_radius * (1. + EMBOSS_HANDLES_RING_GRABBER) +
                         (EMBOSS_HANDLES_ARROW_GAP + EMBOSS_HANDLES_ARROW_LENGTH) * double(Grabber::FixedGrabberSize) * double(INV_ZOOM);
    return frame;
}

void EmbossTransformHandles::set_arrow_picking_active(int i, bool active)
{
    for (const std::shared_ptr<SceneRaycasterItem> &raycaster : m_grabbers[i].raycasters)
        if (raycaster != nullptr)
            raycaster->set_active(active);
}

void EmbossTransformHandles::on_render()
{
    if (!m_registered)
        return;
    const std::optional<Frame> frame = calc_frame();
    if (!frame.has_value())
        return;

    const Camera &camera = wxGetApp().plater()->get_camera();
    const Vec3d   center = frame->matrix.translation();

    // Which arrows: not the one pointing at the camera, and none that would sit over the part's
    // outline, which belongs to the surface drag. While dragging only the dragged handle is drawn.
    std::array<bool, 3> shown{ false, false, false };
    std::optional<Polygon> footprint;
    for (int i = 0; i < 3; ++i) {
        if (m_drag_id >= 0) {
            shown[i] = m_drag_id == MoveX + i;
            continue;
        }
        const Vec3d axis = frame->matrix.linear().col(i);
        if (!EmbossFreeTransform::arrow_visible(axis, camera.get_dir_forward()))
            continue;
        if (!footprint.has_value()) {
            const GLVolume *gl_volume = m_parent.get_selection().get_first_volume();
            footprint = gl_volume != nullptr ? CameraUtils::create_hull2d(camera, *gl_volume) : Polygon();
        }
        const Point  tip     = CameraUtils::project(camera, Vec3d(center + axis * frame->arrow_length));
        const double padding = EmbossPicking::FOOTPRINT_PADDING_PX * m_parent.get_canvas_size().get_scale_factor() +
                               double(Grabber::FixedGrabberSize);
        shown[i] = !EmbossPicking::footprint_contains(*footprint, Vec2d(double(tip.x()), double(tip.y())), padding);
    }

    glsafe(::glEnable(GL_DEPTH_TEST));

    for (int i = 0; i < 3; ++i) {
        Grabber &grabber    = m_grabbers[i];
        grabber.enabled     = shown[i];
        grabber.matrix      = frame->matrix;
        grabber.center      = Vec3d::Unit(i) * frame->arrow_length;
        grabber.color       = AXES_COLOR[i];
        grabber.hover_color = AXES_HOVER_COLOR[i];
        // a hidden arrow keeps its raycaster where it was last drawn: switch it off
        set_arrow_picking_active(i, shown[i]);
    }

    render_arrow_lines(*frame, shown);
    render_grabbers(0, 2, float(Grabber::FixedGrabberSize), false);

    for (size_t i = 0; i < m_rings.size(); ++i)
        if (m_drag_id < 0 || m_drag_id == RotateX + int(i))
            m_rings[i].render();
}

void EmbossTransformHandles::render_arrow_lines(const Frame &frame, const std::array<bool, 3> &shown)
{
#if SLIC3R_OPENGL_ES
    GLShaderProgram *shader = wxGetApp().get_shader("dashed_lines");
#else
    GLShaderProgram *shader = OpenGLManager::get_gl_info().is_core_profile() ? wxGetApp().get_shader("dashed_thick_lines") :
                                                                              wxGetApp().get_shader("flat");
#endif // SLIC3R_OPENGL_ES
    if (shader == nullptr)
        return;

#if !SLIC3R_OPENGL_ES
    if (!OpenGLManager::get_gl_info().is_core_profile())
        glsafe(::glLineWidth((m_hover_id != -1) ? 2.0f : 1.5f));
#endif // !SLIC3R_OPENGL_ES

    shader->start_using();
    const Camera &camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * frame.matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        const std::array<int, 4> &viewport = camera.get_viewport();
        shader->set_uniform("viewport_size", Vec2d(double(viewport[2]), double(viewport[3])));
        shader->set_uniform("width", 0.25f);
        shader->set_uniform("gap_size", 0.0f);
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES

    const double grabber = double(Grabber::FixedGrabberSize) * double(INV_ZOOM);
    const float  from    = float(frame.ring_radius * (1. + EMBOSS_HANDLES_RING_GRABBER) + EMBOSS_HANDLES_ARROW_GAP * grabber);
    const float  to      = float(frame.arrow_length);
    for (int i = 0; i < 3; ++i) {
        if (!shown[i])
            continue;
        ArrowLine  &line   = m_lines[i];
        const Vec3f from_v = Vec3f::Unit(i) * from;
        const Vec3f to_v   = Vec3f::Unit(i) * to;
        if (!line.model.is_initialized() || !line.from.isApprox(from_v) || !line.to.isApprox(to_v)) {
            line.model.reset();
            line.from = from_v;
            line.to   = to_v;
            GLModel::Geometry init_data;
            init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
            init_data.color  = AXES_COLOR[i];
            init_data.reserve_vertices(2);
            init_data.reserve_indices(2);
            init_data.add_vertex(from_v);
            init_data.add_vertex(to_v);
            init_data.add_line(0, 1);
            line.model.init_from(std::move(init_data));
        }
        line.model.render();
    }
    shader->stop_using();
}

bool EmbossTransformHandles::start_drag(int id)
{
    m_drag_id = -1;
    if (!is_handle(id))
        return false;

    Selection &selection = m_parent.get_selection();
    if (selection.volumes_count() != 1)
        return false;
    const GLVolume *gl_volume = get_selected_gl_volume(selection);
    if (gl_volume == nullptr)
        return false;
    const std::optional<Frame> frame = calc_frame();
    if (!frame.has_value())
        return false;

    m_object_idx     = gl_volume->object_idx();
    m_instance_idx   = gl_volume->instance_idx();
    m_volume_idx     = gl_volume->volume_idx();
    m_start_instance = gl_volume->get_instance_transformation().get_matrix();
    m_start_volume   = gl_volume->get_volume_transformation().get_matrix();
    m_start_normal   = frame->normal;
    m_distance       = 0.;
    m_angle          = 0.;

    if (is_move(id)) {
        const int i   = id - MoveX;
        m_axis        = frame->matrix.linear().col(i);
        m_start_point = frame->matrix.translation() + m_axis * frame->arrow_length;
        m_hover_id    = i;
        m_grabbers[i].dragging = true;
    } else {
        GLGizmoRotate &ring = m_rings[id - RotateX];
        ring.set_hover_id(0);
        ring.start_dragging(); // takes the ring's centre and orientation from the selection
        m_start_ring_angle = ring.get_angle();
        m_axis             = ring.get_world_axis();
        m_pivot            = ring.get_center();
        m_hover_id         = -1;
    }
    m_drag_id = id;
    return true;
}

void EmbossTransformHandles::drag(const UpdateData &data)
{
    if (m_drag_id < 0)
        return;

    Transform3d delta = Transform3d::Identity();
    if (is_move(m_drag_id)) {
        const double snap = wxGetKeyState(WXK_SHIFT) ? EMBOSS_HANDLES_MOVE_SNAP : 0.;
        m_distance = EmbossFreeTransform::axis_drag_distance(m_start_point, m_axis, data.mouse_ray.a, data.mouse_ray.unit_vector(), snap);
        delta.translate(m_axis * m_distance);
    } else {
        GLGizmoRotate &ring = m_rings[m_drag_id - RotateX];
        ring.dragging(data);
        m_angle = EmbossFreeTransform::angle_delta(m_start_ring_angle, ring.get_angle());
        delta   = EmbossFreeTransform::rotation_about(m_pivot, m_axis, m_angle);
    }

    // The volume transformation is shared by every instance of the object: the other instances follow.
    const Transform3d volume = EmbossFreeTransform::moved_volume_matrix(m_start_instance, m_start_volume, delta);
    Selection &selection = m_parent.get_selection();
    selection.rotate(unsigned(m_object_idx), unsigned(m_instance_idx), unsigned(m_volume_idx), volume);
    selection.synchronize_unselected_volumes();
    m_parent.set_as_dirty();
}

std::optional<EmbossTransformHandles::Result> EmbossTransformHandles::stop_drag()
{
    if (m_drag_id < 0)
        return {};
    Result res;
    res.id     = m_drag_id;
    res.normal = m_start_normal;
    if (is_move(m_drag_id)) {
        m_grabbers[m_drag_id - MoveX].dragging = false;
        res.displacement = m_axis * m_distance;
    } else {
        const size_t i = size_t(m_drag_id - RotateX);
        m_rings[i].stop_dragging();
        // the grabber goes back to its place, as the host's ring does after a turn
        m_rings[i].set_angle(EMBOSS_HANDLES_RING_ANGLE[i]);
        m_rings[i].set_hover_id(-1);
        res.rotation = true;
        res.axis     = m_axis;
        res.angle    = m_angle;
    }
    // Nothing moved (a click on a handle): put the exact start back, the host records no undo step.
    const bool moved = res.rotation ? std::abs(m_angle) >= 1e-9 : std::abs(m_distance) >= EmbossFreeTransform::MOVE_EPSILON;
    if (!moved) {
        res.displacement     = Vec3d::Zero();
        res.angle            = 0.;
        Selection &selection = m_parent.get_selection();
        selection.rotate(unsigned(m_object_idx), unsigned(m_instance_idx), unsigned(m_volume_idx), m_start_volume);
        selection.synchronize_unselected_volumes();
        m_parent.set_as_dirty();
    }
    m_drag_id  = -1;
    m_hover_id = -1;
    return res;
}

std::string EmbossTransformHandles::get_tooltip() const
{
    static const char *axis_names[3] = { "X", "Y", "Z" };
    const int id = m_drag_id >= 0 ? m_drag_id : (m_hover_id >= 0 ? MoveX + m_hover_id : -1);
    if (is_move(id)) {
        const int i = id - MoveX;
        std::string name = axis_names[i];
        if (!m_world_axes && i == 2)
            name = _u8L("Off the surface");
        if (m_drag_id < 0)
            return name;
        return name + ": " + format(float(m_distance), 2) + " mm";
    }
    if (is_rotate(m_drag_id))
        return std::string(axis_names[m_drag_id - RotateX]) + ": " + format(float(Geometry::rad2deg(m_angle)), 2) + u8"°";
    for (size_t i = 0; i < m_rings.size(); ++i)
        if (m_rings[i].get_hover_id() == 0)
            return std::string(axis_names[i]);
    return {};
}

} // namespace Slic3r::GUI
