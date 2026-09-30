#include "GLGizmoMeasure.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/UltraFitGeometry.hpp"
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include "slic3r/GUI/Gizmos/GizmoObjectManipulation.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/MeasureUtils.hpp"

#include <imgui/imgui_internal.h>

#include <numeric>

#include <glad/gl.h>

#include <tbb/parallel_for.h>

#include <wx/clipbrd.h>

namespace Slic3r {
namespace GUI {
std::string GLGizmoMeasure::format_double(double value)
{
    char buf[1024];
    sprintf(buf, "%.3f", value);
    return std::string(buf);
}

std::string GLGizmoMeasure::format_vec3(const Vec3d &v)
{
    char buf[1024];
    sprintf(buf, "X: %.3f, Y: %.3f, Z: %.3f", v.x(), v.y(), v.z());
    return std::string(buf);
}

std::string GLGizmoMeasure::surface_feature_type_as_string(Measure::SurfaceFeatureType type)
{
    switch (type)
    {
    default:
    case Measure::SurfaceFeatureType::Undef:  { return _u8L("No feature"); }
    case Measure::SurfaceFeatureType::Point:  { return _u8L("Vertex"); }
    case Measure::SurfaceFeatureType::Edge:   { return _u8L("Edge"); }
    case Measure::SurfaceFeatureType::Circle: { return _u8L("Circle"); }
    case Measure::SurfaceFeatureType::Plane:  { return _u8L("Plane"); }
    case Measure::SurfaceFeatureType::Triangle: { return _u8L("Triangle"); }
    case Measure::SurfaceFeatureType::Curve:    { return _u8L("Curved patch"); }
    }
}

std::string GLGizmoMeasure::point_on_feature_type_as_string(Measure::SurfaceFeatureType type, int hover_id)
{
    std::string ret;
    switch (type) {
    case Measure::SurfaceFeatureType::Point:  { ret = _u8L("Vertex"); break; }
    case Measure::SurfaceFeatureType::Edge:   { ret = _u8L("Point on edge"); break; }
    case Measure::SurfaceFeatureType::Circle: { ret = _u8L("Point on circle"); break; }
    case Measure::SurfaceFeatureType::Plane:  { ret = _u8L("Point on plane"); break; }
    case Measure::SurfaceFeatureType::Triangle: { ret = _u8L("Point on triangle"); break; }
    case Measure::SurfaceFeatureType::Curve:    { ret = _u8L("Point on curved patch"); break; }
    default:                                  { assert(false); break; }
    }
    return ret;
}

std::string GLGizmoMeasure::center_on_feature_type_as_string(Measure::SurfaceFeatureType type)
{
    std::string ret;
    switch (type) {
    case Measure::SurfaceFeatureType::Edge:   { ret = _u8L("Center of edge"); break; }
    case Measure::SurfaceFeatureType::Circle: { ret = _u8L("Center of circle"); break; }
    default: { assert(false); break; }
    }
    return ret;
}

bool GLGizmoMeasure::is_feature_with_center(const Measure::SurfaceFeature &feature)
{
    const Measure::SurfaceFeatureType type = feature.get_type();
    return (type == Measure::SurfaceFeatureType::Circle || (type == Measure::SurfaceFeatureType::Edge && feature.get_extra_point().has_value()));
}

Vec3d GLGizmoMeasure::get_feature_offset(const Measure::SurfaceFeature &feature)
{
    Vec3d ret;
    switch (feature.get_type())
    {
    case Measure::SurfaceFeatureType::Circle:
    {
        const auto [center, radius, normal] = feature.get_circle();
        ret = center;
        break;
    }
    case Measure::SurfaceFeatureType::Edge:
    {
        std::optional<Vec3d> p = feature.get_extra_point();
        assert(p.has_value());
        ret = *p;
        break;
    }
    case Measure::SurfaceFeatureType::Point:
    {
        ret = feature.get_point();
        break;
    }
    default: { assert(false); }
    }
    return ret;
}

Vec3d TransformHelper::model_to_world(const Vec3d &model, const Transform3d &world_matrix) {
    return world_matrix * model;
}

Vec4d TransformHelper::world_to_clip(const Vec3d &world, const Matrix4d &projection_view_matrix)
{
    return projection_view_matrix * Vec4d(world.x(), world.y(), world.z(), 1.0);
}

Vec3d TransformHelper::clip_to_ndc(const Vec4d &clip) {
    return Vec3d(clip.x(), clip.y(), clip.z()) / clip.w();
}

Vec2d TransformHelper::ndc_to_ss(const Vec3d &ndc, const std::array<int, 4> &viewport)
{
    const double half_w = 0.5 * double(viewport[2]);
    const double half_h = 0.5 * double(viewport[3]);
    return { half_w * ndc.x() + double(viewport[0]) + half_w, half_h * ndc.y() + double(viewport[1]) + half_h };
};

Vec4d TransformHelper::model_to_clip(const Vec3d &model, const Transform3d &world_matrix, const Matrix4d &projection_view_matrix)
{
    return world_to_clip(model_to_world(model, world_matrix), projection_view_matrix);
}

Vec3d TransformHelper::model_to_ndc(const Vec3d &model, const Transform3d &world_matrix, const Matrix4d &projection_view_matrix)
{
    return clip_to_ndc(world_to_clip(model_to_world(model, world_matrix), projection_view_matrix));
}

Vec2d TransformHelper::model_to_ss(const Vec3d &model, const Transform3d &world_matrix, const Matrix4d &projection_view_matrix, const std::array<int, 4> &viewport)
{
    return ndc_to_ss(clip_to_ndc(world_to_clip(model_to_world(model, world_matrix), projection_view_matrix)), viewport);
}

Vec2d TransformHelper::world_to_ss(const Vec3d &world, const Matrix4d &projection_view_matrix, const std::array<int, 4> &viewport)
{
    return ndc_to_ss(clip_to_ndc(world_to_clip(world, projection_view_matrix)), viewport);
}

const Matrix4d &TransformHelper::ndc_to_ss_matrix(const std::array<int, 4> &viewport)
{
    update(viewport);
    return s_cache.ndc_to_ss_matrix;
}

const Transform3d TransformHelper::ndc_to_ss_matrix_inverse(const std::array<int, 4> &viewport)
{
    update(viewport);
    return s_cache.ndc_to_ss_matrix_inverse;
}

void TransformHelper::update(const std::array<int, 4> &viewport)
{
    if (s_cache.viewport == viewport)
        return;

    const double half_w = 0.5 * double(viewport[2]);
    const double half_h = 0.5 * double(viewport[3]);
    s_cache.ndc_to_ss_matrix << half_w, 0.0, 0.0, double(viewport[0]) + half_w,
        0.0, half_h, 0.0, double(viewport[1]) + half_h,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0;

    s_cache.ndc_to_ss_matrix_inverse = s_cache.ndc_to_ss_matrix.inverse();
    s_cache.viewport = viewport;
}

TransformHelper::Cache TransformHelper::s_cache = { { 0, 0, 0, 0 }, Matrix4d::Identity(), Transform3d::Identity() };

GLGizmoMeasure::GLGizmoMeasure(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
: GLGizmoBase(parent, icon_filename, sprite_id)
{
    GLModel::Geometry sphere_geometry = smooth_sphere(16, 7.5f);
    m_sphere.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(sphere_geometry.get_as_indexed_triangle_set()));
    m_sphere.model.init_from(std::move(sphere_geometry));
    m_gripper_id_raycast_map[GripperType::POINT] = std::make_shared<PickRaycaster>(POINT_ID, *m_sphere.mesh_raycaster);

    GLModel::Geometry cylinder_geometry = smooth_cylinder(16, 5.0f, 1.0f);
    m_cylinder.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(cylinder_geometry.get_as_indexed_triangle_set()));
    m_cylinder.model.init_from(std::move(cylinder_geometry));
    m_gripper_id_raycast_map[GripperType::EDGE] = std::make_shared<PickRaycaster>(EDGE_ID, *m_cylinder.mesh_raycaster);
}

bool GLGizmoMeasure::on_mouse(const wxMouseEvent &mouse_event)
{
    if (mouse_event.Moving()) {
        // only for sure
        m_mouse_left_down = false;
        return false;
    }
    else if (mouse_event.Dragging()) {
        // Enable/Disable panning/rotating the 3D scene
        // Ctrl is pressed or the mouse is not hovering a selected volume
        bool unlock_dragging = mouse_event.CmdDown() || (m_hover_id == -1 && !m_parent.get_selection().contains_volume(m_parent.get_first_hover_volume_idx()));
        // mode is not center selection or mouse is not hovering a center
        unlock_dragging &= !mouse_event.ShiftDown() || (m_hover_id != SEL_SPHERE_1_ID && m_hover_id != SEL_SPHERE_2_ID && m_hover_id != POINT_ID);
        return !unlock_dragging;
    }
    else if (mouse_event.LeftDown()) {
        // let the event pass through to allow panning/rotating the 3D scene
        if (mouse_event.CmdDown())
            return false;

        if (m_hover_id != -1) {
            m_mouse_left_down = true;
            m_mouse_left_down_mesh_deal = true;
            auto detect_current_item = [this]() {
                SelectedFeatures::Item item;
                if (m_hover_id == SEL_SPHERE_1_ID) {
                    if (m_selected_features.first.is_center)
                        // mouse is hovering over a selected center
                        item = { true, m_selected_features.first.source, { Measure::SurfaceFeature(get_feature_offset(*m_selected_features.first.source)) } };
                    else if (is_feature_with_center(*m_selected_features.first.feature))
                        // mouse is hovering over a unselected center
                        item = { true, m_selected_features.first.feature, { Measure::SurfaceFeature(get_feature_offset(*m_selected_features.first.feature)) } };
                    else
                        // mouse is hovering over a point
                        item = m_selected_features.first;
                }
                else if (m_hover_id == SEL_SPHERE_2_ID) {
                    if (m_selected_features.second.is_center)
                        // mouse is hovering over a selected center
                        item = { true, m_selected_features.second.source, { Measure::SurfaceFeature(get_feature_offset(*m_selected_features.second.source)) } };
                    else if (is_feature_with_center(*m_selected_features.second.feature))
                        // mouse is hovering over a center
                        item = { true, m_selected_features.second.feature, { Measure::SurfaceFeature(get_feature_offset(*m_selected_features.second.feature)) } };
                    else
                        // mouse is hovering over a point
                        item = m_selected_features.second;
                }
                else {
                    switch (m_mode)
                    {
                    case EMode::FeatureSelection: { item = { false, m_curr_feature, m_curr_feature }; break; }
                    case EMode::PointSelection:   {
                       item = { false, m_curr_feature, Measure::SurfaceFeature(*m_curr_point_on_feature_position)};
                       auto local_pt = m_curr_feature->world_tran.inverse() * (*m_curr_point_on_feature_position);
                       item.feature->origin_surface_feature = std::make_shared<Measure::SurfaceFeature>(local_pt);
                       item.feature->world_tran             = m_curr_feature->world_tran;
                       item.feature->volume                 = m_curr_feature->volume;
                       break; }
                    }
                }
                return item;
            };

            auto requires_sphere_raycaster_for_picking = [this](const SelectedFeatures::Item& item) {
                if (m_mode == EMode::PointSelection || item.feature->get_type() == Measure::SurfaceFeatureType::Point)
                    return true;
                else if (m_mode == EMode::FeatureSelection) {
                    if (is_feature_with_center(*item.feature))
                        return true;
                }
                return false;
            };

            if (m_selected_features.first.feature.has_value()) {
                reset_feature2_render();
                const SelectedFeatures::Item item = detect_current_item();
                if (!is_pick_meet_assembly_mode(item)) { // assembly deal
                    m_selected_wrong_feature_waring_tip = true;
                    return true;
                }
                m_selected_wrong_feature_waring_tip = false;
                if (m_selected_features.first != item) {
                    bool processed = false;
                    if (item.is_center) {
                        if (item.source == m_selected_features.first.feature) {
                            // switch 1st selection from feature to its center
                            m_selected_features.first = item;
                            processed = true;
                        }
                        else if (item.source == m_selected_features.second.feature) {
                            // switch 2nd selection from feature to its center
                            m_selected_features.second = item;
                            processed = true;
                        }
                    }
                    else if (is_feature_with_center(*item.feature)) {
                      if (m_selected_features.first.is_center && m_selected_features.first.source == item.feature) {
                          // switch 1st selection from center to its feature
                          m_selected_features.first = item;
                          processed = true;
                      }
                      else if (m_selected_features.second.is_center && m_selected_features.second.source == item.feature) {
                          // switch 2nd selection from center to its feature
                          m_selected_features.second = item;
                          processed = true;
                      }
                    }

                    if (!processed) {
                        remove_selected_sphere_raycaster(SEL_SPHERE_2_ID);
                        // Ultra (Exact highlight): a click on the moving part anchors the footprint there; a
                        // second click on the same face moves it rather than dropping the pick.
                        const bool exact_anchor = ultra_exact_active() && m_ultra_exact_hover.valid && item.feature.has_value() &&
                                                  item.feature->is_planar() && item.feature->volume == m_ultra_exact_hover.volume;
                        if (m_selected_features.second == item && exact_anchor) {
                            m_ultra_exact_pick = m_ultra_exact_hover;
                            m_ultra_exact_pick_key.clear();
                        }
                        else if (m_selected_features.second == item) {
                            // 2nd feature deselection
                            // m_selected_features.second.reset();
                            reset_feature2();
                        }
                        else {
                            // 2nd feature selection
                            m_selected_features.second = item;
                            m_ultra_exact_pick = exact_anchor ? m_ultra_exact_hover : UltraExactAnchor();
                            m_ultra_exact_pick_key.clear();
                            if (requires_sphere_raycaster_for_picking(item)) {
                                auto pick = std::make_shared<PickRaycaster>(SEL_SPHERE_2_ID, *m_sphere.mesh_raycaster);
                                m_gripper_id_raycast_map[GripperType::SPHERE_2] = pick;
                            }
                        }
                    }
                }
                else {
                    remove_selected_sphere_raycaster(SEL_SPHERE_1_ID);
                    if (m_selected_features.second.feature.has_value()) {
                        // promote 2nd feature to 1st feature
                        reset_feature1();
                        if (requires_sphere_raycaster_for_picking(m_selected_features.first)) {
                            auto pick = std::make_shared<PickRaycaster>(SEL_SPHERE_1_ID, *m_sphere.mesh_raycaster);
                            m_gripper_id_raycast_map[GripperType::SPHERE_1] = pick;
                        }
                    } else {
                        // 1st feature deselection
                        reset_feature1();
                    }
                }
            }
            else {
                // 1st feature selection
                reset_feature1_render();
                const SelectedFeatures::Item item = detect_current_item();
                if (!is_pick_meet_assembly_mode(item)) {//assembly deal
                    m_selected_wrong_feature_waring_tip = true;
                    return true;
                }
                m_selected_wrong_feature_waring_tip = false;
                m_selected_features.first = item;
                if (requires_sphere_raycaster_for_picking(item)) {
                    auto pick = std::make_shared<PickRaycaster>(SEL_SPHERE_1_ID, *m_sphere.mesh_raycaster);
                    m_gripper_id_raycast_map[GripperType::SPHERE_1] = pick;
                }
            }

            update_measurement_result();

            m_imgui->set_requires_extra_frame();
            return true;
        }
        else
            // if the mouse pointer is on any volume, filter out the event to prevent the user to move it
            // equivalent tp: return (m_parent.get_first_hover_volume_idx() != -1);
            return m_curr_feature.has_value();

        // fix: prevent restart gizmo when reselect object
        // take responsibility for left up
        if (m_parent.get_first_hover_volume_idx() >= 0)
            m_mouse_left_down = true;
    }
    else if (mouse_event.LeftUp()) {
        if (m_mouse_left_down) {
            // responsible for mouse left up after selecting plane
            m_mouse_left_down = false;
            return true;
        }
        if (m_hover_id == -1 && !m_parent.is_mouse_dragging())
            // avoid closing the gizmo if the user clicks outside of any volume
            return true;
    }
    else if (mouse_event.RightDown()) {
        // let the event pass through to allow panning/rotating the 3D scene
        if (mouse_event.CmdDown())
            return false;
    }
    else if (mouse_event.Leaving())
        m_mouse_left_down = false;

    return false;
}


void GLGizmoMeasure::data_changed(bool is_serializing)
{
    wxBusyCursor wait;

    if (m_pending_scale > 0) {
        update_if_needed();
        register_single_mesh_pick();
        update_measurement_result();
        m_pending_scale --;
    }
    else {
        m_parent.toggle_selected_volume_visibility(true);
        reset_all_pick();
        update_if_needed();
        register_single_mesh_pick();
        reset_all_feature();
    }
    m_last_inv_zoom  = 0.0f;
    m_last_plane_idx = -1;
    m_editing_distance                = false;
    m_is_editing_distance_first_frame = true;
}

bool GLGizmoMeasure::gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    if (action == SLAGizmoEventType::ShiftDown) {
        if (m_shift_kar_filter.is_first()) {
            m_mode = EMode::PointSelection;
            disable_scene_raycasters();
        }
        m_shift_kar_filter.increase_count();
    }
    else if (action == SLAGizmoEventType::ShiftUp) {
        m_shift_kar_filter.reset_count();
        m_mode = EMode::FeatureSelection;
        restore_scene_raycasters_state();
    }
    else if (action == SLAGizmoEventType::Delete) {
        reset_all_feature();
        m_parent.request_extra_frame();
    }
    else if (action == SLAGizmoEventType::Escape) {
        if (!m_selected_features.first.feature.has_value()) {
            update_measurement_result();
            return false;
        }
        else {
            if (m_selected_features.second.feature.has_value()) {
                reset_feature2();
            }
            else {
                reset_feature1();
            }
        }
    }
    return true;
}

bool GLGizmoMeasure::on_init()
{
    m_shortcut_key = WXK_CONTROL_U;

    const wxString shift = _L("Shift+");

    m_desc["feature_selection"]         = _L("Select feature");
    m_desc["point_selection_caption"]   = shift + _L("Left mouse button");
    m_desc["point_selection"]           = _L("Select point");
    m_desc["reset_caption"]             = _L("Delete");
    m_desc["reset"]                     = _L("Restart selection");
    m_desc["unselect_caption"]          = _L("Esc");
    m_desc["unselect"]                  = _L("Cancel a feature until exit");

    return true;
}

void GLGizmoMeasure::on_set_state()
{
    if (m_state == Off) {
        m_parent.toggle_selected_volume_visibility(false);
        m_shift_kar_filter.reset_count();
        m_curr_feature.reset();
        m_curr_point_on_feature_position.reset();
        restore_scene_raycasters_state();
        m_editing_distance = false;
        m_is_editing_distance_first_frame = true;
        std::map<GLVolume *, std::shared_ptr<Measure::Measuring>>().swap(m_mesh_measure_map);
    }
    else {
        m_mode = EMode::FeatureSelection;
        m_hover_id = -1;
        m_show_reset_first_tip = false;
        m_only_select_plane    = false;
        m_distance             = Vec3d::Zero();
    }
}

std::string GLGizmoMeasure::on_get_name() const
{
    if (!on_is_activable() && m_state == EState::Off) {
        if (wxGetApp().plater()->canvas3D()->get_canvas_type() == GLCanvas3D::ECanvasType::CanvasAssembleView) {
            return _u8L("Measure") + ":\n" + _u8L("Please confirm explosion ratio = 1, and please select at least one object.");
        }
        else {
            return _u8L("Measure") + ":\n" + _u8L("Please select at least one object.");
        }
    } else {
        return _u8L("Measure");
    }
}

bool GLGizmoMeasure::on_is_activable() const
{
    const Selection& selection = m_parent.get_selection();
    if (selection.is_wipe_tower()) {
        return false;
    }
    if (wxGetApp().plater()->canvas3D()->get_canvas_type() == GLCanvas3D::ECanvasType::CanvasAssembleView) {
        if (abs(m_parent.get_explosion_ratio() - 1.0f) < 1e-2 && selection.volumes_count() > 0) {
            return true;
        }
        return false;
    } else {
        return selection.volumes_count() > 0;
    }
}

void GLGizmoMeasure::init_circle_glmodel(GripperType gripper_type, const Measure::SurfaceFeature &feature, CircleGLModel &circle_gl_model,float inv_zoom)
{
    const auto [center, radius, normal] = feature.get_circle();
    auto cur_feature =const_cast<Measure::SurfaceFeature *>(&feature);
    if (circle_gl_model.inv_zoom != inv_zoom) {
        if (circle_gl_model.last_circle_feature) {
            const auto [last_center, last_radius, last_normal] = circle_gl_model.last_circle_feature->get_circle();
            float eps    = 1e-2;
            if ((last_center - center).norm() < eps && (last_normal - normal).norm() < eps && abs(last_radius - radius) < eps){
                return;
            }
        }
        reset_gripper_pick(gripper_type);
        circle_gl_model.circle.reset();
        GLModel::Geometry circle_geometry = init_torus_data(64, 16, center.cast<float>(), float(radius), 5.0f * inv_zoom, normal.cast<float>(), Transform3f::Identity());
        circle_gl_model.circle.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(circle_geometry.get_as_indexed_triangle_set()));
        circle_gl_model.circle.model.init_from(std::move(circle_geometry));
        if (circle_gl_model.circle.model.is_initialized()) {
            m_gripper_id_raycast_map[gripper_type] = std::make_shared<PickRaycaster>(CIRCLE_ID, *m_curr_circle.circle.mesh_raycaster);
        }
        circle_gl_model.last_circle_feature = cur_feature;
        circle_gl_model.inv_zoom            = inv_zoom;
    }
}
const float MEASURE_PLNE_NORMAL_OFFSET = 0.05;
void GLGizmoMeasure::init_plane_glmodel(GripperType gripper_type, const Measure::SurfaceFeature &feature, PlaneGLModel &plane_gl_model)
{
    if (!feature.volume) { return; }
    Selection&  selection = m_parent.get_selection();
    auto volume = static_cast<GLVolume *>(feature.volume);
    const ModelObject* obj      = selection.get_model()->objects[volume->object_idx()];
    const ModelVolume* vol      = obj->volumes[volume->volume_idx()];
    auto               mesh = vol->mesh_ptr();
    if (!feature.plane_indices) { return; }
    // Ultra: Triangle / Curve reuse the plane highlighter. Their cache key is the (negated, offset) facet
    // index so it can never collide with a real plane index.
    const int idx = feature.get_type() == Measure::SurfaceFeatureType::Plane ? std::get<0>(feature.get_plane())
                                                                              : -int(feature.get_value()) - 1;
    if (plane_gl_model.plane_idx != idx) {
        plane_gl_model.plane.reset();
    }
    if (!plane_gl_model.plane.model.is_initialized()) {
        plane_gl_model.plane_idx    = idx;
        reset_gripper_pick(gripper_type);
        GLModel::Geometry init_data = init_plane_data(mesh->its, *feature.plane_indices, MEASURE_PLNE_NORMAL_OFFSET);
        plane_gl_model.plane.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(init_data.get_as_indexed_triangle_set()));
        plane_gl_model.plane.model.init_from(std::move(init_data));
        if (plane_gl_model.plane.model.is_initialized()) {
            m_gripper_id_raycast_map[gripper_type] = std::make_shared<PickRaycaster>(PLANE_ID, *plane_gl_model.plane.mesh_raycaster, feature.world_tran);
        }
    }
}

