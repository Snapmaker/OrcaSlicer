#include "GLGizmoCut.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
// OpenGLManager::get_gl_info().is_mesa(), for the mm_contour shader's clip-space
// bias the drawn stroke's ribbon uses.
#include "slic3r/GUI/OpenGLManager.hpp"

#include <glad/gl.h>

#include <algorithm>
#include <limits>
#include <cmath>

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Gizmos/GizmoObjectManipulation.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

#include "imgui/imgui_internal.h"
#include "slic3r/GUI/Field.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "FixModelByWin10.hpp"

namespace Slic3r {
namespace GUI {

static const ColorRGBA GRABBER_COLOR = ColorRGBA::YELLOW();
static const ColorRGBA UPPER_PART_COLOR = ColorRGBA::CYAN();
static const ColorRGBA LOWER_PART_COLOR = ColorRGBA::MAGENTA();
static const ColorRGBA MODIFIER_COLOR   = ColorRGBA(0.75f, 0.75f, 0.75f, 0.5f);

// connector colors
static const ColorRGBA PLAG_COLOR           = ColorRGBA::YELLOW();
static const ColorRGBA DOWEL_COLOR          = ColorRGBA::DARK_YELLOW();
static const ColorRGBA HOVERED_PLAG_COLOR   = ColorRGBA::CYAN();
static const ColorRGBA HOVERED_DOWEL_COLOR  = ColorRGBA(0.0f, 0.5f, 0.5f, 1.0f);
static const ColorRGBA SELECTED_PLAG_COLOR  = ColorRGBA::GRAY();
static const ColorRGBA SELECTED_DOWEL_COLOR = ColorRGBA::DARK_GRAY();
static const ColorRGBA CONNECTOR_DEF_COLOR  = ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f);
static const ColorRGBA CONNECTOR_ERR_COLOR  = ColorRGBA(1.0f, 0.3f, 0.3f, 0.5f);
static const ColorRGBA HOVERED_ERR_COLOR    = ColorRGBA(1.0f, 0.3f, 0.3f, 1.0f);

static const ColorRGBA CUT_PLANE_DEF_COLOR  = ColorRGBA(0.9f, 0.9f, 0.9f, 0.5f);
static const ColorRGBA CUT_PLANE_ERR_COLOR  = ColorRGBA(1.0f, 0.8f, 0.8f, 0.5f);

const unsigned int AngleResolution = 64;
const unsigned int ScaleStepsCount = 72;
const float ScaleStepRad = 2.0f * float(PI) / ScaleStepsCount;
const unsigned int ScaleLongEvery = 2;
const float ScaleLongTooth = 0.1f; // in percent of radius
const unsigned int SnapRegionsCount = 8;

const float         UndefFloat = -999.f;
const std::string   UndefLabel = " ";

using namespace Geometry;

// Generates mesh for a line
static GLModel::Geometry its_make_line(Vec3f beg_pos, Vec3f end_pos)
{
    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(2);
    init_data.reserve_indices(2);

    // vertices
    init_data.add_vertex(beg_pos);
    init_data.add_vertex(end_pos);

    // indices
    init_data.add_line(0, 1);
    return init_data;
}

//! -- #ysFIXME those functions bodies are ported from GizmoRotation
// Generates mesh for a circle 
static void init_from_circle(GLModel& model, double radius)
{
    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::LineLoop, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(ScaleStepsCount);
    init_data.reserve_indices(ScaleStepsCount);

    // vertices + indices
    for (unsigned int i = 0; i < ScaleStepsCount; ++i) {
        const float angle = float(i * ScaleStepRad);
        init_data.add_vertex(Vec3f(::cos(angle) * float(radius), ::sin(angle) * float(radius), 0.0f));
        init_data.add_index(i);
    }

    model.init_from(std::move(init_data));
    model.set_color(ColorRGBA::WHITE());
}

// Generates mesh for a scale
static void init_from_scale(GLModel& model, double radius)
{
    const float out_radius_long  = float(radius) * (1.0f + ScaleLongTooth);
    const float out_radius_short = float(radius) * (1.0f + 0.5f * ScaleLongTooth);

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(2 * ScaleStepsCount);
    init_data.reserve_indices(2 * ScaleStepsCount);

    // vertices + indices
    for (unsigned int i = 0; i < ScaleStepsCount; ++i) {
        const float angle = float(i * ScaleStepRad);
        const float cosa = ::cos(angle);
        const float sina = ::sin(angle);
        const float in_x = cosa * float(radius);
        const float in_y = sina * float(radius);
        const float out_x = (i % ScaleLongEvery == 0) ? cosa * out_radius_long : cosa * out_radius_short;
        const float out_y = (i % ScaleLongEvery == 0) ? sina * out_radius_long : sina * out_radius_short;

        // vertices
        init_data.add_vertex(Vec3f(in_x, in_y, 0.0f));
        init_data.add_vertex(Vec3f(out_x, out_y, 0.0f));

        // indices
        init_data.add_line(i * 2, i * 2 + 1);
    }

    model.init_from(std::move(init_data));
    model.set_color(ColorRGBA::WHITE());
}

// Generates mesh for a snap_radii
static void init_from_snap_radii(GLModel& model, double radius)
{
    const float step = 2.0f * float(PI) / float(SnapRegionsCount);
    const float in_radius = float(radius) / 3.0f;
    const float out_radius = 2.0f * in_radius;

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(2 * ScaleStepsCount);
    init_data.reserve_indices(2 * ScaleStepsCount);

    // vertices + indices
    for (unsigned int i = 0; i < ScaleStepsCount; ++i) {
        const float angle = float(i) * step;
        const float cosa = ::cos(angle);
        const float sina = ::sin(angle);
        const float in_x = cosa * in_radius;
        const float in_y = sina * in_radius;
        const float out_x = cosa * out_radius;
        const float out_y = sina * out_radius;

        // vertices
        init_data.add_vertex(Vec3f(in_x, in_y, 0.0f));
        init_data.add_vertex(Vec3f(out_x, out_y, 0.0f));

        // indices
        init_data.add_line(i * 2, i * 2 + 1);
    }

    model.init_from(std::move(init_data));
    model.set_color(ColorRGBA::WHITE());
}

// Generates mesh for a angle_arc
static void init_from_angle_arc(GLModel& model, double angle, double radius)
{
    model.reset();

    const float step_angle = float(angle) / float(AngleResolution);
    const float ex_radius = float(radius);

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(1 + AngleResolution);
    init_data.reserve_indices(1 + AngleResolution);

    // vertices + indices
    for (unsigned int i = 0; i <= AngleResolution; ++i) {
        const float angle = float(i) * step_angle;
        init_data.add_vertex(Vec3f(::cos(angle) * ex_radius, ::sin(angle) * ex_radius, 0.0f));
        init_data.add_index(i);
    }

    model.init_from(std::move(init_data));
}

//! --

GLGizmoCut3D::GLGizmoCut3D(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
    , m_connectors_group_id (GrabberID::Count)
    , m_connector_type (CutConnectorType::Plug)
    , m_connector_style (int(CutConnectorStyle::Prism))
    , m_connector_shape_id (int(CutConnectorShape::Circle))
{
    m_modes = { _u8L("Planar"), _u8L("Dovetail")//, _u8L("Grid")
//              , _u8L("Radial"), _u8L("Modular")
    };

    m_connector_modes = { _u8L("Auto"), _u8L("Manual") };

    // NOTE: indexed by CutConnectorType, so the Undef slot has to be filled before FlexiJoint.
    m_connector_types = { _u8L("Plug"), _u8L("Dowel"), _u8L("Snap"), "", _u8L("Flexi") };

    // NOTE: indexed by FlexiJointKind, so the order here has to follow the enum exactly.
    m_flexi_kinds = { _u8L("Double ring"), _u8L("Ball & socket"), _u8L("Chain link"), _u8L("Hinge"),
                      _u8L("Thread"), _u8L("Bayonet") };

    m_connector_styles = { _u8L("Prism"), _u8L("Frustum")
//              , _u8L("Claw")
    };

    m_connector_shapes = { _u8L("Triangle"), _u8L("Square"), _u8L("Hexagon"), _u8L("Circle")
//              , _u8L("D-shape")
    };

    m_axis_names = { "X", "Y", "Z" };

    m_part_orientation_names = {
        {"none",    _L("Keep orientation")},
        {"on_cut",  _L("Place on cut")},
        {"flip",    _L("Flip upside down")},
    };

    m_labels_map = {
        {"Connectors"   , _u8L("Connectors")},
        {"Type"         , _u8L("Type")},
        {"Style"        , _u8L("Style")},
        {"Shape"        , _u8L("Shape")},
        {"Depth"        , _u8L("Depth")},
        {"Size"         , _u8L("Size")},
        {"Rotation"     , _u8L("Rotation")},
        {"Groove"       , _u8L("Groove")},
        {"Width"        , _u8L("Width")},
        {"Flap Angle"   , _u8L("Flap Angle")},
        {"Groove Angle" , _u8L("Groove Angle")},
        {"Joint"        , _u8L("Joint")},
        {"Outer radius" , _u8L("Outer radius")},
        {"Ring width"   , _u8L("Ring width")},
        {"Ring height"  , _u8L("Ring height")},
        {"Clearance"    , _u8L("Clearance")},
        {"Hub radius"   , _u8L("Hub radius")},
        {"Tilt"         , _u8L("Tilt allowance")},
        {"Ball radius"  , _u8L("Ball radius")},
        {"Opening"      , _u8L("Opening angle")},
        {"Gap"          , _u8L("Gap")},
        {"Link length"  , _u8L("Link length")},
        {"Link width"   , _u8L("Link width")},
        {"Wire"         , _u8L("Wire thickness")},
        {"Link tilt"    , _u8L("Loop tilt")},
        {"Stem"         , _u8L("Stem depth")},
        {"Knuckles"     , _u8L("Knuckles")},
        {"Pin dia"      , _u8L("Pin diameter")},
        {"Barrel dia"   , _u8L("Barrel diameter")},
        {"Hinge length" , _u8L("Hinge length")},
        {"Edge offset"  , _u8L("Edge offset")},
        {"Major dia"    , _u8L("Major diameter")},
        {"Pitch"        , _u8L("Pitch")},
        {"Starts"       , _u8L("Starts")},
        {"Turns"        , _u8L("Turns")},
        {"Lead-in"      , _u8L("Lead-in turns")},
        {"Lugs"         , _u8L("Lugs")},
        {"Lug height"   , _u8L("Lug height")},
        {"Lug thickness", _u8L("Lug thickness")},
        {"Lug arc"      , _u8L("Lug width")},
        {"Lock angle"   , _u8L("Lock angle")},
        {"Entry depth"  , _u8L("Entry depth")},
        {"Detent"       , _u8L("Detent")},
    };

//    update_connector_shape();
}

std::string GLGizmoCut3D::get_tooltip() const
{
    std::string tooltip;
    if (m_hover_id == Z || (m_dragging && m_hover_id == CutPlane)) {
        double koef = m_imperial_units ? GizmoObjectManipulation::mm_to_in : 1.0;
        std::string unit_str = " " + (m_imperial_units ? _u8L("in") : _u8L("mm"));
        const BoundingBoxf3& tbb = m_transformed_bounding_box;

        const std::string name = m_keep_as_parts ? _u8L("Part") : _u8L("Object");
        if (tbb.max.z() >= 0.0) {
            double top = (tbb.min.z() <= 0.0 ? tbb.max.z() : tbb.size().z()) * koef;
            tooltip += format(static_cast<float>(top), 2) + " " + unit_str + " (" + name + " A)";
            if (tbb.min.z() <= 0.0)
                tooltip += "\n";
        }
        if (tbb.min.z() <= 0.0) {
            double bottom = (tbb.max.z() <= 0.0 ? tbb.size().z() : (tbb.min.z() * (-1))) * koef;
            tooltip += format(static_cast<float>(bottom), 2) + " " + unit_str + " (" + name + " B)";
        }
        return tooltip;
    }

    // Only while the pointer is on the surface AS DRAWN - the picking quad reaches
    // far past it, and promising "drag to move the cut plane" out there was the
    // visible half of the accidental-drag report.
    if (!m_dragging && m_hover_id == CutPlane && m_cut_surface_hovered) {
        if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
            return _u8L("Click to flip the cut plane\n"
                        "Drag to move the cut plane");
        return _u8L("Click to flip the cut plane\n"
                    "Drag to move the cut plane\n"
                    "Right-click a part to assign it to the other side");
    }

    if (tooltip.empty() && (m_hover_id == X || m_hover_id == Y || m_hover_id == CutPlaneZRotation)) {
        std::string axis = m_hover_id == X ? "X" : m_hover_id == Y ? "Y" : "Z";
        return axis + ": " + format(float(rad2deg(m_angle)), 1) + "°";
    }

    return tooltip;
}

bool GLGizmoCut3D::on_mouse(const wxMouseEvent &mouse_event)
{
    Vec2i32 mouse_coord(mouse_event.GetX(), mouse_event.GetY());
    Vec2d mouse_pos = mouse_coord.cast<double>();

    // Pick-face mode (armed via the "Pick face" button). A click means "pick that facet"
    // and is consumed so grabbers/pan don't also fire; hover just updates the highlight.
    if (m_facet_picker.is_active() && !m_connectors_editing) {
        if (mouse_event.Moving() || mouse_event.Dragging()) {
            m_facet_picker.update(mouse_pos, m_c, m_parent.get_selection(), wxGetApp().plater()->get_camera());
            m_parent.request_extra_frame();
        } else if (mouse_event.LeftDown()) {
            m_facet_picker.update(mouse_pos, m_c, m_parent.get_selection(), wxGetApp().plater()->get_camera());
            if (apply_picked_facet())
                m_facet_picker.set_active(false); // one-shot; press the button again to pick another
            return true;
        } else if (mouse_event.Leaving()) {
            m_facet_picker.reset();
        }
    }

    // Keep the cut-surface hover flag in step with the pointer, so the highlight
    // obeys the same bound the click does (see mouse_on_cut_surface). Done here,
    // before any early return below, because a plain Moving() event bails out of
    // this function long before the grabber code runs.
    if (mouse_event.Moving() || mouse_event.Dragging() || mouse_event.LeftDown() || mouse_event.RightDown()) {
        const bool on_surface = m_hover_id == CutPlane && !m_connectors_editing && mouse_on_cut_surface(mouse_pos);
        if (on_surface != m_cut_surface_hovered) {
            m_cut_surface_hovered = on_surface;
            m_parent.set_as_dirty();
        }
    }

    // Curved surface: a control-point drag wins over the plane grabbers, so a
    // click that lands on a handle bends the sheet instead of moving the plane.
    if (curved_on_mouse(mouse_event))
        return true;

    // Draw surface: a left drag on the model PAINTS the cut line, so it has to
    // win over everything the plane would otherwise do with a drag. It claims the
    // event only when the ray actually hits the model, so a drag on empty space
    // still orbits the camera.
    if (draw_on_mouse(mouse_event))
        return true;

    if (mouse_event.ShiftDown() && mouse_event.LeftDown())
        return gizmo_event(SLAGizmoEventType::LeftDown, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), mouse_event.CmdDown());
    if (mouse_event.CmdDown() && mouse_event.LeftDown())
        return false;
    if (cut_line_processing()) {
        if (mouse_event.ShiftDown()) {
            if (mouse_event.Moving()|| mouse_event.Dragging())
                return gizmo_event(SLAGizmoEventType::Moving, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), mouse_event.CmdDown());
            if (mouse_event.LeftUp())
                return gizmo_event(SLAGizmoEventType::LeftUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), mouse_event.CmdDown());
        }
        discard_cut_line_processing();
    }
    else if (mouse_event.Moving())
        return false;

    // BOUND THE PLANE DRAG. m_hover_id == CutPlane comes from a picking quad that
    // is 1.5x the object's bounding-box half diagonal and, in Curved mode, is not
    // the surface being drawn at all - so a press on empty canvas that happened to
    // land on that oversized quad started a plane drag and nudged the cut while
    // the user was only navigating. Only let the press through when the ray really
    // is on the RENDERED surface. The other grabbers (Z, X, Y, the rotation and
    // move handles, the connectors) have picking meshes that match what they draw,
    // so they are deliberately untouched.
    // Only the LEFT press is gated. The right press on the cut plane is the
    // part-selection toggle, which is aimed at the MODEL and does not consult
    // m_hover_id at all - swallowing it here would break "right-click a part to
    // assign it to the other side".
    if (m_hover_id == CutPlane && !m_connectors_editing &&
        mouse_event.LeftDown() && !mouse_on_cut_surface(mouse_pos))
        return false;

    if (m_hover_id >= CutPlane && mouse_event.LeftDown() && !m_connectors_editing) {
        // before processing of a use_grabbers(), detect start move position as a projection of mouse position to the cut plane
        Vec3d pos;
        Vec3d pos_world;
        if (unproject_on_cut_plane(mouse_pos, pos, pos_world, false))
            m_cut_plane_start_move_pos = pos_world;
    }

    if (use_grabbers(mouse_event)) {
        if (m_hover_id >= m_connectors_group_id) {
            if (mouse_event.LeftDown() && !mouse_event.CmdDown() && !mouse_event.AltDown())
                unselect_all_connectors();
            if (mouse_event.LeftUp() && !mouse_event.ShiftDown())
                gizmo_event(SLAGizmoEventType::LeftUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), mouse_event.CmdDown());
        }
        else if (m_hover_id == CutPlane) {
            if (mouse_event.LeftDown()) {
                m_was_cut_plane_dragged = m_was_contour_selected = false;

                // disable / enable current contour
                Vec3d pos;
                Vec3d pos_world;
                m_was_contour_selected = unproject_on_cut_plane(mouse_pos.cast<double>(), pos, pos_world);
                if (m_was_contour_selected) {
                    // Following would inform the clipper about the mouse click, so it can
                    // toggle the respective contour as disabled.
                    //m_c->object_clipper()->pass_mouse_click(pos_world);
                    //process_contours();
                    return true;
                }

            }
            else if (mouse_event.LeftUp() && !m_was_cut_plane_dragged && !m_was_contour_selected)
                flip_cut_plane();
        }

        if (m_hover_id >= CutPlane && mouse_event.Dragging() && !m_connectors_editing) {
            // if we continue to dragging a cut plane, than update a start move position as a projection of mouse position to the cut plane after processing of a use_grabbers()
            Vec3d pos;
            Vec3d pos_world;
            if (unproject_on_cut_plane(mouse_pos, pos, pos_world, false))
                m_cut_plane_start_move_pos = pos_world;
        }

        toggle_model_objects_visibility();
        return true;
    }

    static bool pending_right_up = false;
    if (mouse_event.LeftDown()) {
        bool grabber_contains_mouse = (get_hover_id() != -1);
        const bool shift_down = mouse_event.ShiftDown();
        if ((!shift_down || grabber_contains_mouse) &&
            gizmo_event(SLAGizmoEventType::LeftDown, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), false))
            return true;
    }
    else if (mouse_event.Dragging()) {
        bool control_down = mouse_event.CmdDown();
        if (m_parent.get_move_volume_id() != -1) {
            // don't allow dragging objects with the Sla gizmo on
            return true;
        }
        if (!control_down &&
            gizmo_event(SLAGizmoEventType::Dragging, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), false)) {
            // the gizmo got the event and took some action, no need to do
            // anything more here
            m_parent.set_as_dirty();
            return true;
        }
        if (control_down && (mouse_event.LeftIsDown() || mouse_event.RightIsDown())) {
            // CTRL has been pressed while already dragging -> stop current action
            if (mouse_event.LeftIsDown())
                gizmo_event(SLAGizmoEventType::LeftUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), true);
            else if (mouse_event.RightIsDown())
                pending_right_up = false;
        }
    }
    else if (mouse_event.LeftUp() && !m_parent.is_mouse_dragging()) {
        // in case SLA/FDM gizmo is selected, we just pass the LeftUp event
        // and stop processing - neither object moving or selecting is
        // suppressed in that case
        gizmo_event(SLAGizmoEventType::LeftUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), mouse_event.CmdDown());
        return true;
    }
    else if (mouse_event.RightDown()) {
        if (! m_connectors_editing && mouse_event.GetModifiers() == wxMOD_NONE &&
            CutMode(m_mode) == CutMode::cutPlanar && !is_draw_surface()) {
            // 2026-09-13, ITEM 3: NOT IN DRAW MODE. process_contours() builds the part
            // selection by performing a FLAT-PLANE cut (PartSelection's constructor calls
            // Cut::perform_with_plane), which on a drawn cut is not the cut at all - the
            // parts it produces are the two halves of a plane the user never asked for.
            //
            // Worse for the colouring: once m_part_selection is valid the object's real
            // volumes are HIDDEN (toggle_model_objects_visibility) and the part loop in
            // PartSelection::render draws the halves itself with "gouraud_light" and a
            // flat per-part colour. That shader declares no colour-clip uniforms at all,
            // so every uniform GLVolumeCollection::render sets - use_color_clip_plane,
            // the flat plane, the curved sheet AND the drawn field - is silently dropped
            // (GLShaderProgram::set_uniform is a no-op for an unknown name). Nothing the
            // classification does could reach those pixels.
            // Check the internal part raycasters.
            if (! m_part_selection.valid())
                process_contours();
            m_part_selection.toggle_selection(mouse_pos);
            check_and_update_connectors_state(); // after a contour is deactivated, its connectors are inside the object
            return true;
        }

        if (m_parent.get_selection().get_object_idx() != -1 &&
            gizmo_event(SLAGizmoEventType::RightDown, mouse_pos, false, false, false)) {
            // we need to set the following right up as processed to avoid showing
            // the context menu if the user release the mouse over the object
            pending_right_up = true;
            // event was taken care of by the SlaSupports gizmo
            return true;
        }
    }
    else if (pending_right_up && mouse_event.RightUp()) {
        pending_right_up = false;
        return true;
    }
    return false;
}

void GLGizmoCut3D::shift_cut(double delta)
{
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Move cut plane"), UndoRedo::SnapshotType::GizmoAction);
    set_center(m_plane_center + m_cut_normal * delta, true);
    m_ar_plane_center = m_plane_center;
}

void GLGizmoCut3D::rotate_vec3d_around_plane_center(Vec3d&vec)
{
    vec = Transformation(translation_transform(m_plane_center) * m_rotation_m * translation_transform(-m_plane_center)).get_matrix() * vec;
}

void GLGizmoCut3D::put_connectors_on_cut_plane(const Vec3d& cp_normal, double cp_offset)
{
    // 2026-09-12, OWNER FEEDBACK 3 (the connectors half of it). This snaps every
    // connector onto the FLAT PLANE, and it runs from update_clipper() - i.e. on every
    // plane nudge, every rotation and every gizmo open. On a DRAWN cut that is exactly
    // wrong: a connector there was placed by a raycast against the drawn surface, and
    // a drawn surface is not the plane, so the snap would walk it straight off the cut
    // and it would end up half in the air.
    //
    // A drawn connector needs no snapping at all - unproject_on_draw_surface() puts it
    // ON the cut surface and there is no "same parameters, new surface" relationship to
    // restore (phase 2, deviation #4). So this is a no-op in Draw mode, which is also
    // what keeps connector_rotation_m()'s frame - derived from the surface at the
    // connector's own (s, w) - describing the place the connector actually is.
    if (is_draw_surface())
        return;
    // Null and range guarded for the same reason check_and_update_connectors_state()
    // is: a recipe can be applied while the common data pool still describes a
    // selection that is gone, and "no object" has to mean "nothing to move".
    ModelObject* mo = m_c && m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;
    const int active_inst = m_c->selection_info()->get_active_instance();
    if (active_inst < 0 || size_t(active_inst) >= mo->instances.size())
        return;
    if (CutConnectors& connectors = mo->cut_connectors; !connectors.empty()) {
        const float sla_shift        = m_c->selection_info()->get_sla_shift();
        const Vec3d& instance_offset = mo->instances[active_inst]->get_offset();

        for (auto& connector : connectors) {
            // convert connetor pos to the world coordinates
            Vec3d pos = connector.pos + instance_offset;
            pos[Z] += sla_shift;
            // scalar distance from point to plane along the normal
            double distance = -cp_normal.dot(pos) + cp_offset;
            // move connector
            connector.pos += distance * cp_normal;
        }
    }
}

// returns true if the camera (forward) is pointing in the negative direction of the cut normal
bool GLGizmoCut3D::is_looking_forward() const
{
    const Camera& camera = wxGetApp().plater()->get_camera();
    const double dot = camera.get_dir_forward().dot(m_cut_normal);
    return dot < 0.05;
}

void GLGizmoCut3D::update_clipper()
{
    // update cut_normal
    Vec3d normal = m_rotation_m * Vec3d::UnitZ();
    normal.normalize();
    m_cut_normal = normal;

    // calculate normal and offset for clipping plane
    Vec3d beg = m_bb_center;
    beg[Z] -= m_radius;
    rotate_vec3d_around_plane_center(beg);

    m_clp_normal  = normal;
    double offset = normal.dot(m_plane_center);
    double dist   = normal.dot(beg);

    m_parent.set_color_clip_plane(normal, offset);

    if (!is_looking_forward()) {
        // recalculate normal and offset for clipping plane, if camera is looking downward to cut plane
        normal = m_rotation_m * (-1. * Vec3d::UnitZ());
        normal.normalize();

        beg = m_bb_center;
        beg[Z] += m_radius;
        rotate_vec3d_around_plane_center(beg);

        m_clp_normal = normal;
        offset       = normal.dot(m_plane_center);
        dist         = normal.dot(beg);
    }

    m_c->object_clipper()->set_range_and_pos(normal, offset, dist);

    // Curved surface: hand the sheet to the volume shader so the coloured
    // upper/lower halves follow it instead of the flat plane above. A flat (or
    // untouched) sheet clears it, and the plain plane split stands.
    apply_curved_color_clip();
    // DRAW surface, 2026-09-12 (owner feedback item 3): the same job against the drawn
    // cutter's voxel field. Here rather than anywhere the field is marked dirty, because
    // this is reached from the top of on_render() and so has the GL context current for
    // the texture upload. A clean field makes it a no-op, and an open chain (no cut
    // surface yet) clears the field so the plain plane split stands - which is honest,
    // since there is nothing else to show yet.
    apply_draw_color_clip();
    // Per-side visibility rides the same update: the colour clip is shared
    // state on the canvas, so it has to be re-armed whenever the clip is.
    apply_side_visibility();

    put_connectors_on_cut_plane(normal, offset);

    if (m_raycasters.empty())
        on_register_raycasters_for_picking();
    else
        update_raycasters_for_picking_transform();
}

void GLGizmoCut3D::set_center(const Vec3d& center, bool update_tbb /*=false*/)
{
    set_center_pos(center, update_tbb);
    check_and_update_connectors_state();
    update_clipper();
}

// 2026-09-20, OWNER REPORT (connectors on a drawn cut): on a DRAWN cut the plane
// is not the cut, so connector editing must not hand the clipper the plane either.
// set_behavior(hide_clipped=true) makes the canvas clip the model render at the
// flat plane - which hollowed the part ("no top") and drew the flat section cap
// as a ghost disc while connectors were being placed on a cut that does not go
// there. The draw field colours the halves and the cutter shell shows the surface;
// the plane clip is wanted only for the flat (and curved-sheet) cut.
static bool clip_by_plane_during_connector_editing(bool connectors_editing, bool draw_surface, bool draw_stroke_valid)
{
    return connectors_editing && !(draw_surface && draw_stroke_valid);
}

void GLGizmoCut3D::switch_to_mode(size_t new_mode)
{
    m_mode = new_mode;
    update_raycasters_for_picking();

    apply_color_clip_plane_colors();
    if (auto oc = m_c->object_clipper()) {
        m_contour_width = CutMode(m_mode) == CutMode::cutTongueAndGroove ? 0.f : 0.4f;
        const bool clip = clip_by_plane_during_connector_editing(m_connectors_editing, is_draw_surface(), m_draw_stroke.valid());
        oc->set_behavior(clip, clip, double(m_contour_width));
    }

    update_plane_model();
    reset_cut_by_contours();
}

bool GLGizmoCut3D::render_cut_mode_combo()
{
    ImGui::AlignTextToFramePadding();
    ImGuiWrapper::push_combo_style(m_parent.get_scale());
    int selection_idx = int(m_mode);
    const bool is_changed = m_imgui->combo(_u8L("Mode"), m_modes, selection_idx, 0, m_label_width, m_control_width);
    ImGuiWrapper::pop_combo_style();

    if (is_changed) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Change cut mode"), UndoRedo::SnapshotType::GizmoAction);
        switch_to_mode(size_t(selection_idx));
        check_and_update_connectors_state();
    }

    return is_changed;
}

bool GLGizmoCut3D::render_double_input(const std::string& label, double& value_in)
{
    ImGui::AlignTextToFramePadding();
    m_imgui->text(label);
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(m_control_width);

    double value = value_in;
    if (m_imperial_units)
        value *= GizmoObjectManipulation::mm_to_in;
    double old_val = value;
    ImGui::InputDouble(("##" + label).c_str(), &value, 0.0f, 0.0f, "%.2f", ImGuiInputTextFlags_CharsDecimal);

    ImGui::SameLine();
    m_imgui->text(m_imperial_units ? _L("in") : _L("mm"));

    value_in = value * (m_imperial_units ? GizmoObjectManipulation::in_to_mm : 1.0);
    return !is_approx(old_val, value);
}

bool GLGizmoCut3D::render_slider_double_input(const std::string& label, float& value_in, float& tolerance_in, float min_val/* = -0.1f*/, float max_tolerance/* = -0.1f*/)
{
    // -------- [ ] -------- [ ]
    // slider_with + item_in_gap + first_input_width + item_out_gap + slider_with + item_in_gap + second_input_width
    double slider_with        = 0.24 * m_editing_window_width; // m_control_width * 0.35;
    double item_in_gap        = 0.01 * m_editing_window_width;
    double item_out_gap       = 0.01 * m_editing_window_width;
    double first_input_width  = 0.29 * m_editing_window_width;
    double second_input_width = 0.29 * m_editing_window_width;

    constexpr float UndefMinVal = -0.1f;
    const float f_mm_to_in = static_cast<float>(GizmoObjectManipulation::mm_to_in);

    ImGui::AlignTextToFramePadding();
    m_imgui->text(label);
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(slider_with);

    double left_width = m_label_width + slider_with + item_in_gap;

    bool m_imperial_units = false;

    float value = value_in;
    if (m_imperial_units)
        value *= f_mm_to_in;
    float old_val = value;

    const BoundingBoxf3 bbox = m_bounding_box;
    const float mean_size = float((bbox.size().x() + bbox.size().y() + bbox.size().z()) / 9.0) * (m_imperial_units ? f_mm_to_in : 1.f);
    const float min_v = min_val > 0.f ? /*std::min(max_val, mean_size)*/min_val : 1.f;

    float min_size = value_in < 0.f ? UndefMinVal : min_v;
    if (m_imperial_units) {
        min_size *= f_mm_to_in;
    }
    std::string format = value_in < 0.f ? " " : m_imperial_units ? "%.4f  " + _u8L("in") : "%.2f  " + _u8L("mm");

    m_imgui->bbl_slider_float_style(("##" + label).c_str(), &value, min_size, mean_size, format.c_str());

    ImGui::SameLine(left_width);
    ImGui::PushItemWidth(first_input_width);
    ImGui::BBLDragFloat(("##input_" + label).c_str(), &value, 0.05f, min_size, mean_size, format.c_str());

    value_in = value * float(m_imperial_units ? GizmoObjectManipulation::in_to_mm : 1.0);

    left_width += (first_input_width + item_out_gap);
    ImGui::SameLine(left_width);
    ImGui::PushItemWidth(slider_with);

    float tolerance = tolerance_in;
    if (m_imperial_units)
        tolerance *= f_mm_to_in;
    float old_tolerance = tolerance;
    // std::string format_t      = tolerance_in < 0.f ? " " : "%.f %%";
    float min_tolerance = tolerance_in < 0.f ? UndefMinVal : 0.f;
    const float max_tolerance_v = max_tolerance > 0.f ? std::min(max_tolerance, 0.5f * mean_size) : 0.5f * mean_size;

    m_imgui->bbl_slider_float_style("##tolerance_" + label, &tolerance, min_tolerance, max_tolerance_v, format.c_str(), 1.f, true,
                                    _L("Tolerance"));

    left_width += (slider_with + item_in_gap);
    ImGui::SameLine(left_width);
    ImGui::PushItemWidth(second_input_width);
    ImGui::BBLDragFloat(("##tolerance_input_" + label).c_str(), &tolerance, 0.05f, min_tolerance, max_tolerance_v, format.c_str());

    tolerance_in = tolerance * float(m_imperial_units ? GizmoObjectManipulation::in_to_mm : 1.0);

    return !is_approx(old_val, value) || !is_approx(old_tolerance, tolerance);
}

void GLGizmoCut3D::render_move_center_input(int axis)
{
    m_imgui->text(m_axis_names[axis]+":");
    ImGui::SameLine();
    ImGui::PushItemWidth(0.3f*m_control_width);

    Vec3d move = m_plane_center;
    double in_val, value = in_val = move[axis];
    if (m_imperial_units)
        value *= GizmoObjectManipulation::mm_to_in;
    ImGui::InputDouble(("##move_" + m_axis_names[axis]).c_str(), &value, 0.0, 0.0, "%.2f", ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();

    double val = value * (m_imperial_units ? GizmoObjectManipulation::in_to_mm : 1.0);

    if (in_val != val) {
        move[axis] = val;
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Move cut plane"), UndoRedo::SnapshotType::GizmoAction);
        set_center(move, true);
        m_ar_plane_center = m_plane_center;

        reset_cut_by_contours();
    }
}

bool GLGizmoCut3D::render_connect_type_radio_button(CutConnectorType type)
{
    ImGui::SameLine(type == CutConnectorType::Plug ? m_label_width : 0);
    ImGui::PushItemWidth(m_control_width);
    if (ImGui::RadioButton(m_connector_types[size_t(type)].c_str(), m_connector_type == type)) {
        m_connector_type = type;
//        update_connector_shape();
        return true;
    }
    return false;
}

void GLGizmoCut3D::render_connect_mode_radio_button(CutConnectorMode mode)
{
    ImGui::SameLine(mode == CutConnectorMode::Auto ? m_label_width : 2 * m_label_width);
    ImGui::PushItemWidth(m_control_width);
    if (ImGui::RadioButton(m_connector_modes[int(mode)].c_str(), m_connector_mode == mode))
        m_connector_mode = mode;
}

bool GLGizmoCut3D::render_reset_button(const std::string& label_id, const std::string& tooltip) const
{
    const ImGuiStyle &style = ImGui::GetStyle();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {1, style.ItemSpacing.y});

    ImGui::PushStyleColor(ImGuiCol_Button, {0.25f, 0.25f, 0.25f, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.4f, 0.4f, 0.4f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.4f, 0.4f, 0.4f, 1.0f});

    const bool revert = m_imgui->button(wxString(ImGui::RevertBtn) + "##" + wxString::FromUTF8(label_id));

    ImGui::PopStyleColor(3);

    if (ImGui::IsItemHovered())
        m_imgui->tooltip(tooltip.c_str(), ImGui::GetFontSize() * 20.0f);

    ImGui::PopStyleVar();

    return revert;
}

static double get_grabber_mean_size(const BoundingBoxf3& bb)
{
#if ENABLE_FIXED_GRABBER
    // Orca: make grabber larger
    return 32. * GLGizmoBase::INV_ZOOM;
#else
    return (bb.size().x() + bb.size().y() + bb.size().z()) / 30.;
#endif
}

indexed_triangle_set GLGizmoCut3D::its_make_groove_plane()
{
    // values for calculation

    const float  side_width     = is_approx(m_groove.flaps_angle, 0.f) ? m_groove.depth : (m_groove.depth / sin(m_groove.flaps_angle));
    const float  flaps_width    = 2.f * side_width * cos(m_groove.flaps_angle);

    const float groove_half_width_upper = 0.5f * (m_groove.width);
    const float groove_half_width_lower = 0.5f * (m_groove.width + flaps_width);

    const float cut_plane_radius = 1.5f * float(m_radius);
    const float cut_plane_length = 1.5f * cut_plane_radius;

    const float groove_half_depth = 0.5f * m_groove.depth;

    const float x       = 0.5f * cut_plane_radius;
    const float y       = 0.5f * cut_plane_length;
    float       z_upper = groove_half_depth;
    float       z_lower = -groove_half_depth;

    const float proj = y * tan(m_groove.angle);

    float ext_upper_x = groove_half_width_upper + proj; // upper_x extension
    float ext_lower_x = groove_half_width_lower + proj; // lower_x extension

    float nar_upper_x = groove_half_width_upper - proj; // upper_x narrowing
    float nar_lower_x = groove_half_width_lower - proj; // lower_x narrowing

    const float cut_plane_thiknes = 0.02f;// 0.02f * (float)get_grabber_mean_size(m_bounding_box);   // cut_plane_thiknes

    // Vertices of the groove used to detection if groove is valid
    // They are written as:
    // {left_ext_lower, left_nar_lower, left_ext_upper, left_nar_upper,
    //  right_ext_lower, right_nar_lower, right_ext_upper, right_nar_upper }
    {
        m_groove_vertices.clear();
        m_groove_vertices.reserve(8);

        m_groove_vertices.emplace_back(Vec3f(-ext_lower_x, -y, z_lower).cast<double>());
        m_groove_vertices.emplace_back(Vec3f(-nar_lower_x,  y, z_lower).cast<double>());
        m_groove_vertices.emplace_back(Vec3f(-ext_upper_x, -y, z_upper).cast<double>());
        m_groove_vertices.emplace_back(Vec3f(-nar_upper_x,  y, z_upper).cast<double>());
        m_groove_vertices.emplace_back(Vec3f( ext_lower_x, -y, z_lower).cast<double>());
        m_groove_vertices.emplace_back(Vec3f( nar_lower_x,  y, z_lower).cast<double>());
        m_groove_vertices.emplace_back(Vec3f( ext_upper_x, -y, z_upper).cast<double>());
        m_groove_vertices.emplace_back(Vec3f( nar_upper_x,  y, z_upper).cast<double>());
    }

    // Different cases of groove plane:

    // groove is open

    if (groove_half_width_upper > proj && groove_half_width_lower > proj) {
        indexed_triangle_set mesh;

        auto get_vertices = [x, y](float z_upper, float z_lower, float nar_upper_x, float nar_lower_x, float ext_upper_x, float ext_lower_x) {
            return std::vector<stl_vertex>({
                // upper left part vertices
                {-x, -y, z_upper}, {-x, y, z_upper}, {-nar_upper_x, y, z_upper}, {-ext_upper_x, -y, z_upper},
                // lower part vertices
                {-ext_lower_x, -y, z_lower}, {-nar_lower_x, y, z_lower}, {nar_lower_x, y, z_lower}, {ext_lower_x, -y, z_lower},
                // upper right part vertices
                {ext_upper_x, -y, z_upper}, {nar_upper_x, y, z_upper}, {x, y, z_upper}, {x, -y, z_upper}
                });
        };

        mesh.vertices = get_vertices(z_upper, z_lower, nar_upper_x, nar_lower_x, ext_upper_x, ext_lower_x);
        mesh.vertices.reserve(2 * mesh.vertices.size());

        z_upper -= cut_plane_thiknes;
        z_lower -= cut_plane_thiknes;

        const float under_x_shift = cut_plane_thiknes / tan(0.5f * m_groove.flaps_angle);

        nar_upper_x += under_x_shift;
        nar_lower_x += under_x_shift;
        ext_upper_x += under_x_shift;
        ext_lower_x += under_x_shift;

        std::vector<stl_vertex> vertices = get_vertices(z_upper, z_lower, nar_upper_x, nar_lower_x, ext_upper_x, ext_lower_x);
        mesh.vertices.insert(mesh.vertices.end(), vertices.begin(), vertices.end());

        mesh.indices = {
            // above view
            {5,4,7}, {5,7,6},       // lower part
            {3,4,5}, {3,5,2},       // left side
            {9,6,8}, {8,6,7},       // right side
            {1,0,2}, {2,0,3},       // upper left part
            {9,8,10}, {10,8,11},    // upper right part
            // under view
            {20,21,22}, {20,22,23}, // upper right part
            {12,13,14}, {12,14,15}, // upper left part
            {18,21,20}, {18,20,19}, // right side
            {16,15,14}, {16,14,17}, // left side
            {16,17,18}, {16,18,19}, // lower part  
            // left edge
            {1,13,0}, {0,13,12},
            // front edge
            {0,12,3}, {3,12,15}, {3,15,4}, {4,15,16}, {4,16,7}, {7,16,19}, {7,19,20}, {7,20,8}, {8,20,11}, {11,20,23},
            // right edge
            {11,23,10}, {10,23,22},
            // back edge
            {1,13,2}, {2,13,14}, {2,14,17}, {2,17,5}, {5,17,6}, {6,17,18}, {6,18,9}, {9,18,21}, {9,21,10}, {10,21,22}
        };
        return mesh;
    }

    float cross_pt_upper_y = groove_half_width_upper / tan(m_groove.angle);

    // groove is closed

    if (groove_half_width_upper < proj && groove_half_width_lower < proj) {
        float cross_pt_lower_y = groove_half_width_lower / tan(m_groove.angle);

        indexed_triangle_set mesh;

        auto get_vertices = [x, y](float z_upper, float z_lower, float cross_pt_upper_y, float cross_pt_lower_y, float ext_upper_x, float ext_lower_x) {
            return std::vector<stl_vertex>({
                // upper part vertices
                {-x, -y, z_upper}, {-x, y, z_upper}, {x, y, z_upper}, {x, -y, z_upper},
                {ext_upper_x, -y, z_upper}, {0.f, cross_pt_upper_y, z_upper}, {-ext_upper_x, -y, z_upper},
                // lower part vertices
                {-ext_lower_x, -y, z_lower}, {0.f, cross_pt_lower_y, z_lower}, {ext_lower_x, -y, z_lower}
                });
        };

        mesh.vertices = get_vertices(z_upper, z_lower, cross_pt_upper_y, cross_pt_lower_y, ext_upper_x, ext_lower_x);
        mesh.vertices.reserve(2 * mesh.vertices.size());

        z_upper -= cut_plane_thiknes;
        z_lower -= cut_plane_thiknes;

        const float under_x_shift = cut_plane_thiknes / tan(0.5f * m_groove.flaps_angle);

        cross_pt_upper_y += cut_plane_thiknes;
        cross_pt_lower_y += cut_plane_thiknes;
        ext_upper_x += under_x_shift;
        ext_lower_x += under_x_shift;

        std::vector<stl_vertex> vertices = get_vertices(z_upper, z_lower, cross_pt_upper_y, cross_pt_lower_y, ext_upper_x, ext_lower_x);
        mesh.vertices.insert(mesh.vertices.end(), vertices.begin(), vertices.end());

        mesh.indices = {
            // above view
            {8,7,9},                    // lower part
            {5,8,6}, {6,8,7},       // left side
            {4,9,8}, {4,8,5},       // right side
            {1,0,6}, {1,6,5},{1,5,2}, {2,5,4}, {2,4,3},   // upper part
            // under view
            {10,11,16}, {16,11,15}, {15,11,12}, {15,12,14}, {14,12,13},   // upper part
            {18,15,14}, {14,18,19}, // right side
            {17,16,15}, {17,15,18}, // left side
            {17,18,19},                 // lower part  
            // left edge
            {1,11,0}, {0,11,10},
            // front edge
            {0,10,6}, {6,10,16}, {6,17,16}, {6,7,17}, {7,17,19}, {7,19,9}, {4,14,19}, {4,19,9}, {4,14,13}, {4,13,3},
            // right edge
            {3,13,12}, {3,12,2},
            // back edge
            {2,12,11}, {2,11,1}
        };

        return mesh;
    }

    // groove is closed from the roof

    indexed_triangle_set mesh;
    mesh.vertices = {
        // upper part vertices
        {-x, -y, z_upper}, {-x, y, z_upper}, {x, y, z_upper}, {x, -y, z_upper},
        {ext_upper_x, -y, z_upper}, {0.f, cross_pt_upper_y, z_upper}, {-ext_upper_x, -y, z_upper},
        // lower part vertices
        {-ext_lower_x, -y, z_lower}, {-nar_lower_x, y, z_lower}, {nar_lower_x, y, z_lower}, {ext_lower_x, -y, z_lower}
    };

    mesh.vertices.reserve(2 * mesh.vertices.size() + 1);

    z_upper -= cut_plane_thiknes;
    z_lower -= cut_plane_thiknes;

    const float under_x_shift = cut_plane_thiknes / tan(0.5f * m_groove.flaps_angle);

    nar_lower_x += under_x_shift;
    ext_upper_x += under_x_shift;
    ext_lower_x += under_x_shift;

    std::vector<stl_vertex> vertices = {
        // upper part vertices
        {-x, -y, z_upper}, {-x, y, z_upper}, {x, y, z_upper}, {x, -y, z_upper},
        {ext_upper_x, -y, z_upper}, {under_x_shift, cross_pt_upper_y, z_upper}, {-under_x_shift, cross_pt_upper_y, z_upper}, {-ext_upper_x, -y, z_upper},
        // lower part vertices
        {-ext_lower_x, -y, z_lower}, {-nar_lower_x, y, z_lower}, {nar_lower_x, y, z_lower}, {ext_lower_x, -y, z_lower}
    };
    mesh.vertices.insert(mesh.vertices.end(), vertices.begin(), vertices.end());

    mesh.indices = {
        // above view
        {8,7,10}, {8,10,9},     // lower part
        {5,8,7}, {5,7,6},       // left side
        {4,10,9}, {4,9,5},      // right side
        {1,0,6}, {1,6,5},{1,5,2}, {2,5,4}, {2,4,3},   // upper part
        // under view
        {11,12,18}, {18,12,17}, {17,12,16}, {16,12,13}, {16,13,15}, {15,13,14},   // upper part
        {21,16,15}, {21,15,22}, // right side
        {19,18,17}, {19,17,20}, // left side
        {19,20,21}, {19,21,22}, // lower part  
        // left edge
        {1,12,11}, {1,11,0},
        // front edge
        {0,11,18}, {0,18,6}, {7,19,18}, {7,18,6}, {7,19,22}, {7,22,10}, {10,22,15}, {10,15,4}, {4,15,14}, {4,14,3},
        // right edge
        {3,14,13}, {3,14,2},
        // back edge
        {2,13,12}, {2,12,1}, {5,16,21}, {5,21,9}, {9,21,20}, {9,20,8}, {5,17,20}, {5,20,8}
    };

    return mesh;
}

void GLGizmoCut3D::render_cut_plane()
{
    if (cut_line_processing())
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    shader->start_using();

    const Camera& camera = wxGetApp().plater()->get_camera();

    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    ColorRGBA cp_clr = can_perform_cut() && has_valid_groove() ? CUT_PLANE_DEF_COLOR : CUT_PLANE_ERR_COLOR;
    if (m_mode == size_t(CutMode::cutTongueAndGroove))
        cp_clr.a(cp_clr.a() - 0.1f);
    m_plane.model.set_color(cp_clr);

    const Transform3d view_model_matrix = camera.get_view_matrix() * translation_transform(m_plane_center) * m_rotation_m;
    // PHASE 3: with a cut thickness the plane is drawn TWICE, at the two faces the
    // kerf produces, so the translucent band between them is the material that
    // will be removed. At thickness 0 the two offsets are both zero and this is
    // the single draw it always was.
    double face_lo = 0.0, face_hi = 0.0;
    cut_thickness_faces(face_lo, face_hi);
    if (face_hi > face_lo) {
        for (double off : { face_lo, face_hi }) {
            shader->set_uniform("view_model_matrix", view_model_matrix * translation_transform(off * Vec3d::UnitZ()));
            m_plane.model.render();
        }
    }
    else {
        shader->set_uniform("view_model_matrix", view_model_matrix);
        m_plane.model.render();
    }

    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));

    shader->stop_using();
}

static double get_half_size(double size)
{
    return std::max(size * 0.35, 0.05);
}

static double get_dragging_half_size(double size)
{
    return get_half_size(size) * 1.25;
}

void GLGizmoCut3D::render_model(GLModel& model, const ColorRGBA& color, Transform3d view_model_matrix)
{
    GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (shader) {
        shader->start_using();

        shader->set_uniform("view_model_matrix", view_model_matrix);
        shader->set_uniform("emission_factor", 0.2f);
        shader->set_uniform("projection_matrix", wxGetApp().plater()->get_camera().get_projection_matrix());

        model.set_color(color);
        model.render();

        shader->stop_using();
    }
}

void GLGizmoCut3D::render_line(GLModel& line_model, const ColorRGBA& color, Transform3d view_model_matrix, float width)
{
    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader) {
        shader->start_using();

        shader->set_uniform("view_model_matrix", view_model_matrix);
        shader->set_uniform("projection_matrix", wxGetApp().plater()->get_camera().get_projection_matrix());
        shader->set_uniform("width", width);

        line_model.set_color(color);
        line_model.render();

        shader->stop_using();
    }
}

void GLGizmoCut3D::render_rotation_snapping(GrabberID axis, const ColorRGBA& color)
{
    GLShaderProgram* line_shader = wxGetApp().get_shader("flat");
    if (!line_shader)
        return;

    const Camera& camera = wxGetApp().plater()->get_camera();
    Transform3d view_model_matrix = camera.get_view_matrix() * translation_transform(m_plane_center) * m_start_dragging_m;

    if (axis == X)
        view_model_matrix = view_model_matrix * rotation_transform(0.5 * PI * Vec3d::UnitY()) * rotation_transform(-PI * Vec3d::UnitZ());
    else if (axis == Y)
        view_model_matrix = view_model_matrix * rotation_transform(-0.5 * PI * Vec3d::UnitZ()) * rotation_transform(-0.5 * PI * Vec3d::UnitY());
    else
        view_model_matrix = view_model_matrix * rotation_transform(-0.5 * PI * Vec3d::UnitZ());

    line_shader->start_using();
    line_shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    line_shader->set_uniform("view_model_matrix", view_model_matrix);
    line_shader->set_uniform("width", 0.25f);

    m_circle.render();
    m_scale.render();
    m_snap_radii.render();
    m_reference_radius.render();
    if (m_dragging) {
        line_shader->set_uniform("width", 1.5f);
        m_angle_arc.set_color(color);
        m_angle_arc.render();
    }

    line_shader->stop_using();
}

void GLGizmoCut3D::render_grabber_connection(const ColorRGBA& color, Transform3d view_matrix, double line_len_koef/* = 1.0*/)
{
    const Transform3d line_view_matrix = view_matrix * scale_transform(Vec3d(1.0, 1.0, line_len_koef * m_grabber_connection_len));

    render_line(m_grabber_connection, color, line_view_matrix, 0.2f);
};

// ---------------------------------------------------------------------------
// Curved cut (phase 1)
//
// Surface: Flat | Curved. Curved keeps the base cut plane exactly as it is -
// the same rotate/translate grabbers position and orient it - and defines a
// height field z = f(u,v) OVER it from a coarse control grid. The dense sheet
// the preview and the cut use is upsampled from that grid with Catmull-Rom, so
// a control point's displacement IS the surface height there.
//
// Zero displacement is the flat plane, and Cut::perform_with_curved_sheet
// dispatches straight back into perform_with_plane() in that case, so a curved
// cut with nothing dragged is the same code path as today's cut.
// ---------------------------------------------------------------------------

double GLGizmoCut3D::curved_sheet_half_size() const
{
    // The FALLBACK only. Phase 1 sized the sheet at the flat plane model's own
    // extent, which is derived from the object's bounding-box diagonal, so on a
    // slab or a rotated plane most control points sat well outside the part -
    // the owner's complaint. fit_curved_sheet_to_section() replaces this with a
    // fit to the cut's own cross-section; this value is what the sheet starts at
    // and what it falls back to when the plane misses the object entirely.
    return double(m_cut_plane_radius_koef) * m_radius;
}

// The instance mesh (every model-part volume, merged) expressed in the CUT
// PLANE's own frame - the frame the sheet lives in and cut_mesh() slices at
// z == 0. Both the cross-section fit and the handle snap work there, so they
// share one derivation.
bool GLGizmoCut3D::curved_instance_mesh_in_plane(indexed_triangle_set& out) const
{
    out.clear();

    const CommonGizmosDataObjects::SelectionInfo* sel = m_c->selection_info();
    const ModelObject* mo = sel ? sel->model_object() : nullptr;
    if (mo == nullptr)
        return false;

    const int inst_idx = sel->get_active_instance();
    if (inst_idx < 0 || inst_idx >= int(mo->instances.size()))
        return false;

    const Transform3d inst_matrix   = mo->instances[inst_idx]->get_transformation().get_matrix();
    const Transform3d world_to_plane = (translation_transform(m_plane_center) * m_rotation_m).inverse();

    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part() || mv->mesh().empty())
            continue;
        indexed_triangle_set part = mv->mesh().its;
        its_transform(part, world_to_plane * inst_matrix * mv->get_matrix());
        its_merge(out, part);
    }
    return !out.empty();
}

void GLGizmoCut3D::fit_curved_sheet_to_section(bool force)
{
    // The sheet's own fit. Draw has no sheet, so there is nothing to fit.
    if (m_surface_mode != CutSurfaceMode::Curved)
        return;

    // Only re-fit when the plane has actually moved or turned. A fit costs a
    // pass over the instance mesh; a redraw must not pay for one.
    const bool moved = !m_curved_fit_valid ||
                       !m_curved_fit_center.isApprox(m_plane_center) ||
                       !m_curved_fit_rotation.isApprox(m_rotation_m);
    if (!force && !moved)
        return;

    indexed_triangle_set mesh;
    if (!curved_instance_mesh_in_plane(mesh))
        return;

    double hs_u = 0.0, hs_v = 0.0;
    // PHASE 3: the WHOLE part projected onto the plane's axes, not the plane's
    // cross-section. Beyond the sheet's own domain the cut slab extrudes the
    // sheet's RIM height outwards, so anything of the part that hangs outside the
    // domain is cut by that extruded rim rather than by the surface the user
    // drew - and on a part that is wider above or below the plane than it is AT
    // the plane, a strongly bent sheet takes the rim clear off the part and one
    // side comes back empty. Covering the whole projection means the rim never
    // touches material. See curved_cut_fit_projection_extent().
    //
    // 15% of the extent, or 5 mm, whichever is larger: the relative term keeps a
    // big part's handles clear of the silhouette, the absolute one keeps a small
    // part from getting a sheet barely wider than itself.
    if (!curved_cut_fit_projection_extent(mesh, hs_u, hs_v, 0.15, 5.0)) {
        // An empty mesh. Keep the extent we have rather than collapsing the
        // sheet to nothing.
        return;
    }

    // SNAP THE EXTENT. A plane drag re-fits every time the plane moves, and a
    // continuously varying extent would re-sample the grid continuously. The
    // projection's extent does not depend on where the plane sits ALONG its
    // normal at all, so in the common gesture (sliding the cut position) the
    // snapped extent is simply constant and no re-sample happens. 0.5 mm is far
    // below anything visible on a handle.
    auto snap = [](double v) { return std::ceil(v * 2.0 - 1e-9) * 0.5; };
    hs_u = snap(hs_u);
    hs_v = snap(hs_v);

    // The domain now covers the whole part, which on a large model is much bigger
    // than the cross-section fit was, so a fixed 5 x 5 would spread the handles
    // too thin. Aim for ~10 mm spacing, clamped into 5..MaxResolution. The user's
    // own slider choice wins once they have made one.
    // Per axis, so a long thin part gets a grid shaped like the part rather than
    // the long axis' density stretched across the short one. The fit never picks
    // a ruled (2-count) axis on its own - min_res floors both at 5 - so a 10 x 2
    // stays something the user asks for.
    if (!m_curved_res_user_set) {
        int gx = 0, gy = 0;
        curved_cut_default_grid(hs_u, hs_v, gx, gy, 10.0, CurvedCutSheet::DefaultResolution);
        if (gx != m_curved_sheet.nx() || gy != m_curved_sheet.ny()) {
            m_curved_sheet.set_grid(gx, gy);
            m_curved_nx = m_curved_sheet.nx();
            m_curved_ny = m_curved_sheet.ny();
            m_curved_hover_ctl = m_curved_drag_ctl = -1;
        }
    }

    // RE-SAMPLE, so the surface the user drew stays where it is in the plane
    // while the rectangle around it changes - the same contract set_resolution()
    // honours for a grid change.
    m_curved_sheet.set_half_size(hs_u, hs_v, /*resample*/ true);

    m_curved_fit_center   = m_plane_center;
    m_curved_fit_rotation = m_rotation_m;
    m_curved_fit_valid    = true;
    m_curved_fit_pending  = false;

    // PHASE 4: the sheet's WORLD position just changed (the plane moved or
    // turned), so the connector pick mesh - which is the sheet in world space -
    // has to be rebuilt before the next click.
    m_curved_pick_dirty = true;

    // The spacing changed, so the default bend radius did too. Only follow it
    // while the user has not overridden it - m_curved_brush_radius <= 0 is the
    // "not set yet" marker the panel already uses.
    if (m_curved_brush_radius <= 0.f)
        m_curved_brush_radius = default_curved_bend_radius();

    // The sheet moved, so which side is empty may have changed.
    update_curved_empty_sides();

    invalidate_curved_sheet();
    m_curved_hover_ctl = m_curved_drag_ctl = -1;
}

// ---------------------------------------------------------------------------
// Phase 3: the empty-side warning.
//
// curved_cut_split() already REPORTS an empty half (it returns false and leaves
// that side's mesh empty), but only once the user has committed to the cut and
// paid for two booleans. The panel needs the same answer before the click, so it
// asks the cheap question instead: is there any vertex of the part on that side
// of the sheet? One pass over the vertices, refreshed with the fit and after
// every edit, against two booleans.
// ---------------------------------------------------------------------------

void GLGizmoCut3D::update_curved_empty_sides()
{
    m_curved_upper_empty = m_curved_lower_empty = false;
    if (!is_curved_surface())
        return;

    indexed_triangle_set mesh;
    if (!curved_instance_mesh_in_plane(mesh))
        return;

    curved_cut_empty_sides(mesh, m_curved_sheet, m_curved_upper_empty, m_curved_lower_empty,
                           double(m_cut_thickness), cut_thickness_offset());
}

void GLGizmoCut3D::cut_thickness_faces(double& lo, double& hi) const
{
    curved_cut_thickness_faces(double(m_cut_thickness), cut_thickness_offset(), lo, hi);
}

void GLGizmoCut3D::update_curved_sheet_model()
{
    m_curved_sheet_model.reset();

    // The dense sheet as a two-sided slab thin enough to read as a surface:
    // the same look as the flat plane's frustum-dowel disc/quad.
    const double     thickness = 0.02 * get_grabber_mean_size(m_bounding_box);
    const int        n         = CurvedCutSheet::DefaultSamples;
    indexed_triangle_set its   = m_curved_sheet.sample_sheet(n);

    // Duplicate the sheet thickness/2 below and stitch the rim, so the preview
    // has a body and is not a zero-thickness surface that z-fights with itself.
    const size_t verts = its.vertices.size();
    const size_t faces = its.indices.size();
    its.vertices.reserve(verts * 2);
    for (size_t i = 0; i < verts; ++ i) {
        Vec3f v = its.vertices[i];
        v.z() -= float(thickness);
        its.vertices.emplace_back(v);
    }
    its.indices.reserve(faces * 2 + size_t(n) * 8);
    for (size_t f = 0; f < faces; ++ f) {
        const Vec3i32 t = its.indices[f];
        its.indices.emplace_back(Vec3i32(int(verts) + t(0), int(verts) + t(2), int(verts) + t(1)));
    }
    auto top = [n](int i, int j) { return j * n + i; };
    auto bot = [n, verts](int i, int j) { return int(verts) + j * n + i; };
    for (int i = 0; i + 1 < n; ++ i) {
        its.indices.emplace_back(Vec3i32(top(i, 0), bot(i, 0), bot(i + 1, 0)));
        its.indices.emplace_back(Vec3i32(top(i, 0), bot(i + 1, 0), top(i + 1, 0)));
        its.indices.emplace_back(Vec3i32(top(i, n - 1), top(i + 1, n - 1), bot(i + 1, n - 1)));
        its.indices.emplace_back(Vec3i32(top(i, n - 1), bot(i + 1, n - 1), bot(i, n - 1)));
    }
    for (int j = 0; j + 1 < n; ++ j) {
        its.indices.emplace_back(Vec3i32(top(0, j), top(0, j + 1), bot(0, j + 1)));
        its.indices.emplace_back(Vec3i32(top(0, j), bot(0, j + 1), bot(0, j)));
        its.indices.emplace_back(Vec3i32(top(n - 1, j), bot(n - 1, j), bot(n - 1, j + 1)));
        its.indices.emplace_back(Vec3i32(top(n - 1, j), bot(n - 1, j + 1), top(n - 1, j + 1)));
    }

    m_curved_sheet_model.init_from(its);
    m_curved_sheet_dirty = false;
}

// 1.5 control spacings: enough for a handle to carry its neighbours, small enough that a single
// point can still be bent on its own by turning it down.
float GLGizmoCut3D::default_curved_bend_radius() const
{
    // Phase 2: the domain is a rectangle, so u and v have different spacings.
    // The radius is one number (grab() measures a circular distance), so take
    // the SMALLER spacing: 1.5 of the larger would already reach past three
    // neighbours along the short axis, which is not "carry your neighbours"
    // any more, it is "bend the whole sheet".
    // Each axis has its own count now, so each has its own spacing: a 10 x 2
    // sheet's v spacing is the whole domain, its u spacing a ninth of it.
    const int    nx      = std::max(2, m_curved_sheet.nx());
    const int    ny      = std::max(2, m_curved_sheet.ny());
    const double sp_u    = 2.0 * m_curved_sheet.half_size_u() / double(nx - 1);
    const double sp_v    = 2.0 * m_curved_sheet.half_size_v() / double(ny - 1);
    return float(std::max(1.0, 1.5 * std::min(sp_u, sp_v)));
}

void GLGizmoCut3D::render_curved_sheet()
{
    if (cut_line_processing())
        return;

    if (m_curved_sheet_dirty || !m_curved_sheet_model.is_initialized())
        update_curved_sheet_model();

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    shader->start_using();

    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    // The same translucent plane material the flat cut plane uses.
    const ColorRGBA cp_clr = can_perform_cut() ? CUT_PLANE_DEF_COLOR : CUT_PLANE_ERR_COLOR;
    m_curved_sheet_model.set_color(cp_clr);

    // The sheet rides on the base plane's own frame, so the existing rotate and
    // translate of the plane move the sheet with it.
    const Transform3d view_model_matrix = camera.get_view_matrix() * translation_transform(m_plane_center) * m_rotation_m;
    // PHASE 3: the same two-surface draw the flat plane does for a cut thickness.
    // The sheet is a height field over local Z, so offsetting the whole model
    // along local Z IS the offset surface - no re-sampling needed.
    double face_lo = 0.0, face_hi = 0.0;
    cut_thickness_faces(face_lo, face_hi);
    if (face_hi > face_lo) {
        for (double off : { face_lo, face_hi }) {
            shader->set_uniform("view_model_matrix", view_model_matrix * translation_transform(off * Vec3d::UnitZ()));
            m_curved_sheet_model.render();
        }
    }
    else {
        shader->set_uniform("view_model_matrix", view_model_matrix);
        m_curved_sheet_model.render();
    }

    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));

    shader->stop_using();
}

// ---------------------------------------------------------------------------
// Curved PREVIEW.
//
// The coloured upper/lower halves come from the volume shader, which until now
// split them by the FLAT cut plane (GLVolumeCollection::set_color_clip_plane +
// color_clip_plane in gouraud.vs). With a bent sheet that reads as "a flat cut
// with the curve as a sort of centre point" - the sheet is drawn curved but the
// colours ignore it.
//
// Two pieces fix that:
//  * the halves: the height field goes to the shader as a small float texture
//    plus the world -> plane frame, and gouraud.fs compares the fragment's own
//    local z against f(u,v). Per fragment, so it is exact whatever the mesh
//    tessellation is, and free during a drag - the texture is 64x64 floats.
//  * the cut face: MeshClipper slices at a single z, so its cap can only ever
//    be flat. The curved cap is the sheet restricted to the object's interior,
//    which is a point-in-mesh test per sample rather than a boolean: one ray
//    per sample along the plane normal, parity of the hits ahead of it. A few
//    ms for a 64x64 grid, against ~130 ms for the slab boolean on a 40 mm cube.
// ---------------------------------------------------------------------------

void GLGizmoCut3D::update_curved_sheet_texture()
{
    if (!m_curved_sheet_tex_dirty && m_curved_sheet_tex != 0)
        return;

    const int n = CurvedCutSheet::DefaultSamples;
    std::vector<float> h(size_t(n) * size_t(n));
    for (int j = 0; j < n; ++ j) {
        const double v = double(j) / double(n - 1);
        for (int i = 0; i < n; ++ i)
            h[size_t(j) * n + i] = float(m_curved_sheet.evaluate(double(i) / double(n - 1), v));
    }

    if (m_curved_sheet_tex == 0) {
        GLuint id = 0;
        glsafe(::glGenTextures(1, &id));
        m_curved_sheet_tex = (unsigned int) id;
    }

    glsafe(::glActiveTexture(GL_TEXTURE3));
    glsafe(::glBindTexture(GL_TEXTURE_2D, (GLuint) m_curved_sheet_tex));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    // CLAMP_TO_EDGE, not REPEAT: outside the sheet's square domain the split
    // has to continue along the border height, not wrap round to the far side.
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    // GL_R32F / GL_RED are core from 3.0, which is what the 140 shaders run on
    // (GLShadersManager picks 140/ at >= 3.1). The 110 fallback runs on a 2.1
    // context where neither name is guaranteed, so use GL_LUMINANCE there - it
    // reads back in .r all the same. GL_LUMINANCE is fixed point and clamps to
    // [0,1], so encode f there as (f/range + 1)/2 and undo it in the shader;
    // curved_sheet_encoded tells the shader which of the two it is looking at.
    m_curved_sheet_range = std::max(1e-6, 2.0 * std::max(1.0, m_curved_sheet.max_displacement()));
    if (wxGetApp().is_gl_version_greater_or_equal_to(3, 0)) {
        m_curved_sheet_encoded = false;
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, n, n, 0, GL_RED, GL_FLOAT, h.data()));
    }
    else {
        m_curved_sheet_encoded = true;
        for (float& f : h)
            f = float(0.5 * (double(f) / m_curved_sheet_range + 1.0));
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, n, n, 0, GL_LUMINANCE, GL_FLOAT, h.data()));
    }
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    glsafe(::glActiveTexture(GL_TEXTURE0));

    m_curved_sheet_tex_dirty = false;
}

void GLGizmoCut3D::release_curved_sheet_texture()
{
    if (m_curved_sheet_tex != 0) {
        GLuint id = (GLuint) m_curved_sheet_tex;
        glsafe(::glDeleteTextures(1, &id));
        m_curved_sheet_tex = 0;
    }
    m_curved_sheet_tex_dirty = true;
    m_curved_sheet_encoded   = false;
    m_curved_sheet_range     = 1.0;
    m_curved_cap_model.reset();
    m_curved_cap_dirty = true;
    m_curved_cap_key   = 0;
    m_curved_cap_stale = false;
    m_parent.set_curved_color_clip(0, Transform3d::Identity(), 1.0, 1.0);
}

void GLGizmoCut3D::apply_curved_color_clip()
{
    // Only a sheet that is actually bent takes over the split: "Curved but
    // untouched" must stay exactly the flat cut, preview included.
    if (!is_curved_surface() || m_curved_sheet.is_flat() || m_connectors_editing || m_hide_cut_plane) {
        m_parent.set_curved_color_clip(0, Transform3d::Identity(), 1.0, 1.0);
        return;
    }

    update_curved_sheet_texture();

    // The sheet lives in the cut plane's own frame - the frame the plane model
    // is drawn in, and the frame the cut itself works in (process_volume_curved_cut
    // inverts the same rotation and offset). Its inverse takes a world point
    // into that frame, which is what the fragment shader needs.
    const Transform3d plane_to_world = translation_transform(m_plane_center) * m_rotation_m;
    m_parent.set_curved_color_clip(m_curved_sheet_tex, plane_to_world.inverse(),
                                   m_curved_sheet.half_size_u(), m_curved_sheet.half_size_v(),
                                   m_curved_sheet_encoded ? m_curved_sheet_range : 0.);
}

// ---------------------------------------------------------------------------
// Phase 2 (2): side visibility.
//
// The two coloured halves the colour-clip shader draws can each be solid,
// ghosted or hidden. Ghost is a translucent draw with depth WRITES off, so the
// sheet and the far half show through it; Hidden is a discard in the shader,
// because a zero-alpha fragment would still write depth and go on occluding.
// Both default to Visible and are reset when the gizmo closes.
// ---------------------------------------------------------------------------

float GLGizmoCut3D::side_visibility_alpha(SideVisibility v)
{
    // One source of truth: the contract lives in libslic3r (curved_cut_side_alpha)
    // so a headless test can pin it - nobody can look at the rendered result, and
    // the two-pass draw in GLVolumeCollection::render keys off the same helpers.
    switch (v) {
    case SideVisibility::Ghost:  return curved_cut_side_alpha(CurvedCutSideVisibility::Ghost);
    case SideVisibility::Hidden: return curved_cut_side_alpha(CurvedCutSideVisibility::Hidden);
    default:                     return curved_cut_side_alpha(CurvedCutSideVisibility::Visible);
    }
}

void GLGizmoCut3D::apply_side_visibility()
{
    // Side 1 is the UPPER half, side 2 the lower, and that holds in both the
    // flat and the curved path:
    //  - flat: set_color_clip_plane() stores -normal, so color_clip_plane_dot is
    //    negative above the plane; apply_color_clip_plane_colors() feeds side 1
    //    with UPPER_PART_COLOR.
    //  - curved: side = h - local.z, negative when local.z is above the sheet.
    // (The camera direction flips m_clp_normal for the object clipper, not for
    // the colour clip, so no is_looking_forward() swap belongs here.)
    m_parent.set_color_clip_plane_alphas(side_visibility_alpha(m_upper_visibility),
                                         side_visibility_alpha(m_lower_visibility));
}

void GLGizmoCut3D::update_curved_cap_model()
{
    m_curved_cap_model.reset();
    m_curved_cap_dirty = false;

    const CommonGizmosDataObjects::SelectionInfo* sel = m_c->selection_info();
    const ModelObject* mo = sel ? sel->model_object() : nullptr;
    if (mo == nullptr || m_c->raycaster() == nullptr)
        return;
    const std::vector<const MeshRaycaster*> rcs = m_c->raycaster()->raycasters();
    if (rcs.empty())
        return;

    const int inst_idx = sel->get_active_instance();
    if (inst_idx < 0 || inst_idx >= int(mo->instances.size()))
        return;
    const Transform3d inst_matrix = mo->instances[inst_idx]->get_transformation().get_matrix();

    const Transform3d plane_to_world = translation_transform(m_plane_center) * m_rotation_m;
    const Transform3d world_to_plane = plane_to_world.inverse();

    // One raycaster per model-part volume, in that volume's own coordinates
    // (see Raycaster::on_update), so each needs its own plane -> volume map.
    std::vector<Transform3d> plane_to_vol;
    {
        size_t k = 0;
        for (const ModelVolume* mv : mo->volumes) {
            if (!mv->is_model_part())
                continue;
            if (k >= rcs.size())
                break;
            plane_to_vol.emplace_back((world_to_plane * inst_matrix * mv->get_matrix()).inverse());
            ++ k;
        }
    }
    if (plane_to_vol.empty())
        return;

    const int    n    = CurvedCutSheet::DefaultSamples;
    const double hs_u = m_curved_sheet.half_size_u();
    const double hs_v = m_curved_sheet.half_size_v();

    std::vector<Vec3d> pts(size_t(n) * size_t(n));
    for (int j = 0; j < n; ++ j) {
        const double v = double(j) / double(n - 1);
        const double y = (2.0 * v - 1.0) * hs_v;
        for (int i = 0; i < n; ++ i) {
            const double u = double(i) / double(n - 1);
            pts[size_t(j) * n + i] = Vec3d((2.0 * u - 1.0) * hs_u, y, m_curved_sheet.evaluate(u, v));
        }
    }

    // Inside test: shoot the sample point along the plane's +Z and count the
    // hits ahead of it. Odd means the point is inside that volume. One ray per
    // sample, not a boolean, and it is exactly the test the cap needs - the cap
    // IS the sheet, wherever the object is.
    std::vector<char> inside(pts.size(), 0);
    for (size_t vi = 0; vi < plane_to_vol.size(); ++ vi) {
        const AABBMesh& em = rcs[vi]->get_aabb_mesh();
        Vec3d dir = plane_to_vol[vi].linear() * Vec3d::UnitZ();
        if (dir.norm() < EPSILON)
            continue;
        dir.normalize();
        for (size_t p = 0; p < pts.size(); ++ p) {
            if (inside[p])
                continue;
            const std::vector<AABBMesh::hit_result> hits = em.query_ray_hits(plane_to_vol[vi] * pts[p], dir);
            size_t ahead = 0;
            for (const AABBMesh::hit_result& hr : hits)
                if (hr.is_hit() && hr.distance() > 0.)
                    ++ ahead;
            if ((ahead & 1) != 0)
                inside[p] = 1;
        }
    }

    // Triangulate the cells whose four corners are all inside. A cell with a
    // corner outside straddles the silhouette; dropping it costs at most one
    // sample spacing of cap round the rim, and the object's own shaded surface
    // shows through there, so the face still reads as closed.
    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    auto idx = [n](int i, int j) { return size_t(j) * size_t(n) + size_t(i); };

    // One sample spacing per axis: on a rectangular sheet they differ, and a
    // finite difference taken over the wrong step gives the wrong slope.
    const double du = 2.0 * hs_u / double(n - 1);
    const double dv = 2.0 * hs_v / double(n - 1);
    std::vector<int> remap(pts.size(), -1);
    std::vector<Vec3i32> tris;
    for (int j = 0; j + 1 < n; ++ j)
        for (int i = 0; i + 1 < n; ++ i) {
            const size_t corner[4] = { idx(i, j), idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1) };
            if (!inside[corner[0]] || !inside[corner[1]] || !inside[corner[2]] || !inside[corner[3]])
                continue;
            for (size_t s : corner)
                if (remap[s] < 0) {
                    remap[s] = int(init_data.vertices_count());
                    // The height field's own surface normal, (-df/dx, -df/dy, 1),
                    // so the face is shaded as the curve it is rather than flat.
                    const double fx = (m_curved_sheet.evaluate_local(pts[s].x() + du, pts[s].y()) -
                                       m_curved_sheet.evaluate_local(pts[s].x() - du, pts[s].y())) / (2.0 * du);
                    const double fy = (m_curved_sheet.evaluate_local(pts[s].x(), pts[s].y() + dv) -
                                       m_curved_sheet.evaluate_local(pts[s].x(), pts[s].y() - dv)) / (2.0 * dv);
                    const Vec3d nrm = Vec3d(-fx, -fy, 1.0).normalized();
                    init_data.add_vertex((Vec3f) pts[s].cast<float>(), (Vec3f) nrm.cast<float>());
                }
            tris.emplace_back(Vec3i32(remap[corner[0]], remap[corner[1]], remap[corner[2]]));
            tris.emplace_back(Vec3i32(remap[corner[0]], remap[corner[2]], remap[corner[3]]));
        }

    if (tris.empty())
        return;
    init_data.reserve_indices(tris.size() * 3);
    for (const Vec3i32& t : tris)
        init_data.add_triangle((unsigned int) t(0), (unsigned int) t(1), (unsigned int) t(2));

    m_curved_cap_model.init_from(std::move(init_data));
}

void GLGizmoCut3D::render_curved_cap()
{
    if (!is_curved_surface() || m_curved_sheet.is_flat() || m_connectors_editing)
        return;

    // The cap costs a ray per sample, so key it on everything it depends on and
    // skip the redraws (camera orbit, hover) that changed none of it. While a
    // control point is being dragged the cap is left as it was and the panel
    // says so, which keeps the drag at the frame rate the shaded halves - which
    // are only a texture upload - already run at.
    size_t key = 0;
    auto mix = [&key](double d) { key = key * 1000003u + std::hash<double>{}(d); };
    for (double z : m_curved_sheet.values())
        mix(z);
    mix(m_curved_sheet.half_size_u());
    mix(m_curved_sheet.half_size_v());
    for (int i = 0; i < 3; ++ i)
        mix(m_plane_center[i]);
    for (int r = 0; r < 3; ++ r)
        for (int c = 0; c < 3; ++ c)
            mix(m_rotation_m(r, c));

    if (m_curved_drag_ctl >= 0)
        m_curved_cap_stale = (key != m_curved_cap_key);
    else if (key != m_curved_cap_key || m_curved_cap_dirty) {
        m_curved_cap_key   = key;
        m_curved_cap_stale = false;
        update_curved_cap_model();
    }

    if (!m_curved_cap_model.is_initialized())
        return;

    // Phase 2 fix: the cap FOLLOWS the visibility of the side it belongs to.
    // Before, it was always drawn solid, so a half set to Ghost or Hidden still
    // showed a fully opaque cut face floating where the half used to be - which
    // is what "only the intersection of the plane and the part is drawn" meant
    // in the report.
    //
    // The cap is the face of whichever half is towards the camera (the same
    // choice is_looking_forward() already makes for its colour). When that half
    // is Hidden the cap belongs to the FAR half instead, so it is drawn in the
    // far half's colour and follows the far half's state; when both are hidden
    // there is no face left to draw.
    const bool near_is_lower = is_looking_forward();
    SideVisibility near_vis  = near_is_lower ? m_lower_visibility : m_upper_visibility;
    SideVisibility far_vis   = near_is_lower ? m_upper_visibility : m_lower_visibility;
    ColorRGBA      cap_color = near_is_lower ? LOWER_PART_COLOR : UPPER_PART_COLOR;
    if (near_vis == SideVisibility::Hidden) {
        // The near half is gone; the cut face the camera now sees is the far one.
        near_vis  = far_vis;
        cap_color = near_is_lower ? UPPER_PART_COLOR : LOWER_PART_COLOR;
    }
    if (near_vis == SideVisibility::Hidden)
        return;

    const float cap_alpha = side_visibility_alpha(near_vis);
    cap_color.a(cap_color.a() * cap_alpha);

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * translation_transform(m_plane_center) * m_rotation_m);
    // The same two colours the flat cap uses, picked by which half is towards
    // the camera - see is_looking_forward() and apply_color_clip_plane_colors().
    m_curved_cap_model.set_color(cap_color);

    GLboolean cull_face = GL_FALSE;
    ::glGetBooleanv(GL_CULL_FACE, &cull_face);
    glsafe(::glDisable(GL_CULL_FACE));

    // A ghosted cap blends and does not write depth, the same rule the ghosted
    // half itself follows in GLVolumeCollection::render.
    GLboolean was_blend = GL_FALSE;
    GLboolean depth_mask = GL_TRUE;
    const bool ghost_cap = curved_cut_side_is_ghost(cap_alpha);
    if (ghost_cap) {
        ::glGetBooleanv(GL_BLEND, &was_blend);
        ::glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        glsafe(::glDepthMask(GL_FALSE));
    }

    m_curved_cap_model.render();

    if (ghost_cap) {
        glsafe(::glDepthMask(depth_mask));
        if (!was_blend)
            glsafe(::glDisable(GL_BLEND));
    }
    if (cull_face)
        glsafe(::glEnable(GL_CULL_FACE));

    shader->stop_using();
}

Vec3d GLGizmoCut3D::curved_control_world(int i, int j) const
{
    return m_plane_center + m_rotation_m * m_curved_sheet.control_pos(i, j);
}

// ---------------------------------------------------------------------------
// PHASE 4: connectors on a curved cut.
//
// A connector is stored as a position in the OBJECT's frame plus a rotation.
// On a flat cut that rotation is the plane's, shared by every connector, and the
// position lies on the plane. On a curved cut both become per-connector and both
// are derived from the SHEET - and, crucially, derived on demand rather than
// stored, so the connector carries no new state: the (u,v) is just the position's
// own in-plane part, and the height and the frame are read from f(u,v) whenever
// they are needed. That is what makes a connector FOLLOW a later sheet edit, and
// it is why the 3MF round trip needs nothing new (the baked pos + rotation_m
// that already persist are exactly what a re-opened project needs to draw the
// connector where it was, sheet or no sheet).
// ---------------------------------------------------------------------------

Vec2d GLGizmoCut3D::connector_plane_xy(const Vec3d& pos_object) const
{
    const CommonGizmosDataObjects::SelectionInfo* sel = m_c->selection_info();
    Vec3d instance_offset = Vec3d::Zero();
    double sla_shift = 0.0;
    if (sel && sel->model_object() && sel->get_active_instance() >= 0) {
        instance_offset = sel->model_object()->instances[sel->get_active_instance()]->get_offset();
        sla_shift       = double(sel->get_sla_shift());
    }
    Vec3d world = pos_object + instance_offset;
    world.z() += sla_shift;
    const Vec3d local = m_rotation_m.inverse() * (world - m_plane_center);
    return Vec2d(local.x(), local.y());
}

Vec3d GLGizmoCut3D::project_onto_cut_plane(const Vec3d& pos_world) const
{
    const Vec3d local = m_rotation_m.inverse() * (pos_world - m_plane_center);
    return m_plane_center + m_rotation_m * Vec3d(local.x(), local.y(), 0.0);
}

Transform3d GLGizmoCut3D::connector_rotation_m(const Vec3d& pos_object) const
{
    // PHASE 2: on a DRAWN cut the frame comes from the ruled strip's own normal at
    // the connector's (s, w), composed after the plane's m_rotation_m exactly the way
    // the sheet's is - so a connector stands perpendicular to the surface the cut
    // will make, and its hole and its plug come out coaxial in the two halves.
    if (draw_connectors_live()) {
        double s = 0.0, w = 0.0;
        if (draw_connector_sw(pos_object, s, w))
            return m_rotation_m * draw_cut_surface_frame(m_draw_stroke, m_draw_params, s, w);
        return m_rotation_m;
    }

    if (!is_curved_surface() || m_curved_sheet.is_flat())
        return m_rotation_m;
    const Vec2d xy = connector_plane_xy(pos_object);
    // The sheet's frame is expressed in the PLANE's frame, so composing it after
    // m_rotation_m puts it in the world - and on a flat sheet it is the identity,
    // so this collapses to m_rotation_m exactly.
    return m_rotation_m * curved_cut_sheet_frame(m_curved_sheet, xy.x(), xy.y());
}

Transform3d GLGizmoCut3D::connector_rotation_m(const CutConnector& connector) const
{
    return connector_rotation_m(connector.pos);
}

Vec3d GLGizmoCut3D::connector_pos_on_sheet(const Vec3d& pos_object) const
{
    // PHASE 2, Draw: nothing to lift. The sheet's version exists because a connector
    // placed on the flat plane has to be raised to f(u,v) when the sheet is bent
    // afterwards - the (u,v) is the state and the height is derived. A drawn
    // connector's position comes from a raycast against the cut surface itself and is
    // already ON it, and there is no "same (u,v), new height" relationship to restore
    // when the stroke changes: the stroke moving is the surface moving, so the
    // sensible thing is to leave the connector where the user put it.
    if (draw_connectors_live())
        return pos_object;

    if (!is_curved_surface() || m_curved_sheet.is_flat())
        return pos_object;
    const Vec2d  xy = connector_plane_xy(pos_object);
    const double f  = m_curved_sheet.evaluate_local(xy.x(), xy.y());
    // Lift along the PLANE normal by f: the point (x, y, f) in the plane frame is
    // the point of the sheet over (x,y), which is where the connector belongs.
    return pos_object + f * (m_rotation_m.linear() * Vec3d::UnitZ());
}

void GLGizmoCut3D::update_curved_sheet_raycaster()
{
    if (!m_curved_pick_dirty && m_curved_pick_raycaster)
        return;

    // The same dense grid the preview draws, so the ray hits exactly the surface
    // the user is looking at, taken to the WORLD through the base plane's frame.
    indexed_triangle_set its = m_curved_sheet.sample_sheet(CurvedCutSheet::DefaultSamples);
    its_transform(its, translation_transform(m_plane_center) * m_rotation_m);
    m_curved_pick_mesh = TriangleMesh(std::move(its));
    m_curved_pick_raycaster = m_curved_pick_mesh.empty() ? nullptr
                            : std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(m_curved_pick_mesh));
    m_curved_pick_dirty = false;
}

bool GLGizmoCut3D::unproject_on_curved_sheet(const Vec2d& mouse_position, Vec3d& pos, Vec3d& pos_world, bool respect_contours)
{
    // PHASE 2: in Draw mode the click has to hit the DRAWN surface - the translucent
    // shell the user is looking at - rather than the flat plane, which on a drawn cut
    // is not where the cut goes at all. This is the one entry point both add_connector()
    // and dragging_connector() go through, which is why extending it here is the whole
    // of the plumbing.
    //
    // A MISS DOES NOT FALL BACK TO THE PLANE, unlike the sheet's version. The sheet
    // spans the plane and a click past its rim still means something sensible; the
    // drawn surface is the cut and a click that misses it is a click on nothing, so
    // putting a connector on the plane there would place it where the cut is not.
    if (draw_connectors_live())
        return unproject_on_draw_surface(mouse_position, pos, pos_world);

    if (!is_curved_surface() || m_curved_sheet.is_flat())
        return unproject_on_cut_plane(mouse_position, pos, pos_world, respect_contours);

    update_curved_sheet_raycaster();
    if (!m_curved_pick_raycaster)
        return unproject_on_cut_plane(mouse_position, pos, pos_world, respect_contours);

    const Camera& camera = wxGetApp().plater()->get_camera();
    Vec3f hit_f, normal_f;
    // The sheet mesh is already in the world, so the raycaster's own transform is
    // the identity here.
    if (!m_curved_pick_raycaster->unproject_on_mesh(mouse_position, Transform3d::Identity(), camera, hit_f, normal_f))
        // The ray missed the sheet entirely (the user clicked past its rim).
        // Fall back to the flat plane so the click still lands somewhere sensible
        // rather than doing nothing.
        return unproject_on_cut_plane(mouse_position, pos, pos_world, respect_contours);

    const Vec3d hit = hit_f.cast<double>();

    if (respect_contours) {
        // The contour test is a 2D test in the CLIP PLANE's frame, so it has to
        // see the point projected back down onto the plane - the sheet point
        // itself sits off the plane by f and would test against the wrong place.
        const Vec3d local = m_rotation_m.inverse() * (hit - m_plane_center);
        const Vec3d on_plane = m_plane_center + m_rotation_m * Vec3d(local.x(), local.y(), 0.0);
        if (m_c->object_clipper()) {
            const int cont_id = m_c->object_clipper()->is_projection_inside_cut(on_plane);
            if (cont_id == -1)
                return false;
            if (m_part_selection.valid()) {
                const std::vector<size_t>& ign = *m_part_selection.get_ignored_contours_ptr();
                if (std::find(ign.begin(), ign.end(), size_t(cont_id)) != ign.end())
                    return false;
            }
        }
    }

    const CommonGizmosDataObjects::SelectionInfo* sel = m_c->selection_info();
    Vec3d hit_d = hit;
    if (sel && sel->model_object() && sel->get_active_instance() >= 0) {
        hit_d -= sel->model_object()->instances[sel->get_active_instance()]->get_offset();
        hit_d.z() -= double(sel->get_sla_shift());
    }

    pos       = hit_d;
    pos_world = hit;
    return true;
}

double GLGizmoCut3D::connector_extent(const CutConnector& connector) const
{
    if (connector.attribs.type == CutConnectorType::FlexiJoint) {
        // The joint's real outline, not a bounding circle: a hinge is long and
        // thin, and it is its LONG dimension that decides whether the patch under
        // it is flat enough for the knuckle run to sit straight.
        double e = 0.0;
        for (const Vec2d& c : flexi_footprint_corners(connector.flexi))
            e = std::max(e, c.norm());
        return e > 0.0 ? e : double(connector.radius);
    }
    return double(connector.radius);
}

void GLGizmoCut3D::update_curved_connector_warnings()
{
    m_curved_tilted_connectors = 0;
    m_curved_unflat_connectors = 0;
    if (!is_curved_surface() || m_curved_sheet.is_flat())
        return;
    const CommonGizmosDataObjects::SelectionInfo* sel = m_c->selection_info();
    const ModelObject* mo = sel ? sel->model_object() : nullptr;
    if (mo == nullptr)
        return;

    for (const CutConnector& connector : mo->cut_connectors) {
        const Vec2d xy = connector_plane_xy(connector.pos);
        if (curved_cut_sheet_tilt_deg(m_curved_sheet, xy.x(), xy.y()) > CurvedConnectorTiltWarnDeg)
            ++ m_curved_tilted_connectors;
        // Only the STRAIGHT-featured kinds care: a hinge's knuckle run and a
        // thread's pitch line are generated as if for a plane, so a patch whose
        // curvature radius is comparable to the connector's own extent will not
        // let them mate. A plug or a dowel is a solid of revolution about the
        // local normal and sits fine on a curved patch.
        if (connector.attribs.type == CutConnectorType::FlexiJoint &&
            (connector.flexi.kind == FlexiJointKind::Hinge || connector.flexi.kind == FlexiJointKind::Thread) &&
            !curved_cut_patch_is_flat_enough(m_curved_sheet, xy.x(), xy.y(), connector_extent(connector)))
            ++ m_curved_unflat_connectors;
    }
}

void GLGizmoCut3D::render_curved_control_points()
{
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));

    const Camera&     camera      = wxGetApp().plater()->get_camera();
    const Transform3d view_matrix = camera.get_view_matrix();
    const double      mean_size   = get_grabber_mean_size(m_bounding_box);
    const double      size        = 0.5 * get_half_size(mean_size);

    // Flat control-point ids are row-major with the ROW STRIDE = nx, so
    // id = j * nx + i and the decode below is i = id % nx, j = id / nx.
    const int nx = m_curved_sheet.nx();
    const int ny = m_curved_sheet.ny();
    for (int j = 0; j < ny; ++ j)
        for (int i = 0; i < nx; ++ i) {
            const int  id      = j * nx + i;
            const bool hovered = (id == m_curved_hover_ctl) || (id == m_curved_drag_ctl) || (id == m_curved_snap_ctl);
            const ColorRGBA color = hovered ? ColorRGBA::ORANGE() :
                                    m_curved_sheet.at(i, j) != 0.0 ? GRABBER_COLOR : ColorRGBA::GRAY();
            render_model(m_sphere.model, color,
                         view_matrix * translation_transform(curved_control_world(i, j)) *
                         scale_transform(hovered ? size * 1.4 : size));
        }

    render_curved_snap_marker();
}

// ---------------------------------------------------------------------------
// Phase 2 (3): snapping a handle onto the model.
//
// Right-click-drag a handle: it rides the model surface directly under it,
// updating live, and the button release commits (one undo snapshot, taken on
// the press, exactly as the left drag does). Shift carries the neighbours along
// with the Bend radius falloff.
//
// "Under it" is along the plane normal in BOTH directions - a handle floating
// above the part snaps down onto it, one buried inside snaps out to whichever
// face is nearer. That means the surface it finds may be on the far side of the
// object from the camera, which is exactly why this wants the side-visibility
// control above: hide or ghost the near half and you can see what you are
// snapping to.
// ---------------------------------------------------------------------------

bool GLGizmoCut3D::curved_snap_apply(int ctl, bool falloff)
{
    if (ctl < 0 || m_curved_snap_mesh.empty())
        return false;

    const int   nx = m_curved_sheet.nx();
    const int   i  = ctl % nx;
    const int   j  = ctl / nx;
    const Vec2d xy = m_curved_sheet.control_xy(i, j);

    // The gesture is ABSOLUTE, like the left drag: measure and apply from the
    // grid the gesture STARTED on, never from the sheet as it stands. Measuring
    // from the current sheet would find the handle already sitting on the
    // surface after the first event, report a delta of zero, and then apply
    // that zero to the start grid - undoing the snap on the very next motion.
    const size_t k0  = size_t(j) * size_t(nx) + size_t(i);
    const double z0  = k0 < m_curved_drag_grid.size() ? m_curved_drag_grid[k0] : m_curved_sheet.at(i, j);
    const Vec3d  pos(xy.x(), xy.y(), z0);

    double dist = 0.0;
    if (!curved_cut_snap_distance(m_curved_snap_mesh, pos, dist)) {
        // No surface above or below this handle. Leave the point alone - the
        // gesture is a no-op there, not a reason to fling it to zero.
        m_curved_snap_hit_valid = false;
        return false;
    }

    m_curved_snap_hit       = Vec3d(xy.x(), xy.y(), z0 + dist);
    m_curved_snap_hit_valid = true;

    m_curved_sheet.set_values(m_curved_drag_grid);
    if (falloff && m_curved_brush_radius > 0.f)
        m_curved_sheet.grab(xy, double(m_curved_brush_radius), dist, m_curved_falloff);
    else
        m_curved_sheet.at(i, j) += dist;

    invalidate_curved_sheet();
    return true;
}

void GLGizmoCut3D::render_curved_snap_marker()
{
    if (m_curved_snap_ctl < 0 || !m_curved_snap_hit_valid)
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Camera& camera = wxGetApp().plater()->get_camera();
    const double  mean   = get_grabber_mean_size(m_bounding_box);
    const double  arm    = 0.6 * get_half_size(mean);

    // A three-axis crosshair at the hit, in the PLANE's frame, so it reads as
    // "this is where the handle is going to land on the surface".
    GLModel::Geometry g;
    g.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    g.reserve_vertices(6);
    g.reserve_indices(6);
    for (int a = 0; a < 3; ++ a) {
        Vec3d d = Vec3d::Zero();
        d[a] = arm;
        g.add_vertex((Vec3f) (m_curved_snap_hit - d).cast<float>());
        g.add_vertex((Vec3f) (m_curved_snap_hit + d).cast<float>());
        g.add_line((unsigned int) (2 * a), (unsigned int) (2 * a + 1));
    }
    GLModel marker;
    marker.init_from(std::move(g));
    marker.set_color(ColorRGBA::ORANGE());

    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("view_model_matrix",
                        camera.get_view_matrix() * translation_transform(m_plane_center) * m_rotation_m);
    OpenGLManager::set_line_width(2.f);
    marker.render();
    OpenGLManager::set_line_width(1.f);
    shader->stop_using();
}

int GLGizmoCut3D::curved_pick_control(const Vec2d& mouse_position) const
{
    const Camera& camera = wxGetApp().plater()->get_camera();

    // A fixed screen-space pick radius, in pixels. Generous enough that a
    // handle is grabbable when the grid is dense and the view zoomed out.
    const double pick_px2 = 18.0 * 18.0;

    int    best    = -1;
    double best_d2 = pick_px2;
    const int nx = m_curved_sheet.nx();
    const int ny = m_curved_sheet.ny();
    for (int j = 0; j < ny; ++ j)
        for (int i = 0; i < nx; ++ i) {
            // CameraUtils::project gives screen coordinates directly, the same
            // frame the mouse position arrives in.
            const Slic3r::Point p  = CameraUtils::project(camera, curved_control_world(i, j));
            const Vec2d         sp(double(p.x()), double(p.y()));
            const double        d2 = (sp - mouse_position).squaredNorm();
            if (d2 < best_d2) {
                best_d2 = d2;
                best    = j * nx + i;
            }
        }
    return best;
}

bool GLGizmoCut3D::curved_drag_delta(const Vec2d& mouse_position, double& delta) const
{
    // The Sculpt gizmo's drag-plane projection: a camera-facing plane through
    // the anchor. Then keep only the component along the cut plane normal -
    // f(u,v) is a height over the plane, not a free 3D position.
    const Camera& camera = wxGetApp().plater()->get_camera();
    const Linef3  ray    = m_parent.mouse_ray(Point(int(mouse_position.x()), int(mouse_position.y())));
    const Vec3d   dir    = ray.b - ray.a;
    const Vec3d   n      = camera.get_dir_forward();
    const double  denom  = n.dot(dir);
    if (std::abs(denom) < EPSILON)
        return false;
    const double t   = n.dot(m_curved_drag_anchor_world - ray.a) / denom;
    const Vec3d  hit = ray.a + t * dir;

    Vec3d normal = m_rotation_m * Vec3d::UnitZ();
    if (normal.norm() < EPSILON)
        return false;
    normal.normalize();
    delta = (hit - m_curved_drag_anchor_world).dot(normal);
    return true;
}

// ---------------------------------------------------------------------------
// Curved cut: gizmo-local undo/redo for SHEET edits.
//
// The sheet never reaches the Model, so the plater's undo stack - which the
// sheet edits already take snapshots on - restores the model and leaves the
// control grid untouched. That is the whole of "Ctrl+Z does not undo a control
// point drag". The stack below is the missing half: it stores the surface
// itself, and the keyboard hook (on_cut_char) is offered the key before the
// canvas's own Ctrl+Z, exactly the way GLGizmoSculpt takes its modal keys.
// ---------------------------------------------------------------------------

const size_t GLGizmoCut3D::CurvedUndoLimit;

GLGizmoCut3D::CurvedSheetState GLGizmoCut3D::curved_sheet_state() const
{
    CurvedSheetState st;
    st.values      = m_curved_sheet.values();
    st.nx          = m_curved_sheet.nx();
    st.ny          = m_curved_sheet.ny();
    st.half_size_u = m_curved_sheet.half_size_u();
    st.half_size_v = m_curved_sheet.half_size_v();
    return st;
}

void GLGizmoCut3D::apply_curved_sheet_state(const CurvedSheetState& st)
{
    if (st.nx <= 0 || st.ny <= 0 || st.values.size() != size_t(st.nx) * size_t(st.ny))
        return;

    // Order matters. reset() is the only way to change the grid without
    // re-sampling (set_grid would re-sample the CURRENT surface onto the new
    // grid, which is the opposite of restoring one), and the extent has to be
    // set with resample == false so the stored values land verbatim.
    //
    // BOTH counts are compared, not just the value count: a 10 x 2 and a 2 x 10
    // hold twenty values each, so restoring across them would silently transpose
    // the surface rather than refuse.
    if (m_curved_sheet.nx() != st.nx || m_curved_sheet.ny() != st.ny)
        m_curved_sheet.reset(st.nx, st.ny);
    m_curved_sheet.set_half_size(st.half_size_u, st.half_size_v, /*resample*/ false);
    m_curved_sheet.set_values(st.values);
    // set_values() republishes the reference, so a later extent re-fit
    // re-samples the RESTORED surface rather than the one we just left.
    m_curved_nx = m_curved_sheet.nx();
    m_curved_ny = m_curved_sheet.ny();

    // The extent we just restored is the one the fit should consider current;
    // otherwise the next re-fit would immediately resample it away.
    m_curved_fit_center   = m_plane_center;
    m_curved_fit_rotation = m_rotation_m;
    m_curved_fit_valid    = true;
    m_curved_fit_pending  = false;

    // Rebuild the preview the same way a control-point drag does.
    invalidate_curved_sheet();
    update_curved_empty_sides();
    update_curved_connector_warnings();
    m_curved_hover_ctl = m_curved_drag_ctl = -1;
    // Only re-derive the bend radius while the user has not set one; <= 0 is the
    // "not chosen yet" marker the panel already uses. Undoing a handle nudge must
    // not silently resize their brush.
    if (m_curved_brush_radius <= 0.f)
        m_curved_brush_radius = default_curved_bend_radius();
    m_parent.set_as_dirty();
}

void GLGizmoCut3D::push_curved_undo()
{
    if (m_surface_mode != CutSurfaceMode::Curved)
        return;
    m_curved_undo.emplace_back(curved_sheet_state());
    if (m_curved_undo.size() > CurvedUndoLimit)
        m_curved_undo.erase(m_curved_undo.begin());
    // A new edit ends the redo branch, the way every undo stack does.
    m_curved_redo.clear();
}

bool GLGizmoCut3D::curved_undo()
{
    if (m_surface_mode != CutSurfaceMode::Curved || m_curved_undo.empty())
        return false;
    // Park the CURRENT state on the redo stack before restoring, so redo has
    // somewhere to come back to.
    m_curved_redo.emplace_back(curved_sheet_state());
    const CurvedSheetState st = m_curved_undo.back();
    m_curved_undo.pop_back();
    apply_curved_sheet_state(st);
    return true;
}

bool GLGizmoCut3D::curved_redo()
{
    if (m_surface_mode != CutSurfaceMode::Curved || m_curved_redo.empty())
        return false;
    m_curved_undo.emplace_back(curved_sheet_state());
    const CurvedSheetState st = m_curved_redo.back();
    m_curved_redo.pop_back();
    apply_curved_sheet_state(st);
    return true;
}

bool GLGizmoCut3D::on_cut_char(int key_code, bool shift_down, bool ctrl_down)
{
    if (m_state != On)
        return false;

    // ESC IN DRAW MODE CLEARS THE LINE. It has to be claimed here, ahead of
    // GLGizmosManager's own Esc branch, because that branch calls
    // reset_all_states() - i.e. it CLOSES the gizmo - and "Esc clears the stroke"
    // is the gesture the research spec asks for. Returning true is what keeps the
    // gizmo open; with no stroke to clear it falls through and Esc closes the
    // gizmo as it always did.
    if (!ctrl_down && !shift_down && key_code == WXK_ESCAPE && is_draw_surface() && !m_connectors_editing) {
        // PHASE 2: Esc leaves EDIT POINTS first. The line is the thing the user has
        // just spent effort on, so the escalation is "stop editing" before "throw the
        // line away" - one more Esc does the latter, which is what it always did.
        if (m_draw_editing) {
            m_draw_editing = false;
            m_draw_hover_pt = m_draw_drag_pt = -1;
            m_parent.set_as_dirty();
            return true;
        }
        if (m_draw_capturing) {
            // Mid-stroke: abandon the STROKE being drawn. 2026-09-12 this is simply
            // dropping the capture buffer - the chain was never touched, because the
            // append happens on LeftUp - so the undo entry the press pushed goes back
            // too rather than being consumed to restore something that never changed.
            m_draw_capturing  = false;
            m_draw_last_mouse = Vec2d::Zero();
            m_draw_snap_armed = false;
            m_draw_capture.clear();
            if (!m_draw_undo.empty())
                m_draw_undo.pop_back();
            invalidate_draw_stroke();
            m_parent.set_as_dirty();
            return true;
        }
        // The CHAIN, not the stroke: an open chain has a line the user can see, and
        // m_draw_stroke is deliberately empty for it, so gating on the stroke would
        // make Esc a no-op on exactly the state it is most wanted for.
        if (!m_draw_chain.empty()) {
            clear_draw_stroke(/*push_undo*/ true);
            return true;
        }
        return false;
    }

    if (!ctrl_down)
        return false;
    // Only while a SHAPED surface is the thing being edited - the sheet or the
    // stroke. In Flat mode, or while the connector panel is up, Ctrl+Z means what
    // it always meant and must fall straight through to the plater.
    //
    // THE TRAP THE RESEARCH SPEC FLAGS: this used to gate on is_curved_surface()
    // alone, so Draw would have got no Ctrl+Z at all - and the symptom (Ctrl+Z
    // undoing a MODEL action instead of a stroke) reads as confusing rather than
    // broken.
    if (!is_shaped_surface() || m_connectors_editing)
        return false;

    // wx delivers Ctrl+<letter> as either the control code or the letter,
    // depending on platform and on whether this came through EVT_CHAR; accept
    // both rather than guessing.
    const bool is_z = key_code == 'z' || key_code == 'Z' || key_code == WXK_CONTROL_Z;
    const bool is_y = key_code == 'y' || key_code == 'Y' || key_code == WXK_CONTROL_Y;

    // Ctrl+Shift+Z is redo as well, the second binding everyone expects.
    if (is_draw_surface()) {
        if (is_y || (is_z && shift_down))
            return draw_redo();
        if (is_z)
            return draw_undo();
        return false;
    }
    if (is_y || (is_z && shift_down))
        return curved_redo();
    if (is_z)
        return curved_undo();
    return false;
}

bool GLGizmoCut3D::curved_on_mouse(const wxMouseEvent& mouse_event)
{
    if (!is_curved_surface() || m_connectors_editing || m_hide_cut_plane)
        return false;

    const Vec2d mouse_pos(mouse_event.GetX(), mouse_event.GetY());

    // --- snap gesture (right button held) ---------------------------------
    if (m_curved_snap_ctl >= 0) {
        if (mouse_event.Dragging() || mouse_event.Moving()) {
            // The handle does not follow the CURSOR - it follows the surface
            // under itself. Re-applying on every motion event is what makes it
            // live: the plane or the grid may have moved under the gesture.
            if (curved_snap_apply(m_curved_snap_ctl, m_curved_snap_falloff))
                m_parent.set_as_dirty();
            return true;
        }
        if (mouse_event.RightUp() || mouse_event.Leaving()) {
            // Release commits: the snapshot was taken on the press, so there is
            // nothing to do but drop the gesture state.
            // Same as the drag: a snap that found nothing leaves the surface
            // untouched, so it must not leave an undo step either.
            if (!m_curved_undo.empty() && m_curved_undo.back().values == m_curved_sheet.values())
                m_curved_undo.pop_back();
            m_curved_snap_ctl       = -1;
            m_curved_snap_hit_valid = false;
            m_curved_snap_mesh.clear();
            m_parent.set_as_dirty();
            return true;
        }
        return true;
    }

    if (mouse_event.RightDown() && !mouse_event.CmdDown()) {
        const int ctl = curved_pick_control(mouse_pos);
        if (ctl < 0)
            return false; // not on a handle: let the normal right-click through
        // The mesh in the plane frame is fixed for the whole gesture; derive it
        // once here rather than per motion event.
        if (!curved_instance_mesh_in_plane(m_curved_snap_mesh))
            return false;
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Snap curved cut handle"), UndoRedo::SnapshotType::GizmoAction);
        // The sheet is gizmo-local, so the plater snapshot above cannot restore
        // it. Push the pre-gesture surface onto the gizmo's own stack too.
        push_curved_undo();
        m_curved_snap_ctl       = ctl;
        m_curved_hover_ctl      = ctl;
        m_curved_snap_falloff   = mouse_event.ShiftDown();
        m_curved_drag_grid      = m_curved_sheet.values();
        m_curved_snap_hit_valid = false;
        curved_snap_apply(ctl, m_curved_snap_falloff);
        m_parent.set_as_dirty();
        return true;
    }

    if (m_curved_drag_ctl >= 0) {
        if (mouse_event.Dragging()) {
            double delta = 0.0;
            if (curved_drag_delta(mouse_pos, delta)) {
                const int nx = m_curved_sheet.nx();
                const int i  = m_curved_drag_ctl % nx;
                const int j  = m_curved_drag_ctl / nx;
                // Put the dragged point back where the stroke started, then apply
                // the whole displacement as one falloff-weighted grab: dragging is
                // absolute, so a drag that comes back to where it began undoes
                // itself instead of accumulating.
                m_curved_sheet.set_values(m_curved_drag_grid);
                m_curved_sheet.grab(m_curved_sheet.control_xy(i, j), double(m_curved_brush_radius), delta, m_curved_falloff);
                invalidate_curved_sheet();
                m_parent.set_as_dirty();
            }
            return true;
        }
        if (mouse_event.LeftUp() || mouse_event.Leaving()) {
            // A click that did not actually bend anything (press and release on a
            // handle without moving) must not leave a no-op step on the undo stack -
            // the first Ctrl+Z would then appear to do nothing.
            if (!m_curved_undo.empty() && m_curved_undo.back().values == m_curved_sheet.values())
                m_curved_undo.pop_back();
            m_curved_drag_ctl = -1;
            m_parent.set_as_dirty();
            return true;
        }
        return true;
    }

    if (mouse_event.Moving()) {
        const int hover = curved_pick_control(mouse_pos);
        if (hover != m_curved_hover_ctl) {
            m_curved_hover_ctl = hover;
            m_parent.set_as_dirty();
        }
        // Do not consume a plain move: the rest of the gizmo needs it too.
        return false;
    }

    if (mouse_event.LeftDown() && !mouse_event.ShiftDown() && !mouse_event.CmdDown()) {
        const int ctl = curved_pick_control(mouse_pos);
        if (ctl < 0)
            return false;
        // One undo step per drag gesture, not per frame - the granularity the
        // research spec asks for, and the one GLGizmoSculpt uses for a stroke.
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Curved cut surface"), UndoRedo::SnapshotType::GizmoAction);
        // ... and onto the gizmo's own stack, which is the one Ctrl+Z will read:
        // the sheet never reaches the Model, so the plater snapshot rolls back
        // everything EXCEPT the control grid the user is about to move.
        push_curved_undo();
        m_curved_drag_ctl          = ctl;
        m_curved_hover_ctl         = ctl;
        m_curved_drag_grid         = m_curved_sheet.values();
        const int nx               = m_curved_sheet.nx();
        m_curved_drag_anchor_world = curved_control_world(ctl % nx, ctl / nx);
        return true;
    }

    return false;
}

// Visible / Ghost / Hidden, per half. Three radio buttons a side rather than a
// combo: the state has to be readable at a glance while the user is looking at
// the model, not at the panel.
void GLGizmoCut3D::render_side_visibility_inputs()
{
    auto row = [this](const wxString& label, const char* id, SideVisibility& value) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(label + ": ");
        ImGui::SameLine(m_label_width);

        // The button labels have to differ between the two rows or ImGui treats
        // them as the same widget; the "##u"/"##l" suffix is invisible and does
        // exactly that.
        const std::string suffix = std::string("##") + id;
        struct { const char* key; SideVisibility v; } opts[3] = {
            { "Visible", SideVisibility::Visible },
            { "Ghost",   SideVisibility::Ghost   },
            { "Hidden",  SideVisibility::Hidden  },
        };
        for (int k = 0; k < 3; ++ k) {
            if (k > 0)
                ImGui::SameLine();
            const std::string lbl = _u8L(opts[k].key) + suffix;
            bool sel = value == opts[k].v;
            if (m_imgui->bbl_radio_button(lbl.c_str(), sel)) {
                value = opts[k].v;
                apply_side_visibility();
                m_parent.set_as_dirty();
            }
        }
    };

    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Show halves") + ": ");
    row(_L("Upper"), "u", m_upper_visibility);
    row(_L("Lower"), "l", m_lower_visibility);
    ImGui::PushTextWrapPos(m_editing_window_width);
    m_imgui->text(_L("Ghost draws a half translucent so the cut surface and the other half show through; "
                     "Hidden leaves it out of the preview entirely. Neither changes the cut."));
    ImGui::PopTextWrapPos();
}

void GLGizmoCut3D::render_curved_surface_inputs()
{
    ImGui::Separator();

    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Surface") + ": ");
    ImGui::SameLine();

    // Named locals: bbl_radio_button takes a const char*, so the string has to
    // outlive the call.
    const std::string flat_label   = _u8L("Flat");
    const std::string curved_label = _u8L("Curved");
    const std::string draw_label   = _u8L("Draw");

    bool flat = m_surface_mode == CutSurfaceMode::Flat;
    if (m_imgui->bbl_radio_button(flat_label.c_str(), flat)) {
        m_surface_mode = CutSurfaceMode::Flat;
        invalidate_curved_sheet();
        invalidate_draw_stroke();
    }
    ImGui::SameLine();
    bool curved = m_surface_mode == CutSurfaceMode::Curved;
    if (m_imgui->bbl_radio_button(curved_label.c_str(), curved)) {
        m_surface_mode = CutSurfaceMode::Curved;
        // A fresh Curved session starts with no sheet history: the surfaces the
        // old entries describe belong to a sheet that has just been re-fitted.
        clear_curved_undo();
        // Start from the fallback extent, then fit to the cross-section at once
        // - the user has just asked for the sheet, so there is nothing to
        // debounce and the handles should already be over the part.
        m_curved_sheet.set_half_size(curved_sheet_half_size());
        m_curved_fit_valid = false;
        fit_curved_sheet_to_section(/*force*/ true);
        invalidate_curved_sheet();
    }
    ImGui::SameLine();
    bool draw = m_surface_mode == CutSurfaceMode::Draw;
    if (m_imgui->bbl_radio_button(draw_label.c_str(), draw)) {
        m_surface_mode = CutSurfaceMode::Draw;
        // A fresh Draw session starts with no line and no history. The sheet is left
        // alone rather than reset: switching back to Curved should find the surface
        // where it was.
        m_draw_chain.clear();
        m_draw_capture.clear();
        m_draw_snap_armed = false;
        m_draw_reject_msg = DrawRejectReason::None;
        m_draw_stroke.clear();
        clear_draw_undo();
        m_draw_capturing = false;
        // PHASE 2: and no handles, for the line that no longer exists.
        m_draw_editing = false;
        m_draw_hover_pt = m_draw_drag_pt = -1;
        m_draw_points.clear();
        m_draw_frame_flips = false;
        m_draw_ext_clipped = false;
        m_draw_surface_raycaster.reset();
        m_draw_surface_pick_dirty = true;
        invalidate_draw_stroke();
        invalidate_curved_sheet();
    }

    // Side visibility is useful on a flat cut too - "let me see the other half"
    // has nothing to do with the sheet - so it is offered in every mode.
    render_side_visibility_inputs();

    if (m_surface_mode == CutSurfaceMode::Draw) {
        render_draw_surface_inputs();
        return;
    }
    if (m_surface_mode != CutSurfaceMode::Curved)
        return;

    // Control grid: nx COLUMNS by ny ROWS. Resizing re-samples the current
    // surface onto the new grid, so the shape survives the change.
    //
    // "Square" locks the two counts together and is ON by default, which makes
    // this row behave exactly as the old single "Control points" slider did -
    // nothing changes for anyone who does not go looking for it. Unlock it and
    // the two counts move apart, which is how a RULED sheet is asked for: 10 x 2
    // gives ten columns each of which is a single straight line along v, so the
    // user bends a column by grabbing either of its two ends.
    int nx = m_curved_sheet.nx();
    int ny = m_curved_sheet.ny();
    if (m_curved_brush_radius <= 0.f)
        m_curved_brush_radius = default_curved_bend_radius();

    // Applies a new pair of counts: one undo entry, one re-sample, handles
    // dropped (their flat ids decode against nx and would point elsewhere).
    auto apply_grid = [this](int gx, int gy) {
        // The user has an opinion now, so the fit stops choosing for them.
        m_curved_res_user_set = true;
        // A grid change re-samples the surface, which is lossy - very much
        // something to be able to take back.
        push_curved_undo();
        m_curved_sheet.set_grid(gx, gy);
        m_curved_nx = m_curved_sheet.nx();
        m_curved_ny = m_curved_sheet.ny();
        m_curved_hover_ctl = m_curved_drag_ctl = -1;
        invalidate_curved_sheet();
        m_curved_brush_radius = default_curved_bend_radius();
    };

    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Control points") + ": ");
    ImGui::SameLine(m_label_width);
    // ImGui formats ONE value from a slider's format string, so a literal
    // "%d x %d" would read a second, garbage integer off the stack. Pre-format
    // the whole label with snprintf and hand ImGui a string with no conversion
    // left in it. (This is the format-string bug from the square panel; it stays
    // fixed, and now there really are two different numbers to show.)
    char grid_fmt[32];
    // MinResolution dropped to 2 so a ruled AXIS is expressible, but a 2 x 2
    // SQUARE grid is just a tilted plane - nothing the square slider ever
    // offered, and not a useful place to land by dragging. So the locked slider
    // keeps the old floor of 3 and only the per-axis ones go down to 2.
    static const int SquareMinResolution = 3;
    if (m_curved_square) {
        ImGui::PushItemWidth(m_control_width * 0.7f);
        int res = std::max({ nx, ny, SquareMinResolution });
        snprintf(grid_fmt, sizeof(grid_fmt), "%d x %d", res, res);
        if (ImGui::SliderInt("##curved_res", &res, SquareMinResolution, CurvedCutSheet::MaxResolution, grid_fmt))
            apply_grid(res, res);
        ImGui::PopItemWidth();
    }
    else {
        // Two sliders on one row: columns (along u/X) then rows (along v/Y).
        const float half = m_control_width * 0.34f;
        ImGui::PushItemWidth(half);
        snprintf(grid_fmt, sizeof(grid_fmt), "%d cols", nx);
        const bool nx_changed = ImGui::SliderInt("##curved_nx", &nx, CurvedCutSheet::MinResolution, CurvedCutSheet::MaxResolution, grid_fmt);
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Control points across the sheet (along X). 2 makes every row a single straight line.").c_str(), ImGui::GetFontSize() * 20.f);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::PushItemWidth(half);
        snprintf(grid_fmt, sizeof(grid_fmt), "%d rows", ny);
        const bool ny_changed = ImGui::SliderInt("##curved_ny", &ny, CurvedCutSheet::MinResolution, CurvedCutSheet::MaxResolution, grid_fmt);
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Control points down the sheet (along Y). 2 makes every column a single straight line, grabbable from either end - a ruled bend.").c_str(), ImGui::GetFontSize() * 20.f);
        ImGui::PopItemWidth();
        if (nx_changed || ny_changed)
            apply_grid(nx, ny);
    }

    // Locking back to square takes the LARGER count, so ticking the box never
    // silently throws away detail the user has already placed.
    ImGui::AlignTextToFramePadding();
    m_imgui->text(" ");
    ImGui::SameLine(m_label_width);
    bool square = m_curved_square;
    if (m_imgui->bbl_checkbox(_L("Square"), square)) {
        m_curved_square = square;
        if (square && m_curved_sheet.nx() != m_curved_sheet.ny()) {
            const int n = std::max(m_curved_sheet.nx(), m_curved_sheet.ny());
            apply_grid(n, n);
        }
    }
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("Keep the same number of control points across and down. Turn it off for a ruled sheet, e.g. 10 x 2.").c_str(), ImGui::GetFontSize() * 20.f);

    // How far a dragged handle pulls its neighbours along (falloff-weighted), so the sheet bends
    // as one surface instead of spiking at a single control point. Defaults to 1.5 control
    // spacings; 1 spacing moves the dragged handle alone.
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Bend radius") + ": ");
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(m_control_width * 0.7f);
    ImGui::SliderFloat("##curved_radius", &m_curved_brush_radius, 1.f, 200.f, "%.1f mm");
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("Neighbouring control points within this distance follow a dragged handle, less the further they are. Small: bend one point; large: bend the whole sheet.").c_str(), ImGui::GetFontSize() * 20.f);

    m_imgui->bbl_checkbox(_L("Falloff"), m_curved_falloff);

    if (m_imgui->button(_L("Smooth"), _L("One smoothing pass over the control grid"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Smooth curved cut surface"), UndoRedo::SnapshotType::GizmoAction);
        push_curved_undo();
        m_curved_sheet.smooth(0.5);
        invalidate_curved_sheet();
    }
    ImGui::SameLine();
    m_imgui->disabled_begin(m_curved_sheet.is_flat());
    if (m_imgui->button(_L("Reset surface"), _L("Flatten the surface back to the cut plane"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reset curved cut surface"), UndoRedo::SnapshotType::GizmoAction);
        push_curved_undo();
        m_curved_sheet.reset();
        invalidate_curved_sheet();
    }
    m_imgui->disabled_end();

    ImGui::PushTextWrapPos(m_editing_window_width);
    m_imgui->text(_L("Drag a handle to bend the cut surface. With every handle at zero the cut is "
                     "exactly the flat plane cut."));
    m_imgui->text(_L("Right-click-drag a handle to snap it onto the model surface; hold Shift to carry "
                     "its neighbours with it. The surface it finds may be on the far side of the part - "
                     "set that half to Ghost or Hidden above to see it."));
    // The shaded halves follow the sheet every frame (the shader does the split);
    // only the cut FACE waits for the drag to finish, so say which one is behind.
    if (m_curved_cap_stale)
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, _L("Cut face updating…"));
    // PHASE 3: say so BEFORE the cut when the sheet does not cross the part. The
    // cut still runs - it produces the one non-empty half rather than refusing -
    // but silence was the thing the owner reported as "it removes the smaller
    // part", so the reason is on screen.
    if (m_curved_upper_empty)
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                              _L("The surface does not cross the part on the upper side; that side would be empty."));
    if (m_curved_lower_empty)
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                              _L("The surface does not cross the part on the lower side; that side would be empty."));
    // PHASE 4: connectors are available now, and these are the two things a
    // connector on a curved surface can be wrong about. Both are ADVISORY - the
    // cut runs either way, because a deliberately tilted connector (or a hinge on
    // a gentle bend) is a legitimate thing to ask for.
    if (m_curved_tilted_connectors > 0)
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                              format_wxstr(_L("%1% connector(s) stand more than %2%\u00b0 off the cut direction; "
                                              "they will print at an angle and may need supports."),
                                           m_curved_tilted_connectors, int(CurvedConnectorTiltWarnDeg)));
    if (m_curved_unflat_connectors > 0)
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                              format_wxstr(_L("%1% Hinge/Thread connector(s) sit on a patch that curves too tightly "
                                              "for their straight features to mate. Flatten the surface under them "
                                              "or use a Plug, Dowel or Double ring instead."),
                                           m_curved_unflat_connectors));
    ImGui::PopTextWrapPos();
}

// ---------------------------------------------------------------------------
// Phase 3: cut thickness ("kerf"). Shared by Flat and Curved - it is a property
// of the CUT, not of the surface, so it sits next to the cut position rather
// than inside the curved-surface block.
// ---------------------------------------------------------------------------

// ===========================================================================
// DRAW CUT (phase 1)
//
// Draw is the third surface mode: instead of the flat plane or the curved
// sheet's height field, the cut surface is a RULED STRIP swept along a stroke
// the user paints on the model. The geometry is all in libslic3r/DrawCut.{hpp,cpp}
// where tests reach it; what lives here is the capture, the preview and the
// panel.
//
// THE PREVIEW, and why it is what it is. The curved cut's coloured halves come
// from the volume shader, which splits fragments by sampling the sheet's height
// field out of a texture (gouraud.fs, curved_sheet_*). That trick needs the cut
// surface to BE a height field over the plane, single-valued in (u,v) - and a
// ruled strip swept along a curve is exactly the thing that is not. The research
// spec offered the alternative of computing the real split in a background job on
// stroke end and previewing the resulting half meshes with a spinner.
//
// This takes a third, more robust route: render the CUTTER SOLID itself,
// translucent, over the model. That solid IS what the boolean will use - not an
// approximation of it, not a re-derivation - so what the user sees is what they
// will get, it needs no boolean and therefore no job, no spinner and no stale
// state, and it updates the instant any parameter changes rather than after a
// few hundred milliseconds of Manifold. The two halves are not separately
// coloured (the shader cannot tell them apart without the split), which is the
// deviation; the Visible / Ghost / Hidden side controls still work against the
// plane's own split, and ghosting the near half is what lets the shell be seen
// inside the part.
// ===========================================================================

void GLGizmoCut3D::invalidate_draw_stroke()
{
    m_draw_preview_dirty = true;
}

// The instance mesh is cached in the CUT PLANE's frame, so moving or turning the
// plane makes it stale - and both the empty-side test and the preview's bounding
// box read it. Dropping it here forces the next use to re-derive it in the new
// frame. (The stroke itself does NOT need re-deriving: it is expressed in that
// frame too, so it rides along with the plane the way the sheet does.)
void GLGizmoCut3D::invalidate_draw_pick_mesh()
{
    m_draw_pick_its.clear();
    m_draw_pick_mesh.clear();
    m_draw_raycaster.reset();
}

Vec3d GLGizmoCut3D::draw_view_dir_in_plane() const
{
    // The camera's forward direction, taken into the cut plane's frame - the frame
    // the stroke and the cutter live in. Forward, i.e. INTO the screen and so into
    // the part, which is the direction a "View" cut reaches.
    const Camera& camera = wxGetApp().plater()->get_camera();
    const Vec3d   fwd_world = camera.get_dir_forward();
    const Vec3d   d = m_rotation_m.linear().inverse() * fwd_world;
    return d.norm() > 1e-9 ? Vec3d(d.normalized()) : Vec3d(-Vec3d::UnitZ());
}

void GLGizmoCut3D::latch_draw_view_dir()
{
    m_draw_params.view_dir = draw_view_dir_in_plane();
}

bool GLGizmoCut3D::update_draw_raycaster()
{
    // ONE MESH PER GESTURE. curved_instance_mesh_in_plane() merges every
    // is_model_part() volume of the selected instance into the cut plane's frame -
    // the right frame, the right volume filter, one existing call - and it costs a
    // pass over the instance mesh, so it is derived on the press and kept for the
    // whole stroke. Re-deriving it per motion event stalls the drag on a heavy
    // model, which is the same rule m_curved_snap_mesh follows.
    if (m_draw_raycaster && !m_draw_pick_mesh.empty())
        return true;

    if (!curved_instance_mesh_in_plane(m_draw_pick_its))
        return false;
    m_draw_pick_mesh = TriangleMesh(m_draw_pick_its);
    if (m_draw_pick_mesh.empty())
        return false;
    m_draw_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(m_draw_pick_mesh));
    return true;
}

std::vector<double> GLGizmoCut3D::compute_draw_extension_clearance()
{
    // 2026-09-20, the extension-clip fix: the unclamped extension is a blind
    // extrusion, and where another wall of the part - or a branch just past the
    // stroke's end - sits within Extension of the line, the extended surface ran
    // through it and the boolean cut a slot into the neighbour. Each entry below
    // is the distance from one rail's base point along its extension direction to
    // the first mesh hit (infinity when there is none), minus a small margin, so
    // the cutter can clamp the outward rail per sample instead of trusting the
    // slider value everywhere.
    //
    // Layout matches the cutter's rail order: [front tangent end, path samples
    // 0..n-1, back tangent end]. EMPTY means "no clamp" - closed strokes (the
    // band/skirt is a different mechanism), no valid stroke, or no raycaster all
    // keep the legacy behaviour.
    std::vector<double> out;
    if (!m_draw_stroke.valid() || m_draw_stroke.is_closed())
        return out;
    if (!update_draw_raycaster() || !m_draw_raycaster)
        return out;

    const std::vector<DrawCutSample>& p = m_draw_stroke.path();
    const auto& em = m_draw_raycaster->get_aabb_mesh();

    // The origin starts 0.05 mm along the cast direction so the facet the stroke
    // was drawn on cannot be the first hit; the same 0.05 mm comes back out of
    // the reported distance, and a further 0.1 mm margin keeps grazing
    // self-intersections (a curved wall the ray clips on its own way out) from
    // zeroing the clamp.
    auto clearance_along = [&em](const Vec3d& from, const Vec3d& dir) -> double {
        const Vec3d d = dir.norm() > 1e-9 ? Vec3d(dir.normalized()) : -Vec3d::UnitZ();
        const auto  hit = em.query_ray_hit(from + 0.05 * d, d);
        const double t  = hit.is_hit() ? hit.distance() + 0.05 : std::numeric_limits<double>::max();
        return std::max(0.0, t - 0.1);
    };

    const size_t n = p.size();
    out.reserve(n + 2);
    // Front tangent end, then one outward query per sample, then the back end.
    out.push_back(clearance_along(p.front().pos, -m_draw_stroke.tangent(0)));
    for (size_t i = 0; i < n; ++ i)
        out.push_back(clearance_along(p[i].pos, -draw_cut_inward_dir(m_draw_stroke, m_draw_params, i)));
    out.push_back(clearance_along(p.back().pos, m_draw_stroke.tangent(n - 1)));
    return out;
}

bool GLGizmoCut3D::draw_sample_at(const Vec2d& mouse_position)
{
    if (!update_draw_raycaster())
        return false;

    const Camera& camera = wxGetApp().plater()->get_camera();
    // The pick mesh is in the PLANE frame, so the raycaster's trafo is what takes
    // the plane frame to the world. The hit and its normal come back in the plane
    // frame, which is exactly where the stroke is stored.
    const Transform3d plane_to_world = translation_transform(m_plane_center) * m_rotation_m;

    Vec3f  hit_f, normal_f;
    size_t facet = 0;
    // unproject_on_mesh returns FALSE when the hit count is odd, i.e. when the
    // nearest hit would be from inside the mesh - a ray starting inside the part is
    // refused rather than clamped. That is the behaviour wanted here: a click that
    // lands inside is not a point on the surface.
    if (!m_draw_raycaster->unproject_on_mesh(mouse_position, plane_to_world, camera, hit_f, normal_f, nullptr, &facet))
        return false;

    // 2026-09-12: the sample goes into the IN-PROGRESS STROKE, not straight into the
    // line. The chain only learns about it on LeftUp, which is what lets a gesture be
    // abandoned (Esc, or a press that turns out not to continue the chain) without the
    // line ever having changed.
    DrawCutSample smp;
    smp.pos    = hit_f.cast<double>();
    smp.normal = normal_f.cast<double>().normalized();
    smp.facet  = facet;
    m_draw_capture.push_back(smp);
    // The painter's contract: the last mouse position is re-set only on an actual
    // HIT, which is what makes a miss non-fatal - the next hit interpolates from
    // the last place the ray found the surface rather than from a miss.
    m_draw_last_mouse = mouse_position;
    return true;
}

void GLGizmoCut3D::draw_interpolate_to(const Vec2d& mouse_position)
{
    // GLGizmoPainterBase::get_projected_mouse_positions()'s interpolation loop
    // (GLGizmoPainterBase.cpp 403-417), and ONLY that loop: without it a quick
    // flick leaves a metre-long chord across the part, because one wx Dragging
    // event can jump a hundred pixels. GLGizmoSculpt does not interpolate - a
    // sphere brush closes its own gaps - but a thin line has no such forgiveness.
    //
    // NOT reused: the plane-fit tail at :442-516. It fits a plane, projects to 2D,
    // simplifies and projects back, and the points that come back are PLANE points
    // never re-projected onto the mesh (only the facet is re-derived) - and the
    // plane fit degenerates on exactly the curved strokes Draw exists for.
    //
    // The painter builds its seed list NEWEST-FIRST (current, then backwards, then
    // the last click); a chronological stroke has to walk it the other way, which
    // is what the loop below does directly rather than reversing a list.
    const double resolution = 1.0; // px, the value every painter caller passes

    // COPY the anchor before the loop. draw_sample_at() re-sets m_draw_last_mouse on
    // every hit (that is the painter's contract - only a hit moves it), so reading
    // the member inside the loop would walk the origin forward as the seeds land and
    // bunch them all against the end of the gap.
    const Vec2d from = m_draw_last_mouse;

    if (from != Vec2d::Zero()) {
        const double dist = (mouse_position - from).norm();
        if (size_t patches = size_t(dist / resolution); patches > 0) {
            // Cap the seed count: a 2000 px flick at 1 px would be 2000 raycasts in
            // one event, which is a stall of its own. 256 seeds across any gap is
            // finer than the 1 mm resample can use anyway.
            const size_t n = std::min<size_t>(patches, 256);
            const Vec2d  step = (mouse_position - from) / double(n + 1);
            for (size_t k = 1; k <= n; ++ k)
                // A miss is SKIPPED, not terminating: crossing a hole or the
                // silhouette must not end the stroke.
                draw_sample_at(from + double(k) * step);
        }
    }
    draw_sample_at(mouse_position);
}

void GLGizmoCut3D::refresh_draw_stroke()
{
    // NOTE what is NOT refreshed here: m_draw_params.view_dir.
    //
    // This runs on stroke end AND on every parameter change, so re-reading the
    // camera each time would mean that with Direction = View, orbiting and then
    // nudging the Extension slider silently re-aims the cut - the preview would
    // swing round for a reason the user did not ask for. The view direction is
    // LATCHED instead, at the moment the user picks View and at the moment a stroke
    // is finished, which are the two moments they are looking at the angle they
    // mean. latch_draw_view_dir() does that.
    m_draw_params.direction   = DrawCutDirection(m_draw_direction);
    m_draw_params.extension   = double(m_draw_extension);
    m_draw_params.depth       = double(m_draw_depth);
    m_draw_params.thickness   = double(m_cut_thickness);
    m_draw_params.thickness_offset = cut_thickness_offset();
    m_draw_params.angle_deg   = double(m_draw_angle);
    // Through all is hidden (DrawThroughAllHidden): OFF for every cut, a reopened
    // recipe that had it on included.
    if (DrawThroughAllHidden)
        m_draw_params.through_all = false;
    // 2026-09-15, owner item 4. OFF is an EMPTY optional, not a number: "continue the
    // band" has to keep tracking Angle as the user moves it, which a latched number
    // would not. The value is kept in m_draw_ext_angle either way, so unticking and
    // re-ticking gives back what was set.
    if (m_draw_ext_angle_on)
        m_draw_params.extension_angle_deg = double(m_draw_ext_angle);
    else
        m_draw_params.extension_angle_deg.reset();

    // 2026-09-12: THE STROKE COMES FROM THE CHAIN, and only from a CLOSED one.
    //
    // DrawCutChain::finish() leaves the stroke cleared with DrawCutError::NotClosed
    // while the chain is open, which is owner feedback items 1 and 2 in one line:
    // every "is there a cut surface" site in this file asks m_draw_stroke.valid(),
    // so an open chain lofts nothing, previews no cutter shell, classifies nothing
    // and greys out the Cut button - without any of those sites knowing about chains.
    m_draw_chain.finish(m_draw_stroke, DrawCutStroke::DefaultSpacing, double(m_draw_smoothing));

    // PHASE 2 closes phase 1's deviation #7: push the smoothed path back onto the
    // model. draw_cut_smooth() cannot - it lives in libslic3r, where there is no
    // raycaster - but the gizmo has one, so the line and the surface normals it
    // carries really belong to the surface rather than to a chord's sag inside it.
    reproject_draw_stroke_on_mesh();

    // THE HOLONOMY CHECK, which only matters once there IS an angle. A closed stroke
    // whose outward binormal does not close up on itself has no consistent outward
    // side, so a draft would flare one way round one arc of the loop and the other
    // way round the rest. Fall back to 0 and say why - the research spec's
    // "warn and fall back to theta = 0".
    m_draw_frame_flips = m_draw_stroke.valid() && m_draw_params.angle_deg != 0.0 &&
                         draw_cut_frame_holonomy_flips(m_draw_stroke);
    if (m_draw_frame_flips)
        m_draw_params.angle_deg = 0.0;

    // THE FOLD GUARD ASKS THE SURFACE THE STROKE ACTUALLY GETS. 2026-09-13, owner
    // click-test symptom 4: a gentle wavy loop with a 4 mm dent at Depth 3 raised
    // "The line turns tighter than the cut surface reaches sideways, so the surface
    // folds there" - which is a claim about a RULED STRIP, and a closed loop has not
    // been a ruled strip since phase 3. Worse, on a dense hand-drawn stroke the strip
    // test measures the raycast JITTER's curvature rather than the line's, so it fired
    // on any long line whatever its shape. draw_cut_band_folds() asks each kind of
    // stroke the question its own surface can actually fail; see DrawCut.hpp.
    double tight = 0.0;
    m_draw_folds = m_draw_stroke.valid() &&
                   draw_cut_band_folds(m_draw_stroke, m_draw_params, &tight);

    // THE EXTENSION CLAMP, 2026-09-20: per-rail clearance against the instance
    // mesh, so the outward extension stops where it would run into other geometry
    // instead of cutting a slot into it. Computed only for OPEN strokes; a closed
    // loop leaves the vector empty and the band/skirt behaviour unchanged. The
    // advisory flag tells the panel to say the clamp bit.
    m_draw_params.extension_clearance = compute_draw_extension_clearance();
    m_draw_ext_clipped = false;
    for (double c : m_draw_params.extension_clearance)
        if (c < double(m_draw_extension) - 1e-6) { m_draw_ext_clipped = true; break; }

    sync_draw_points();
    update_draw_empty_sides();
    update_draw_connector_warnings();
    invalidate_draw_stroke();
    m_draw_surface_pick_dirty = true;
    // OWNER FEEDBACK 3: the coloured halves follow the DRAWN surface, so the field the
    // shader samples is stale the moment the stroke or a sweep parameter changes.
    //
    // MARKED DIRTY ONLY. The field is a TEXTURE, and this runs from mouse and panel
    // handlers; the upload happens in update_clipper(), which is called at the top of
    // on_render() where the canvas's GL context is current. That is also exactly where
    // apply_curved_color_clip() uploads the sheet's texture, so both surfaces arm their
    // colour clip in the same place.
    m_draw_field_dirty = true;
    m_parent.set_as_dirty();
}

// The snap radius for this object, in mm - scaled by its size, so the gesture feels
// the same on a trinket and on a 200 mm print.
double GLGizmoCut3D::draw_chain_snap_radius() const
{
    return draw_cut_chain_snap_radius(m_bounding_box);
}

// The reach the cut will actually use, in mm. Through-all derives it from the
// object's bounding box exactly the way draw_cut_cutter_solid() does, so the fold
// guard and the connector domain test measure against the number the cutter uses.
double GLGizmoCut3D::draw_cut_depth_reach() const
{
    if (!m_draw_params.through_all)
        return std::max(0.01, double(m_draw_depth));
    const double diag = m_bounding_box.defined ? m_bounding_box.size().norm() : 100.0;
    return 1.05 * diag + 1.0;
}

// Re-project the finished path onto the model - PHASE 2, closing phase 1's
// deviation #7.
//
// Smoothing moves a sample toward the average of its neighbours, which on a convex
// face pulls it INTO the material and on a concave one lifts it off. Phase 1 could
// leave that (a couple of passes is within the chord sag) because nothing read the
// normals for anything but the cut direction at angle 0. Phase 2 reads them for the
// draft angle and for every connector frame, and Edit points lets the user drag a
// point wherever they like - so the path has to be back on the surface first.
//
// MeshRaycaster::get_closest_point() against the cached instance mesh, which is
// already in the PLANE frame - the frame the stroke lives in - so nothing is
// transformed either way.
void GLGizmoCut3D::reproject_draw_stroke_on_mesh()
{
    if (!m_draw_stroke.valid())
        return;
    if (!update_draw_raycaster() || !m_draw_raycaster)
        return;

    // REVIEW GUARD: get_closest_point() is mesh-global - near an edge, a sample that
    // smoothing pulled slightly into the material snaps to the NEIGHBOURING face
    // across the edge, which puts a zigzag into the finished path exactly where the
    // capture filter above keeps one out. Only accept the reprojection when it stays
    // close AND does not flip to a disoriented (in the limit back-facing) surface;
    // otherwise leave the sample where phase 1 put it - it is already on the face.
    const double dist_limit = std::max(1.0, 0.2 * draw_chain_snap_radius());

    std::vector<DrawCutSample> path = m_draw_stroke.path();
    bool moved = false;
    for (DrawCutSample& smp : path) {
        Vec3f normal = Vec3f::Zero();
        const Vec3f closest = m_draw_raycaster->get_closest_point(smp.pos.cast<float>(), &normal);
        const Vec3d to = closest.cast<double>();
        const Vec3d n  = normal.cast<double>();
        if ((to - smp.pos).norm() > dist_limit ||
            (n.squaredNorm() > 1e-12 && n.normalized().dot(smp.normal) < -0.2))
            continue;
        if ((to - smp.pos).squaredNorm() > 1e-12) {
            smp.pos = to;
            moved = true;
        }
        if (n.squaredNorm() > 1e-12 && (n.normalized() - smp.normal).squaredNorm() > 1e-12) {
            smp.normal = n.normalized();
            moved = true;
        }
    }
    if (moved)
        m_draw_stroke.set_path(path);
}

// ---------------------------------------------------------------------------
// PHASE 2: LINE EDITING.
//
// After the stroke ends its resampled points become draggable handles, the way the
// curved sheet's control points are: hover highlights, drag moves, right-click
// deletes, Shift+click on a segment inserts. Three things make this different from
// the sheet's grid and each is the reason for a line of code below:
//
//  1. The sheet's control points are a FIXED grid with a fixed count - dragging one
//     changes a height and nothing else. A stroke's points are a LIST, and insert
//     and delete change its length, so the whole line has to be re-finished after
//     every edit or the resampling and the open/closed decision go stale.
//
//  2. A sheet control point moves along ONE axis (the plane normal). A stroke point
//     moves ON THE SURFACE, so the drag is a RAYCAST, not a plane projection - which
//     is also what keeps the surface normal it carries correct, and the normal is
//     what the draft angle is measured from.
//
//  3. The edited points are the RESAMPLED path, not the raw capture. Feeding them
//     back in as the raw samples and re-running finish() is what keeps the line
//     evenly spaced after an edit - otherwise a dragged point leaves one long span
//     and one short one either side of it, and the ruling density follows.
// ---------------------------------------------------------------------------

void GLGizmoCut3D::sync_draw_points()
{
    // The handles ARE the finished path. Rebuilding them from it on every refresh
    // keeps them in step with smoothing, with the re-projection above, and with an
    // undo - and means there is exactly one place the line lives.
    //
    // NOT rebuilt during a drag: the drag writes m_draw_points directly and commits
    // on release, so re-deriving them mid-gesture would snap the point being
    // dragged back to wherever the last commit left it.
    if (m_draw_drag_pt >= 0)
        return;
    m_draw_points       = m_draw_stroke.path();
}

void GLGizmoCut3D::commit_draw_points()
{
    if (m_draw_points.size() < 2)
        return;

    // The edited points become the RAW samples and finish() runs over them. That is
    // what re-resamples the line (so a dragged point does not leave a long span next
    // to a short one), re-smooths it, and re-decides open vs closed - a point dragged
    // so the two ends now meet really does close the loop, which is the behaviour a
    // user editing a line expects.
    //
    // A CLOSED stroke needs its closing span expressed, because finish() decides
    // closed from the gap between the first and the last sample and the path is
    // stored OPEN (no duplicate first point at the end). Without this an edit would
    // silently open every loop.
    // 2026-09-12: THE EDIT REWRITES THE CHAIN, not the stroke - refresh_draw_stroke()
    // derives the stroke from the chain, so writing the stroke here would be undone on
    // the next line.
    //
    // The edited points become the chain's samples as ONE stroke, which is the honest
    // answer: an edit that inserts or deletes a point changes the sample count, so the
    // original per-stroke ranges no longer describe this line and pretending they do
    // would corrupt undo. Undo still works - the entry pushed before the edit holds the
    // chain as it was, ranges and all - it just makes the edited line one link.
    //
    // Editing is only reachable on a VALID stroke, which means a CLOSED chain, so the
    // closed flag is restored unconditionally. (set_samples() refuses to call a chain
    // of fewer than MinChainSamples closed, which is the guard against an edit that
    // deletes its way down to a triangle.)
    m_draw_chain.set_samples(m_draw_points, /*closed*/ true);
    refresh_draw_stroke();
}

Vec3d GLGizmoCut3D::draw_point_world(int i) const
{
    if (i < 0 || size_t(i) >= m_draw_points.size())
        return Vec3d::Zero();
    return m_plane_center + m_rotation_m * m_draw_points[size_t(i)].pos;
}

int GLGizmoCut3D::draw_point_at(const Vec2d& mouse_position) const
{
    if (m_draw_points.empty())
        return -1;

    const Camera& camera = wxGetApp().plater()->get_camera();
    // The same fixed screen-space pick radius the sheet's control points use, so a
    // handle is grabbable when the line is dense and the view is zoomed out.
    const double pick_px2 = 14.0 * 14.0;

    int    best    = -1;
    double best_d2 = pick_px2;
    for (size_t i = 0; i < m_draw_points.size(); ++ i) {
        const Slic3r::Point p = CameraUtils::project(camera, draw_point_world(int(i)));
        const double d2 = (Vec2d(double(p.x()), double(p.y())) - mouse_position).squaredNorm();
        if (d2 < best_d2) {
            best_d2 = d2;
            best    = int(i);
        }
    }
    return best;
}

// 2026-09-13, OWNER CLICK-TEST ITEM 1: WHICH ENDPOINT A CLICK GRABS.
//
// This replaces a 3D test - `end_for_start(hit_pos, draw_chain_snap_radius())`, i.e.
// "is the point the ray hit on the model within N mm of the endpoint" - whose radius
// is `clamp(0.02 * bbox diagonal, 1, 6)` mm. Two things were wrong with that:
//
//  - it scales with the OBJECT, not with what the user can see. On a 200 mm part the
//    radius is 4 mm, which at a normal viewing zoom is a handful of pixels - so a click
//    that visually lands dead centre on the marker misses, and the press is refused with
//    "that does not continue the line". That is the owner's "clicking on or near them
//    does not continue the chain".
//  - it is measured against a RAYCAST HIT, so an endpoint on the far side of the part
//    can never be picked at all: the ray stops at the near wall. The markers are drawn
//    after a depth clear precisely so a far endpoint stays visible - and then could not
//    be clicked.
//
// A screen-space pixel radius around the PROJECTED endpoint fixes both, and it is the
// same mechanism draw_point_at() uses for the line's edit handles and
// GLGizmoBase/SceneRaycaster use for the connectors: what you can see, you can hit.
DrawChainEnd GLGizmoCut3D::draw_chain_end_at(const Vec2d& mouse_position) const
{
    // The TEST ITSELF lives in libslic3r (draw_cut_chain_end_at_pixel), so it can be
    // pinned by a unit test against a projection built by hand - there is no GUI harness
    // to click a real handle with. All this supplies is the camera.
    const Camera&     camera         = wxGetApp().plater()->get_camera();
    const Transform3d plane_to_world = translation_transform(m_plane_center) * m_rotation_m;

    return draw_cut_chain_end_at_pixel(
        m_draw_chain, mouse_position,
        [&](const Vec3d& p_plane) -> std::optional<Vec2d> {
            const Slic3r::Point p = CameraUtils::project(camera, plane_to_world * p_plane);
            return Vec2d(double(p.x()), double(p.y()));
        },
        DrawEndPickPx);
}

// ---------------------------------------------------------------------------
// 2026-09-13, OWNER CLICK-TEST ITEM 2: "CLOSE LOOP MIRRORS THE LINE TO THE OTHER SIDE
// OF THE OBJECT INSTEAD OF CLOSING THE CHAIN".
//
// Nothing ever reflected anything. What Close loop did was call
// DrawCutChain::force_close(), which sets m_closed and adds NO samples - so the span
// that closes the loop is the straight CHORD from the chain's last sample to its first,
// in the cut plane's frame. DrawCutStroke::finish() resamples along that chord like any
// other span and draw_cut_cutter_solid() sweeps the ruling along it.
//
// On a line drawn round the OUTSIDE of a part - which is the case Close loop exists for,
// a line whose two ends cannot comfortably be brought together - that chord goes
// straight THROUGH the material and comes out the far side, and the ruled surface swept
// along it is a sheet standing on the far face. From outside, a chord through a solid
// and a mirrored copy of the line are indistinguishable, which is what the report
// describes.
//
// THE FIX: close along a path that lies ON the surface. The straight segment between the
// two endpoints is sampled, each sample is raycast onto the mesh FROM THE CURRENT VIEW,
// and the hits become real samples appended before the closure. That is a geodesic-ish
// path - not the true geodesic, but a path on the surface, which is the property that
// matters: the closing span sweeps material that is there.
//
// THREE OUTCOMES, and no fourth:
//   - the ends are already within the snap radius: join them, with no intermediate
//     points at all (close_along_path({}) is exactly force_close(), and correctly so -
//     there is nothing between them to walk);
//   - a path is found: append it and close;
//   - no path (the segment leaves the model from this view - the endpoints are on
//     opposite sides, or round a corner the camera cannot see): REFUSE, leave the chain
//     exactly as it was, and tell the user to draw the rest. Never join through the air,
//     which is the behaviour being replaced.
// ---------------------------------------------------------------------------
bool GLGizmoCut3D::close_draw_chain_on_surface()
{
    if (m_draw_chain.is_closed() || m_draw_chain.size() < DrawCutChain::MinChainSamples)
        return false;

    const Vec3d  a = m_draw_chain.back_pos();   // the path runs BACK -> FRONT, which is
    const Vec3d  b = m_draw_chain.front_pos();  // the order close_along_path() wants
    const double gap = (b - a).norm();

    // ALREADY TOUCHING. Within the snap radius the two ends are the same place as far as
    // the cut is concerned, and the closing span is under the 1 mm resample spacing -
    // exactly the case the drag-time snap handles. Nothing to walk, so just join them.
    if (gap <= draw_chain_snap_radius())
        return m_draw_chain.close_along_path({});

    if (!update_draw_raycaster() || !m_draw_raycaster)
        return false;

    const Camera&     camera         = wxGetApp().plater()->get_camera();
    const Transform3d plane_to_world = translation_transform(m_plane_center) * m_rotation_m;

    // SAMPLE IN SCREEN SPACE, not in 3D. The raycast is done from the camera through a
    // pixel, so stepping along the projected segment is what puts the samples an even
    // distance apart ON SCREEN - which is what keeps the hit density even on a surface
    // that is oblique to the view. Stepping in 3D and projecting would bunch the samples
    // wherever the segment runs away from the camera.
    const Slic3r::Point pa = CameraUtils::project(camera, plane_to_world * a);
    const Slic3r::Point pb = CameraUtils::project(camera, plane_to_world * b);
    const Vec2d sa(double(pa.x()), double(pa.y()));
    const Vec2d sb(double(pb.x()), double(pb.y()));

    // One sample every 2 px, capped: the path is resampled to 1 mm by finish() anyway, so
    // more than a few hundred raycasts buys nothing, and a cap keeps a long diagonal on a
    // 4K display from turning a button press into a visible stall.
    const double px = (sb - sa).norm();
    const size_t n  = std::clamp<size_t>(size_t(px / 2.0), 8, 400);

    std::vector<DrawCutSample> path;
    path.reserve(n);
    size_t misses = 0;
    for (size_t k = 1; k < n; ++ k) {
        const Vec2d s = sa + (double(k) / double(n)) * (sb - sa);

        Vec3f  hit_f, normal_f;
        size_t facet = 0;
        if (!m_draw_raycaster->unproject_on_mesh(s, plane_to_world, camera, hit_f, normal_f, nullptr, &facet)) {
            ++ misses;
            continue;
        }
        DrawCutSample smp;
        smp.pos    = hit_f.cast<double>();
        smp.normal = normal_f.cast<double>().normalized();
        smp.facet  = facet;
        // Drop a hit that repeats its predecessor: consecutive pixels on a face nearly
        // edge-on to the view land on the same point, and a zero-length span is what the
        // tangent maths cannot read.
        if (!path.empty() && (smp.pos - path.back().pos).squaredNorm() < 1e-12)
            continue;
        path.push_back(smp);
    }

    // REFUSE rather than bridge. A quarter of the segment missing means the straight line
    // between the ends leaves the model from here - they are round a corner, or on
    // opposite sides - and the "path" that survived would be two arcs with a jump between
    // them, which finish() would bridge with a chord: the very thing being fixed. The
    // panel then tells the user to draw the remaining part, which always works and is
    // honest about what the button cannot do.
    if (path.size() < 2 || misses * 4 > n)
        return false;

    // THE JOIN GAPS. The path's own ends have to meet the chain's endpoints, or the
    // closure is two jumps instead of one. Each is a straight span in the plane frame -
    // the same thing force_close() made the WHOLE closure, which is only acceptable
    // because these are short: a couple of screen pixels' worth of surface, since the
    // sampling started one step in from each endpoint.
    if ((path.front().pos - a).norm() > gap || (path.back().pos - b).norm() > gap)
        // The hits are further from the endpoints than the endpoints are from each
        // other, so the ray is finding a different part of the model (a near wall in
        // front of the line). Refuse - this is the far-side case again, seen from a view
        // where the segment happens to hit something.
        return false;

    return m_draw_chain.close_along_path(path);
}

int GLGizmoCut3D::draw_segment_at(const Vec2d& mouse_position) const
{
    const size_t n = m_draw_points.size();
    if (n < 2)
        return -1;

    const Camera& camera = wxGetApp().plater()->get_camera();
    const double pick_px2 = 12.0 * 12.0;

    // A closed stroke has one more segment than it has points: the closing span.
    const size_t segs = m_draw_stroke.is_closed() ? n : n - 1;

    int    best    = -1;
    double best_d2 = pick_px2;
    for (size_t i = 0; i < segs; ++ i) {
        const Slic3r::Point pa = CameraUtils::project(camera, draw_point_world(int(i)));
        const Slic3r::Point pb = CameraUtils::project(camera, draw_point_world(int((i + 1) % n)));
        const Vec2d a(double(pa.x()), double(pa.y()));
        const Vec2d b(double(pb.x()), double(pb.y()));
        const Vec2d ab = b - a;
        const double len2 = ab.squaredNorm();
        if (len2 < 1e-9)
            continue;
        // The nearest point on the SEGMENT, clamped - not on the infinite line, or a
        // click well past the end of a short segment would claim it.
        const double t = std::clamp((mouse_position - a).dot(ab) / len2, 0.0, 1.0);
        const double d2 = (a + t * ab - mouse_position).squaredNorm();
        if (d2 < best_d2) {
            best_d2 = d2;
            best    = int(i);
        }
    }
    return best;
}

bool GLGizmoCut3D::mouse_near_draw_line(const Vec2d& mouse_position) const
{
    // The CHAIN's samples rather than the finished stroke's path: the chain is the
    // line the user sees from the first stroke on, closed or not, and it is what the
    // plane drag moves.
    const std::vector<DrawCutSample>& pts = m_draw_chain.samples();
    if (pts.empty())
        return false;

    const Camera&     camera         = wxGetApp().plater()->get_camera();
    const Transform3d plane_to_world = translation_transform(m_plane_center) * m_rotation_m;
    std::vector<Vec2d> px;
    px.reserve(pts.size());
    for (const DrawCutSample& smp : pts) {
        const Slic3r::Point p = CameraUtils::project(camera, plane_to_world * smp.pos);
        px.emplace_back(double(p.x()), double(p.y()));
    }
    return draw_cut_distance_to_polyline(px, m_draw_chain.is_closed(), mouse_position) <= DrawLineGrabPx;
}

bool GLGizmoCut3D::draw_point_reproject(int i, const Vec2d& mouse_position)
{
    if (i < 0 || size_t(i) >= m_draw_points.size())
        return false;
    if (!update_draw_raycaster() || !m_draw_raycaster)
        return false;

    const Camera& camera = wxGetApp().plater()->get_camera();
    // The pick mesh is in the PLANE frame, so this is the transform that takes it to
    // the world - the same one draw_sample_at() uses, and the hit comes back in the
    // plane frame, which is where the point lives.
    const Transform3d plane_to_world = translation_transform(m_plane_center) * m_rotation_m;

    Vec3f  hit, normal;
    size_t facet = 0;
    // A MISS DOES NOT MOVE THE POINT. Dragging off the silhouette must leave the
    // line where it was rather than flinging the point to wherever the ray happened
    // to end - which is the same "a miss is skipped, not fatal" rule capture follows.
    if (!m_draw_raycaster->unproject_on_mesh(mouse_position, plane_to_world, camera, hit, normal, nullptr, &facet))
        return false;

    DrawCutSample& s = m_draw_points[size_t(i)];
    s.pos    = hit.cast<double>();
    s.normal = normal.cast<double>().normalized();
    s.facet  = facet;
    return true;
}

// ---------------------------------------------------------------------------
// PHASE 2: CONNECTORS ON THE DRAWN SURFACE.
//
// The curved cut's phase 4 already generalised the connector frame from "one
// shared m_rotation_m" to "a rotation built from the local surface normal", and it
// did so IN THE CONNECTOR PATH rather than in the sheet - which is why nothing in
// process_connector_cut() has to know about either surface. All Draw needs is the
// same three answers against the ruled strip instead of the height field:
//
//   where is the surface under this click  -> unproject_on_draw_surface()
//   what frame does it have there          -> draw_cut_surface_frame(), via
//                                             connector_rotation_m()
//   is the patch there any good            -> update_draw_connector_warnings()
//
// WHY THEY SURVIVE THE SPLIT AND THE KERF, which is the thing worth stating
// because it is not obvious: a connector's body is subtracted from one half and
// added to the other by process_connector_cut(), which works off the connector's
// own pos and rotation_m and knows nothing about the cut surface. Both halves are
// produced by ONE boolean against ONE cutter, so the faces they present to each
// other are the same surface - and a connector standing perpendicular to that
// surface therefore meets both of them squarely. The kerf moves both faces along
// the SAME strip normal (draw_cut_cutter_solid offsets along -binormal / -sweep,
// one field for the whole strip), so the hole and the plug stay coaxial: the gap
// opens along the connector's own axis, which is the direction it is designed to
// come apart in anyway.
//
// WHICH KINDS. Plug / Dowel / Snap are solids of revolution about the local normal
// and work unchanged. The FLEXI kinds are gated by the existing per-kind footprint
// check, and on a swept surface that check is stricter than it is on a sheet for a
// real reason: a ruled strip is DEVELOPABLE along its rules (a straight ruling has
// zero curvature that way) but can bend arbitrarily hard ACROSS them, which is
// exactly the direction a hinge's knuckle run or a thread's pitch line lies along
// when the connector is placed on a curving stroke. So Hinge and Thread get the
// same curved_cut_patch_is_flat_enough()-shaped warning, computed from the
// cross-rule curvature only (draw_cut_surface_curvature_radius does that), and a
// stroke drawn straight enough passes it. Nothing is refused that the curved cut
// would allow.
// ---------------------------------------------------------------------------

bool GLGizmoCut3D::draw_connectors_live() const
{
    return is_draw_surface() && m_draw_stroke.valid();
}

void GLGizmoCut3D::update_draw_surface_raycaster()
{
    if (!m_draw_surface_pick_dirty && m_draw_surface_raycaster)
        return;

    m_draw_surface_mesh.clear();
    m_draw_surface_raycaster.reset();
    m_draw_surface_pick_dirty = false;

    if (!m_draw_stroke.valid())
        return;

    // THE CUTTER SOLID IS THE PICK TARGET, so a connector can be placed anywhere on
    // the surface the cut will use, including on the closure geometry (a plug's lid,
    // a wrap's flange) that render_draw_stroke()'s preview no longer shows - the
    // preview and the pick target intentionally part ways here, the preview trimmed
    // to what the user drew (draw_cut_preview_surface(), see its declaration) and
    // this kept at the cut's own full reach (not the preview's clamped one) so
    // nothing the boolean can use is off limits to a connector.
    BoundingBoxf3 bbox;
    indexed_triangle_set mesh;
    const indexed_triangle_set* src = nullptr;
    if (!m_draw_pick_its.empty())
        src = &m_draw_pick_its;
    else if (curved_instance_mesh_in_plane(mesh))
        src = &mesh;
    if (src != nullptr)
        for (const Vec3f& v : src->vertices)
            bbox.merge(v.cast<double>());

    // `src` is the object's mesh IN THE PLANE FRAME, which is the frame the cutter is
    // built in - so it is exactly what draw_cut_loop_separates() needs to tell a loop
    // drawn ROUND the part from one drawn ON it. Passing it keeps the preview's cutter
    // identical to the one the cut will use.
    indexed_triangle_set cutter = draw_cut_cutter_solid(m_draw_stroke, m_draw_params, bbox, 0.0, src);
    if (cutter.empty())
        return;
    // To the WORLD, the way update_curved_sheet_raycaster() takes the sheet there,
    // so the raycaster's own transform is the identity at use.
    its_transform(cutter, translation_transform(m_plane_center) * m_rotation_m);
    m_draw_surface_mesh = TriangleMesh(std::move(cutter));
    if (m_draw_surface_mesh.empty())
        return;
    m_draw_surface_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(m_draw_surface_mesh));
}

bool GLGizmoCut3D::unproject_on_draw_surface(const Vec2d& mouse_position, Vec3d& pos, Vec3d& pos_world)
{
    if (!draw_connectors_live())
        return false;

    update_draw_surface_raycaster();
    if (!m_draw_surface_raycaster)
        return false;

    const Camera& camera = wxGetApp().plater()->get_camera();
    Vec3f hit_f, normal_f;
    if (!m_draw_surface_raycaster->unproject_on_mesh(mouse_position, Transform3d::Identity(), camera, hit_f, normal_f))
        return false;

    const Vec3d hit = hit_f.cast<double>();

    // NO CONTOUR TEST HERE, and that is deliberate. The curved sheet's version
    // projects the hit back down onto the plane and asks the object clipper whether
    // it is inside the cut's cross-section, because the sheet spans the whole plane
    // and most of it is outside the part. The drawn cutter is not like that: it IS
    // the cut surface and nothing else, so every point of it is on the cut. What
    // replaces the test is the (s, w) domain check in update_draw_connector_warnings(),
    // which asks the question that actually matters here - is the connector clear of
    // the strip's own rims.
    const CommonGizmosDataObjects::SelectionInfo* sel = m_c->selection_info();
    Vec3d hit_d = hit;
    if (sel && sel->model_object() && sel->get_active_instance() >= 0) {
        hit_d -= sel->model_object()->instances[sel->get_active_instance()]->get_offset();
        hit_d.z() -= double(sel->get_sla_shift());
    }

    pos       = hit_d;
    pos_world = hit;
    return true;
}

bool GLGizmoCut3D::draw_connector_sw(const Vec3d& pos_object, double& s, double& w) const
{
    s = w = 0.0;
    if (!m_draw_stroke.valid())
        return false;

    // The connector's position is in the OBJECT's frame; the stroke is in the cut
    // PLANE's. connector_plane_xy() does the same hop for the sheet, but it throws
    // away z - which a ruled surface cannot afford, because the surface is not a
    // height field over the plane. So the full local point is carried through.
    const CommonGizmosDataObjects::SelectionInfo* sel = m_c->selection_info();
    Vec3d world = pos_object;
    if (sel && sel->model_object() && sel->get_active_instance() >= 0) {
        world += sel->model_object()->instances[sel->get_active_instance()]->get_offset();
        world.z() += double(sel->get_sla_shift());
    }
    const Vec3d local = m_rotation_m.inverse() * (world - m_plane_center);
    return draw_cut_surface_project(m_draw_stroke, m_draw_params, local, s, w);
}

void GLGizmoCut3D::update_draw_connector_warnings()
{
    m_draw_tilted_connectors = m_draw_unflat_connectors = m_draw_offsurface_connectors = 0;
    if (!draw_connectors_live())
        return;
    const CommonGizmosDataObjects::SelectionInfo* sel = m_c->selection_info();
    const ModelObject* mo = sel ? sel->model_object() : nullptr;
    if (mo == nullptr)
        return;

    const double reach = draw_cut_depth_reach();

    for (const CutConnector& connector : mo->cut_connectors) {
        double s = 0.0, w = 0.0;
        if (!draw_connector_sw(connector.pos, s, w))
            continue;

        const double extent = connector_extent(connector);

        // OFF THE SURFACE: the (s, w) domain test, the Draw analogue of the curved
        // cut's (u,v)-in-contour check. A connector whose body hangs off the strip's
        // rim is only half made by the split, which is a silent wrong result rather
        // than a visible one - so it is worth counting and saying.
        if (!draw_cut_surface_contains(m_draw_stroke, m_draw_params, s, w, extent, reach))
            ++ m_draw_offsurface_connectors;

        if (draw_cut_surface_tilt_deg(m_draw_stroke, m_draw_params, s, w) > CurvedConnectorTiltWarnDeg)
            ++ m_draw_tilted_connectors;

        // Only the STRAIGHT-featured flexi kinds care, exactly as on the sheet: a
        // plug, a dowel or a snap is a solid of revolution about the local normal and
        // sits fine on a curving patch. A hinge's knuckle run and a thread's pitch
        // line are generated as if for a plane, and the direction they lie along is
        // the ACROSS-rules one - the only direction a ruled strip can curve in.
        if (connector.attribs.type == CutConnectorType::FlexiJoint &&
            (connector.flexi.kind == FlexiJointKind::Hinge || connector.flexi.kind == FlexiJointKind::Thread) &&
            !draw_cut_patch_is_flat_enough(m_draw_stroke, m_draw_params, s, w, extent))
            ++ m_draw_unflat_connectors;
    }
}

void GLGizmoCut3D::render_draw_point_handles()
{
    if (!m_draw_editing || m_draw_points.empty())
        return;

    // The handles have to be visible THROUGH the part - a closed loop's far side is
    // behind the model, and a handle you cannot see is a handle you cannot grab.
    // Same depth clear render_curved_control_points() uses for the same reason.
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));

    const Camera&     camera      = wxGetApp().plater()->get_camera();
    const Transform3d view_matrix = camera.get_view_matrix();
    const double      mean_size   = get_grabber_mean_size(m_bounding_box);
    const double      size        = 0.32 * get_half_size(mean_size);

    for (size_t i = 0; i < m_draw_points.size(); ++ i) {
        const bool hovered = (int(i) == m_draw_hover_pt) || (int(i) == m_draw_drag_pt);
        const ColorRGBA color = hovered ? ColorRGBA::ORANGE() : GRABBER_COLOR;
        render_model(m_sphere.model, color,
                     view_matrix * translation_transform(draw_point_world(int(i))) *
                         scale_transform(size));
    }
}

void GLGizmoCut3D::update_draw_empty_sides()
{
    m_draw_upper_empty = m_draw_lower_empty = false;
    if (!is_draw_surface() || !m_draw_stroke.valid())
        return;

    // The same mesh the stroke was drawn against, which is already cached for the
    // gesture; derive it here when there was no gesture (a parameter change).
    indexed_triangle_set mesh;
    if (!m_draw_pick_its.empty())
        mesh = m_draw_pick_its;
    else if (!curved_instance_mesh_in_plane(mesh))
        return;

    draw_cut_empty_sides(mesh, m_draw_stroke, m_draw_params, m_draw_upper_empty, m_draw_lower_empty);
}

void GLGizmoCut3D::clear_draw_stroke(bool push_undo)
{
    // The CHAIN is what "is there a line" means now: a chain that is open has a line
    // the user can see and continue, while m_draw_stroke is deliberately empty, so
    // gating on the stroke would make Clear line a no-op on exactly the state the user
    // most wants to clear.
    if (m_draw_chain.empty() && m_draw_capture.empty())
        return;
    if (push_undo)
        push_draw_undo();
    m_draw_chain.clear();
    m_draw_capture.clear();
    m_draw_snap_armed = false;
    // 2026-09-13, item 1: the hover latch says which HANDLE the cursor is over, and a
    // cleared line has no handles. Left set, the next frame would light a marker that is
    // no longer drawn - and, worse, the next chain's handle would come up pre-lit.
    m_draw_hover_end  = DrawChainEnd::None;
    // The refusal message describes ONE GESTURE, not a state of the line, so clearing
    // the line clears it too - otherwise "the line is already closed" would still be on
    // screen next to an empty canvas.
    m_draw_reject_msg = DrawRejectReason::None;
    m_draw_stroke.clear();
    m_draw_capturing  = false;
    m_draw_last_mouse = Vec2d::Zero();
    m_draw_upper_empty = m_draw_lower_empty = false;
    m_draw_folds       = false;
    m_draw_ext_clipped = false;
    // PHASE 2: the handles belong to a line that no longer exists, and leaving
    // Edit points on with nothing to edit is a mode the user cannot get out of by
    // doing the obvious thing (drawing a new line, which editing mode refuses).
    m_draw_editing     = false;
    m_draw_hover_pt = m_draw_drag_pt = -1;
    m_draw_points.clear();
    m_draw_frame_flips = false;
    m_draw_tilted_connectors = m_draw_unflat_connectors = m_draw_offsurface_connectors = 0;
    m_draw_surface_pick_dirty = true;
    m_draw_surface_raycaster.reset();
    // Dirty only, as in refresh_draw_stroke(): the upload and the release both happen
    // from update_clipper(), inside the render.
    m_draw_field_dirty = true;
    invalidate_draw_stroke();
    m_parent.set_as_dirty();
}

// --- gizmo-local undo, the stroke's half of it -----------------------------
//
// Same contract as the sheet's: one entry per COMPLETED stroke, pushed BEFORE the
// change, consumed by the same on_cut_char() hook so Ctrl+Z reaches the plater
// only when this stack has nothing to give. The entry stores the RAW samples, not
// the finished path, so restoring it and re-running finish() with the current
// panel settings gives a stroke that is consistent with them - restoring a
// finished path would resurrect the smoothing the user has since changed.

GLGizmoCut3D::DrawStrokeState GLGizmoCut3D::draw_stroke_state() const
{
    DrawStrokeState st;
    st.chain = m_draw_chain;
    return st;
}

void GLGizmoCut3D::apply_draw_stroke_state(const DrawStrokeState& st)
{
    m_draw_chain = st.chain;
    // Abandon any gesture in flight: restoring a chain under a half-drawn stroke would
    // append that stroke to a chain whose endpoints have moved out from under it.
    m_draw_capture.clear();
    m_draw_capturing  = false;
    m_draw_snap_armed = false;
    // An undo answers the refusal the message was complaining about (it is usually the
    // very reason the user pressed Ctrl+Z), so the message goes with it.
    m_draw_reject_msg = DrawRejectReason::None;
    refresh_draw_stroke();
}

void GLGizmoCut3D::push_draw_undo()
{
    if (m_surface_mode != CutSurfaceMode::Draw)
        return;
    m_draw_undo.emplace_back(draw_stroke_state());
    if (m_draw_undo.size() > CurvedUndoLimit)
        m_draw_undo.erase(m_draw_undo.begin());
    // A new edit ends the redo branch, the way every undo stack does.
    m_draw_redo.clear();
}

// 2026-09-12: ONE STEP PER APPENDED STROKE. The entry holds the whole chain as it
// stood before that stroke - including whether it was closed - so Ctrl+Z after a
// continuation gives back the chain minus that link, and Ctrl+Z after the stroke that
// closed the loop gives back the open chain with the surface preview gone. That is the
// behaviour the brief asks to keep ("Ctrl+Z per stroke segment inside the gizmo"), now
// meaning a segment of the chain rather than a whole separate line.
bool GLGizmoCut3D::draw_undo()
{
    if (m_surface_mode != CutSurfaceMode::Draw || m_draw_undo.empty())
        return false;
    m_draw_redo.emplace_back(draw_stroke_state());
    const DrawStrokeState st = m_draw_undo.back();
    m_draw_undo.pop_back();
    apply_draw_stroke_state(st);
    return true;
}

bool GLGizmoCut3D::draw_redo()
{
    if (m_surface_mode != CutSurfaceMode::Draw || m_draw_redo.empty())
        return false;
    m_draw_undo.emplace_back(draw_stroke_state());
    const DrawStrokeState st = m_draw_redo.back();
    m_draw_redo.pop_back();
    apply_draw_stroke_state(st);
    return true;
}

// --- mouse ----------------------------------------------------------------

bool GLGizmoCut3D::draw_on_mouse(const wxMouseEvent& mouse_event)
{
    if (!is_draw_surface() || m_connectors_editing || m_hide_cut_plane)
        return false;

    const Vec2d mouse_pos(mouse_event.GetX(), mouse_event.GetY());

    // ---------------------------------------------------------------------
    // PHASE 2: EDIT POINTS. Claimed BEFORE the capture branch, because in editing
    // mode a left-drag on the model must move a handle rather than start a new
    // stroke - the two gestures are the same gesture and only the mode tells them
    // apart. Everything else (Esc, Ctrl+Z, the panel) is unchanged.
    // ---------------------------------------------------------------------
    if (m_draw_editing && m_draw_stroke.valid() && !m_draw_capturing) {
        if (m_draw_drag_pt >= 0) {
            if (mouse_event.Dragging()) {
                if (draw_point_reproject(m_draw_drag_pt, mouse_pos)) {
                    // The CUTTER PREVIEW updates live, which is the point of the
                    // exercise: the shell is rebuilt from the edited points on every
                    // motion event rather than on release. It is one
                    // draw_cut_cutter_solid() call over a few hundred samples, which
                    // is the same cost a slider drag already pays.
                    //
                    // The stroke itself is NOT re-finished here - that would resample
                    // mid-drag and walk the point out from under the cursor. The
                    // preview reads the points directly (see update_draw_preview_models).
                    invalidate_draw_stroke();
                    m_parent.set_as_dirty();
                }
                return true;
            }
            if (mouse_event.LeftUp() || mouse_event.Leaving()) {
                m_draw_drag_pt = -1;
                // ONE UNDO ENTRY PER EDIT, and it was pushed on the press - the same
                // "push before the change" rule the stroke and the sheet both follow.
                commit_draw_points();
                m_parent.set_as_dirty();
                return true;
            }
            return true;
        }

        if (mouse_event.Moving()) {
            const int hover = draw_point_at(mouse_pos);
            if (hover != m_draw_hover_pt) {
                m_draw_hover_pt = hover;
                m_parent.set_as_dirty();
            }
            return hover >= 0;
        }

        // RIGHT-CLICK DELETES a point. Guarded on keeping enough points to still be a
        // line: MinSamples is the floor finish() enforces, and deleting below it
        // would turn a good line into an error message with no way back but undo.
        if (mouse_event.RightDown()) {
            const int pt = draw_point_at(mouse_pos);
            if (pt >= 0 && m_draw_points.size() > size_t(DrawCutStroke::MinSamples)) {
                Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Delete cut line point"), UndoRedo::SnapshotType::GizmoAction);
                push_draw_undo();
                m_draw_points.erase(m_draw_points.begin() + pt);
                m_draw_hover_pt = -1;
                commit_draw_points();
                m_parent.set_as_dirty();
                return true;
            }
            // A right-click that hit no point falls through to the plane-flip gesture
            // the gizmo already has, rather than being swallowed here.
            return false;
        }

        if (mouse_event.LeftDown() && !mouse_event.CmdDown() && !mouse_event.AltDown()) {
            // SHIFT+CLICK ON A SEGMENT INSERTS a point, at the click, re-projected
            // onto the model - so the new point is on the surface with the surface's
            // own normal, not on the chord between its neighbours.
            if (mouse_event.ShiftDown()) {
                const int seg = draw_segment_at(mouse_pos);
                if (seg >= 0) {
                    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Insert cut line point"), UndoRedo::SnapshotType::GizmoAction);
                    push_draw_undo();
                    // Seed it on the chord, then let the raycast put it on the surface.
                    const size_t a = size_t(seg);
                    const size_t b = (a + 1) % m_draw_points.size();
                    DrawCutSample mid;
                    mid.pos    = 0.5 * (m_draw_points[a].pos + m_draw_points[b].pos);
                    mid.normal = (m_draw_points[a].normal + m_draw_points[b].normal).normalized();
                    mid.facet  = m_draw_points[a].facet;
                    m_draw_points.insert(m_draw_points.begin() + int(b == 0 ? m_draw_points.size() : b), mid);
                    const int inserted = int(b == 0 ? m_draw_points.size() - 1 : b);
                    draw_point_reproject(inserted, mouse_pos);
                    m_draw_hover_pt = inserted;
                    commit_draw_points();
                    m_parent.set_as_dirty();
                    return true;
                }
                return false;
            }

            const int pt = draw_point_at(mouse_pos);
            if (pt >= 0) {
                Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Move cut line point"), UndoRedo::SnapshotType::GizmoAction);
                push_draw_undo();
                m_draw_drag_pt  = pt;
                m_draw_hover_pt = pt;
                return true;
            }
            // A click on nothing in editing mode does NOT start a new stroke: that
            // would throw away the line the user is editing on a stray click. Leaving
            // editing mode is the deliberate way back to drawing.
            return false;
        }

        return false;
    }

    // 2026-09-13, ITEM 1: HOVER over an endpoint marker. Tracked on every motion event
    // while no stroke is in flight, so the marker can brighten and the user can see it
    // is grab-able rather than having to discover it with a click. Cheap - two
    // projections - and it does NOT consume the event: hovering an endpoint must not
    // stop the rest of the gizmo (or the camera) from seeing the mouse move.
    if (!m_draw_capturing && (mouse_event.Moving() || mouse_event.Dragging())) {
        const DrawChainEnd hov = m_draw_editing ? DrawChainEnd::None : draw_chain_end_at(mouse_pos);
        if (hov != m_draw_hover_end) {
            m_draw_hover_end = hov;
            m_parent.set_as_dirty();
        }
    }

    if (m_draw_capturing) {
        if (mouse_event.Dragging()) {
            draw_interpolate_to(mouse_pos);
            // SNAP FEEDBACK. Light the far endpoint's ring up while the cursor is
            // inside the radius that would close the chain on release, so the user can
            // see the closure coming rather than discovering it afterwards. Measured
            // against the last captured sample (which is on the model) rather than
            // against the mouse ray, so it agrees exactly with the test append() will
            // make on release.
            m_draw_snap_armed = false;
            if (!m_draw_capture.empty() && !m_draw_chain.empty()) {
                const Vec3d far_end = m_draw_capture_end == DrawChainEnd::Back
                                    ? m_draw_chain.front_pos() : m_draw_chain.back_pos();
                const size_t total = m_draw_chain.size() + m_draw_capture.size();
                m_draw_snap_armed = total >= DrawCutChain::MinChainSamples &&
                                    (m_draw_capture.back().pos - far_end).norm() <= draw_chain_snap_radius();
            }
            else if (m_draw_capture.size() >= DrawCutChain::MinChainSamples) {
                // A FIRST stroke closes on its own start - the phase 1 "a circle in one
                // gesture" case, which still works exactly as it did.
                m_draw_snap_armed = (m_draw_capture.back().pos - m_draw_capture.front().pos).norm()
                                    <= draw_chain_snap_radius();
            }
            // The RIBBON is rebuilt every tick - it is a light strip of triangles
            // and the raycaster's AABB tree stays valid all stroke (Draw never
            // moves a vertex, so none of Sculpt's stale-tree caveats apply). The
            // CUTTER SHELL waits for LeftUp, which is where the stroke is resampled
            // and so where the surface it describes actually exists.
            invalidate_draw_stroke();
            m_parent.set_as_dirty();
            return true;
        }
        if (mouse_event.LeftUp() || mouse_event.Leaving()) {
            m_draw_capturing  = false;
            m_draw_last_mouse = Vec2d::Zero();
            m_draw_snap_armed = false;
            // The camera angle AT THE MOMENT THE STROKE WAS DRAWN is the one a
            // "View" cut means, so latch it here rather than re-reading it on every
            // later parameter change (see refresh_draw_stroke).
            latch_draw_view_dir();

            // APPEND THE CAPTURED STROKE TO THE CHAIN. This is the whole of the 2026-09-12
            // change on the gesture side: the stroke is not the line, it is one link of it,
            // and append() decides which end it joins and whether it closes the chain.
            //
            // A stroke of one sample (a click, or a drag that only ever hit once) is
            // dropped rather than appended: append() needs two samples to have a
            // direction, and a one-sample link would be invisible and unremovable.
            // CAPTURE CONTINUITY FILTER (draw-cut review item 2): near an edge or a
            // neighbouring wall the mouse ray can skim onto an adjacent surface, and a
            // handful of those hits makes the line zigzag off the face and back. Drop
            // short confirmed skims BEFORE the append sees the stroke; the first and
            // last sample are never touched, so the join span and the closing span are
            // judged by append_at() exactly as before.
            const std::vector<DrawCutSample> captured =
                draw_cut_filter_capture_skims(m_draw_capture);
            m_draw_capture.clear();
            // 2026-09-13, ITEM 1: appended AT THE END THE PRESS PICKED. append()'s own
            // `snap_radius` decides two things - which end the stroke joins, and whether
            // it CLOSES the chain - and only the second is wanted here: the first was
            // already decided in screen space on the press (draw_chain_end_at()), and
            // letting append() re-decide it from a 3D radius would refuse exactly the
            // strokes the screen pick exists to rescue. append_at() takes the end as
            // given and keeps the closure test.
            const DrawChainEnd at = captured.size() >= 2
                                  ? m_draw_chain.append_at(captured, m_draw_capture_end,
                                                           draw_chain_snap_radius())
                                  : DrawChainEnd::None;
            refresh_draw_stroke();

            // NOTHING WAS APPENDED: no step belongs on the undo stack. Otherwise the
            // first Ctrl+Z would appear to do nothing, taking back a link the user
            // never got. (The press pushed the entry speculatively, because it has to
            // hold the state to come BACK to and by LeftUp that state is gone.)
            if (at == DrawChainEnd::None && !m_draw_undo.empty())
                m_draw_undo.pop_back();
            m_parent.set_as_dirty();
            return true;
        }
        return true;
    }

    if (mouse_event.LeftDown() && !mouse_event.ShiftDown() && !mouse_event.CmdDown() && !mouse_event.AltDown()) {
        if (!update_draw_raycaster())
            return false;

        // 2026-09-13, OWNER CLICK-TEST ITEM 1. WHICH END THIS STROKE CONTINUES IS
        // DECIDED IN SCREEN SPACE, BEFORE the mesh is touched.
        //
        // It used to be decided from the raycast HIT: sample the mesh, then ask
        // end_for_start() whether that 3D point is within the snap radius (a few mm,
        // scaled off the bounding box) of an endpoint. Two failures followed, and both
        // are what the owner hit - "a new stroke started on an endpoint is not
        // recognised, so the chain can never be extended or closed":
        //
        //  - the radius is in OBJECT mm, so how close you have to click depends on how
        //    far the camera is and how big the part is. A click visually centred on the
        //    marker misses on anything but a small part seen close up.
        //  - a hit is needed at all, so an endpoint drawn OVER the part (the markers are
        //    rendered after a depth clear, so a far-side endpoint is visible by design)
        //    could not be clicked: the ray stops at the near wall and the hit is metres
        //    away from the endpoint in 3D.
        //
        // draw_chain_end_at() answers it from the projected marker position and a pixel
        // radius, so the region you can click is the region you can see.
        const DrawChainEnd picked = draw_chain_end_at(mouse_pos);

        // The painter's guard: a click that MISSES the mesh must not capture the mouse,
        // or the rest of the gizmo (and the plater's own rectangle select) stops working
        // wherever the model is not. Probe FIRST, before anything is touched.
        m_draw_capture.clear();
        m_draw_last_mouse = Vec2d::Zero();
        if (!draw_sample_at(mouse_pos))
            return false;

        // OWNER FEEDBACK 4: ONLY ONE CHAIN MAY EXIST, and a stroke that does not begin
        // at one of the current chain's endpoints is REJECTED. The check happens here,
        // on the press, so the refusal costs the user a click rather than a whole
        // stroke they then watch disappear.
        //
        // A CLOSED chain accepts nothing: it is finished, and the way to get a different
        // line is Clear line or Ctrl+Z, which are deliberate gestures rather than a
        // stray drag on the model. That is the branch of the feedback's "(or, if the
        // chain is closed, replaces it after confirmation...)" this takes - no implicit
        // replacement at all.
        //
        // An EMPTY chain accepts anything (there is nothing to continue), which is what
        // end_for_start() says and what the screen pick cannot: it has no endpoints to
        // project. So the two are combined - the screen pick decides for a chain that
        // HAS ends, and the chain itself decides the empty and closed cases.
        const DrawChainEnd at = m_draw_chain.empty()      ? DrawChainEnd::Back :
                                m_draw_chain.is_closed()  ? DrawChainEnd::None
                                                          : picked;
        if (at == DrawChainEnd::None) {
            m_draw_capture.clear();
            m_draw_last_mouse = Vec2d::Zero();
            // Say why, in the place the user is already looking. The panel's own line
            // is static text, so this goes through the notification manager the way the
            // rest of the gizmo's refusals do.
            m_draw_reject_msg = m_draw_chain.is_closed()
                              ? DrawRejectReason::ChainClosed : DrawRejectReason::Disjoint;
            m_parent.set_as_dirty();
            return false;
        }
        m_draw_reject_msg  = DrawRejectReason::None;
        m_draw_capture_end = at;

        // 2026-09-13, ITEM 1, THE FAR-SIDE CLICK, and what is deliberately NOT done
        // about it.
        //
        // The pick is now in screen space, so an endpoint marker drawn THROUGH the part
        // (they are rendered after a depth clear, deliberately, because a far endpoint
        // is where the next stroke has to start) can be clicked. The click still
        // raycasts the mesh and the ray stops at the NEAR wall, so the stroke's first
        // sample is on the face in front of the marker, not at the marker.
        //
        // That is correct, and seeding the endpoint in front of it would not improve it:
        // append_at() drops a first sample that duplicates the join (a zero-length span
        // is what the tangent maths cannot read), so the seed would vanish and the chain
        // would join from its own last sample either way - which is exactly what
        // happens without it. The span from the endpoint to the first hit is the span
        // the user actually drew, and the resampler walks it like any other.
        //
        // What the screen pick DID fix is the refusal: the press is now accepted, so the
        // chain can be extended from an endpoint the user can see. Which face the stroke
        // starts on is then their aim, as it is for every other stroke.
        m_draw_hover_end   = DrawChainEnd::None;

        // ONE UNDO ENTRY PER APPENDED STROKE, pushed here while m_draw_chain still holds
        // the state to come back TO - the same "push before the change" rule
        // push_curved_undo() follows. LeftUp pops it again if nothing was appended.
        push_draw_undo();

        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Draw cut line"), UndoRedo::SnapshotType::GizmoAction);
        m_draw_capturing  = true;
        m_draw_snap_armed = false;
        invalidate_draw_stroke();
        m_parent.set_as_dirty();
        return true;
    }

    return false;
}

// --- render ----------------------------------------------------------------

void GLGizmoCut3D::update_draw_preview_models()
{
    m_draw_ribbon_model.reset();
    m_draw_cutter_model.reset();
    m_draw_preview_dirty = false;

    // THE RIBBON. Drawn from the samples that exist right now - the RAW ones while
    // a stroke is in progress (there is no finished path yet), the finished path
    // once there is one - as a narrow strip of quads lying on the surface, one
    // quad per span, widened along the surface normal crossed with the tangent so
    // the strip hugs the face rather than standing off it.
    // PHASE 2: while a point is being DRAGGED the edited points are the truth - the
    // stroke has not been re-finished yet (that happens on release, so a resample
    // mid-drag does not walk the point out from under the cursor), so both the ribbon
    // and the cutter shell below read m_draw_points instead. That is what makes the
    // preview update live under the drag.
    // 2026-09-12: while the chain is OPEN there is no finished path at all (that is the
    // point - nothing is lofted), so the polyline is drawn from the chain's own samples,
    // with whatever stroke is being captured right now spliced on at the end it
    // continues. That is what the user watches as they draw.
    std::vector<DrawCutSample> live;
    const bool dragging_pt = m_draw_drag_pt >= 0 && m_draw_points.size() >= 2;
    if (!dragging_pt && m_draw_stroke.path().empty()) {
        live = m_draw_chain.samples();
        if (!m_draw_capture.empty()) {
            if (m_draw_capture_end == DrawChainEnd::Front && !live.empty()) {
                // The in-progress stroke runs AWAY from the chain's front, so it is
                // reversed and put in front - the same thing append() will do on release,
                // so what the user sees while drawing is what they get.
                std::vector<DrawCutSample> rev = m_draw_capture;
                std::reverse(rev.begin(), rev.end());
                live.insert(live.begin(), rev.begin(), rev.end());
            }
            else
                live.insert(live.end(), m_draw_capture.begin(), m_draw_capture.end());
        }
    }
    const std::vector<DrawCutSample>& pts = dragging_pt ? m_draw_points
                                          : m_draw_stroke.path().empty() ? live
                                                                         : m_draw_stroke.path();
    if (pts.size() >= 2) {
        // Half width scaled to the part, so the line reads the same on a 10 mm
        // trinket and a 200 mm print.
        const double half_w = std::max(0.15, 0.004 * get_grabber_mean_size(m_bounding_box));
        const bool   closed = (dragging_pt || !m_draw_stroke.path().empty()) && m_draw_stroke.is_closed();

        indexed_triangle_set its;
        const size_t n = pts.size();
        its.vertices.reserve(n * 2);
        for (size_t i = 0; i < n; ++ i) {
            // The tangent, one-sided at an open end.
            Vec3d t;
            if (closed)
                t = pts[(i + 1) % n].pos - pts[(i + n - 1) % n].pos;
            else if (i == 0)
                t = pts[1].pos - pts[0].pos;
            else if (i + 1 == n)
                t = pts[n - 1].pos - pts[n - 2].pos;
            else
                t = pts[i + 1].pos - pts[i - 1].pos;
            if (t.norm() < 1e-9)
                t = Vec3d::UnitX();
            t.normalize();
            const Vec3d& nrm = pts[i].normal;
            Vec3d side = t.cross(nrm);
            if (side.norm() < 1e-9)
                side = Vec3d::UnitY();
            side.normalize();
            its.vertices.emplace_back((pts[i].pos - half_w * side).cast<float>());
            its.vertices.emplace_back((pts[i].pos + half_w * side).cast<float>());
        }
        const size_t spans = closed ? n : n - 1;
        its.indices.reserve(spans * 2);
        for (size_t i = 0; i < spans; ++ i) {
            const int a = int(2 * i), b = int(2 * i + 1);
            const int c = int(2 * ((i + 1) % n)), d = int(2 * ((i + 1) % n) + 1);
            its.indices.emplace_back(Vec3i32(a, b, d));
            its.indices.emplace_back(Vec3i32(a, d, c));
        }
        if (!its.indices.empty())
            m_draw_ribbon_model.init_from(its);
    }

    // THE CUTTER SHELL, i.e. the preview of the cut surface. Built from exactly the
    // same call the cut will make, so this is the surface, not a picture of it.
    // PHASE 2: mid-drag the shell is built from a stroke made of the EDITED points,
    // so the surface the user sees follows the handle they are holding. It is set_path
    // rather than a fresh finish() for the same reason the ribbon reads the points
    // directly: re-resampling mid-drag would move the point out from under the cursor.
    // The path is the same LENGTH as the stroke's whenever nothing was inserted or
    // deleted, which is the only case a drag can reach.
    DrawCutStroke shell_stroke = m_draw_stroke;
    if (dragging_pt && m_draw_points.size() == m_draw_stroke.path().size())
        shell_stroke.set_path(m_draw_points);

    if (shell_stroke.valid()) {
        // THE MESH GOES TO THE BUILDER TOO. 2026-09-16: a plug's lid is now measured
        // off the part (draw_cut_band_core_solid: the height field over the loop), and
        // wrap-or-plug is decided against the part's own section. Without the mesh the
        // builder falls back to a flat lid at the bounding box's far side, so the shell
        // would not be the surface the cut makes.
        BoundingBoxf3 bbox;
        indexed_triangle_set        mesh;
        const indexed_triangle_set* src = nullptr;
        if (!m_draw_pick_its.empty())
            src = &m_draw_pick_its;
        else if (curved_instance_mesh_in_plane(mesh))
            src = &mesh;
        if (src != nullptr)
            for (const Vec3f& v : src->vertices)
                bbox.merge(v.cast<double>());
        // THE PREVIEW'S REACH IS NOT THE CUT'S REACH. Through-all deliberately runs
        // 1.05 * the bbox diagonal so the cutter exits any side of any part, which is
        // right for the boolean and wrong for the eye: a translucent tube stretching
        // most of a diagonal past the model reads as a mistake rather than as a cut.
        //
        // So the shell is drawn with the depth CLAMPED to what the part can actually
        // use - the largest distance from the stroke to the far side of the bounding
        // box - which is the same surface over the part itself, just not trailing off
        // into empty space. The cut still uses the full reach.
        DrawCutParams shown = m_draw_params;
        if (shown.through_all && bbox.defined) {
            double need = 0.0;
            for (const DrawCutSample& smp : shell_stroke.path()) {
                // The furthest corner of the box from this sample. Whatever direction
                // the ray takes, it is outside the box by then - so the shell still
                // covers the whole part and only the empty-space tail is cut off.
                need = std::max(need, (bbox.max - smp.pos).norm());
                need = std::max(need, (smp.pos - bbox.min).norm());
            }
            shown.through_all = false;
            shown.depth       = std::max(1.0, need);
        }

        // draw_cut_preview_surface(), not draw_cut_cutter_solid(): the shell shows
        // the surface the user drew - band, skirt, core plate - not the closure
        // geometry (a plug's lid past the loop, or a wrap's flange/wall/cap) that
        // only exists so the boolean has a watertight solid. 2026-09-16 owner
        // report: a belt round the bunny previewed as "a giant translucent curved
        // wall/cylinder sweeping far outside the part" - the wrap's flange, which
        // reaches out to `reach` (past the object's bounding box) before turning
        // down - even though the cut itself was exactly right. See that function's
        // declaration in DrawCut.hpp.
        const indexed_triangle_set cutter = draw_cut_preview_surface(shell_stroke, shown, bbox, 0.0, src);
        if (!cutter.empty())
            m_draw_cutter_model.init_from(cutter);
    }
}

void GLGizmoCut3D::render_draw_stroke()
{
    if (cut_line_processing())
        return;

    if (m_draw_preview_dirty)
        update_draw_preview_models();

    const Camera&     camera = wxGetApp().plater()->get_camera();
    const Transform3d plane_to_world  = translation_transform(m_plane_center) * m_rotation_m;
    const Transform3d view_model_matrix = camera.get_view_matrix() * plane_to_world;

    // (1) THE CUTTER SHELL, translucent, in the plain flat shader with blending -
    // the same treatment the curved sheet's own preview gets.
    if (m_draw_cutter_model.is_initialized()) {
        if (GLShaderProgram* shader = wxGetApp().get_shader("flat"); shader != nullptr) {
            glsafe(::glEnable(GL_DEPTH_TEST));
            glsafe(::glDisable(GL_CULL_FACE));
            glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            shader->start_using();
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            shader->set_uniform("view_model_matrix", view_model_matrix);
            ColorRGBA clr = can_perform_cut() ? CUT_PLANE_DEF_COLOR : CUT_PLANE_ERR_COLOR;
            clr.a(0.25f); // a shell, not a wall: the part has to show through it
            m_draw_cutter_model.set_color(clr);
            m_draw_cutter_model.render();
            shader->stop_using();
            glsafe(::glEnable(GL_CULL_FACE));
            glsafe(::glDisable(GL_BLEND));
        }
    }

    if (!m_draw_ribbon_model.is_initialized()) {
        // Still draw the handles: a line with no ribbon (a stroke too short to
        // resample) is one the user may still want to fix by dragging a point.
        render_draw_point_handles();
        return;
    }

    // (2) THE RIBBON, twice.
    //
    // First with the depth test OFF in a dim colour, so a closed loop's HIDDEN
    // half still reads as a ring - the render_cursor_circle() treatment. Then on
    // the surface in full colour.
    if (GLShaderProgram* shader = wxGetApp().get_shader("flat"); shader != nullptr) {
        glsafe(::glDisable(GL_DEPTH_TEST));
        glsafe(::glDisable(GL_CULL_FACE));
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        shader->start_using();
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("view_model_matrix", view_model_matrix);
        m_draw_ribbon_model.set_color(ColorRGBA(0.9f, 0.55f, 0.1f, 0.35f));
        m_draw_ribbon_model.render();
        shader->stop_using();
        glsafe(::glEnable(GL_DEPTH_TEST));
        glsafe(::glEnable(GL_CULL_FACE));
        glsafe(::glDisable(GL_BLEND));
    }

    // A strip lying exactly on the mesh z-fights with it. The codebase solved this
    // twice and the FIRST way is the one to use: the mm_contour shader biases the
    // vertex IN CLIP SPACE (clip_position.z -= offset * abs(clip_position.w), so
    // the nudge is depth-independent), which is why nothing here needs
    // glPolygonOffset or a geometric normal offset. The shader-swap dance is
    // TriangleSelectorGUI::render_paint_contour()'s, copied including the stash and
    // restart of whatever shader was current.
    {
        auto* curr_shader = wxGetApp().get_current_shader();
        if (curr_shader != nullptr)
            curr_shader->stop_using();

        if (auto* contour = wxGetApp().get_shader("mm_contour"); contour != nullptr) {
            contour->start_using();
            contour->set_uniform("offset", OpenGLManager::get_gl_info().is_mesa() ? 0.0005 : 0.00001);
            contour->set_uniform("view_model_matrix", view_model_matrix);
            contour->set_uniform("projection_matrix", camera.get_projection_matrix());
            glsafe(::glDisable(GL_CULL_FACE));
            m_draw_ribbon_model.set_color(ColorRGBA(1.f, 0.65f, 0.f, 1.f));
            m_draw_ribbon_model.render();
            glsafe(::glEnable(GL_CULL_FACE));
            contour->stop_using();
        }

        if (curr_shader != nullptr)
            curr_shader->start_using();
    }

    // 2026-09-12: THE CHAIN'S TWO ENDPOINTS, clearly marked, because they are the only
    // places a new stroke may begin. Drawn before the edit handles so a handle sitting
    // on an endpoint wins the depth fight.
    render_draw_chain_endpoints();

    // PHASE 2: the editable points, on top of everything, with the depth buffer
    // cleared so a closed loop's far handles are grabbable too.
    render_draw_point_handles();
}

// 2026-09-12. The chain's two endpoints, as spheres, with the SNAP RING round the one a
// release would close on.
//
// Not drawn for a CLOSED chain: it has no free endpoints, so marking two arbitrary
// samples as special would be a lie about what a click there does (nothing - a closed
// chain refuses continuation).
void GLGizmoCut3D::render_draw_chain_endpoints()
{
    // A FINISHED-OPEN chain still has two free endpoints - the user can carry on from
    // either, which clears the "finished" verdict - so its handles stay up. Only a CLOSED
    // chain has no free ends, and marking two arbitrary samples special there would be a
    // lie about what clicking them does.
    if (m_draw_chain.empty() || m_draw_chain.is_closed())
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;

    const Camera&     camera          = wxGetApp().plater()->get_camera();
    const Transform3d plane_to_world  = translation_transform(m_plane_center) * m_rotation_m;

    // 2026-09-13, OWNER CLICK-TEST ITEM 1: THE MARKERS WERE GIANT.
    //
    // This was `0.75 * m_grabber_radius`, and m_grabber_radius is
    // `0.5 * box.radius() * 0.85` (see on_set_state's sizing block) - a fraction of the
    // OBJECT's bounding sphere, in world mm. On a 40 mm cylinder that is a ball about
    // 30 mm across, i.e. the size of the part, which is what the owner saw. The name is
    // the trap: m_grabber_radius is NOT the size the plane grabbers are drawn at. Those
    // use get_half_size(get_grabber_mean_size(bb)), and with ENABLE_FIXED_GRABBER
    // get_grabber_mean_size() returns `32 * INV_ZOOM` - a SCREEN-SPACE size that shrinks
    // as the camera pulls back, which is what makes a grabber the same number of pixels
    // at every zoom. m_grabber_radius is only the radius of the rotation RING, which is
    // meant to span the part.
    //
    // So: the same screen-space rule, at the connector/control-point factor (0.5), which
    // puts these markers between the plane's own sphere grabber and the line's smaller
    // edit handles (0.32) - findable, without covering the line they sit on.
    const double r = 0.5 * get_half_size(get_grabber_mean_size(m_bounding_box));

    // WHICH END A RELEASE WOULD CLOSE ON. While a stroke is in flight that is the end
    // it is NOT continuing; with no stroke in flight neither is armed, so both rings
    // are drawn in the plain colour.
    const bool  capturing = m_draw_capturing && !m_draw_capture.empty();
    const bool  arm_front = capturing && m_draw_capture_end == DrawChainEnd::Back;
    const bool  arm_back  = capturing && m_draw_capture_end == DrawChainEnd::Front;

    // 2026-09-13, ITEM 1: HOVER. A marker the cursor is over brightens and grows a
    // little, so "this is a thing you can grab and carry on from" is visible before
    // the click rather than discovered by a click that does nothing. Only while no
    // stroke is in flight - during a stroke the green ARMED state is the one that
    // matters and two highlights at once would be noise.
    const bool  hov_front = !capturing && m_draw_hover_end == DrawChainEnd::Front;
    const bool  hov_back  = !capturing && m_draw_hover_end == DrawChainEnd::Back;

    // The depth buffer is cleared first, exactly as render_draw_point_handles() does:
    // an endpoint on the far side of a tall part still has to be findable, because it
    // is where the user has to start their next stroke.
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    shader->start_using();
    shader->set_uniform("emission_factor", 0.1f);

    auto sphere = [&](const Vec3d& p_plane, bool armed, bool hovered) {
        const double      s = armed ? 1.4 * r : hovered ? 1.25 * r : r;
        const Transform3d m = camera.get_view_matrix() * plane_to_world *
                              translation_transform(p_plane) * scale_transform(s);
        shader->set_uniform("view_model_matrix", m);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("view_normal_matrix", (Matrix3d) m.matrix().block(0, 0, 3, 3).inverse().transpose());
        // ARMED is bright green - "let go here and the loop closes". HOVERED is a pale
        // yellow-orange - "click here and you carry on from this end". A plain endpoint
        // is the same orange the line is, so the two read as one object.
        m_sphere.model.set_color(armed   ? ColorRGBA(0.1f, 0.95f, 0.3f, 1.f) :
                                 hovered ? ColorRGBA(1.f, 0.85f, 0.35f, 1.f)
                                         : ColorRGBA(1.f, 0.65f, 0.f, 1.f));
        m_sphere.model.render();
    };

    sphere(m_draw_chain.front_pos(), arm_front && m_draw_snap_armed, hov_front);
    sphere(m_draw_chain.back_pos(),  arm_back  && m_draw_snap_armed, hov_back);
    shader->stop_using();
}

// ---------------------------------------------------------------------------
// 2026-09-12, OWNER FEEDBACK 3: the halves classification.
//
// The cyan/magenta colouring, the Visible/Ghost/Hidden side display and the
// connectors all read the same per-fragment "which side" the colour-clip shader
// computes. In Draw mode that was the FLAT PLANE's dot product, so the colours ran
// straight through the drawn surface and told the user nothing.
//
// The curved cut answers this with a height field in a 2D texture. A ruled strip is
// NOT a height field over the plane - it is not single-valued in (u,v), which is
// precisely what the mode exists for - so there is no (u,v) -> z to put in a 2D
// texture and the sheet's mechanism cannot be reused at all.
//
// What IS available is the question the split itself asks: is the point inside the
// cutter solid. That is exact by construction (it is the same solid the boolean gets)
// and needs no assumption about the surface being a graph over anything. Baked into a
// 3D texture, one voxel per cell of the part's bounding box, it is one fetch per
// fragment. sampler3D is core GL 1.2 / GLSL 110, so this works on the 2.1 fallback
// path as well as the 140 one.
// ---------------------------------------------------------------------------

void GLGizmoCut3D::update_draw_field_texture()
{
    if (!m_draw_field_dirty && m_draw_field_tex != 0)
        return;

    m_draw_field_bbox = BoundingBoxf3();

    // 2026-09-13, ITEM 3. WHY THE FIELD DID NOT TAKE EFFECT, said out loud.
    //
    // Every failure below used to be a silent `return` that left the flat plane's
    // colours standing, and from the outside all of them look identical to "the
    // classification is ignored" - which is how item 3 could be reported as one bug
    // when it is a chain of five places any one of which drops the texture. The GL
    // uniforms are silent too: GLShaderProgram::set_uniform is a no-op for a name the
    // linked program does not have, with no log and no assert.
    //
    // Logged on the TRANSITION only, so a per-frame path does not flood the log.
    auto bail = [this](const char* why) {
        if (m_draw_field_fail != why) {
            m_draw_field_fail = why;
            BOOST_LOG_TRIVIAL(info) << "draw cut: halves classification falls back to the flat plane - " << why;
        }
        release_draw_field_texture();
        m_draw_field_dirty = false;
    };

    if (!m_draw_stroke.valid()) {
        // No closed line, no field - and no texture, so apply_draw_color_clip() leaves
        // the plain flat split standing. That is the right thing to show while the user
        // is still drawing: the plane's own colours are at least honest about being the
        // plane's.
        bail("the line is not a closed loop yet, so there is no cut surface to classify");
        return;
    }

    indexed_triangle_set mesh;
    if (!m_draw_pick_its.empty())
        mesh = m_draw_pick_its;
    else if (!curved_instance_mesh_in_plane(mesh)) {
        bail("the instance mesh could not be built in the cut plane's frame");
        return;
    }

    BoundingBoxf3 bbox;
    for (const Vec3f& v : mesh.vertices)
        bbox.merge(v.cast<double>());

    // The cutter with the kerf's LOWER face, which is the one that bounds the upper
    // half: with a kerf there are two surfaces and the band between them belongs to
    // neither side, so the colouring has to pick one and the upper half's own boundary
    // is the one that makes "cyan is what you keep on top" true.
    double face_lo = 0.0, face_hi = 0.0;
    cut_thickness_faces(face_lo, face_hi);
    const indexed_triangle_set cutter = draw_cut_cutter_solid(m_draw_stroke, m_draw_params, bbox,
                                                             m_draw_stroke.is_closed() ? face_hi : face_lo);
    if (cutter.empty()) {
        bail("the cutter solid came out empty");
        return;
    }

    const std::vector<float> field = draw_cut_inside_field(cutter, m_draw_stroke.is_closed(), bbox,
                                                           DrawFieldRes, DrawFieldRes, DrawFieldRes,
                                                           &m_draw_field_bbox);

    if (m_draw_field_tex == 0) {
        GLuint id = 0;
        glsafe(::glGenTextures(1, &id));
        m_draw_field_tex = (unsigned int) id;
    }
    glsafe(::glActiveTexture(GL_TEXTURE4));
    glsafe(::glBindTexture(GL_TEXTURE_3D, (GLuint) m_draw_field_tex));
    // NEAREST, not LINEAR. The field is a two-valued sign, and interpolating between
    // -1 and +1 puts a band of intermediate values across the boundary whose sign
    // depends on which side of 0 the interpolation lands - which reads as a ragged
    // edge that moves as the camera moves. Nearest gives a boundary on voxel faces,
    // which at 48^3 over a part is finer than the eye resolves on a translucent
    // preview.
    glsafe(::glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    // GL_R32F / GL_RED from 3.0, GL_LUMINANCE on the 2.1 fallback - the same pair the
    // sheet's 2D texture uses. The field is a SIGN, so the fallback's clamp to [0,1] is
    // handled by storing 0 for upper and 1 for lower and testing against 0.5 in the
    // shader rather than by an encode/decode of a range.
    if (wxGetApp().is_gl_version_greater_or_equal_to(3, 0))
        glsafe(::glTexImage3D(GL_TEXTURE_3D, 0, GL_R32F, DrawFieldRes, DrawFieldRes, DrawFieldRes, 0,
                              GL_RED, GL_FLOAT, field.data()));
    else {
        std::vector<float> enc(field.size());
        for (size_t i = 0; i < field.size(); ++ i)
            enc[i] = field[i] < 0.f ? 0.f : 1.f;
        glsafe(::glTexImage3D(GL_TEXTURE_3D, 0, GL_LUMINANCE, DrawFieldRes, DrawFieldRes, DrawFieldRes, 0,
                              GL_LUMINANCE, GL_FLOAT, enc.data()));
    }
    glsafe(::glBindTexture(GL_TEXTURE_3D, 0));
    glsafe(::glActiveTexture(GL_TEXTURE0));

    if (m_draw_field_fail != nullptr) {
        BOOST_LOG_TRIVIAL(info) << "draw cut: halves classification now follows the drawn surface ("
                                << DrawFieldRes << "^3 field, texture " << m_draw_field_tex << ")";
        m_draw_field_fail = nullptr;
    }
    m_draw_field_dirty = false;
}

void GLGizmoCut3D::release_draw_field_texture()
{
    if (m_draw_field_tex != 0) {
        GLuint id = (GLuint) m_draw_field_tex;
        glsafe(::glDeleteTextures(1, &id));
        m_draw_field_tex = 0;
    }
    m_draw_field_dirty = true;
}

void GLGizmoCut3D::apply_draw_color_clip()
{
    if (!is_draw_surface()) {
        m_parent.set_draw_color_clip(0, Transform3d::Identity(), Vec3d::Zero(), Vec3d::Ones());
        // Back from Draw mode with no line (which turned the plane split off):
        // the flat surface owns the colouring again, and it IS the cut here.
        m_parent.set_use_color_clip_plane(true);
        return;
    }

    // 2026-09-20, OWNER REPORT items 2 and 4. Until a closed, usable line exists
    // there is no cut surface - and the flat plane's halves must not stand in for
    // one, or switching to Draw keeps showing a cut that will never happen (item
    // 2). So Draw mode without a line shows the object UNSPLIT, and the plane
    // colour clip is switched off until a line completes or the surface switches
    // back. "Hide cut plane and grabbers" does not gate this (the comment below
    // about that trap still stands); CONNECTORS EDITING does not either: with a
    // finished line the drawn classification is the cut, and it has to stay drawn
    // while connectors are placed on it (item 4).
    if (!m_draw_stroke.valid()) {
        m_parent.set_draw_color_clip(0, Transform3d::Identity(), Vec3d::Zero(), Vec3d::Ones());
        m_parent.set_use_color_clip_plane(false);
        return;
    }

    // Restore on the way back (see the off-branch above): a line exists, so the
    // colouring is armed again and the draw field takes it over.
    m_parent.set_use_color_clip_plane(true);

    update_draw_field_texture();
    if (m_draw_field_tex == 0 || !m_draw_field_bbox.defined) {
        m_parent.set_draw_color_clip(0, Transform3d::Identity(), Vec3d::Zero(), Vec3d::Ones());
        return;
    }

    // world -> plane frame, and the field's own box in that frame, so the shader can
    // turn a world position into a texture coordinate. Exactly the shape
    // apply_curved_color_clip() passes for the sheet, one dimension wider.
    const Transform3d plane_to_world = translation_transform(m_plane_center) * m_rotation_m;
    m_parent.set_draw_color_clip(m_draw_field_tex, plane_to_world.inverse(),
                                 m_draw_field_bbox.min, m_draw_field_bbox.size());
}

// --- panel ----------------------------------------------------------------

void GLGizmoCut3D::render_draw_surface_inputs()
{
    // Direction. The constant modes (View, Axis X/Y/Z) are the "as-is" extrusion:
    // a prism through the stroke, the same ray at every sample. Surface normal is
    // per-sample, so the surface follows the shape the stroke was drawn on.
    // (The draft ANGLE that rotates this toward the binormal is phase 2.)
    static const char* dir_labels[5] = { "Surface normal", "View", "Axis X", "Axis Y", "Axis Z" };

    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Direction") + ": ");
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(m_control_width * 0.7f);
    const std::string dir_current = _u8L(dir_labels[std::clamp(m_draw_direction, 0, 4)]);
    if (ImGui::BeginCombo("##draw_dir", dir_current.c_str())) {
        for (int k = 0; k < 5; ++ k) {
            const std::string lbl = _u8L(dir_labels[k]);
            if (ImGui::Selectable(lbl.c_str(), m_draw_direction == k) && m_draw_direction != k) {
                m_draw_direction = k;
                // Picking View means "this way, the way I am looking now", so take
                // the camera here. The other directions do not use it.
                if (DrawCutDirection(k) == DrawCutDirection::View)
                    latch_draw_view_dir();
                refresh_draw_stroke();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("Which way the cut surface reaches into the part. Surface normal follows the face the line was drawn on; the others are a single direction, i.e. a straight extrusion of the line.").c_str(),
                         ImGui::GetFontSize() * 20.f);

    // Extension: how far the surface reaches PAST the line. This is what lets an
    // open line reach past the silhouette so the cut separates the part, and what
    // lifts a closed loop's rim clear of a concavity.
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Extension") + ": ");
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(m_control_width * 0.7f);
    if (ImGui::SliderFloat("##draw_ext", &m_draw_extension, 0.f, 50.f, "%.1f mm"))
        refresh_draw_stroke();
    ImGui::PopItemWidth();
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("How far the cut surface reaches past the line. An open line needs enough of this to reach across the part, or the cut will not separate it.").c_str(),
                         ImGui::GetFontSize() * 20.f);

    // EXTENSION ANGLE. 2026-09-15, owner item 4: which way the outward skirt leaves
    // the drawn line. Off by default, and OFF MEANS "CONTINUE THE BAND" - the skirt
    // leaves along the band's own ruling run backwards, which is what it has always
    // done and which keeps the surface C1 across the drawn line.
    //
    // A CHECKBOX plus a slider rather than a bare slider with a magic default value,
    // because the default is not a NUMBER: it tracks Angle. A slider alone would have
    // to be parked at some value, and that value would then stop following Angle the
    // moment the user moved it - which is the behaviour the checkbox makes explicit
    // instead of hiding.
    {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Extension angle") + ": ");
        ImGui::SameLine(m_label_width);
        bool ext_on = m_draw_ext_angle_on;
        if (m_imgui->bbl_checkbox(_L("Set"), ext_on)) {
            m_draw_ext_angle_on = ext_on;
            // Taking it on for the first time starts where the band is, so ticking the
            // box does not move the cut - it only makes the angle editable.
            if (m_draw_ext_angle_on && m_draw_ext_angle == 0.f)
                m_draw_ext_angle = m_draw_angle;
            refresh_draw_stroke();
        }
        const bool ext_box_hovered = ImGui::IsItemHovered();
        if (ext_box_hovered)
            m_imgui->tooltip(_u8L("Off, the skirt outside the line continues the band's own slope, which is the "
                                  "default and keeps the cut surface smooth where it crosses the skin. Tick this "
                                  "to aim the skirt somewhere else instead.").c_str(),
                             ImGui::GetFontSize() * 20.f);

        m_imgui->disabled_begin(!m_draw_ext_angle_on);
        ImGui::AlignTextToFramePadding();
        m_imgui->text(" ");
        ImGui::SameLine(m_label_width);
        ImGui::PushItemWidth(m_control_width * 0.7f);
        const bool ext_angle_changed = ImGui::SliderFloat("##draw_ext_angle", &m_draw_ext_angle,
                                                          float(DrawCutMinExtAngleDeg),
                                                          float(DrawCutMaxExtAngleDeg), "%.0f deg");
        const bool ext_angle_hovered = ImGui::IsItemHovered();
        if (ext_angle_changed && m_draw_ext_angle_on)
            refresh_draw_stroke();
        ImGui::PopItemWidth();
        m_imgui->disabled_end();
        if (ext_angle_hovered)
            m_imgui->tooltip(_u8L("Which way the skirt outside the drawn line leaves it. 0 lays it flat, level with "
                                  "the middle of the cut; higher lifts it away from the part; negative tucks it back "
                                  "under. With this off it continues the band, which is what the Angle sets.").c_str(),
                             ImGui::GetFontSize() * 20.f);
    }

    // PHASE 2: THE DRAFT ANGLE. Only Surface normal can use it - the constant
    // directions are one direction at every sample by definition, which is exactly
    // what a per-sample tilt is not - so the slider greys out for them rather than
    // sitting there doing nothing.
    // PHASE 3 (2026-09-13): the angle is the LIP ANGLE - how steeply the outer band
    // leans in towards the flat core - not the old signed draft. It is 0..90 and it
    // applies to a closed loop whatever the Direction is, because the band's
    // direction now comes from the core plane rather than from the surface normal.
    // An OPEN line still has no core, so it keeps the Surface-normal-only rule.
    // PHASE 3b (2026-09-13, owner decision): THROUGH ALL IGNORES ANGLE AND DEPTH.
    // Through all extrudes the line straight along the core-plane normal through the
    // whole part in both directions - no taper, which is what stops it converging to
    // a point below the loop and re-expanding into a second cone (the hourglass the
    // owner screenshotted). With no taper there is nothing for the Angle to lean and
    // nothing for the Depth to stop at, so both are greyed out while it is ticked.
    // The VALUES are kept, not zeroed: unticking Through all has to give the user
    // back the lip they had set.
    const bool angle_usable = (!m_draw_params.through_all) &&
                              (m_draw_chain.is_closed() ||
                               DrawCutDirection(m_draw_direction) == DrawCutDirection::SurfaceNormal);
    m_imgui->disabled_begin(!angle_usable);
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Angle") + ": ");
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(m_control_width * 0.7f);
    const bool angle_changed = ImGui::SliderFloat("##draw_angle", &m_draw_angle,
                                                  float(DrawCutMinLipAngleDeg),
                                                  float(DrawCutMaxLipAngleDeg), "%.0f deg");
    const bool angle_hovered = ImGui::IsItemHovered();
    if (angle_changed)
        refresh_draw_stroke();
    ImGui::PopItemWidth();
    m_imgui->disabled_end();
    if (angle_hovered)
        m_imgui->tooltip(m_draw_params.through_all
                         ? _u8L("Through all ignores the Angle: the cut goes straight through the part along the "
                                "middle plane's own normal, with no lip to lean. Untick Through all to use it again.").c_str()
                         : _u8L("The angle at which the outer shell projects in towards the flat middle of the cut. "
                                "0 makes a flat shelf around the core, 45 a chamfered lip the two halves key into, "
                                "and 90 a straight wall with no lip. The SIGN is which way the lip leans: positive "
                                "projects it away from the face you drew on, negative back towards you, so the same "
                                "line can be given a lip on either side.").c_str(),
                         ImGui::GetFontSize() * 20.f);

    // Depth: how far the band travels in before the surface turns onto the flat
    // core. Through all drops the core entirely.
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Depth") + ": ");
    ImGui::SameLine(m_label_width);
    if (!DrawThroughAllHidden) {
        // THE CONTROL IS HIDDEN - see DrawThroughAllHidden in the header. Kept rather
        // than deleted so the mode can come back once it has a defined purpose.
        bool through = m_draw_params.through_all;
        const bool through_toggled = m_imgui->bbl_checkbox(_L("Through all"), through);
        const bool through_hovered = ImGui::IsItemHovered();
        if (through_toggled) {
            m_draw_params.through_all = through;
            refresh_draw_stroke();
        }
        if (through_hovered)
            m_imgui->tooltip(_u8L("Cut all the way through the part instead of meeting a flat middle. "
                                  "The line is carried straight INWARD from the surface you drew on - square to the "
                                  "middle plane, away from you - through everything behind it. Nothing in front of the "
                                  "line is touched. The Angle and the Depth do not apply: a line all the way round a "
                                  "part separates it, and a line on a face leaves a straight plug.").c_str(),
                             ImGui::GetFontSize() * 20.f);
    }
    {
        // THE DEPTH SLIDER IS GREYED, NOT HIDDEN, under Through all. Hiding it made
        // the panel jump by a row every time the checkbox was clicked, and it hid the
        // value the user would get back on unticking - so they could not see what they
        // had set. Greyed out says "this does nothing right now" without either.
        // 2026-09-15, owner item 2: A SLIDER OVER THE USEFUL RANGE WITH THE EXACT
        // FIELD BESIDE IT - the same pattern the Edit gizmo's bevel width uses.
        //
        // The slider used to run 0.1 .. the bounding box DIAGONAL, which is the range
        // the old ruled-strip Depth needed ("reach right through the part") and is
        // nonsense for a band travel: on a 200 mm part every useful value - 1 to 5 mm -
        // sat inside the first two pixels of the track, so the control could not be
        // set by dragging at all. 0..20 mm is the band's own range, and a value past
        // it is typed rather than dragged, which is what the field is for.
        //
        // The two are ONE VALUE (&m_draw_depth), so they cannot disagree: whichever
        // control moved wrote it, and the other draws from it on the same frame.
        m_imgui->disabled_begin(m_draw_params.through_all);
        if (!DrawThroughAllHidden) {
            // The checkbox took the label's row, so the slider goes on the next one.
            // With the checkbox hidden the slider sits beside the label itself.
            ImGui::AlignTextToFramePadding();
            m_imgui->text(" ");
            ImGui::SameLine(m_label_width);
        }
        const float slider_icon_width = m_imgui->get_slider_icon_size().x;
        ImGui::PushItemWidth(m_control_width * 0.7f - 1.5f * slider_icon_width);
        bool depth_changed = ImGui::SliderFloat("##draw_depth", &m_draw_depth,
                                                DrawDepthSliderMin, DrawDepthSliderMax, "%.1f mm");
        const bool depth_hovered = ImGui::IsItemHovered();
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::PushItemWidth(1.5f * slider_icon_width);
        // Bounds 0..DrawDepthMax rather than the slider's, so a value the slider
        // cannot reach can still be typed - which is the owner's "values typed above
        // 20 are allowed via the text box".
        if (ImGui::BBLDragFloat("##draw_depth_input", &m_draw_depth, DrawDepthStep,
                                DrawDepthMin, DrawDepthMax, "%.1f mm"))
            depth_changed = true;
        ImGui::PopItemWidth();
        m_draw_depth = std::clamp(m_draw_depth, DrawDepthMin, DrawDepthMax);
        if (depth_changed && !m_draw_params.through_all)
            refresh_draw_stroke();
        m_imgui->disabled_end();
        if (depth_hovered)
            m_imgui->tooltip(m_draw_params.through_all
                             ? _u8L("Through all ignores the Depth: the cut runs the whole way through the part rather "
                                    "than stopping at a flat middle. Untick Through all to use it again.").c_str()
                             : _u8L("How far in the cut travels from the drawn line before it turns onto the flat middle. "
                                    "The deeper it goes, the smaller the flat core - too deep and there is no flat left at all.").c_str(),
                             ImGui::GetFontSize() * 20.f);
    }

    // Smoothing. A raw stroke picks up every triangle normal it crossed, and an
    // unsmoothed normal field visibly kinks the ruled surface.
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Smoothing") + ": ");
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(m_control_width * 0.7f);
    if (ImGui::SliderFloat("##draw_smooth", &m_draw_smoothing, 0.f, 1.f, "%.2f"))
        refresh_draw_stroke();
    ImGui::PopItemWidth();
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("Averages the line and its surface normals. A hand-drawn line picks up every facet it crossed; a little of this takes the kinks out of the cut surface.").c_str(),
                         ImGui::GetFontSize() * 20.f);

    // PHASE 2: EDIT POINTS. A toggle rather than a modifier key, because the two
    // gestures collide - a left-drag on the model is "draw a new line" in one mode
    // and "move this point" in the other, and there is no way to tell them apart
    // from the event alone.
    m_imgui->disabled_begin(!m_draw_stroke.valid());
    bool editing = m_draw_editing;
    if (m_imgui->bbl_checkbox(_L("Edit points"), editing)) {
        m_draw_editing = editing && m_draw_stroke.valid();
        m_draw_hover_pt = m_draw_drag_pt = -1;
        if (m_draw_editing)
            sync_draw_points();
        m_parent.set_as_dirty();
    }
    m_imgui->disabled_end();
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("Turn the line's points into handles you can move. "
                              "Drag a handle to move it along the surface, right-click one to delete it, "
                              "and Shift+click a segment to add one. Each edit is a separate undo step.").c_str(),
                         ImGui::GetFontSize() * 20.f);

    ImGui::SameLine();

    // CLOSE LOOP: the explicit way to finish a chain whose two ends the user cannot
    // comfortably bring together - a line round a feature that comes back to within a
    // few millimetres but not within the snap radius. The closing span is then whatever
    // gap is left, which the resampler walks like any other span.
    m_imgui->disabled_begin(m_draw_chain.is_closed() || m_draw_chain.size() < DrawCutChain::MinChainSamples);
    if (m_imgui->button(_L("Close loop"),
                        _L("Join the two ends of the line along the surface, so the cut surface can be made"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Close draw cut loop"), UndoRedo::SnapshotType::GizmoAction);
        push_draw_undo();
        // 2026-09-13, ITEM 2: ALONG THE SURFACE, never through the part. When no path
        // can be found from this view the chain is left exactly as it was and the undo
        // entry is popped again - a refusal must not cost the user a Ctrl+Z that appears
        // to do nothing.
        if (close_draw_chain_on_surface()) {
            m_draw_reject_msg = DrawRejectReason::None;
            refresh_draw_stroke();
        }
        else {
            if (!m_draw_undo.empty())
                m_draw_undo.pop_back();
            m_draw_reject_msg = DrawRejectReason::CloseNoPath;
        }
    }
    m_imgui->disabled_end();

    ImGui::SameLine();

    // PHASE 1'S OPEN CUT, now explicit. A line right across a part splits it in two
    // with no inside or outside, which is a perfectly good cut - but the chain cannot
    // loft it automatically, because "not finished yet" and "finished, not a loop" look
    // identical from the samples, and lofting every partial line is the wild-shape
    // preview this change exists to remove. So the user says which.
    m_imgui->disabled_begin(m_draw_chain.is_closed() || m_draw_chain.is_finished_open() ||
                            m_draw_chain.size() < DrawCutChain::MinChainSamples);
    if (m_imgui->button(_L("Cut along the line"),
                        _L("Use the line as it is, without closing it: the cut splits the part along it"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Finish draw cut line"), UndoRedo::SnapshotType::GizmoAction);
        push_draw_undo();
        m_draw_chain.finish_open();
        m_draw_reject_msg = DrawRejectReason::None;
        refresh_draw_stroke();
    }
    m_imgui->disabled_end();

    ImGui::SameLine();

    m_imgui->disabled_begin(m_draw_chain.empty());
    if (m_imgui->button(_L("Clear line"), _L("Remove the drawn line"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Clear draw cut line"), UndoRedo::SnapshotType::GizmoAction);
        clear_draw_stroke(/*push_undo*/ true);
    }
    m_imgui->disabled_end();

    ImGui::PushTextWrapPos(m_editing_window_width);
    if (m_draw_editing)
        m_imgui->text(_L("Drag a handle to move it; right-click one to delete it; Shift+click a segment to add one."));
    else if (m_draw_chain.empty())
        m_imgui->text(_L("Drag on the model to start the cut line. You can rotate the view and carry on from either end until the line closes on itself."));
    else if (m_draw_chain.is_finished_open())
        m_imgui->text(_L("Using the line as it is. Draw from an end to carry on; Esc clears it; "
                         "Ctrl+Z takes back one stroke and Ctrl+Y puts it back."));
    else if (!m_draw_chain.is_closed())
        // THE INSTRUCTION THAT MAKES THE FEATURE DISCOVERABLE (owner feedback item 1):
        // the gesture is not "draw the whole thing in one go", and nothing on screen
        // said so.
        m_imgui->text(_L("Start your next stroke at one of the marked ends. Rotate the view freely. "
                         "Finish a stroke near the other end and the loop closes - or press "
                         "\"Cut along the line\" to use it open."));
    else
        m_imgui->text(_L("Closed. Esc clears the line; Ctrl+Z takes back one stroke and Ctrl+Y puts it back."));

    // WHY THE LAST PRESS WAS REFUSED. Only one chain may exist, so a press that does
    // not continue it does nothing - and "nothing happened" is the worst possible
    // feedback for a gesture the user thought they made.
    if (m_draw_reject_msg == DrawRejectReason::Disjoint)
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                              _L("That stroke did not start at either end of the line, so it was not added. "
                                 "Start at one of the marked ends, or clear the line first."));
    else if (m_draw_reject_msg == DrawRejectReason::ChainClosed)
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                              _L("The line is already closed. Clear it, or press Ctrl+Z, before drawing a different one."));
    // 2026-09-13, ITEM 2. Close loop joins the two ends ALONG THE SURFACE, and from this
    // view there is no surface between them - they are round a corner, or on opposite
    // sides of the part. Joining them anyway would run the closing span straight through
    // the material and put the cut surface out the far side, which is what this replaced.
    // So say what will work: draw the rest.
    else if (m_draw_reject_msg == DrawRejectReason::CloseNoPath)
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                              _L("The two ends cannot be joined across the surface from this view. "
                                 "Turn the model so the gap between them is visible and try again, "
                                 "or draw the remaining part of the line by hand."));

    if (m_draw_chain.empty())
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, _L("Draw a line on the model."));
    else if (!m_draw_chain.is_closed() && !m_draw_chain.is_finished_open())
        // OWNER FEEDBACK ITEMS 1 AND 2: no surface until the line is finished, said in
        // the panel so the absence of a preview reads as "not yet" rather than as broken.
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                              format_wxstr(_L("The line is not finished, so there is no cut surface yet. "
                                              "Carry on from an end until it comes within %1% mm of the other, "
                                              "or use it open."),
                                           int(std::lround(draw_chain_snap_radius()))));
    else if (!m_draw_stroke.valid()) {
        // The message comes from libslic3r as untranslated English (the tests read the
        // enum, so the strings live next to it), and _L() needs a literal to extract.
        // Translate the ENUM here, where the literals are visible to gettext.
        wxString why;
        switch (m_draw_stroke.error()) {
        case DrawCutError::TooShort:         why = _L("Draw a longer line on the model."); break;
        case DrawCutError::SelfCrossing:     why = _L("The line crosses itself."); break;
        case DrawCutError::LeavesMesh:       why = _L("The line leaves the model."); break;
        case DrawCutError::CutterDegenerate: why = _L("The line does not make a usable cut surface."); break;
        case DrawCutError::EmptySide:        why = _L("The stroke does not separate the part."); break;
        default:                             why = from_u8(draw_cut_error_message(m_draw_stroke.error())); break;
        }
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, why);
    }
    else {
        // Say WHICH it chose. A loop and a line cut differently (a loop takes a
        // plug out; a line splits the part), so the user has to be able to see
        // which one they got.
        m_imgui->text(m_draw_stroke.is_closed()
                      ? _L("Closed loop: the cut takes out the piece the line goes around.")
                      : _L("Open line: the cut splits the part along the line."));

        // THE EMPTY-SIDE WARNING, the analogue of the curved cut's. Said BEFORE the
        // click, and here it also DISABLES the cut (see can_perform_cut): a stroke
        // that does not separate the part gives back the whole part and nothing
        // else, which is not a cut at all.
        if (m_draw_upper_empty || m_draw_lower_empty)
            m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                                  _L("The stroke does not separate the part - increase Extension or draw the line right across it."));
        if (m_draw_folds)
            // TWO DIFFERENT FAULTS, AND THEY NEED DIFFERENT WORDS. A CLOSED loop
            // cannot fold against a tight corner at all - its band travels toward the
            // loop's own axis and stops on the core plane - so what it can fail at is
            // the Depth eating the whole core. An OPEN line is still a ruled strip and
            // still folds the phase-2 way. Saying "reduce the Angle" to someone whose
            // core has been eaten sends them the wrong way: a LARGER angle insets less
            // (the inset is depth * cos(angle)), which is why the closed wording names
            // raising it.
            m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                                  m_draw_chain.is_closed()
                                  ? _L("Depth reaches almost to the middle of the line, so there is no flat left to mate on. Reduce Depth, or raise the Angle.")
                                  : (m_draw_params.angle_deg != 0.0
                                     ? _L("The line turns tighter than the cut surface reaches sideways, so the surface folds there. Reduce the Angle, the Extension or the Depth.")
                                     : _L("The line turns tighter than the Extension reaches, so the cut surface folds there. Reduce Extension.")));

        // 2026-09-20: the extension-clip advisory. Not an error - the cut works -
        // but the surface the user asked for is shorter than the slider says,
        // exactly where the unclamped extension would have crossed other geometry.
        if (m_draw_ext_clipped)
            m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                                  _L("The Extension reaches other parts of the model here, so it has been clipped to keep the cut on the line you drew. Shorten the line's gap to those parts or reduce Extension."));

        // PHASE 2: the holonomy fallback. Said plainly, because the symptom without
        // it ("the draft went the wrong way round half my loop") is baffling.
        if (m_draw_frame_flips)
            m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                                  _L("This loop has no consistent outside, so the Angle has been ignored. Draw a simpler loop, or use Direction = Axis or View."));

        // PHASE 2: the connector advisories, the analogues of the curved cut's.
        if (m_draw_offsurface_connectors > 0)
            m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                                  format_wxstr(_L("%1% connector(s) are not clear of the cut surface's edge; "
                                                  "the split may only make part of them. Move them further onto the surface."),
                                               m_draw_offsurface_connectors));
        if (m_draw_tilted_connectors > 0)
            m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                                  format_wxstr(_L("%1% connector(s) stand more than %2%° off the cut direction; "
                                                  "they will print at an angle and may need supports."),
                                               m_draw_tilted_connectors, int(CurvedConnectorTiltWarnDeg)));
        if (m_draw_unflat_connectors > 0)
            m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                                  format_wxstr(_L("%1% Hinge/Thread connector(s) sit where the drawn surface curves too tightly "
                                                  "for their straight features to mate. Straighten the line under them "
                                                  "or use a Plug, Dowel or Snap instead."),
                                               m_draw_unflat_connectors));
    }
    ImGui::PopTextWrapPos();
}

// The cut THICKNESS applies to every surface mode, so a change to it has to
// refresh whichever surface is live - the sheet's empty-side test and preview, or
// the stroke's. One call rather than the pair repeated at every widget, which is
// how the Draw case came to be missing from three of the four.
void GLGizmoCut3D::invalidate_cut_surfaces()
{
    if (is_draw_surface())
        refresh_draw_stroke();
    else {
        update_curved_empty_sides();
        invalidate_curved_sheet();
    }
}

void GLGizmoCut3D::render_cut_thickness_input()
{
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Thickness") + ": ");
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(m_control_width * 0.7f);

    float t = m_cut_thickness;
    if (ImGui::SliderFloat("##cut_thickness", &t, float(CutThicknessMin), float(CutThicknessMax), "%.2f mm")) {
        m_cut_thickness = std::clamp(t, float(CutThicknessMin), float(CutThicknessMax));
        invalidate_cut_surfaces();
    }
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("Width of the band of material the cut removes, measured along the cut normal. "
                              "0 is a zero-width cut - the two halves meet exactly, as they do today.").c_str(),
                         ImGui::GetFontSize() * 20.f);

    if (m_cut_thickness > 0.f) {
        // Which side the band is taken from. Centred is the default and is what
        // a saw does; Above / Below let the user keep one half's face exactly on
        // the plane they positioned.
        const std::string centred_label = _u8L("Centred");
        const std::string above_label   = _u8L("Above");
        const std::string below_label   = _u8L("Below");

        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Remove from") + ": ");
        ImGui::SameLine(m_label_width);
        bool centred = m_cut_thickness_offset == int(CutThicknessOffset::Centred);
        if (m_imgui->bbl_radio_button(centred_label.c_str(), centred)) {
            m_cut_thickness_offset = int(CutThicknessOffset::Centred);
            invalidate_cut_surfaces();
        }
        ImGui::SameLine();
        bool above = m_cut_thickness_offset == int(CutThicknessOffset::Above);
        if (m_imgui->bbl_radio_button(above_label.c_str(), above)) {
            m_cut_thickness_offset = int(CutThicknessOffset::Above);
            invalidate_cut_surfaces();
        }
        ImGui::SameLine();
        bool below = m_cut_thickness_offset == int(CutThicknessOffset::Below);
        if (m_imgui->bbl_radio_button(below_label.c_str(), below)) {
            m_cut_thickness_offset = int(CutThicknessOffset::Below);
            invalidate_cut_surfaces();
        }

        ImGui::PushTextWrapPos(m_editing_window_width);
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                              _L("The two halves will be this far apart. Connectors span the gap; "
                                 "a Thread connector needs a thickness below its pitch to stay usable."));
        ImGui::PopTextWrapPos();
    }
}

void GLGizmoCut3D::render_cut_plane_grabbers()
{
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));

    ColorRGBA color = ColorRGBA::GRAY();

    const Transform3d view_matrix = wxGetApp().plater()->get_camera().get_view_matrix() * translation_transform(m_plane_center) * m_rotation_m;

    const double mean_size = get_grabber_mean_size(m_bounding_box);
    double size;

    const bool no_xy_dragging = m_dragging && m_hover_id == CutPlane;

    if (!no_xy_dragging && m_hover_id != CutPlaneZRotation && m_hover_id != CutPlaneXMove && m_hover_id != CutPlaneYMove) {
        render_grabber_connection(GRABBER_COLOR, view_matrix);

        // render sphere grabber
        size = m_dragging ? get_dragging_half_size(mean_size) : get_half_size(mean_size);
        color = m_hover_id == Y ? ColorRGBA::Y() : // ORCA match axis colors
                m_hover_id == X ? ColorRGBA::X() : // ORCA match axis colors
                m_hover_id == Z ? GRABBER_COLOR                     :   ColorRGBA::GRAY();
        render_model(m_sphere.model, color, view_matrix * translation_transform(m_grabber_connection_len * Vec3d::UnitZ()) * scale_transform(size));
    }

    const bool no_xy_grabber_hovered = !m_dragging && (m_hover_id < 0 || m_hover_id == CutPlane);

    // render X grabber

    if (no_xy_grabber_hovered || m_hover_id == X)
    {
        size = m_dragging && m_hover_id == X ? get_dragging_half_size(mean_size) : get_half_size(mean_size);
        const Vec3d cone_scale = Vec3d(0.75 * size, 0.75 * size, 1.8 * size);
        //color = m_hover_id == X ? complementary(ColorRGBA::X()) : ColorRGBA::X();
        color = ColorRGBA::X(); // ORCA match axis colors

        if (m_hover_id == X) {
            render_grabber_connection(color, view_matrix);
            render_rotation_snapping(X, color);
        }

        Vec3d offset = Vec3d(0.0, 1.25 * size, m_grabber_connection_len);
        render_model(m_cone.model, color, view_matrix * translation_transform(offset) * rotation_transform(-0.5 * PI * Vec3d::UnitX()) * scale_transform(cone_scale));
        offset = Vec3d(0.0, -1.25 * size, m_grabber_connection_len);
        render_model(m_cone.model, color, view_matrix * translation_transform(offset) * rotation_transform(0.5 * PI * Vec3d::UnitX()) * scale_transform(cone_scale));
    }

    // render Y grabber

    if (no_xy_grabber_hovered || m_hover_id == Y)
    {
        size = m_dragging && m_hover_id == Y ? get_dragging_half_size(mean_size) : get_half_size(mean_size);
        const Vec3d cone_scale = Vec3d(0.75 * size, 0.75 * size, 1.8 * size);
        //color = m_hover_id == Y ? complementary(ColorRGBA::Y()) : ColorRGBA::Y();
        color = ColorRGBA::Y(); // ORCA match axis colors

        if (m_hover_id == Y) {
            render_grabber_connection(color, view_matrix);
            render_rotation_snapping(Y, color);
        }

        Vec3d offset = Vec3d(1.25 * size, 0.0, m_grabber_connection_len);
        render_model(m_cone.model, color, view_matrix * translation_transform(offset) * rotation_transform(0.5 * PI * Vec3d::UnitY()) * scale_transform(cone_scale));
        offset = Vec3d(-1.25 * size, 0.0, m_grabber_connection_len);
        render_model(m_cone.model, color, view_matrix * translation_transform(offset) * rotation_transform(-0.5 * PI * Vec3d::UnitY()) * scale_transform(cone_scale));
    }

    if (CutMode(m_mode) == CutMode::cutTongueAndGroove) {

        // render CutPlaneZRotation grabber

        if (no_xy_grabber_hovered || m_hover_id == CutPlaneZRotation)
        {
            size = 0.75 * (m_dragging ? get_dragging_half_size(mean_size) : get_half_size(mean_size));
            color = ColorRGBA::Z(); // ORCA match axis colors
            const ColorRGBA cp_color = m_hover_id == CutPlaneZRotation ? color : m_plane.model.get_color();

            const double grabber_shift = -1.75 * m_grabber_connection_len;

            render_model(m_sphere.model, cp_color, view_matrix * translation_transform(grabber_shift * Vec3d::UnitY()) * scale_transform(size));

            if (m_hover_id == CutPlaneZRotation) {
                const Vec3d cone_scale = Vec3d(0.75 * size, 0.75 * size, 1.8 * size);

                render_rotation_snapping(CutPlaneZRotation, color);
                render_grabber_connection(GRABBER_COLOR, view_matrix * rotation_transform(0.5 * PI * Vec3d::UnitX()), 1.75);

                Vec3d offset = Vec3d(1.25 * size, grabber_shift, 0.0);
                render_model(m_cone.model, color, view_matrix * translation_transform(offset) * rotation_transform(0.5 * PI * Vec3d::UnitY()) * scale_transform(cone_scale));
                offset = Vec3d(-1.25 * size, grabber_shift, 0.0);
                render_model(m_cone.model, color, view_matrix * translation_transform(offset) * rotation_transform(-0.5 * PI * Vec3d::UnitY()) * scale_transform(cone_scale));
            }
        }

        const double xy_connection_len = 0.75 * m_grabber_connection_len;

        // render CutPlaneXMove grabber

        if (no_xy_grabber_hovered || m_hover_id == CutPlaneXMove)
        {
            size = (m_dragging ? get_dragging_half_size(mean_size) : get_half_size(mean_size));
            color = m_hover_id == CutPlaneXMove ? ColorRGBA::X() : m_plane.model.get_color(); // ORCA match axis colors

            render_grabber_connection(GRABBER_COLOR, view_matrix * rotation_transform(0.5 * PI * Vec3d::UnitY()), 0.75);

            Vec3d offset = xy_connection_len * Vec3d::UnitX() - 0.5 * size * Vec3d::Ones();
            render_model(m_cube.model, color, view_matrix * translation_transform(offset) * scale_transform(size));

            const Vec3d cone_scale = Vec3d(0.5 * size, 0.5 * size, 1.8 * size);

            offset = (size + xy_connection_len) * Vec3d::UnitX();
            render_model(m_cone.model, color, view_matrix * translation_transform(offset) * rotation_transform(0.5 * PI * Vec3d::UnitY()) * scale_transform(cone_scale));
        }

        // render CutPlaneYMove grabber

        if (m_groove.angle > 0.0f && (no_xy_grabber_hovered || m_hover_id == CutPlaneYMove))
        {
            size = (m_dragging ? get_dragging_half_size(mean_size) : get_half_size(mean_size));
            color = m_hover_id == CutPlaneYMove ? ColorRGBA::Y() : m_plane.model.get_color(); // ORCA match axis colors

            render_grabber_connection(GRABBER_COLOR, view_matrix * rotation_transform(-0.5 * PI * Vec3d::UnitX()), 0.75);

            Vec3d offset = xy_connection_len * Vec3d::UnitY() - 0.5 * size * Vec3d::Ones();
            render_model(m_cube.model, color, view_matrix * translation_transform(offset) * scale_transform(size));

            const Vec3d cone_scale = Vec3d(0.5 * size, 0.5 * size, 1.8 * size);

            offset = (size + xy_connection_len) * Vec3d::UnitY();
            render_model(m_cone.model, color, view_matrix * translation_transform(offset) * rotation_transform(-0.5 * PI * Vec3d::UnitX()) * scale_transform(cone_scale));
        }
    }
}

void GLGizmoCut3D::render_cut_line()
{
    if (!cut_line_processing() || m_line_end.isApprox(Vec3d::Zero()))
        return;

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));

    m_cut_line.reset();
    m_cut_line.init_from(its_make_line((Vec3f)m_line_beg.cast<float>(), (Vec3f)m_line_end.cast<float>()));

    render_line(m_cut_line, GRABBER_COLOR, wxGetApp().plater()->get_camera().get_view_matrix(), 0.25f);
}

bool GLGizmoCut3D::on_init()
{
    m_grabbers.emplace_back();
    m_shortcut_key = WXK_CONTROL_C;

    // initiate info shortcuts
    const wxString ctrl  = GUI::shortkey_ctrl_prefix();
    const wxString alt   = GUI::shortkey_alt_prefix();
    const wxString shift = _L("Shift+");

    m_shortcuts_cut.push_back(std::make_pair(shift + _L("Drag"), _L("Draw cut line")));

    m_shortcuts_connector.push_back(std::make_pair(_L("Left click"),         _L("Add connector")));
    m_shortcuts_connector.push_back(std::make_pair(_L("Right click"),        _L("Remove connector")));
    m_shortcuts_connector.push_back(std::make_pair(_L("Drag"),               _L("Move connector")));
    m_shortcuts_connector.push_back(std::make_pair(shift + _L("Left click"), _L("Add connector to selection")));
    m_shortcuts_connector.push_back(std::make_pair(alt   + _L("Left click"), _L("Remove connector from selection")));
    m_shortcuts_connector.push_back(std::make_pair(ctrl  + "A",              _L("Select all connectors")));

    return true;
}

void GLGizmoCut3D::on_load(cereal::BinaryInputArchive& ar)
{
    size_t mode;
    float groove_depth;
    float groove_width;
    float groove_flaps_angle;
    float groove_angle;
    float groove_depth_tolerance;
    float groove_width_tolerance;

    ar( m_keep_upper, m_keep_lower, m_rotate_lower, m_rotate_upper, m_hide_cut_plane, mode, m_connectors_editing,
        m_ar_plane_center, m_rotation_m,
        groove_depth, groove_width, groove_flaps_angle, groove_angle, groove_depth_tolerance, groove_width_tolerance);

    m_start_dragging_m = m_rotation_m;

    m_transformed_bounding_box = transformed_bounding_box(m_ar_plane_center, m_rotation_m);
    set_center_pos(m_ar_plane_center);

    if (m_mode != mode)
        switch_to_mode(mode);
    else if (CutMode(m_mode) == CutMode::cutTongueAndGroove) {
        if (!is_approx(m_groove.depth          , groove_depth) ||
            !is_approx(m_groove.width          , groove_width) ||
            !is_approx(m_groove.flaps_angle    , groove_flaps_angle) ||
            !is_approx(m_groove.angle          , groove_angle) ||
            !is_approx(m_groove.depth_tolerance, groove_depth_tolerance) ||
            !is_approx(m_groove.width_tolerance, groove_width_tolerance) ) 
        {
            m_groove.depth          = groove_depth;
            m_groove.width          = groove_width;
            m_groove.flaps_angle    = groove_flaps_angle;
            m_groove.angle          = groove_angle;
            m_groove.depth_tolerance= groove_depth_tolerance;
            m_groove.width_tolerance= groove_width_tolerance;
            update_plane_model();
        }
        reset_cut_by_contours();
    }

    m_parent.request_extra_frame();
}

void GLGizmoCut3D::on_save(cereal::BinaryOutputArchive& ar) const
{ 
    ar( m_keep_upper, m_keep_lower, m_rotate_lower, m_rotate_upper, m_hide_cut_plane, m_mode, m_connectors_editing,
        m_ar_plane_center, m_start_dragging_m,
        m_groove.depth, m_groove.width, m_groove.flaps_angle, m_groove.angle, m_groove.depth_tolerance, m_groove.width_tolerance);
}

std::string GLGizmoCut3D::on_get_name() const
{
    return _u8L("Cut");
}

void GLGizmoCut3D::apply_color_clip_plane_colors()
{
    if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
        m_parent.set_color_clip_plane_colors({ CUT_PLANE_DEF_COLOR , CUT_PLANE_DEF_COLOR });
    else
        m_parent.set_color_clip_plane_colors({ UPPER_PART_COLOR , LOWER_PART_COLOR });
}

void GLGizmoCut3D::on_set_state()
{
    m_facet_picker.set_active(false); // never leave pick-face armed across open/close
    // The control grid AND the drawn stroke are session state, never persisted
    // (the cut is baked, as a plane cut is), so opening or closing the gizmo
    // starts from the flat surface.
    m_surface_mode = CutSurfaceMode::Flat;
    m_curved_sheet.reset(m_curved_nx, m_curved_ny);
    // The sheet history is session state too - it describes surfaces that no
    // longer exist once the grid is flattened.
    clear_curved_undo();
    m_cut_surface_hovered = false;
    m_curved_hover_ctl = m_curved_drag_ctl = -1;
    // Phase 2 session state goes with it: the fit has to be recomputed for the
    // next object, and side visibility is a preview aid, not a preference, so
    // both halves come back Visible every time the gizmo opens or closes.
    m_curved_fit_valid      = false;
    m_curved_fit_pending    = false;
    m_curved_snap_ctl       = -1;
    m_curved_snap_hit_valid = false;
    m_curved_snap_mesh.clear();
    // Phase 3 session state. The thickness is a property of ONE cut, not a
    // preference: leaving it set would silently kerf the next object the user
    // cuts, which is the sticky-m_keep_as_parts bug in a new place.
    m_cut_thickness        = 0.f;
    m_cut_thickness_offset = int(CutThicknessOffset::Centred);
    m_curved_res_user_set  = false;
    m_curved_upper_empty = m_curved_lower_empty = false;
    // The drawn stroke goes the same way: on_set_state() runs on open AND on
    // close, which is where a stroke has to be cleared or the next object would
    // open the gizmo with the last one's line still on it.
    m_draw_chain.clear();
    m_draw_capture.clear();
    m_draw_snap_armed  = false;
    m_draw_reject_msg  = DrawRejectReason::None;
    m_draw_stroke.clear();
    clear_draw_undo();
    m_draw_capturing   = false;
    m_draw_last_mouse  = Vec2d::Zero();
    m_draw_upper_empty = m_draw_lower_empty = false;
    m_draw_folds       = false;
    m_draw_ext_clipped = false;
    // OWNER FEEDBACK 3: the classification field is per-cut state too, and it holds a
    // GL texture, so it has to be released here and not merely marked dirty.
    release_draw_field_texture();
    m_draw_field_bbox  = BoundingBoxf3();
    // PHASE 2 state goes the same way, and for the same reason: the Angle is a
    // property of ONE cut (leaving it set would silently draft the next object's),
    // and Edit points with no line is a mode with no way out.
    m_draw_angle       = 0.f;
    m_draw_frame_flips = false;
    m_draw_editing     = false;
    m_draw_hover_pt = m_draw_drag_pt = -1;
    m_draw_points.clear();
    m_draw_tilted_connectors = m_draw_unflat_connectors = m_draw_offsurface_connectors = 0;
    m_draw_surface_raycaster.reset();
    m_draw_surface_pick_dirty = true;
    invalidate_draw_pick_mesh();
    invalidate_draw_stroke();
    m_upper_visibility = m_lower_visibility = SideVisibility::Visible;
    apply_side_visibility();
    invalidate_curved_sheet();
    release_curved_sheet_texture();
    if (m_state == On) {
        m_parent.set_use_color_clip_plane(true);

        // A Flexi joint forces "Cut to parts" and both "Keep" flags on for the duration of its
        // cut. Those are plain gizmo members on a singleton, so without this they would stay
        // forced after the cut and grey out "Add connectors" (which is disabled whenever
        // keep-as-parts is set or either half is dropped) on every later cut - the bug the
        // owner hit, where the connector option only came back after restarting the slicer.
        // Opening the gizmo is the natural place to clear it: it is once per cut.
        if (m_flexi_forced_after_cut) {
            m_keep_as_parts        = false;
            m_keep_upper           = true;
            m_keep_lower           = true;
            m_place_on_cut_upper   = false;
            m_place_on_cut_lower   = false;
            m_rotate_upper         = false;
            m_rotate_lower         = false;
            m_connector_type       = CutConnectorType::Plug;
            m_flexi_forced_after_cut = false;
        }

        update_bb();
        m_connectors_editing = !m_selected.empty();
        m_transformed_bounding_box = transformed_bounding_box(m_plane_center, m_rotation_m);

        // initiate archived values
        m_ar_plane_center   = m_plane_center;
        m_start_dragging_m  = m_rotation_m;
        reset_cut_by_contours();

        // RE-EDITABLE CUTS. Everything above has just flattened the session state,
        // which is exactly why a parked re-edit is consumed HERE and not earlier:
        // a recipe applied before the reset would have been wiped by it.
        //
        // The MODEL SURGERY normally already happened, in begin_reedit_now() called
        // by ObjectList::edit_cut() before it opened this gizmo (see the note there:
        // doing it from inside set_state(On) made reload_scene() reset the gizmo and
        // the panel never appeared). What is left to do here is re-apply the recipe
        // on top of the reset above - and this is also the fallback for any caller
        // that armed a re-edit without running the surgery first.
        if (m_reedit_pending)
            begin_reedit();
        else if (m_reedit_active)
            apply_recipe_to_gizmo(m_reedit_recipe);
        // ... and a parked "Copy cut to...", which is the same problem: the reset
        // above would wipe a recipe applied any earlier. Mutually exclusive with a
        // re-edit by construction (each menu path arms exactly one), but the order
        // is fixed rather than left to chance: a re-edit's stand-in has to be on the
        // bed before anything reads the selection's mesh.
        if (m_copy_pending)
            begin_copy();

        m_parent.request_extra_frame();
    }
    else {
        // Closing without cutting ends a re-edit: the stand-in goes and the two
        // halves come back. perform_cut() clears m_reedit_active before it closes
        // the gizmo, so the committing path does not reach this.
        if (m_reedit_active)
            cancel_reedit();
        m_reedit_pending = false;
        // A parked copy that never got consumed (the gizmo closed again before it
        // opened) must not survive to ambush the NEXT time Cut is opened.
        m_copy_pending = false;
        m_copy_pending_recipe = CutRecipe();
        if (auto oc = m_c->object_clipper()) {
            oc->set_behavior(true, true, 0.);
            oc->release();
        }
        m_selected.clear();
        m_parent.set_use_color_clip_plane(false);
        //m_c->selection_info()->set_use_shift(false);

        // Make sure that the part selection data are released when the gizmo is closed.
        // The CallAfter is needed because in perform_cut, the gizmo is closed BEFORE
        // the cut is performed (because of undo/redo snapshots), so the data would
        // be deleted prematurely.
        if (m_part_selection.valid())
            wxGetApp().CallAfter([this]() { m_part_selection = PartSelection(); });
    }
}

void GLGizmoCut3D::on_register_raycasters_for_picking()
{
 //   assert(m_raycasters.empty());
    if (!m_raycasters.empty())
        on_unregister_raycasters_for_picking();
    // the gizmo grabbers are rendered on top of the scene, so the raytraced picker should take it into account
    m_parent.set_raycaster_gizmos_on_top(true);

    init_picking_models();

    if (m_connectors_editing) {
        if (CommonGizmosDataObjects::SelectionInfo* si = m_c->selection_info()) {
            const CutConnectors& connectors = si->model_object()->cut_connectors;
            for (int i = 0; i < int(connectors.size()); ++i)
                m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, i + m_connectors_group_id, *(m_shapes[connectors[i].attribs]).mesh_raycaster, Transform3d::Identity()));
        }
    }
    else if (!cut_line_processing()) {
        m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, X, *m_cone.mesh_raycaster, Transform3d::Identity()));
        m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, X, *m_cone.mesh_raycaster, Transform3d::Identity()));

        m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, Y, *m_cone.mesh_raycaster, Transform3d::Identity()));
        m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, Y, *m_cone.mesh_raycaster, Transform3d::Identity()));

        m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, Z, *m_sphere.mesh_raycaster, Transform3d::Identity()));

        m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::FallbackGizmo, CutPlane, *m_plane.mesh_raycaster, Transform3d::Identity()));

        if (CutMode(m_mode) == CutMode::cutTongueAndGroove) {
            m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CutPlaneZRotation, *m_sphere.mesh_raycaster, Transform3d::Identity()));
            m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CutPlaneZRotation, *m_cone.mesh_raycaster, Transform3d::Identity()));
            m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CutPlaneZRotation, *m_cone.mesh_raycaster, Transform3d::Identity()));

            m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CutPlaneXMove, *m_cube.mesh_raycaster, Transform3d::Identity()));
            m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CutPlaneXMove, *m_cone.mesh_raycaster, Transform3d::Identity()));

            m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CutPlaneYMove, *m_cube.mesh_raycaster, Transform3d::Identity()));
            m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CutPlaneYMove, *m_cone.mesh_raycaster, Transform3d::Identity()));
        }
    }

    update_raycasters_for_picking_transform();
}

void GLGizmoCut3D::on_unregister_raycasters_for_picking()
{
    m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo);
    m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::FallbackGizmo);
    m_raycasters.clear();
    // the gizmo grabbers are rendered on top of the scene, so the raytraced picker should take it into account
    m_parent.set_raycaster_gizmos_on_top(false);
}

void GLGizmoCut3D::update_raycasters_for_picking()
{
    on_unregister_raycasters_for_picking();
    on_register_raycasters_for_picking();
}

void GLGizmoCut3D::set_volumes_picking_state(bool state)
{
    std::vector<std::shared_ptr<SceneRaycasterItem>>* raycasters = m_parent.get_raycasters_for_picking(SceneRaycaster::EType::Volume);
    if (raycasters != nullptr) {
        const Selection& selection = m_parent.get_selection();
        const Selection::IndicesList ids = selection.get_volume_idxs();
        for (unsigned int id : ids) {
            const GLVolume* v = selection.get_volume(id);
            auto it = std::find_if(raycasters->begin(), raycasters->end(), [v](std::shared_ptr<SceneRaycasterItem> item) { return item->get_raycaster() == v->mesh_raycaster.get(); });
            if (it != raycasters->end())
                (*it)->set_active(state);
        }
    }
}

void GLGizmoCut3D::update_raycasters_for_picking_transform()
{
    if (m_connectors_editing) {
        CommonGizmosDataObjects::SelectionInfo* si = m_c->selection_info();
        if (!si) 
            return;
        const ModelObject* mo = si->model_object();
        const CutConnectors& connectors = mo->cut_connectors;
        if (connectors.empty())
            return;
        auto inst_id = m_c->selection_info()->get_active_instance();
        if (inst_id < 0)
            return;

        const Vec3d& instance_offset = mo->instances[inst_id]->get_offset();
        const double sla_shift = double(m_c->selection_info()->get_sla_shift());

        const bool looking_forward = is_looking_forward();

        for (size_t i = 0; i < connectors.size(); ++i) {
            const CutConnector& connector = connectors[i];

            float height = connector.height;
            // recalculate connector position to world position
            Vec3d pos = connector.pos + instance_offset;
            if (connector.attribs.type == CutConnectorType::Dowel &&
                connector.attribs.style == CutConnectorStyle::Prism) {
                height = 0.05f;
                if (!looking_forward)
                    pos += 0.05 * m_clp_normal;
            }
            pos[Z] += sla_shift;

            const Transform3d scale_trafo = scale_transform(Vec3f(connector.radius, connector.radius, height).cast<double>());
            // PHASE 4: on a curved cut the connector stands on the SHEET, so it is
            // picked where it is drawn - at the sheet point, on the sheet's frame.
            m_raycasters[i]->set_transform(translation_transform(pos) * connector_rotation_m(connector.pos) * scale_trafo);
        }
    }
    else if (!cut_line_processing()){
        const Transform3d trafo = translation_transform(m_plane_center) * m_rotation_m;

        const BoundingBoxf3 box = m_bounding_box;

        const double size = get_half_size(get_grabber_mean_size(box));
        Vec3d scale = Vec3d(0.75 * size, 0.75 * size, 1.8 * size);

        int id = 0;

        Vec3d offset = Vec3d(0.0, 1.25 * size, m_grabber_connection_len);
        m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * rotation_transform(-0.5 * PI * Vec3d::UnitX()) * scale_transform(scale));
        offset = Vec3d(0.0, -1.25 * size, m_grabber_connection_len);
        m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * rotation_transform(0.5 * PI * Vec3d::UnitX()) * scale_transform(scale));

        offset = Vec3d(1.25 * size, 0.0, m_grabber_connection_len);
        m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * rotation_transform(0.5 * PI * Vec3d::UnitY()) * scale_transform(scale));
        offset = Vec3d(-1.25 * size, 0.0, m_grabber_connection_len);
        m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * rotation_transform(-0.5 * PI * Vec3d::UnitY()) * scale_transform(scale));

        m_raycasters[id++]->set_transform(trafo * translation_transform(m_grabber_connection_len * Vec3d::UnitZ()) * scale_transform(size));

        m_raycasters[id++]->set_transform(trafo);

        if (CutMode(m_mode) == CutMode::cutTongueAndGroove) {

            double grabber_y_shift = -1.75 * m_grabber_connection_len;

            m_raycasters[id++]->set_transform(trafo * translation_transform(grabber_y_shift * Vec3d::UnitY()) * scale_transform(size));

            offset = Vec3d(1.25 * size, grabber_y_shift, 0.0);
            m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * rotation_transform(0.5 * PI * Vec3d::UnitY()) * scale_transform(scale));
            offset = Vec3d(-1.25 * size, grabber_y_shift, 0.0);
            m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * rotation_transform(-0.5 * PI * Vec3d::UnitY()) * scale_transform(scale));

            const double xy_connection_len = 0.75 * m_grabber_connection_len;
            const Vec3d cone_scale = Vec3d(0.5 * size, 0.5 * size, 1.8 * size);

            offset = xy_connection_len * Vec3d::UnitX() - 0.5 * size * Vec3d::Ones();
            m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * scale_transform(size));
            offset = (size + xy_connection_len) * Vec3d::UnitX();
            m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * rotation_transform(0.5 * PI * Vec3d::UnitY()) * scale_transform(cone_scale));

            if (m_groove.angle > 0.0f) {
                offset = xy_connection_len * Vec3d::UnitY() - 0.5 * size * Vec3d::Ones();
                m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * scale_transform(size));
                offset = (size + xy_connection_len) * Vec3d::UnitY();
                m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * rotation_transform(-0.5 * PI * Vec3d::UnitX()) * scale_transform(cone_scale));
            }
            else {
                // discard transformation for CutPlaneYMove grabbers
                m_raycasters[id++]->set_transform(Transform3d::Identity());
                m_raycasters[id++]->set_transform(Transform3d::Identity());
            }
        }
    }
}

void GLGizmoCut3D::update_plane_model()
{
    m_plane.reset();
    on_unregister_raycasters_for_picking();

    init_picking_models();
}

void GLGizmoCut3D::on_set_hover_id() 
{
}

bool GLGizmoCut3D::on_is_activable() const
{
    const Selection& selection = m_parent.get_selection();
    const int object_idx = selection.get_object_idx();
    if (object_idx < 0 || selection.is_wipe_tower())
        return false;

    if (const ModelObject* mo = wxGetApp().plater()->model().objects[object_idx];
        mo->is_cut() && mo->volumes.size() == 1) {
        const ModelVolume* volume = mo->volumes[0];
        if (volume->is_cut_connector() && volume->cut_info.connector_type == CutConnectorType::Dowel)
            return false;
    }

    // This is assumed in GLCanvas3D::do_rotate, do not change this
    // without updating that function too.
    return selection.is_single_full_instance() && !m_parent.is_layers_editing_enabled();
}

bool GLGizmoCut3D::on_is_selectable() const
{
    // Always on the toolbar, as in BambuStudio: new installs start in Simple mode, and hiding
    // Cut there left users unable to find it without first switching to Advanced.
    return true;
}

Vec3d GLGizmoCut3D::mouse_position_in_local_plane(GrabberID axis, const Linef3& mouse_ray) const
{
    double half_pi = 0.5 * PI;

    Transform3d m = Transform3d::Identity();

    switch (axis)
    {
    case X:
    {
        m.rotate(Eigen::AngleAxisd(half_pi, Vec3d::UnitZ()));
        m.rotate(Eigen::AngleAxisd(-half_pi, Vec3d::UnitY()));
        break;
    }
    case Y:
    {
        m.rotate(Eigen::AngleAxisd(half_pi, Vec3d::UnitY()));
        m.rotate(Eigen::AngleAxisd(half_pi, Vec3d::UnitZ()));
        break;
    }
    case Z:
    default:
    {
        // no rotation applied
        break;
    }
    }

    m = m * m_start_dragging_m.inverse();
    m.translate(-m_plane_center);

    return transform(mouse_ray, m).intersect_plane(0.0);
}

void GLGizmoCut3D::dragging_grabber_move(const GLGizmoBase::UpdateData &data)
{
    Vec3d starting_drag_position;
    if (m_hover_id == Z)
        starting_drag_position = translation_transform(m_plane_center) * m_rotation_m * (m_grabber_connection_len * Vec3d::UnitZ());
    else
        starting_drag_position = m_cut_plane_start_move_pos;

    double projection  = 0.0;

    Vec3d starting_vec = m_rotation_m * (m_hover_id == CutPlaneXMove ? Vec3d::UnitX() : m_hover_id == CutPlaneYMove ? Vec3d::UnitY() : Vec3d::UnitZ());
    if (starting_vec.norm() != 0.0) {
        const Vec3d mouse_dir = data.mouse_ray.unit_vector();
        // finds the intersection of the mouse ray with the plane parallel to the camera viewport and passing through the starting position
        // use ray-plane intersection see i.e. https://en.wikipedia.org/wiki/Line%E2%80%93plane_intersection algebraic form
        // in our case plane normal and ray direction are the same (orthogonal view)
        // when moving to perspective camera the negative z unit axis of the camera needs to be transformed in world space and used as plane normal
        const Vec3d inters = data.mouse_ray.a + (starting_drag_position - data.mouse_ray.a).dot(mouse_dir) * mouse_dir;
        // vector from the starting position to the found intersection
        const Vec3d inters_vec = inters - starting_drag_position;

        starting_vec.normalize();
        // finds projection of the vector along the staring direction
        projection = inters_vec.dot(starting_vec);
    }
    if (wxGetKeyState(WXK_SHIFT))
        projection = m_snap_step * std::round(projection / m_snap_step);

    const Vec3d shift = starting_vec * projection;
    if (shift != Vec3d::Zero())
        reset_cut_by_contours();

    // move  cut plane center
    set_center(m_plane_center + shift, true);

    m_was_cut_plane_dragged = true;
}

void GLGizmoCut3D::dragging_grabber_rotation(const GLGizmoBase::UpdateData &data)
{
    const Vec2d mouse_pos = to_2d(mouse_position_in_local_plane((GrabberID)m_hover_id, data.mouse_ray));

    const Vec2d orig_dir = Vec2d::UnitX();
    const Vec2d new_dir  = mouse_pos.normalized();

    const double two_pi = 2.0 * PI;

    double theta = ::acos(std::clamp(new_dir.dot(orig_dir), -1.0, 1.0));
    if (cross2(orig_dir, new_dir) < 0.0)
        theta = two_pi - theta;

    const double len = mouse_pos.norm();
    // snap to coarse snap region
    if (m_snap_coarse_in_radius <= len && len <= m_snap_coarse_out_radius) {
        const double step = two_pi / double(SnapRegionsCount);
        theta             = step * std::round(theta / step);
    }
    // snap to fine snap region (scale)
    else if (m_snap_fine_in_radius <= len && len <= m_snap_fine_out_radius) {
        const double step = two_pi / double(ScaleStepsCount);
        theta             = step * std::round(theta / step);
    }

    if (is_approx(theta, two_pi))
        theta = 0.0;
    if (m_hover_id != Y)
        theta += 0.5 * PI;

    if (!is_approx(theta, 0.0))
        reset_cut_by_contours();

    Vec3d rotation = Vec3d::Zero();
    rotation[m_hover_id == CutPlaneZRotation ? Z : m_hover_id] = theta;

    const Transform3d rotation_tmp = m_start_dragging_m * rotation_transform(rotation);
    const bool update_tbb = !m_rotation_m.rotation().isApprox(rotation_tmp.rotation());
    m_rotation_m = rotation_tmp;
    if (update_tbb)
        m_transformed_bounding_box = transformed_bounding_box(m_plane_center, m_rotation_m);

    m_angle = theta;
    while (m_angle > two_pi)
        m_angle -= two_pi;
    if (m_angle < 0.0)
        m_angle += two_pi;

    update_clipper();
}

void GLGizmoCut3D::dragging_connector(const GLGizmoBase::UpdateData &data)
{
    CutConnectors&          connectors = m_c->selection_info()->model_object()->cut_connectors;
    Vec3d                   pos;
    Vec3d                   pos_world;

    // PHASE 4: dragging a connector SLIDES it on the sheet - the ray is re-hit
    // against the surface every motion, so the connector rides the curve rather
    // than sliding on the flat plane and then popping back onto the sheet.
    if (unproject_on_curved_sheet(data.mouse_pos.cast<double>(), pos, pos_world)) {
        CutConnector& c = connectors[m_hover_id - m_connectors_group_id];
        c.pos        = pos;
        c.rotation_m = connector_rotation_m(pos);
        update_raycasters_for_picking_transform();
    }
}

void GLGizmoCut3D::on_dragging(const UpdateData& data)
{
    if (m_hover_id < 0)
        return;
    if (m_hover_id == Z || m_hover_id == CutPlane || m_hover_id == CutPlaneXMove || m_hover_id == CutPlaneYMove)
        dragging_grabber_move(data);
    else if (m_hover_id == X || m_hover_id == Y || m_hover_id == CutPlaneZRotation)
        dragging_grabber_rotation(data);
    else if (m_hover_id >= m_connectors_group_id && m_connector_mode == CutConnectorMode::Manual)
        dragging_connector(data);
    check_and_update_connectors_state();

    // The cross-section fit is DEBOUNCED to the end of the drag: it walks the
    // instance mesh, so paying for it on every frame of a plane drag would stall
    // the drag on any real model. Note the intent here and honour it in
    // on_stop_dragging().
    const bool plane_gesture = m_hover_id == Z || m_hover_id == CutPlane || m_hover_id == CutPlaneXMove ||
                               m_hover_id == CutPlaneYMove || m_hover_id == X || m_hover_id == Y ||
                               m_hover_id == CutPlaneZRotation;
    if (m_surface_mode == CutSurfaceMode::Curved && plane_gesture)
        request_curved_fit();
    // DRAW: the cached instance mesh is in the plane's frame, so a plane gesture
    // makes it stale. Drop it rather than re-deriving here - the fit's own reason
    // for being debounced (it walks the instance mesh, and a drag must not pay for
    // that every frame) applies just as much to this.
    if (m_surface_mode == CutSurfaceMode::Draw && plane_gesture)
        invalidate_draw_pick_mesh();

    if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
        reset_cut_by_contours();
}

void GLGizmoCut3D::on_start_dragging()
{
    m_angle = 0.0;
    if (m_hover_id >= m_connectors_group_id && m_connector_mode == CutConnectorMode::Manual)
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Move connector"), UndoRedo::SnapshotType::GizmoAction);

    if (m_hover_id == X || m_hover_id == Y || m_hover_id == CutPlaneZRotation)
        m_start_dragging_m = m_rotation_m;
}

void GLGizmoCut3D::on_stop_dragging()
{
    if (m_hover_id == X || m_hover_id == Y || m_hover_id == CutPlaneZRotation) {
        m_angle_arc.reset();
        m_angle = 0.0;
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Rotate cut plane"), UndoRedo::SnapshotType::GizmoAction);
        m_start_dragging_m = m_rotation_m;
    }
    else if (m_hover_id == Z || m_hover_id == CutPlane || m_hover_id == CutPlaneXMove|| m_hover_id == CutPlaneYMove) {
        if (m_was_cut_plane_dragged)
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Move cut plane"), UndoRedo::SnapshotType::GizmoAction);
        m_ar_plane_center = m_plane_center;
    }

    // The drag is over, so pay for the re-fit now. It re-samples the surface
    // onto the new rectangle, so a bend the user drew stays where it is in the
    // plane rather than sliding or stretching with the new extent.
    if (m_curved_fit_pending) {
        m_curved_fit_pending = false;
        fit_curved_sheet_to_section();
    }

    if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
        reset_cut_by_contours();
    //check_and_update_connectors_state();
}

void GLGizmoCut3D::set_center_pos(const Vec3d& center_pos, bool update_tbb /*=false*/)
{
    BoundingBoxf3 tbb = m_transformed_bounding_box;
    if (update_tbb) {
        Vec3d normal = m_rotation_m.inverse() * Vec3d(m_plane_center - center_pos);
        tbb.translate(normal.z() * Vec3d::UnitZ());
    }

    bool can_set_center_pos = false;
    {
        double limit_val = /*CutMode(m_mode) == CutMode::cutTongueAndGroove ? 0.5 * double(m_groove.depth) : */0.5;
        // The rule itself lives in libslic3r so it can be tested without a GUI -
        // see cut_plane_center_allowed(). Same arithmetic as before.
        if (cut_plane_center_allowed(tbb.min.z(), tbb.max.z(), limit_val))
            can_set_center_pos = true;
        else {
            const double old_dist = (m_bb_center - m_plane_center).norm();
            const double new_dist = (m_bb_center - center_pos).norm();
            // check if forcing is reasonable
            if (new_dist < old_dist)
                can_set_center_pos = true;
        }
    }

    if (can_set_center_pos) {
        m_transformed_bounding_box = tbb;
        m_plane_center = center_pos;
        m_center_offset = m_plane_center - m_bb_center;
    }
}

BoundingBoxf3 GLGizmoCut3D::bounding_box() const
{
    BoundingBoxf3 ret;
    const Selection& selection = m_parent.get_selection();
    const Selection::IndicesList& idxs = selection.get_volume_idxs();
    for (unsigned int i : idxs) {
        const GLVolume* volume = selection.get_volume(i);
        // respect just to the solid parts for FFF and ignore pad and supports for SLA
        if (!volume->is_modifier && !volume->is_sla_pad() && !volume->is_sla_support())
            ret.merge(volume->transformed_convex_hull_bounding_box());
    }
    return ret;
}

BoundingBoxf3 GLGizmoCut3D::transformed_bounding_box(const Vec3d& plane_center, const Transform3d& rotation_m/* = Transform3d::Identity()*/) const
{
    const Selection& selection = m_parent.get_selection();

    const auto first_volume = selection.get_first_volume();
    Vec3d instance_offset   = first_volume->get_instance_offset();
    instance_offset[Z]     += first_volume->get_sla_shift_z();

    const auto cut_matrix = Transform3d::Identity() * rotation_m.inverse() * translation_transform(instance_offset - plane_center);

    const Selection::IndicesList& idxs = selection.get_volume_idxs();
    BoundingBoxf3 ret;
    for (unsigned int i : idxs) {
        const GLVolume* volume = selection.get_volume(i);
        // respect just to the solid parts for FFF and ignore pad and supports for SLA
        if (!volume->is_modifier && !volume->is_sla_pad() && !volume->is_sla_support()) {

            const auto instance_matrix = volume->get_instance_transformation().get_matrix_no_offset();
            auto volume_trafo = instance_matrix * volume->get_volume_transformation().get_matrix();
            ret.merge(volume->transformed_convex_hull_bounding_box(cut_matrix * volume_trafo));
        }
    }
    return ret;
}

void GLGizmoCut3D::update_bb()
{
    const BoundingBoxf3 box = bounding_box();
    if (!box.defined)
        return;
    if (!m_max_pos.isApprox(box.max) || !m_min_pos.isApprox(box.min)) {

        m_bounding_box = box;

        // check, if mode is set to Planar, when object has a connectors
        if (const int object_idx = m_parent.get_selection().get_object_idx();
            object_idx >= 0 && !wxGetApp().plater()->model().objects[object_idx]->cut_connectors.empty())
            m_mode = size_t(CutMode::cutPlanar);

        invalidate_cut_plane();
        reset_cut_by_contours();
        apply_color_clip_plane_colors();

        m_max_pos = box.max;
        m_min_pos = box.min;
        m_bb_center = box.center();
        m_transformed_bounding_box = transformed_bounding_box(m_bb_center);
        if (box.contains(m_center_offset))
            set_center_pos(m_bb_center + m_center_offset);
        else
            set_center_pos(m_bb_center);

        m_contour_width = CutMode(m_mode) == CutMode::cutTongueAndGroove ? 0.f : 0.4f;

        m_radius = box.radius();
        m_grabber_connection_len = 0.5 * m_radius;// std::min<double>(0.75 * m_radius, 35.0);
        m_grabber_radius = m_grabber_connection_len * 0.85;

        m_snap_coarse_in_radius   = m_grabber_radius / 3.0;
        m_snap_coarse_out_radius  = m_snap_coarse_in_radius * 2.;
        m_snap_fine_in_radius     = m_grabber_connection_len * 0.85;
        m_snap_fine_out_radius    = m_grabber_connection_len * 1.15;

        // input params for cut with tongue and groove
        m_groove.depth = m_groove.depth_init = std::max(1.f , 0.5f * float(get_grabber_mean_size(m_bounding_box)));
        m_groove.width = m_groove.width_init = 4.0f * m_groove.depth;
        m_groove.flaps_angle = m_groove.flaps_angle_init = float(PI) / 3.f;
        m_groove.angle = m_groove.angle_init = 0.f;
        // A new bounding box means a new object (or a new instance), so the
        // fallback extent and the cross-section fit both have to be redone -
        // the control grid itself is kept.
        invalidate_curved_sheet();
        m_curved_fit_valid = false;
        m_curved_sheet.set_half_size(curved_sheet_half_size());
        m_curved_hover_ctl = m_curved_drag_ctl = -1;
        m_plane.reset();
        m_cone.reset();
        m_sphere.reset();
        m_cube.reset();
        m_grabber_connection.reset();
        m_circle.reset();
        m_scale.reset();
        m_snap_radii.reset();
        m_reference_radius.reset();

        on_unregister_raycasters_for_picking();

        clear_selection();
        if (CommonGizmosDataObjects::SelectionInfo* selection = m_c->selection_info();
            selection && selection->model_object())
            m_selected.resize(selection->model_object()->cut_connectors.size(), false);
    }
}

void GLGizmoCut3D::init_picking_models()
{
    if (!m_cone.model.is_initialized()) {
        indexed_triangle_set its = its_make_cone(1.0, 1.0, PI / 12.0);
        m_cone.model.init_from(its);
        m_cone.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }
    if (!m_sphere.model.is_initialized()) {
        indexed_triangle_set its = its_make_sphere(1.0, PI / 12.0);
        m_sphere.model.init_from(its);
        m_sphere.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }
    if (!m_cube.model.is_initialized()) {
        indexed_triangle_set its = its_make_cube(1., 1., 1.);
        m_cube.model.init_from(its);
        m_cube.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }

    if (!m_plane.model.is_initialized() && !m_hide_cut_plane && !m_connectors_editing) {
        const double cp_width = 0.02 * get_grabber_mean_size(m_bounding_box);
        indexed_triangle_set its = m_mode == size_t(CutMode::cutTongueAndGroove) ? its_make_groove_plane() :
                                   its_make_frustum_dowel((double)m_cut_plane_radius_koef * m_radius, cp_width, m_cut_plane_as_circle ? 180 : 4);

        m_plane.model.init_from(its);
        m_plane.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }

    if (m_shapes.empty())
        init_connector_shapes();
}

void GLGizmoCut3D::init_rendering_items()
{
    if (!m_grabber_connection.is_initialized())
        m_grabber_connection.init_from(its_make_line(Vec3f::Zero(), Vec3f::UnitZ()));
    if (!m_circle.is_initialized())
        init_from_circle(m_circle, m_grabber_radius);
    if (!m_scale.is_initialized())
        init_from_scale(m_scale, m_grabber_radius);
    if (!m_snap_radii.is_initialized())
        init_from_snap_radii(m_snap_radii, m_grabber_radius);
    if (!m_reference_radius.is_initialized()) {
        m_reference_radius.init_from(its_make_line(Vec3f::Zero(), m_grabber_connection_len * Vec3f::UnitX()));
        m_reference_radius.set_color(ColorRGBA::WHITE());
    }
    if (!m_angle_arc.is_initialized() || m_angle != 0.0)
        init_from_angle_arc(m_angle_arc, m_angle, m_grabber_connection_len);
}

void GLGizmoCut3D::render_clipper_cut()
{
    // A bent sheet gets its cut face from render_curved_cap(): MeshClipper
    // slices at a single z, so the clipper's cap is flat by construction and
    // drawing it here would put a flat disc through the curved one.
    if (is_curved_surface() && !m_curved_sheet.is_flat() && !m_connectors_editing) {
        render_curved_cap();
        return;
    }

    // DRAW: MeshClipper's cap is a slice at one z, so on a drawn cut it would put
    // a flat disc through the ruled surface, in a place the cut does not go. The
    // drawn surface's own preview is the translucent cutter shell
    // render_draw_stroke() puts up, which IS the surface the boolean will use, so
    // there is nothing to draw here.
    //
    // 2026-09-20, OWNER REPORT item 4: the carve-out for connectors editing went
    // back to the flat cap + contour whenever "Add connectors" was picked on a
    // drawn cut - the one moment the flat outline is guaranteed to be wrong. The
    // draw surface is the substrate there too (placement raycasts it), so in Draw
    // mode the clipper never renders, connectors editing or not.
    if (is_draw_surface())
        return;

    if (! m_connectors_editing)
        ::glDisable(GL_DEPTH_TEST);

    GLboolean cull_face = GL_FALSE;
    ::glGetBooleanv(GL_CULL_FACE, &cull_face);
    ::glDisable(GL_CULL_FACE);
    m_c->object_clipper()->render_cut(m_part_selection.get_ignored_contours_ptr());
    if (cull_face)
        ::glEnable(GL_CULL_FACE);

    if (! m_connectors_editing)
        ::glEnable(GL_DEPTH_TEST);
}

void GLGizmoCut3D::PartSelection::add_object(const ModelObject* object)
{
    m_model = Model();
    m_model.add_object(*object);

    const double sla_shift_z = wxGetApp().plater()->canvas3D()->get_selection().get_first_volume()->get_sla_shift_z();
    if (!is_approx(sla_shift_z, 0.)) {
        Vec3d inst_offset = model_object()->instances[m_instance_idx]->get_offset();
        inst_offset[Z] += sla_shift_z;
        model_object()->instances[m_instance_idx]->set_offset(inst_offset);
    }
}


GLGizmoCut3D::PartSelection::PartSelection(const ModelObject* mo, const Transform3d& cut_matrix, int instance_idx_in, const Vec3d& center, const Vec3d& normal, const CommonGizmosDataObjects::ObjectClipper& oc)
    : m_instance_idx(instance_idx_in)
{
    Cut cut(mo, instance_idx_in, cut_matrix);
    add_object(cut.perform_with_plane().front());

    const ModelVolumePtrs& volumes = model_object()->volumes;

    // split to parts
    for (int id = int(volumes.size())-1; id >= 0; id--)
        if (volumes[id]->is_splittable())
            volumes[id]->split(1);

    m_parts.clear();
    for (const ModelVolume* volume : volumes) {
        assert(volume != nullptr);
        m_parts.emplace_back(Part{GLModel(), MeshRaycaster(volume->mesh()), true, !volume->is_model_part()});
        m_parts.back().glmodel.set_color({ 0.f, 0.f, 1.f, 1.f });
        m_parts.back().glmodel.init_from(volume->mesh());

        // Now check whether this part is below or above the plane.
        Transform3d tr = (model_object()->instances[m_instance_idx]->get_matrix() * volume->get_matrix()).inverse();
        Vec3f pos = (tr * center).cast<float>();
        Vec3f norm = (tr.linear().inverse().transpose() * normal).cast<float>();
        for (const Vec3f& v : volume->mesh().its.vertices) {
            double p = (v - pos).dot(norm);
            if (std::abs(p) > EPSILON) {
                m_parts.back().selected = p > 0.;
                break;
            }
        }
    }

    // Now go through the contours and create a map from contours to parts.
    m_contour_points.clear();
    m_contour_to_parts.clear();
    m_debug_pts = std::vector<std::vector<Vec3d>>(m_parts.size(), std::vector<Vec3d>());
    if (std::vector<Vec3d> pts = oc.point_per_contour();! pts.empty()) {
        
        m_contour_to_parts.resize(pts.size());

        for (size_t pt_idx=0; pt_idx<pts.size(); ++pt_idx) {
            const Vec3d& pt = pts[pt_idx];
            const Vec3d dir = (center-pt).dot(normal) * normal;
            m_contour_points.emplace_back(dir + pt); // the result is in world coordinates.
            
            // Now, cast a ray from every contour point and see which volumes of the ones above
            // the plane are hit from the inside.
            for (size_t part_id=0; part_id<m_parts.size(); ++part_id) {
                const AABBMesh& aabb = m_parts[part_id].raycaster.get_aabb_mesh();
                const Transform3d& tr = (translation_transform(model_object()->instances[m_instance_idx]->get_offset()) * translation_transform(model_object()->volumes[part_id]->get_offset())).inverse();
                for (double d : {-1., 1.}) {
                    const Vec3d dir_mesh = d * tr.linear().inverse().transpose() * normal;
                    const Vec3d src = tr * (m_contour_points[pt_idx] + d*0.01 * normal);
                    AABBMesh::hit_result hit = aabb.query_ray_hit(src, dir_mesh);

                    m_debug_pts[part_id].emplace_back(src);

                    if (hit.is_inside()) {
                        // This part belongs to this point.
                        if (d == 1.)
                            m_contour_to_parts[pt_idx].first.emplace_back(part_id);
                        else
                            m_contour_to_parts[pt_idx].second.emplace_back(part_id);
                    }
                }
            }
        }

    }

    
    m_valid = true;
}

// In CutMode::cutTongueAndGroove we use PartSelection just for rendering
GLGizmoCut3D::PartSelection::PartSelection(const ModelObject* object, int instance_idx_in)
    : m_instance_idx (instance_idx_in)
{
    add_object(object);

    m_parts.clear();

    for (const ModelVolume* volume : object->volumes) {
        assert(volume != nullptr);
        m_parts.emplace_back(Part{ GLModel(), MeshRaycaster(volume->mesh()), true, !volume->is_model_part() });
        m_parts.back().glmodel.init_from(volume->mesh());

        // Now check whether this part is below or above the plane.
        m_parts.back().selected = volume->is_from_upper();
    }
    
    m_valid = true;
}

void GLGizmoCut3D::PartSelection::render(const Vec3d* normal, GLModel& sphere_model)
{
    if (! valid())
        return;

    const Camera&       camera          = wxGetApp().plater()->get_camera();

    if (GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light")) {
        shader->start_using();
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("emission_factor", 0.f);

        // FIXME: Cache the transforms.

        const Vec3d         inst_offset     = model_object()->instances[m_instance_idx]->get_offset();
        const Transform3d   view_inst_matrix= camera.get_view_matrix() * translation_transform(inst_offset);

        const bool is_looking_forward = normal && camera.get_dir_forward().dot(*normal) < 0.05;

        for (size_t id=0; id<m_parts.size(); ++id) {
            if (!m_parts[id].is_modifier && normal && ((is_looking_forward && m_parts[id].selected) ||
                                                      (!is_looking_forward && !m_parts[id].selected)   ) )
                continue;
            shader->set_uniform("view_model_matrix", view_inst_matrix * model_object()->volumes[id]->get_matrix());
            if (m_parts[id].is_modifier) {
                glsafe(::glEnable(GL_BLEND));
                glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            }
            m_parts[id].glmodel.set_color(m_parts[id].is_modifier ? MODIFIER_COLOR : (m_parts[id].selected ? UPPER_PART_COLOR : LOWER_PART_COLOR));
            m_parts[id].glmodel.render();
            if (m_parts[id].is_modifier)
                glsafe(::glDisable(GL_BLEND));
        }

        shader->stop_using();
    }



    // { // Debugging render:

    //     static int idx = -1;
    //     ImGui::Begin("DEBUG");
    //     for (int i=0; i<m_parts.size(); ++i)
    //         if (ImGui::Button(std::to_string(i).c_str()))
    //             idx = i;
    //     if (idx >= m_parts.size())
    //         idx = -1;
    //     ImGui::End();

    //     ::glDisable(GL_DEPTH_TEST);
    //     if (valid()) {
    //         for (size_t i=0; i<m_contour_points.size(); ++i) {
    //             const Vec3d& pt = m_contour_points[i];
    //             ColorRGBA col = ColorRGBA::GREEN();
            
    //             bool red = false;
    //             bool yellow = false;
    //             for (size_t j=0; j<m_contour_to_parts[i].first.size(); ++j) {
    //                 red |= m_parts[m_contour_to_parts[i].first[j]].selected;
    //                 yellow |= m_parts[m_contour_to_parts[i].second[j]].selected;
    //             }
    //             if (red)
    //                 col = ColorRGBA::RED();
    //             if (yellow)
    //                 col = ColorRGBA::YELLOW();
                    
    //             GLGizmoCut3D::render_model(sphere_model, col, camera.get_view_matrix() * translation_transform(pt));
    //         }
    //     }
        
    //     if (idx != -1) {
    //         render_model(m_parts[idx].glmodel, ColorRGBA::RED(), camera.get_view_matrix());
    //         for (const Vec3d& pt : m_debug_pts[idx]) {
    //             render_model(sphere_model, ColorRGBA::GREEN(), camera.get_view_matrix() * translation_transform(pt));
    //         }
    //     }
    //     ::glEnable(GL_DEPTH_TEST);
    // }
}


bool GLGizmoCut3D::PartSelection::is_one_object() const
{
    // In theory, the implementation could be just this:
    // return m_contour_to_parts.size() == m_ignored_contours.size();
    // However, this would require that the part-contour correspondence works
    // flawlessly. Because it is currently not always so for self-intersecting
    // objects, let's better check the parts itself:
    if (m_parts.size() < 2)
        return true;
    return std::all_of(m_parts.begin(), m_parts.end(), [this](const Part& part) {
        return part.is_modifier || part.selected == m_parts.front().selected;
    });
}

std::vector<Cut::Part> GLGizmoCut3D::PartSelection::get_cut_parts()
{
    std::vector<Cut::Part> parts;

    for (const auto& part : m_parts)
        parts.push_back({part.selected, part.is_modifier});

    return parts;
}


void GLGizmoCut3D::PartSelection::toggle_selection(const Vec2d& mouse_pos)
{
    // FIXME: Cache the transforms.
    const Camera& camera     = wxGetApp().plater()->get_camera();
    const Vec3d&  camera_pos = camera.get_position();

    Vec3f pos;
    Vec3f normal;

    std::vector<std::pair<size_t, double>> hits_id_and_sqdist;

    for (size_t id=0; id<m_parts.size(); ++id) {
//        const Vec3d volume_offset = model_object()->volumes[id]->get_offset();
        Transform3d tr = translation_transform(model_object()->instances[m_instance_idx]->get_offset()) * translation_transform(model_object()->volumes[id]->get_offset());
        if (m_parts[id].raycaster.unproject_on_mesh(mouse_pos, tr, camera, pos, normal)) {
            hits_id_and_sqdist.emplace_back(id, (camera_pos - tr*(pos.cast<double>())).squaredNorm());
        }
    }
    if (! hits_id_and_sqdist.empty()) {
        size_t id = std::min_element(hits_id_and_sqdist.begin(), hits_id_and_sqdist.end(),
            [](const std::pair<size_t, double>& a, const std::pair<size_t, double>& b) { return a.second < b.second; })->first;
        m_parts[id].selected = ! m_parts[id].selected;

        // And now recalculate the contours which should be ignored.
        m_ignored_contours.clear();
        size_t cont_id = 0;
        for (const auto& [parts_above, parts_below] : m_contour_to_parts) {
            for (size_t upper : parts_above) {
                bool upper_sel = m_parts[upper].selected;
                if (std::find_if(parts_below.begin(), parts_below.end(), [this, &upper_sel](const size_t& i) { return m_parts[i].selected == upper_sel; }) != parts_below.end()) {
                    m_ignored_contours.emplace_back(cont_id);
                    break;
                }
            }
            ++cont_id;
        }
    }
}

void GLGizmoCut3D::PartSelection::turn_over_selection()
{
    for (Part& part : m_parts)
        part.selected = !part.selected;
}

void GLGizmoCut3D::on_render()
{
    if (m_state == On) {
        // This gizmo is showing the object elevated. Tell the common
        // SelectionInfo object to lie about the actual shift.
        //m_c->selection_info()->set_use_shift(true);
    }

    // check objects visibility
    toggle_model_objects_visibility();

    update_clipper();

    init_picking_models();

    init_rendering_items();

    render_connectors();

    if (m_facet_picker.is_active() && !m_connectors_editing)
        m_facet_picker.render(wxGetApp().plater()->get_camera());

    if (!m_connectors_editing)
        m_part_selection.render(nullptr, m_sphere.model);
    else
        m_part_selection.render(&m_cut_normal, m_sphere.model);

    render_clipper_cut();

    // 2026-09-20, OWNER REPORT item 4: connectors editing hides the surface so the
    // flat plane can stand in - on a DRAWN cut that both removes the only visual
    // substrate a connector lands on and (with the clipper skip above) leaves
    // nothing in its place. The draw stroke and its shell stay up while connectors
    // are placed; the plane grabbers stay hidden exactly as before.
    if (!m_hide_cut_plane && (!m_connectors_editing || is_draw_surface())) {
        // Curved surface: the deformed sheet stands in for the flat plane and
        // its control handles are drawn on top. The plane's own rotate and
        // translate grabbers stay exactly as they are, and the sheet rides on
        // the plane's frame, so moving the plane moves the sheet.
        // DRAW: the stroke and its cutter shell stand in for the plane entirely,
        // and THE PLANE GRABBERS HIDE - the drawn surface is positioned by where
        // the user drew, not by dragging a plane, and leaving the grabbers up
        // would invite a gesture that moves the stroke out from under itself.
        // (The plane's own frame still matters - the stroke lives in it - so the
        // Cut position / rotation inputs in the panel stay available.)
        if (is_draw_surface())
            render_draw_stroke();
        else {
            if (is_curved_surface())
                render_curved_sheet();
            else
                render_cut_plane();
            render_cut_plane_grabbers();
            if (is_curved_surface())
                render_curved_control_points();
        }
    }

    render_cut_line();

    m_selection_rectangle.render(m_parent);
}

void GLGizmoCut3D::render_debug_input_window(float x)
{
    return;
    m_imgui->begin(wxString("DEBUG"));

    m_imgui->end();
/*
    static bool  hide_clipped  = false;
    static bool  fill_cut      = false;
    static float contour_width = 0.4f;

    m_imgui->checkbox(_L("Hide cut plane and grabbers"), m_hide_cut_plane);
    if (m_imgui->checkbox("hide_clipped", hide_clipped) && !hide_clipped)
        m_clp_normal = m_c->object_clipper()->get_clipping_plane()->get_normal();
    m_imgui->checkbox("fill_cut", fill_cut);
    m_imgui->slider_float("contour_width", &contour_width, 0.f, 3.f);
    if (auto oc = m_c->object_clipper())
        oc->set_behavior(hide_clipped || m_connectors_editing, fill_cut || m_connectors_editing, double(contour_width));
*/
    ImGui::PushItemWidth(0.5f * m_label_width);
    if (auto oc = m_c->object_clipper(); oc && m_imgui->slider_float("contour_width", &m_contour_width, 0.f, 3.f))
        oc->set_behavior(m_connectors_editing, m_connectors_editing, double(m_contour_width));

    ImGui::Separator();

    if (m_imgui->checkbox(("Render cut plane as disc"), m_cut_plane_as_circle))
        m_plane.reset();

    ImGui::PushItemWidth(0.5f * m_label_width);
    if (m_imgui->slider_float("cut_plane_radius_koef", &m_cut_plane_radius_koef, 1.f, 2.f))
        m_plane.reset();

    m_imgui->end();
}

void GLGizmoCut3D::unselect_all_connectors()
{
    std::fill(m_selected.begin(), m_selected.end(), false);
    m_selected_count = 0;
    validate_connector_settings();
}

void GLGizmoCut3D::select_all_connectors()
{
    std::fill(m_selected.begin(), m_selected.end(), true);
    m_selected_count = int(m_selected.size());
}

void GLGizmoCut3D::apply_selected_connectors(std::function<void(size_t idx)> apply_fn)
{
    for (size_t idx = 0; idx < m_selected.size(); idx++)
        if (m_selected[idx])
            apply_fn(idx);
    check_and_update_connectors_state();
    update_raycasters_for_picking_transform();
}

void GLGizmoCut3D::render_connectors_input_window(CutConnectors &connectors, float x, float y, float bottom_limit)
{
    // Connectors section

    ImGui::Separator();

    // WIP : Auto : Need to implement
    // m_imgui->text(_L("Mode"));
    // render_connect_mode_radio_button(CutConnectorMode::Auto);
    // render_connect_mode_radio_button(CutConnectorMode::Manual);

    ImGui::AlignTextToFramePadding();
    m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, m_labels_map["Connectors"]);

    m_imgui->disabled_begin(connectors.empty());
    ImGui::SameLine(m_label_width);
    const std::string act_name = _u8L("Remove connectors");
    if (render_reset_button("connectors", act_name)) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), act_name, UndoRedo::SnapshotType::GizmoAction);
        reset_connectors();
    }
    m_imgui->disabled_end();

    render_flip_plane_button(m_connectors_editing && connectors.empty());

    m_imgui->text(m_labels_map["Type"]);
    ImGuiWrapper::push_radio_style();
    bool type_changed = render_connect_type_radio_button(CutConnectorType::Plug);
    type_changed     |= render_connect_type_radio_button(CutConnectorType::Dowel);
    type_changed     |= render_connect_type_radio_button(CutConnectorType::Snap);
    type_changed     |= render_connect_type_radio_button(CutConnectorType::FlexiJoint);
    if (type_changed)
        apply_selected_connectors([this, &connectors] (size_t idx) { connectors[idx].attribs.type = CutConnectorType(m_connector_type); });
    ImGuiWrapper::pop_radio_style();

    if (is_flexi_joint_type()) {
        if (type_changed)
            sync_flexi_params(connectors, true);
        render_flexi_joint_inputs(connectors);
        ImGui::Separator();
        render_connectors_window_footer(x, y);
        return;
    }

    m_imgui->disabled_begin(m_connector_type != CutConnectorType::Plug);
        if (type_changed && m_connector_type == CutConnectorType::Dowel) {
            m_connector_style = int(CutConnectorStyle::Prism);
            apply_selected_connectors([this, &connectors](size_t idx) { connectors[idx].attribs.style = CutConnectorStyle(m_connector_style); });
        }
        if (render_combo(m_labels_map["Style"], m_connector_styles, m_connector_style, m_label_width, m_editing_window_width))
            apply_selected_connectors([this, &connectors](size_t idx) { connectors[idx].attribs.style = CutConnectorStyle(m_connector_style); });
    m_imgui->disabled_end();

    m_imgui->disabled_begin(m_connector_type == CutConnectorType::Snap);
        if (type_changed && m_connector_type == CutConnectorType::Snap) {
            m_connector_shape_id = int(CutConnectorShape::Circle);
            apply_selected_connectors([this, &connectors](size_t idx) { connectors[idx].attribs.shape = CutConnectorShape(m_connector_shape_id); });
        }
        if (render_combo(m_labels_map["Shape"], m_connector_shapes, m_connector_shape_id, m_label_width, m_editing_window_width))
            apply_selected_connectors([this, &connectors](size_t idx) { connectors[idx].attribs.shape = CutConnectorShape(m_connector_shape_id); });
    m_imgui->disabled_end();

    const float depth_min_value = m_connector_type == CutConnectorType::Snap ? m_connector_size : -0.1f;
    if (render_slider_double_input(m_labels_map["Depth"], m_connector_depth_ratio, m_connector_depth_ratio_tolerance, depth_min_value))
        apply_selected_connectors([this, &connectors](size_t idx) {
            if (m_connector_depth_ratio > 0)
                connectors[idx].height           = m_connector_depth_ratio;
            if (m_connector_depth_ratio_tolerance >= 0)
                connectors[idx].height_tolerance = m_connector_depth_ratio_tolerance;
        });

    if (render_slider_double_input(m_labels_map["Size"], m_connector_size, m_connector_size_tolerance))
        apply_selected_connectors([this, &connectors](size_t idx) {
            if (m_connector_size > 0)
                connectors[idx].radius           = 0.5f * m_connector_size;
            if (m_connector_size_tolerance >= 0)
                connectors[idx].radius_tolerance = 0.5f * m_connector_size_tolerance;
        });

    if (render_angle_input(m_labels_map["Rotation"], m_connector_angle, 0.f, 0.f, 180.f))
        apply_selected_connectors([this, &connectors](size_t idx) {
            connectors[idx].z_angle = m_connector_angle;
        });

    if (m_connector_type == CutConnectorType::Snap) {
        render_snap_specific_input(_u8L("Bulge"), _L("Bulge proportion related to radius"), m_snap_bulge_proportion, 0.15f, 5.f, 100.f * m_snap_space_proportion);
        render_snap_specific_input(_u8L("Space"), _L("Space proportion related to radius"), m_snap_space_proportion, 0.3f, 10.f, 50.f);
    }

    ImGui::Separator();

    render_connectors_window_footer(x, y);
}

void GLGizmoCut3D::render_connectors_window_footer(float x, float y)
{
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    show_tooltip_information(x, get_cur_y);

    float f_scale = m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    ImGui::SameLine();
    if (m_imgui->button(_L("Confirm connectors"))) {
        unselect_all_connectors();
        set_connectors_editing(false);
    }

    ImGui::SameLine(m_label_width + m_editing_window_width - m_imgui->calc_text_size(_L("Cancel")).x - m_imgui->get_style_scaling() * 8);

    if (m_imgui->button(_L("Cancel"))) {
        reset_connectors();
        set_connectors_editing(false);
    }

    ImGui::PopStyleVar(2);
}

// ------------------------------------------------------------------------ Flexi joint UI

double GLGizmoCut3D::flexi_section_inscribed_radius() const
{
    // Phase 1 approximation of "the inscribed circle of the cut cross section": half the
    // smaller side of the object's bounding box measured in the cut plane's own frame.
    // (Contour-exact inscribed circle is phase 2 - see the spec.)
    const Vec3d sz = m_transformed_bounding_box.size();
    return 0.5 * std::min(sz.x(), sz.y());
}

float GLGizmoCut3D::flexi_nozzle_diameter() const
{
    try {
        const auto* opt = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
        if (opt && !opt->values.empty())
            return float(opt->get_at(0));
    } catch (...) {}
    return 0.4f;
}

double GLGizmoCut3D::flexi_slice_closing_radius() const
{
    try {
        const auto* opt = wxGetApp().preset_bundle->prints.get_edited_preset().config.option<ConfigOptionFloat>("slice_closing_radius");
        if (opt)
            return opt->value;
    } catch (...) {}
    return 0.049;
}

// The hinge axis d in WORLD coordinates: the joint frame's +X, spun by the connector's own
// Rotation about the cut normal and then by the cut plane's orientation. This is what decides
// whether the pin prints the easy way (axis flat on the bed) or needs care.
Vec3d GLGizmoCut3D::flexi_hinge_axis_world() const
{
    // The joint frame's +X, turned inside the cut plane by the joint's own Rotation and then
    // carried into the world by the cut plane's orientation.
    const double a = double(m_flexi.rotation) * PI / 180.;
    return m_rotation_m.linear() * Vec3d(std::cos(a), std::sin(a), 0.);
}

// The cut NORMAL in world coordinates: the joint frame's +Z carried out through the cut
// plane's orientation. A thread or a bayonet wants this vertical - the mirror image of what
// the hinge's pin axis wants - so this is what the twist-lock printability warning reads.
Vec3d GLGizmoCut3D::flexi_twist_axis_world() const
{
    return m_rotation_m.linear() * Vec3d::UnitZ();
}

// Auto edge placement (spec 2.2). The barrel has to sit at the EDGE of the cut face, offset
// along -e (e = n x d), or the two halves collide the moment they start to fold: material
// behind the hinge line on each half sweeps through the other's. Phase 1 uses the object's
// bounding box measured in the cut plane's own frame - the same approximation
// flexi_section_inscribed_radius() already makes - rather than a contour-exact offset, which
// the spec defers with it.
float GLGizmoCut3D::flexi_hinge_auto_edge_offset() const
{
    // How far it is from the joint origin out to the cut face's edge along e. The connector's
    // Rotation turns the run in the plane, so the relevant half extent runs between the box's
    // x and y half sizes as the angle sweeps.
    const Vec3d  sz   = m_transformed_bounding_box.size();
    const double a    = double(m_flexi.rotation) * PI / 180.;
    const double half = std::abs(std::sin(a)) * 0.5 * sz.x() + std::abs(std::cos(a)) * 0.5 * sz.y();

    // Park the barrel's outer wall tangent to that edge - but walk it back in until the WHOLE
    // knuckle run fits, not just its centre. On a round or tapered cut face the run's far ends
    // reach further out than its middle does, so a barrel tangent to the widest point of the
    // face has its ends hanging in the air. Backing off by the sagitta of the run's own
    // half-length against the face's inscribed circle is the cheap, always-safe version of
    // that: on a square face it costs nothing worth seeing, and on a round one it is exactly
    // the correction needed.
    const double r_in = flexi_section_inscribed_radius();
    const double hl   = 0.5 * double(m_flexi.hinge_length);
    double       reach = half;
    if (r_in > 0. && hl < r_in)
        // The chord at half-length: how far out the run's ENDS can sit and still be inside a
        // circle of radius r_in.
        reach = std::min(reach, std::sqrt(r_in * r_in - hl * hl));

    // One last step back. A barrel whose outer wall lands EXACTLY on the part's own side face
    // gives the boolean two coplanar surfaces to union across, which is the classic way to
    // make Manifold (and mcut behind it) give up - and a failed boolean drops the whole joint,
    // not just the placement. A tenth of a millimetre of bite into the wall costs nothing
    // visually and keeps every face transverse.
    const double off = reach - 0.5 * double(m_flexi.hinge_barrel_dia) - 0.1;
    return float(std::max(0., off));
}

void GLGizmoCut3D::sync_flexi_params(CutConnectors& connectors, bool resize_from_section)
{
    m_flexi.kind = FlexiJointKind(m_flexi_kind_id);

    const float floor_c = flexi_clearance_floor(double(flexi_nozzle_diameter()));
    if (m_flexi.clearance < floor_c)
        m_flexi.clearance = floor_c;
    // The gap defaults per kind and can never close below the clearance.
    if (m_flexi.gap <= 0.f)
        m_flexi.gap = flexi_default_gap(m_flexi.kind);
    if (m_flexi.gap < m_flexi.clearance)
        m_flexi.gap = m_flexi.clearance;

    if (resize_from_section && m_flexi_auto_size)
        m_flexi = flexi_auto_size(m_flexi, flexi_section_inscribed_radius());

    // The hinge's edge placement follows the cut face, so it is recomputed whenever the size
    // or the rotation changes - unless the user has taken the wheel with "Auto edge" off.
    if (m_flexi.kind == FlexiJointKind::Hinge && m_flexi_hinge_auto_edge)
        m_flexi.hinge_edge_offset = flexi_hinge_auto_edge_offset();

    // Keep radius/height in sync so the existing "fits inside the contour" and overlap
    // checks, the preview scaling and the raycasters all keep working unchanged.
    const float r = flexi_outer_extent(m_flexi);
    const float h = flexi_protrusion_height(m_flexi);
    m_connector_size          = 2.f * r;
    m_connector_depth_ratio   = h;

    for (CutConnector& c : connectors)
        if (c.attribs.type == CutConnectorType::FlexiJoint) {
            c.flexi  = m_flexi;
            c.radius = r;
            c.height = h;
        }

    update_connector_shape();
    update_raycasters_for_picking();
    check_and_update_connectors_state();
}

bool GLGizmoCut3D::render_flexi_float_input(const std::string& label, float& in_val, float min_val, float max_val, const wxString& tooltip)
{
    ImGui::AlignTextToFramePadding();
    m_imgui->text(label);
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(0.55f * float(m_editing_window_width));
    float val = in_val;
    ImGui::BBLDragFloat(("##flexi_" + label).c_str(), &val, 0.01f, min_val, max_val, "%.2f");
    if (!tooltip.IsEmpty() && ImGui::IsItemHovered())
        m_imgui->tooltip(tooltip, ImGui::GetFontSize() * 20.0f);
    if (val < min_val) val = min_val;
    if (val > max_val) val = max_val;
    if (is_approx(val, in_val))
        return false;
    in_val = val;
    return true;
}

// The Flexi joint's Rotation: degrees about the cut normal, laid out like the plain
// connector's "Rotation" row (slider + numeric field) so the panel reads the same either way.
// The value lives in FlexiJointParams::rotation, in DEGREES - unlike CutConnector::z_angle,
// which the plain connector keeps in radians.
bool GLGizmoCut3D::render_flexi_rotation_input(const std::string& label, float& in_val, const wxString& tooltip)
{
    const double slider_width = 0.24 * m_editing_window_width;
    const double item_in_gap  = 0.01 * m_editing_window_width;
    const double input_width  = 0.29 * m_editing_window_width;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(label);
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(float(slider_width));

    float val = in_val;
    const std::string format = "%.0f" "\xC2\xB0";
    m_imgui->bbl_slider_float_style("##flexi_rot_" + label, &val, 0.f, 180.f, format.c_str(), 1.f, true, from_u8(label));

    ImGui::SameLine(float(m_label_width + slider_width + item_in_gap));
    ImGui::PushItemWidth(float(input_width));
    ImGui::BBLDragFloat(("##flexi_rot_input_" + label).c_str(), &val, 0.5f, 0.f, 180.f, format.c_str());
    if (!tooltip.IsEmpty() && ImGui::IsItemHovered())
        m_imgui->tooltip(tooltip, ImGui::GetFontSize() * 20.0f);

    if (val < 0.f)   val = 0.f;
    if (val > 180.f) val = 180.f;
    if (is_approx(val, in_val))
        return false;
    in_val = val;
    return true;
}

void GLGizmoCut3D::render_flexi_joint_inputs(CutConnectors& connectors)
{
    bool changed = false;

    if (render_combo(m_labels_map["Joint"], m_flexi_kinds, m_flexi_kind_id, m_label_width, m_editing_window_width)) {
        m_flexi.kind = FlexiJointKind(m_flexi_kind_id);
        sync_flexi_params(connectors, true);
    }

    if (m_imgui->bbl_checkbox(_L("Auto size from the cut cross-section"), m_flexi_auto_size) && m_flexi_auto_size)
        sync_flexi_params(connectors, true);

    if (flexi_kind_is_twist(m_flexi.kind)) {
        // A TWIST LOCK. Everything here is sized off the major diameter, which auto-sizing
        // parks at 0.7 x the cut section's inscribed radius.
        m_imgui->disabled_begin(m_flexi_auto_size);
            changed |= render_flexi_float_input(m_labels_map["Major dia"], m_flexi.thread_major_dia, 4.f, 200.f,
                                                _L("Outside diameter of the thread, or of the bayonet's lug circle. Auto = 0.7 x the inscribed radius of the cut cross-section."));
        m_imgui->disabled_end();

        if (m_flexi.kind == FlexiJointKind::Thread) {
            m_imgui->disabled_begin(m_flexi_auto_size);
                changed |= render_flexi_float_input(m_labels_map["Pitch"], m_flexi.thread_pitch, 1.f, 20.f,
                                                    _L("How far the thread rises per turn. A coarse pitch prints far better than a fine one: 2 mm is the floor on a 0.4 mm nozzle, 3 to 4 mm is the comfortable range."));
            m_imgui->disabled_end();

            // Starts. An N-start thread seats in 360/N degrees, which is the whole difference
            // between a lid and a fastener.
            {
                ImGui::AlignTextToFramePadding();
                m_imgui->text(m_labels_map["Starts"]);
                ImGui::SameLine(m_label_width);
                ImGui::PushItemWidth(0.55f * float(m_editing_window_width));
                static const int starts_min = 1;
                static const int starts_max = 4;
                int n = thread_start_count(m_flexi);
                if (ImGui::BBLSliderScalar("##flexi_starts", ImGuiDataType_S32, &n, &starts_min, &starts_max, "%d")) {
                    n = std::max(starts_min, std::min(starts_max, n));
                    if (n != m_flexi.thread_starts) {
                        m_flexi.thread_starts = n;
                        changed = true;
                    }
                }
                if (ImGui::IsItemHovered())
                    m_imgui->tooltip(_L("How many thread strands run side by side. An N-start thread does up in 360/N degrees of turn: 2 starts gives a half-turn lid, 4 a quarter-turn one."), ImGui::GetFontSize() * 20.0f);
            }

            changed |= render_flexi_float_input(m_labels_map["Turns"], m_flexi.thread_turns, 0.25f, 10.f,
                                                _L("How many full revolutions of thread there are. 1.25 is the usual jar and pill-bottle range: enough to seat without winding it on forever."));
            changed |= render_flexi_float_input(m_labels_map["Lead-in"], m_flexi.thread_lead_turns, 0.f, 2.f,
                                                _L("How much of a turn, at each end, the thread fades to nothing over. This is the lead-in chamfer: it is what lets the two halves catch at any relative angle instead of having to be lined up."));
            {
                bool lh = m_flexi.thread_left_hand;
                if (m_imgui->bbl_checkbox(_L("Left-hand thread"), lh)) {
                    m_flexi.thread_left_hand = lh;
                    changed = true;
                }
            }
        } else {
            // Bayonet.
            {
                ImGui::AlignTextToFramePadding();
                m_imgui->text(m_labels_map["Lugs"]);
                ImGui::SameLine(m_label_width);
                ImGui::PushItemWidth(0.55f * float(m_editing_window_width));
                static const int lugs_min = 2;
                static const int lugs_max = 4;
                int n = bayonet_lug_count(m_flexi);
                if (ImGui::BBLSliderScalar("##flexi_lugs", ImGuiDataType_S32, &n, &lugs_min, &lugs_max, "%d")) {
                    n = std::max(lugs_min, std::min(lugs_max, n));
                    if (n != m_flexi.bayonet_lugs) {
                        m_flexi.bayonet_lugs = n;
                        changed = true;
                    }
                }
                if (ImGui::IsItemHovered())
                    m_imgui->tooltip(_L("How many lugs ride in the bayonet's slots, evenly spaced around the plug. Three is the usual compromise between holding power and having wall left between the slots."), ImGui::GetFontSize() * 20.0f);
            }
            m_imgui->disabled_begin(m_flexi_auto_size);
                changed |= render_flexi_float_input(m_labels_map["Lug height"], m_flexi.bayonet_lug_height, 0.4f, 10.f,
                                                    _L("How far each lug stands out of the plug wall."));
                changed |= render_flexi_float_input(m_labels_map["Lug thickness"], m_flexi.bayonet_lug_thickness, 0.4f, 10.f,
                                                    _L("How tall each lug is along the axis. This is what carries the pull-out load, so it wants at least a few layers."));
            m_imgui->disabled_end();
            changed |= render_flexi_float_input(m_labels_map["Lug arc"], m_flexi.bayonet_lug_arc, 5.f, 90.f,
                                                _L("Angular width of one lug, in degrees. It has to share its slice of the bore with the entry channel and the track."));
            changed |= render_flexi_float_input(m_labels_map["Lock angle"], m_flexi.bayonet_lock_angle, 20.f, 120.f,
                                                _L("How far the lid turns to lock, in degrees. 60 to 90 is the usual quarter-turn range; it is clamped so the track cannot run into the next lug's channel."));
            changed |= render_flexi_float_input(m_labels_map["Entry depth"], m_flexi.bayonet_entry_depth, 0.5f, 40.f,
                                                _L("How far the lid pushes straight in before it can be turned. It has to be at least the lug's own thickness."));
            changed |= render_flexi_float_input(m_labels_map["Detent"], m_flexi.bayonet_detent, 0.f, 3.f,
                                                _L("Height of the bump at the end of the track that the lug clicks over, so the lid stops where it locks instead of turning back. 0 leaves the track plain."));
            {
                bool lh = m_flexi.thread_left_hand;
                if (m_imgui->bbl_checkbox(_L("Turn the other way to lock"), lh)) {
                    m_flexi.thread_left_hand = lh;
                    changed = true;
                }
            }
        }

        // Which half is the LID, i.e. which one carries the male fastener and comes off.
        {
            bool upper = m_flexi.thread_lid_upper;
            if (m_imgui->bbl_checkbox(_L("Lid side: the upper half screws on"), upper)) {
                m_flexi.thread_lid_upper = upper;
                changed = true;
            }
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_L("Which half carries the male thread or the lugged plug - i.e. which one is the cap. The other half gets the bore."), ImGui::GetFontSize() * 20.0f);
        }

        // ROTATION - for a thread this is the START ANGLE, which is what decides where the lid
        // points when it is done up.
        changed |= render_flexi_rotation_input(m_labels_map["Rotation"], m_flexi.rotation,
                                               m_flexi.kind == FlexiJointKind::Thread ?
                                               _L("The thread's start angle: where the first strand begins around the axis. Turning it turns where the lid ends up pointing when it is screwed down.") :
                                               _L("Where the first lug sits around the axis. Turning it turns where the lid ends up pointing when it is locked."));
    } else if (m_flexi.kind == FlexiJointKind::Hinge) {
        // Knuckle count is an int, and an ODD one is self-centring: the middle knuckle sits on
        // the joint origin, so the hinge does not drift off the point the user clicked.
        {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_labels_map["Knuckles"]);
            ImGui::SameLine(m_label_width);
            ImGui::PushItemWidth(0.55f * float(m_editing_window_width));
            static const int knuckles_min = 1;
            static const int knuckles_max = 9;
            int n = hinge_knuckle_count(m_flexi);
            if (ImGui::BBLSliderScalar("##flexi_knuckles", ImGuiDataType_S32, &n, &knuckles_min, &knuckles_max, "%d")) {
                n = std::max(knuckles_min, std::min(knuckles_max, n));
                if (n != m_flexi.hinge_knuckles) {
                    m_flexi.hinge_knuckles = n;
                    changed = true;
                }
            }
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_L("How many knuckles the hinge is split into, alternating between the two halves. An odd count centres the hinge on the point you clicked; an even one shifts it by half a knuckle."), ImGui::GetFontSize() * 20.0f);
        }

        m_imgui->disabled_begin(m_flexi_auto_size);
            changed |= render_flexi_float_input(m_labels_map["Pin dia"], m_flexi.hinge_pin_dia, 0.4f, 20.f,
                                                _L("Diameter of the pin that runs through every knuckle. The pin is one continuous solid, integral to whichever half the Fold side names."));
            changed |= render_flexi_float_input(m_labels_map["Barrel dia"], m_flexi.hinge_barrel_dia, 1.f, 40.f,
                                                _L("Outer diameter of each knuckle. It has to clear the pin, the clearance on both sides and a wall on each side."));
            changed |= render_flexi_float_input(m_labels_map["Hinge length"], m_flexi.hinge_length, 2.f, 200.f,
                                                _L("Overall length of the knuckle run along the hinge axis. Each knuckle gets an equal share of it, less the gap at each end face."));
        m_imgui->disabled_end();

        // Edge placement: the barrel belongs at the EDGE of the cut face, or the halves cannot
        // fold shut without colliding. Auto parks it there; the number stays editable for the
        // protruding / flush / recessed cases.
        if (m_imgui->bbl_checkbox(_L("Auto edge placement"), m_flexi_hinge_auto_edge) && m_flexi_hinge_auto_edge) {
            m_flexi.hinge_edge_offset = flexi_hinge_auto_edge_offset();
            changed = true;
        }
        m_imgui->disabled_begin(m_flexi_hinge_auto_edge);
            changed |= render_flexi_float_input(m_labels_map["Edge offset"], m_flexi.hinge_edge_offset, 0.f, 200.f,
                                                _L("How far the barrel sits out from the cut plane's centre, towards the edge of the cut face. The halves can only fold shut when the barrel is at (or past) that edge."));
        m_imgui->disabled_end();

        // Which half owns the pin. The cut's male/female split is a Z-order convention that has
        // nothing to do with which way the user wants the part to open, so this has to be a
        // flag rather than something derived.
        {
            bool upper = m_flexi.hinge_fold_upper;
            if (m_imgui->bbl_checkbox(_L("Fold side: the upper half carries the pin"), upper)) {
                m_flexi.hinge_fold_upper = upper;
                changed = true;
            }
        }

        // ROTATION - the same field the chain link turns on, and for the hinge it is not a
        // secondary control at all: d, the direction the knuckle run points, IS this angle.
        changed |= render_flexi_rotation_input(m_labels_map["Rotation"], m_flexi.rotation,
                                               _L("Turns the hinge about the cut normal. This angle IS the pin axis: it decides which way the part folds, and whether the pin ends up lying flat on the bed (easy to print) or standing up (needs care)."));
    } else if (m_flexi.kind == FlexiJointKind::ChainLink) {
        m_imgui->disabled_begin(m_flexi_auto_size);
            changed |= render_flexi_float_input(m_labels_map["Link length"], m_flexi.link_length, 1.f, 80.f,
                                                _L("Overall length of each loop, along the loop's long axis."));
            changed |= render_flexi_float_input(m_labels_map["Link width"], m_flexi.link_width, 1.f, 80.f,
                                                _L("Overall width of each loop, across the loop. The loop's opening has to pass the other loop's wire."));
            changed |= render_flexi_float_input(m_labels_map["Wire"], m_flexi.wire, 0.2f, 10.f,
                                                _L("Tube radius of the wire each loop is made of."));
            changed |= render_flexi_float_input(m_labels_map["Stem"], m_flexi.stem, 0.2f, 20.f,
                                                _L("How deep each loop's far end is embedded in its own segment."));
        m_imgui->disabled_end();
        changed |= render_flexi_float_input(m_labels_map["Link tilt"], m_flexi.tilt_angle, 0.f, 30.f,
                                            _L("How far the upper ring is tipped inside its own plane, in degrees, so its free end leans clear of the lower ring."));
        changed |= render_flexi_rotation_input(m_labels_map["Rotation"], m_flexi.rotation,
                                               _L("Turns the whole link about the cut normal. Both rings turn together, so their planes stay perpendicular to each other; this only chooses where in the cut plane the pair sits - use it to line the rings up with the part, or with the way it prints."));
    } else {
        m_imgui->disabled_begin(m_flexi_auto_size);
            changed |= render_flexi_float_input(m_labels_map[m_flexi.kind == FlexiJointKind::DoubleRing ? "Outer radius" : "Ball radius"],
                                                m_flexi.outer_radius, 0.6f, 60.f,
                                                _L("Outer radius of the joint. Auto = 0.4 x the inscribed radius of the cut cross-section."));
        m_imgui->disabled_end();

        if (m_flexi.kind == FlexiJointKind::DoubleRing) {
            changed |= render_flexi_float_input(m_labels_map["Ring width"], m_flexi.ring_width, 0.4f, 20.f,
                                                _L("Radial thickness of the ring lip."));
            changed |= render_flexi_float_input(m_labels_map["Ring height"], m_flexi.ring_height, 0.4f, 20.f,
                                                _L("How far the ring lip reaches into the groove."));
        } else {
            changed |= render_flexi_float_input(m_labels_map["Opening"], m_flexi.open_angle, 10.f, 75.f,
                                                _L("Half angle of the socket mouth, in degrees. Smaller keeps the ball captive, larger gives more movement."));
        }
    }

    const float floor_c = flexi_clearance_floor(double(flexi_nozzle_diameter()));
    changed |= render_flexi_float_input(m_labels_map["Clearance"], m_flexi.clearance, floor_c, 2.f,
                                        _L("Space left between the joint's moving surfaces so they do not fuse. The floor comes from the nozzle diameter of the active printer preset."));

    // The GAP is the thickness of the cut itself: how far apart the two segments' flat faces
    // end up. It applies to every joint kind, and it is what actually makes the segment bend -
    // a joint whose faces nearly touch barely moves. It can never be smaller than the
    // clearance.
    if (m_flexi.gap <= 0.f)
        m_flexi.gap = flexi_default_gap(m_flexi.kind);
    changed |= render_flexi_float_input(m_labels_map["Gap"], m_flexi.gap, m_flexi.clearance, 20.f,
                                        _L("Thickness of the cut: how far apart the two segments' faces end up. Each face is set back from the cut plane by half of it, and the joint bridges the gap. A larger gap makes the joint visibly more flexible; it can never be smaller than the clearance."));

    if (m_flexi.kind != FlexiJointKind::ChainLink && m_flexi.kind != FlexiJointKind::Hinge &&
        !flexi_kind_is_twist(m_flexi.kind))
        changed |= render_flexi_float_input(m_labels_map["Tilt"], m_flexi.tilt, 0.f, 3.f,
                                            _L("Extra headroom carved into the groove so the segment can rock."));

    if (m_flexi.kind == FlexiJointKind::DoubleRing) {
        m_imgui->disabled_begin(m_flexi_auto_size);
            changed |= render_flexi_float_input(m_labels_map["Hub radius"], m_flexi.hub_radius, 0.f, 60.f,
                                                _L("Radius of the solid central hub that keeps the joint from collapsing. 0 disables it."));
        m_imgui->disabled_end();
    }

    if (changed)
        sync_flexi_params(connectors, false);

    // --- guards -------------------------------------------------------------------------
    // The guard texts are long sentences: wrapped to the panel's own width so a warning never
    // stretches the window across the part the user is trying to look at.
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + m_label_width + m_control_width);
    const std::string invalid = flexi_validate(m_flexi);
    if (!invalid.empty())
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, invalid);

    // PIN AXIS PRINTABILITY. A pin lying flat on the bed is the EASY case - the bore's roof is
    // a short bridge the ordinary overhang settings carry. A pin standing up along Z is the one
    // that needs care, because the bore's roof becomes a full unsupported circle and the moving
    // interface runs across the layers. Warn, never block: the user may well know better, and
    // the teardrop bore that fixes it properly is phase 2.
    if (m_flexi.kind == FlexiJointKind::Hinge && hinge_axis_needs_care(flexi_hinge_axis_world()))
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
            _u8L("This hinge's pin axis is not horizontal. A vertical or steeply tilted pin axis needs "
                 "support inside the bore; rotate the part, or change Rotation so the axis lies flat, "
                 "or expect to support the bore by hand."));

    // TWIST AXIS PRINTABILITY - the mirror image of the hinge's warning. A thread or a bayonet
    // wants its axis UPRIGHT: laid on its side, every turn's upper flank is a horizontal
    // overhang that sags into the very clearance the fit depends on, and the lid binds. Warn,
    // never block: the user may well reorient the part before slicing.
    if (flexi_kind_is_twist(m_flexi.kind) && twist_axis_needs_care(flexi_twist_axis_world()))
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
            m_flexi.kind == FlexiJointKind::Thread ?
            _u8L("This cut's axis is not vertical. A thread printed on its side has every turn's "
                 "upper flank hanging in the air, which sags into the clearance and jams the lid; "
                 "rotate the part so the cut normal points up, or expect a poor fit.") :
            _u8L("This cut's axis is not vertical. A bayonet printed on its side has its lugs and "
                 "slots overhanging; rotate the part so the cut normal points up for the best fit."));

    const double closing_radius = flexi_slice_closing_radius();
    if (flexi_gap_closing_conflict(m_flexi, closing_radius))
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
            _u8L("Slice gap closing radius") + " (" + double_to_string(closing_radius, 3).ToStdString() + " mm) " +
            _u8L("will fuse this clearance shut. Keep it below") + " " +
            double_to_string(flexi_max_safe_gap_closing_radius(m_flexi), 3).ToStdString() + " mm, " +
            _u8L("or raise the clearance."));

    if (flexi_kind_is_twist(m_flexi.kind)) {
        if (m_flexi.kind == FlexiJointKind::Thread)
            m_imgui->text(_L("The lid screws on. The two halves print in place as one object; "
                             "unscrew them after printing."));
        else
            m_imgui->text(_L("The lid pushes in and turns to lock. The two halves print in place "
                             "as one object."));
    } else
        m_imgui->text(_L("Both halves stay parts of one object so the joint prints in place."));
    ImGui::PopTextWrapPos();
}

void GLGizmoCut3D::render_build_size()
{
    double              koef     = m_imperial_units ? GizmoObjectManipulation::mm_to_in : 1.0;
    wxString            unit_str = " " + (m_imperial_units ? _L("in") : _L("mm"));
            
    Vec3d    tbb_sz = m_transformed_bounding_box.size();
    wxString size   =   "X: " + double_to_string(tbb_sz.x() * koef, 2) + unit_str +
                     ",  Y: " + double_to_string(tbb_sz.y() * koef, 2) + unit_str +
                     ",  Z: " + double_to_string(tbb_sz.z() * koef, 2) + unit_str;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Build Volume"));
    ImGui::SameLine();
    m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, size);
}

void GLGizmoCut3D::reset_cut_plane()
{
    m_angle_arc.reset();
    m_transformed_bounding_box = transformed_bounding_box(m_bb_center);
    set_center(m_bb_center);
    m_start_dragging_m = m_rotation_m = Transform3d::Identity();
    m_ar_plane_center  = m_plane_center;

    reset_cut_by_contours();
    m_parent.request_extra_frame();
}

void GLGizmoCut3D::invalidate_cut_plane()
{
    m_rotation_m    = Transform3d::Identity();
    m_plane_center  = Vec3d::Zero();
    m_min_pos       = Vec3d::Zero();
    m_max_pos       = Vec3d::Zero();
    m_bb_center     = Vec3d::Zero();
    m_center_offset = Vec3d::Zero();
}

void GLGizmoCut3D::set_connectors_editing(bool connectors_editing)
{
    if (m_connectors_editing == connectors_editing)
        return;

    m_connectors_editing = connectors_editing;
    if (m_connectors_editing)
        m_facet_picker.set_active(false); // pick-face is a planar-cut interaction only
    update_raycasters_for_picking();

    const bool clip = clip_by_plane_during_connector_editing(m_connectors_editing, is_draw_surface(), m_draw_stroke.valid());
    m_c->object_clipper()->set_behavior(clip, clip, double(m_contour_width));

    m_parent.request_extra_frame();
}

// Pick-face mode: place the cut plane flush with the currently-picked facet.
// Mirrors flip_cut_plane()'s state-update idiom but sets an ABSOLUTE rotation + center
// derived from the facet (plane normal = m_rotation_m * UnitZ).
bool GLGizmoCut3D::apply_picked_facet()
{
    const FacetPicker::Hit &picked = m_facet_picker.hit();
    if (!picked.valid() || picked.world_normal.isZero())
        return false;

    Eigen::Quaterniond q;
    Transform3d        m = Transform3d::Identity();
    m.matrix().block(0, 0, 3, 3) = q.setFromTwoVectors(Vec3d::UnitZ(), picked.world_normal).toRotationMatrix();

    // Require the plane to actually straddle the model (bbox in the plane frame crosses
    // z=0) so a grazing pick can't place a non-cutting plane.
    const BoundingBoxf3 tbb = transformed_bounding_box(picked.world_pos, m);
    const double        limit_val = 0.5;
    if (!(tbb.max.z() > -limit_val && tbb.min.z() < limit_val))
        return false;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Cut plane from face"), UndoRedo::SnapshotType::GizmoAction);

    m_rotation_m       = m;
    m_start_dragging_m = m;
    // Set the plane center directly rather than via set_center()/set_center_pos(): with a
    // changed rotation, set_center_pos clamps the new center against the STALE
    // m_transformed_bounding_box (old rotation) and, on failure, rejects the move so the
    // plane snapped back to the model centre. Our straddle guard above already validated the
    // pick against the NEW rotation, so assign directly and refresh the caches.
    m_plane_center             = picked.world_pos;
    m_center_offset            = m_plane_center - m_bb_center;
    m_ar_plane_center          = m_plane_center;
    m_transformed_bounding_box = transformed_bounding_box(m_plane_center, m_rotation_m);
    check_and_update_connectors_state();
    update_clipper();

    if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
        reset_cut_by_contours();

    m_parent.request_extra_frame();
    return true;
}

void GLGizmoCut3D::flip_cut_plane()
{
    // THE SHEET HAS TO COME WITH THE FRAME.
    //
    // The flip turns the plane frame 180 degrees about its own X, so local y and
    // z both negate. The sheet's control values are expressed IN that frame, so
    // leaving them alone would describe a different world surface: the reported
    // "right-click switch-sides partially flattens the sheet", with the preview
    // stuck wrong until the plane was nudged. flip_about_u() is the exact matching
    // change to the height field - mirror the rows along v, negate every value -
    // so the surface in WORLD space is identical before and after and only which
    // half counts as upper and which as lower swaps. It is pure arithmetic on the
    // control grid: no evaluation, no re-sampling, nothing lost.
    const bool carry_sheet = m_surface_mode == CutSurfaceMode::Curved && !m_curved_sheet.is_flat();
    if (carry_sheet) {
        push_curved_undo();
        m_curved_sheet.flip_about_u();
    }

    // THE STROKE HAS TO COME WITH THE FRAME TOO, for the same reason: the samples
    // are expressed IN the plane frame, so leaving them alone would describe a
    // different world curve. The flip turns the frame 180 degrees about its own X,
    // so a point that was at local (x, y, z) is at (x, -y, -z) afterwards - and
    // the same for the normals, which are directions in that frame. That is the
    // exact matching change, and like the sheet's it is pure arithmetic: no
    // re-projection, no re-fit, nothing lost.
    // 2026-09-12: the CHAIN is what carries, and its stroke ranges are untouched by the
    // flip - the samples keep their order and only their coordinates change - so
    // set_samples() would be wrong here (it would collapse the chain to one stroke and
    // lose the per-stroke undo the user has built up). The chain is rebuilt by replaying
    // its own ranges instead.
    const bool carry_stroke = m_surface_mode == CutSurfaceMode::Draw && !m_draw_chain.empty();
    if (carry_stroke) {
        push_draw_undo();
        std::vector<DrawCutSample> flipped = m_draw_chain.samples();
        for (DrawCutSample& smp : flipped) {
            smp.pos    = Vec3d(smp.pos.x(), -smp.pos.y(), -smp.pos.z());
            smp.normal = Vec3d(smp.normal.x(), -smp.normal.y(), -smp.normal.z());
        }
        CutRecipeStroke keep = cut_recipe_stroke_from_chain(m_draw_chain, double(m_draw_smoothing));
        keep.samples = flipped;
        cut_recipe_stroke_to_chain(keep, m_draw_chain);
    }

    m_rotation_m = m_rotation_m * rotation_transform(PI * Vec3d::UnitX());
    m_transformed_bounding_box = transformed_bounding_box(m_plane_center, m_rotation_m);

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Flip cut plane"), UndoRedo::SnapshotType::GizmoAction);
    m_start_dragging_m = m_rotation_m;

    // Do NOT let the extent re-fit run here. The rotation changed, so
    // fit_curved_sheet_to_section() would consider the plane "moved" and
    // re-sample the grid - and a re-sample straight after a flip is exactly the
    // path that used to wash the surface out. The projected extent is invariant
    // under this flip anyway (the part projects onto the same u and the mirrored
    // v, so the half extents are unchanged), so tell the fit the sheet is already
    // correct for the new frame rather than making it re-derive that.
    if (m_surface_mode == CutSurfaceMode::Curved) {
        m_curved_fit_rotation = m_rotation_m;
        m_curved_fit_center   = m_plane_center;
        m_curved_fit_valid    = true;
        m_curved_fit_pending  = false;
    }

    update_clipper();
    m_part_selection.turn_over_selection();

    if (carry_sheet) {
        // Force the preview to rebuild the way a control-point drag does: the
        // sheet model, the colour-clip texture, the cut cap and the connector
        // pick mesh are all keyed on a surface that just changed frame.
        invalidate_curved_sheet();
        update_curved_empty_sides();
        update_curved_connector_warnings();
        m_curved_hover_ctl = m_curved_drag_ctl = -1;
    }

    if (carry_stroke) {
        // The frame turned, so the cached instance mesh in it is stale - and
        // refresh_draw_stroke() reads it for the empty-side test.
        invalidate_draw_pick_mesh();
        refresh_draw_stroke();
    }

    if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
        reset_cut_by_contours();

    m_parent.set_as_dirty();
}

void GLGizmoCut3D::reset_cut_by_contours()
{
    m_part_selection = PartSelection();

    if (CutMode(m_mode) == CutMode::cutTongueAndGroove) {
        if (m_dragging || m_groove_editing || !has_valid_groove())
            return;
        process_contours();
    }
    else
        toggle_model_objects_visibility();
}

void GLGizmoCut3D::process_contours()
{
    const Selection& selection = m_parent.get_selection();
    const ModelObjectPtrs& model_objects = selection.get_model()->objects;

    const int instance_idx = selection.get_instance_idx();
    if (instance_idx < 0)
        return;
    const int object_idx = selection.get_object_idx();

    wxBusyCursor wait;

    if (CutMode(m_mode) == CutMode::cutTongueAndGroove) {
        if (has_valid_groove()) {
            Cut cut(model_objects[object_idx], instance_idx, get_cut_matrix(selection));
            const ModelObjectPtrs& new_objects = cut.perform_with_groove(m_groove, m_rotation_m, true);
            if (!new_objects.empty())
                m_part_selection = PartSelection(new_objects.front(), instance_idx);
        }
    }
    else {
        reset_cut_by_contours();
        m_part_selection = PartSelection(model_objects[object_idx], get_cut_matrix(selection), instance_idx, m_plane_center, m_cut_normal, *m_c->object_clipper());
    }

    toggle_model_objects_visibility();
}

void GLGizmoCut3D::render_flip_plane_button(bool disable_pred /*=false*/)
{
    ImGui::SameLine();

    // Same bound as the click: the button lights up only while the pointer is on
    // the surface as drawn, not merely on the oversized picking quad.
    const bool cp_hovered = m_hover_id == CutPlane && m_cut_surface_hovered;
    if (cp_hovered)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonHovered));

    m_imgui->disabled_begin(disable_pred);
        if (m_imgui->button(_L("Flip cut plane")))
            flip_cut_plane();
    m_imgui->disabled_end();

    if (cp_hovered)
        ImGui::PopStyleColor();
}


// ---------------------------------------------------------------------------
// MIRROR ON X / Y / Z.
//
// The owner's ask: "currently it's hard to duplicate a cut exactly on both
// halves of an object". These three buttons are the authoring half of the
// answer (Copy cut to... is the other half), and they are ALWAYS AVAILABLE -
// not gated to a copy-cut session - because mirroring a cut onto the opposite
// face of the SAME part is just as useful as mirroring it onto another object,
// and is as freely reversible as Flip cut plane already is.
//
// ONE CODE PATH, not six. The live state a mirror has to move is spread over
// m_plane_center, m_rotation_m, m_curved_sheet, m_draw_chain, m_groove and the
// object's cut_connectors - six different representations, in three different
// frames. Rather than mirroring each of them here, the gizmo round-trips through
// the recipe it already builds and applies: build_recipe_from_gizmo() gathers
// all six into one object in one frame, cut_recipe_mirrored() is the pure,
// unit-tested function that moves it, and apply_recipe_to_gizmo() puts every
// control back. That reuses machinery the re-edit path exercises on every
// project open rather than inventing a second, parallel one that can drift.
//
// THE FRAME. The recipe holds positions in the OBJECT frame (world minus the
// instance offset - see build_recipe_from_gizmo), and so does every connector.
// m_bb_center is in the WORLD, so the pivot has to have the same offset taken
// off it before it goes in. Mirroring about the BOUNDING-BOX CENTRE, not the
// world origin: the two halves of a cut sit symmetrically about that centre
// after the first cut, and an object that is not centred on the bed origin
// would otherwise have its mirrored cut fly off to the far side of the plate.
//
// THE MESH. build_recipe_from_gizmo() wants one for the content hash, and
// hashing the real part mesh on every button press would stall the panel on a
// heavy model for nothing: the mirror never looks at the mesh and
// apply_recipe_to_gizmo() never reads it. An empty one it is.
//
// NO UPPER/LOWER SWAP. cut_recipe_mirrored() reflects the plane normal exactly
// (see the handedness note in CutRecipe.hpp), so the side that was upper still
// is, and the keep_/place_on_cut_/rotate_/visibility pairs stay where the user
// put them. This is deliberately NOT the 180-degree flip, which does turn the
// plane over and does call m_part_selection.turn_over_selection().
void GLGizmoCut3D::mirror_cut(CutMirrorAxis axis)
{
    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;

    Vec3d instance_offset = Vec3d::Zero();
    if (mo && !mo->instances.empty())
        instance_offset = mo->instances.front()->get_offset();

    const std::string act_name = axis == CutMirrorAxis::X ? _u8L("Mirror cut on X") :
                                 axis == CutMirrorAxis::Y ? _u8L("Mirror cut on Y") :
                                                            _u8L("Mirror cut on Z");
    // Its own snapshot, exactly as flip_cut_plane() takes one: a mirror is a
    // single, independently undoable gesture, not part of whatever came before.
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), act_name, UndoRedo::SnapshotType::GizmoAction);

    const CutRecipe live     = build_recipe_from_gizmo(mo, TriangleMesh());
    const CutRecipe mirrored = cut_recipe_mirrored(live, axis, m_bb_center - instance_offset);
    apply_recipe_to_gizmo(mirrored);

    // The caches apply_recipe_to_gizmo() does not own, because they are about the
    // WORLD the frame sits in rather than about the frame: the instance mesh cached
    // in the plane frame, and the groove's contour tessellation.
    if (m_surface_mode == CutSurfaceMode::Draw) {
        invalidate_draw_pick_mesh();
        refresh_draw_stroke();
    }
    if (m_surface_mode == CutSurfaceMode::Curved) {
        update_curved_connector_warnings();
        m_curved_hover_ctl = m_curved_drag_ctl = -1;
    }
    if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
        reset_cut_by_contours();

    check_and_update_connectors_state();
    update_raycasters_for_picking();
    m_parent.set_as_dirty();
}

// The three buttons, on their own row under "Cut position". Same style and the
// same row idiom as Flip cut plane / Reset cut beside them - m_imgui->button
// with SameLine between, no icons, so they read as one group.
void GLGizmoCut3D::render_mirror_buttons()
{
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Mirror cut") + ": ");
    ImGui::SameLine();

    // Always enabled (design point 4). There is no state in which mirroring is
    // meaningless: a plane with no surface mirrors to the plane on the other
    // side, which is exactly what a user asking for "the same cut on the other
    // half" means.
    if (m_imgui->button(_L("X"), _L("Mirror the whole cut - plane, surface and connectors - about the object's centre on the world X axis")))
        mirror_cut(CutMirrorAxis::X);
    ImGui::SameLine();
    if (m_imgui->button(_L("Y"), _L("Mirror the whole cut - plane, surface and connectors - about the object's centre on the world Y axis")))
        mirror_cut(CutMirrorAxis::Y);
    ImGui::SameLine();
    if (m_imgui->button(_L("Z"), _L("Mirror the whole cut - plane, surface and connectors - about the object's centre on the world Z axis")))
        mirror_cut(CutMirrorAxis::Z);
}
void GLGizmoCut3D::add_vertical_scaled_interval(float interval)
{
    ImGui::GetCurrentWindow()->DC.CursorPos.y += m_imgui->scaled(interval);
}

void GLGizmoCut3D::add_horizontal_scaled_interval(float interval)
{
    ImGui::GetCurrentWindow()->DC.CursorPos.x += m_imgui->scaled(interval);
}

void GLGizmoCut3D::add_horizontal_shift(float shift)
{
    ImGui::GetCurrentWindow()->DC.CursorPos.x += shift;
}

void GLGizmoCut3D::render_color_marker(float size, const ImU32& color)
{
    const float radius = 0.5f * size;
    ImVec2 pos = ImGui::GetCurrentWindow()->DC.CursorPos;
    pos.x += radius;
    pos.y += 1.4f * radius;
    ImGui::GetCurrentWindow()->DrawList->AddNgonFilled(pos, radius, color, 6);
    m_imgui->text("  ");
    ImGui::SameLine();
}

void GLGizmoCut3D::render_groove_float_input(const std::string& label, float& in_val, const float& init_val, float& in_tolerance)
{
    bool is_changed{false};

    float val = in_val;
    float tolerance = in_tolerance;
    if (render_slider_double_input(label, val, tolerance, -0.1f, std::min(0.3f*in_val, 1.5f))) {
        if (m_imgui->get_last_slider_status().can_take_snapshot) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), GUI::format("%1%: %2%", _u8L("Groove change"), label), UndoRedo::SnapshotType::GizmoAction);
            m_imgui->get_last_slider_status().invalidate_snapshot();
            m_groove_editing = true;
        }
        in_val = val;
        in_tolerance = tolerance;
        is_changed = true;
    }

    ImGui::SameLine();

    m_imgui->disabled_begin(is_approx(in_val, init_val) && is_approx(in_tolerance, 0.1f));
        const std::string act_name = _u8L("Reset");
        if (render_reset_button("##groove_" + label + act_name, act_name)) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), GUI::format("%1%: %2%", act_name, label), UndoRedo::SnapshotType::GizmoAction);
            in_val = init_val;
            in_tolerance = 0.1f;
            is_changed = true;
        }
    m_imgui->disabled_end();

    if (is_changed) {
        update_plane_model();
        reset_cut_by_contours();
    }

    if (m_is_slider_editing_done) {
        m_groove_editing = false;
        reset_cut_by_contours();
    }
}

bool GLGizmoCut3D::render_angle_input(const std::string& label, float& in_val, const float& init_val, float min_val, float max_val)
{
    // -------- [ ]
    // slider_with + item_in_gap + input_width
    double slider_with = 0.24 * m_editing_window_width; // m_control_width * 0.35;
    double item_in_gap = 0.01 * m_editing_window_width;
    double input_width = 0.29 * m_editing_window_width;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(label);
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(slider_with);

    double left_width = m_label_width + slider_with + item_in_gap;

    bool is_changed{ false };

    float val = rad2deg(in_val);
    const float old_val = val;

    const std::string format = "%.0f°";
    m_imgui->bbl_slider_float_style("##angle_" + label, &val, min_val, max_val, format.c_str(), 1.f, true, from_u8(label));

    ImGui::SameLine(left_width);
    ImGui::PushItemWidth(input_width);
    ImGui::BBLDragFloat(("##angle_input_" + label).c_str(), &val, 0.05f, min_val, max_val, format.c_str());

    m_is_slider_editing_done |= m_imgui->get_last_slider_status().deactivated_after_edit;
    if (!is_approx(old_val, val)) {
        if (m_imgui->get_last_slider_status().can_take_snapshot) {
            // TRN: This is an entry in the Undo/Redo stack. The whole line will be 'Edited: (name of whatever was edited)'.
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), GUI::format("%1%: %2%", _L("Edited"), label), UndoRedo::SnapshotType::GizmoAction);
            m_imgui->get_last_slider_status().invalidate_snapshot();
            if (m_mode == size_t(CutMode::cutTongueAndGroove))
                m_groove_editing = true;
        }
        in_val = deg2rad(val);
        is_changed = true;
    }

    ImGui::SameLine();

    m_imgui->disabled_begin(is_approx(in_val, init_val));
    const std::string act_name = _u8L("Reset");
    if (render_reset_button("##angle_" + label + act_name, act_name)) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), GUI::format("%1%: %2%", act_name, label), UndoRedo::SnapshotType::GizmoAction);
        in_val = init_val;
        is_changed = true;
    }
    m_imgui->disabled_end();

    return is_changed;
}

void GLGizmoCut3D::render_groove_angle_input(const std::string& label, float& in_val, const float& init_val, float min_val, float max_val)
{
    if (render_angle_input(label, in_val, init_val, min_val, max_val)) {
        update_plane_model();
        reset_cut_by_contours();
    }

    if (m_is_slider_editing_done) {
        m_groove_editing = false;
        reset_cut_by_contours();
    }
}

void GLGizmoCut3D::render_snap_specific_input(const std::string& label, const wxString& tooltip, float& in_val, const float& init_val, const float min_val, const float max_val)
{
    // -------- [ ]
    // slider_with + item_in_gap + input_width
    double slider_with = 0.24 * m_editing_window_width; // m_control_width * 0.35;
    double item_in_gap = 0.01 * m_editing_window_width;
    double input_width = 0.29 * m_editing_window_width;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(label);
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(slider_with);

    double left_width = m_label_width + slider_with + item_in_gap;

    bool is_changed = false;
    const std::string format = "%.0f %%";

    float val = in_val * 100.f;
    const float old_val = val;
    m_imgui->bbl_slider_float_style("##snap_" + label, &val, min_val, max_val, format.c_str(), 1.f, true, tooltip);

    ImGui::SameLine(left_width);
    ImGui::PushItemWidth(input_width);
    ImGui::BBLDragFloat(("##snap_input_" + label).c_str(), &val, 0.05f, min_val, max_val, format.c_str());

    if (!is_approx(old_val, val)) {
        in_val = val * 0.01f;
        is_changed = true;
    }
    
    ImGui::SameLine();

    m_imgui->disabled_begin(is_approx(in_val, init_val));
    const std::string act_name = _u8L("Reset");
    if (render_reset_button("##snap_" + label + act_name, act_name)) {
        in_val = init_val;
        is_changed = true;
    }
    m_imgui->disabled_end();

    if (is_changed) {
        update_connector_shape();
        update_raycasters_for_picking();
    }
}

void GLGizmoCut3D::render_cut_plane_input_window(CutConnectors &connectors, float x, float y, float bottom_limit)
{
//    if (m_mode == size_t(CutMode::cutPlanar)) {
    CutMode mode = CutMode(m_mode);
    if (mode == CutMode::cutPlanar || mode == CutMode::cutTongueAndGroove) {
        const bool has_connectors = !connectors.empty();

        m_imgui->disabled_begin(has_connectors);
        if (render_cut_mode_combo())
            mode = CutMode(m_mode);
        m_imgui->disabled_end();

        // RE-EDITABLE CUTS: say so, before anything else, when this panel is
        // editing a cut that already happened rather than making a new one.
        render_reedit_notice();

        render_build_size();

        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Cut position") + ": ");
        ImGui::SameLine();
        render_move_center_input(Z);
        ImGui::SameLine();

        const bool is_cut_plane_init = m_rotation_m.isApprox(Transform3d::Identity()) && m_bb_center.isApprox(m_plane_center);
        m_imgui->disabled_begin(is_cut_plane_init);
            std::string act_name = _u8L("Reset cutting plane");
            if (render_reset_button("cut_plane", act_name)) {
                Plater::TakeSnapshot snapshot(wxGetApp().plater(), act_name, UndoRedo::SnapshotType::GizmoAction);
                reset_cut_plane();
            }
        m_imgui->disabled_end();

//        render_flip_plane_button();

        // "Mirror on X / Y / Z", on their own row under Cut position. Always
        // available, on every surface kind and in both cut modes - see mirror_cut().
        add_vertical_scaled_interval(0.75f);
        render_mirror_buttons();

        if (mode == CutMode::cutPlanar) {
            // Cut thickness ("kerf"), next to the cut position - it is a property
            // of the cut, not of the surface, and applies to Flat and Curved alike.
            render_cut_thickness_input();

            // Surface: Flat / Curved, and the curved surface's own controls.
            m_imgui->disabled_begin(has_connectors);
            render_curved_surface_inputs();
            m_imgui->disabled_end();

            add_vertical_scaled_interval(0.75f);

            // PHASE 4: connectors ARE available on a curved cut. They stand on the
            // sheet, on the sheet's own frame at their (u,v) - see
            // connector_rotation_m(). The remaining conditions are the flat cut's
            // own (both halves kept, not cut-to-parts, not a one-object contour
            // selection), unchanged.
            //
            // 2026-09-20, OWNER REPORT item 4: on a DRAWN cut a connector can only
            // stand on the drawn surface, which does not exist until a line is
            // finished - offering the button before that would silently fall back
            // to placing connectors on the (now hidden) flat plane.
            m_imgui->disabled_begin(!m_keep_upper || !m_keep_lower || m_keep_as_parts || (m_part_selection.valid() && m_part_selection.is_one_object())
                                    || (is_draw_surface() && !m_draw_stroke.valid()));
                if (m_imgui->button(has_connectors ? _L("Edit connectors") : _L("Add connectors")))
                    set_connectors_editing(true);
            m_imgui->disabled_end();

            ImGui::SameLine(1.5f * m_control_width);

            m_imgui->disabled_begin(is_cut_plane_init && !has_connectors);
                act_name = _u8L("Reset cut");
                if (m_imgui->button(wxString::FromUTF8(act_name), _L("Reset cutting plane and remove connectors"))) {
                    Plater::TakeSnapshot snapshot(wxGetApp().plater(), act_name, UndoRedo::SnapshotType::GizmoAction);
                    reset_cut_plane();
                    reset_connectors();
                    m_curved_sheet.reset();
                    invalidate_curved_sheet();
                }
            m_imgui->disabled_end();

            // Pick-face: click a model facet to set the cut plane flush with it.
            add_vertical_scaled_interval(0.75f);
            const bool picking = m_facet_picker.is_active();
            m_imgui->disabled_begin(has_connectors);
                if (picking)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
                if (m_imgui->button(_L("Pick face"), _L("Click a model face to set the cut plane flush with it; use Cut position to offset")))
                    m_facet_picker.set_active(!picking);
                if (picking)
                    ImGui::PopStyleColor();
            m_imgui->disabled_end();
            if (picking) {
                ImGui::SameLine();
                m_imgui->text(_L("Click a face of the model."));
            }
        }
        else if (mode == CutMode::cutTongueAndGroove) {
            m_is_slider_editing_done = false;
            ImGui::Separator();
            m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, m_labels_map["Groove"] + ": ");
            render_groove_float_input(m_labels_map["Depth"], m_groove.depth, m_groove.depth_init, m_groove.depth_tolerance);
            render_groove_float_input(m_labels_map["Width"], m_groove.width, m_groove.width_init, m_groove.width_tolerance);
            render_groove_angle_input(m_labels_map["Flap Angle"], m_groove.flaps_angle, m_groove.flaps_angle_init, 30.f, 120.f);
            render_groove_angle_input(m_labels_map["Groove Angle"], m_groove.angle, m_groove.angle_init, 0.f, 15.f);
        }

        ImGui::Separator();

        // render "After Cut" section

        ImVec2 label_size;
        for (const wxString &label : {_L("Upper part"), _L("Lower part")}) {
            const ImVec2 text_size = ImGuiWrapper::calc_text_size(label);
            if (label_size.x < text_size.x)
                label_size.x = text_size.x;
            if (label_size.y < text_size.y)
                label_size.y = text_size.y;
        }

        const float marker_size = label_size.y;
        const float h_shift     = marker_size + label_size.x + m_imgui->scaled(2.f);

        auto render_part_action_line = [this, h_shift, marker_size, &connectors](const wxString &label, const wxString &suffix, bool &keep_part,
                                                                        bool &place_on_cut_part, bool &rotate_part) {
            bool keep = true;

            ImGui::AlignTextToFramePadding();
            render_color_marker(marker_size, ImGuiWrapper::to_ImU32(suffix == "##upper" ? UPPER_PART_COLOR : LOWER_PART_COLOR));
            m_imgui->text(label);

            ImGui::SameLine(h_shift);

            m_imgui->disabled_begin(!connectors.empty() || m_keep_as_parts);
            m_imgui->bbl_checkbox(_L("Keep") + suffix, connectors.empty() ? keep_part : keep);
            m_imgui->disabled_end();

            ImGui::SameLine();

            m_imgui->disabled_begin(!keep_part || m_keep_as_parts);
            if (m_imgui->bbl_checkbox(_L("Place on cut") + suffix, place_on_cut_part))
                rotate_part = false;
            ImGui::SameLine();
            if (m_imgui->bbl_checkbox(_L("Flip") + suffix, rotate_part))
                place_on_cut_part = false;
            m_imgui->disabled_end();
        };

        m_imgui->text(_L("After cut") + ": ");
        render_part_action_line(_L("Upper part"), "##upper", m_keep_upper, m_place_on_cut_upper, m_rotate_upper);
        render_part_action_line(_L("Lower part"), "##lower", m_keep_lower, m_place_on_cut_lower, m_rotate_lower);

        m_imgui->disabled_begin(has_connectors || m_part_selection.valid() || mode == CutMode::cutTongueAndGroove);

            if (m_part_selection.valid())
                m_keep_as_parts = false;

            const bool flexi_placed = std::any_of(connectors.begin(), connectors.end(),
                                                  [](const CutConnector& c) { return c.attribs.type == CutConnectorType::FlexiJoint; });
            // A Flexi joint forces keep-as-parts, but that force must NOT be written back into
            // m_keep_as_parts: the gizmo is a singleton and that member outlives the cut, so a
            // sticky `true` there disables "Add connectors" (and the Keep / Place on cut
            // checkboxes) for every later cut until the app restarts. Show the forced value,
            // keep the user's own setting intact, and let perform_cut() apply the force.
            bool shown_keep_as_parts = flexi_placed ? true : m_keep_as_parts;
            m_imgui->disabled_begin(flexi_placed);
            if (m_imgui->bbl_checkbox(_L("Cut to parts"), shown_keep_as_parts) && !flexi_placed)
                m_keep_as_parts = shown_keep_as_parts;
            m_imgui->disabled_end();
            if (flexi_placed)
                m_imgui->text(_L("A Flexi cut always keeps both halves as parts of one object."));
            if (shown_keep_as_parts) {
                m_keep_upper = m_keep_lower = true;
                m_place_on_cut_upper = m_place_on_cut_lower = false;
                m_rotate_upper = m_rotate_lower = false;
            }
        m_imgui->disabled_end();

        // RE-EDITABLE CUTS. With this on, the cut records how it was made - the
        // surface, its parameters, the connectors - and, crucially, the object's
        // mesh AS IT IS NOW, so the cut can be reopened from either half and made
        // again. The mesh is what costs: it roughly doubles the project's size for
        // this object, which is why the tooltip says so and why this is a
        // per-cut choice rather than a preference.
        m_imgui->bbl_checkbox(_L("Keep cut editable"), m_keep_cut_editable);
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Remember this cut so it can be edited later: select either half and "
                                "choose \"Edit cut...\" to reopen this panel with the same surface and "
                                "settings.\n\n"
                                "The original, uncut shape is stored in the project file, so a cut "
                                "object takes about twice the space. Turn this off for a one-off cut."),
                             m_control_width * 2.f);
    }

    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    show_tooltip_information(x, get_cur_y);

    float f_scale = m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    ImGui::SameLine();
    m_imgui->disabled_begin(!can_perform_cut());
        if(m_imgui->button(_L("Perform cut")))
            perform_cut(m_parent.get_selection());
    m_imgui->disabled_end();

    ImGui::PopStyleVar(2);
}

void GLGizmoCut3D::validate_connector_settings()
{
    if (m_connector_depth_ratio < 0.f)
        m_connector_depth_ratio = 3.f;
    if (m_connector_depth_ratio_tolerance < 0.f)
        m_connector_depth_ratio_tolerance = 0.1f;
    if (m_connector_size < 0.f)
        m_connector_size = 2.5f;
    if (m_connector_size_tolerance < 0.f)
        m_connector_size_tolerance = 0.f;
    if (m_connector_angle < 0.f || m_connector_angle > float(PI) )
        m_connector_angle = 0.f;

    if (m_connector_type == CutConnectorType::Undef)
        m_connector_type = CutConnectorType::Plug;
    if (m_connector_style == int(CutConnectorStyle::Undef))
        m_connector_style = int(CutConnectorStyle::Prism);
    if (m_connector_shape_id == int(CutConnectorShape::Undef))
        m_connector_shape_id = int(CutConnectorShape::Circle);
}

void GLGizmoCut3D::init_input_window_data(CutConnectors &connectors)
{
    m_imperial_units = wxGetApp().app_config->get_bool("use_inches");
    m_control_width  = m_imgui->get_font_size() * 9.f;

    m_editing_window_width = 1.45 * m_control_width + 11;

    if (m_connectors_editing && m_selected_count > 0) {
        float               depth_ratio             { UndefFloat };
        float               depth_ratio_tolerance   { UndefFloat };
        float               radius                  { UndefFloat };
        float               radius_tolerance        { UndefFloat };
        float               angle                   { UndefFloat };
        CutConnectorType    type                    { CutConnectorType::Undef };
        CutConnectorStyle   style                   { CutConnectorStyle::Undef };
        CutConnectorShape   shape                   { CutConnectorShape::Undef };

        bool is_init = false;
        for (size_t idx = 0; idx < m_selected.size(); idx++)
            if (m_selected[idx]) {
                const CutConnector& connector = connectors[idx];
                if (!is_init) {
                    depth_ratio             = connector.height;
                    depth_ratio_tolerance   = connector.height_tolerance;
                    radius                  = connector.radius;
                    radius_tolerance        = connector.radius_tolerance;
                    angle                   = connector.z_angle;
                    type                    = connector.attribs.type;
                    style                   = connector.attribs.style;
                    shape                   = connector.attribs.shape;

                    if (m_selected_count == 1)
                        break;
                    is_init = true;
                }
                else {
                    if (!is_approx(depth_ratio, connector.height))
                        depth_ratio         = UndefFloat;
                    if (!is_approx(depth_ratio_tolerance, connector.height_tolerance))
                        depth_ratio_tolerance = UndefFloat;
                    if (!is_approx(radius,connector.radius))
                        radius              = UndefFloat;
                    if (!is_approx(radius_tolerance, connector.radius_tolerance))
                        radius_tolerance    = UndefFloat;
                    if (!is_approx(angle, connector.z_angle))
                        angle               = UndefFloat;

                    if (type != connector.attribs.type)
                        type = CutConnectorType::Undef;
                    if (style != connector.attribs.style)
                        style = CutConnectorStyle::Undef;
                    if (shape != connector.attribs.shape)
                        shape = CutConnectorShape::Undef;
                }
            }

        m_connector_depth_ratio             = depth_ratio;
        m_connector_depth_ratio_tolerance   = depth_ratio_tolerance;
        m_connector_size                    = 2.f * radius;
        m_connector_size_tolerance          = 2.f * radius_tolerance;
        m_connector_type                    = type;
        m_connector_angle                   = angle;
        m_connector_style                   = int(style);
        m_connector_shape_id                = int(shape);
    }

    if (m_label_width == 0.f) {
        for (const auto& item : m_labels_map) {
            const float width = m_imgui->calc_text_size(item.second).x;
            if (m_label_width < width)
                m_label_width = width;
        }
        m_label_width += m_imgui->scaled(1.f);
        m_label_width += ImGui::GetStyle().WindowPadding.x;
    }
}

void GLGizmoCut3D::render_input_window_warning() const
{
    if (! m_invalid_connectors_idxs.empty()) {
        wxString out = /*wxString(ImGui::WarningMarkerSmall)*/ _L("Warning") + ": " + _L("Invalid connectors detected") + ":";
        if (m_info_stats.outside_cut_contour > size_t(0))
            out += "\n - " + format_wxstr(_L_PLURAL("%1$d connector is out of cut contour", "%1$d connectors are out of cut contour", m_info_stats.outside_cut_contour),
                                          m_info_stats.outside_cut_contour);
        if (m_info_stats.outside_bb > size_t(0))
            out += "\n - " + format_wxstr(_L_PLURAL("%1$d connector is out of object", "%1$d connectors are out of object", m_info_stats.outside_bb),
                                           m_info_stats.outside_bb);
        if (m_info_stats.is_overlap)
            out += "\n - " + _L("Some connectors are overlapped");
        m_imgui->text(out);
    }
    if (!m_keep_upper && !m_keep_lower)
        m_imgui->text(/*wxString(ImGui::WarningMarkerSmall)*/ _L("Warning") + ": " + _L("Select at least one object to keep after cutting."));
    if (!has_valid_contour())
        m_imgui->text(/*wxString(ImGui::WarningMarkerSmall)*/ _L("Warning") + ": " + _L("Cut plane is placed out of object"));
    else if (!has_valid_groove())
        m_imgui->text(/*wxString(ImGui::WarningMarkerSmall)*/ _L("Warning") + ": " + _L("Cut plane with groove is invalid"));
}

void GLGizmoCut3D::on_render_input_window(float x, float y, float bottom_limit)
{
    dock_setup_next_window(x, y, bottom_limit);
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), dock_window_flags(ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar));

    if (!dock_render_titlebar(get_name())) {
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }

    CutConnectors& connectors = m_c->selection_info()->model_object()->cut_connectors;

    init_input_window_data(connectors);

    if (m_connectors_editing) // connectors mode
        render_connectors_input_window(connectors, x, y, bottom_limit); 
    else
        render_cut_plane_input_window(connectors, x, y, bottom_limit);

    render_input_window_warning();

    GizmoImguiEnd();

    // Orca
    ImGuiWrapper::pop_toolbar_style();

    if (!m_connectors_editing) // connectors mode
        render_debug_input_window(x);
}

void GLGizmoCut3D::show_tooltip_information(float x, float y)
{
    auto &shortcuts = m_connectors_editing ? m_shortcuts_connector : m_shortcuts_cut;

    float                      caption_max = 0.f;
    for (const auto &short_cut : shortcuts) {
        caption_max = std::max(caption_max, m_imgui->calc_text_size(short_cut.first).x);
    }

    ImTextureID normal_id = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += m_imgui->calc_text_size(std::string_view{": "}).x + 35.f;

    float  scale       = m_parent.get_scale();
    ImVec2 button_size = ImVec2(25 * scale, 25 * scale); // ORCA: Use exact resolution will prevent blur on icon
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, 0}); // ORCA: Dont add padding
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &caption_max](const wxString &caption, const wxString &text) {
            m_imgui->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            m_imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        for (const auto &short_cut : shortcuts)
            draw_text_with_caption(short_cut.first + ": ", short_cut.second);
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

bool GLGizmoCut3D::is_outside_of_cut_contour(size_t idx, const CutConnectors& connectors, const Vec3d cur_pos)
{
    // 2026-09-13, OWNER CLICK-TEST ITEM 3, the third place the flat plane was still the
    // authority on a drawn cut.
    //
    // Every test below asks the OBJECT CLIPPER whether a point is inside "the cut
    // contour", and the clipper's contour is the object's cross-section at the flat
    // plane - a single slice at z == 0 in the plane's frame. On a DRAWN cut the cut
    // surface is a ruled strip that goes wherever the user drew, and a connector on it
    // is generally nowhere near z == 0: project_onto_cut_plane() drops it onto the plane
    // and the clipper then reports a connector that is sitting correctly on the cut
    // surface as "out of cut contour", greying out the Perform cut button for a reason
    // that is not true.
    //
    // On a drawn cut the question has a different and better answer, and the gizmo
    // already computes it: a connector is IN the cut iff it is on the drawn surface,
    // which is what unproject_on_draw_surface() guarantees at placement and what
    // m_draw_offsurface_connectors counts afterwards (a plane nudge moves the surface
    // out from under a connector). So the contour test does not apply here, and
    // pretending it does is worse than not asking.
    //
    // The other two conflict tests in is_conflict_for_connector() - the bounding box and
    // the overlap - are about the connectors themselves and stay.
    if (is_draw_surface())
        return false;

    // check if connector pos is out of clipping plane
    if (m_c->object_clipper() && m_c->object_clipper()->is_projection_inside_cut(cur_pos) == -1) {
        m_info_stats.outside_cut_contour++;
        return true;
    }

    // check if connector bottom contour is out of clipping plane
    const CutConnector& cur_connector = connectors[idx];

    indexed_triangle_set mesh;
    auto& vertices = mesh.vertices;

    if (cur_connector.attribs.type == CutConnectorType::FlexiJoint) {
        // A FLEXI joint is not a disc. The hinge is a long thin rectangle along its own axis;
        // the chain link is the slot its two loops sweep through the plane. Testing either
        // against a CIRCLE of radius flexi_outer_extent() - the distance to the farthest corner
        // of that shape - rejects joints that fit the cut contour with room to spare, which is
        // exactly the spurious "1 connector is out of cut contour" the chain link was getting.
        // The footprint helper returns the real outline, already padded by the clearance, in
        // the joint's own frame; sampling its EDGES as well as its corners is what catches a
        // contour notch that a corners-only test would step over.
        const std::vector<Vec2d> corners = flexi_footprint_corners(cur_connector.flexi);
        const size_t             n       = corners.size();
        const int                per_edge = 8;
        vertices.reserve(n * size_t(per_edge));
        for (size_t i = 0; i < n; ++ i) {
            const Vec2d& a = corners[i];
            const Vec2d& b = corners[(i + 1) % n];
            for (int k = 0; k < per_edge; ++ k) {
                const double t = double(k) / double(per_edge);
                const Vec2d  q = a + t * (b - a);
                vertices.emplace_back(Vec3f(float(q.x()), float(q.y()), 0.f));
            }
        }
        // The joint's own Rotation is already baked into those corners - the helper turns them
        // the same way the bodies are turned - so a rotated hinge tests as a rotated rectangle
        // rather than a bigger one, and nothing extra is applied here.
        // PHASE 4: on a curved cut the footprint is laid out in the SHEET's tangent
        // plane at the connector, not in the cut plane - see the projection below.
        its_transform(mesh, translation_transform(cur_pos) * connector_rotation_m(cur_connector));
    }
    else {
        const CutConnectorShape shape = CutConnectorShape(cur_connector.attribs.shape);
        const int   sectorCount = shape == CutConnectorShape::Triangle  ? 3 :
                                  shape == CutConnectorShape::Square    ? 4 :
                                  shape == CutConnectorShape::Circle    ? 60: // supposably, 60 points are enough for conflict detection
                                  shape == CutConnectorShape::Hexagon   ? 6 : 1 ;

        vertices.reserve(sectorCount + 1);

        float fa = 2 * PI / sectorCount;
        auto vec = Eigen::Vector2f(0, cur_connector.radius);
        for (float angle = 0; angle < 2.f * PI; angle += fa) {
            Vec2f p = Eigen::Rotation2Df(angle) * vec;
            vertices.emplace_back(Vec3f(p(0), p(1), 0.f));
        }
        its_transform(mesh, translation_transform(cur_pos) * connector_rotation_m(cur_connector));
    }

    // PHASE 4: the clipper's contour test is a 2D test in the CUT PLANE's frame.
    // A footprint sampled in the sheet's TANGENT plane sits off the cut plane by
    // f(u,v) and is tilted, so each sample is projected back down the plane normal
    // onto the plane before it is tested - which is exactly "does the footprint,
    // seen from the cut direction, stay inside the object's section here". On a
    // flat cut the projection is the identity and this is the original test.
    for (const Vec3f& vertex : vertices) {
        if (m_c->object_clipper()) {
            const Vec3d vtx = project_onto_cut_plane(vertex.cast<double>());
            int contour_idx = m_c->object_clipper()->is_projection_inside_cut(vtx);
            bool is_invalid = (contour_idx == -1);
            if (m_part_selection.valid() && ! is_invalid) {
                assert(contour_idx >= 0);
                const std::vector<size_t>& ignored = *(m_part_selection.get_ignored_contours_ptr());
                is_invalid = (std::find(ignored.begin(), ignored.end(), size_t(contour_idx)) != ignored.end());
            }
            if (is_invalid) {
                m_info_stats.outside_cut_contour++;
                return true;
            }
        }
    }

    return false;
}

bool GLGizmoCut3D::is_conflict_for_connector(size_t idx, const CutConnectors& connectors, const Vec3d cur_pos)
{
    if (is_outside_of_cut_contour(idx, connectors, cur_pos))
        return true;

    const CutConnector& cur_connector = connectors[idx];    

    const Transform3d matrix = translation_transform(cur_pos) * connector_rotation_m(cur_connector) *
                               scale_transform(Vec3f(cur_connector.radius, cur_connector.radius, cur_connector.height).cast<double>());
    const BoundingBoxf3 cur_tbb = m_shapes[cur_connector.attribs].model.get_bounding_box().transformed(matrix);

    // check if connector's bounding box is inside the object's bounding box
    if (!m_bounding_box.contains(cur_tbb)) {
        m_info_stats.outside_bb++;
        return true;
    }

    // check if connectors are overlapping 
    for (size_t i = 0; i < connectors.size(); ++i) {
        if (i == idx)
            continue;
        const CutConnector& connector = connectors[i];

        if ((connector.pos - cur_connector.pos).norm() < double(connector.radius + cur_connector.radius)) {
            m_info_stats.is_overlap = true;
            return true;
        }
    }

    return false;
}

void GLGizmoCut3D::check_and_update_connectors_state()
{
    m_info_stats.invalidate();
    m_invalid_connectors_idxs.clear();
    if (CutMode(m_mode) != CutMode::cutPlanar)
        return;
    // THE SELECTION MAY NOT BE RESOLVED YET, and that has to degrade to "nothing
    // to check" rather than to a null dereference. The re-edit and copy paths run
    // inside GLGizmosManager::activate_gizmo(), which calls set_state(On) - and so
    // begin_reedit() / begin_copy() and the recipe they apply - BEFORE open_gizmo()
    // reaches its update_data(). While a recipe is being applied, SelectionInfo can
    // therefore still be the stale one from before the halves were taken out of the
    // model, whose model_object() is null. That is what took the slicer down here,
    // at `mo->cut_connectors`: 0xC0000005 reading null + 0x18.
    const CommonGizmosDataObjects::SelectionInfo* sel = m_c ? m_c->selection_info() : nullptr;
    const ModelObject*                            mo  = sel ? sel->model_object() : nullptr;
    if (mo == nullptr)
        return;
    auto inst_id = sel->get_active_instance();
    // Bounds-checked against THIS object, not merely non-negative: an active
    // instance index outliving a selection change would index past instances.
    if (inst_id < 0 || size_t(inst_id) >= mo->instances.size())
        return;
    const CutConnectors& connectors = mo->cut_connectors;
    const ModelInstance* mi = mo->instances[inst_id];
    const Vec3d& instance_offset = mi->get_offset();
    const double sla_shift       = double(sel->get_sla_shift());

     for (size_t i = 0; i < connectors.size(); ++i) {
        const CutConnector& connector = connectors[i];
        Vec3d pos = connector.pos + instance_offset + sla_shift * Vec3d::UnitZ(); // recalculate connector position to world position
        if (is_conflict_for_connector(i, connectors, pos))
            m_invalid_connectors_idxs.emplace_back(i);
     }

     // PHASE 4: the tilt / flat-patch advisories ride along with the conflict
     // check, so they are refreshed by every edit that could change them (a
     // connector moved, a sheet handle dragged, the plane turned).
     update_curved_connector_warnings();
}

void GLGizmoCut3D::toggle_model_objects_visibility()
{
    bool has_active_volume = false;
    std::vector<std::shared_ptr<SceneRaycasterItem>>* raycasters = m_parent.get_raycasters_for_picking(SceneRaycaster::EType::Volume);
    for (const std::shared_ptr<SceneRaycasterItem> &raycaster : *raycasters)
        if (raycaster->is_active()) {
            has_active_volume = true;
            break;
        }

    if (m_part_selection.valid() && has_active_volume)
        m_parent.toggle_model_objects_visibility(false);
    else if (!m_part_selection.valid() && !has_active_volume) {
        const Selection& selection = m_parent.get_selection();
        const ModelObjectPtrs& model_objects = selection.get_model()->objects;
        m_parent.toggle_model_objects_visibility(true, model_objects[selection.get_object_idx()], selection.get_instance_idx());        
    }
}

void GLGizmoCut3D::render_connectors()
{
    ::glEnable(GL_DEPTH_TEST);

    if (cut_line_processing() ||
        CutMode(m_mode) != CutMode::cutPlanar ||
        m_connector_mode == CutConnectorMode::Auto || !m_c->selection_info())
        return;

    // Same guard as check_and_update_connectors_state(): the pool can be showing a
    // selection that no longer exists (see the note there), and a frame drawn in
    // that window must render nothing rather than dereference null.
    const ModelObject* mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    auto inst_id = m_c->selection_info()->get_active_instance();
    if (inst_id < 0 || size_t(inst_id) >= mo->instances.size())
        return;
    const CutConnectors& connectors = mo->cut_connectors;
    if (connectors.size() != m_selected.size()) {
        // #ysFIXME
        clear_selection();
        m_selected.resize(connectors.size(), false);
    }

    ColorRGBA render_color = CONNECTOR_DEF_COLOR;

    const ModelInstance* mi = mo->instances[inst_id];
    const Vec3d& instance_offset = mi->get_offset();
    const double sla_shift       = double(m_c->selection_info()->get_sla_shift());

    const bool looking_forward = is_looking_forward();

    for (size_t i = 0; i < connectors.size(); ++i) {
        const CutConnector& connector = connectors[i];

        float height = connector.height;
        // recalculate connector position to world position
        Vec3d pos = connector.pos + instance_offset + sla_shift * Vec3d::UnitZ();

        // First decide about the color of the point.
        assert(std::is_sorted(m_invalid_connectors_idxs.begin(), m_invalid_connectors_idxs.end()));
        const bool conflict_connector = std::binary_search(m_invalid_connectors_idxs.begin(), m_invalid_connectors_idxs.end(), i);
        if (conflict_connector)
            render_color = CONNECTOR_ERR_COLOR;
        else // default connector color
            render_color = connector.attribs.type == CutConnectorType::Dowel ? DOWEL_COLOR          : PLAG_COLOR;

        if (!m_connectors_editing)
            render_color = CONNECTOR_ERR_COLOR;
        else if (size_t(m_hover_id - m_connectors_group_id) == i)
            render_color = conflict_connector ? HOVERED_ERR_COLOR :
                           connector.attribs.type == CutConnectorType::Dowel ? HOVERED_DOWEL_COLOR  : HOVERED_PLAG_COLOR;
        else if (m_selected[i])
            render_color = connector.attribs.type == CutConnectorType::Dowel ? SELECTED_DOWEL_COLOR : SELECTED_PLAG_COLOR;

        const Camera& camera = wxGetApp().plater()->get_camera();
        if (connector.attribs.type  == CutConnectorType::Dowel &&
            connector.attribs.style == CutConnectorStyle::Prism) {
            if (m_connectors_editing) {
                height = 0.05f;
                if (!looking_forward)
                    pos += 0.05 * m_clp_normal;
            }
            else {
                if (looking_forward)
                    pos -= static_cast<double>(height) * m_clp_normal;
                else
                    pos += static_cast<double>(height) * m_clp_normal;
                height *= 2;
            }
        }
        else if (!looking_forward)
            pos += 0.05 * m_clp_normal;

        // PHASE 4: the connector's own frame - the sheet's local frame at its
        // (u,v) on a curved cut, m_rotation_m on a flat one (and on a flat sheet
        // the two are the same matrix, so nothing about the flat path moves).
        const Transform3d view_model_matrix = camera.get_view_matrix() * translation_transform(pos) * connector_rotation_m(connector.pos) *
                                              rotation_transform(-connector.z_angle * Vec3d::UnitZ()) *
                                              scale_transform(Vec3f(connector.radius, connector.radius, height).cast<double>());

        render_model(m_shapes[connector.attribs].model, render_color, view_model_matrix);
    }
}

bool GLGizmoCut3D::can_perform_cut() const
{
    if (! m_invalid_connectors_idxs.empty() || (!m_keep_upper && !m_keep_lower) || m_connectors_editing)
        return false;

    if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
        return has_valid_groove();

    // DRAW: there has to BE a usable line. The same gate shape has_valid_groove()
    // uses - a mode's own precondition, checked here so the Cut button greys out
    // rather than the cut failing after the click.
    //
    // An EMPTY SIDE is a refusal here, unlike on the curved cut. The curved cut
    // produces the one half that has material, which is a sensible thing to get
    // from a plane that misses part of the object; a stroke that does not separate
    // the part gives back the whole part and nothing else, which is not a cut at
    // all.
    if (is_draw_surface())
        return m_draw_stroke.valid() && !m_draw_upper_empty && !m_draw_lower_empty;

    if (m_part_selection.valid())
        return ! m_part_selection.is_one_object();

    return true;
}

bool GLGizmoCut3D::has_valid_groove() const
{
    if (CutMode(m_mode) != CutMode::cutTongueAndGroove)
        return true;

    const float flaps_width = -2.f * m_groove.depth / tan(m_groove.flaps_angle);
    if (flaps_width > m_groove.width)
        return false;

    const Selection& selection  = m_parent.get_selection();
    const auto&list = selection.get_volume_idxs();
    // is more volumes selected?
    if (list.empty())
        return false;

    const Transform3d cp_matrix = translation_transform(m_plane_center) * m_rotation_m;

    for (size_t id = 0; id < m_groove_vertices.size(); id += 2) {
        const Vec3d beg = cp_matrix * m_groove_vertices[id];
        const Vec3d end = cp_matrix * m_groove_vertices[id + 1];

        bool intersection = false;
        for (const unsigned int volume_idx : list) {
            const GLVolume* glvol = selection.get_volume(volume_idx);
            if (!glvol->is_modifier && 
                glvol->mesh_raycaster->intersects_line(beg, end - beg, glvol->world_matrix())) {
                intersection = true;
                break;
            }
        }
        if (!intersection)
            return false;
    }

    return true;
}

bool GLGizmoCut3D::has_valid_contour() const
{
    const auto clipper = m_c->object_clipper();
    return clipper && clipper->has_valid_contour();
}

void GLGizmoCut3D::apply_connectors_in_model(ModelObject* mo, int &dowels_count)
{
    if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
        return;
    if (m_connector_mode == CutConnectorMode::Manual) {
        clear_selection();

        // PHASE 3: the cut thickness. A connector's job is to hold the two halves
        // together, so with a kerf it has to SPAN the gap as well as reach into
        // both halves - its length grows by the thickness, and the pocket depth
        // that gets cut into each half is then measured from that half's OFFSET
        // face, not from the mid surface. Both fall out of one change:
        //
        //   - the length grows by t, so the reach into each half is unchanged;
        //   - the centre moves to the middle of the LENGTHENED body, which for a
        //     Plug (which sits entirely on the upper side of the plane today)
        //     means starting at the LOWER face rather than at the mid surface.
        //
        // A Dowel already straddles the plane symmetrically (its height is
        // doubled below), so lengthening it by t keeps it centred and no shift
        // is needed. A Flexi joint opens its own gap and the kerf is added to
        // THAT instead, inside perform_with_flexi_joints() - see CutUtils.cpp.
        double face_lo = 0.0, face_hi = 0.0;
        cut_thickness_faces(face_lo, face_hi);
        const double kerf = face_hi - face_lo;

        // PHASE 4: on a curved cut every connector gets its OWN frame - the
        // sheet's local frame at its (u,v) - and its own normal, so the kerf's
        // centre shift below is measured along the surface normal there rather
        // than along the plane normal. On a flat cut (or a flat sheet) the frame
        // IS m_rotation_m and the normal IS m_cut_normal, so the flat path is
        // untouched and a curved-but-flat cut produces the same volumes.
        for (CutConnector&connector : mo->cut_connectors) {
            // Re-derive from the sheet at bake time so the connector follows any
            // edit made since it was placed, then BAKE it: from here on the
            // connector carries a plain position and rotation, which is what the
            // cut path and the 3MF both already understand.
            connector.pos        = connector_pos_on_sheet(connector.pos);
            connector.rotation_m = connector_rotation_m(connector.pos);
            const Vec3d conn_normal = connector.rotation_m.linear() * Vec3d::UnitZ();

            if (connector.attribs.type == CutConnectorType::FlexiJoint) {
                // The flexi bodies straddle the cut plane by construction: no centre shift.
                // The kerf reaches them through the joint's own gap, not here.
            }
            else if (connector.attribs.type == CutConnectorType::Dowel) {
                if (connector.attribs.style == CutConnectorStyle::Prism)
                    connector.height *= 2;
                // Symmetric about the plane, so it only has to get longer.
                connector.height += float(kerf);
                dowels_count ++;
            }
            else {
                // calculate shift of the connector center regarding to the position on the cut plane
                // With a kerf the body starts at the lower face and is `kerf` longer,
                // so its centre lands at face_lo + height/2 rather than at height/2.
                // PHASE 4: measured along the CONNECTOR's own normal - on a curved
                // cut that is the sheet normal at its (u,v), which is the direction
                // the body actually points, so the plug still bridges the gap when
                // the surface is tilted under it.
                connector.height += float(kerf);
                connector.pos += conn_normal * (face_lo + 0.5 * double(connector.height));
            }
        }
        apply_cut_connectors(mo, _u8L("Connector"));
    }
}

Transform3d GLGizmoCut3D::get_cut_matrix(const Selection& selection)
{
    const int instance_idx = selection.get_instance_idx();
    const int object_idx = selection.get_object_idx();
    ModelObject* mo = selection.get_model()->objects[object_idx];
    if (!mo)
        return Transform3d::Identity();

    // m_cut_z is the distance from the bed. Subtract possible SLA elevation.
    const double sla_shift_z = selection.get_first_volume()->get_sla_shift_z();

    const Vec3d instance_offset = mo->instances[instance_idx]->get_offset();
    Vec3d cut_center_offset = m_plane_center - instance_offset;
    cut_center_offset[Z] -= sla_shift_z;

    return translation_transform(cut_center_offset) * m_rotation_m;
}

void update_object_cut_id(CutObjectBase& cut_id, ModelObjectCutAttributes attributes, const int dowels_count)
{
    // we don't save cut information, if result will not contains all parts of initial object
    if (!attributes.has(ModelObjectCutAttribute::KeepUpper) ||
        !attributes.has(ModelObjectCutAttribute::KeepLower) ||
        attributes.has(ModelObjectCutAttribute::InvalidateCutInfo))
        return;

    if (cut_id.id().invalid())
        cut_id.init();
    // increase check sum, if it's needed
    {
        int cut_obj_cnt = -1;
        if (attributes.has(ModelObjectCutAttribute::KeepUpper))    cut_obj_cnt++;
        if (attributes.has(ModelObjectCutAttribute::KeepLower))    cut_obj_cnt++;
        if (attributes.has(ModelObjectCutAttribute::CreateDowels)) cut_obj_cnt+= dowels_count;
        if (cut_obj_cnt > 0)
            cut_id.increase_check_sum(size_t(cut_obj_cnt));
    }
}

static void check_objects_after_cut(const ModelObjectPtrs& objects)
{
    std::vector<std::string> err_objects_names;
    for (const ModelObject* object : objects) {
        std::vector<std::string> connectors_names;
        connectors_names.reserve(object->volumes.size());
        for (const ModelVolume* vol : object->volumes)
            if (vol->cut_info.is_connector)
                connectors_names.push_back(vol->name);
        const size_t connectors_count = connectors_names.size();
        sort_remove_duplicates(connectors_names);
        if (connectors_count != connectors_names.size())
            err_objects_names.push_back(object->name);
    }
    if (err_objects_names.empty())
        return;

    wxString names = from_u8(err_objects_names[0]);
    for (size_t i = 1; i < err_objects_names.size(); i++)
        names += ", " + from_u8(err_objects_names[i]);
    WarningDialog(wxGetApp().plater(), format_wxstr("Objects(%1%) have duplicated connectors. "
                                "Some connectors may be missing in slicing result.\n"
                                "Please report to PrusaSlicer team in which scenario this issue happened.\n"
                                "Thank you.", names)).ShowModal();
}

void synchronize_model_after_cut(Model& model, const CutObjectBase& cut_id)
{
    for (ModelObject* obj : model.objects)
        if (obj->is_cut() && obj->cut_id.has_same_id(cut_id) && !obj->cut_id.is_equal(cut_id))
            obj->cut_id.copy(cut_id);
}

// ---------------------------------------------------------------------------
// RE-EDITABLE CUTS
// ---------------------------------------------------------------------------

// The mesh a cut actually operates on: every MODEL_PART volume that is not a cut
// connector, merged and taken into the OBJECT's frame. Connector volumes are
// excluded deliberately - they are generated BY the cut from the connector
// definitions the recipe already stores, so keeping them in the pre-cut mesh
// would bake one generation of them into the next re-cut's input.
TriangleMesh GLGizmoCut3D::object_part_mesh(const ModelObject* mo)
{
    TriangleMesh out;
    if (!mo)
        return out;
    for (const ModelVolume* v : mo->volumes) {
        if (v->type() != ModelVolumeType::MODEL_PART || v->is_cut_connector())
            continue;
        TriangleMesh m = v->mesh();
        m.transform(v->get_matrix());
        out.merge(m);
    }
    return out;
}

// Everything the gizmo currently holds, as a recipe.
//
// The plane is recorded in the OBJECT's frame, not the world: the halves get
// moved, rotated and re-arranged on the bed after a cut, and a world-frame plane
// would then describe a cut through empty space next time the project opens.
CutRecipe GLGizmoCut3D::build_recipe_from_gizmo(const ModelObject* mo, const TriangleMesh& mesh) const
{
    CutRecipe r;
    r.version = CutRecipeVersion;

    // Which surface produced this cut. The gizmo's CutMode and CutSurfaceMode
    // cross into the one choice that decides which Cut::perform_* runs - exactly
    // the decision perform_cut() makes, kept in step with it.
    if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
        r.kind = CutRecipeKind::Groove;
    else if (is_draw_surface() && m_draw_stroke.valid())
        r.kind = CutRecipeKind::Drawn;
    else if (is_curved_surface() && !m_curved_sheet.is_flat())
        r.kind = CutRecipeKind::Curved;
    else
        r.kind = CutRecipeKind::Plane;

    // The cut frame, taken into the object frame. m_plane_center is in world
    // coordinates; the instance offset is what separates the two.
    Vec3d instance_offset = Vec3d::Zero();
    if (mo && !mo->instances.empty())
        instance_offset = mo->instances.front()->get_offset();
    r.plane_center = m_plane_center - instance_offset;
    r.rotation_m   = m_rotation_m;

    // The surface.
    if (r.kind == CutRecipeKind::Curved) {
        r.sheet.nx          = m_curved_sheet.nx();
        r.sheet.ny          = m_curved_sheet.ny();
        r.sheet.half_size_u = m_curved_sheet.half_size_u();
        r.sheet.half_size_v = m_curved_sheet.half_size_v();
        r.sheet.values      = m_curved_sheet.values();
    }
    else if (r.kind == CutRecipeKind::Drawn) {
        // The RAW samples, not the finished path: finish() regenerates the path,
        // the smoothing and the binormals from them deterministically, and storing
        // both would let the two disagree. See CutRecipeStroke.
        // 2026-09-12: from the CHAIN, which also carries the per-stroke ranges the
        // chain's own undo works off - so reopening a cut gives back a line whose
        // Ctrl+Z takes back one stroke, not the whole thing. Version 2 of the schema;
        // a version 1 file has no ranges and loads as one stroke.
        r.stroke = cut_recipe_stroke_from_chain(m_draw_chain, double(m_draw_smoothing));

        r.draw_direction   = m_draw_direction;
        r.draw_view_dir    = m_draw_params.view_dir;
        r.draw_extension   = double(m_draw_extension);
        r.draw_angle_deg   = double(m_draw_angle);
        r.draw_through_all = m_draw_params.through_all;
        r.draw_depth       = double(m_draw_depth);
        r.draw_ext_angle_set = m_draw_ext_angle_on;
        r.draw_ext_angle_deg = double(m_draw_ext_angle);
    }
    else if (r.kind == CutRecipeKind::Groove) {
        r.groove.depth            = m_groove.depth;
        r.groove.width            = m_groove.width;
        r.groove.flaps_angle      = m_groove.flaps_angle;
        r.groove.angle            = m_groove.angle;
        r.groove.depth_init       = m_groove.depth_init;
        r.groove.width_init       = m_groove.width_init;
        r.groove.flaps_angle_init = m_groove.flaps_angle_init;
        r.groove.angle_init       = m_groove.angle_init;
        r.groove.depth_tolerance  = m_groove.depth_tolerance;
        r.groove.width_tolerance  = m_groove.width_tolerance;
    }

    // Shared parameters.
    r.thickness          = CutMode(m_mode) == CutMode::cutTongueAndGroove ? 0.0 : double(m_cut_thickness);
    r.thickness_offset   = cut_thickness_offset();
    r.keep_upper         = m_keep_upper;
    r.keep_lower         = m_keep_lower;
    r.keep_as_parts      = m_keep_as_parts;
    r.place_on_cut_upper = m_place_on_cut_upper;
    r.place_on_cut_lower = m_place_on_cut_lower;
    r.rotate_upper       = m_rotate_upper;
    r.rotate_lower       = m_rotate_lower;
    r.upper_visibility   = int(m_upper_visibility);
    r.lower_visibility   = int(m_lower_visibility);

    // The connector DEFINITIONS, as they stand before apply_connectors_in_model()
    // bakes them - a re-cut regenerates the volumes from these, so what has to be
    // stored is the unbaked form.
    if (mo)
        cut_recipe_connectors_from_model(mo->cut_connectors, r.connectors);

    // The pre-cut mesh, and the hash that names its blob in the 3MF.
    r.mesh      = mesh;
    r.mesh_hash = cut_recipe_mesh_hash(cut_recipe_mesh_to_blob(r.mesh));

    return r;
}

// Put every control back from a recipe.
void GLGizmoCut3D::apply_recipe_to_gizmo(const CutRecipe& recipe)
{
    // Shared parameters first: the surface setup below reads some of them.
    m_cut_thickness        = float(recipe.thickness);
    m_cut_thickness_offset = int(recipe.thickness_offset);
    m_keep_upper           = recipe.keep_upper;
    m_keep_lower           = recipe.keep_lower;
    m_keep_as_parts        = recipe.keep_as_parts;
    m_place_on_cut_upper   = recipe.place_on_cut_upper;
    m_place_on_cut_lower   = recipe.place_on_cut_lower;
    m_rotate_upper         = recipe.rotate_upper;
    m_rotate_lower         = recipe.rotate_lower;
    m_upper_visibility     = SideVisibility(std::max(0, std::min(2, recipe.upper_visibility)));
    m_lower_visibility     = SideVisibility(std::max(0, std::min(2, recipe.lower_visibility)));
    apply_side_visibility();

    // The mode.
    const size_t want_mode = recipe.kind == CutRecipeKind::Groove ? size_t(CutMode::cutTongueAndGroove)
                                                                  : size_t(CutMode::cutPlanar);
    if (m_mode != want_mode)
        switch_to_mode(want_mode);

    // The cut frame. The recipe holds it in the OBJECT frame; the gizmo works in
    // the world, and the stand-in object is placed at the instance offset the
    // halves had, so the two differ by exactly that offset.
    Vec3d instance_offset = Vec3d::Zero();
    if (const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr)
        if (!mo->instances.empty())
            instance_offset = mo->instances.front()->get_offset();

    m_rotation_m       = recipe.rotation_m;
    m_start_dragging_m = m_rotation_m;

    // THE CLAMP BOX IS REBUILT, NOT NUDGED.
    //
    // m_transformed_bounding_box is the selected object's bounding box expressed in
    // the PLANE's frame, relative to the plane centre, and set_center_pos() uses it
    // to decide whether a new centre is allowed: a push/pull is refused once the box
    // no longer straddles z == 0 (only a move back towards m_bb_center is let
    // through). So it has to describe THIS object under THIS rotation.
    //
    // set_center(..., update_tbb = true) does not do that. It TRANSLATES the box it
    // already holds along the current plane normal - an increment that is only valid
    // when neither the object nor the rotation changed. Here both just did:
    // m_rotation_m was replaced on the line above, and the selection is the copy's
    // target (or the re-edit's stand-in), not whatever the gizmo was last open on.
    // The box kept was the one update_bb() had built about the object's own centre,
    // in the old frame, so the plane hit an invisible stop part-way through the
    // target - the owner's "push/pull refuses to go any further after Copy cut
    // to...". Ctrl+Z cleared it precisely because the undo restore recomputes the
    // box (see the m_ar_plane_center path, which does this same pair).
    //
    // Recomputing from the live selection is what every other frame change here
    // does, so do the same.
    const Vec3d wanted_center  = recipe.plane_center + instance_offset;
    m_transformed_bounding_box = transformed_bounding_box(wanted_center, m_rotation_m);
    set_center_pos(wanted_center);
    // ... and the rest of what set_center() would have done, now the centre is in
    // place: the connector validity and the clipper both read it.
    check_and_update_connectors_state();
    update_clipper();
    m_ar_plane_center  = m_plane_center;

    // The surface.
    switch (recipe.kind) {
    case CutRecipeKind::Curved: {
        m_surface_mode = CutSurfaceMode::Curved;
        m_curved_sheet = recipe.curved_sheet();
        m_curved_nx    = m_curved_sheet.nx();
        m_curved_ny    = m_curved_sheet.ny();
        m_curved_square = (m_curved_nx == m_curved_ny);
        // The fit must NOT run over this: it would resize the sheet's domain and
        // resample the values the recipe just restored, which is a different
        // surface from the one the halves were cut with.
        m_curved_res_user_set = true;
        m_curved_fit_valid    = true;
        m_curved_fit_pending  = false;
        m_curved_fit_center   = m_plane_center;
        m_curved_fit_rotation = m_rotation_m;
        invalidate_curved_sheet();
        update_curved_empty_sides();
        // Seed the gizmo-local undo stack with the loaded surface, so the first
        // Ctrl+Z in a re-edit returns to what was loaded rather than to a flat
        // sheet (design point 5).
        clear_curved_undo();
        push_curved_undo();
        break;
    }
    case CutRecipeKind::Drawn: {
        m_surface_mode    = CutSurfaceMode::Draw;
        m_draw_smoothing  = float(recipe.stroke.smoothing);
        m_draw_extension  = float(recipe.draw_extension);
        m_draw_angle      = float(recipe.draw_angle_deg);
        m_draw_depth      = float(recipe.draw_depth);
        // A version <= 3 recipe has the flag false, which is "continue the band" -
        // the skirt it was cut with - so an old cut reopens as exactly itself.
        m_draw_ext_angle_on = recipe.draw_ext_angle_set;
        m_draw_ext_angle    = float(recipe.draw_ext_angle_deg);
        m_draw_direction  = recipe.draw_direction;
        m_draw_params     = recipe.draw_params();
        // Rebuild the stroke from the RAW samples, exactly as capture would have
        // left it, then re-run finish() with the recipe's own smoothing - which is
        // what makes the reproduced path identical to the one that was cut with.
        // 2026-09-12: rebuild the CHAIN from the stored samples and ranges, then let
        // refresh_draw_stroke() derive the stroke from it - the one path a stroke is ever
        // produced by, so a reopened cut and a freshly drawn one cannot diverge.
        cut_recipe_stroke_to_chain(recipe.stroke, m_draw_chain);
        m_draw_capture.clear();
        m_draw_snap_armed = false;
        sync_draw_points();
        invalidate_draw_stroke();
        refresh_draw_stroke();
        // Same seeding as the sheet's.
        clear_draw_undo();
        push_draw_undo();
        break;
    }
    case CutRecipeKind::Groove: {
        m_groove.depth            = recipe.groove.depth;
        m_groove.width            = recipe.groove.width;
        m_groove.flaps_angle      = recipe.groove.flaps_angle;
        m_groove.angle            = recipe.groove.angle;
        m_groove.depth_init       = recipe.groove.depth_init;
        m_groove.width_init       = recipe.groove.width_init;
        m_groove.flaps_angle_init = recipe.groove.flaps_angle_init;
        m_groove.angle_init       = recipe.groove.angle_init;
        m_groove.depth_tolerance  = recipe.groove.depth_tolerance;
        m_groove.width_tolerance  = recipe.groove.width_tolerance;
        update_plane_model();
        break;
    }
    case CutRecipeKind::Plane:
    default:
        m_surface_mode = CutSurfaceMode::Flat;
        break;
    }

    // The connectors. These are DEFINITIONS: the volumes are regenerated from
    // them by the re-cut, so what goes back is the unbaked form the recipe holds.
    if (ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr) {
        cut_recipe_connectors_to_model(recipe.connectors, mo->cut_connectors);
        m_selected.assign(mo->cut_connectors.size(), false);
        m_selected_count = 0;
    }

    update_clipper();
    m_parent.request_extra_frame();
}

bool GLGizmoCut3D::arm_reedit(const CutRecipe& recipe, const std::vector<ObjectID>& object_ids)
{
    // A recipe with no pre-cut mesh (or from a schema this build does not know)
    // cannot reproduce the cut. Refuse here rather than opening a gizmo that
    // would then quietly cut the wrong thing.
    if (!recipe.valid() || object_ids.empty())
        return false;
    m_reedit_pending            = true;
    m_reedit_pending_recipe     = recipe;
    m_reedit_pending_object_ids = object_ids;
    return true;
}


// ---------------------------------------------------------------------------
// "COPY CUT TO..." - the third way to arm the gizmo.
//
// Beside "a fresh cut" (nothing armed) and "Edit cut..." (arm_reedit), this is
// "start a fresh cut on THIS object, with THAT object's cut already set up".
//
// WHAT IT IS NOT: it is not a re-edit. A re-edit takes the halves out of the
// model and puts the pre-cut stand-in back in their place, because it is going
// to REPLACE those halves. A copy has no halves to replace - the target is being
// cut, for the first time as far as this session is concerned - so it must run
// on the TARGET'S OWN MESH, in place, with the ordinary gizmo session the
// ordinary Cut button gives. m_reedit_active therefore stays false, no stand-in
// is created, and perform_cut() writes a fresh recipe on the target's halves
// under the usual "Keep cut editable" rule, referencing the TARGET's own pre-cut
// mesh hash. No cross-object blob aliasing, no re-edit bookkeeping.
//
// THE TARGET MAY ALREADY HAVE A RECIPE, and that changes nothing here. A copy
// onto an object that was itself cut before simply REPLACES the gizmo state -
// the plane, the surface, the settings and the connectors the panel is showing -
// and the cut then proceeds as an ordinary cut of whatever that object is now.
// It deliberately does NOT route into arm_reedit() to un-cut the target first:
// "copy this cut onto that" means "make that cut here", not "undo what that
// object is and remake it". The user who wants the latter has "Edit cut..." on
// the target, which is exactly that operation. The target's own old recipe is
// left alone until the cut commits, at which point the fresh one written by
// perform_cut() supersedes it the way any re-cut's does.
//
// THE FRAME. The recipe's plane_center and connector positions are in the SOURCE
// object's own frame, and they are used in the TARGET's object frame unchanged -
// design section 2's "object-local as-is". For the case this feature is for, two
// halves cut from one mesh, that is exactly right: they share the frame they were
// cut in, so the source's plane already IS the target's plane, position for
// position. For two unrelated objects sitting at different poses on the plate it
// is a starting point the user then drags, which is why the submenu says so.
// (Design section 4's "match world position instead" option is deliberately out
// of scope for v1.)
//
// Like arm_reedit, this only PARKS the request: the gizmo's own opening path
// flattens the session state, so the recipe is applied after that rather than
// before, or the reset would wipe it.
bool GLGizmoCut3D::arm_copy(const CutRecipe& recipe)
{
    // Unlike a re-edit, no pre-cut mesh is needed - the target's own mesh is what
    // gets cut - so recipe.valid() is deliberately NOT required here. What is
    // required is that the surface it describes can actually be set up.
    if (!cut_recipe_kind_valid(int(recipe.kind)))
        return false;
    if (recipe.kind == CutRecipeKind::Curved && !recipe.sheet.valid())
        return false;
    if (recipe.kind == CutRecipeKind::Drawn && recipe.stroke.samples.size() < size_t(DrawCutStroke::MinSamples))
        return false;

    m_copy_pending        = true;
    m_copy_pending_recipe = recipe;
    // The mesh is the SOURCE's pre-cut shape and has no business on the target.
    // Dropping it here rather than at the call site is what guarantees nothing
    // downstream can accidentally cut the target with the source's geometry.
    m_copy_pending_recipe.mesh = TriangleMesh();
    m_copy_pending_recipe.mesh_hash.clear();
    return true;
}

// Consume a parked copy. Called from on_set_state() at the same point a parked
// re-edit is consumed, and for the same reason.
void GLGizmoCut3D::begin_copy()
{
    if (!m_copy_pending)
        return;
    m_copy_pending = false;

    // The pool first, for the same reason begin_reedit() refreshes it: this runs
    // from on_set_state(On), before open_gizmo()'s update_data(), so
    // m_c->selection_info() still describes whatever was selected when the gizmo
    // was last open rather than the target ObjectList::copy_cut_to() just selected.
    // apply_recipe_to_gizmo() reads exactly that to place the plane and to write
    // the connectors, and its set_center() reaches
    // check_and_update_connectors_state(), which is where a null model_object()
    // crashed - the "Copy cut to... (this object)" report, same fault as
    // "Edit cut...".
    refresh_common_data();

    // The ordinary session, with every control pre-populated. That is the whole
    // of it: apply_recipe_to_gizmo() reads the CURRENT selection's instance offset
    // when it puts the plane back, so pointing it at the target object is all the
    // frame mapping there is.
    apply_recipe_to_gizmo(m_copy_pending_recipe);
    m_copy_pending_recipe = CutRecipe();

    // The same caches mirror_cut() refreshes, for the same reason: the plane frame
    // just moved to somewhere the cached meshes in it know nothing about.
    if (m_surface_mode == CutSurfaceMode::Draw) {
        invalidate_draw_pick_mesh();
        refresh_draw_stroke();
    }
    if (m_surface_mode == CutSurfaceMode::Curved) {
        update_curved_connector_warnings();
        m_curved_hover_ctl = m_curved_drag_ctl = -1;
    }
    if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
        reset_cut_by_contours();

    check_and_update_connectors_state();
    update_raycasters_for_picking();
    m_parent.set_as_dirty();
}
// Re-read the common gizmo data pool against the CURRENT canvas selection.
//
// See the declaration for why this is needed at all: begin_reedit() and
// begin_copy() run from on_set_state(On), which activate_gizmo() calls before
// open_gizmo() reaches its own update_data(), so a selection they set is not yet
// visible through m_c->selection_info(). Updating with the gizmo's own
// requirements is exactly what GLGizmosManager::update_data() would do for this
// gizmo; doing it early is idempotent, and the manager's later call is then a
// harmless second update.
void GLGizmoCut3D::refresh_common_data()
{
    if (m_c)
        m_c->update(get_requirements());
}

// Run the parked re-edit's model surgery now, before the gizmo is opened. See the
// declaration for why the ordering matters.
bool GLGizmoCut3D::begin_reedit_now()
{
    if (!m_reedit_pending)
        return false;
    begin_reedit();
    return m_reedit_active;
}

// Become a re-edit session.
//
// The halves are REMOVED from the model and a stand-in carrying the pre-cut mesh
// is added in their place, at the transform the first half had. Removing rather
// than merely hiding them is what lets the selection - and therefore every
// raycaster, the clipper and the cut itself - address the pre-cut mesh as an
// ordinary object, so the whole gizmo works unchanged during a re-edit.
//
// The whole thing sits inside ONE undo snapshot taken by the caller, so Cancel
// and Ctrl+Z both restore the halves in a single step.
void GLGizmoCut3D::begin_reedit()
{
    if (!m_reedit_pending)
        return;
    m_reedit_pending = false;

    Plater* plater = wxGetApp().plater();
    if (!plater)
        return;
    Model& model = plater->model();

    // Find the halves that are still in the model. One of them may have been
    // deleted since the cut; the re-cut still works and produces both halves
    // again, but the panel has to say so (design point 2).
    std::vector<int> idxs;
    for (const ObjectID& id : m_reedit_pending_object_ids)
        for (size_t i = 0; i < model.objects.size(); ++i)
            if (model.objects[i]->id() == id) {
                idxs.push_back(int(i));
                break;
            }
    if (idxs.empty())
        return;
    m_reedit_one_half_missing = idxs.size() < m_reedit_pending_object_ids.size() ||
                                m_reedit_pending_object_ids.size() < 2;

    // The transform to place the stand-in with: the first surviving half's own
    // instance transform, so the pre-cut mesh appears where the cut object is and
    // the re-cut result lands there too rather than back at the origin.
    const ModelObject* first      = model.objects[size_t(idxs.front())];
    const std::string  proxy_name = first->name;

    // The transform the stand-in is placed with, taken as plain DATA before the
    // halves are deleted. `first` does not survive that deletion, and the stashed
    // copy cannot be found by id either: Model::add_object(const ModelObject&)
    // clones rather than copies, and a clone is given fresh ObjectIDs by
    // assign_clone() (which asserts the new id DIFFERS from the old). Copying the
    // transformation out now sidesteps both problems.
    const bool             first_has_instance = !first->instances.empty();
    const Geometry::Transformation first_instance_trafo =
        first_has_instance ? first->instances.front()->get_transformation() : Geometry::Transformation();
    const bool             first_printable    = first_has_instance ? first->instances.front()->printable : true;

    m_reedit_recipe = m_reedit_pending_recipe;
    m_reedit_object_ids.clear();
    for (int i : idxs)
        m_reedit_object_ids.push_back(model.objects[size_t(i)]->id());

    // THE HALVES GO FIRST, and the stand-in is added afterwards. The order is the
    // whole of the fix for the "Edit cut..." crash: the object LIST and the model
    // are addressed by the same index, and the only moment they agree is before
    // either is touched. Adding the stand-in to the model first (as this did) made
    // the model one longer than the list, so every delete_object_from_list(i)
    // below removed the WRONG ROW - and on an object that is not the last on the
    // plate the list and the model then disagreed about what every later index
    // meant, which is what took the slicer down as soon as anything resolved a
    // selection against it (ObjectList::part_selection_changed() indexes
    // (*m_objects)[obj_idx] unguarded).
    //
    // Deleting first keeps the two in lockstep: descending indices, each removed
    // from the model and the list together, and only then is the stand-in appended
    // to BOTH. The copies stashed here are what Cancel puts back - see
    // m_reedit_stash for why this does not go through the plater's undo.
    m_reedit_stash.clear_objects();
    std::sort(idxs.begin(), idxs.end(), std::greater<int>());
    for (int i : idxs) {
        m_reedit_stash.add_object(*model.objects[size_t(i)]);
        model.delete_object(size_t(i));
        wxGetApp().obj_list()->delete_object_from_list(size_t(i));
    }

    // The stand-in, now that the indices it will be appended at agree.
    ModelObject* proxy = model.add_object();
    proxy->name        = proxy_name;
    // An explicit COPY, moved in: the three-argument overload takes a
    // TriangleMesh&&, and the recipe's own mesh has to survive this - it is what
    // the re-cut runs against and what the recipe written forward carries.
    // modify_to_center_geometry is false because the mesh is already in the
    // object frame; letting add_volume re-centre it would shift the cut plane
    // out from under the recipe's own coordinates.
    proxy->add_volume(TriangleMesh(m_reedit_pending_recipe.mesh), ModelVolumeType::MODEL_PART, false);
    // The instance, rebuilt from the transformation captured above rather than
    // from a ModelInstance that no longer exists.
    ModelInstance* proxy_instance = proxy->add_instance();
    if (first_has_instance) {
        proxy_instance->set_transformation(first_instance_trafo);
        proxy_instance->printable = first_printable;
    }
    // The recipe's connector definitions travel with it, so the gizmo shows the
    // connectors the cut was made with.
    cut_recipe_connectors_to_model(m_reedit_pending_recipe.connectors, proxy->cut_connectors);
    // The proxy is NOT itself a cut half - it is the original - so it carries no
    // cut_id. It does carry the recipe, which is what the re-cut writes forward.
    proxy->cut_recipe = m_reedit_pending_recipe;

    m_reedit_proxy_id = proxy->id();

    // ... and into the LIST, which model.add_object() knows nothing about. Without
    // this the stand-in has no row at all, so the list is one SHORT of the model
    // and every index past it means a different object in each - the same
    // desynchronisation the delete order above avoids, arrived at from the other
    // side. call_selection_changed is false because the selection is set
    // deliberately below.
    wxGetApp().obj_list()->add_object_to_list(model.objects.size() - 1, false);

    m_reedit_active = true;

    // SHOW THE STAND-IN, THEN SELECT IT - and in that order, because the canvas
    // has no GLVolumes for an object the model has only just gained.
    //
    // plater->update() runs GLCanvas3D::reload_scene(), which rebuilds the
    // GLVolume vector. Two things in there matter here. It calls
    // Selection::volumes_changed(), which DROPS every selected volume that no
    // longer exists - and the halves were deleted above, so the selection empties.
    // And with the selection empty it then runs reset_all_states() (GLCanvas3D.cpp
    // ~3087, "If no object is selected, deactivate the active gizmo") plus
    // refresh_on_off_state(), which turns this gizmo back OFF while our
    // on_set_state(On) is still on the stack.
    //
    // That is the "the object disappears and nothing else happens" the owner saw:
    // the halves went, the stand-in arrived, and the Cut gizmo was switched off
    // again before it ever rendered. Selecting BEFORE the reload cannot help
    // either - Selection::add_object() resolves GLVolumes by object index, and
    // there are none for the stand-in until the reload has run.
    //
    // So: reload first (the stand-in gets its volumes), select it second (now
    // there is something to select), and re-assert the gizmo's own state last,
    // since the reload may have cleared it.
    plater->update();

    Selection& selection = m_parent.get_selection();
    selection.clear();
    int proxy_idx = -1;
    for (size_t i = 0; i < model.objects.size(); ++i)
        if (model.objects[i]->id() == m_reedit_proxy_id) {
            proxy_idx = int(i);
            break;
        }
    if (proxy_idx >= 0) {
        selection.add_object(static_cast<unsigned int>(proxy_idx), true);
        // The object list's own row, so the sidebar agrees with the canvas and a
        // later refresh_on_off_state() sees a consistent selection.
        if (auto* ol = wxGetApp().obj_list())
            ol->select_item(ol->GetModel()->GetItemById(proxy_idx));
    }

    // The common data pool is refreshed so SelectionInfo is the stand-in rather than
    // the halves that have just gone.
    refresh_common_data();

    // Now that the selection is the pre-cut mesh, the gizmo's own bookkeeping can
    // be rebuilt against it.
    update_bb();

    // THE RECIPE IS DELIBERATELY NOT APPLIED HERE. This surgery runs BEFORE the
    // gizmo is opened (begin_reedit_now(), called from ObjectList::edit_cut()), so
    // the gizmo's common data pool is not populated yet: m_c->object_clipper() is
    // still null, and apply_recipe_to_gizmo() reaches it through update_clipper()
    // (0xC0000005 at null + 0x48). on_set_state(On) applies the recipe instead,
    // once the manager has really opened this gizmo and the pool is live.
}

// Undo begin_reedit(): the stand-in goes, the halves come back.
//
// The halves are restored from m_reedit_stash rather than by asking the plater to
// undo. Plater::TakeSnapshot suppresses snapshots only for its own scope, so by
// the time the user cancels, the top of the undo stack is no longer the snapshot
// the menu item took and a single undo() would land somewhere else entirely.
// Putting the stashed objects back is exact, and leaves the undo stack alone - so
// the user's own Ctrl+Z still reaches the "Edit cut" snapshot and does the same
// thing, which is what design point 2 asks for.
void GLGizmoCut3D::cancel_reedit()
{
    if (!m_reedit_active)
        return;
    m_reedit_active = false;

    // THE MODEL WORK IS DEFERRED OUT OF THIS CALL STACK. cancel_reedit() runs from
    // on_set_state(Off), i.e. from inside GLGizmosManager::activate_gizmo(), and
    // the restoration below calls plater->update(). Its reload_scene() reaches
    // refresh_on_off_state() -> activate_gizmo() -> Plater::take_snapshot() while
    // the manager is still mid-transition, and taking an undo snapshot re-entrantly
    // corrupts the stack (observed as 0xC0000005 in
    // UndoRedo::Stack::reduce_noisy_snapshots). Running it after this stack has
    // unwound is both correct and enough: the gizmo is already closing.
    auto restore = [this]() {
    Plater* plater = wxGetApp().plater();
    if (plater) {
        Model& model = plater->model();

        // The stand-in goes.
        for (size_t i = 0; i < model.objects.size(); ++i)
            if (model.objects[i]->id() == m_reedit_proxy_id) {
                model.delete_object(i);
                wxGetApp().obj_list()->delete_object_from_list(i);
                break;
            }

        // ... and the halves come back, in the order they were taken - into the
        // model AND the list together. model.add_object() only does the former, and
        // a half with no row leaves the list short of the model, which is the same
        // index desynchronisation begin_reedit() is careful to avoid: every later
        // index would then mean a different object in each, and the first thing to
        // resolve a selection against them takes the slicer down.
        if (!m_reedit_stash.objects.empty()) {
            for (ModelObject* o : m_reedit_stash.objects) {
                model.add_object(*o);
                wxGetApp().obj_list()->add_object_to_list(model.objects.size() - 1, false);
            }
            plater->update();
            for (size_t i = 0; i < model.objects.size(); ++i)
                wxGetApp().obj_list()->update_info_items(i);
        }
        else {
            plater->update();
        }
    }
    // The bookkeeping is cleared HERE, inside the deferred work, because the stash
    // and the proxy id are what the restoration above reads - clearing them before
    // it ran would leave the halves nowhere to come back from.
    m_reedit_one_half_missing = false;
    m_reedit_object_ids.clear();
    m_reedit_proxy_id         = ObjectID();
    m_reedit_recipe           = CutRecipe();
    m_reedit_stash.clear_objects();
    };
    wxGetApp().CallAfter(restore);
}

void GLGizmoCut3D::render_reedit_notice()
{
    if (!m_reedit_active)
        return;
    m_imgui->text(_L("Editing an existing cut. Perform cut replaces both halves."));
    if (m_reedit_one_half_missing)
        m_imgui->warning_text(_L("One half of this cut is missing from the project. "
                                 "Cutting again will produce both halves."));
    ImGui::Separator();
}

void GLGizmoCut3D::perform_cut(const Selection& selection)
{
    if (!can_perform_cut())
        return;
    const int instance_idx = selection.get_instance_idx();
    const int object_idx = selection.get_object_idx();

    wxCHECK_RET(instance_idx >= 0 && object_idx >= 0, "GLGizmoCut: Invalid object selection");

    Plater* plater = wxGetApp().plater();
    ModelObject* mo = plater->model().objects[object_idx];
    if (!mo)
        return;

    // The curved sheet is session state that on_set_state() flattens when the gizmo
    // closes - and reset_all_gizmos() below closes it. Take the surface (and the
    // Curved/Flat choice) BEFORE that, or the cut would be performed against a sheet
    // that has just been zeroed and come out as the plain flat plane cut.
    const bool           curved_surface = is_curved_surface();
    const CurvedCutSheet curved_sheet   = m_curved_sheet;
    // Same reason for the drawn stroke: on_set_state() clears it when the gizmo
    // closes, and reset_all_gizmos() below closes it.
    const bool           draw_surface   = is_draw_surface();
    const DrawCutStroke  draw_stroke    = m_draw_stroke;
    DrawCutParams        draw_params    = m_draw_params;
    // Same reason: the thickness is session state and reset_all_gizmos() below
    // closes the gizmo. Take it first.
    const double         cut_thickness  = CutMode(m_mode) == CutMode::cutTongueAndGroove ? 0.0 : double(m_cut_thickness);
    const CutThicknessOffset thickness_offset = cut_thickness_offset();

    // RE-EDITABLE CUTS. Build the recipe NOW, for exactly the same reason every
    // other line above does: reset_all_gizmos() below closes the gizmo, and
    // on_set_state() flattens the sheet, the stroke and the thickness on the way
    // out - so a recipe built after it would describe a flat cut with no kerf.
    //
    // The pre-cut mesh is likewise the object AS IT STANDS, before the cut
    // replaces it. In a re-edit the recipe's own mesh is that object (the
    // stand-in carries it), so both paths take it from the same place.
    const bool  write_recipe = m_keep_cut_editable;
    CutRecipe   new_recipe;
    if (write_recipe)
        new_recipe = build_recipe_from_gizmo(mo, object_part_mesh(mo));

    // A re-edit replaces the halves it was opened on. Take their ids before
    // reset_all_gizmos() clears the session, and keep the stand-in's id so the
    // re-cut can remove it along with them.
    const bool            was_reedit        = m_reedit_active;
    std::vector<ObjectID> reedit_replace_ids = m_reedit_object_ids;
    const ObjectID        reedit_proxy_id    = m_reedit_proxy_id;
    // The gizmo is about to close, which runs on_set_state(); the re-edit must be
    // torn down WITHOUT the model rollback cancel_reedit() does, because this is
    // the committing path.
    m_reedit_active           = false;
    m_reedit_one_half_missing = false;
    m_reedit_object_ids.clear();
    m_reedit_proxy_id         = ObjectID();
    // The stashed halves are being REPLACED, not restored, so they go now: holding
    // them past this point would keep a whole object's meshes alive for nothing.
    m_reedit_stash.clear_objects();

    // deactivate CutGizmo and than perform a cut
    m_parent.reset_all_gizmos();

    // perform cut
    {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Cut by Plane"));

        // This shall delete the part selection class and deallocate the memory.
        ScopeGuard part_selection_killer([this]() { m_part_selection = PartSelection(); });

        const bool cut_with_groove = CutMode(m_mode) == CutMode::cutTongueAndGroove;
        const bool cut_drawn       = !cut_with_groove && draw_surface && draw_stroke.valid();
        // A bent sheet wins over a part selection: the contour cut is a plane cut and would
        // silently discard the curve (second route to a flat result, via right-click part picking).
        const bool cut_by_contour = !cut_with_groove && !cut_drawn && m_part_selection.valid() && !(curved_surface && !curved_sheet.is_flat());

        ModelObject* cut_mo = cut_by_contour ? m_part_selection.model_object() : nullptr;
        if (cut_mo)
            cut_mo->cut_connectors = mo->cut_connectors;
        else
            cut_mo = mo;

        int dowels_count = 0;
        const bool has_connectors = !mo->cut_connectors.empty();
        // A flexi joint only works print-in-place, so it forces keep-as-parts (one object,
        // two volumes) no matter what the Cut-to-parts / separate-objects checkboxes say.
        const bool has_flexi = std::any_of(mo->cut_connectors.begin(), mo->cut_connectors.end(),
                                           [](const CutConnector& c) { return c.attribs.type == CutConnectorType::FlexiJoint; });
        // Remember it so on_set_state() can put the after-cut checkboxes (and the connector
        // type) back to their defaults the next time the gizmo opens; see on_set_state().
        if (has_flexi)
            m_flexi_forced_after_cut = true;
        // update connectors pos as offset of its center before cut performing
        apply_connectors_in_model(cut_mo , dowels_count);

        wxBusyCursor wait;

        ModelObjectCutAttributes attributes = only_if(has_connectors ? true : m_keep_upper, ModelObjectCutAttribute::KeepUpper) |
                                              only_if(has_connectors ? true : m_keep_lower, ModelObjectCutAttribute::KeepLower) |
                                              only_if(has_flexi ? true : (has_connectors ? false : m_keep_as_parts), ModelObjectCutAttribute::KeepAsParts) |
                                              only_if(m_place_on_cut_upper, ModelObjectCutAttribute::PlaceOnCutUpper) |
                                              only_if(m_place_on_cut_lower, ModelObjectCutAttribute::PlaceOnCutLower) |
                                              only_if(m_rotate_upper, ModelObjectCutAttribute::FlipUpper) |
                                              only_if(m_rotate_lower, ModelObjectCutAttribute::FlipLower) |
                                              only_if(dowels_count > 0, ModelObjectCutAttribute::CreateDowels) |
                                              only_if(!has_connectors && !cut_with_groove && cut_mo->cut_id.id().invalid(), ModelObjectCutAttribute::InvalidateCutInfo);

        // update cut_id for the cut object in respect to the attributes
        update_object_cut_id(cut_mo->cut_id, attributes, dowels_count);

        // A curved surface replaces the plane cut with a height-field cut. A sheet
        // with every control point at zero routes back into perform_with_plane()
        // inside Cut, so "Curved but untouched" is the flat cut, not an
        // approximation of it.
        const bool cut_curved = !cut_with_groove && !cut_by_contour && curved_surface && !curved_sheet.is_flat();
        if (curved_surface)
            BOOST_LOG_TRIVIAL(warning) << "Curved cut: perform, grid=" << curved_sheet.nx() << "x" << curved_sheet.ny()
                                       << " half_size=" << curved_sheet.half_size_u() << "x" << curved_sheet.half_size_v()
                                       << " max_displacement=" << curved_sheet.max_displacement()
                                       << " is_flat=" << curved_sheet.is_flat()
                                       << " cut_curved=" << cut_curved;

        // PHASE 3: an empty half is NOT a refusal - the cut runs and produces the
        // one half that has material - but it must not be silent. The panel warns
        // before the click; this is the notification for the click itself, so a
        // user who did not read the panel still learns why one part came back.
        if (curved_surface && !curved_sheet.is_flat() && (m_curved_upper_empty || m_curved_lower_empty)) {
            const wxString which = m_curved_upper_empty && m_curved_lower_empty ? _L("both sides")
                                 : m_curved_upper_empty                         ? _L("the upper side")
                                                                                : _L("the lower side");
            wxGetApp().plater()->get_notification_manager()->push_notification(
                NotificationType::CustomNotification, NotificationManager::NotificationLevel::WarningNotificationLevel,
                into_u8(format_wxstr(_L("Cut: the surface does not cross the part on %1%, so that half is empty."), which)));
        }

        // DRAW: the kerf reaches the cut through the params, not through the
        // perform() arguments, because it is offset along the strip's OWN normal -
        // there is no single plane normal to measure it along.
        if (cut_drawn) {
            draw_params.thickness        = cut_thickness;
            draw_params.thickness_offset = thickness_offset;
        }

        Cut cut(cut_mo, instance_idx, get_cut_matrix(selection), attributes);
        const ModelObjectPtrs& new_objects = cut_by_contour    ? cut.perform_by_contour(m_part_selection.get_cut_parts(), dowels_count):
                                             cut_with_groove   ? cut.perform_with_groove(m_groove, m_rotation_m) :
                                             cut_drawn         ? cut.perform_with_draw_stroke(draw_stroke, draw_params) :
                                             cut_curved        ? cut.perform_with_curved_sheet(curved_sheet, cut_thickness, thickness_offset) :
                                                                 cut.perform_with_plane(cut_thickness, thickness_offset);

        // fix_non_manifold_edges
#ifdef HAS_WIN10SDK
        if (is_windows10()) {
            bool is_showed_dialog = false;
            bool user_fix_model   = false;
            for (size_t i = 0; i < new_objects.size(); i++) {
                for (size_t j = 0; j < new_objects[i]->volumes.size(); j++) {
                    if (its_num_open_edges(new_objects[i]->volumes[j]->mesh().its) > 0) {
                        if (!is_showed_dialog) {
                            is_showed_dialog = true;
                            MessageDialog dlg(nullptr, _L("non-manifold edges be caused by cut tool, do you want to fix it now?"), "", wxYES | wxCANCEL);
                            int           ret = dlg.ShowModal();
                            if (ret == wxID_YES) {
                                user_fix_model = true;
                            }
                        }
                        if (!user_fix_model) {
                            break;
                        }
                        // model_name
                        std::vector<std::string> succes_models;
                        // model_name     failing reason
                        std::vector<std::pair<std::string, std::string>> failed_models;
                        auto                                             plater = wxGetApp().plater();
                        auto fix_and_update_progress = [this, plater](ModelObject *model_object, const int vol_idx, const string &model_name, ProgressDialog &progress_dlg,
                                                                      std::vector<std::string> &succes_models, std::vector<std::pair<std::string, std::string>> &failed_models) {
                            wxString msg = _L("Repairing model object");
                            msg += ": " + from_u8(model_name) + "\n";
                            std::string res;
                            if (!fix_model_by_win10_sdk_gui(*model_object, vol_idx, progress_dlg, msg, res)) return false;
                            return true;
                        };
                        ProgressDialog progress_dlg(_L("Repairing model object"), "", 100, find_toplevel_parent(plater), wxPD_AUTO_HIDE | wxPD_APP_MODAL | wxPD_CAN_ABORT, true);

                        auto model_name = new_objects[i]->name;
                        if (!fix_and_update_progress(new_objects[i], j, model_name, progress_dlg, succes_models, failed_models)) {
                            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "run fix_and_update_progress error";
                        };
                    };
                }
            }
        }
 #endif
        check_objects_after_cut(new_objects);

        // save cut_id to post update synchronization
        const CutObjectBase cut_id = cut_mo->cut_id;

        // RE-EDITABLE CUTS. Both halves get the SAME recipe - that is what makes
        // selecting either one enough to reopen the cut - and it is attached here,
        // before the objects reach the model, so the very first save carries it.
        //
        // Dowel objects (CreateDowels) are results of the cut too, but they are not
        // halves: re-editing from one makes no sense, so they are left alone. They
        // are told apart by their content - a dowel is one connector volume - and
        // NOT by is_cut(): a plain cut (no connectors, no groove) carries the
        // InvalidateCutInfo attribute above, so its halves come back with an
        // invalid cut_id and is_cut() false. Filtering on is_cut() here dropped
        // the recipe from every plain cut, which is why "Edit cut..." and
        // "Copy cut to..." never appeared on the halves of an ordinary cut.
        if (write_recipe) {
            for (ModelObject* o : new_objects) {
                if (o == nullptr || o->volumes.empty())
                    continue;
                const bool is_dowel = o->volumes.size() == 1 && o->volumes.front()->is_cut_connector() &&
                                      o->volumes.front()->cut_info.connector_type == CutConnectorType::Dowel;
                if (!is_dowel)
                    o->cut_recipe = new_recipe;
            }
        }

        // update cut results on plater and in the model
        plater->apply_cut_object_to_model(object_idx, new_objects);

        synchronize_model_after_cut(plater->model(), cut_id);

        // RE-EDITABLE CUTS: a re-edit REPLACES the halves it was opened on. The
        // cut above consumed the stand-in (apply_cut_object_to_model deleted it by
        // index), so what is left to remove is the old halves - by ObjectID,
        // because every index has just been invalidated.
        //
        // Deliberately after apply_cut_object_to_model: removing them earlier
        // would change the index the cut object itself sits at.
        if (was_reedit) {
            Model& model = plater->model();
            std::vector<size_t> doomed;
            for (size_t i = 0; i < model.objects.size(); ++i) {
                const ObjectID id = model.objects[i]->id();
                if (id == reedit_proxy_id ||
                    std::find(reedit_replace_ids.begin(), reedit_replace_ids.end(), id) != reedit_replace_ids.end())
                    doomed.push_back(i);
            }
            for (auto it = doomed.rbegin(); it != doomed.rend(); ++it) {
                model.delete_object(*it);
                wxGetApp().obj_list()->delete_object_from_list(*it);
            }
            if (!doomed.empty())
                plater->update();
        }
    }
}

// Is the mouse ray on the cut surface AS DRAWN?
//
// The CutPlane picking mesh is the flat plane's quad at 1.5x the object's
// bounding-box half diagonal, which reaches far past the part and, in Curved
// mode, is not the surface on screen at all - so a click on empty canvas could
// grab the plane and slide the cut while the user was only orbiting. Bound the
// hit to the rendered extent instead.
//
// Everything happens in the CUT PLANE's own frame: intersect the mouse ray with
// local z == 0 (Flat) or with the sheet (Curved), then test the resulting (x,y)
// against the extent that is actually drawn.
bool GLGizmoCut3D::mouse_on_cut_surface(const Vec2d& mouse_position) const
{
    // Nothing is drawn, so nothing can be hit.
    if (m_hide_cut_plane || m_connectors_editing || cut_line_processing())
        return false;

    // DRAW: the surface as drawn is the LINE. 2026-09-16, owner: Draw mode fell into
    // the flat-quad branch below, so the grab that moves the whole line fired anywhere
    // on the oversized picking quad and an attempt to orbit round the part moved the
    // line instead. Only a press within a few pixels of the line grabs it now; the
    // hover highlight and the tooltip go through here too, so they agree.
    if (is_draw_surface())
        return mouse_near_draw_line(mouse_position);

    const Camera& camera = wxGetApp().plater()->get_camera();
    Vec3d         ray_o, ray_dir;
    MeshRaycaster::line_from_mouse_pos(mouse_position, Transform3d::Identity(), camera, ray_o, ray_dir);
    if (ray_dir.isZero())
        return false;

    // World -> plane frame. The sheet and the drawn quad both live here.
    const Transform3d world_to_plane = (translation_transform(m_plane_center) * m_rotation_m).inverse();
    const Vec3d       o = world_to_plane * ray_o;
    const Vec3d       d = world_to_plane.linear() * ray_dir;

    if (std::abs(d.z()) < 1e-9)
        return false; // ray parallel to the plane: it grazes, it does not hit

    if (!is_curved_surface()) {
        // FLAT: the drawn quad is its_make_frustum_dowel(r, w, 4) - a square
        // whose corners sit at distance r, so its half side is r / sqrt(2).
        // Test against that square, in the quad's own orientation (the dowel's
        // sectors start at 45 degrees, which is exactly what makes it an
        // axis-aligned square of that half side).
        const double t = -o.z() / d.z();
        if (t < 0.0)
            return false;
        const Vec3d  hit  = o + t * d;
        const double half = double(m_cut_plane_radius_koef) * m_radius * (m_cut_plane_as_circle ? 1.0 : 0.70710678118654752440 /* 1/sqrt(2) */);
        if (m_cut_plane_as_circle)
            return hit.head<2>().norm() <= half;
        return std::abs(hit.x()) <= half && std::abs(hit.y()) <= half;
    }

    // CURVED: the drawn thing is the sheet over its own rectangle. Walk the ray
    // through the domain and look for a sign change of (ray z - sheet height);
    // the sheet is a single-valued height field, so one crossing is the answer
    // and a coarse march plus a bisection is both robust and cheap. A plain
    // z == 0 test would be wrong wherever the sheet is bent away from the plane.
    const double hs_u = m_curved_sheet.half_size_u();
    const double hs_v = m_curved_sheet.half_size_v();

    // Clip the ray to the domain's infinite prism in x and y, so the march only
    // covers the stretch that can possibly be over the sheet.
    double t_lo = 0.0, t_hi = std::numeric_limits<double>::max();
    auto clip = [&](double oc, double dc, double half) {
        if (std::abs(dc) < 1e-12)
            return std::abs(oc) <= half; // parallel: inside for all t, or never
        double ta = (-half - oc) / dc;
        double tb = ( half - oc) / dc;
        if (ta > tb)
            std::swap(ta, tb);
        t_lo = std::max(t_lo, ta);
        t_hi = std::min(t_hi, tb);
        return t_lo <= t_hi;
    };
    if (!clip(o.x(), d.x(), hs_u) || !clip(o.y(), d.y(), hs_v))
        return false;
    if (t_hi <= 0.0)
        return false;
    t_lo = std::max(t_lo, 0.0);
    if (!(t_hi > t_lo))
        return false;

    auto gap = [&](double t) {
        const Vec3d p = o + t * d;
        return p.z() - m_curved_sheet.evaluate_local(p.x(), p.y());
    };

    // 64 steps over the clipped span: the sheet is a Catmull-Rom height field
    // over at most 15 control points, so it cannot oscillate faster than that.
    const int    Steps = 64;
    double       t0    = t_lo;
    double       g0    = gap(t0);
    if (g0 == 0.0)
        return true;
    for (int k = 1; k <= Steps; ++ k) {
        const double t1 = t_lo + (t_hi - t_lo) * double(k) / double(Steps);
        const double g1 = gap(t1);
        if (g1 == 0.0 || (g0 < 0.0) != (g1 < 0.0)) {
            // Sign change: the ray crosses the sheet in [t0, t1]. It is inside
            // the domain by construction (we clipped to it), so this is a hit.
            return true;
        }
        t0 = t1;
        g0 = g1;
    }
    return false;
}

// Unprojects the mouse position on the mesh and saves hit point and normal of the facet into pos_and_normal
// Return false if no intersection was found, true otherwise.
bool GLGizmoCut3D::unproject_on_cut_plane(const Vec2d& mouse_position, Vec3d& pos, Vec3d& pos_world, bool respect_contours/* = true*/)
{
    // No selection resolved yet means no surface to unproject onto - "no hit"
    // rather than a null dereference. Same window as
    // check_and_update_connectors_state(); see the note there.
    const CommonGizmosDataObjects::SelectionInfo* sel = m_c ? m_c->selection_info() : nullptr;
    const ModelObject*                            mo  = sel ? sel->model_object() : nullptr;
    if (mo == nullptr)
        return false;
    const int active_inst = sel->get_active_instance();
    if (active_inst < 0 || size_t(active_inst) >= mo->instances.size())
        return false;

    const float sla_shift = sel->get_sla_shift();

    const ModelInstance* mi = mo->instances[active_inst];
    const Camera& camera = wxGetApp().plater()->get_camera();

    // Calculate intersection with the clipping plane.
    const ClippingPlane* cp = m_c->object_clipper()->get_clipping_plane(true);
    Vec3d point;
    Vec3d direction;
    Vec3d hit;
    MeshRaycaster::line_from_mouse_pos(mouse_position, Transform3d::Identity(), camera, point, direction);
    Vec3d normal = -cp->get_normal().cast<double>();
    double den = normal.dot(direction);
    if (den != 0.) {
        double t = (-cp->get_offset() - normal.dot(point))/den;
        hit = (point + t * direction);
    } else
        return false;

    // Now check if the hit is not obscured by a selected part on this side of the plane.
    // FIXME: This would be better solved by remembering which contours are active. We will
    // probably need that anyway because there is not other way to find out which contours
    // to render. If you want to uncomment it, fix it first. It does not work yet.
    /*for (size_t id = 0; id < m_part_selection.parts.size(); ++id) {
        if (! m_part_selection.parts[id].selected) {
            Vec3f pos, normal;
            const ModelObject* model_object = m_part_selection.model_object;
            const Vec3d volume_offset = m_part_selection.model_object->volumes[id]->get_offset();
            Transform3d tr = model_object->instances[m_part_selection.instance_idx]->get_matrix() * model_object->volumes[id]->get_matrix();
            if (m_part_selection.parts[id].raycaster.unproject_on_mesh(mouse_position, tr, camera, pos, normal))
                return false;
        }
    }*/

    if (respect_contours)
    {
        // Do not react to clicks outside a contour (or inside a contour that is ignored)
        int cont_id = m_c->object_clipper()->is_projection_inside_cut(hit);
        if (cont_id == -1)
            return false;
        if (m_part_selection.valid()) {
            const std::vector<size_t>& ign = *m_part_selection.get_ignored_contours_ptr();
            if (std::find(ign.begin(), ign.end(), cont_id) != ign.end())
                return false;
        }    
    }
    

    // recalculate hit to object's local position
    Vec3d hit_d = hit;
    hit_d -= mi->get_offset();
    hit_d[Z] -= sla_shift;

    // Return both the point and the facet normal.
    pos = hit_d;
    pos_world = hit;

    return true; 
}

void GLGizmoCut3D::clear_selection()
{
    m_selected.clear();
    m_selected_count = 0;
}

void GLGizmoCut3D::reset_connectors()
{
    m_c->selection_info()->model_object()->cut_connectors.clear();
    update_raycasters_for_picking();
    clear_selection();
    check_and_update_connectors_state();
}

void GLGizmoCut3D::init_connector_shapes()
{
    for (const CutConnectorType& type : {CutConnectorType::Dowel, CutConnectorType::Plug, CutConnectorType::Snap})
        for (const CutConnectorStyle& style : {CutConnectorStyle::Frustum, CutConnectorStyle::Prism}) {
            if (type == CutConnectorType::Dowel && style == CutConnectorStyle::Frustum)
                continue;
            for (const CutConnectorShape& shape : {CutConnectorShape::Circle, CutConnectorShape::Hexagon, CutConnectorShape::Square, CutConnectorShape::Triangle}) {
                if (type == CutConnectorType::Snap && shape != CutConnectorShape::Circle)
                    continue;
                const CutConnectorAttributes attribs = { type, style, shape };
                indexed_triangle_set its = get_connector_mesh(attribs);
                m_shapes[attribs].model.init_from(its);
                m_shapes[attribs].mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
            }
        }

    // The Flexi joint has one cached shape, rebuilt whenever its parameters change.
    {
        const CutConnectorAttributes attribs = { CutConnectorType::FlexiJoint, CutConnectorStyle::Prism, CutConnectorShape::Circle };
        indexed_triangle_set its = get_connector_mesh(attribs);
        m_shapes[attribs].model.init_from(its);
        m_shapes[attribs].mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }
}

void GLGizmoCut3D::update_connector_shape()
{
    CutConnectorAttributes attribs = { m_connector_type, CutConnectorStyle(m_connector_style), CutConnectorShape(m_connector_shape_id) };

    if (m_connector_type == CutConnectorType::FlexiJoint) {
        // One cached shape per flexi joint; it is rebuilt on every parameter change.
        attribs = { CutConnectorType::FlexiJoint, CutConnectorStyle::Prism, CutConnectorShape::Circle };
        indexed_triangle_set its = get_connector_mesh(attribs);
        m_shapes[attribs].reset();
        m_shapes[attribs].model.init_from(its);
        m_shapes[attribs].mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
        return;
    }

    if (m_connector_type == CutConnectorType::Snap) {
        indexed_triangle_set its = get_connector_mesh(attribs);
        m_shapes[attribs].reset();
        m_shapes[attribs].model.init_from(its);
        m_shapes[attribs].mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));

        //const indexed_triangle_set its = get_connector_mesh(attribs);
        //m_connector_mesh.clear();
        //m_connector_mesh = TriangleMesh(its);
    }


}

bool GLGizmoCut3D::cut_line_processing() const
{
    return !m_line_beg.isApprox(Vec3d::Zero());
}

void GLGizmoCut3D::discard_cut_line_processing()
{
    m_line_beg = m_line_end = Vec3d::Zero();
}

bool GLGizmoCut3D::process_cut_line(SLAGizmoEventType action, const Vec2d& mouse_position)
{
    const Camera& camera = wxGetApp().plater()->get_camera();

    Vec3d pt;
    Vec3d dir;
    MeshRaycaster::line_from_mouse_pos(mouse_position, Transform3d::Identity(), camera, pt, dir);
    dir.normalize();
    pt += dir; // Move the pt along dir so it is not clipped.

    if (action == SLAGizmoEventType::LeftDown && !cut_line_processing()) {
        m_line_beg = pt;
        m_line_end = pt;
        on_unregister_raycasters_for_picking();
        return true;
    }

    if (cut_line_processing()) {
        if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
            m_groove_editing = true;
        reset_cut_by_contours();

        m_line_end = pt;
        if (action == SLAGizmoEventType::LeftDown || action == SLAGizmoEventType::LeftUp) {
            Vec3d line_dir = m_line_end - m_line_beg;
            if (line_dir.norm() < 3.0)
                return true;

            Vec3d cross_dir = line_dir.cross(dir).normalized();
            Eigen::Quaterniond q;
            Transform3d m = Transform3d::Identity();
            m.matrix().block(0, 0, 3, 3) = q.setFromTwoVectors(Vec3d::UnitZ(), cross_dir).toRotationMatrix();

            const Vec3d new_plane_center = m_bb_center + cross_dir * cross_dir.dot(pt - m_bb_center);
            // update transformed bb
            const auto new_tbb = transformed_bounding_box(new_plane_center, m);
            const GLVolume* first_volume = m_parent.get_selection().get_first_volume();
            Vec3d instance_offset = first_volume->get_instance_offset();
            instance_offset[Z] += first_volume->get_sla_shift_z();

            const Vec3d trans_center_pos = m.inverse() * (new_plane_center - instance_offset) + new_tbb.center();
            if (new_tbb.contains(trans_center_pos)) {
                Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Cut by line"), UndoRedo::SnapshotType::GizmoAction);
                m_transformed_bounding_box = new_tbb;
                set_center(new_plane_center);
                m_start_dragging_m = m_rotation_m = m;
                m_ar_plane_center = m_plane_center;
            }

            m_angle_arc.reset();
            discard_cut_line_processing();

            if (CutMode(m_mode) == CutMode::cutTongueAndGroove) {
                m_groove_editing = false;
                reset_cut_by_contours();
            }
        }
        else if (action == SLAGizmoEventType::Moving)
            this->set_dirty();
        return true;
    }
    return false;
}

bool GLGizmoCut3D::add_connector(CutConnectors& connectors, const Vec2d& mouse_position)
{
    if (!m_connectors_editing)
        return false;

    Vec3d pos;
    Vec3d pos_world;
    // PHASE 4: on a curved cut the click hits the SHEET, not the plane, so the
    // connector lands on the surface the user is looking at. The stored rotation
    // is the sheet's frame there - and it is re-derived on every use, so a later
    // sheet edit moves the connector with the surface.
    if (unproject_on_curved_sheet(mouse_position.cast<double>(), pos, pos_world)) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Add connector"), UndoRedo::SnapshotType::GizmoAction);
        unselect_all_connectors();

        const bool flexi = is_flexi_joint_type();
        connectors.emplace_back(pos, connector_rotation_m(pos),
                                flexi ? flexi_outer_extent(m_flexi)      : m_connector_size * 0.5f,
                                flexi ? flexi_protrusion_height(m_flexi) : m_connector_depth_ratio,
                                m_connector_size_tolerance * 0.5f, m_connector_depth_ratio_tolerance,
                                m_connector_angle,
                                CutConnectorAttributes( CutConnectorType(m_connector_type),
                                                        flexi ? CutConnectorStyle::Prism  : CutConnectorStyle(m_connector_style),
                                                        flexi ? CutConnectorShape::Circle : CutConnectorShape(m_connector_shape_id)));
        if (flexi)
            connectors.back().flexi = m_flexi;
        m_selected.push_back(true);
        m_selected_count = 1;
        assert(m_selected.size() == connectors.size());
        update_raycasters_for_picking();
        m_parent.set_as_dirty();
        check_and_update_connectors_state();

        return true;
    }
    return false;
}

bool GLGizmoCut3D::delete_selected_connectors(CutConnectors& connectors)
{
    if (connectors.empty())
        return false;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Delete connector"), UndoRedo::SnapshotType::GizmoAction);

    // remove  connectors
    for (int i = int(connectors.size()) - 1; i >= 0; i--)
        if (m_selected[i])
            connectors.erase(connectors.begin() + i);
    // remove selections
    m_selected.erase(std::remove_if(m_selected.begin(), m_selected.end(), [](const auto& selected) {
        return selected; }), m_selected.end());
    m_selected_count = 0;

    assert(m_selected.size() == connectors.size());
    update_raycasters_for_picking();
    m_parent.set_as_dirty();
    check_and_update_connectors_state();
    return true;
}

void GLGizmoCut3D::select_connector(int idx, bool select)
{
    m_selected[idx] = select;
    if (select)
        ++m_selected_count;
    else
        --m_selected_count;
}

bool GLGizmoCut3D::is_selection_changed(bool alt_down, bool shift_down)
{
    if (m_hover_id >= m_connectors_group_id) {
        if (alt_down)
            select_connector(m_hover_id - m_connectors_group_id, false);
        else {
            if (!shift_down)
                unselect_all_connectors();
            select_connector(m_hover_id - m_connectors_group_id, true);
        }
        return true;
    }
    return false;
}

void GLGizmoCut3D::process_selection_rectangle(CutConnectors &connectors)
{
    GLSelectionRectangle::EState rectangle_status = m_selection_rectangle.get_state();

    ModelObject* mo          = m_c->selection_info()->model_object();
    int          active_inst = m_c->selection_info()->get_active_instance();

    // First collect positions of all the points in world coordinates.
    Transformation trafo = mo->instances[active_inst]->get_transformation();
    trafo.set_offset(trafo.get_offset() + double(m_c->selection_info()->get_sla_shift()) * Vec3d::UnitZ());

    std::vector<Vec3d> points;
    for (const CutConnector&connector : connectors)
        points.push_back(connector.pos + trafo.get_offset());

    // Now ask the rectangle which of the points are inside.
    std::vector<unsigned int> points_idxs = m_selection_rectangle.contains(points);
    m_selection_rectangle.stop_dragging();

    for (size_t idx : points_idxs)
        select_connector(int(idx), rectangle_status == GLSelectionRectangle::EState::Select);
}

bool GLGizmoCut3D::gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    if (is_dragging() || m_connector_mode == CutConnectorMode::Auto)
        return false;

    if ( (m_hover_id < 0 || m_hover_id == CutPlane) && shift_down &&  ! m_connectors_editing &&
        (action == SLAGizmoEventType::LeftDown || action == SLAGizmoEventType::LeftUp || action == SLAGizmoEventType::Moving) )
        return process_cut_line(action, mouse_position);

    if (!m_keep_upper || !m_keep_lower)
        return false;

    if (!m_connectors_editing)
        return false;

    CutConnectors& connectors = m_c->selection_info()->model_object()->cut_connectors;

    if (action == SLAGizmoEventType::LeftDown) {
        if (shift_down || alt_down) {
            // left down with shift - show the selection rectangle:
            if (m_hover_id == -1)
                m_selection_rectangle.start_dragging(mouse_position, shift_down ? GLSelectionRectangle::EState::Select : GLSelectionRectangle::EState::Deselect);
        }
        else
            // If there is no selection and no hovering, add new point
            if (m_hover_id == -1 && !shift_down && !alt_down)
                if (!add_connector(connectors, mouse_position))
                    m_ldown_mouse_position = mouse_position;
        return true;
    }

    if (action == SLAGizmoEventType::LeftUp && !m_selection_rectangle.is_dragging()) {
        if ((m_ldown_mouse_position - mouse_position).norm() < 5.)
            unselect_all_connectors();
        return is_selection_changed(alt_down, shift_down);
    }

    // left up with selection rectangle - select points inside the rectangle:
    if ((action == SLAGizmoEventType::LeftUp || action == SLAGizmoEventType::ShiftUp || action == SLAGizmoEventType::AltUp) && m_selection_rectangle.is_dragging()) {
        // Is this a selection or deselection rectangle?
        process_selection_rectangle(connectors);
        return true;
    }

    // dragging the selection rectangle:
    if (action == SLAGizmoEventType::Dragging) {
        if (m_selection_rectangle.is_dragging()) {
            m_selection_rectangle.dragging(mouse_position);
            return true;
        }
        return false;
    }
    
    if (action == SLAGizmoEventType::RightDown && !shift_down) {
        // If any point is in hover state, this should initiate its move - return control back to GLCanvas:
        if (m_hover_id < m_connectors_group_id)
            return false;
        unselect_all_connectors();
        select_connector(m_hover_id - m_connectors_group_id, true);
        return delete_selected_connectors(connectors);
    }
    
    if (action == SLAGizmoEventType::Delete)
        return delete_selected_connectors(connectors);

    if (action == SLAGizmoEventType::SelectAll) {
        select_all_connectors();
        return true;
    }

    return false;
}

CommonGizmosDataID GLGizmoCut3D::on_get_requirements() const {
    return CommonGizmosDataID(
                int(CommonGizmosDataID::SelectionInfo)
              | int(CommonGizmosDataID::InstancesHider)
              | int(CommonGizmosDataID::ObjectClipper)
              | int(CommonGizmosDataID::Raycaster)); // for pick-face (FacetPicker)
}

void GLGizmoCut3D::data_changed(bool is_serializing) 
{
    update_bb();
    if (auto oc = m_c->object_clipper())
        oc->set_behavior(m_connectors_editing, m_connectors_editing, double(m_contour_width));
}




indexed_triangle_set GLGizmoCut3D::get_connector_mesh(CutConnectorAttributes connector_attributes)
{
    indexed_triangle_set connector_mesh;

    int   sectorCount{ 1 };
    switch (CutConnectorShape(connector_attributes.shape)) {
    case CutConnectorShape::Triangle:
        sectorCount = 3;
        break;
    case CutConnectorShape::Square:
        sectorCount = 4;
        break;
    case CutConnectorShape::Circle:
        sectorCount = 360;
        break;
    case CutConnectorShape::Hexagon:
        sectorCount = 6;
        break;
    default:
        break;
    }

    if (connector_attributes.type == CutConnectorType::FlexiJoint) {
        // The flexi bodies are generated in millimetres; normalize them by the same
        // (radius, radius, height) the renderer/raycaster/bbox checks scale connectors by.
        connector_mesh = flexi_preview_body(m_flexi);
        const double r = std::max(0.001f, flexi_outer_extent(m_flexi));
        const double h = std::max(0.001f, flexi_protrusion_height(m_flexi));
        its_transform(connector_mesh, scale_transform(Vec3d(1. / r, 1. / r, 1. / h)));
        return connector_mesh;
    }

    if (connector_attributes.type == CutConnectorType::Snap)
        connector_mesh = its_make_snap(1.0, 1.0, m_snap_space_proportion, m_snap_bulge_proportion);
    else if (connector_attributes.style == CutConnectorStyle::Prism)
        connector_mesh = its_make_cylinder(1.0, 1.0, (2 * PI / sectorCount));
    else if (connector_attributes.type == CutConnectorType::Plug)
        connector_mesh = its_make_frustum(1.0, 1.0, (2 * PI / sectorCount));
    else
        connector_mesh = its_make_frustum_dowel(1.0, 1.0, sectorCount);

    return connector_mesh;
}

void GLGizmoCut3D::apply_cut_connectors(ModelObject* mo, const std::string& connector_name)
{
    if (mo->cut_connectors.empty())
        return;

    using namespace Geometry;

    size_t connector_id = mo->cut_id.connectors_cnt();
    for (const CutConnector& connector : mo->cut_connectors) {
        if (connector.attribs.type == CutConnectorType::FlexiJoint) {
            add_flexi_joint_volume(mo, connector, connector_name + "-" + std::to_string(++connector_id));
            continue;
        }
        TriangleMesh mesh = TriangleMesh(get_connector_mesh(connector.attribs));
        // Mesh will be centered when loading.
        ModelVolume* new_volume = mo->add_volume(std::move(mesh), ModelVolumeType::NEGATIVE_VOLUME);

        // Transform the new modifier to be aligned inside the instance
        new_volume->set_transformation(translation_transform(connector.pos) * connector.rotation_m *
            rotation_transform(-connector.z_angle * Vec3d::UnitZ()) *
            scale_transform(Vec3f(connector.radius, connector.radius, connector.height).cast<double>()));

        new_volume->cut_info = { connector.attribs.type, connector.radius_tolerance, connector.height_tolerance };
        new_volume->name = connector_name + "-" + std::to_string(++connector_id);
    }
    mo->cut_id.increase_connectors_cnt(mo->cut_connectors.size());

    // delete all connectors
    mo->cut_connectors.clear();
}



} // namespace GUI
} // namespace Slic3r