void GLGizmoMeasure::on_render()
{
#if ENABLE_MEASURE_GIZMO_DEBUG
    render_debug_dialog();
#endif // ENABLE_MEASURE_GIZMO_DEBUG

    Vec2d mouse_position = m_parent.get_local_mouse_position();
    update_if_needed();
    ultra_load_exact_setting();
    m_ultra_exact_hover.valid = false; // Ultra (Exact highlight): re-armed below while the cursor is on the moving part

    const Camera& camera = wxGetApp().plater()->get_camera();
    const float inv_zoom = (float)camera.get_inv_zoom();
    bool mouse_on_gripper = false;
    bool mouse_on_object =false;
    {
        if (!m_editing_distance) {
            Vec3f  hit                          = Vec3f::Zero();
            double closest_hit_squared_distance = std::numeric_limits<double>::max();
            for (auto item : m_gripper_id_raycast_map) {

                auto  world_tran = item.second->get_transform();
                Vec3f normal_on_gripper;
                if (item.second->get_raycaster()->closest_hit(mouse_position, item.second->get_transform(), camera, hit, normal_on_gripper)) {
                    double hit_squared_distance = (camera.get_position() - world_tran * hit.cast<double>()).squaredNorm();
                    if (hit_squared_distance < closest_hit_squared_distance) {
                        closest_hit_squared_distance = hit_squared_distance;
                        if (item.second->get_id() > 0) {
                            m_hover_id       = item.second->get_id();
                            mouse_on_gripper = true;
                        }
                    }
                }
            }
        }
    }
    Vec3d  position_on_model;
    Vec3d  direction_on_model;
    size_t model_facet_idx = -1;
    double closest_hit_distance = std::numeric_limits<double>::max();
    {
        if (!m_editing_distance) {
            for (auto item : m_mesh_raycaster_map) {
                auto   raycaster                    = item.second->get_raycaster();
                auto   world_tran                   = item.second->get_transform();
                Vec3f  normal                       = Vec3f::Zero();
                Vec3f  hit                          = Vec3f::Zero();
                size_t facet                        = 0;
                if (raycaster->unproject_on_mesh(mouse_position, world_tran, camera, hit, normal, nullptr, &facet)) {
                    // Is this hit the closest to the camera so far?
                    double hit_squared_distance = (camera.get_position() - world_tran * hit.cast<double>()).norm();
                    if (hit_squared_distance < closest_hit_distance) {
                        closest_hit_distance = hit_squared_distance;
                        mouse_on_object   = true;
                        model_facet_idx   = facet;
                        position_on_model = hit.cast<double>();
                        m_last_hit_volume = item.first;
                        m_curr_measuring  = m_mesh_measure_map[m_last_hit_volume];
                    }
                }
            }
        }

        if (!(mouse_on_gripper || mouse_on_object)) {
            m_hover_id = -1;
            m_last_plane_idx = -1;
            reset_gripper_pick(GripperType::PLANE);
            reset_gripper_pick(GripperType::CIRCLE);
        }
        if (m_mouse_left_down_mesh_deal && m_hover_id >= 0) {
            m_mouse_left_down_mesh_deal = false;
            if (m_selected_features.second.feature.has_value()) {
                if (m_hit_order_volumes.size() == 1) {
                    m_hit_order_volumes.push_back(m_last_hit_volume);
                } else {
                    m_hit_order_volumes[1] = m_last_hit_volume;
                }
                // deal hit_different_volumes
                if (m_hit_different_volumes.size() >= 1) {
                    if (m_last_hit_volume == m_hit_different_volumes[0]) {
                        if (m_hit_different_volumes.size() == 2) {//hit same volume
                            m_hit_different_volumes.erase(m_hit_different_volumes.begin() + 1);
                        }
                    } else  {
                        if (m_hit_different_volumes.size() == 2) {
                            m_hit_different_volumes[1] = m_last_hit_volume;
                        } else {
                            m_hit_different_volumes.push_back(m_last_hit_volume);
                        }
                    }
                }
            }
            else if (m_selected_features.first.feature.has_value()) {
                if (m_hit_order_volumes.size() == 0) {
                    m_hit_order_volumes.push_back(m_last_hit_volume);
                } else {
                    m_hit_order_volumes[0] = m_last_hit_volume;
                }
                //deal hit_different_volumes
                if (m_hit_different_volumes.size() == 0) {
                    m_hit_different_volumes.push_back(m_last_hit_volume);
                }
            }
        }
    }
    //const bool mouse_on_object = m_raycaster->unproject_on_mesh(mouse_position, Transform3d::Identity(), camera, position_on_model, normal_on_model, nullptr, &model_facet_idx);
    const bool is_hovering_on_feature = m_mode == EMode::PointSelection && m_hover_id != -1;

    if (m_mode == EMode::FeatureSelection || m_mode == EMode::PointSelection) {
        if (m_hover_id == SEL_SPHERE_1_ID || m_hover_id == SEL_SPHERE_2_ID) {
            // Skip feature detection if hovering on a selected point/center
            //reset_gripper_pick(GripperType::UNDEFINE, true);
            m_curr_feature.reset();
            m_curr_point_on_feature_position.reset();
        }
        else {
            std::optional<Measure::SurfaceFeature> curr_feature = std::nullopt;
            if (m_curr_measuring) {
                if (wxGetMouseState().LeftIsDown()) {
                    curr_feature = m_curr_feature;
                } else if (mouse_on_object) {
                    // Ultra: view-scaled pick radius (~8 px) expressed in MESH units, so vertices and edges
                    // stay pickable at any zoom and on scaled instances (the legacy limit was a fixed
                    // 0.5 mm in mesh space -- sub-pixel when zoomed out).
                    const Transform3d hit_tran   = m_mesh_raycaster_map[m_last_hit_volume]->get_transform();
                    const double      mesh_scale = std::max(1e-6, (hit_tran.linear() * Vec3d::UnitX()).norm());
                    const double      snap_radius = 8.0 * double(inv_zoom) / mesh_scale;
                    // Ultra: Triangle / Curve assembly modes pick the raw facet / a curved patch directly.
                    const int pick_kind = (m_measure_mode == EMeasureMode::ONLY_ASSEMBLY && m_assembly_mode == AssemblyMode::TRIANGLE_TRIANGLE) ? 1 :
                                          (m_measure_mode == EMeasureMode::ONLY_ASSEMBLY && m_assembly_mode == AssemblyMode::CURVE_CURVE)       ? 2 : 0;
                    curr_feature = m_curr_measuring->get_feature(model_facet_idx, position_on_model, hit_tran, m_only_select_plane, snap_radius, pick_kind,
                                                                 ultra_curve_pick_params());
                }
            }
            if (m_measure_mode == EMeasureMode::ONLY_ASSEMBLY) {
                if (m_assembly_mode == AssemblyMode::FACE_FACE) {
                    // Ultra (Stage 2): allow Circle rims (peg/hole/shaft) as mate features, not just flat faces.
                    if (curr_feature.has_value() &&
                        curr_feature->get_type() != Measure::SurfaceFeatureType::Plane &&
                        curr_feature->get_type() != Measure::SurfaceFeatureType::Circle) {
                        curr_feature.reset();
                    }
                } else if (m_assembly_mode == AssemblyMode::TRIANGLE_TRIANGLE) {
                    // Ultra: only the raw facet pick counts in this mode.
                    if (curr_feature.has_value() && curr_feature->get_type() != Measure::SurfaceFeatureType::Triangle)
                        curr_feature.reset();
                } else if (m_assembly_mode == AssemblyMode::CURVE_CURVE) {
                    // Ultra: only a curved-patch pick counts in this mode (Stage C wires the patch grow).
                    if (curr_feature.has_value() && curr_feature->get_type() != Measure::SurfaceFeatureType::Curve)
                        curr_feature.reset();
                } else if (m_assembly_mode == AssemblyMode::POINT_POINT) {
                    // Ultra: guard the empty case (cursor off the mesh) -- this used to dereference nullopt.
                    if (curr_feature.has_value() &&
                        !(curr_feature->get_type() == Measure::SurfaceFeatureType::Point ||
                        curr_feature->get_type() == Measure::SurfaceFeatureType::Circle ||
                        (m_mode == EMode::PointSelection && (curr_feature->get_type() == Measure::SurfaceFeatureType::Plane || curr_feature->get_type() == Measure::SurfaceFeatureType::Edge)))) {
                        curr_feature.reset();
                    }
                }
            }
            if (m_curr_feature != curr_feature ||
                (curr_feature.has_value() && curr_feature->get_type() == Measure::SurfaceFeatureType::Circle && (m_curr_feature != curr_feature || m_last_inv_zoom != inv_zoom))) {
                reset_gripper_pick(GripperType::UNDEFINE, true);

                m_curr_feature = curr_feature;
                if (!m_curr_feature.has_value())
                    return;
                m_curr_feature->volume     = m_last_hit_volume;
                m_curr_feature->world_tran = m_mesh_raycaster_map[m_last_hit_volume]->get_transform();

                switch (m_curr_feature->get_type()) {
                default: { assert(false); break; }
                case Measure::SurfaceFeatureType::Point:
                {
                    m_gripper_id_raycast_map[GripperType::POINT] = std::make_shared<PickRaycaster>(POINT_ID, *m_sphere.mesh_raycaster);
                    break;
                }
                case Measure::SurfaceFeatureType::Edge:
                {
                    m_gripper_id_raycast_map[GripperType::EDGE] = std::make_shared<PickRaycaster>(EDGE_ID, *m_cylinder.mesh_raycaster);
                    break;
                }
                case Measure::SurfaceFeatureType::Circle: {
                    m_curr_circle.last_circle_feature = nullptr;
                    m_curr_circle.inv_zoom            = 0;
                    init_circle_glmodel(GripperType::CIRCLE, *m_curr_feature, m_curr_circle,inv_zoom);
                    break;
                }
                case Measure::SurfaceFeatureType::Plane: {
                    update_world_plane_features(m_curr_measuring.get(), *m_curr_feature);
                    m_curr_plane.plane_idx = -1;
                    init_plane_glmodel(GripperType::PLANE, *m_curr_feature, m_curr_plane);
                    break;
                }
                // Ultra: Triangle / Curve carry their own facet list -> highlight via the plane model,
                // but no plane-border features to gather.
                case Measure::SurfaceFeatureType::Triangle:
                case Measure::SurfaceFeatureType::Curve: {
                    m_curr_plane.plane_idx = -1;
                    init_plane_glmodel(GripperType::PLANE, *m_curr_feature, m_curr_plane);
                    break;
                }
                }
            }
        }
    }

    // Ultra (Exact highlight): with the target picked, remember where the cursor sits on the moving part
    // (and the facet normal there) so the target's shape can be drawn -- and later mated -- at that spot.
    if (ultra_exact_active() && m_mode == EMode::FeatureSelection && mouse_on_object && m_curr_measuring &&
        m_curr_feature.has_value() && m_curr_feature->volume == m_last_hit_volume &&
        m_selected_features.first.feature.has_value() && m_selected_features.first.feature->is_planar() &&
        m_selected_features.first.feature->volume != m_last_hit_volume) {
        const indexed_triangle_set& its = m_curr_measuring->get_its();
        if (model_facet_idx < its.indices.size()) {
            const auto& f = its.indices[model_facet_idx];
            const Vec3d a = its.vertices[f[0]].cast<double>(), b = its.vertices[f[1]].cast<double>(), c = its.vertices[f[2]].cast<double>();
            const Vec3d n = (b - a).cross(c - a);
            if (n.norm() > 1e-12) {
                m_ultra_exact_hover.volume      = m_last_hit_volume;
                m_ultra_exact_hover.hit_mesh    = position_on_model;
                m_ultra_exact_hover.normal_mesh = n.normalized();
                m_ultra_exact_hover.valid       = true;
            }
        }
    }

    if (m_mode != EMode::PointSelection) {
        m_curr_point_on_feature_position.reset();
    }
    else if (is_hovering_on_feature) {
        auto position_on_feature = [this, &mouse_position](int feature_type_id, const Camera &camera, std::function<Vec3f(const Vec3f &)> callback = nullptr) -> Vec3d {
            auto it = m_gripper_id_raycast_map.find((GripperType) feature_type_id); // m_raycasters.find(feature_type_id);
            if (it != m_gripper_id_raycast_map.end() && it->second != nullptr) {
                Vec3f p;
                Vec3f n;
                const Transform3d& trafo = it->second->get_transform();
                bool res = it->second->get_raycaster()->closest_hit(mouse_position, trafo, camera, p, n);
                if (res) {
                    if (callback)
                        p = callback(p);
                    return trafo * p.cast<double>();
                }
            }
            return Vec3d(DBL_MAX, DBL_MAX, DBL_MAX);
        };
        if (m_curr_feature.has_value()) {
            switch (m_curr_feature->get_type())
            {
            default: { assert(false); break; }
            case Measure::SurfaceFeatureType::Point:
            {
                m_curr_point_on_feature_position = m_curr_feature->get_point();
                break;
            }
            case Measure::SurfaceFeatureType::Edge:
            {
                const std::optional<Vec3d> extra = m_curr_feature->get_extra_point();
                if (extra.has_value() && m_hover_id == GripperType::POINT)
                    m_curr_point_on_feature_position = *extra;
                else {
                    const Vec3d pos = position_on_feature(GripperType::EDGE, camera, [](const Vec3f &v) { return Vec3f(0.0f, 0.0f, v.z()); });
                    if (!pos.isApprox(Vec3d(DBL_MAX, DBL_MAX, DBL_MAX)))
                        m_curr_point_on_feature_position = pos;
                }
                break;
            }
            case Measure::SurfaceFeatureType::Plane:
            {
                m_curr_point_on_feature_position = position_on_feature(GripperType::PLANE, camera);
                break;
            }
            case Measure::SurfaceFeatureType::Circle:
            {
                const auto [center, radius, normal] = m_curr_feature->get_circle();
                if (m_hover_id == POINT_ID)
                    m_curr_point_on_feature_position = center;
                else {
                    const Vec3d world_pof = position_on_feature(GripperType::CIRCLE, camera, [](const Vec3f &v) { return v; });
                    const Eigen::Hyperplane<double, 3> plane(normal, center);
                    const Transform3d local_to_model_matrix = Geometry::translation_transform(center) * Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitZ(), normal);
                    const Vec3d local_proj = local_to_model_matrix.inverse() * plane.projection(world_pof);
                    double angle = std::atan2(local_proj.y(), local_proj.x());
                    if (angle < 0.0)
                        angle += 2.0 * double(M_PI);

                    const Vec3d local_pos = radius * Vec3d(std::cos(angle), std::sin(angle), 0.0);
                    m_curr_point_on_feature_position = local_to_model_matrix * local_pos;
                }
                break;
            }
            }
        }
    }
    else {
        m_curr_point_on_feature_position.reset();
    }

    ultra_exact_refresh_models();

    if (!m_curr_feature.has_value() && !m_selected_features.first.feature.has_value()) {
        return;
    }

    GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;

    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));
    const bool old_cullface = ::glIsEnabled(GL_CULL_FACE);
    glsafe(::glDisable(GL_CULL_FACE));

    const Transform3d& view_matrix = camera.get_view_matrix();

    auto set_matrix_uniforms = [shader, &view_matrix](const Transform3d& model_matrix) {
        const Transform3d view_model_matrix = view_matrix * model_matrix;
        shader->set_uniform("view_model_matrix", view_model_matrix);
        const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);
    };

    auto set_emission_uniform = [shader](const ColorRGBA& color, bool hover) {
        shader->set_uniform("emission_factor", /*(color == GLVolume::SELECTED_COLOR) ? 0.0f :*/
            hover ? 0.5f : 0.25f);
    };

    auto render_glmodel = [set_matrix_uniforms, set_emission_uniform](PickingModel& model, const ColorRGBA& color, const Transform3d& model_matrix, bool hover) {
        set_matrix_uniforms(model_matrix);
        set_emission_uniform(color, hover);
        model.model.set_color(color);
        model.model.render();
    };

    auto render_feature =
        [this, render_glmodel](const Measure::SurfaceFeature& feature, const std::vector<ColorRGBA>& colors,
        float inv_zoom, bool hover, bool update_raycasters_transform,int featura_index = -1) {
            switch (feature.get_type())
            {
            default: { assert(false); break; }
            case Measure::SurfaceFeatureType::Point:
            {
                const Transform3d feature_matrix =  Geometry::translation_transform(feature.get_point()) * Geometry::scale_transform(inv_zoom);
                Geometry::Transformation tran(feature_matrix);
                render_glmodel(m_sphere, colors.front(), tran.get_matrix(), hover);
                if (update_raycasters_transform) {
                    auto it = m_gripper_id_raycast_map.find(GripperType::POINT);
                    if (it != m_gripper_id_raycast_map.end() && it->second != nullptr)
                        it->second->set_transform(feature_matrix);
                }
                break;
            }
            case Measure::SurfaceFeatureType::Circle:
            {
                const auto& [center, radius, normal] = feature.get_circle();
                // render circle
                const Transform3d circle_matrix =  Transform3d::Identity();
                //set_matrix_uniforms(circle_matrix);
                Geometry::Transformation tran(circle_matrix);
                if (featura_index == -1) {
                    init_circle_glmodel(GripperType::CIRCLE, feature, m_curr_circle, inv_zoom);
                    render_glmodel(m_curr_circle.circle, colors.front(), tran.get_matrix(), hover);
                }
                // render plane feature1 or feature2
                if (featura_index == 0) { // feature1
                    init_circle_glmodel(GripperType::CIRCLE_1, feature, m_feature_circle_first, inv_zoom);
                    render_glmodel(m_feature_circle_first.circle, colors.front(), tran.get_matrix(), hover);
                } else if (featura_index == 1) { // feature2
                    init_circle_glmodel(GripperType::CIRCLE_2, feature, m_feature_circle_second, inv_zoom);
                    render_glmodel(m_feature_circle_second.circle, colors.front(), tran.get_matrix(), hover);
                }
                // render center
                if (colors.size() > 1) {
                    const Transform3d center_matrix = Geometry::translation_transform(center) * Geometry::scale_transform(inv_zoom);
                    render_glmodel(m_sphere, colors.back(), center_matrix, hover);

                    Geometry::Transformation tran(center_matrix);
                    auto it = m_gripper_id_raycast_map.find(GripperType::POINT);
                    if (it != m_gripper_id_raycast_map.end() && it->second != nullptr)
                        it->second->set_transform(center_matrix);
                }
                break;
            }
            case Measure::SurfaceFeatureType::Edge:
            {
                const auto& [from, to] = feature.get_edge();
                // render edge
                const Transform3d edge_matrix =  Geometry::translation_transform(from) *
                    Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitZ(), to - from) *
                    Geometry::scale_transform({ (double)inv_zoom, (double)inv_zoom, (to - from).norm() });

                Geometry::Transformation tran(edge_matrix);
                render_glmodel(m_cylinder, colors.front(), tran.get_matrix(), hover);
                if (update_raycasters_transform) {
                    auto it = m_gripper_id_raycast_map.find(GripperType::EDGE);
                    if (it != m_gripper_id_raycast_map.end() && it->second != nullptr){
                        it->second->set_transform(edge_matrix);
                    }
                }
                // render extra point
                if (colors.size() > 1) {
                    const std::optional<Vec3d> extra = feature.get_extra_point();
                    if (extra.has_value()) {
                        const Transform3d point_matrix = Geometry::translation_transform(*extra) * Geometry::scale_transform(inv_zoom);
                        Geometry::Transformation tran(point_matrix);
                        render_glmodel(m_sphere, colors.back(), tran.get_matrix(), hover);

                        auto it = m_gripper_id_raycast_map.find(GripperType::POINT);
                        if (it != m_gripper_id_raycast_map.end() && it->second != nullptr) {
                            it->second->set_transform(point_matrix);
                        }
                    }
                }
                break;
            }
            case Measure::SurfaceFeatureType::Triangle: // Ultra: facet / patch highlights reuse the plane model
            case Measure::SurfaceFeatureType::Curve:
            case Measure::SurfaceFeatureType::Plane: {
                if (featura_index == -1) {
                    // Ultra (Exact highlight): only the target's shape at the cursor, not the whole face.
                    if (m_ultra_exact_hover_shown)
                        render_glmodel(m_ultra_exact_hover_model, colors.back(), Transform3d::Identity(), hover);
                    else
                        render_glmodel(m_curr_plane.plane, colors.back(), feature.world_tran, hover);
                    break;
                }
                //render plane feature1 or feature2
                if (featura_index == 0) {//feature1
                    init_plane_glmodel(GripperType::PLANE_1, feature, m_feature_plane_first);
                    render_glmodel(m_feature_plane_first.plane, colors.back(), feature.world_tran, hover);
                } else if (featura_index == 1) {//feature2
                    init_plane_glmodel(GripperType::PLANE_2, feature, m_feature_plane_second); // keeps the face pickable
                    if (m_ultra_exact_pick_shown) // Ultra (Exact highlight): show the clicked footprint only
                        render_glmodel(m_ultra_exact_pick_model, colors.back(), Transform3d::Identity(), hover);
                    else
                        render_glmodel(m_feature_plane_second.plane, colors.back(), feature.world_tran, hover);
                }
                break;
            }
            }
    };

    auto hover_selection_color = [this]() {
      return ((m_mode == EMode::PointSelection && !m_selected_features.first.feature.has_value()) ||
              (m_mode != EMode::PointSelection && (!m_selected_features.first.feature.has_value() || *m_curr_feature == *m_selected_features.first.feature))) ?
          SELECTED_1ST_COLOR : SELECTED_2ND_COLOR;
    };

    auto hovering_color = [this, hover_selection_color]() {
        return (m_mode == EMode::PointSelection) ? HOVER_COLOR : hover_selection_color();
    };

    if (m_curr_feature.has_value()) {
        // render hovered feature
        std::vector<ColorRGBA> colors;
        if (m_selected_features.first.feature.has_value() && *m_curr_feature == *m_selected_features.first.feature) {
            // hovering over the 1st selected feature
            if (m_selected_features.first.is_center)
                // hovering over a center
                colors = { NEUTRAL_COLOR, hovering_color() };
            else if (is_feature_with_center(*m_selected_features.first.feature))
                // hovering over a feature with center
                colors = { hovering_color(), NEUTRAL_COLOR };
            else
                colors = { hovering_color() };
        }
        else if (m_selected_features.second.feature.has_value() && *m_curr_feature == *m_selected_features.second.feature) {
            // hovering over the 2nd selected feature
            if (m_selected_features.second.is_center)
                // hovering over a center
                colors = { NEUTRAL_COLOR, hovering_color() };
            else if (is_feature_with_center(*m_selected_features.second.feature))
                // hovering over a feature with center
                colors = { hovering_color(), NEUTRAL_COLOR };
            else
                colors = { hovering_color() };
        }
        else {
            switch (m_curr_feature->get_type())
            {
            default: { assert(false); break; }
            case Measure::SurfaceFeatureType::Point:
            {
                colors.emplace_back(hover_selection_color());
                break;
            }
            case Measure::SurfaceFeatureType::Edge:
            case Measure::SurfaceFeatureType::Circle:
            {
                if (m_selected_features.first.is_center && m_curr_feature == m_selected_features.first.source)
                    colors = { SELECTED_1ST_COLOR, NEUTRAL_COLOR };
                else if (m_selected_features.second.is_center && m_curr_feature == m_selected_features.second.source)
                    colors = { SELECTED_2ND_COLOR, NEUTRAL_COLOR };
                else
                    colors = { hovering_color(), hovering_color() };
                break;
            }
            case Measure::SurfaceFeatureType::Triangle:
            case Measure::SurfaceFeatureType::Curve:
            case Measure::SurfaceFeatureType::Plane:
            {
                colors.emplace_back(hovering_color());
                break;
            }
            }
        }

        render_feature(*m_curr_feature, colors, inv_zoom, true, true);
    }

    if (m_selected_features.first.feature.has_value() && (!m_curr_feature.has_value() || *m_curr_feature != *m_selected_features.first.feature)) {
        // render 1st selected feature

        std::optional<Measure::SurfaceFeature> feature_to_render;
        std::vector<ColorRGBA> colors;
        bool requires_raycaster_update = false;
        if (m_hover_id == SEL_SPHERE_1_ID && (m_selected_features.first.is_center || is_feature_with_center(*m_selected_features.first.feature))) {
            // hovering over a center
            feature_to_render = m_selected_features.first.source;
            colors = { NEUTRAL_COLOR, SELECTED_1ST_COLOR };
            requires_raycaster_update = true;
        }
        else if (is_feature_with_center(*m_selected_features.first.feature)) {
            // hovering over a feature with center
            feature_to_render = m_selected_features.first.feature;
            colors = { SELECTED_1ST_COLOR, NEUTRAL_COLOR };
            requires_raycaster_update = true;
        }
        else {
            feature_to_render = m_selected_features.first.feature;
            colors = { SELECTED_1ST_COLOR };
            requires_raycaster_update = m_selected_features.first.feature->get_type() == Measure::SurfaceFeatureType::Point;
        }

        render_feature(*feature_to_render, colors, inv_zoom, m_hover_id == SEL_SPHERE_1_ID, false, 0);

        if (requires_raycaster_update) {
            if (m_gripper_id_raycast_map.find(GripperType::SPHERE_1) != m_gripper_id_raycast_map.end()) {
                m_gripper_id_raycast_map[GripperType::SPHERE_1]->set_transform(Geometry::translation_transform(get_feature_offset(*m_selected_features.first.feature)) *
                                                                               Geometry::scale_transform(inv_zoom));
            }
        }
    }

    if (m_selected_features.second.feature.has_value() && (!m_curr_feature.has_value() || *m_curr_feature != *m_selected_features.second.feature)) {
        // render 2nd selected feature

        std::optional<Measure::SurfaceFeature> feature_to_render;
        std::vector<ColorRGBA> colors;
        bool requires_raycaster_update = false;
        if (m_hover_id == SEL_SPHERE_2_ID && (m_selected_features.second.is_center || is_feature_with_center(*m_selected_features.second.feature))) {
            // hovering over a center
            feature_to_render = m_selected_features.second.source;
            colors = { NEUTRAL_COLOR, SELECTED_2ND_COLOR };
            requires_raycaster_update = true;
        }
        else if (is_feature_with_center(*m_selected_features.second.feature)) {
            // hovering over a feature with center
            feature_to_render = m_selected_features.second.feature;
            colors = { SELECTED_2ND_COLOR, NEUTRAL_COLOR };
            requires_raycaster_update = true;
        }
        else {
            feature_to_render = m_selected_features.second.feature;
            colors = { SELECTED_2ND_COLOR };
            requires_raycaster_update = m_selected_features.second.feature->get_type() == Measure::SurfaceFeatureType::Point;
        }

        render_feature(*feature_to_render, colors, inv_zoom, m_hover_id == SEL_SPHERE_2_ID, false, 1);

        if (requires_raycaster_update) {
            if (m_gripper_id_raycast_map.find(GripperType::SPHERE_2) != m_gripper_id_raycast_map.end()) {
                m_gripper_id_raycast_map[GripperType::SPHERE_2]->set_transform(Geometry::translation_transform(get_feature_offset(*m_selected_features.first.feature)) *
                                                                               Geometry::scale_transform(inv_zoom));
            }
        }
    }

    if (is_hovering_on_feature && m_curr_point_on_feature_position.has_value()) {
        if (m_hover_id != POINT_ID) {
            // render point on feature while SHIFT is pressed
            const Transform3d matrix = Geometry::translation_transform(*m_curr_point_on_feature_position) * Geometry::scale_transform(inv_zoom);
            const ColorRGBA color = hover_selection_color();

            Geometry::Transformation tran(matrix);
            render_glmodel(m_sphere, color, tran.get_matrix(), true);
        }
    }

    shader->stop_using();

    if (old_cullface)
        glsafe(::glEnable(GL_CULL_FACE));

    render_dimensioning();
}

void GLGizmoMeasure::update_if_needed()
{
    //const Selection& selection = m_parent.get_selection();
    //if (selection.is_empty())
    //    return;

    //const Selection::IndicesList& idxs = selection.get_volume_idxs();
    //std::vector<VolumeCacheItem> volumes_cache;
    //volumes_cache.reserve(idxs.size());
    //for (unsigned int idx : idxs) {
    //    const GLVolume* v = selection.get_volume(idx);
    //    const int volume_idx = v->volume_idx();
    //    if (volume_idx < 0)
    //        continue;

    //    const ModelObject* obj = selection.get_model()->objects[v->object_idx()];
    //    const ModelInstance* inst = obj->instances[v->instance_idx()];
    //    const ModelVolume* vol = obj->volumes[volume_idx];
    //    const VolumeCacheItem item = {
    //        obj, inst, vol,
    //        Geometry::translation_transform(selection.get_first_volume()->get_sla_shift_z() * Vec3d::UnitZ()) * inst->get_matrix() * vol->get_matrix()
    //    };
    //    volumes_cache.emplace_back(item);
    //}

    //if (m_state != On || volumes_cache.empty())
    //    return;

    //if (m_volumes_cache != volumes_cache) {
    //    m_volumes_cache = volumes_cache;
    //}
}

void GLGizmoMeasure::disable_scene_raycasters()
{
    for(auto iter:m_gripper_id_raycast_map)
    {
        iter.second->set_active(false);
    }
}

void GLGizmoMeasure::restore_scene_raycasters_state()
{
    for(auto iter:m_gripper_id_raycast_map)
    {
        iter.second->set_active(true);
    }
}


void GLGizmoMeasure::render_dimensioning()
{
    static SelectedFeatures last_selected_features;

    if (!m_selected_features.first.feature.has_value())
        return;

    if (!m_selected_features.second.feature.has_value() && m_selected_features.first.feature->get_type() != Measure::SurfaceFeatureType::Circle)
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    auto point_point = [this, &shader](const Vec3d &v1, const Vec3d &v2, float distance, const ColorRGBA &color, bool show_label = true, bool show_first_tri = true) {
        if ((v2 - v1).squaredNorm() < 0.000001 || abs(distance) < 0.001f)
            return;

        const Camera& camera = wxGetApp().plater()->get_camera();
        const Matrix4d projection_view_matrix = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
        const std::array<int, 4>& viewport = camera.get_viewport();

        // screen coordinates
        const Vec2d v1ss = TransformHelper::world_to_ss(v1, projection_view_matrix, viewport);
        const Vec2d v2ss = TransformHelper::world_to_ss(v2, projection_view_matrix, viewport);

        if (v1ss.isApprox(v2ss))
            return;

        const Vec2d v12ss = v2ss - v1ss;
        const double v12ss_len = v12ss.norm();

        const bool overlap = v12ss_len - 2.0 * TRIANGLE_HEIGHT < 0.0;

        const auto q12ss = Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitX(), Vec3d(v12ss.x(), v12ss.y(), 0.0));
        const auto q21ss = Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitX(), Vec3d(-v12ss.x(), -v12ss.y(), 0.0));

        shader->set_uniform("projection_matrix", Transform3d::Identity());

        const Vec3d v1ss_3 = { v1ss.x(), v1ss.y(), 0.0 };
        const Vec3d v2ss_3 = { v2ss.x(), v2ss.y(), 0.0 };

        const Transform3d ss_to_ndc_matrix = TransformHelper::ndc_to_ss_matrix_inverse(viewport);

#if ENABLE_GL_CORE_PROFILE
        if (OpenGLManager::get_gl_info().is_core_profile()) {
            shader->stop_using();

            shader = wxGetApp().get_shader("dashed_thick_lines");
            if (shader == nullptr)
                return;

            shader->start_using();
            shader->set_uniform("projection_matrix", Transform3d::Identity());
            const std::array<int, 4>& viewport = camera.get_viewport();
            shader->set_uniform("viewport_size", Vec2d(double(viewport[2]), double(viewport[3])));
            shader->set_uniform("width", 1.0f);
            shader->set_uniform("gap_size", 0.0f);
        }
        else
#endif // ENABLE_GL_CORE_PROFILE
            glsafe(::glLineWidth(2.0f));
        // stem
        shader->set_uniform("view_model_matrix", overlap ?
            ss_to_ndc_matrix * Geometry::translation_transform(v1ss_3) * q12ss * Geometry::translation_transform(-2.0 * TRIANGLE_HEIGHT * Vec3d::UnitX()) * Geometry::scale_transform({ v12ss_len + 4.0 * TRIANGLE_HEIGHT, 1.0f, 1.0f }) :
            ss_to_ndc_matrix * Geometry::translation_transform(v1ss_3) * q12ss * Geometry::scale_transform({ v12ss_len, 1.0f, 1.0f }));
        m_dimensioning.line.set_color(color);
        m_dimensioning.line.render();

#if ENABLE_GL_CORE_PROFILE
        if (OpenGLManager::get_gl_info().is_core_profile()) {
            shader->stop_using();

            shader = wxGetApp().get_shader("flat");
            if (shader == nullptr)
                return;

            shader->start_using();
        }
        else
#endif // ENABLE_GL_CORE_PROFILE
            glsafe(::glLineWidth(1.0f));

        // arrow 1
        if (show_first_tri) {
            shader->set_uniform("view_model_matrix", overlap ?
                ss_to_ndc_matrix * Geometry::translation_transform(v1ss_3) * q12ss :
                ss_to_ndc_matrix * Geometry::translation_transform(v1ss_3) * q21ss);
            m_dimensioning.triangle.render();
        }
        // arrow 2
        shader->set_uniform("view_model_matrix", overlap ?
            ss_to_ndc_matrix * Geometry::translation_transform(v2ss_3) * q21ss :
            ss_to_ndc_matrix * Geometry::translation_transform(v2ss_3) * q12ss);
        m_dimensioning.triangle.render();

        const bool use_inches = wxGetApp().app_config->get_bool("use_inches");
        const double curr_value = use_inches ? GizmoObjectManipulation::mm_to_in * distance : distance;
        const std::string curr_value_str = format_double(curr_value);
        const std::string units = use_inches ? _u8L("in") : _u8L("mm");
        const float value_str_width = 20.0f + ImGui::CalcTextSize(curr_value_str.c_str()).x;
        static double edit_value = 0.0;

        ImGuiWrapper::push_common_window_style(m_parent.get_scale());
        const Vec2d label_position = 0.5 * (v1ss + v2ss);
        m_imgui->set_next_window_pos(label_position.x(), viewport[3] - label_position.y(), ImGuiCond_Always, 0.0f, 1.0f);
        m_imgui->set_next_window_bg_alpha(0.0f);

        if (!m_editing_distance && show_label) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 1.0f, 1.0f });
            m_imgui->begin(std::string("distance"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration);
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            ImGui::AlignTextToFramePadding();
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const std::string txt = curr_value_str + " " + units;
            ImVec2 txt_size = ImGui::CalcTextSize(txt.c_str());
            const ImGuiStyle& style = ImGui::GetStyle();
            draw_list->AddRectFilled({pos.x - style.FramePadding.x, pos.y + style.FramePadding.y},
                                     {pos.x + txt_size.x + 2.0f * style.FramePadding.x, pos.y + txt_size.y + 2.0f * style.FramePadding.y},
                                     ImGuiWrapper::to_ImU32({1.0f, 1.0f, 1.0f, 0.5f}));
            ImGui::SetCursorScreenPos({ pos.x + style.FramePadding.x, pos.y });
            m_imgui->text(txt);
            if (m_hit_different_volumes.size() < 2 && wxGetApp().plater()->canvas3D()->get_canvas_type() == GLCanvas3D::ECanvasType::CanvasView3D) {
                ImGui::SameLine();
                if (m_imgui->image_button(ImGui::SliderFloatEditBtnIcon, _L("Edit to scale"))) {
                    m_editing_distance = true;
                    edit_value         = curr_value;
                    m_imgui->requires_extra_frame();
                }
            }
            m_imgui->end();
            ImGui::PopStyleVar(3);
        }

        if (m_editing_distance && !ImGui::IsPopupOpen("distance_popup"))
            ImGui::OpenPopup("distance_popup");

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 1.0f, 1.0f });
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 4.0f, 0.0f });
        if (show_label && ImGui::BeginPopupModal("distance_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration)) {
            auto perform_scale = [this](double new_value, double old_value,bool scale_single_volume) {
                if (new_value == old_value || new_value <= 0.0)
                    return;

                const double ratio = new_value / old_value;
                wxGetApp().plater()->take_snapshot(_u8L("Scale"), UndoRedo::SnapshotType::GizmoAction);
                // apply scale
                TransformationType type;
                type.set_world();
                type.set_relative();
                type.set_joint();

                // scale selection
                Selection& selection = m_parent.get_selection();
                selection.setup_cache();
                if (scale_single_volume && m_hit_different_volumes.size()==1) {
                    //todo//update_single_mesh_pick(m_hit_different_volumes[0])
                } else {
                    selection.scale(ratio * Vec3d::Ones(), type);
                    wxGetApp().plater()->canvas3D()->do_scale(""); // avoid storing another snapshot
                    register_single_mesh_pick();
                }
                wxGetApp().obj_manipul()->set_dirty();
                // scale dimensioning
                update_feature_by_tran(*m_selected_features.first.feature);
                if (m_selected_features.second.feature.has_value())
                    update_feature_by_tran(*m_selected_features.second.feature);
                // update measure on next call to data_changed()
                m_pending_scale = 1;

            };
            auto action_exit = [this]() {
                m_editing_distance = false;
                m_is_editing_distance_first_frame = true;
                ImGui::CloseCurrentPopup();
            };
            auto action_scale = [perform_scale, action_exit](double new_value, double old_value, bool scale_single_volume) {
                perform_scale(new_value, old_value, scale_single_volume);
                action_exit();
            };

            m_imgui->disable_background_fadeout_animation();
            ImGui::PushItemWidth(value_str_width);
            if (ImGui::InputDouble("##distance", &edit_value, 0.0f, 0.0f, "%.3f")) {
            }

            // trick to auto-select text in the input widgets on 1st frame
            if (m_is_editing_distance_first_frame) {
                ImGui::SetKeyboardFocusHere(0);
                m_is_editing_distance_first_frame = false;
                m_imgui->set_requires_extra_frame();
            }

            // handle keys input
            if (ImGui::IsKeyPressedMap(ImGuiKey_Enter) || ImGui::IsKeyPressedMap(ImGuiKey_KeyPadEnter))
                action_scale(edit_value, curr_value, false);
            else if (ImGui::IsKeyPressedMap(ImGuiKey_Escape))
                action_exit();

            ImGui::SameLine();
            ImGuiWrapper::push_confirm_button_style();
            //if (m_imgui->button(_CTX(L_CONTEXT("Scale part", "Verb"), "Verb")))
                //action_scale(edit_value, curr_value, true);
            //ImGui::SameLine();
            if (m_imgui->button(_CTX(L_CONTEXT("Scale all", "Verb"), "Verb")))
                action_scale(edit_value, curr_value, false);
            ImGuiWrapper::pop_confirm_button_style();
            ImGui::SameLine();
            ImGuiWrapper::push_cancel_button_style();
            if (m_imgui->button(_L("Cancel")))
                action_exit();
            ImGuiWrapper::pop_cancel_button_style();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(4);
        ImGuiWrapper::pop_common_window_style();
    };

    auto point_edge = [this, shader](const Measure::SurfaceFeature& f1, const Measure::SurfaceFeature& f2) {
        assert(f1.get_type() == Measure::SurfaceFeatureType::Point && f2.get_type() == Measure::SurfaceFeatureType::Edge);
        std::pair<Vec3d, Vec3d> e = f2.get_edge();
        const Vec3d v_proj = m_measurement_result.distance_infinite->to;
        const Vec3d e1e2 = e.second - e.first;
        const Vec3d v_proje1 = v_proj - e.first;
        const bool on_e1_side = v_proje1.dot(e1e2) < -EPSILON;
        const bool on_e2_side = !on_e1_side && v_proje1.norm() > e1e2.norm();
        if (on_e1_side || on_e2_side) {
            const Camera& camera = wxGetApp().plater()->get_camera();
            const Matrix4d projection_view_matrix = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
            const std::array<int, 4>& viewport = camera.get_viewport();
            const Transform3d ss_to_ndc_matrix = TransformHelper::ndc_to_ss_matrix_inverse(viewport);

            const Vec2d v_projss = TransformHelper::world_to_ss(v_proj, projection_view_matrix, viewport);
            auto render_extension = [this, &v_projss, &projection_view_matrix, &viewport, &ss_to_ndc_matrix, shader](const Vec3d& p) {
                const Vec2d pss = TransformHelper::world_to_ss(p, projection_view_matrix, viewport);
                if (!pss.isApprox(v_projss)) {
                    const Vec2d pv_projss = v_projss - pss;
                    const double pv_projss_len = pv_projss.norm();

                    const auto q = Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitX(), Vec3d(pv_projss.x(), pv_projss.y(), 0.0));

                    shader->set_uniform("projection_matrix", Transform3d::Identity());
                    shader->set_uniform("view_model_matrix", ss_to_ndc_matrix * Geometry::translation_transform({ pss.x(), pss.y(), 0.0 }) * q *
                        Geometry::scale_transform({ pv_projss_len, 1.0f, 1.0f }));
                    m_dimensioning.line.set_color(ColorRGBA::LIGHT_GRAY());
                    m_dimensioning.line.render();
                }
            };

            render_extension(on_e1_side ? e.first : e.second);
        }
    };

    auto arc_edge_edge = [this, &shader](const Measure::SurfaceFeature& f1, const Measure::SurfaceFeature& f2, double radius = 0.0) {
        assert(f1.get_type() == Measure::SurfaceFeatureType::Edge && f2.get_type() == Measure::SurfaceFeatureType::Edge);
        if (!m_measurement_result.angle.has_value())
            return;

        const double angle = m_measurement_result.angle->angle;
        const Vec3d  center = m_measurement_result.angle->center;
        const std::pair<Vec3d, Vec3d> e1 = m_measurement_result.angle->e1;
        const std::pair<Vec3d, Vec3d> e2 = m_measurement_result.angle->e2;
        const double calc_radius = m_measurement_result.angle->radius;
        const bool   coplanar = m_measurement_result.angle->coplanar;

        if (std::abs(angle) < EPSILON || std::abs(calc_radius) < EPSILON)
            return;

        const double draw_radius = (radius > 0.0) ? radius : calc_radius;

        const Vec3d e1_unit = Measure::edge_direction(e1);
        const Vec3d e2_unit = Measure::edge_direction(e2);

        const unsigned int resolution = std::max<unsigned int>(2, 64 * angle / double(PI));
        const double step = angle / double(resolution);
        const Vec3d normal = e1_unit.cross(e2_unit).normalized();

        if (!m_dimensioning.arc.is_initialized()) {
            GLModel::Geometry init_data;
            init_data.format = { GLModel::Geometry::EPrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P3 };
            init_data.color  = ColorRGBA::WHITE();
            init_data.reserve_vertices(resolution + 1);
            init_data.reserve_indices(resolution + 1);

            // vertices + indices
            for (unsigned int i = 0; i <= resolution; ++i) {
                const double a = step * double(i);
                const Vec3d  v = draw_radius * (Eigen::Quaternion<double>(Eigen::AngleAxisd(a, normal)) * e1_unit);
                init_data.add_vertex((Vec3f) v.cast<float>());
                init_data.add_index(i);
            }

            m_dimensioning.arc.init_from(std::move(init_data));
        }

        const Camera& camera = wxGetApp().plater()->get_camera();
#if ENABLE_GL_CORE_PROFILE
        if (OpenGLManager::get_gl_info().is_core_profile()) {
            shader->stop_using();

            shader = wxGetApp().get_shader("dashed_thick_lines");
            if (shader == nullptr)
                return;

            shader->start_using();
            shader->set_uniform("projection_matrix", Transform3d::Identity());
            const std::array<int, 4>& viewport = camera.get_viewport();
            shader->set_uniform("viewport_size", Vec2d(double(viewport[2]), double(viewport[3])));
            shader->set_uniform("width", 1.0f);
            shader->set_uniform("gap_size", 0.0f);
        }
        else
#endif // ENABLE_GL_CORE_PROFILE
          glsafe(::glLineWidth(2.0f));

        // arc
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("view_model_matrix", camera.get_view_matrix() * Geometry::translation_transform(center));
        m_dimensioning.arc.render();

#if ENABLE_GL_CORE_PROFILE
        if (OpenGLManager::get_gl_info().is_core_profile()) {
            shader->stop_using();

            shader = wxGetApp().get_shader("flat");
            if (shader == nullptr)
                return;

            shader->start_using();
        }
        else
#endif // ENABLE_GL_CORE_PROFILE
          glsafe(::glLineWidth(1.0f));

        // arrows
        auto render_arrow = [this, shader, &camera, &normal, &center, &e1_unit, draw_radius, step, resolution](unsigned int endpoint_id) {
            const double angle = (endpoint_id == 1) ? 0.0 : step * double(resolution);
            const Vec3d position_model = Geometry::translation_transform(center) * (draw_radius * (Eigen::Quaternion<double>(Eigen::AngleAxisd(angle, normal)) * e1_unit));
            const Vec3d direction_model = (endpoint_id == 1) ? -normal.cross(position_model - center).normalized() : normal.cross(position_model - center).normalized();
            const auto qz = Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitZ(), (endpoint_id == 1) ? normal : -normal);
            const auto qx = Eigen::Quaternion<double>::FromTwoVectors(qz * Vec3d::UnitX(), direction_model);
            const Transform3d view_model_matrix = camera.get_view_matrix() * Geometry::translation_transform(position_model) *
                qx * qz * Geometry::scale_transform(camera.get_inv_zoom());
            shader->set_uniform("view_model_matrix", view_model_matrix);
            m_dimensioning.triangle.render();
        };

        glsafe(::glDisable(GL_CULL_FACE));
        render_arrow(1);
        render_arrow(2);
        glsafe(::glEnable(GL_CULL_FACE));

        // edge 1 extension
        const Vec3d e11e12 = e1.second - e1.first;
        const Vec3d e11center = center - e1.first;
        const double e11center_len = e11center.norm();

        if (e11center_len > EPSILON && e11center.dot(e11e12) < 0.0) {
            shader->set_uniform("view_model_matrix", camera.get_view_matrix() * Geometry::translation_transform(center) *
                Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitX(), Measure::edge_direction(e1.first, e1.second)) *
                Geometry::scale_transform({ e11center_len, 1.0f, 1.0f }));
            m_dimensioning.line.set_color(ColorRGBA::LIGHT_GRAY());
            m_dimensioning.line.render();
        }

        // edge 2 extension
        const Vec3d e21center = center - e2.first;
        const double e21center_len = e21center.norm();
        if (e21center_len > EPSILON) {
            shader->set_uniform("view_model_matrix", camera.get_view_matrix() * Geometry::translation_transform(center) *
                Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitX(), Measure::edge_direction(e2.first, e2.second)) *
                Geometry::scale_transform({ (coplanar && radius > 0.0) ? e21center_len : draw_radius, 1.0f, 1.0f }));
            m_dimensioning.line.set_color(ColorRGBA::LIGHT_GRAY());
            m_dimensioning.line.render();
        }

        // label
        // label world coordinates
        const Vec3d label_position_world = Geometry::translation_transform(center) * (draw_radius * (Eigen::Quaternion<double>(Eigen::AngleAxisd(step * 0.5 * double(resolution), normal)) * e1_unit));

        // label screen coordinates
        const std::array<int, 4>& viewport = camera.get_viewport();
        const Vec2d label_position_ss = TransformHelper::world_to_ss(label_position_world,
            camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix(), viewport);

        ImGuiWrapper::push_common_window_style(m_parent.get_scale());
        m_imgui->set_next_window_pos(label_position_ss.x(), viewport[3] - label_position_ss.y(), ImGuiCond_Always, 0.0f, 1.0f);
        m_imgui->set_next_window_bg_alpha(0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        m_imgui->begin(wxString("##angle"), ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImGui::AlignTextToFramePadding();
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const std::string txt = format_double(Geometry::rad2deg(angle)) + "°";
        ImVec2 txt_size = ImGui::CalcTextSize(txt.c_str());
        const ImGuiStyle& style = ImGui::GetStyle();
        ColorRGBA         color{1.0f, 1.0f, 1.0f, 0.5f};
        draw_list->AddRectFilled({ pos.x - style.FramePadding.x, pos.y + style.FramePadding.y }, { pos.x + txt_size.x + 2.0f * style.FramePadding.x,
            pos.y + txt_size.y + 2.0f * style.FramePadding.y}, ImGuiWrapper::to_ImU32(color));
        ImGui::SetCursorScreenPos({ pos.x + style.FramePadding.x, pos.y });
        m_imgui->text(txt);
        m_imgui->end();
        ImGui::PopStyleVar();
        ImGuiWrapper::pop_common_window_style();
    };

    auto arc_edge_plane = [this, arc_edge_edge](const Measure::SurfaceFeature& f1, const Measure::SurfaceFeature& f2) {
        assert(f1.get_type() == Measure::SurfaceFeatureType::Edge && f2.get_type() == Measure::SurfaceFeatureType::Plane);
        if (!m_measurement_result.angle.has_value())
            return;

        const std::pair<Vec3d, Vec3d> e1 = m_measurement_result.angle->e1;
        const std::pair<Vec3d, Vec3d> e2 = m_measurement_result.angle->e2;
        const double calc_radius = m_measurement_result.angle->radius;

        if (calc_radius == 0.0)
            return;

        arc_edge_edge(Measure::SurfaceFeature(Measure::SurfaceFeatureType::Edge, e1.first, e1.second),
            Measure::SurfaceFeature(Measure::SurfaceFeatureType::Edge, e2.first, e2.second), calc_radius);
    };

    auto arc_plane_plane = [this, arc_edge_edge](const Measure::SurfaceFeature& f1, const Measure::SurfaceFeature& f2) {
        assert(f1.get_type() == Measure::SurfaceFeatureType::Plane && f2.get_type() == Measure::SurfaceFeatureType::Plane);
        if (!m_measurement_result.angle.has_value())
            return;

        const std::pair<Vec3d, Vec3d> e1 = m_measurement_result.angle->e1;
        const std::pair<Vec3d, Vec3d> e2 = m_measurement_result.angle->e2;
        const double calc_radius = m_measurement_result.angle->radius;

        if (calc_radius == 0.0)
            return;

        arc_edge_edge(Measure::SurfaceFeature(Measure::SurfaceFeatureType::Edge, e1.first, e1.second),
            Measure::SurfaceFeature(Measure::SurfaceFeatureType::Edge, e2.first, e2.second), calc_radius);
    };

    shader->start_using();

    if (!m_dimensioning.line.is_initialized()) {
        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        init_data.color  = ColorRGBA::WHITE();
        init_data.reserve_vertices(2);
        init_data.reserve_indices(2);

        // vertices
        init_data.add_vertex(Vec3f(0.0f, 0.0f, 0.0f));
        init_data.add_vertex(Vec3f(1.0f, 0.0f, 0.0f));

        // indices
        init_data.add_line(0, 1);

        m_dimensioning.line.init_from(std::move(init_data));
    }

    if (!m_dimensioning.triangle.is_initialized()) {
        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        init_data.color  = ColorRGBA::WHITE();
        init_data.reserve_vertices(3);
        init_data.reserve_indices(3);

        // vertices
        init_data.add_vertex(Vec3f(0.0f, 0.0f, 0.0f));
        init_data.add_vertex(Vec3f(-TRIANGLE_HEIGHT, 0.5f * TRIANGLE_BASE, 0.0f));
        init_data.add_vertex(Vec3f(-TRIANGLE_HEIGHT, -0.5f * TRIANGLE_BASE, 0.0f));

        // indices
        init_data.add_triangle(0, 1, 2);

        m_dimensioning.triangle.init_from(std::move(init_data));
    }

    if (last_selected_features != m_selected_features)
        m_dimensioning.arc.reset();

    glsafe(::glDisable(GL_DEPTH_TEST));

    const bool has_distance = m_measurement_result.has_distance_data();

    const Measure::SurfaceFeature* f1 = &(*m_selected_features.first.feature);
    const Measure::SurfaceFeature* f2 = nullptr;
    std::unique_ptr<Measure::SurfaceFeature> temp_feature;
    if (m_selected_features.second.feature.has_value())
        f2 = &(*m_selected_features.second.feature);
    else {
        assert(m_selected_features.first.feature->get_type() == Measure::SurfaceFeatureType::Circle);
        temp_feature = std::make_unique<Measure::SurfaceFeature>(std::get<0>(m_selected_features.first.feature->get_circle()));
        f2 = temp_feature.get();
    }

    if (!m_selected_features.second.feature.has_value() && m_selected_features.first.feature->get_type() != Measure::SurfaceFeatureType::Circle)
        return;

    Measure::SurfaceFeatureType ft1 = f1->get_type();
    Measure::SurfaceFeatureType ft2 = f2->get_type();

    // Order features by type so following conditions are simple.
    if (ft1 > ft2) {
        std::swap(ft1, ft2);
        std::swap(f1, f2);
    }

    // If there is an angle to show, draw the arc:
    if (ft1 == Measure::SurfaceFeatureType::Edge && ft2 == Measure::SurfaceFeatureType::Edge)
        arc_edge_edge(*f1, *f2);
    else if (ft1 == Measure::SurfaceFeatureType::Edge && ft2 == Measure::SurfaceFeatureType::Plane)
        arc_edge_plane(*f1, *f2);
    else if (ft1 == Measure::SurfaceFeatureType::Plane && ft2 == Measure::SurfaceFeatureType::Plane)
        arc_plane_plane(*f1, *f2);

    if (has_distance){
        // Where needed, draw the extension of the edge to where the dist is measured:
        if (ft1 == Measure::SurfaceFeatureType::Point && ft2 == Measure::SurfaceFeatureType::Edge)
            point_edge(*f1, *f2);

        // Render the arrow between the points that the backend passed:
        const Measure::DistAndPoints& dap = m_measurement_result.distance_infinite.has_value()
            ? *m_measurement_result.distance_infinite
            : *m_measurement_result.distance_strict;
        if (m_selected_features.second.feature.has_value() &&
            !(m_measure_mode == EMeasureMode::ONLY_ASSEMBLY && m_assembly_mode == AssemblyMode::FACE_FACE)) {
            auto x_to = dap.from;
            x_to[0]   = dap.to[0];
            point_point(dap.from, x_to, x_to[0] - dap.from[0], ColorRGBA::RED(), false, false);
            auto y_to = x_to;
            y_to[1]   = dap.to[1];
            point_point(x_to, y_to, y_to[1] - x_to[1], ColorRGBA::GREEN(), false, false);
            point_point(y_to, dap.to, dap.to[2] - y_to[2], ColorRGBA::BLUE(), false, false);
        }

        point_point(dap.from, dap.to, dap.dist, ColorRGBA::WHITE());
    }

    glsafe(::glEnable(GL_DEPTH_TEST));

    shader->stop_using();
}

static void add_row_to_table(std::function<void(void)> col_1 = nullptr, std::function<void(void)> col_2 = nullptr)
{
    assert(col_1 != nullptr && col_2 != nullptr);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    col_1();
    ImGui::TableSetColumnIndex(1);
    col_2();
}

static void add_strings_row_to_table(ImGuiWrapper& imgui, const std::string& col_1, const ImVec4& col_1_color, const std::string& col_2, const ImVec4& col_2_color)
{
    add_row_to_table([&]() { imgui.text_colored(col_1_color, col_1); }, [&]() { imgui.text_colored(col_2_color, col_2); });
};

#if ENABLE_MEASURE_GIZMO_DEBUG
void GLGizmoMeasure::render_debug_dialog()
{
    auto add_feature_data = [this](const SelectedFeatures::Item& item) {
        const std::string text = (item.source == item.feature) ? surface_feature_type_as_string(item.feature->get_type()) : point_on_feature_type_as_string(item.source->get_type(), m_hover_id);
        add_strings_row_to_table(*m_imgui, "Type", ImGuiWrapper::COL_ORCA, text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
        switch (item.feature->get_type())
        {
        case Measure::SurfaceFeatureType::Point:
        {
            add_strings_row_to_table(*m_imgui, "m_pt1", ImGuiWrapper::COL_ORCA, format_vec3(item.feature->get_point()), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            break;
        }
        case Measure::SurfaceFeatureType::Edge:
        {
            auto [from, to] = item.feature->get_edge();
            add_strings_row_to_table(*m_imgui, "m_pt1", ImGuiWrapper::COL_ORCA, format_vec3(from), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            add_strings_row_to_table(*m_imgui, "m_pt2", ImGuiWrapper::COL_ORCA, format_vec3(to), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            break;
        }
        case Measure::SurfaceFeatureType::Plane:
        {
            auto [idx, normal, origin] = item.feature->get_plane();
            add_strings_row_to_table(*m_imgui, "m_pt1", ImGuiWrapper::COL_ORCA, format_vec3(normal), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            add_strings_row_to_table(*m_imgui, "m_pt2", ImGuiWrapper::COL_ORCA, format_vec3(origin), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            add_strings_row_to_table(*m_imgui, "m_value", ImGuiWrapper::COL_ORCA, format_double(idx), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            break;
        }
        case Measure::SurfaceFeatureType::Circle:
        {
            auto [center, radius, normal] = item.feature->get_circle();
            const Vec3d on_circle = center + radius * Measure::get_orthogonal(normal, true);
            radius = (on_circle - center).norm();
            add_strings_row_to_table(*m_imgui, "m_pt1", ImGuiWrapper::COL_ORCA, format_vec3(center), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            add_strings_row_to_table(*m_imgui, "m_pt2", ImGuiWrapper::COL_ORCA, format_vec3(normal), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            add_strings_row_to_table(*m_imgui, "m_value", ImGuiWrapper::COL_ORCA, format_double(radius), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            break;
        }
        }
        std::optional<Vec3d> extra_point = item.feature->get_extra_point();
        if (extra_point.has_value())
            add_strings_row_to_table(*m_imgui, "m_pt3", ImGuiWrapper::COL_ORCA, format_vec3(*extra_point), ImGui::GetStyleColorVec4(ImGuiCol_Text));
    };

    m_imgui->begin("Measure tool debug", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    if (ImGui::BeginTable("Mode", 2)) {
        std::string txt;
        switch (m_mode)
        {
        case EMode::FeatureSelection: { txt = "Feature selection"; break; }
        case EMode::PointSelection:   { txt = "Point selection"; break; }
        default:                      { assert(false); break; }
        }
        add_strings_row_to_table(*m_imgui, "Mode", ImGuiWrapper::COL_ORCA, txt, ImGui::GetStyleColorVec4(ImGuiCol_Text));
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (ImGui::BeginTable("Hover", 2)) {
        add_strings_row_to_table(*m_imgui, "Hover id", ImGuiWrapper::COL_ORCA, std::to_string(m_hover_id), ImGui::GetStyleColorVec4(ImGuiCol_Text));
        const std::string txt = m_curr_feature.has_value() ? surface_feature_type_as_string(m_curr_feature->get_type()) : "None";
        add_strings_row_to_table(*m_imgui, "Current feature", ImGuiWrapper::COL_ORCA, txt, ImGui::GetStyleColorVec4(ImGuiCol_Text));
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (!m_selected_features.first.feature.has_value() && !m_selected_features.second.feature.has_value())
        m_imgui->text("Empty selection");
    else {
        const ImGuiTableFlags flags = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersH;
        if (m_selected_features.first.feature.has_value()) {
            m_imgui->text_colored(ImGuiWrapper::COL_ORCA, "Selection 1");
            if (ImGui::BeginTable("Selection 1", 2, flags)) {
                add_feature_data(m_selected_features.first);
                ImGui::EndTable();
            }
        }
        if (m_selected_features.second.feature.has_value()) {
            m_imgui->text_colored(ImGuiWrapper::COL_ORCA, "Selection 2");
            if (ImGui::BeginTable("Selection 2", 2, flags)) {
                add_feature_data(m_selected_features.second);
                ImGui::EndTable();
            }
        }
    }
    m_imgui->end();
}
#endif // ENABLE_MEASURE_GIZMO_DEBUG

void GLGizmoMeasure::show_selection_ui()
{
    auto space_size = m_space_size;
    // Show selection
    {
        auto format_item_text = [this](const SelectedFeatures::Item &item) {
            if (!item.feature.has_value()) return _u8L("None");

            std::string text = (item.source == item.feature) ? surface_feature_type_as_string(item.feature->get_type()) :
                               item.is_center                ? center_on_feature_type_as_string(item.source->get_type()) :
                                                               point_on_feature_type_as_string(item.source->get_type(), m_hover_id);
            if (item.feature.has_value() && item.feature->get_type() == Measure::SurfaceFeatureType::Circle) {
                auto [center, radius, normal] = item.feature->get_circle();
                const Vec3d on_circle         = center + radius * Measure::get_orthogonal(normal, true);
                radius                        = (on_circle - center).norm();
                if (m_use_inches)
                    radius = GizmoObjectManipulation::mm_to_in * radius;
                text += " (" + _u8L("Diameter") + ": " + format_double(2.0 * radius) + m_units + ")";
            } else if (item.feature.has_value() && item.feature->get_type() == Measure::SurfaceFeatureType::Edge) {
                auto [start, end] = item.feature->get_edge();
                double length     = (end - start).norm();
                if (m_use_inches)
                    length = GizmoObjectManipulation::mm_to_in * length;
                text += " (" + _u8L("Length") + ": " + format_double(length) + m_units + ")";
            }
            return text;
        };

        float selection_cap_length = 0;
        if (m_measure_mode == EMeasureMode::ONLY_ASSEMBLY) {
            // Ultra: one width for every assembly mode, measured off the widest label ("Triangle").
            selection_cap_length = ImGui::CalcTextSize((_u8L("Triangle") + " 1" + _u8L(" (Moving)")).c_str()).x * 1.2;
        }
        else {
            selection_cap_length = ImGui::CalcTextSize((_u8L("Selection") + " 1").c_str()).x * 1.2;
        }
        auto        feature_first_text        = format_item_text(m_selected_features.first);
        const float feature_first_text_length = ImGui::CalcTextSize((_u8L(feature_first_text)).c_str()).x;
        ImGui::AlignTextToFramePadding();
        if (m_measure_mode == EMeasureMode::ONLY_ASSEMBLY) {
            // Ultra (Stage 2): detect circles too in Face/Face mode (not planes-only) so curved rims are
            // pickable for mating; the hover filter above keeps only Plane/Circle features.
            m_only_select_plane = false;
            if (m_assembly_mode == AssemblyMode::FACE_FACE) {
                m_imgui->text(_u8L("Select 2 faces on objects and \n make objects assemble together.")); // tip
            } else if (m_assembly_mode == AssemblyMode::POINT_POINT) {
                m_imgui->text(_u8L("Select 2 points or circles on objects and \n specify distance between them.")); // tip
            } else if (m_assembly_mode == AssemblyMode::TRIANGLE_TRIANGLE) {
                m_imgui->text(_u8L("Select 1 mesh triangle on each object and \n make them assemble together.")); // tip
            } else if (m_assembly_mode == AssemblyMode::CURVE_CURVE) {
                m_imgui->text(_u8L("Select 1 curved surface patch on each object and \n make them assemble together.")); // tip
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::to_ImVec4(SELECTED_1ST_COLOR));
        if (m_measure_mode == EMeasureMode::ONLY_ASSEMBLY) {
            if (m_assembly_mode == AssemblyMode::FACE_FACE) {
                m_imgui->text(_u8L("Face") + " 1" + _u8L(" (Fixed)"));
            } else if (m_assembly_mode == AssemblyMode::POINT_POINT) {
                m_imgui->text(_u8L("Point") + " 1" + _u8L(" (Fixed)"));
            } else if (m_assembly_mode == AssemblyMode::TRIANGLE_TRIANGLE) {
                m_imgui->text(_u8L("Triangle") + " 1" + _u8L(" (Fixed)"));
            } else if (m_assembly_mode == AssemblyMode::CURVE_CURVE) {
                m_imgui->text(_u8L("Curve") + " 1" + _u8L(" (Fixed)"));
            }
        }
        else {
            m_imgui->text(_u8L("Selection") + " 1");
        }
        ImGui::SameLine(selection_cap_length + space_size);
        ImGui::PushItemWidth(feature_first_text_length);
        m_imgui->text(feature_first_text);
        if (m_selected_features.first.feature.has_value()) {
            ImGui::SameLine(selection_cap_length + feature_first_text_length + space_size * 2);
            ImGui::PushItemWidth(space_size * 2);
            ImGui::PushID("Reset1"); // for image_button
            if (m_imgui->image_button(m_is_dark_mode ? ImGui::RevertBtn : ImGui::RevertBtn, _L("Reset"))) { reset_feature1(); }
            ImGui::PopID();
        }
        ImGui::PopStyleColor();

        auto        feature_second_text        = format_item_text(m_selected_features.second);
        const float feature_second_text_length = ImGui::CalcTextSize((_u8L(feature_second_text)).c_str()).x;
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::to_ImVec4(SELECTED_2ND_COLOR));
        if (m_measure_mode == EMeasureMode::ONLY_ASSEMBLY) {
            if (m_assembly_mode == AssemblyMode::FACE_FACE) {
                m_imgui->text(_u8L("Face") + " 2"+ _u8L(" (Moving)"));
            } else if (m_assembly_mode == AssemblyMode::POINT_POINT) {
                m_imgui->text(_u8L("Point") + " 2"+ _u8L(" (Moving)"));
            } else if (m_assembly_mode == AssemblyMode::TRIANGLE_TRIANGLE) {
                m_imgui->text(_u8L("Triangle") + " 2"+ _u8L(" (Moving)"));
            } else if (m_assembly_mode == AssemblyMode::CURVE_CURVE) {
                m_imgui->text(_u8L("Curve") + " 2"+ _u8L(" (Moving)"));
            }
        } else {
            m_imgui->text(_u8L("Selection") + " 2");
        }
        ImGui::SameLine(selection_cap_length + space_size);
        ImGui::PushItemWidth(feature_second_text_length);
        m_imgui->text(feature_second_text);
        if (m_selected_features.first.feature.has_value() && m_selected_features.second.feature.has_value()) {
            m_show_reset_first_tip = false;
            ImGui::SameLine(selection_cap_length + feature_second_text_length + space_size * 2);
            ImGui::PushItemWidth(space_size * 2);
            ImGui::PushID("Reset2");
            if (m_imgui->image_button(m_is_dark_mode ? ImGui::RevertBtn : ImGui::RevertBtn, _L("Reset"))) { reset_feature2(); }
            ImGui::PopID();
        }
        ImGui::PopStyleColor();
    }

    /*m_imgui->disabled_begin(!m_selected_features.first.feature.has_value());
    if (m_imgui->button(_L("Restart selection"))) {
        reset_all_feature();
        m_imgui->set_requires_extra_frame();
    }
    m_imgui->disabled_end();*/

    if (m_show_reset_first_tip) {
        m_imgui->text(_L("Feature 1 has been reset, \nfeature 2 has been feature 1"));
    }
    if (m_selected_wrong_feature_waring_tip) {
        if (m_measure_mode == EMeasureMode::ONLY_ASSEMBLY) {
            if (m_assembly_mode == AssemblyMode::FACE_FACE) {
                m_imgui->warning_text(_L("Warning: please select a Plane or Circle feature."));
            } else if (m_assembly_mode == AssemblyMode::POINT_POINT) {
                m_imgui->warning_text(_L("Warning: please select Point's or Circle's feature."));
            } else if (m_assembly_mode == AssemblyMode::TRIANGLE_TRIANGLE) {
                m_imgui->warning_text(_L("Warning: please select a mesh triangle."));
            } else if (m_assembly_mode == AssemblyMode::CURVE_CURVE) {
                m_imgui->warning_text(_L("Warning: please select a curved surface patch."));
            }
        }
    }
    if (m_measure_mode == EMeasureMode::ONLY_ASSEMBLY && m_hit_different_volumes.size() == 1) {
        m_imgui->warning_text(_L("Warning: please select two different meshes."));
    }
}

void GLGizmoMeasure::show_distance_xyz_ui()
{
    if (m_measure_mode == EMeasureMode::ONLY_MEASURE) {
        m_imgui->text(_u8L("Measure"));
    }
    auto add_measure_row_to_table = [this](const std::string &col_1, const ImVec4 &col_1_color, const std::string &col_2, const ImVec4 &col_2_color) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        m_imgui->text_colored(col_1_color, col_1);
        ImGui::TableSetColumnIndex(1);
        m_imgui->text_colored(col_2_color, col_2);
        ImGui::TableSetColumnIndex(2);
        if (m_imgui->image_button(m_is_dark_mode ? ImGui::ClipboardBtnDarkIcon : ImGui::ClipboardBtnIcon, _L("Copy to clipboard"))) {
            wxTheClipboard->Open();
            wxTheClipboard->SetData(new wxTextDataObject(wxString((col_1 + ": " + col_2).c_str(), wxConvUTF8)));
            wxTheClipboard->Close();
        }
    };
    auto add_edit_distance_xyz_box = [this](Vec3d &distance) {
        //m_imgui->disabled_begin(m_hit_different_volumes.size() == 1);
        //{
            if (m_measure_mode == EMeasureMode::ONLY_MEASURE) {
                m_can_set_xyz_distance = false;
            }
            bool volume = m_hit_different_volumes.size() == 1;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            m_imgui->text_colored(ImGuiWrapper::to_ImVec4(ColorRGBA::X()), "X:"); // ORCA match axis color
            ImGui::TableSetColumnIndex(1);
            ImGui::PushItemWidth(m_input_size_max);
            m_imgui->disabled_begin(volume || !m_can_set_xyz_distance); // ORCA disable only input box othervise axis colors rendered dimmed
            ImGui::BBLInputDouble("##measure_distance_x", &m_buffered_distance[0], 0.0f, 0.0f, "%.2f");
            m_imgui->disabled_end();

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            m_imgui->text_colored(ImGuiWrapper::to_ImVec4(ColorRGBA::Y()), "Y:"); // ORCA match axis color
            ImGui::TableSetColumnIndex(1);
            m_imgui->disabled_begin(volume || !m_can_set_xyz_distance); // ORCA disable only input box othervise axis colors rendered dimmed
            ImGui::BBLInputDouble("##measure_distance_y", &m_buffered_distance[1], 0.0f, 0.0f, "%.2f");
            m_imgui->disabled_end();

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            m_imgui->text_colored(ImGuiWrapper::to_ImVec4(ColorRGBA::Z()), "Z:"); // ORCA match axis color
            ImGui::TableSetColumnIndex(1);
            m_imgui->disabled_begin(volume || !(m_same_model_object && m_can_set_xyz_distance)); // ORCA disable only input box othervise axis colors rendered dimmed
            ImGui::BBLInputDouble("##measure_distance_z", &m_buffered_distance[2], 0.0f, 0.0f, "%.2f");
            m_imgui->disabled_end();
        //}
        //m_imgui->disabled_end();
        if (m_last_active_item_imgui != m_current_active_imgui_id && m_hit_different_volumes.size() == 2) {
            Vec3d displacement = Vec3d::Zero();
            if (std::abs(m_buffered_distance[0] - distance[0]) > EPSILON) {
                displacement[0] = m_buffered_distance[0] - distance[0];
                distance[0]     = m_buffered_distance[0];
            } else if (std::abs(m_buffered_distance[1] - distance[1]) > EPSILON) {
                displacement[1] = m_buffered_distance[1] - distance[1];
                distance[1]     = m_buffered_distance[1];
            } else if (std::abs(m_buffered_distance[2] - distance[2]) > EPSILON) {
                displacement[2] = m_buffered_distance[2] - distance[2];
                distance[2]     = m_buffered_distance[2];
            }
            if (displacement.norm() > 0.0f) { set_distance(m_same_model_object, displacement); }
        }
    };
    const unsigned int max_measure_row_count = 2;
    unsigned int       measure_row_count     = 0;
    if (ImGui::BeginTable("Measure", 4)) {
        if (m_selected_features.second.feature.has_value()) {
            const Measure::MeasurementResult &measure = m_measurement_result;
            if (measure.angle.has_value() && m_measure_mode == EMeasureMode::ONLY_MEASURE)
                {
                ImGui::PushID("ClipboardAngle");
                add_measure_row_to_table(_u8L("Angle"), ImGuiWrapper::COL_ORCA, format_double(Geometry::rad2deg(measure.angle->angle)) + "°",
                                            ImGui::GetStyleColorVec4(ImGuiCol_Text));
                ++measure_row_count;
                ImGui::PopID();
            }

            const bool show_strict = measure.distance_strict.has_value() &&
                                        (!measure.distance_infinite.has_value() || std::abs(measure.distance_strict->dist - measure.distance_infinite->dist) > EPSILON);

            if (measure.distance_infinite.has_value() && m_measure_mode == EMeasureMode::ONLY_MEASURE) {
                double distance = measure.distance_infinite->dist;
                if (m_use_inches) distance = GizmoObjectManipulation::mm_to_in * distance;
                ImGui::PushID("ClipboardDistanceInfinite");
                add_measure_row_to_table(show_strict ? _u8L("Perpendicular distance") : _u8L("Distance"), ImGuiWrapper::COL_ORCA, format_double(distance) + m_units,
                                            ImGui::GetStyleColorVec4(ImGuiCol_Text));
                ++measure_row_count;
                ImGui::PopID();
            }
            if (show_strict &&
                (m_measure_mode == EMeasureMode::ONLY_MEASURE ||
                    (m_measure_mode == EMeasureMode::ONLY_ASSEMBLY && m_assembly_mode == AssemblyMode::POINT_POINT)))
                {
                double distance = measure.distance_strict->dist;
                if (m_use_inches)
                    distance = GizmoObjectManipulation::mm_to_in * distance;
                ImGui::PushID("ClipboardDistanceStrict");
                add_measure_row_to_table(_u8L("Direct distance"), ImGuiWrapper::COL_ORCA, format_double(distance) + m_units, ImGui::GetStyleColorVec4(ImGuiCol_Text));
                ++measure_row_count;
                ImGui::PopID();
            }
            if (measure.distance_xyz.has_value() && m_measure_mode == EMeasureMode::ONLY_MEASURE) {
                Vec3d distance = *measure.distance_xyz;
                if (m_use_inches) distance = GizmoObjectManipulation::mm_to_in * distance;
                if (measure.distance_xyz->norm() > EPSILON) {
                    ImGui::PushID("ClipboardDistanceXYZ");
                    add_measure_row_to_table(_u8L("Distance XYZ"), ImGuiWrapper::COL_ORCA, format_vec3(distance), ImGui::GetStyleColorVec4(ImGuiCol_Text));
                    ++measure_row_count;
                    ImGui::PopID();
                }
            }

            if (m_distance.norm() > 0.01) {
                if (!(m_measure_mode == EMeasureMode::ONLY_ASSEMBLY && m_assembly_mode == AssemblyMode::FACE_FACE)) {
                    add_edit_distance_xyz_box(m_distance);
                }
            }
        }
        // add dummy rows to keep dialog size fixed
        /*for (unsigned int i = measure_row_count; i < max_measure_row_count; ++i) {
            add_strings_row_to_table(*m_imgui, " ", ImGuiWrapper::COL_ORCA, " ", ImGui::GetStyleColorVec4(ImGuiCol_Text));
        }*/
        ImGui::EndTable();
    }
}

//void GLGizmoMeasure::show_point_point_assembly()
//{
//}

void GLGizmoMeasure::show_face_face_assembly_common() {
    // Ultra: accept Plane OR Circle picks (Stage 2 lets peg/hole rims mate, not just flat faces).
    auto feat_ok = [](const SelectedFeatures::Item& it) {
        return it.feature.has_value() &&
               (it.feature->is_planar() || it.feature->get_type() == Measure::SurfaceFeatureType::Circle);
    };
    if (m_measure_mode == EMeasureMode::ONLY_ASSEMBLY && m_hit_different_volumes.size() == 2 &&
        feat_ok(m_selected_features.first) && feat_ok(m_selected_features.second)) {
        auto &action                         = m_assembly_action;
        auto  set_to_parallel_size           = m_imgui->calc_button_size(_L("Parallel")).x;
        auto  set_to_center_coincidence_size = m_imgui->calc_button_size(_L("Center coincidence")).x;

        m_imgui->disabled_begin(!(action.can_set_to_center_coincidence));
        {
            ImGui::PushItemWidth(set_to_center_coincidence_size);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0 / 255.0, 150 / 255.0, 136 / 255.0, 1.0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(38 / 255.0f, 166 / 255.0f, 154 / 255.0f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0 / 255.0f, 137 / 255.0f, 123 / 255.0f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(254 / 255.0f, 254 / 255.0f, 254 / 255.0f, 1.00f));
            if (m_imgui->button(_L("Center coincidence"))) {
                set_to_center_coincidence(m_same_model_object);
            }
            ImGui::PopStyleColor(4);
            ImGui::SameLine(set_to_center_coincidence_size + m_space_size * 2);
        }
        m_imgui->disabled_end();

        m_imgui->disabled_begin(!action.can_set_to_parallel);
        {
            if (m_imgui->button(_L("Parallel"))) { set_to_parallel(m_same_model_object); }
        }
        m_imgui->disabled_end();
    }
}

void GLGizmoMeasure::show_face_face_assembly_senior()
{
    if (m_measure_mode == EMeasureMode::ONLY_ASSEMBLY && m_hit_different_volumes.size() == 2 &&
        m_selected_features.first.feature.has_value() && m_selected_features.second.feature.has_value() &&
        m_selected_features.first.feature->is_planar() && m_selected_features.second.feature->is_planar()) {
        auto &action                         = m_assembly_action;
        auto  feature_text_size              = m_imgui->calc_button_size(_L("Feature 1")).x + m_imgui->calc_button_size(":").x;
        auto  set_to_reverse_rotation_size   = m_imgui->calc_button_size(_L("Reverse rotation")).x;
        auto  rotate_around_center_size      = m_imgui->calc_button_size(_L("Rotate around center:")).x;
        auto  parallel_distance_size         = m_imgui->calc_button_size(_L("Parallel distance:")).x;

        if (m_imgui->bbl_checkbox(_L("Flip by Face 2"), m_flip_volume_2)) {
            set_to_reverse_rotation(m_same_model_object, 1);
        }

        // Ultra: the "Parallel distance" / "Rotate around center" text boxes were replaced by the live
        // Adjust sliders (ultra_show_adjust_ui), which work in every assembly mode.
        (void) action; (void) parallel_distance_size; (void) rotate_around_center_size;
    }
}

void GLGizmoMeasure::init_render_input_window()
{
    m_use_inches        = wxGetApp().app_config->get_bool("use_inches");
    m_units             = m_use_inches ? " " + _u8L("in") : " " + _u8L("mm");
    m_space_size        = ImGui::CalcTextSize("  ").x * 2;
    m_input_size_max    = ImGui::CalcTextSize("-100.00").x * 1.2;
    m_same_model_object = is_two_volume_in_same_model_object();
}

void GLGizmoMeasure::on_render_input_window(float x, float y, float bottom_limit)
{
    static std::optional<Measure::SurfaceFeature> last_feature;
    static EMode last_mode = EMode::FeatureSelection;
    static SelectedFeatures last_selected_features;

    static float last_y = 0.0f;
    static float last_h = 0.0f;

    if (m_editing_distance)
        return;
    m_current_active_imgui_id = ImGui::GetActiveID();
    // adjust window position to avoid overlap the view toolbar
    const float win_h = ImGui::GetWindowHeight();
    dock_setup_next_window(x, y, bottom_limit, 0.f, /*size_to_content=*/true);
    if (last_h != win_h || last_y != y) {
        // ask canvas for another frame to render the window in the correct position
        m_imgui->set_requires_extra_frame();
        if (last_h != win_h)
            last_h = win_h;
        if (last_y != y)
            last_y = y;
    }
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), dock_window_flags(ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar));
    if (!dock_render_titlebar(get_name())) {
        m_last_active_item_imgui = m_current_active_imgui_id;
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }

    init_render_input_window();
    show_selection_ui();

    ImGui::Separator();
    show_distance_xyz_ui();
    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    float caption_max    = 0.f;
    float total_text_max = 0.f;
    for (const auto &t : std::array<std::string, 3>{"point_selection", "reset", "unselect"}) {
        caption_max    = std::max(caption_max, m_imgui->calc_text_size(m_desc[t + "_caption"]).x);
        total_text_max = std::max(total_text_max, m_imgui->calc_text_size(m_desc[t]).x);
    }
    show_tooltip_information(caption_max, x, get_cur_y);
    ImGui::PopStyleVar(1);
    if (last_feature != m_curr_feature || last_mode != m_mode || last_selected_features != m_selected_features) {
        // the dialog may have changed its size, ask for an extra frame to render it properly
        last_feature = m_curr_feature;
        last_mode = m_mode;
        last_selected_features = m_selected_features;
        m_imgui->set_requires_extra_frame();
    }
    m_last_active_item_imgui = m_current_active_imgui_id;
    GizmoImguiEnd();
    // Orca
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoMeasure::render_input_window_warning(bool same_model_object)
{
}

void GLGizmoMeasure::remove_selected_sphere_raycaster(int id)
{
    reset_gripper_pick(id == SEL_SPHERE_1_ID ? GripperType::SPHERE_1 : GripperType::SPHERE_2);
}

void GLGizmoMeasure::update_measurement_result()
{
    if (!m_selected_features.first.feature.has_value()) {
        m_measurement_result = Measure::MeasurementResult();
        m_assembly_action    = Measure::AssemblyAction();
    }
    else if (m_selected_features.second.feature.has_value()) {
        m_measurement_result = Measure::get_measurement(*m_selected_features.first.feature, *m_selected_features.second.feature, true);
        m_assembly_action    = Measure::get_assembly_action(*m_selected_features.first.feature, *m_selected_features.second.feature);
        m_can_set_xyz_distance = Measure::can_set_xyz_distance(*m_selected_features.first.feature, *m_selected_features.second.feature);
        //update buffer
        const Measure::MeasurementResult &measure = m_measurement_result;
        m_distance                                = Vec3d::Zero();
        if (measure.distance_xyz.has_value() && measure.distance_xyz->norm() > EPSILON) {
            m_distance = *measure.distance_xyz;
        } else if (measure.distance_infinite.has_value()) {
            m_distance = measure.distance_infinite->to - measure.distance_infinite->from;
        } else if (measure.distance_strict.has_value()) {
            m_distance = measure.distance_strict->to - measure.distance_strict->from;
        }
        if (wxGetApp().app_config->get_bool("use_inches")) m_distance = GizmoObjectManipulation::mm_to_in * m_distance;
        m_buffered_distance = m_distance;
        if (m_assembly_action.has_parallel_distance) {
            m_buffered_parallel_distance = m_assembly_action.parallel_distance;
        }
    }
    else if (!m_selected_features.second.feature.has_value() && m_selected_features.first.feature->get_type() == Measure::SurfaceFeatureType::Circle)
        m_measurement_result = Measure::get_measurement(*m_selected_features.first.feature, Measure::SurfaceFeature(std::get<0>(m_selected_features.first.feature->get_circle())));
}

void GLGizmoMeasure::show_tooltip_information(float caption_max, float x, float y)
{
    ImTextureID normal_id = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += m_imgui->calc_text_size(": "sv).x + 35.f;

    float  scale       = m_parent.get_scale();
    ImVec2 button_size = ImVec2(25 * scale, 25 * scale); // ORCA: Use exact resolution will prevent blur on icon
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 0, 0 }); // ORCA: Dont add padding
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &caption_max](const wxString &caption, const wxString &text) {
            m_imgui->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            m_imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        for (const auto &t : std::array<std::string, 3>{"point_selection", "reset", "unselect"})
            draw_text_with_caption(m_desc.at(t + "_caption") + ": ", m_desc.at(t));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

void GLGizmoMeasure::reset_all_pick()
{
   std::map<GLVolume*, std::shared_ptr<PickRaycaster>>().swap(m_mesh_raycaster_map);
   reset_gripper_pick(GripperType::UNDEFINE,true);
}

void GLGizmoMeasure::reset_gripper_pick(GripperType id, bool is_all)
{
    if (m_gripper_id_raycast_map.find(id) != m_gripper_id_raycast_map.end()) {
        m_gripper_id_raycast_map[id].reset();
        m_gripper_id_raycast_map.erase(id);
    }
    if (id == GripperType ::UNDEFINE && is_all) {
        reset_gripper_pick(GripperType::POINT);
        reset_gripper_pick(GripperType::EDGE);
        reset_gripper_pick(GripperType::PLANE);
        reset_gripper_pick(GripperType::PLANE_1);
        reset_gripper_pick(GripperType::PLANE_2);
        reset_gripper_pick(GripperType::CIRCLE);
        reset_gripper_pick(GripperType::CIRCLE_1);
        reset_gripper_pick(GripperType::CIRCLE_2);
    }
}

void GLGizmoMeasure::register_single_mesh_pick()
{
    Selection&  selection = m_parent.get_selection();
    const Selection::IndicesList &idxs  = selection.get_volume_idxs();
    if (idxs.size() > 0) {
        for (unsigned int idx : idxs) {
            GLVolume *v                = const_cast<GLVolume *>(selection.get_volume(idx));
            auto world_tran            = v->get_instance_transformation() * v->get_volume_transformation();
            if (m_mesh_raycaster_map.find(v) != m_mesh_raycaster_map.end()) {
                m_mesh_raycaster_map[v]->set_transform(world_tran.get_matrix());
            } else {
                const ModelObject*   obj  = selection.get_model()->objects[v->object_idx()];
                const ModelVolume*   vol  = obj->volumes[v->volume_idx()];
                auto                 mesh = vol->mesh_ptr();
                m_mesh_raycaster_map[v] = std::make_shared<PickRaycaster>(-1, *v->mesh_raycaster, world_tran.get_matrix());
                m_mesh_raycaster_map[v]->set_transform(world_tran.get_matrix());
                m_mesh_measure_map[v] = std::make_shared<Measure::Measuring>(mesh->its);
            }
        }
    }
}

//void GLGizmoMeasure::update_single_mesh_pick(GLVolume *v)
//{
//    if (m_mesh_raycaster_map.find(v) != m_mesh_raycaster_map.end()) {
//        auto world_tran = v->get_instance_transformation() * v->get_volume_transformation();
//        m_mesh_raycaster_map[v]->world_tran.set_from_transform(world_tran.get_matrix());
//    }
//}

 void GLGizmoMeasure::reset_all_feature() {
     reset_feature2();
     reset_feature1();
     m_show_reset_first_tip = false;
     m_hit_different_volumes.clear();
     m_hit_order_volumes.clear();
     m_last_hit_volume = nullptr;
     m_ultra_adjust_rot = 0.f; m_ultra_adjust_off = 0.f; // Ultra: sliders start fresh with a new pick
 }

void GLGizmoMeasure::reset_feature1_render()
{
    if (m_feature_plane_first.plane.model.is_initialized()) {
        m_feature_plane_first.plane.model.reset();
        m_feature_plane_first.plane_idx = -1;
    }
    if (m_feature_circle_first.circle.model.is_initialized()) {
        m_feature_circle_first.circle.model.reset();
        m_feature_circle_first.last_circle_feature = nullptr;
        m_feature_circle_first.inv_zoom            = 0;
    }
}

void GLGizmoMeasure::reset_feature2_render()
{
    if (m_feature_plane_second.plane.model.is_initialized()) {
        m_feature_plane_second.plane.model.reset();
        m_feature_plane_second.plane_idx = -1;
    }
    if (m_feature_circle_second.circle.model.is_initialized()) {
        m_feature_circle_second.circle.model.reset();
        m_feature_circle_second.last_circle_feature = nullptr;
        m_feature_circle_second.inv_zoom            = 0;
    }
}

void GLGizmoMeasure::reset_feature1()
{
     m_selected_wrong_feature_waring_tip = false;
     reset_feature1_render();
     if (m_selected_features.second.feature.has_value()) {
         if (m_hit_different_volumes.size() == 2) {
             m_hit_different_volumes[0] = m_hit_different_volumes[1];
         }
         if (m_hit_order_volumes.size() == 2) {
             m_hit_order_volumes[0] = m_hit_order_volumes[1];
         }
         m_selected_features.first = m_selected_features.second;
         reset_feature2();
         m_show_reset_first_tip = true;
     } else {
         remove_selected_sphere_raycaster(SEL_SPHERE_1_ID);
         m_selected_features.first.feature.reset();
         m_show_reset_first_tip = false;
         if (m_hit_different_volumes.size() == 1) {
             m_hit_different_volumes.clear();
         }
         if (m_hit_order_volumes.size() == 1) {
             m_hit_order_volumes.clear();
         }
         reset_gripper_pick(GripperType::PLANE_1);
         reset_gripper_pick(GripperType::CIRCLE_1);
         reset_gripper_pick(GripperType::SPHERE_1);
     }
     update_measurement_result();
 }

void GLGizmoMeasure::reset_feature2()
{
     reset_feature2_render();
     if (m_hit_different_volumes.size() == 2) {
         m_hit_different_volumes.erase(m_hit_different_volumes.begin() + 1);
     }
     if (m_hit_order_volumes.size() == 2) {
         m_hit_order_volumes.erase(m_hit_order_volumes.begin() + 1);
     }
     remove_selected_sphere_raycaster(SEL_SPHERE_2_ID);
     m_selected_features.second.reset();
     m_ultra_exact_pick = UltraExactAnchor(); // Ultra (Exact highlight)
     m_ultra_exact_pick_key.clear();
     m_show_reset_first_tip = false;
     m_selected_wrong_feature_waring_tip = false;
     reset_gripper_pick(GripperType::PLANE_2);
     reset_gripper_pick(GripperType::CIRCLE_2);
     reset_gripper_pick(GripperType::SPHERE_2);

     update_measurement_result();
}

bool Slic3r::GUI::GLGizmoMeasure::is_two_volume_in_same_model_object()
{
    if (m_hit_different_volumes.size() == 2) {
        if (m_hit_different_volumes[0]->composite_id.object_id == m_hit_different_volumes[1]->composite_id.object_id) {
            return true;
        }
    }
    return false;
}

Measure::Measuring *GLGizmoMeasure::get_measuring_of_mesh(GLVolume *v, Transform3d &tran)
{
    for (auto glvolume:m_hit_order_volumes) {
        if (glvolume == v) {
            tran = m_mesh_raycaster_map[glvolume]->get_transform();
            return m_mesh_measure_map[glvolume].get();
        }
    }
    return nullptr;
}

void GLGizmoMeasure::update_world_plane_features(Measure::Measuring *cur_measuring, Measure::SurfaceFeature& feautre)
{
    if (cur_measuring) {
        const auto &[idx, normal, pt] = feautre.get_plane();
        feautre.plane_indices       = const_cast<std::vector<int> *>(&cur_measuring->get_plane_triangle_indices(idx));
        auto cur_plane_features       = const_cast<std::vector<Measure::SurfaceFeature> *>(&cur_measuring->get_plane_features(idx));
        if (cur_plane_features) {
            if (!feautre.world_plane_features) {
                feautre.world_plane_features = std::make_shared<std::vector<Measure::SurfaceFeature>>();
            }
            feautre.world_plane_features->clear(); // resize(cur_plane_features->size());
            for (size_t i = 0; i < cur_plane_features->size(); i++) {
                Measure::SurfaceFeature temp(cur_plane_features->at(i));
                temp.translate(feautre.world_tran);
                feautre.world_plane_features->push_back(std::move(temp));
            }
        }
    }
}

void GLGizmoMeasure::update_feature_by_tran(Measure::SurfaceFeature &feature)
{
    if (!feature.volume) { return; }
    auto  volume  = static_cast<GLVolume *>(feature.volume);
    Measure::Measuring *cur_measuring = get_measuring_of_mesh(volume, feature.world_tran);
    switch (feature.get_type()) {
    case Measure::SurfaceFeatureType::Point:
    case Measure::SurfaceFeatureType::Edge:
    case Measure::SurfaceFeatureType::Circle:
    case Measure::SurfaceFeatureType::Triangle: // Ultra: re-sync facet/patch picks after a mate too
    case Measure::SurfaceFeatureType::Curve:
    case Measure::SurfaceFeatureType::Plane: {
        feature.clone(*feature.origin_surface_feature);
        feature.translate(feature.world_tran);
        if (feature.get_type() == Measure::SurfaceFeatureType::Circle) {
            m_feature_circle_first.last_circle_feature  = nullptr;
            m_feature_circle_first.inv_zoom             = 0;
            m_feature_circle_second.last_circle_feature = nullptr;
            m_feature_circle_second.inv_zoom            = 0;
        }
        if (feature.get_type() == Measure::SurfaceFeatureType::Plane) {
            if (cur_measuring) {
                update_world_plane_features(cur_measuring, feature);
            }
        }
        break;
    }
    default: {
        break;
    }
    }
}

void GLGizmoMeasure::set_distance(bool same_model_object, const Vec3d &displacement, bool take_shot)
{
    if (m_hit_different_volumes.size() == 2 && displacement.norm() > 0.0f) {
        auto v         = m_hit_different_volumes[1];
        auto selection = const_cast<Selection *>(&m_parent.get_selection());
        selection->setup_cache();
        if (take_shot) {
            wxGetApp().plater()->take_snapshot("MoveInMeasure", UndoRedo::SnapshotType::GizmoAction); // avoid storing another snapshot
        }
        selection->set_mode(same_model_object ? Selection::Volume : Selection::Instance);
        m_pending_scale ++;
        if (same_model_object == false) {
            Vec3d object_displacement = v->get_instance_transformation().get_matrix_no_offset().inverse() * displacement;
            v->set_instance_transformation(v->get_instance_transformation().get_matrix() * Geometry::translation_transform(object_displacement));
        } else {
            Geometry::Transformation tran(v->world_matrix());
            Vec3d                     local_displacement = tran.get_matrix_no_offset().inverse() * displacement;
            v->set_volume_transformation(v->get_volume_transformation().get_matrix() * Geometry::translation_transform(local_displacement));
        }
        wxGetApp().plater()->canvas3D()->do_move("");
        register_single_mesh_pick();
        if (same_model_object ) {
            update_feature_by_tran(*m_selected_features.first.feature);
        }
        update_feature_by_tran(*m_selected_features.second.feature);
    }
}

// Ultra (Stage 2: curved mates): return a mating DIRECTION + POINT for a feature. A Plane gives (normal,
// centroid); a Circle gives (axis, center). This lets the plane-based mate ops (parallel / center-coincidence)
// also mate cylindrical/circular features (pegs, pins, shafts, bosses) that have no single flat face.
static bool ultra_feature_dir_point(const Measure::SurfaceFeature& f, Vec3d& dir, Vec3d& pt)
{
    switch (f.get_type()) {
    case Measure::SurfaceFeatureType::Plane:  { const auto [i, n, p]  = f.get_plane();  dir = n;  pt = p; return true; }
    case Measure::SurfaceFeatureType::Circle: { const auto [c, r, ax] = f.get_circle(); dir = ax; pt = c; return true; }
    case Measure::SurfaceFeatureType::Triangle:
    case Measure::SurfaceFeatureType::Curve:  { const auto [n, p]     = f.get_patch();  dir = n;  pt = p; return true; }
    default: return false;
    }
}

void GLGizmoMeasure::set_to_parallel(bool same_model_object, bool take_shot, bool is_anti_parallel)
{
    if (m_hit_different_volumes.size() == 2) {
        auto &action    = m_assembly_action;
        auto  v         = m_hit_different_volumes[1];
        auto  selection = const_cast<Selection *>(&m_parent.get_selection());
        selection->setup_cache();
        if (take_shot) {
            wxGetApp().plater()->take_snapshot("RotateInMeasure", UndoRedo::SnapshotType::GizmoAction); // avoid storing another snapshot
        }
        selection->set_mode(same_model_object ? Selection::Volume : Selection::Instance);
        Vec3d normal1, pt1, normal2, pt2;
        if (!ultra_feature_dir_point(*m_selected_features.first.feature, normal1, pt1) ||
            !ultra_feature_dir_point(*m_selected_features.second.feature, normal2, pt2))
            return;
        if ((is_anti_parallel && normal1.dot(normal2) > -1 + 1e-3) ||
            (is_anti_parallel == false && (normal1.dot(normal2) < 1 - 1e-3))) {
            m_pending_scale ++;
            Vec3d    axis;
            double   angle;
            Matrix3d rotation_matrix;
            Geometry::rotation_from_two_vectors(normal2, -normal1, axis, angle, &rotation_matrix);
            Transform3d r_m = (Transform3d) rotation_matrix;
            if (same_model_object == false) {
                auto new_rotation_tran = r_m * v->get_instance_transformation().get_rotation_matrix();
                Vec3d rotation         = Geometry::extract_euler_angles(new_rotation_tran);
                v->set_instance_rotation(rotation);
                selection->rotate(v->object_idx(), v->instance_idx(), v->get_instance_transformation().get_matrix());
            } else {
                Geometry::Transformation world_tran(v->world_matrix());
                auto        new_tran         = r_m * world_tran.get_rotation_matrix();
                Transform3d volume_rotation_tran = v->get_instance_transformation().get_rotation_matrix().inverse() * new_tran;
                Vec3d       rotation             = Geometry::extract_euler_angles(volume_rotation_tran);
                v->set_volume_rotation(rotation);
                selection->rotate(v->object_idx(), v->instance_idx(), v->volume_idx(), v->get_volume_transformation().get_matrix());
            }
            wxGetApp().plater()->canvas3D()->do_rotate("");
            register_single_mesh_pick();
            if (same_model_object) {
                update_feature_by_tran(*m_selected_features.first.feature);
            }
            update_feature_by_tran(*m_selected_features.second.feature);
        }
    }
}

void GLGizmoMeasure::set_to_reverse_rotation(bool same_model_object, int feature_index)
{
    if (m_hit_different_volumes.size() == 2 && feature_index < 2) {
        auto &action    = m_assembly_action;
        auto  v         = m_hit_different_volumes[feature_index];
        auto  selection = const_cast<Selection *>(&m_parent.get_selection());
        selection->setup_cache();
        wxGetApp().plater()->take_snapshot("ReverseRotateInMeasure", UndoRedo::SnapshotType::GizmoAction); // avoid storing another snapshot

        selection->set_mode(same_model_object ? Selection::Volume : Selection::Instance);
        m_pending_scale                 = 1;
        Vec3d plane_normal,plane_center;
        if (feature_index ==0) {//feature 1
            const auto [idx1, normal1, pt1] = m_selected_features.first.feature->get_plane();
            plane_normal                    = normal1;
            plane_center                    = pt1;
        }
        else  { // feature 2
            const auto [idx2, normal2, pt2] = m_selected_features.second.feature->get_plane();
            plane_normal                    = normal2;
            plane_center                    = pt2;
        }
        auto  new_pt = Measure::get_one_point_in_plane(plane_center, plane_normal);
        Vec3d axis   = (new_pt - plane_center).normalized();
        if (axis.norm() < 0.1) {
            throw;
        }
        if (same_model_object == false) {
            Geometry::Transformation inMat(v->get_instance_transformation());
            Geometry::Transformation outMat = Geometry::mat_around_a_point_rotate(inMat, plane_center, axis, PI);
            selection->rotate(v->object_idx(), v->instance_idx(), outMat.get_matrix());
        } else {
            Geometry::Transformation inMat(v->world_matrix());
            Geometry::Transformation outMat      = Geometry::mat_around_a_point_rotate(inMat, plane_center, axis, PI);
            Transform3d volume_tran = v->get_instance_transformation().get_matrix().inverse() * outMat.get_matrix();
            selection->rotate(v->object_idx(), v->instance_idx(), v->volume_idx(), volume_tran);
        }
        wxGetApp().plater()->canvas3D()->do_rotate("");
        register_single_mesh_pick();
        if (same_model_object == false) {
            if (feature_index == 0) { // feature 1
                update_feature_by_tran(*m_selected_features.first.feature);
            } else { // feature 2
                update_feature_by_tran(*m_selected_features.second.feature);
            }
        }
        else {
            update_feature_by_tran(*m_selected_features.first.feature);
            update_feature_by_tran(*m_selected_features.second.feature);
        }
    }
}

void GLGizmoMeasure::set_to_around_center_of_faces(bool same_model_object, float rotate_degree, bool take_shot)
{
    if (m_hit_different_volumes.size() == 2 ) {
        auto &action    = m_assembly_action;
        auto  v         = m_hit_different_volumes[1];
        auto  selection = const_cast<Selection *>(&m_parent.get_selection());
        selection->setup_cache();
        if (take_shot)
            wxGetApp().plater()->take_snapshot("ReverseRotateInMeasure", UndoRedo::SnapshotType::GizmoAction); // avoid storing another snapshot

        selection->set_mode(same_model_object ? Selection::Volume : Selection::Instance);
        m_pending_scale = 1;

        auto  radian = Geometry::deg2rad(rotate_degree);
        Vec3d plane_normal, plane_center;
        // Ultra: any feature with a direction + point (Plane / Circle / Triangle / Curve), not only Plane.
        if (!ultra_feature_dir_point(*m_selected_features.second.feature, plane_normal, plane_center)) return;

        if (same_model_object == false) {
            Geometry::Transformation inMat(v->get_instance_transformation());
            Geometry::Transformation outMat = Geometry::mat_around_a_point_rotate(inMat, plane_center, plane_normal, radian);
            selection->rotate(v->object_idx(), v->instance_idx(), outMat.get_matrix());
        } else {
            Geometry::Transformation inMat(v->world_matrix());
            Geometry::Transformation outMat      = Geometry::mat_around_a_point_rotate(inMat, plane_center, plane_normal, radian);
            Transform3d              volume_tran = v->get_instance_transformation().get_matrix().inverse() * outMat.get_matrix();
            selection->rotate(v->object_idx(), v->instance_idx(), v->volume_idx(), volume_tran);
        }
        wxGetApp().plater()->canvas3D()->do_rotate("");
        register_single_mesh_pick();
        if (same_model_object) {
            update_feature_by_tran(*m_selected_features.first.feature);
        }
        update_feature_by_tran(*m_selected_features.second.feature);
    }
}

void GLGizmoMeasure::set_to_center_coincidence(bool same_model_object) {
    auto v = m_hit_different_volumes[1];
    wxGetApp().plater()->take_snapshot("RotateThenMoveInMeasure", UndoRedo::SnapshotType::GizmoAction);
    set_to_parallel(same_model_object, false,true);

    Vec3d normal1, pt1, normal2, pt2;
    if (!ultra_feature_dir_point(*m_selected_features.first.feature, normal1, pt1) ||
        !ultra_feature_dir_point(*m_selected_features.second.feature, normal2, pt2))
        return;
    set_distance(same_model_object, pt1 - pt2, false);
    m_set_center_coincidence = true;
}

// In-plane reference direction (world, in the feature's world_tran frame) from a picked Plane's
// LONGEST BOUNDARY EDGE. Aligning these across the two faces pins the free roll DOF that a normal-only
// mate leaves open (e.g. a square peg landing diagonal in a square slot). Returns false for non-planes.
bool GLGizmoMeasure::ultra_plane_axis_world(GLVolume* v, const Measure::SurfaceFeature& f, Vec3d& axis_world, double& aspect,
                                            std::vector<Vec3d>* boundary_world)
{
    if (!v || !f.is_planar() || !f.plane_indices) return false; // Plane, Triangle or Curve (all carry a facet list)
    auto it = m_mesh_measure_map.find(v);
    if (it == m_mesh_measure_map.end() || !it->second) return false;
    const indexed_triangle_set& its = it->second->get_its();
    const Transform3d& W  = f.world_tran;
    // Feature coords are already WORLD (baked via world_tran), so the normal is used as-is; only the
    // raw mesh vertices below need W applied.
    const Vec3d n  = f.get_pt1(); // normal -- Plane, Triangle and Curve share the (normal, point) layout
    const Vec3d nw = n.normalized();
    const Vec3d bu = nw.unitOrthogonal();
    const Vec3d bw = nw.cross(bu).normalized();
    // Boundary edges = edges used by exactly one of the plane's triangles.
    std::map<std::pair<int,int>, int> ecount;
    auto ekey = [](int a, int b){ return a < b ? std::make_pair(a,b) : std::make_pair(b,a); };
    for (int t : *f.plane_indices) {
        if (t < 0 || t >= (int) its.indices.size()) continue;
        const auto& tri = its.indices[t];
        ecount[ekey(tri[0], tri[1])]++; ecount[ekey(tri[1], tri[2])]++; ecount[ekey(tri[2], tri[0])]++;
    }
    // Project boundary vertices into the plane. A single "longest edge" is unreliable on filleted /
    // tessellated boundaries, so instead fit the MIN-AREA oriented bounding rectangle (its optimum is
    // always aligned with some boundary edge, so boundary edge angles are the candidate set).
    std::vector<Eigen::Vector2d> pts; std::vector<double> cand; std::map<int, Eigen::Vector2d> proj;
    std::vector<Vec3d> wpts; // boundary vertices in world, for the caller's containment test
    auto P2 = [&](int vi) -> Eigen::Vector2d {
        auto pit = proj.find(vi);
        if (pit != proj.end()) return pit->second;
        const Vec3d Pw = W * its.vertices[vi].cast<double>();
        Eigen::Vector2d q(Pw.dot(bu), Pw.dot(bw));
        proj.emplace(vi, q); pts.push_back(q); wpts.push_back(Pw); return q;
    };
    for (const auto& kv : ecount) {
        if (kv.second != 1) continue;
        const Eigen::Vector2d e = P2(kv.first.second) - P2(kv.first.first);
        if (e.norm() > 1e-6) cand.push_back(std::atan2(e.y(), e.x()));
    }
    if (pts.size() < 3 || cand.empty()) return false;
    const double inf = std::numeric_limits<double>::infinity();
    double best_area = inf, best_ang = 0.0, best_asp = 1.0; bool long_is_x = true;
    for (double ang : cand) {
        const double c = std::cos(ang), s = std::sin(ang);
        double minx = inf, maxx = -inf, miny = inf, maxy = -inf;
        for (const auto& q : pts) {
            const double x = q.x() * c + q.y() * s, y = -q.x() * s + q.y() * c;
            minx = std::min(minx, x); maxx = std::max(maxx, x); miny = std::min(miny, y); maxy = std::max(maxy, y);
        }
        const double dx = maxx - minx, dy = maxy - miny, area = dx * dy;
        if (area < best_area) {
            best_area = area; best_ang = ang; long_is_x = dx >= dy;
            best_asp = std::max(dx, dy) / std::max(std::min(dx, dy), 1e-9);
        }
    }
    const double a = long_is_x ? best_ang : best_ang + M_PI / 2; // direction of the rectangle's LONG side
    axis_world = (bu * std::cos(a) + bw * std::sin(a)).normalized();
    aspect = best_asp;
    if (boundary_world) *boundary_world = wpts;
    return true;
}

// Ultra guided Auto-Fit: take the two picked features (target = volume[0], attachment = volume[1]),
// mate them on the PRINT transform (converting each feature from its bake-time view world into its
// object's print world, so it is correct even when picking happened in the exploded assembly view),
// then merge the two objects into one multi-part object that reassembles for printing.
void GLGizmoMeasure::ultra_fit_for_print_and_merge()
{
    auto notify = [](const std::string& t) {
        wxGetApp().plater()->get_notification_manager()->push_notification(
            NotificationType::CustomNotification, NotificationManager::NotificationLevel::RegularNotificationLevel, t);
    };
    if (m_hit_different_volumes.size() != 2) return;
    if (!m_selected_features.first.feature.has_value() || !m_selected_features.second.feature.has_value()) return;
    GLVolume* vt = m_hit_different_volumes[0]; // target -- stays fixed
    GLVolume* va = m_hit_different_volumes[1]; // attachment -- moves onto target
    if (!vt || !va) return;
    // Ultra (Exact highlight): land the clicked footprint on the target; falls through when it cannot.
    if (ultra_exact_fit()) return;
    if (vt->object_idx() == va->object_idx()) {
        // Two PARTS of one object: no merge needed (already one printable object). Mate via the proven
        // same-object path (it moves the ModelVolume transform slicing uses), then pin the free spin about
        // the mating normal by containment, applied through the stock rotate-about-face-centre action so
        // refresh + undo stay standard. The roll is computed from the PRE-mate features by predicting the
        // post-mate outline (same object == same frame), so it does not depend on feature refresh timing.
        if (vt->volume_idx() == va->volume_idx()) {
            notify(_u8L("Fit for Print: pick two different parts."));
            return;
        }
        double roll_deg = 0.0; bool rolled = false;
        if (m_selected_features.first.feature->is_planar() && m_selected_features.second.feature->is_planar()) {
            Vec3d n1, c1, n2, c2, ax; double asp; std::vector<Vec3d> o1, o2;
            if (ultra_feature_dir_point(*m_selected_features.first.feature,  n1, c1) &&
                ultra_feature_dir_point(*m_selected_features.second.feature, n2, c2) &&
                ultra_plane_axis_world(vt, *m_selected_features.first.feature,  ax, asp, &o1) &&
                ultra_plane_axis_world(va, *m_selected_features.second.feature, ax, asp, &o2) &&
                o1.size() >= 3 && o2.size() >= 3) {
                Vec3d axis; double phi; Matrix3d R;
                Geometry::rotation_from_two_vectors(n2, -n1, axis, phi, &R);
                std::vector<Vec3d> o2m; o2m.reserve(o2.size());
                for (const auto& P : o2) o2m.push_back(c1 + R * (P - c2)); // face 2 outline after the normal mate
                const UltraFit::RollResult rr = UltraFit::roll_by_containment(n1, c1, o1, o2m);
                if (rr.ok && std::abs(rr.roll) > 1e-6) { roll_deg = rr.roll * 180.0 / M_PI; rolled = true; }
                BOOST_LOG_TRIVIAL(warning) << "[UltraFit] parts roll by containment: " << (rr.roll * 180.0 / M_PI)
                                           << " deg, residual=" << rr.protrusion << " clearance=" << rr.clearance;
            }
        }
        set_to_center_coincidence(true);
        // set_to_around_center_of_faces spins volume[1] about FACE 2's normal, which is -n1 after the
        // anti-parallel mate, so negate to express the roll about n1.
        if (rolled) set_to_around_center_of_faces(true, (float) (-roll_deg));
        // Keep the picks alive (the Adjust sliders need them); only the slider accumulators restart.
        m_ultra_adjust_rot = 0.f; m_ultra_adjust_off = 0.f;
        notify(rolled ? (boost::format(_u8L("Fit for Print: parts mated, roll %.2f deg.")) % roll_deg).str()
                      : _u8L("Fit for Print: parts mated for assembly."));
        return;
    }

    Vec3d d1, p1, d2, p2;
    if (!ultra_feature_dir_point(*m_selected_features.first.feature,  d1, p1)) return;
    if (!ultra_feature_dir_point(*m_selected_features.second.feature, d2, p2)) return;
    // Face boundary outlines (world) for roll-by-containment; absent for circular features (symmetric).
    Vec3d a1 = Vec3d::Zero(), a2 = Vec3d::Zero(); double asp1 = 1.0, asp2 = 1.0;
    std::vector<Vec3d> b1, b2;
    const bool have_b1 = ultra_plane_axis_world(vt, *m_selected_features.first.feature,  a1, asp1, &b1);
    const bool have_b2 = ultra_plane_axis_world(va, *m_selected_features.second.feature, a2, asp2, &b2);

    Model& model = wxGetApp().plater()->model();
    // world(bake frame) -> PRINT world for a picked volume, via instance[0] (which ObjectList::merge
    // bakes). world_tran is the exact transform the feature coords were baked with, so the volume +
    // picked-instance view transform cancel.
    auto w2p_of = [&model](GLVolume* v, const Measure::SurfaceFeature& f, Transform3d& out) -> bool {
        int oi = v->object_idx(), vi = v->volume_idx();
        if (oi < 0 || oi >= (int) model.objects.size()) return false;
        ModelObject* mo = model.objects[oi];
        if (mo->instances.empty() || vi < 0 || vi >= (int) mo->volumes.size()) return false;
        const Transform3d printW = mo->instances[0]->get_transformation().get_matrix()
                                 * mo->volumes[vi]->get_transformation().get_matrix();
        out = printW * f.world_tran.inverse();
        return true;
    };
    Transform3d W1, W2;
    if (!w2p_of(vt, *m_selected_features.first.feature,  W1)) return;
    if (!w2p_of(va, *m_selected_features.second.feature, W2)) return;
    p1 = W1 * p1; d1 = (W1.linear() * d1).normalized(); for (auto& P : b1) P = W1 * P;
    p2 = W2 * p2; d2 = (W2.linear() * d2).normalized(); for (auto& P : b2) P = W2 * P;

    // Ultra (Curve mode): a Curve pick is not necessarily a cylinder. Fit both analytic shapes and let the
    // residual decide. When BOTH picks come back as spheres the mate is centre-to-centre -- two hemispheres
    // nest by making their centres coincide, and a normal-based mate would instead butt them nose to nose.
    const Measure::PatchFit fit1 = ultra_curve_fit_print(vt, *m_selected_features.first.feature);
    const Measure::PatchFit fit2 = ultra_curve_fit_print(va, *m_selected_features.second.feature);
    if (fit1.ok && fit2.ok && fit1.shape == Measure::PatchShape::Sphere && fit2.shape == Measure::PatchShape::Sphere) {
        const Transform3d Msph = Transform3d(Eigen::Translation3d(fit1.centre - fit2.centre));
        const double dr = fit2.radius - fit1.radius;
        BOOST_LOG_TRIVIAL(warning) << "[UltraFit] sphere mate: r1=" << fit1.radius << " (res " << fit1.residual
                                   << ") r2=" << fit2.radius << " (res " << fit2.residual << ") dr=" << dr;
        ModelObject* amo_sph = model.objects[va->object_idx()];
        if (amo_sph->instances.empty()) return;
        wxGetApp().plater()->take_snapshot("Auto-fit");
        ultra_apply_attachment_print_pose(Msph * amo_sph->instances[0]->get_transformation().get_matrix());
        m_ultra_adjust_rot = 0.f; m_ultra_adjust_off = 0.f;
        notify((boost::format(_u8L("Auto-fit (sphere): centres aligned, moved %.1fmm. Radii %.2f / %.2f mm (mismatch %.2f mm)."))
                % Msph.translation().norm() % fit1.radius % fit2.radius % std::abs(dr)).str());
        return;
    }
    // A cylinder fit does not change the mate -- the axis it reports IS the picked patch's mean normal
    // direction's orthogonal, and the legacy (normal, centroid) mate plus the collision roll below already
    // handles it. It is logged so a bad pick is diagnosable.
    if (fit1.ok || fit2.ok)
        BOOST_LOG_TRIVIAL(warning) << "[UltraFit] curve fits: 1 shape=" << int(fit1.shape) << " r=" << fit1.radius
                                   << " rel=" << fit1.rel_residual << " | 2 shape=" << int(fit2.shape)
                                   << " r=" << fit2.radius << " rel=" << fit2.rel_residual;

    // Normal alignment: rotate the attachment so its face normal is anti-parallel to the target's.
    Vec3d axis; double phi; Matrix3d R;
    Geometry::rotation_from_two_vectors(d2, -d1, axis, phi, &R);

    // Roll alignment BY CONTAINMENT (shared helper, UltraFitGeometry.hpp): keep the spin about the shared
    // normal where the smaller outline protrudes least outside the larger -- "parts touch, never intersect".
    double roll_deg = 0.0, roll_res = -1.0, roll_clear = 0.0; // reported in the notification
    if (have_b1 && have_b2 && b1.size() >= 3 && b2.size() >= 3) {
        std::vector<Vec3d> b2m; b2m.reserve(b2.size());
        for (const auto& P : b2) b2m.push_back(p1 + R * (P - p2)); // attachment outline after the normal mate
        const UltraFit::RollResult rr = UltraFit::roll_by_containment(d1, p1, b1, b2m);
        if (rr.ok) {
            if (std::abs(rr.roll) > 1e-9) R = Eigen::AngleAxisd(rr.roll, d1.normalized()).toRotationMatrix() * R;
            roll_deg = rr.roll * 180.0 / M_PI; roll_res = rr.protrusion; roll_clear = rr.clearance;
            BOOST_LOG_TRIVIAL(warning) << "[UltraFit] roll by containment: " << roll_deg << " deg, residual=" << rr.protrusion
                                       << " clearance=" << rr.clearance << " att_small=" << rr.attach_is_smaller
                                       << " asp1=" << asp1 << " asp2=" << asp2;
        }
    }
    Transform3d rot = Transform3d::Identity(); rot.linear() = R;
    Transform3d M = Eigen::Translation3d(p1) * rot * Eigen::Translation3d(-p2);

    // Body-level collision-aware roll (UltraFitGeometry.hpp): the containment above only sees the two picked
    // faces, and a curved patch has no rotational cue, so spin the attachment about the mating axis and keep
    // the angle where its whole mesh penetrates the target's solid least (ties -> least extra spin).
    double coll_deg = 0.0, coll_pen = -1.0;
    {
        auto mt = m_mesh_measure_map.find(vt), ma = m_mesh_measure_map.find(va);
        Transform3d tW2p, aW2p;
        if (mt != m_mesh_measure_map.end() && ma != m_mesh_measure_map.end() && mt->second && ma->second &&
            ultra_w2p(vt, *m_selected_features.first.feature,  tW2p) &&
            ultra_w2p(va, *m_selected_features.second.feature, aW2p)) {
            // mesh -> print == (view -> print) * (mesh -> view == world_tran); attachment taken AFTER the mate.
            const Transform3d t_mesh2print = tW2p * m_selected_features.first.feature->world_tran;
            const Transform3d a_mesh2print = M * (aW2p * m_selected_features.second.feature->world_tran);
            const UltraFit::CollisionRoll cr = UltraFit::roll_by_collision(mt->second->get_its(), t_mesh2print,
                                                                           ma->second->get_its(), a_mesh2print, d1, p1);
            if (cr.ok) {
                coll_pen = cr.penetration;
                if (std::abs(cr.roll) > 1e-6) {
                    R   = Eigen::AngleAxisd(cr.roll, d1.normalized()).toRotationMatrix() * R;
                    rot = Transform3d::Identity(); rot.linear() = R;
                    M   = Eigen::Translation3d(p1) * rot * Eigen::Translation3d(-p2);
                    coll_deg = cr.roll * 180.0 / M_PI;
                }
            }
            BOOST_LOG_TRIVIAL(warning) << "[UltraFit] roll by collision: " << coll_deg << " deg, penetration=" << coll_pen
                                       << " samples=" << cr.samples;
        }
    }

    ModelObject* tmo = model.objects[vt->object_idx()];
    ModelObject* amo = model.objects[va->object_idx()];

    const Vec3d mv = M.translation();
    BOOST_LOG_TRIVIAL(warning) << "[UltraFit] mate objs t=" << vt->object_idx() << " a=" << va->object_idx()
        << " move=(" << mv.x() << "," << mv.y() << "," << mv.z() << ") d1=(" << d1.transpose()
        << ") d2=(" << d2.transpose() << ")";

    // Ultra Auto-fit: apply the mate to the attachment's PRINT pose and mirror the same pose RELATIVE TO THE
    // TARGET onto its assembly pose (so the assembly view shows it). Objects stay separate with the picks
    // alive: nudge with the Adjust sliders, then "Merge parts".
    (void) tmo;
    wxGetApp().plater()->take_snapshot("Auto-fit");
    ultra_apply_attachment_print_pose(M * amo->instances[0]->get_transformation().get_matrix());
    m_ultra_adjust_rot = 0.f; m_ultra_adjust_off = 0.f;
    notify((boost::format(_u8L("Auto-fit: moved %.1fmm; outline roll %.1f deg (protrusion %.3f, clearance %.2fmm); collision roll %.1f deg (penetration %.3f). Nudge with the sliders, then Merge parts."))
            % mv.norm() % roll_deg % roll_res % roll_clear % coll_deg % coll_pen).str());
}

// Ultra: view-frame -> PRINT-world transform for a picked volume. Feature coords are baked with
// world_tran (the view's frame, i.e. the exploded assembly pose when picking there).
bool GLGizmoMeasure::ultra_w2p(GLVolume* v, const Measure::SurfaceFeature& f, Transform3d& out)
{
    Model& model = wxGetApp().plater()->model();
    const int oi = v ? v->object_idx() : -1, vi = v ? v->volume_idx() : -1;
    if (oi < 0 || oi >= (int) model.objects.size()) return false;
    ModelObject* mo = model.objects[oi];
    if (mo->instances.empty() || vi < 0 || vi >= (int) mo->volumes.size()) return false;
    const Transform3d printW = mo->instances[0]->get_transformation().get_matrix()
                             * mo->volumes[vi]->get_transformation().get_matrix();
    out = printW * f.world_tran.inverse();
    return true;
}

// Ultra: give the attachment (volume[1]) a new PRINT pose and mirror the same pose RELATIVE TO THE TARGET onto
// its assembly pose. The transform the current canvas SHOWS is committed through the stock selection path
// (do_rotate writes the assembly pose in the assembly view, the print pose in the 3D view) so GLVolumes and
// raycasters stay valid and the view updates; the other one is written directly on the ModelInstance.
void GLGizmoMeasure::ultra_apply_attachment_print_pose(const Transform3d& new_print)
{
    if (m_hit_different_volumes.size() != 2) return;
    GLVolume* vt = m_hit_different_volumes[0];
    GLVolume* va = m_hit_different_volumes[1];
    Model& model = wxGetApp().plater()->model();
    if (!vt || !va || vt->object_idx() < 0 || va->object_idx() < 0 ||
        vt->object_idx() >= (int) model.objects.size() || va->object_idx() >= (int) model.objects.size()) return;
    ModelObject* tmo = model.objects[vt->object_idx()];
    ModelObject* amo = model.objects[va->object_idx()];
    if (tmo->instances.empty() || amo->instances.empty()) return;
    ModelInstance* ti = tmo->instances[0];
    ModelInstance* ai = amo->instances[0];
    const Transform3d rel          = ti->get_transformation().get_matrix().inverse() * new_print; // attachment in target's frame
    const Transform3d new_assemble = ti->get_assemble_transformation().get_matrix() * rel;
    const bool in_assembly_view    = m_parent.get_canvas_type() == GLCanvas3D::ECanvasType::CanvasAssembleView;

    // Write BOTH poses on the ModelInstance directly and update the shown GLVolume through the selection,
    // deliberately NOT via GLCanvas3D::do_rotate: its "fix flying instances" pass grounds any lifted
    // separate object the moment the gizmo runs in the Prepare view, which un-did the fit. The part stays
    // where the fit put it until "Merge parts" (a later manual move would still ground it -- merge first).
    ai->set_transformation(Geometry::Transformation(new_print));
    ai->set_assemble_transformation(Geometry::Transformation(new_assemble));

    auto selection = const_cast<Selection*>(&m_parent.get_selection());
    selection->setup_cache();
    selection->set_mode(Selection::Instance);
    m_pending_scale = 1;
    va->set_instance_transformation(Geometry::Transformation(in_assembly_view ? new_assemble : new_print));
    selection->rotate(va->object_idx(), va->instance_idx(), va->get_instance_transformation().get_matrix());
    m_parent.set_as_dirty();
    wxGetApp().plater()->schedule_background_process(); // print pose changed -> invalidate the slice
    register_single_mesh_pick();
    if (m_selected_features.second.feature.has_value()) update_feature_by_tran(*m_selected_features.second.feature);
}

// Ultra: fuse the two picked objects into one multi-part object (durable: drops / moves / prints as a unit).
void GLGizmoMeasure::ultra_merge_parts()
{
    auto notify = [](const std::string& t) {
        wxGetApp().plater()->get_notification_manager()->push_notification(
            NotificationType::CustomNotification, NotificationManager::NotificationLevel::RegularNotificationLevel, t);
    };
    if (m_hit_different_volumes.size() != 2) return;
    GLVolume* vt = m_hit_different_volumes[0];
    GLVolume* va = m_hit_different_volumes[1];
    Model& model = wxGetApp().plater()->model();
    if (!vt || !va || vt->object_idx() < 0 || va->object_idx() < 0 ||
        vt->object_idx() >= (int) model.objects.size() || va->object_idx() >= (int) model.objects.size()) return;
    if (vt->object_idx() == va->object_idx()) { notify(_u8L("Merge parts: these are already parts of one object.")); return; }
    ModelObject* tmo = model.objects[vt->object_idx()];
    ModelObject* amo = model.objects[va->object_idx()];
    const size_t before = model.objects.size();
    ObjectList* ol = wxGetApp().obj_list();
    ol->select_items(std::vector<ObjectVolumeID>{ ObjectVolumeID{ tmo, nullptr }, ObjectVolumeID{ amo, nullptr } });
    ol->merge(true);
    reset_all_feature(); // merge() deleted the two source objects -> picked GLVolume* would dangle
    notify((boost::format(_u8L("Merged into one object (objects %d -> %d). Check the Prepare view.")) % (int) before % (int) model.objects.size()).str());
}

// Ultra: spin part 2 about the mating axis (feature 1's direction through its point).
void GLGizmoMeasure::ultra_adjust_rotate(double deg_delta, bool take_shot)
{
    if (m_hit_different_volumes.size() != 2 || std::abs(deg_delta) < 1e-6) return;
    if (!m_selected_features.first.feature.has_value() || !m_selected_features.second.feature.has_value()) return;
    if (m_same_model_object) { set_to_around_center_of_faces(true, (float) deg_delta, take_shot); return; } // stock volume path
    Vec3d d1, p1;
    if (!ultra_feature_dir_point(*m_selected_features.first.feature, d1, p1)) return;
    Transform3d W1;
    if (!ultra_w2p(m_hit_different_volumes[0], *m_selected_features.first.feature, W1)) return;
    const Vec3d dp = (W1.linear() * d1).normalized(), pp = W1 * p1;
    Model& model = wxGetApp().plater()->model();
    ModelObject* amo = model.objects[m_hit_different_volumes[1]->object_idx()];
    if (take_shot) wxGetApp().plater()->take_snapshot("Adjust rotation", UndoRedo::SnapshotType::GizmoAction);
    const Transform3d delta = Eigen::Translation3d(pp) * Eigen::AngleAxisd(Geometry::deg2rad(deg_delta), dp) * Eigen::Translation3d(-pp);
    ultra_apply_attachment_print_pose(delta * amo->instances[0]->get_transformation().get_matrix());
}

// Ultra: slide part 2 along the mating axis.
void GLGizmoMeasure::ultra_adjust_offset(double mm_delta, bool take_shot)
{
    if (m_hit_different_volumes.size() != 2 || std::abs(mm_delta) < 1e-6) return;
    if (!m_selected_features.first.feature.has_value() || !m_selected_features.second.feature.has_value()) return;
    Vec3d d1, p1;
    if (!ultra_feature_dir_point(*m_selected_features.first.feature, d1, p1)) return;
    if (m_same_model_object) { set_distance(true, d1.normalized() * mm_delta, take_shot); return; } // stock volume path
    Transform3d W1;
    if (!ultra_w2p(m_hit_different_volumes[0], *m_selected_features.first.feature, W1)) return;
    const Vec3d dp = (W1.linear() * d1).normalized();
    Model& model = wxGetApp().plater()->model();
    ModelObject* amo = model.objects[m_hit_different_volumes[1]->object_idx()];
    if (take_shot) wxGetApp().plater()->take_snapshot("Adjust offset", UndoRedo::SnapshotType::GizmoAction);
    const Transform3d delta = Transform3d(Eigen::Translation3d(dp * mm_delta));
    ultra_apply_attachment_print_pose(delta * amo->instances[0]->get_transformation().get_matrix());
}

// Ultra (Curve mode): fit the picked patch, then carry the fit into PRINT world. A Curve pick is the only
// kind whose facet list describes a curved surface, so every other feature returns a Plane fit and the
// caller keeps the legacy (mean normal, centroid) mate untouched.
Measure::PatchFit GLGizmoMeasure::ultra_curve_fit_print(GLVolume* v, const Measure::SurfaceFeature& f)
{
    Measure::PatchFit fit; // shape == Plane, ok == false
    if (!v || f.get_type() != Measure::SurfaceFeatureType::Curve || !f.plane_indices) return fit;
    auto it = m_mesh_measure_map.find(v);
    if (it == m_mesh_measure_map.end() || !it->second) return fit;
    fit = Measure::fit_patch(it->second->get_its(), *f.plane_indices);
    if (!fit.ok) return fit;
    // mesh -> print == (view -> print) * (mesh -> view == world_tran)
    Transform3d w2p;
    if (!ultra_w2p(v, f, w2p)) { fit.ok = false; fit.shape = Measure::PatchShape::Plane; return fit; }
    const Transform3d M = w2p * f.world_tran;
    fit.centre = M * fit.centre;
    fit.axis   = (M.linear() * fit.axis).normalized();
    // Uniform-scale the radius with the transform (a non-uniform instance scale would make "radius"
    // meaningless anyway, and the residual check below is what guards against a bad fit).
    const double s = (M.linear() * Vec3d::UnitX()).norm();
    fit.radius   *= s;
    fit.residual *= s;
    return fit;
}

// Ultra (Curve mode): app-config keys for the two Curve-pick settings.
static const char* ULTRA_CURVE_ANGLE_KEY  = "assembly_curve_angle";
static const char* ULTRA_CURVE_SHELL_KEY  = "assembly_curve_smooth_shell";
// Ultra (Curve mode): the per-step crease limit. A Curve pick always stops at an edge sharper than this,
// which is what makes a smooth-shell pick stop at a cut face's rim. Not exposed: the owner's ask was the
// TOTAL spread; this one is the "what counts as a sharp edge" constant and 8 deg matches the tessellation
// of every model we have tried.
static constexpr float ULTRA_CURVE_STEP_DEG = 8.0f;
// Ultra (Curve mode): with no total cap a pick can run over the whole mesh, so the facet bound is lifted
// from the hover-time 20000 to "no bound" -- a smooth-shell pick is a deliberate click, not a hover.
static constexpr size_t ULTRA_CURVE_SHELL_MAX_FACETS = size_t(-1);
static constexpr float  ULTRA_CURVE_TOOLTIP_WIDTH = 400.0f;

void GLGizmoMeasure::ultra_load_curve_pick_settings()
{
    if (m_ultra_curve_settings_loaded) return;
    m_ultra_curve_settings_loaded = true;
    AppConfig* cfg = wxGetApp().app_config;
    if (!cfg) return;
    const std::string a = cfg->get(ULTRA_CURVE_ANGLE_KEY);
    if (!a.empty()) {
        try { m_ultra_curve_angle = std::min(90.0f, std::max(5.0f, float(std::stod(a)))); } catch (...) {}
    }
    const std::string s = cfg->get(ULTRA_CURVE_SHELL_KEY);
    if (!s.empty()) m_ultra_curve_smooth_shell = (s == "1" || s == "true");
}

Measure::CurvePickParams GLGizmoMeasure::ultra_curve_pick_params() const
{
    Measure::CurvePickParams p; // library default == the legacy 8 / 20 / 20000 grow
    if (m_measure_mode != EMeasureMode::ONLY_ASSEMBLY || m_assembly_mode != AssemblyMode::CURVE_CURVE)
        return p;
    p.step_deg = ULTRA_CURVE_STEP_DEG;
    if (m_ultra_curve_smooth_shell) {
        p.cap_deg    = -1.0f;                        // no total cap: stop only at sharp edges
        p.max_facets = ULTRA_CURVE_SHELL_MAX_FACETS;
    } else {
        p.cap_deg = m_ultra_curve_angle;
    }
    return p;
}

// Ultra (Curve mode): the "Curve angle" slider and the "Smooth shell" toggle. Changing either invalidates
// the picks made with the old setting, so both reset the selection -- a half-grown patch mated against a
// whole-shell one is not something the user asked for.
void GLGizmoMeasure::ultra_show_curve_pick_ui()
{
    if (m_measure_mode != EMeasureMode::ONLY_ASSEMBLY || m_assembly_mode != AssemblyMode::CURVE_CURVE) return;
    ultra_load_curve_pick_settings();
    AppConfig* cfg = wxGetApp().app_config;
    ImGui::Separator();
    m_imgui->disabled_begin(m_ultra_curve_smooth_shell);
    float ang = m_ultra_curve_angle;
    ImGui::PushItemWidth(std::max(180.0f, ImGui::CalcTextSize("Curve angle 00 deg").x * 2.0f));
    if (ImGui::SliderFloat("##ultra_curve_angle", &ang, 5.0f, 90.0f, "Curve angle %.0f deg")) {
        m_ultra_curve_angle = std::min(90.0f, std::max(5.0f, ang));
        if (cfg) cfg->set("app", ULTRA_CURVE_ANGLE_KEY, std::to_string(int(m_ultra_curve_angle + 0.5f)));
        reset_all_feature();
    }
    ImGui::PopItemWidth();
    m_imgui->disabled_end();
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        m_imgui->tooltip(_L("How far a curve pick may bend away from the facet under the cursor before it stops growing."), ULTRA_CURVE_TOOLTIP_WIDTH);
    bool shell = m_ultra_curve_smooth_shell;
    if (m_imgui->checkbox(_L("Smooth shell"), shell)) {
        m_ultra_curve_smooth_shell = shell;
        if (cfg) cfg->set("app", ULTRA_CURVE_SHELL_KEY, shell ? "1" : "0");
        reset_all_feature();
    }
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Ignore the angle limit and grow until a sharp edge stops it, so one click takes a whole smooth surface (the outer shell of a half sphere, for instance)."), ULTRA_CURVE_TOOLTIP_WIDTH);
}

// Ultra: live Adjust sliders (all modes). Values are cumulative since the last mate / pick; each change is
// applied as a delta. One undo snapshot per drag (taken when the slider is grabbed).
void GLGizmoMeasure::ultra_show_adjust_ui()
{
    if (m_measure_mode != EMeasureMode::ONLY_ASSEMBLY || m_hit_different_volumes.size() != 2) return;
    if (!m_selected_features.first.feature.has_value() || !m_selected_features.second.feature.has_value()) return;
    Vec3d d, p;
    if (!ultra_feature_dir_point(*m_selected_features.first.feature, d, p)) return; // points have no axis
    ImGui::Separator();
    m_imgui->text(_u8L("Adjust part 2 (live)"));
    ImGui::PushItemWidth(std::max(180.0f, ImGui::CalcTextSize("Offset -20.00 mm").x * 2.2f));
    float rot = m_ultra_adjust_rot;
    if (ImGui::SliderFloat("##ultra_adjust_rot", &rot, -180.0f, 180.0f, "Rotate %.1f deg")) {
        const bool shot = ImGui::IsItemActivated();
        ultra_adjust_rotate(double(rot - m_ultra_adjust_rot), shot);
        m_ultra_adjust_rot = rot;
    }
    float off = m_ultra_adjust_off;
    if (ImGui::SliderFloat("##ultra_adjust_off", &off, -20.0f, 20.0f, "Offset %.2f mm")) {
        const bool shot = ImGui::IsItemActivated();
        ultra_adjust_offset(double(off - m_ultra_adjust_off), shot);
        m_ultra_adjust_off = off;
    }
    ImGui::PopItemWidth();
}

void GLGizmoMeasure::set_parallel_distance(bool same_model_object, float dist)
{
    if (m_hit_different_volumes.size() == 2 && abs(dist) >= 0.0f) {
        auto v         = m_hit_different_volumes[1];
        auto selection = const_cast<Selection *>(&m_parent.get_selection());
        selection->setup_cache();

        wxGetApp().plater()->take_snapshot("SetParallelDistanceInMeasure", UndoRedo::SnapshotType::GizmoAction); // avoid storing another snapshot

        selection->set_mode(same_model_object ? Selection::Volume : Selection::Instance);
        m_pending_scale = 1;

        const auto [idx1, normal1, pt1] = m_selected_features.first.feature->get_plane();
        const auto [idx2, normal2, pt2] = m_selected_features.second.feature->get_plane();
        Vec3d proj_pt2;
        Measure::get_point_projection_to_plane(pt2, pt1, normal1, proj_pt2);
        Vec3d new_pt2 = proj_pt2 + normal1 * dist;

        Vec3d displacement = new_pt2 - pt2;

        if (same_model_object == false) {
            selection->translate(v->object_idx(), v->instance_idx(), displacement);
        } else {
            selection->translate(v->object_idx(), v->instance_idx(), v->volume_idx(), displacement);
        }
        wxGetApp().plater()->canvas3D()->do_move("");
        register_single_mesh_pick();
        if (same_model_object) {
            update_feature_by_tran(*m_selected_features.first.feature);
        }
        update_feature_by_tran(*m_selected_features.second.feature);
    }
}

// Ultra (Point/Point mode): the stock tool offered no mate for two points. Translate part 2 so its picked
// point (or circle centre) lands on part 1's -- same displacement path the face mate uses.
void GLGizmoMeasure::ultra_coincide_points()
{
    if (m_hit_different_volumes.size() != 2) return;
    if (!m_selected_features.first.feature.has_value() || !m_selected_features.second.feature.has_value()) return;
    auto pt_of = [](const Measure::SurfaceFeature& f, Vec3d& p) -> bool {
        switch (f.get_type()) {
        case Measure::SurfaceFeatureType::Point:  p = f.get_point(); return true;
        case Measure::SurfaceFeatureType::Circle: p = std::get<0>(f.get_circle()); return true;
        default: return false;
        }
    };
    Vec3d p1, p2;
    if (!pt_of(*m_selected_features.first.feature, p1) || !pt_of(*m_selected_features.second.feature, p2)) return;
    set_distance(m_same_model_object, p1 - p2, true);
}

bool GLGizmoMeasure::is_pick_meet_assembly_mode(const SelectedFeatures::Item &item) {
    if (m_measure_mode == EMeasureMode::ONLY_ASSEMBLY) {
        // Ultra: Face/Face also accepts Circle rims (peg/hole) -- the hover filter and the mate buttons
        // already did, but the click was still rejected here ("please select Plane's feature").
        if (m_assembly_mode == AssemblyMode::FACE_FACE &&
            (item.feature->get_type() == Measure::SurfaceFeatureType::Plane ||
             item.feature->get_type() == Measure::SurfaceFeatureType::Circle)) {
            return true;
        }
        // Ultra: the facet / curved-patch modes accept exactly their own pick kind.
        if (m_assembly_mode == AssemblyMode::TRIANGLE_TRIANGLE && item.feature->get_type() == Measure::SurfaceFeatureType::Triangle) {
            return true;
        }
        if (m_assembly_mode == AssemblyMode::CURVE_CURVE && item.feature->get_type() == Measure::SurfaceFeatureType::Curve) {
            return true;
        }
        if (m_assembly_mode == AssemblyMode::POINT_POINT &&
            (item.feature->get_type() == Measure::SurfaceFeatureType::Point||
                item.feature->get_type() == Measure::SurfaceFeatureType::Circle)) {
            return true;
        }
        return false;
    }
    else {
        return true;
    }
}

// Ultra (Exact highlight): app-config key for the toggle. Off by default, so a plain Auto-fit is unchanged.
static const char* ULTRA_EXACT_KEY = "assembly_exact_highlight";

void GLGizmoMeasure::ultra_load_exact_setting()
{
    if (m_ultra_exact_loaded) return;
    m_ultra_exact_loaded = true;
    AppConfig* cfg = wxGetApp().app_config;
    if (!cfg) return;
    const std::string v = cfg->get(ULTRA_EXACT_KEY);
    if (!v.empty()) m_ultra_exact = (v == "1" || v == "true");
}

bool GLGizmoMeasure::ultra_exact_active() const
{
    return m_ultra_exact && m_measure_mode == EMeasureMode::ONLY_ASSEMBLY &&
           (m_assembly_mode == AssemblyMode::FACE_FACE || m_assembly_mode == AssemblyMode::CURVE_CURVE);
}

// In-plane reference axis for lining the footprint up with the moving face: a flat face's min-area
// rectangle long side (ultra_plane_axis_world), or a Curve pick's cylinder axis. Cached per feature and
// pose, since a hover asks for it on every cursor move.
bool GLGizmoMeasure::ultra_exact_axis(GLVolume* v, const Measure::SurfaceFeature& f, Vec3d& axis, double& aspect)
{
    if (!v || !f.plane_indices) return false;
    std::vector<double> key{ double(int(f.get_type())), f.get_value(), double(f.plane_indices->size()) };
    for (int i = 0; i < 3; ++i) { key.push_back(f.get_pt1()[i]); key.push_back(f.get_pt2()[i]); }
    for (int i = 0; i < 16; ++i) key.push_back(f.world_tran.matrix().data()[i]);
    UltraExactAxisCache& c = m_ultra_exact_axis_cache[m_selected_features.first.feature.has_value() &&
                                                      m_selected_features.first.feature->volume == v ? 0 : 1];
    if (c.v != v || c.key != key) {
        c = UltraExactAxisCache();
        c.v = v; c.key = std::move(key);
        if (f.get_type() == Measure::SurfaceFeatureType::Curve) {
            auto it = m_mesh_measure_map.find(v);
            if (it != m_mesh_measure_map.end() && it->second) {
                const Measure::PatchFit fit = Measure::fit_patch(it->second->get_its(), *f.plane_indices);
                if (fit.ok && fit.shape == Measure::PatchShape::Cylinder) {
                    c.axis = (f.world_tran.linear() * fit.axis).normalized();
                    c.aspect = std::numeric_limits<double>::max(); // an axis, not a square
                    c.ok = true;
                }
            }
        } else {
            c.ok = ultra_plane_axis_world(v, f, c.axis, c.aspect);
        }
    }
    axis = c.axis; aspect = c.aspect;
    return c.ok;
}

// Footprint of the picked target face on the moving feature `fb`, anchored at `a` (view world, like the
// features themselves).
bool GLGizmoMeasure::ultra_exact_footprint(const UltraExactAnchor& a, const Measure::SurfaceFeature& fb, UltraFit::ExactFootprint& out)
{
    out = UltraFit::ExactFootprint();
    if (!a.valid || !a.volume || !m_selected_features.first.feature.has_value()) return false;
    const Measure::SurfaceFeature& fa = *m_selected_features.first.feature;
    if (!fa.is_planar() || !fa.plane_indices || !fb.is_planar() || !fb.plane_indices) return false;
    GLVolume* vt = static_cast<GLVolume*>(fa.volume);
    if (!vt || vt == a.volume || fb.volume != a.volume) return false;
    auto mt = m_mesh_measure_map.find(vt), mb = m_mesh_measure_map.find(a.volume);
    if (mt == m_mesh_measure_map.end() || mb == m_mesh_measure_map.end() || !mt->second || !mb->second) return false;
    Vec3d nA, cA, nB, cB;
    if (!ultra_feature_dir_point(fa, nA, cA) || !ultra_feature_dir_point(fb, nB, cB)) return false;

    UltraFit::ExactFootprintInput in;
    in.t_its    = &mt->second->get_its();
    in.t_facets = fa.plane_indices;
    in.t_w      = fa.world_tran;
    in.t_normal = nA;
    in.t_centre = cA;
    in.a_its    = &mb->second->get_its();
    in.a_facets = fb.plane_indices;
    in.a_w      = fb.world_tran;
    in.a_hit    = fb.world_tran * a.hit_mesh;
    in.a_curved = fb.get_type() == Measure::SurfaceFeatureType::Curve;
    // A flat face has one normal; on a curved one start from the facet under the cursor (the footprint
    // then re-estimates it from the region it covers).
    in.a_normal = in.a_curved ? Vec3d((fb.world_tran.linear().inverse().transpose() * a.normal_mesh).normalized()) : nB;
    Vec3d ta, tb; double asp_a = 1.0, asp_b = 1.0;
    if (ultra_exact_axis(vt, fa, ta, asp_a) && ultra_exact_axis(a.volume, fb, tb, asp_b)) {
        in.t_axis = ta; in.a_axis = tb;
        // Two oblong faces line up long side to long side; if either is squarish any edge will do.
        in.axis_fold = (asp_a > 1.1 && asp_b > 1.1) ? M_PI : M_PI / 2;
    }
    out = UltraFit::exact_footprint(in);
    return out.ok;
}

void GLGizmoMeasure::ultra_exact_init_model(const UltraFit::ExactFootprint& fp, PickingModel& model)
{
    model.reset();
    GLModel::Geometry g;
    g.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    g.reserve_vertices(fp.tris.size());
    g.reserve_indices(fp.tris.size());
    unsigned int k = 0;
    for (size_t i = 0; i + 2 < fp.tris.size(); i += 3) {
        const Vec3d& a = fp.tris[i]; const Vec3d& b = fp.tris[i + 1]; const Vec3d& c = fp.tris[i + 2];
        Vec3d n = (b - a).cross(c - a);
        if (n.norm() < 1e-12) continue;
        n.normalize();
        const Vec3d off = n * double(MEASURE_PLNE_NORMAL_OFFSET);
        const Vec3f nf  = n.cast<float>();
        g.add_vertex(Vec3f((a + off).cast<float>()), nf);
        g.add_vertex(Vec3f((b + off).cast<float>()), nf);
        g.add_vertex(Vec3f((c + off).cast<float>()), nf);
        g.add_triangle(k, k + 1, k + 2);
        k += 3;
    }
    if (k > 0) model.model.init_from(std::move(g));
}

// Rebuild the hover and picked footprint highlights when their inputs changed (cursor spot, either
// feature, either pose); otherwise reuse them.
void GLGizmoMeasure::ultra_exact_refresh_models()
{
    auto key_of = [](const UltraExactAnchor& a, const Measure::SurfaceFeature& fa, const Measure::SurfaceFeature& fb) {
        std::vector<double> k{ double(reinterpret_cast<uintptr_t>(a.volume)), double(reinterpret_cast<uintptr_t>(fa.volume)) };
        for (int i = 0; i < 3; ++i) { k.push_back(a.hit_mesh[i]); k.push_back(a.normal_mesh[i]); }
        for (const Measure::SurfaceFeature* f : { &fa, &fb }) {
            k.push_back(double(int(f->get_type()))); k.push_back(f->get_value());
            for (int i = 0; i < 3; ++i) { k.push_back(f->get_pt1()[i]); k.push_back(f->get_pt2()[i]); }
            for (int i = 0; i < 16; ++i) k.push_back(f->world_tran.matrix().data()[i]);
        }
        return k;
    };
    auto refresh = [&](const UltraExactAnchor& a, const Measure::SurfaceFeature& fb, std::vector<double>& key, PickingModel& model) {
        std::vector<double> k = key_of(a, *m_selected_features.first.feature, fb);
        if (k != key) {
            key = std::move(k);
            UltraFit::ExactFootprint fp;
            if (ultra_exact_footprint(a, fb, fp)) ultra_exact_init_model(fp, model);
            else model.reset();
        }
        return model.model.is_initialized();
    };

    m_ultra_exact_hover_shown = false;
    m_ultra_exact_pick_shown  = false;
    if (!ultra_exact_active() || !m_selected_features.first.feature.has_value()) return;
    if (m_ultra_exact_hover.valid && m_curr_feature.has_value() && m_curr_feature->is_planar())
        m_ultra_exact_hover_shown = refresh(m_ultra_exact_hover, *m_curr_feature, m_ultra_exact_hover_key, m_ultra_exact_hover_model);
    if (m_ultra_exact_pick.valid && m_selected_features.second.feature.has_value() &&
        m_selected_features.second.feature->volume == m_ultra_exact_pick.volume)
        m_ultra_exact_pick_shown = refresh(m_ultra_exact_pick, *m_selected_features.second.feature, m_ultra_exact_pick_key, m_ultra_exact_pick_model);
}

// Ultra (Exact highlight) Auto-fit: move the moving part so the clicked footprint lands on the target face
// exactly as highlighted (no outline or collision roll search -- what was shown is what you get; the Adjust
// sliders still spin / slide it afterwards). Returns false when the toggle is off or no footprint was
// clicked, so the caller runs the normal Auto-fit.
bool GLGizmoMeasure::ultra_exact_fit()
{
    if (!ultra_exact_active() || !m_ultra_exact_pick.valid) return false;
    if (m_hit_different_volumes.size() != 2) return false;
    if (!m_selected_features.first.feature.has_value() || !m_selected_features.second.feature.has_value()) return false;
    GLVolume* vt = m_hit_different_volumes[0];
    GLVolume* va = m_hit_different_volumes[1];
    if (!vt || !va || m_ultra_exact_pick.volume != va || m_selected_features.second.feature->volume != va) return false;
    auto notify = [](const std::string& t) {
        wxGetApp().plater()->get_notification_manager()->push_notification(
            NotificationType::CustomNotification, NotificationManager::NotificationLevel::RegularNotificationLevel, t);
    };
    UltraFit::ExactFootprint fp;
    if (!ultra_exact_footprint(m_ultra_exact_pick, *m_selected_features.second.feature, fp)) {
        notify(_u8L("Exact highlight: the footprint could not be placed, so the normal Auto-fit was used."));
        return false;
    }
    const Vec3d move = fp.mate * fp.anchor - fp.anchor;
    Model& model = wxGetApp().plater()->model();
    if (vt->object_idx() == va->object_idx()) {
        // Two parts of one object: the view frame is the object's own, so move the part's volume transform.
        if (vt->volume_idx() == va->volume_idx()) return false;
        auto selection = const_cast<Selection*>(&m_parent.get_selection());
        selection->setup_cache();
        wxGetApp().plater()->take_snapshot("Auto-fit (exact)", UndoRedo::SnapshotType::GizmoAction);
        selection->set_mode(Selection::Volume);
        m_pending_scale = 1;
        const Transform3d inst = va->get_instance_transformation().get_matrix();
        const Transform3d vol  = inst.inverse() * fp.mate * inst * va->get_volume_transformation().get_matrix();
        selection->rotate(va->object_idx(), va->instance_idx(), va->volume_idx(), vol);
        wxGetApp().plater()->canvas3D()->do_rotate("");
        register_single_mesh_pick();
        update_feature_by_tran(*m_selected_features.first.feature);
        update_feature_by_tran(*m_selected_features.second.feature);
    } else {
        // Separate objects: carry the view-frame mate into PRINT world (as the normal Auto-fit does) and
        // apply it to both the print and the assembly pose.
        Transform3d W1, W2;
        if (!ultra_w2p(vt, *m_selected_features.first.feature, W1) || !ultra_w2p(va, *m_selected_features.second.feature, W2)) return false;
        ModelObject* amo = model.objects[va->object_idx()];
        if (amo->instances.empty()) return false;
        wxGetApp().plater()->take_snapshot("Auto-fit");
        ultra_apply_attachment_print_pose(W1 * fp.mate * W2.inverse() * amo->instances[0]->get_transformation().get_matrix());
    }
    m_ultra_adjust_rot = 0.f; m_ultra_adjust_off = 0.f;
    BOOST_LOG_TRIVIAL(warning) << "[UltraFit] exact footprint: move=" << move.norm() << " roll=" << fp.roll * 180.0 / M_PI
                               << " contact_shift=" << fp.contact_shift << " draped=" << fp.draped << " missed=" << fp.missed;
    std::string msg = (boost::format(_u8L("Auto-fit (exact): the highlighted footprint landed on the target face (moved %.1fmm, edge roll %.1f deg, contact shift %.2fmm)."))
                       % move.norm() % (fp.roll * 180.0 / M_PI) % fp.contact_shift).str();
    if (fp.missed > 0)
        msg += " " + (boost::format(_u8L("%d footprint points hang past the edge of the moving face.")) % fp.missed).str();
    notify(msg);
    return true;
}

// Ultra (Exact highlight): the toggle, Face and Curve modes only. Flipping it drops the moving pick, whose
// meaning (whole face vs footprint) just changed; the target pick stays.
void GLGizmoMeasure::ultra_show_exact_ui()
{
    if (m_measure_mode != EMeasureMode::ONLY_ASSEMBLY ||
        (m_assembly_mode != AssemblyMode::FACE_FACE && m_assembly_mode != AssemblyMode::CURVE_CURVE)) return;
    ultra_load_exact_setting();
    bool exact = m_ultra_exact;
    if (m_imgui->checkbox(_L("Exact highlight"), exact)) {
        m_ultra_exact = exact;
        if (AppConfig* cfg = wxGetApp().app_config) cfg->set("app", ULTRA_EXACT_KEY, exact ? "1" : "0");
        if (m_selected_features.second.feature.has_value()) reset_feature2();
        m_ultra_exact_hover_key.clear();
        m_ultra_exact_pick_key.clear();
    }
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("For a moving part whose face is larger than the target's. Pick the target face first; the moving part "
                            "then highlights only the target face's own shape, centred on the cursor. Click where it should touch, "
                            "and Auto-fit lands exactly that spot on the target."), ULTRA_CURVE_TOOLTIP_WIDTH);
}

} // namespace GUI
} // namespace Slic3r
