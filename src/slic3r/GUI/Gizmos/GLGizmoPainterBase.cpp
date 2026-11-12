#include "GLGizmoPainterBase.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"

#include <GL/glew.h>

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>
#include <wx/glcanvas.h>

namespace Slic3r::GUI {

namespace {

constexpr size_t RENDER_CHUNK_ROOT_TARGET = 8192;
constexpr size_t MAX_COLOR_UPLOAD_RANGES = 8;
// Vertex budget per chunk draw call: chunk.vertexCount is uint32_t and drawn via GLsizei.
constexpr size_t MAX_SUPPORTED_CHUNK_VERTICES = 2147483647u;

uint32_t ExpandMortonBits(uint32_t value)
{
    value &= 0x000003ffu;
    value = (value | (value << 16)) & 0x030000ffu;
    value = (value | (value << 8)) & 0x0300f00fu;
    value = (value | (value << 4)) & 0x030c30c3u;
    value = (value | (value << 2)) & 0x09249249u;
    return value;
}

float NormalizeMortonCoordinate(float value, float minimum, float extent)
{
    if (extent <= EPSILON)
        return 0.5f;

    return std::clamp((value - minimum) / extent, 0.0f, 1.0f);
}

uint32_t CalculateMortonCode(const Vec3f& point, const Vec3f& minimum, const Vec3f& extent)
{
    const uint32_t x = static_cast<uint32_t>(NormalizeMortonCoordinate(point.x(), minimum.x(), extent.x()) * 1023.0f);
    const uint32_t y = static_cast<uint32_t>(NormalizeMortonCoordinate(point.y(), minimum.y(), extent.y()) * 1023.0f);
    const uint32_t z = static_cast<uint32_t>(NormalizeMortonCoordinate(point.z(), minimum.z(), extent.z()) * 1023.0f);
    return ExpandMortonBits(x) | (ExpandMortonBits(y) << 1) | (ExpandMortonBits(z) << 2);
}

} // namespace

std::shared_ptr<GLModel> GLGizmoPainterBase::s_sphere = nullptr;

GLGizmoPainterBase::GLGizmoPainterBase(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{
    m_vertical_only = false;
    m_horizontal_only = false;
}

GLGizmoPainterBase::~GLGizmoPainterBase()
{
    DetachTriangleSelectorGlResources();
    if (s_sphere != nullptr)
        s_sphere.reset();
}

void GLGizmoPainterBase::DiscardPaintingPreviewsAndFlush()
{
    for (const std::unique_ptr<TriangleSelectorGUI>& triangleSelector : m_triangle_selectors)
        triangleSelector->ClearPointerPreview();

    for (const std::unique_ptr<TriangleSelectorGUI>& triangleSelector : m_triangle_selectors)
        triangleSelector->seed_fill_unselect_all_triangles();

    for (const std::unique_ptr<TriangleSelectorGUI>& triangleSelector : m_triangle_selectors)
    {
        triangleSelector->FlushDeferredCleanup();
        triangleSelector->request_update_render_data();
    }
    m_seed_fill_last_mesh_id = -1;
    if (!m_triangle_selectors.empty())
        m_parent.set_as_dirty();
}

void GLGizmoPainterBase::FinishPaintingInteraction()
{
    if (m_finishingPaintingInteraction)
        return;

    if (m_button_down != Button::None)
    {
        const wxString actionName = handle_snapshot_action_name(false, m_button_down);
        FinalizePaintingStroke(actionName);
        ReleasePaintingMouse();
        return;
    }

    DiscardPaintingPreviewsAndFlush();
    m_strokeHasCommittedState = false;
}

void GLGizmoPainterBase::FinalizePaintingStroke(const wxString& actionName)
{
    if (m_button_down == Button::None || m_finishingPaintingInteraction)
        return;

    m_finishingPaintingInteraction = true;
    DiscardPaintingPreviewsAndFlush();

    if (m_strokeHasCommittedState)
    {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), std::string(actionName.ToUTF8().data()),
                                      UndoRedo::SnapshotType::GizmoAction);
        update_model_object();
    }

    m_button_down = Button::None;
    m_strokeHasCommittedState = false;
    m_last_mouse_click = Vec2d::Zero();
    _heightRangeLastBottomZ.reset();
    m_finishingPaintingInteraction = false;
}

void GLGizmoPainterBase::SplitPaintingStroke()
{
    const Button pressedButton = m_button_down;
    if (pressedButton == Button::None)
    {
        FinishPaintingInteraction();
        return;
    }

    FinalizePaintingStroke(handle_snapshot_action_name(false, pressedButton));
    m_button_down = pressedButton;
}

void GLGizmoPainterBase::ResetPaintingStroke()
{
    if (m_finishingPaintingInteraction)
        return;

    DiscardPaintingPreviewsAndFlush();
    m_strokeHasCommittedState = false;
    m_last_mouse_click = Vec2d::Zero();
    _heightRangeLastBottomZ.reset();
}

void GLGizmoPainterBase::CapturePaintingMouse()
{
    if (m_button_down == Button::None)
        return;

    wxGLCanvas* canvas = m_parent.get_wxglcanvas();
    if (canvas != nullptr && wxWindow::GetCapture() == nullptr)
        canvas->CaptureMouse();
}

void GLGizmoPainterBase::ReleasePaintingMouse()
{
    wxGLCanvas* canvas = m_parent.get_wxglcanvas();
    if (canvas != nullptr && canvas->HasCapture())
        canvas->ReleaseMouse();
}

void GLGizmoPainterBase::DetachTriangleSelectorGlResources()
{
    std::vector<unsigned int> bufferIds;
    for (const std::unique_ptr<TriangleSelectorGUI>& selector : m_triangle_selectors) {
        TriangleSelectorPatch* patchSelector = dynamic_cast<TriangleSelectorPatch*>(selector.get());
        if (patchSelector == nullptr)
            continue;

        std::vector<unsigned int> selectorIds = patchSelector->DetachGlResources();
        bufferIds.insert(bufferIds.end(), selectorIds.begin(), selectorIds.end());
    }
    DetachHeightPreviewGlResources(bufferIds);

    if (!bufferIds.empty())
        wxGetApp().get_opengl_manager().EnqueueBufferDeletes(std::move(bufferIds));
}

void GLGizmoPainterBase::ReleaseTriangleSelectorGlResources()
{
    for (const std::unique_ptr<TriangleSelectorGUI>& selector : m_triangle_selectors) {
        TriangleSelectorPatch* patchSelector = dynamic_cast<TriangleSelectorPatch*>(selector.get());
        if (patchSelector != nullptr)
            patchSelector->ReleaseGlResources();
    }
    ReleaseHeightPreviewGlResources();
}

void GLGizmoPainterBase::data_changed(bool is_serializing)
{
    if (m_state != On)
        return;

    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    const Selection &  selection = m_parent.get_selection();
    if (mo && selection.is_from_single_instance()
     && (m_schedule_update || mo->id() != m_old_mo_id || mo->volumes.size() != m_old_volumes_size))
    {
        InvalidateHeightPreviewCaches("model_rebuild");
        //BBS: add logic to distinguish the first_time_update and later_update
        update_from_model_object(!m_schedule_update);

        m_old_mo_id = mo->id();
        m_old_volumes_size = mo->volumes.size();
        m_schedule_update = false;
    }
}

GLGizmoPainterBase::ClippingPlaneDataWrapper GLGizmoPainterBase::get_clipping_plane_data() const
{
    ClippingPlaneDataWrapper clp_data_out{{0.f, 0.f, 1.f, FLT_MAX}, {-FLT_MAX, FLT_MAX}};
    // Take care of the clipping plane. The normal of the clipping plane is
    // saved with opposite sign than we need to pass to OpenGL (FIXME)
    if (bool clipping_plane_active = m_c->object_clipper()->get_position() != 0.; clipping_plane_active) {
        const ClippingPlane *clp = m_c->object_clipper()->get_clipping_plane();
        for (size_t i = 0; i < 3; ++i)
            clp_data_out.clp_dataf[i] = -1.f * float(clp->get_data()[i]);
        clp_data_out.clp_dataf[3] = float(clp->get_data()[3]);
    }

    // z_range is calculated in the same way as in GLCanvas3D::_render_objects(GLVolumeCollection::ERenderType type)
    if (m_c->get_canvas()->get_use_clipping_planes()) {
        const std::array<ClippingPlane, 2> &clps = m_c->get_canvas()->get_clipping_planes();
        clp_data_out.z_range                     = {float(-clps[0].get_data()[3]), float(clps[1].get_data()[3])};
    }

    return clp_data_out;
}

void GLGizmoPainterBase::render_triangles(const Selection& selection) const
{
    auto* shader = wxGetApp().get_shader("mm_gouraud");
    if (!shader)
        return;
    shader->start_using();
    ScopeGuard guard([shader]() { if (shader) shader->stop_using(); });

    ClippingPlaneDataWrapper clp_data = this->get_clipping_plane_data();
    shader->set_uniform("clipping_plane", clp_data.clp_dataf);
    shader->set_uniform("z_range", clp_data.z_range);

    // BBS: to improve the random white pixel issue
    glsafe(::glDisable(GL_CULL_FACE));

    const ModelObject* mo = m_c->selection_info()->model_object();
    int                mesh_id = -1;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;

        ++mesh_id;

        Transform3d trafo_matrix;
        if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
            trafo_matrix = mo->instances[selection.get_instance_idx()]->get_assemble_transformation().get_matrix() * mv->get_matrix();
            trafo_matrix.translate(mv->get_transformation().get_offset() * (GLVolume::explosion_ratio - 1.0) + mo->instances[selection.get_instance_idx()]->get_offset_to_assembly() * (GLVolume::explosion_ratio - 1.0));
        }
        else {
            trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix()* mv->get_matrix();
        }

        bool is_left_handed = trafo_matrix.matrix().determinant() < 0.;
        if (is_left_handed)
            glsafe(::glFrontFace(GL_CW));

        const Camera& camera = wxGetApp().plater()->get_camera();
        const Transform3d& view_matrix = camera.get_view_matrix();
        shader->set_uniform("view_model_matrix", view_matrix * trafo_matrix);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);

        float normal_z = -::cos(Geometry::deg2rad(m_highlight_by_angle_threshold_deg));
        Matrix3f normal_matrix = static_cast<Matrix3f>(trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose().cast<float>());

        shader->set_uniform("volume_world_matrix", trafo_matrix);
        shader->set_uniform("volume_mirrored", is_left_handed);
        shader->set_uniform("slope.actived", m_parent.is_using_slope());
        shader->set_uniform("slope.volume_world_normal_matrix", normal_matrix);
        shader->set_uniform("slope.normal_z", normal_z);
        m_triangle_selectors[mesh_id]->render(m_imgui, trafo_matrix);

        if (is_left_handed)
            glsafe(::glFrontFace(GL_CCW));
    }
}

void GLGizmoPainterBase::render_cursor()
{
    // First check that the mouse pointer is on an object.
    const ModelObject* mo = m_c->selection_info()->model_object();
    const Selection& selection = m_parent.get_selection();
    const ModelInstance* mi = mo->instances[selection.get_instance_idx()];
    const Camera& camera = wxGetApp().plater()->get_camera();

    // Precalculate transformations of individual meshes.
    std::vector<Transform3d> trafo_matrices;
    for (const ModelVolume* mv : mo->volumes) {
        if (mv->is_model_part())
        {
            if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
                Transform3d temp = mi->get_assemble_transformation().get_matrix() * mv->get_matrix();
                temp.translate(mv->get_transformation().get_offset() * (GLVolume::explosion_ratio - 1.0) + mi->get_offset_to_assembly() * (GLVolume::explosion_ratio - 1.0));
                trafo_matrices.emplace_back(temp);
            }
            else {
                trafo_matrices.emplace_back(mi->get_transformation().get_matrix() * mv->get_matrix());
            }
        }
    }
    // Raycast and return if there's no hit.
    update_raycast_cache(m_parent.get_local_mouse_position(), camera, trafo_matrices);
    if (m_rr.mesh_id == -1)
        return;

    if (m_tool_type == ToolType::BRUSH) {
        if (m_cursor_type == TriangleSelector::SPHERE)
            render_cursor_sphere(trafo_matrices[m_rr.mesh_id]);
        else if (m_cursor_type == TriangleSelector::CIRCLE)
            render_cursor_circle();
        else if (m_cursor_type == TriangleSelector::HEIGHT_RANGE)
            render_cursor_height_range(trafo_matrices[m_rr.mesh_id]);
    }
}

void GLGizmoPainterBase::render_cursor_circle()
{
    const Size cnv_size = m_parent.get_canvas_size();
    const float cnv_width  = float(cnv_size.get_width());
    const float cnv_height = float(cnv_size.get_height());
    if (cnv_width == 0.0f || cnv_height == 0.0f)
        return;

    const float cnv_inv_width  = 1.0f / cnv_width;
    const float cnv_inv_height = 1.0f / cnv_height;

    const Vec2d center = m_parent.get_local_mouse_position();
    const float radius = m_cursor_radius * float(wxGetApp().plater()->get_camera().get_zoom());

    glsafe(::glLineWidth(1.5f));
    glsafe(::glDisable(GL_DEPTH_TEST));

    glsafe(::glPushAttrib(GL_ENABLE_BIT));
    glsafe(::glLineStipple(4, 0xAAAA));
    glsafe(::glEnable(GL_LINE_STIPPLE));

    if (!m_circle.is_initialized() || !m_old_center.isApprox(center) || std::abs(m_old_cursor_radius - radius) > EPSILON) {
        m_old_cursor_radius = radius;
        m_old_center = center;
        m_circle.reset();

        GLModel::Geometry init_data;
        static const unsigned int StepsCount = 32;
        static const float StepSize = 2.0f * float(PI) / float(StepsCount);
        init_data.format = { GLModel::Geometry::EPrimitiveType::LineLoop, GLModel::Geometry::EVertexLayout::P2 };
        init_data.color  = { 0.0f, 1.0f, 0.3f, 1.0f };
        init_data.reserve_vertices(StepsCount);
        init_data.reserve_indices(StepsCount);

        // vertices + indices
        for (unsigned int i = 0; i < StepsCount; ++i) {
            const float angle = float(i * StepSize);
            init_data.add_vertex(Vec2f(2.0f * ((center.x() + ::cos(angle) * radius) * cnv_inv_width - 0.5f),
                                       -2.0f * ((center.y() + ::sin(angle) * radius) * cnv_inv_height - 0.5f)));
            init_data.add_index(i);
        }

        m_circle.init_from(std::move(init_data));
    }

    // BBS
    ColorRGBA render_color = this->get_cursor_hover_color();
    if (m_button_down == Button::Left)
        render_color = this->get_cursor_sphere_left_button_color();
    else if (m_button_down == Button::Right)
        render_color = this->get_cursor_sphere_right_button_color();

    m_circle.set_color(render_color);

    GLShaderProgram* shader = GUI::wxGetApp().get_shader("flat");
    if (shader != nullptr) {
        shader->start_using();
        shader->set_uniform("view_model_matrix", Transform3d::Identity());
        shader->set_uniform("projection_matrix", Transform3d::Identity());
        m_circle.render();
        shader->stop_using();
    }

    glsafe(::glPopAttrib());
    glsafe(::glEnable(GL_DEPTH_TEST));
}


void GLGizmoPainterBase::render_cursor_sphere(const Transform3d& trafo) const
{
    if (s_sphere == nullptr) {
        s_sphere = std::make_shared<GLModel>();
        s_sphere->init_from(its_make_sphere(1.0, double(PI) / 12.0));
    }

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Transform3d complete_scaling_matrix_inverse = Geometry::Transformation(trafo).get_scaling_factor_matrix().inverse();

    // BBS
    ColorRGBA render_color = this->get_cursor_hover_color();
    if (m_button_down == Button::Left)
        render_color = this->get_cursor_sphere_left_button_color();
    else if (m_button_down == Button::Right)
        render_color = this->get_cursor_sphere_right_button_color();

    shader->start_using();

    const Camera& camera = wxGetApp().plater()->get_camera();
    Transform3d view_model_matrix = camera.get_view_matrix() * trafo *
        Geometry::assemble_transform(m_rr.hit.cast<double>()) * complete_scaling_matrix_inverse *
        Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), m_cursor_radius * Vec3d::Ones());

    shader->set_uniform("view_model_matrix", view_model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    const bool is_left_handed = Geometry::Transformation(view_model_matrix).is_left_handed();
    if (is_left_handed)
        glsafe(::glFrontFace(GL_CW));

    assert(s_sphere != nullptr);
    s_sphere->set_color(render_color);
    s_sphere->render();

    if (is_left_handed)
        glsafe(::glFrontFace(GL_CCW));

    shader->stop_using();
}

// BBS
void GLGizmoPainterBase::render_cursor_height_range(const Transform3d& trafo) const
{
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    shader->start_using();

    const BoundingBoxf3 box = bounding_box();
    Vec3d hit_world = trafo * Vec3d(m_rr.hit(0), m_rr.hit(1), m_rr.hit(2));
    float max_z = (float)box.max.z();
    float min_z = (float)box.min.z();

    float cursor_z = std::clamp((float)hit_world.z(), min_z, max_z);
    std::array<float, 2> zs = { cursor_z, std::clamp(cursor_z + m_cursor_height, min_z, max_z) };

    const Selection& selection = m_parent.get_selection();
    const ModelObject* model_object = wxGetApp().model().objects[selection.get_object_idx()];
    const ModelInstance* mi = model_object->instances[selection.get_instance_idx()];
    const Camera& camera = wxGetApp().plater()->get_camera();

    for (size_t volumeIndex = 0; volumeIndex < model_object->volumes.size(); ++volumeIndex)
    {
        const ModelVolume& volume = *model_object->volumes[volumeIndex];

        // Keep the exact baseline transform expression, including assemble view and explosion.
        Transform3d effectiveTransform = Transform3d::Identity();
        if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
            effectiveTransform = mi->get_assemble_transformation().get_matrix() * volume.get_matrix();
            effectiveTransform.translate(volume.get_transformation().get_offset() * (GLVolume::explosion_ratio - 1.0) +
                                         mi->get_offset_to_assembly() * (GLVolume::explosion_ratio - 1.0));
        }
        else {
            effectiveTransform = mi->get_transformation().get_matrix() * volume.get_matrix();
        }

        HeightPreviewVolumeCache& cache = GetHeightPreviewVolumeCache(volumeIndex);
        if (!EnsureHeightPreviewMesh(cache, *model_object, *mi, volume, effectiveTransform)) {
            for (HeightPreviewCut& cut : cache.cuts) {
                cut.key.reset();
                cut.contours.reset();
            }
            continue;
        }

        for (size_t cutIndex = 0; cutIndex < cache.cuts.size(); ++cutIndex)
        {
            UpdateHeightPreviewCut(cache, cutIndex, zs[cutIndex], min_z, max_z);

            // Contours are already in world coordinates; no extra shift.
            shader->set_uniform("view_model_matrix", camera.get_view_matrix());
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            glsafe(::glLineWidth(2.0f));
            cache.cuts[cutIndex].contours.render();
        }
    }

    shader->stop_using();
}

GLGizmoPainterBase::HeightPreviewVolumeCache&
GLGizmoPainterBase::GetHeightPreviewVolumeCache(size_t rawVolumeIndex) const
{
    if (m_heightPreviewVolumes.size() <= rawVolumeIndex)
        m_heightPreviewVolumes.resize(rawVolumeIndex + 1);

    auto& entry = m_heightPreviewVolumes[rawVolumeIndex];
    if (!entry)
        entry = std::make_unique<HeightPreviewVolumeCache>();

    return *entry;
}

bool GLGizmoPainterBase::EnsureHeightPreviewMesh(HeightPreviewVolumeCache& cache,
                                                 const ModelObject& object,
                                                 const ModelInstance& instance,
                                                 const ModelVolume& volume,
                                                 const Transform3d& effectiveTransform) const
{
    std::shared_ptr<const TriangleMesh> source = volume.mesh_ptr();
    if (!source || !effectiveTransform.matrix().allFinite())
        return false;

    // Exact matrix compare: a missed hit only costs performance, a false hit shows wrong geometry.
    if (cache.sourceMesh == source &&
        cache.objectId == object.id() &&
        cache.instanceId == instance.id() &&
        cache.volumeId == volume.id() &&
        (cache.effectiveTransform.matrix().array() == effectiveTransform.matrix().array()).all())
        return true;

    // Build the index first; publish the cache identity only after success.
    cache.sourceMesh.reset();
    cache.zIndex.Build(*source, effectiveTransform);
    cache.sourceMesh = std::move(source);
    cache.objectId = object.id();
    cache.instanceId = instance.id();
    cache.volumeId = volume.id();
    cache.effectiveTransform = effectiveTransform;
    ++cache.meshRevision;

    // CPU metadata invalidation; cut GLModels are governed by their own keys.
    for (HeightPreviewCut& cut : cache.cuts)
        cut.key.reset();

    return true;
}

indexed_triangle_set GLGizmoPainterBase::BuildHeightPreviewCutMesh(HeightPreviewVolumeCache& cache, float zWorld) const
{
    indexed_triangle_set cutMesh;
    if (cache.sourceMesh == nullptr)
        return cutMesh;

    const indexed_triangle_set& sourceIts = cache.sourceMesh->its;
    const int sourceVertexCount = static_cast<int>(sourceIts.vertices.size());

    // Sorted faces and a monotonic vertex remap keep the original relative order, so the slice result is unchanged.
    std::vector<uint32_t> candidateFaces = cache.zIndex.Query(zWorld, zWorld);
    std::sort(candidateFaces.begin(), candidateFaces.end());

    std::vector<int> usedVertices;
    usedVertices.reserve(candidateFaces.size() * 3);
    for (uint32_t face : candidateFaces)
    {
        assert(face < sourceIts.indices.size());
        const stl_triangle_vertex_indices& indices = sourceIts.indices[face];
        for (int corner = 0; corner < 3; ++corner)
            if (indices(corner) >= 0 && indices(corner) < sourceVertexCount)
                usedVertices.push_back(indices(corner));
    }
    std::sort(usedVertices.begin(), usedVertices.end());
    usedVertices.erase(std::unique(usedVertices.begin(), usedVertices.end()), usedVertices.end());

    // Same per-vertex arithmetic as TriangleMesh::transform(), the previous full-mesh path.
    cutMesh.vertices.reserve(usedVertices.size());
    for (int vertexIndex : usedVertices)
    {
        const stl_vertex worldVertex =
            (cache.effectiveTransform * sourceIts.vertices[vertexIndex].cast<double>()).cast<float>();
        cutMesh.vertices.push_back(worldVertex);
    }

    const auto remapVertex = [&usedVertices](int vertexIndex) -> int
    {
        const std::vector<int>::const_iterator found =
            std::lower_bound(usedVertices.begin(), usedVertices.end(), vertexIndex);
        return found != usedVertices.end() && *found == vertexIndex ?
            static_cast<int>(found - usedVertices.begin()) : -1;
    };

    cutMesh.indices.reserve(candidateFaces.size());
    for (uint32_t face : candidateFaces)
    {
        const stl_triangle_vertex_indices& indices = sourceIts.indices[face];
        const int first = remapVertex(indices(0));
        const int second = remapVertex(indices(1));
        const int third = remapVertex(indices(2));
        if (first >= 0 && second >= 0 && third >= 0)
            cutMesh.indices.emplace_back(first, second, third);
    }

    return cutMesh;
}

void GLGizmoPainterBase::UpdateHeightPreviewCut(HeightPreviewVolumeCache& cache, size_t cutIndex,
                                                float zWorld, float minZ, float maxZ) const
{
    assert(cutIndex < cache.cuts.size());
    HeightPreviewCut& cut = cache.cuts[cutIndex];

    const bool finiteRange =
        std::isfinite(zWorld) && std::isfinite(minZ) && std::isfinite(maxZ) && minZ <= maxZ;
    const bool enabled = finiteRange && cache.sourceMesh != nullptr && minZ < zWorld && zWorld < maxZ;

    const HeightPreviewCutKey currentKey{cache.meshRevision, zWorld, enabled};

    if (cut.key.has_value() && *cut.key == currentKey)
        return;

    cut.key.reset();

    if (!enabled) {
        cut.contours.reset();
        // Non-finite input is not cached; normal out-of-range results are.
        if (finiteRange)
            cut.key = currentKey;
        return;
    }

    MeshSlicingParams slicingParams;
    slicingParams.trafo = Transform3d::Identity().matrix();
    const indexed_triangle_set cutMesh = BuildHeightPreviewCutMesh(cache, zWorld);
    const Polygons polygons = slice_mesh(cutMesh, zWorld, slicingParams);

    cut.contours.reset();
    if (!polygons.empty()) {
        cut.contours.init_from(polygons, zWorld);
        cut.contours.set_color({ 1.0f, 1.0f, 1.0f, 1.0f });
    }

    // Empty polygons are a valid computed result; publish the key only after success.
    cut.key = currentKey;
}

void GLGizmoPainterBase::InvalidateHeightPreviewCaches(const char* reason) const
{
    (void) reason; // Reserved for diagnostic logging at the call sites.
    for (const auto& entry : m_heightPreviewVolumes) {
        if (!entry)
            continue;
        for (HeightPreviewCut& cut : entry->cuts)
            cut.key.reset();
        entry->sourceMesh.reset();
        entry->zIndex = TriangleSelector::HeightRangeIndex();
    }
}

void GLGizmoPainterBase::ReleaseHeightPreviewGlResources()
{
    for (const auto& entry : m_heightPreviewVolumes)
        if (entry)
            for (HeightPreviewCut& cut : entry->cuts) {
                cut.contours.reset();
                cut.key.reset();
            }
}

void GLGizmoPainterBase::DetachHeightPreviewGlResources(std::vector<unsigned int>& bufferIds)
{
    for (const auto& entry : m_heightPreviewVolumes)
        if (entry)
            for (HeightPreviewCut& cut : entry->cuts) {
                cut.contours.DetachGpuBuffers(bufferIds);
                cut.key.reset();
            }
}

BoundingBoxf3 GLGizmoPainterBase::bounding_box() const
{
    BoundingBoxf3 ret;
    const Selection& selection = m_parent.get_selection();
    const Selection::IndicesList& idxs = selection.get_volume_idxs();
    for (unsigned int i : idxs) {
        const GLVolume* volume = selection.get_volume(i);
        if (!volume->is_modifier)
            ret.merge(volume->transformed_convex_hull_bounding_box());
    }
    return ret;
}

bool GLGizmoPainterBase::is_mesh_point_clipped(const Vec3d& point, const Transform3d& trafo) const
{
    if (m_c->object_clipper()->get_position() == 0.)
        return false;

    auto sel_info = m_c->selection_info();
    Vec3d transformed_point =  trafo * point;
    transformed_point(2) += sel_info->get_sla_shift();
    return m_c->object_clipper()->get_clipping_plane()->is_point_clipped(transformed_point);
}

// Interpolate points between the previous and current mouse positions, which are then projected onto the object.
// Returned projected mouse positions are grouped by mesh_idx. It may contain multiple std::vector<GLGizmoPainterBase::ProjectedMousePosition>
// with the same mesh_idx, but all items in std::vector<GLGizmoPainterBase::ProjectedMousePosition> always have the same mesh_idx.
std::vector<std::vector<GLGizmoPainterBase::ProjectedMousePosition>> GLGizmoPainterBase::get_projected_mouse_positions(const Vec2d &mouse_position, const double resolution, const std::vector<Transform3d> &trafo_matrices) const
{
    // List of mouse positions that will be used as seeds for painting.
    std::vector<Vec2d> mouse_positions{mouse_position};
    if (m_last_mouse_click != Vec2d::Zero()) {
        // In case current mouse position is far from the last one,
        // add several positions from between into the list, so there
        // are no gaps in the painted region.
        if (size_t patches_in_between = size_t((mouse_position - m_last_mouse_click).norm() / resolution); patches_in_between > 0) {
            const Vec2d diff = (m_last_mouse_click - mouse_position) / (patches_in_between + 1);
            for (size_t patch_idx = 1; patch_idx <= patches_in_between; ++patch_idx)
                mouse_positions.emplace_back(mouse_position + patch_idx * diff);
            mouse_positions.emplace_back(m_last_mouse_click);
        }
    }

    const Camera                       &camera = wxGetApp().plater()->get_camera();
    std::vector<ProjectedMousePosition> mesh_hit_points;
    mesh_hit_points.reserve(mouse_positions.size());

    // In mesh_hit_points only the last item could have mesh_id == -1, any other items mustn't.
    for (const Vec2d &mp : mouse_positions) {
        update_raycast_cache(mp, camera, trafo_matrices);
        mesh_hit_points.push_back({m_rr.hit, m_rr.mesh_id, m_rr.facet});
        if (m_rr.mesh_id == -1)
            break;
    }

    // Divide mesh_hit_points into groups with the same mesh_idx. It may contain multiple groups with the same mesh_idx.
    std::vector<std::vector<ProjectedMousePosition>> mesh_hit_points_by_mesh;
    for (size_t prev_mesh_hit_point = 0, curr_mesh_hit_point = 0; curr_mesh_hit_point < mesh_hit_points.size(); ++curr_mesh_hit_point) {
        size_t next_mesh_hit_point = curr_mesh_hit_point + 1;
        if (next_mesh_hit_point >= mesh_hit_points.size() || mesh_hit_points[curr_mesh_hit_point].mesh_idx != mesh_hit_points[next_mesh_hit_point].mesh_idx) {
            mesh_hit_points_by_mesh.emplace_back();
            mesh_hit_points_by_mesh.back().insert(mesh_hit_points_by_mesh.back().end(), mesh_hit_points.begin() + int(prev_mesh_hit_point), mesh_hit_points.begin() + int(next_mesh_hit_point));
            prev_mesh_hit_point = next_mesh_hit_point;
        }
    }

    auto on_same_facet = [](std::vector<ProjectedMousePosition> &hit_points) -> bool {
        for (const ProjectedMousePosition &mesh_hit_point : hit_points)
            if (mesh_hit_point.facet_idx != hit_points.front().facet_idx)
                return false;
        return true;
    };

    struct Plane
    {
        Vec3d origin;
        Vec3d first_axis;
        Vec3d second_axis;
    };
    auto find_plane = [](std::vector<ProjectedMousePosition> &hit_points) -> std::optional<Plane> {
        assert(hit_points.size() >= 3);
        for (size_t third_idx = 2; third_idx < hit_points.size(); ++third_idx) {
            const Vec3d &first_point  = hit_points[third_idx - 2].mesh_hit.cast<double>();
            const Vec3d &second_point = hit_points[third_idx - 1].mesh_hit.cast<double>();
            const Vec3d &third_point  = hit_points[third_idx].mesh_hit.cast<double>();

            const Vec3d  first_vec    = first_point - second_point;
            const Vec3d  second_vec   = third_point - second_point;

            // If three points aren't collinear, then there exists only one plane going through all points.
            if (first_vec.cross(second_vec).squaredNorm() > sqr(EPSILON)) {
                const Vec3d first_axis_vec_n = first_vec.normalized();
                // Make second_vec perpendicular to first_axis_vec_n using Gram–Schmidt orthogonalization process
                const Vec3d second_axis_vec_n = (second_vec - (first_vec.dot(second_vec) / first_vec.dot(first_vec)) * first_vec).normalized();
                return Plane{second_point, first_axis_vec_n, second_axis_vec_n};
            }
        }

        return std::nullopt;
    };

    for(std::vector<ProjectedMousePosition> &hit_points : mesh_hit_points_by_mesh) {
        assert(!hit_points.empty());
        if (hit_points.back().mesh_idx == -1)
            break;

        if (hit_points.size() <= 2)
            continue;

        if (on_same_facet(hit_points)) {
            hit_points = {hit_points.front(), hit_points.back()};
        } else if (std::optional<Plane> plane = find_plane(hit_points); plane) {
            Polyline polyline;
            polyline.points.reserve(hit_points.size());
            // Project hit_points into its plane to simplified them in the next step.
            for (auto &hit_point : hit_points) {
                const Vec3d &point  = hit_point.mesh_hit.cast<double>();
                const double x_cord = plane->first_axis.dot(point - plane->origin);
                const double y_cord = plane->second_axis.dot(point - plane->origin);
                polyline.points.emplace_back(scale_(x_cord), scale_(y_cord));
            }

            polyline.simplify(scale_(m_cursor_radius) / 10.);

            const int                           mesh_idx = hit_points.front().mesh_idx;
            std::vector<ProjectedMousePosition> new_hit_points;
            new_hit_points.reserve(polyline.points.size());
            // Project 2D simplified hit_points beck to 3D.
            for (const Point &point : polyline.points) {
                const double x_cord        = unscale<double>(point.x());
                const double y_cord        = unscale<double>(point.y());
                const Vec3d  new_hit_point = plane->origin + x_cord * plane->first_axis + y_cord * plane->second_axis;
                const int    facet_idx     = m_c->raycaster()->raycasters()[mesh_idx]->get_closest_facet(new_hit_point.cast<float>());
                new_hit_points.push_back({new_hit_point.cast<float>(), mesh_idx, size_t(facet_idx)});
            }

            hit_points = new_hit_points;
        } else {
            hit_points = {hit_points.front(), hit_points.back()};
        }
    }

    return mesh_hit_points_by_mesh;
}

TriangleSelector::HeightRangeIndex& GLGizmoPainterBase::EnsureHeightRangeIndex(
    size_t meshIndex, const TriangleMesh& mesh, const Transform3d& effectiveTransform)
{
    if (m_heightRangeIndices.size() <= meshIndex)
        m_heightRangeIndices.resize(meshIndex + 1);

    TriangleSelector::HeightRangeIndex& heightRangeIndex = m_heightRangeIndices[meshIndex];
    if (!heightRangeIndex.Matches(mesh, effectiveTransform))
        heightRangeIndex.Build(mesh, effectiveTransform);
    return heightRangeIndex;
}

// Following function is called from GLCanvas3D to inform the gizmo about a mouse/keyboard event.
// The gizmo has an opportunity to react - if it does, it should return true so that the Canvas3D is
// aware that the event was reacted to and stops trying to make different sense of it. If the gizmo
// concludes that the event was not intended for it, it should return false.
bool GLGizmoPainterBase::gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    Vec2d _mouse_position = mouse_position;
    if (action == SLAGizmoEventType::MouseWheelUp
     || action == SLAGizmoEventType::MouseWheelDown) {
        if (control_down) {
            //BBS
            if (m_tool_type == ToolType::BRUSH && m_cursor_type == TriangleSelector::CursorType::HEIGHT_RANGE) {
                m_cursor_height = action == SLAGizmoEventType::MouseWheelDown ? std::max(m_cursor_height - this->get_cursor_height_step(), this->get_cursor_height_min()) :
                                                                                std::min(m_cursor_height + this->get_cursor_height_step(), this->get_cursor_height_max());
                m_parent.set_as_dirty();
                return true;
            }

            if (m_tool_type == ToolType::BRUSH && (m_cursor_type == TriangleSelector::CursorType::SPHERE || m_cursor_type == TriangleSelector::CursorType::CIRCLE)) {
                m_cursor_radius = action == SLAGizmoEventType::MouseWheelDown ? std::max(m_cursor_radius - this->get_cursor_radius_step(), this->get_cursor_radius_min()) :
                                                                                std::min(m_cursor_radius + this->get_cursor_radius_step(), this->get_cursor_radius_max());
                m_parent.set_as_dirty();
                return true;
            }

            if (m_tool_type == ToolType::BUCKET_FILL || m_tool_type == ToolType::SMART_FILL) {
                if (m_tool_type == ToolType::BUCKET_FILL && m_smart_fill_angle < 0.f)
                    return true;

                m_smart_fill_angle = action == SLAGizmoEventType::MouseWheelDown ? std::max(m_smart_fill_angle - SmartFillAngleStep, SmartFillAngleMin)
                                                                                 : std::min(m_smart_fill_angle + SmartFillAngleStep, SmartFillAngleMax);
                m_parent.set_as_dirty();
                FinishPaintingInteraction();
                if (m_rr.mesh_id != -1) {
                    const Selection     &selection                 = m_parent.get_selection();
                    const ModelObject   *mo                        = m_c->selection_info()->model_object();
                    const ModelInstance *mi                        = mo->instances[selection.get_instance_idx()];
                    const Transform3d   trafo_matrix_not_translate = m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
                        mi->get_assemble_transformation().get_matrix_no_offset() * mo->volumes[m_rr.mesh_id]->get_matrix_no_offset() :
                        mi->get_transformation().get_matrix_no_offset() * mo->volumes[m_rr.mesh_id]->get_matrix_no_offset();
                    const Transform3d   trafo_matrix = m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
                        mi->get_assemble_transformation().get_matrix() * mo->volumes[m_rr.mesh_id]->get_matrix() :
                        mi->get_transformation().get_matrix() * mo->volumes[m_rr.mesh_id]->get_matrix();
                    const TriangleSelector::ClippingPlane& clippingPlane =
                        get_clipping_plane_in_volume_coordinates(trafo_matrix);
                    if (m_tool_type == ToolType::BUCKET_FILL)
                        m_triangle_selectors[m_rr.mesh_id]->bucket_fill_select_triangles(
                            m_rr.hit, static_cast<int>(m_rr.facet), clippingPlane, m_smart_fill_angle, true, true);
                    else
                        m_triangle_selectors[m_rr.mesh_id]->seed_fill_select_triangles(
                            m_rr.hit, static_cast<int>(m_rr.facet), trafo_matrix_not_translate, clippingPlane,
                            m_smart_fill_angle,
                            m_paint_on_overhangs_only ? m_highlight_by_angle_threshold_deg : 0.f, true);
                    m_triangle_selectors[m_rr.mesh_id]->request_update_render_data();
                    m_seed_fill_last_mesh_id = m_rr.mesh_id;
                }
                return true;
            }

            if (m_tool_type == ToolType::GAP_FILL) {
                TriangleSelectorPatch::gap_area = action == SLAGizmoEventType::MouseWheelDown ?
                    std::max(TriangleSelectorPatch::gap_area - TriangleSelectorPatch::GapAreaStep, TriangleSelectorPatch::GapAreaMin) :
                    std::min(TriangleSelectorPatch::gap_area + TriangleSelectorPatch::GapAreaStep, TriangleSelectorPatch::GapAreaMax);
                m_parent.set_as_dirty();
                return true;
            }
        }
        else if (alt_down) {
            // BBS
            double pos = m_c->object_clipper()->get_position();
            pos = action == SLAGizmoEventType::MouseWheelDown
                      ? std::max(0., pos - 0.01)
                      : std::min(1., pos + 0.01);
            m_c->object_clipper()->set_position_by_ratio(pos, true);
            return true;
        }
    }

    if (action == SLAGizmoEventType::ResetClippingPlane) {
        m_c->object_clipper()->set_position_by_ratio(-1., false);
        return true;
    }

    if (action == SLAGizmoEventType::LeftDown
     || action == SLAGizmoEventType::RightDown
    || (action == SLAGizmoEventType::Dragging && m_button_down != Button::None)) {

        if (m_triangle_selectors.empty())
            return false;

        EnforcerBlockerType new_state = EnforcerBlockerType::NONE;
        // BBS
        if (action == SLAGizmoEventType::Dragging) {
            if (m_button_down == Button::Right && this->get_right_button_state_type() == EnforcerBlockerType(-1))
                return false;
        }
        else {
            if (action == SLAGizmoEventType::RightDown && this->get_right_button_state_type() == EnforcerBlockerType(-1))
                return false;
        }

        if (! shift_down) {
            if (action == SLAGizmoEventType::Dragging)
                new_state = m_button_down == Button::Left ? this->get_left_button_state_type() : this->get_right_button_state_type();
            else
                new_state = action == SLAGizmoEventType::LeftDown ? this->get_left_button_state_type() : this->get_right_button_state_type();
        }

        const Camera        &camera                      = wxGetApp().plater()->get_camera();
        const Selection     &selection                   = m_parent.get_selection();
        const ModelObject   *mo                          = m_c->selection_info()->model_object();
        const ModelInstance *mi                          = mo->instances[selection.get_instance_idx()];
        Transform3d   instance_trafo = m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
            mi->get_assemble_transformation().get_matrix() :
            mi->get_transformation().get_matrix();
        Transform3d   instance_trafo_not_translate = m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
            mi->get_assemble_transformation().get_matrix_no_offset() :
            mi->get_transformation().get_matrix_no_offset();
        std::vector<const ModelVolume*> part_volumes;

        // Precalculate transformations of individual meshes.
        std::vector<Transform3d> trafo_matrices;
        std::vector<Transform3d> trafo_matrices_not_translate;
        for (const ModelVolume *mv : mo->volumes)
            if (mv->is_model_part()) {
                if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
                    Transform3d temp = instance_trafo * mv->get_matrix();
                    temp.translate(mv->get_transformation().get_offset() * (GLVolume::explosion_ratio - 1.0) + mi->get_offset_to_assembly() * (GLVolume::explosion_ratio - 1.0));
                    trafo_matrices.emplace_back(temp);
                }
                else {
                    trafo_matrices.emplace_back(instance_trafo* mv->get_matrix());
                }
                trafo_matrices_not_translate.emplace_back(instance_trafo_not_translate * mv->get_matrix_no_offset());
                part_volumes.push_back(mv);
            }

        // BBS
        if (m_tool_type == ToolType::BRUSH && m_cursor_type == TriangleSelector::CursorType::HEIGHT_RANGE)
        {
            m_last_mouse_click = Vec2d::Zero();

            update_raycast_cache(_mouse_position, camera, trafo_matrices);
            assert(part_volumes.size() == trafo_matrices.size());
            assert(part_volumes.size() == trafo_matrices_not_translate.size());
            assert(part_volumes.size() == m_triangle_selectors.size());
            size_t volumeCount = std::min(part_volumes.size(), trafo_matrices.size());
            volumeCount = std::min(volumeCount, trafo_matrices_not_translate.size());
            volumeCount = std::min(volumeCount, m_triangle_selectors.size());
            if (m_rr.mesh_id < 0 || static_cast<size_t>(m_rr.mesh_id) >= volumeCount)
            {
                // Leaving the object breaks the sweep, so re-entering does not fill the gap.
                _heightRangeLastBottomZ.reset();
                return true;
            }

            if (m_button_down == Button::None)
            {
                m_button_down = action == SLAGizmoEventType::LeftDown ? Button::Left : Button::Right;
                m_strokeHasCommittedState = false;
                _heightRangeLastBottomZ.reset();
            }

            const size_t hitMeshIndex = static_cast<size_t>(m_rr.mesh_id);
            const Vec3d worldHit = trafo_matrices[hitMeshIndex] * m_rr.hit.cast<double>();
            const float hitZWorld = static_cast<float>(worldHit.z());
            // Sweep from the previous event's band to this one so fast drags leave no gaps.
            float zBottomWorld = hitZWorld;
            float zTopWorld = hitZWorld + m_cursor_height;
            if (_heightRangeLastBottomZ.has_value())
            {
                zBottomWorld = std::min(zBottomWorld, *_heightRangeLastBottomZ);
                zTopWorld = std::max(zTopWorld, *_heightRangeLastBottomZ + m_cursor_height);
            }
            const float sweepHeight = zTopWorld - zBottomWorld;
            _heightRangeLastBottomZ = hitZWorld;

            for (size_t meshIndex = 0; meshIndex < volumeCount; ++meshIndex)
            {
                const TriangleMesh& mesh = part_volumes[meshIndex]->mesh();
                TriangleSelector::HeightRangeIndex& heightRangeIndex =
                    EnsureHeightRangeIndex(meshIndex, mesh, trafo_matrices[meshIndex]);
                const std::vector<uint32_t>& sourceRoots = heightRangeIndex.Query(zBottomWorld, zTopWorld);
                if (sourceRoots.empty())
                    continue;

                const Transform3d& transform = trafo_matrices[meshIndex];
                const Vec3f cameraPosition = (transform.inverse() * camera.get_position()).cast<float>();
                const TriangleSelector::ClippingPlane clippingPlane = get_clipping_plane_in_volume_coordinates(transform);
                std::unique_ptr<TriangleSelector::HeightRange> cursor = std::make_unique<TriangleSelector::HeightRange>(
                    zBottomWorld, cameraPosition, sweepHeight, transform, clippingPlane);

                TriangleSelector::HeightRangeSelectionOptions options;
                options.state = new_state;
                options.transformNoTranslate = trafo_matrices_not_translate[meshIndex];
                options.triangleSplitting = m_triangle_splitting_enabled;
                options.highlightByAngleDeg = m_paint_on_overhangs_only ? m_highlight_by_angle_threshold_deg : 0.f;
                // The index was built with the same transform as the cursor, so its spans are valid here.
                options.rootZSpans = &heightRangeIndex.GetRootZSpans();

                TriangleSelectorGUI& triangleSelector = *m_triangle_selectors[meshIndex];
                const uint64_t stateRevisionBefore = triangleSelector.GetStateRevision();
                triangleSelector.SelectHeightRange(sourceRoots, std::move(cursor), options);
                m_strokeHasCommittedState |= triangleSelector.GetStateRevision() != stateRevisionBefore;
                triangleSelector.request_update_render_data(true);
            }

            m_last_mouse_click = _mouse_position;

            return true;
        }

        if (action == SLAGizmoEventType::Dragging && m_tool_type == ToolType::BRUSH ){
            if(m_vertical_only)
                _mouse_position.x() = m_last_mouse_click.x();
            else if(m_horizontal_only)
                _mouse_position.y() = m_last_mouse_click.y();
        }
        
        std::vector<std::vector<ProjectedMousePosition>> projected_mouse_positions_by_mesh = get_projected_mouse_positions(_mouse_position, 1., trafo_matrices);
        m_last_mouse_click = Vec2d::Zero(); // only actual hits should be saved
        const bool pointerTool = m_tool_type == ToolType::BRUSH && m_cursor_type == TriangleSelector::CursorType::POINTER;
        if (pointerTool)
            for (const std::unique_ptr<TriangleSelectorGUI>& triangleSelector : m_triangle_selectors)
                triangleSelector->SetPointerPreviewEnabled(true);
        else
            for (const std::unique_ptr<TriangleSelectorGUI>& triangleSelector : m_triangle_selectors)
                triangleSelector->SetPointerPreviewEnabled(false);

        for (const std::vector<ProjectedMousePosition> &projected_mouse_positions : projected_mouse_positions_by_mesh) {
            assert(!projected_mouse_positions.empty());
            const int  mesh_idx                = projected_mouse_positions.front().mesh_idx;
            const bool dragging_while_painting = (action == SLAGizmoEventType::Dragging && m_button_down != Button::None);

            // The mouse button click detection is enabled when there is a valid hit.
            // Missing the object entirely
            // shall not capture the mouse.
            if (mesh_idx != -1)
                if (m_button_down == Button::None) {
                    m_button_down = ((action == SLAGizmoEventType::LeftDown) ? Button::Left : Button::Right);
                    m_strokeHasCommittedState = false;
                }

            // In case we have no valid hit, we can return. The event will be stopped when
            // dragging while painting (to prevent scene rotations and moving the object)
            if (mesh_idx == -1) {
                if (pointerTool)
                    for (const std::unique_ptr<TriangleSelectorGUI>& triangleSelector : m_triangle_selectors)
                        triangleSelector->ClearPointerPreview();
                return dragging_while_painting;
            }

            assert(mesh_idx < int(m_triangle_selectors.size()));
            if (pointerTool) {
                // Rebuild the preview at the click: no move event may have restored it after the last stroke.
                if (action != SLAGizmoEventType::Dragging)
                {
                    const ProjectedMousePosition& clickPosition = projected_mouse_positions.front();
                    m_triangle_selectors[mesh_idx]->UpdatePointerPreview(
                        clickPosition.mesh_hit, static_cast<int>(clickPosition.facet_idx));
                }

                for (const ProjectedMousePosition& projectedMousePosition : projected_mouse_positions) {
                    assert(projectedMousePosition.mesh_idx == mesh_idx);

                    for (const std::unique_ptr<TriangleSelectorGUI>& triangleSelector : m_triangle_selectors) {
                        if (triangleSelector->CommitPointerPreview(new_state, TriangleSelector::CleanupMode::Deferred)) {
                            triangleSelector->request_update_render_data(true);
                            m_strokeHasCommittedState = true;
                        }
                    }

                    for (size_t selectorIndex = 0; selectorIndex < m_triangle_selectors.size(); ++selectorIndex)
                        if (selectorIndex != static_cast<size_t>(mesh_idx))
                            m_triangle_selectors[selectorIndex]->ClearPointerPreview();
                    m_triangle_selectors[mesh_idx]->UpdatePointerPreview(
                        projectedMousePosition.mesh_hit, static_cast<int>(projectedMousePosition.facet_idx));
                }
                m_seed_fill_last_mesh_id = -1;
            } else {
                const Transform3d& trafoMatrix = trafo_matrices[mesh_idx];
                const Transform3d& trafoMatrixNotTranslate = trafo_matrices_not_translate[mesh_idx];
                const Vec3f cameraPosition = (trafoMatrix.inverse() * camera.get_position()).cast<float>();
                const TriangleSelector::ClippingPlane& clippingPlane = get_clipping_plane_in_volume_coordinates(trafoMatrix);

                if (m_tool_type == ToolType::SMART_FILL || m_tool_type == ToolType::BUCKET_FILL) {
                    for (const ProjectedMousePosition& projectedMousePosition : projected_mouse_positions) {
                        assert(projectedMousePosition.mesh_idx == mesh_idx);
                        const Vec3f meshHit = projectedMousePosition.mesh_hit;
                        const int facetIndex = static_cast<int>(projectedMousePosition.facet_idx);
                        const TriangleSelector::CleanupMode cleanupMode = m_tool_type == ToolType::BUCKET_FILL ?
                            TriangleSelector::CleanupMode::Deferred : TriangleSelector::CleanupMode::Immediate;
                        TriangleSelectorGUI& triangleSelector = *m_triangle_selectors[mesh_idx];
                        // Rebuild the preview at the click: no move event may have restored it after the last stroke.
                        if (action != SLAGizmoEventType::Dragging)
                        {
                            if (m_tool_type == ToolType::SMART_FILL)
                                triangleSelector.seed_fill_select_triangles(
                                    meshHit, facetIndex, trafoMatrixNotTranslate, clippingPlane, m_smart_fill_angle,
                                    m_paint_on_overhangs_only ? m_highlight_by_angle_threshold_deg : 0.f, false);
                            else
                                triangleSelector.bucket_fill_select_triangles(
                                    meshHit, facetIndex, clippingPlane, m_smart_fill_angle, true, false);
                        }
                        const uint64_t stateRevisionBefore = triangleSelector.GetStateRevision();
                        triangleSelector.seed_fill_apply_on_triangles(new_state, cleanupMode);
                        m_strokeHasCommittedState |= triangleSelector.GetStateRevision() != stateRevisionBefore;
                        if (m_tool_type == ToolType::SMART_FILL)
                            triangleSelector.seed_fill_select_triangles(
                                meshHit, facetIndex, trafoMatrixNotTranslate, clippingPlane, m_smart_fill_angle,
                                m_paint_on_overhangs_only ? m_highlight_by_angle_threshold_deg : 0.f, true);
                        else
                            triangleSelector.bucket_fill_select_triangles(
                                meshHit, facetIndex, clippingPlane, m_smart_fill_angle, true, true);

                        m_seed_fill_last_mesh_id = -1;
                    }
                } else if (m_tool_type == ToolType::BRUSH) {
                    assert(m_cursor_type == TriangleSelector::CursorType::CIRCLE ||
                           m_cursor_type == TriangleSelector::CursorType::SPHERE);
                    const uint64_t stateRevisionBefore = m_triangle_selectors[mesh_idx]->GetStateRevision();

                    if (projected_mouse_positions.size() == 1) {
                        const ProjectedMousePosition& firstPosition = projected_mouse_positions.front();
                        std::unique_ptr<TriangleSelector::Cursor> cursor = TriangleSelector::SinglePointCursor::cursor_factory(
                            firstPosition.mesh_hit, cameraPosition, m_cursor_radius, m_cursor_type, trafoMatrix, clippingPlane);
                        m_triangle_selectors[mesh_idx]->select_patch(
                            static_cast<int>(firstPosition.facet_idx), std::move(cursor), new_state, trafoMatrixNotTranslate,
                            m_triangle_splitting_enabled,
                            m_paint_on_overhangs_only ? m_highlight_by_angle_threshold_deg : 0.f);
                    } else {
                        for (auto firstPosition = projected_mouse_positions.cbegin();
                             firstPosition != projected_mouse_positions.cend() - 1; ++firstPosition) {
                            const auto secondPosition = firstPosition + 1;
                            std::unique_ptr<TriangleSelector::Cursor> cursor = TriangleSelector::DoublePointCursor::cursor_factory(
                                firstPosition->mesh_hit, secondPosition->mesh_hit, cameraPosition, m_cursor_radius,
                                m_cursor_type, trafoMatrix, clippingPlane);
                            m_triangle_selectors[mesh_idx]->select_patch(
                                static_cast<int>(firstPosition->facet_idx), std::move(cursor), new_state, trafoMatrixNotTranslate,
                                m_triangle_splitting_enabled,
                                m_paint_on_overhangs_only ? m_highlight_by_angle_threshold_deg : 0.f);
                        }
                    }
                    m_strokeHasCommittedState |= m_triangle_selectors[mesh_idx]->GetStateRevision() != stateRevisionBefore;
                }
            }

            m_triangle_selectors[mesh_idx]->request_update_render_data(!pointerTool);

            m_last_mouse_click = _mouse_position;
        }

        return true;
    }

    if (action == SLAGizmoEventType::Moving && (m_tool_type == ToolType::SMART_FILL || m_tool_type == ToolType::BUCKET_FILL || (m_tool_type == ToolType::BRUSH && m_cursor_type == TriangleSelector::CursorType::POINTER))) {
        if (m_triangle_selectors.empty())
            return false;

        const Camera        &camera                       = wxGetApp().plater()->get_camera();
        const Selection     &selection                    = m_parent.get_selection();
        const ModelObject   *mo                           = m_c->selection_info()->model_object();
        const ModelInstance *mi                           = mo->instances[selection.get_instance_idx()];
        const Transform3d    instance_trafo = m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
            mi->get_assemble_transformation().get_matrix() :
            mi->get_transformation().get_matrix();
        const Transform3d    instance_trafo_not_translate = m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
            mi->get_assemble_transformation().get_matrix_no_offset() :
            mi->get_transformation().get_matrix_no_offset();

        // Precalculate transformations of individual meshes.
        std::vector<Transform3d> trafo_matrices;
        std::vector<Transform3d> trafo_matrices_not_translate;
        for (const ModelVolume *mv : mo->volumes)
            if (mv->is_model_part()) {
                if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
                    Transform3d temp = instance_trafo * mv->get_matrix();
                    temp.translate(mv->get_transformation().get_offset() * (GLVolume::explosion_ratio - 1.0) + mi->get_offset_to_assembly() * (GLVolume::explosion_ratio - 1.0));
                    trafo_matrices.emplace_back(temp);
                }
                else {
                    trafo_matrices.emplace_back(instance_trafo * mv->get_matrix());
                }
                trafo_matrices_not_translate.emplace_back(instance_trafo_not_translate * mv->get_matrix_no_offset());
            }

        // Now "click" into all the prepared points and spill paint around them.
        update_raycast_cache(_mouse_position, camera, trafo_matrices);

        const bool pointerTool = m_tool_type == ToolType::BRUSH && m_cursor_type == TriangleSelector::CursorType::POINTER;
        if (pointerTool) {
            for (size_t selectorIndex = 0; selectorIndex < m_triangle_selectors.size(); ++selectorIndex) {
                TriangleSelectorGUI& triangleSelector = *m_triangle_selectors[selectorIndex];
                triangleSelector.SetPointerPreviewEnabled(true);
                if (m_rr.mesh_id < 0 || selectorIndex != static_cast<size_t>(m_rr.mesh_id))
                    triangleSelector.ClearPointerPreview();
            }

            if (m_rr.mesh_id >= 0 && m_rr.mesh_id < static_cast<int>(m_triangle_selectors.size()))
                m_triangle_selectors[m_rr.mesh_id]->UpdatePointerPreview(m_rr.hit, static_cast<int>(m_rr.facet));

            m_seed_fill_last_mesh_id = -1;
            m_parent.set_as_dirty();
            return m_rr.mesh_id >= 0;
        }

        for (const std::unique_ptr<TriangleSelectorGUI>& triangleSelector : m_triangle_selectors)
            triangleSelector->SetPointerPreviewEnabled(false);

        auto seed_fill_unselect_all = [this]() {
            for (auto &triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        };

        if (m_rr.mesh_id == -1) {
            // Clean selected by seed fill for all triangles in all meshes when a mouse isn't pointing on any mesh.
            seed_fill_unselect_all();
            m_seed_fill_last_mesh_id = -1;

            // In case we have no valid hit, we can return.
            return false;
        }

        // The mouse moved from one object's volume to another one. So it is needed to unselect all triangles selected by seed fill.
        if(m_rr.mesh_id != m_seed_fill_last_mesh_id)
            seed_fill_unselect_all();

        const Transform3d &trafo_matrix = trafo_matrices[m_rr.mesh_id];
        const Transform3d &trafo_matrix_not_translate = trafo_matrices_not_translate[m_rr.mesh_id];

        assert(m_rr.mesh_id < int(m_triangle_selectors.size()));
        const TriangleSelector::ClippingPlane &clp = this->get_clipping_plane_in_volume_coordinates(trafo_matrix);
        if (m_tool_type == ToolType::SMART_FILL)
            m_triangle_selectors[m_rr.mesh_id]->seed_fill_select_triangles(m_rr.hit, int(m_rr.facet), trafo_matrix_not_translate, clp, m_smart_fill_angle,
                                                                           m_paint_on_overhangs_only ? m_highlight_by_angle_threshold_deg : 0.f);
        else if (m_tool_type == ToolType::BUCKET_FILL)
            // BBS: add infill_angle parameter
            m_triangle_selectors[m_rr.mesh_id]->bucket_fill_select_triangles(m_rr.hit, int(m_rr.facet), clp, m_smart_fill_angle, true);
        m_triangle_selectors[m_rr.mesh_id]->request_update_render_data();
        m_seed_fill_last_mesh_id = m_rr.mesh_id;
        return true;
    }

    if ((action == SLAGizmoEventType::LeftUp || action == SLAGizmoEventType::RightUp)
      && m_button_down != Button::None) {
        const wxString actionName = handle_snapshot_action_name(shift_down, m_button_down);
        FinalizePaintingStroke(actionName);
        // Finalizing cleared the hover preview; rebuild it here since macOS/Linux send no move after mouse-up.
        gizmo_event(SLAGizmoEventType::Moving, mouse_position, shift_down, alt_down, control_down);
        return true;
    }

    return false;
}

bool GLGizmoPainterBase::on_mouse(const wxMouseEvent &mouse_event)
{
    // wxCoord == int --> wx/types.h
    Vec2i32 mouse_coord(mouse_event.GetX(), mouse_event.GetY());
    Vec2d mouse_pos = mouse_coord.cast<double>();

    if (mouse_event.Moving()) {
        gizmo_event(SLAGizmoEventType::Moving, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), false);
        // Cursor and hover previews are drawn inside the cached scene, so a move must redraw the scene.
        m_parent.set_as_dirty();
        return false;
    }

    // when control is down we allow scene pan and rotation even when clicking
    // over some object
    bool control_down           = mouse_event.CmdDown();
    bool grabber_contains_mouse = (get_hover_id() != -1);

    const Selection &selection = m_parent.get_selection();
    int selected_object_idx = selection.get_object_idx();
    if (mouse_event.LeftDown())
    {
        if ((!control_down || grabber_contains_mouse) &&            
            gizmo_event(SLAGizmoEventType::LeftDown, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), false))
        {
            // the gizmo got the event and took some action, there is no need
            // to do anything more
            CapturePaintingMouse();
            m_parent.set_as_dirty();
            return true;
        }
    }
    else if (mouse_event.RightDown())
    {
        if (!control_down && selected_object_idx != -1 &&
            gizmo_event(SLAGizmoEventType::RightDown, mouse_pos, false, false, false))
        {
            // event was taken care of
            CapturePaintingMouse();
            m_parent.set_as_dirty();
            return true;
        }
    } else if (mouse_event.Dragging()) {
        if (m_parent.get_move_volume_id() != -1)
            // don't allow dragging objects with the Sla gizmo on
            return true;
        if (!control_down && gizmo_event(SLAGizmoEventType::Dragging,
                                         mouse_pos, mouse_event.ShiftDown(),
                                         mouse_event.AltDown(), false)) {
            // the gizmo got the event and took some action, no need to do
            // anything more here
            m_parent.set_as_dirty();
            return true;
        }
        if(control_down && (mouse_event.LeftIsDown() || mouse_event.RightIsDown()))
        {
            // CTRL has been pressed while already dragging -> stop current action
            if (mouse_event.LeftIsDown())
                gizmo_event(SLAGizmoEventType::LeftUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), true);
            else if (mouse_event.RightIsDown())
                gizmo_event(SLAGizmoEventType::RightUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), true);
            ReleasePaintingMouse();
            return false;
        }
    } else if (mouse_event.LeftUp()) {
        if (!m_parent.is_mouse_dragging()) {
            // in case SLA/FDM gizmo is selected, we just pass the LeftUp
            // event and stop processing - neither object moving or selecting
            // is suppressed in that case
            gizmo_event(SLAGizmoEventType::LeftUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), control_down);
            return true;
        }
    } else if (mouse_event.RightUp()) {
        if (!m_parent.is_mouse_dragging()) {
            gizmo_event(SLAGizmoEventType::RightUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), control_down);
            return true;
        }
    }
    return false;
}

void GLGizmoPainterBase::OnMouseCaptureLost()
{
    if (m_button_down == Button::None)
        return;

    const wxString actionName = handle_snapshot_action_name(false, m_button_down);
    FinalizePaintingStroke(actionName);
}

void GLGizmoPainterBase::update_raycast_cache(const Vec2d& mouse_position,
                                              const Camera& camera,
                                              const std::vector<Transform3d>& trafo_matrices) const
{
    if (m_rr.mouse_position == mouse_position) {
        // Same query as last time - the answer is already in the cache.
        return;
    }

    Vec3f normal =  Vec3f::Zero();
    Vec3f hit = Vec3f::Zero();
    size_t facet = 0;
    Vec3f closest_hit = Vec3f::Zero();
    double closest_hit_squared_distance = std::numeric_limits<double>::max();
    size_t closest_facet = 0;
    int closest_hit_mesh_id = -1;

    // Cast a ray on all meshes, pick the closest hit and save it for the respective mesh
    for (int mesh_id = 0; mesh_id < int(trafo_matrices.size()); ++mesh_id) {

        if (m_c->raycaster()->raycasters()[mesh_id]->unproject_on_mesh(
                   mouse_position,
                   trafo_matrices[mesh_id],
                   camera,
                   hit,
                   normal,
                   m_c->object_clipper()->get_clipping_plane(),
                   &facet,
                   m_parent.get_canvas_type() != GLCanvas3D::CanvasAssembleView))
        {
            // In case this hit is clipped, skip it.
            if (is_mesh_point_clipped(hit.cast<double>(), trafo_matrices[mesh_id]))
                continue;

            // Is this hit the closest to the camera so far?
            double hit_squared_distance = (camera.get_position()-trafo_matrices[mesh_id]*hit.cast<double>()).squaredNorm();
            if (hit_squared_distance < closest_hit_squared_distance) {
                closest_hit_squared_distance = hit_squared_distance;
                closest_facet = facet;
                closest_hit_mesh_id = mesh_id;
                closest_hit = hit;
            }
        }
    }

    m_rr = {mouse_position, closest_hit_mesh_id, closest_hit, closest_facet};
}

bool GLGizmoPainterBase::on_is_activable() const
{
    const Selection& selection = m_parent.get_selection();

    if (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptFFF
        || !selection.is_single_full_instance()/* || wxGetApp().get_mode() == comSimple*/)
        return false;

    // BBS
#if 0
    // Check that none of the selected volumes is outside. Only SLA auxiliaries (supports) are allowed outside.
    const Selection::IndicesList& list = selection.get_volume_idxs();
    return std::all_of(list.cbegin(), list.cend(), [&selection](unsigned int idx) { return !selection.get_volume(idx)->is_outside; });
#else
    return true;
#endif
}

bool GLGizmoPainterBase::on_is_selectable() const
{
    //BBS
    /*return (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF
         && wxGetApp().get_mode() != comSimple );*/
    return (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF);
}


CommonGizmosDataID GLGizmoPainterBase::on_get_requirements() const
{
    return CommonGizmosDataID(
                int(CommonGizmosDataID::SelectionInfo)
              | int(CommonGizmosDataID::InstancesHider)
              | int(CommonGizmosDataID::Raycaster)
              | int(CommonGizmosDataID::ObjectClipper));
}


void GLGizmoPainterBase::on_set_state()
{
    if (m_state == m_old_state)
        return;

    if (m_state == On && m_old_state != On) { // the gizmo was just turned on
        m_parent.enable_picking(false);
        on_opening();

        const Selection& selection = m_parent.get_selection();
        //Camera& camera = wxGetApp().plater()->get_camera();
        //Vec3d rotate_target = selection.get_bounding_box().center();
        //rotate_target(2) = 0.f;
        //Vec3d position = camera.get_position();
        //camera.set_target(rotate_target);
        //camera.look_at(position, rotate_target, Vec3d::UnitZ());
    }
    if (m_state == Off && m_old_state != Off) { // the gizmo was just turned Off
        m_parent.enable_picking(true);
        // Undo/redo has already replaced the model, so the active stroke must not be written back.
        if (m_parent.get_gizmos_manager().IsSerializing())
            ResetPaintingStroke();
        FinishPaintingInteraction();
        // we are actually shutting down
        on_shutdown();
        m_old_mo_id = -1;
        //m_iva.release_geometry();
        DetachTriangleSelectorGlResources();
        m_heightPreviewVolumes.clear();
        m_triangle_selectors.clear();
        m_heightRangeIndices.clear();

        //Camera& camera = wxGetApp().plater()->get_camera();
        //camera.look_at(camera.get_position(), m_previous_target, Vec3d::UnitZ());
        //camera.set_target(m_previous_target);
        //camera.recover_from_free_camera();
    }
    m_old_state = m_state;
}



void GLGizmoPainterBase::on_load(cereal::BinaryInputArchive&)
{
    // We should update the gizmo from current ModelObject, but it is not
    // possible at this point. That would require having updated selection and
    // common gizmos data, which is not done at this point. Instead, save
    // a flag to do the update in set_painter_gizmo_data, which will be called
    // soon after.
    m_schedule_update = true;
    InvalidateHeightPreviewCaches("undo_redo");
}

TriangleSelector::ClippingPlane GLGizmoPainterBase::get_clipping_plane_in_volume_coordinates(const Transform3d &trafo) const {
    const ::Slic3r::GUI::ClippingPlane *const clipping_plane = m_c->object_clipper()->get_clipping_plane();
    if (clipping_plane == nullptr || !clipping_plane->is_active())
        return {};

    const Vec3d  clp_normal = clipping_plane->get_normal();
    const double clp_offset = clipping_plane->get_offset();

    const Transform3d trafo_normal = Transform3d(trafo.linear().transpose());
    const Transform3d trafo_inv    = trafo.inverse();

    Vec3d point_on_plane             = clp_normal * clp_offset;
    Vec3d point_on_plane_transformed = trafo_inv * point_on_plane;
    Vec3d normal_transformed         = trafo_normal * clp_normal;
    auto offset_transformed          = float(point_on_plane_transformed.dot(normal_transformed));

    return TriangleSelector::ClippingPlane({float(normal_transformed.x()), float(normal_transformed.y()), float(normal_transformed.z()), offset_transformed});
}

ColorRGBA TriangleSelectorGUI::enforcers_color = {0.5f, 1.f, 0.5f, 1.f};
ColorRGBA TriangleSelectorGUI::blockers_color  = {1.f, 0.5f, 0.5f, 1.f};

void TriangleSelectorGUI::SetPointerPreviewEnabled(bool enabled)
{
    if (m_pointerPreviewEnabled == enabled)
        return;

    m_pointerPreviewEnabled = enabled;
    m_pointerPreviewLeaf = -1;
    m_pointerPreviewVertices.reset();
    m_pointerPreviewDirty = enabled;
    InvalidateSeedFillContourCache();
    m_update_render_data = true;
}

bool TriangleSelectorGUI::UpdatePointerPreview(const Vec3f& hit, int facetIndex)
{
    if (!m_pointerPreviewEnabled)
        SetPointerPreviewEnabled(true);

    if (facetIndex < 0 || facetIndex >= m_orig_size_indices)
        return ClearPointerPreview();

    const int leafIndex = select_unsplit_triangle(hit, facetIndex);
    if (leafIndex < 0)
        return ClearPointerPreview();
    if (leafIndex == m_pointerPreviewLeaf && m_pointerPreviewVertices.has_value())
        return false;

    const std::optional<std::array<Vec3f, 3>> vertices = GetLeafVertices(leafIndex);
    if (!vertices.has_value())
        return ClearPointerPreview();

    m_pointerPreviewLeaf = leafIndex;
    m_pointerPreviewVertices = vertices;
    m_pointerPreviewDirty = true;
    m_update_render_data = true;
    return true;
}

bool TriangleSelectorGUI::CommitPointerPreview(EnforcerBlockerType state, CleanupMode cleanupMode)
{
    if (!m_pointerPreviewEnabled || m_pointerPreviewLeaf < 0)
        return false;

    return SetLeafState(m_pointerPreviewLeaf, state, cleanupMode);
}

bool TriangleSelectorGUI::ClearPointerPreview()
{
    if (m_pointerPreviewLeaf == -1 && !m_pointerPreviewVertices.has_value())
        return false;

    m_pointerPreviewLeaf = -1;
    m_pointerPreviewVertices.reset();
    if (m_pointerPreviewEnabled) {
        m_pointerPreviewDirty = true;
        m_update_render_data = true;
    }
    return true;
}

void TriangleSelectorGUI::InvalidatePointerPreview()
{
    if (m_pointerPreviewLeaf == -1 && !m_pointerPreviewVertices.has_value())
        return;

    m_pointerPreviewLeaf = -1;
    m_pointerPreviewVertices.reset();
    if (m_pointerPreviewEnabled) {
        m_pointerPreviewDirty = true;
        m_update_render_data = true;
    }
}

void TriangleSelectorGUI::OnLeafIdentityWillChange()
{
    InvalidatePointerPreview();
}

void TriangleSelectorGUI::OnSelectorMutation(MutationKind kind, int sourceTriangle)
{
    (void) sourceTriangle;
    if (kind == MutationKind::Topology || kind == MutationKind::IndexRebuild || kind == MutationKind::FullReset) {
        InvalidatePointerPreview();
        InvalidateSeedFillContourCache();
    }
}

void TriangleSelectorGUI::InvalidateSeedFillContourCache()
{
    m_seedFillContourKey.reset();
    m_update_render_data = true;
}

TriangleSelectorGUI::~TriangleSelectorGUI()
{
#ifndef NDEBUG
    assert(!HasOwnedGlResources());
#endif
}

void TriangleSelectorGUI::ReleaseOwnedGlResources()
{
    m_iva_enforcers.reset();
    m_iva_blockers.reset();
    for (GLModel& seedFill : m_iva_seed_fills)
        seedFill.reset();
#ifdef PRUSASLICER_TRIANGLE_SELECTOR_DEBUG
    for (GLModel& model : m_varrays)
        model.reset();
#endif
    m_paint_contour.reset();
    InvalidatePointerPreview();
    InvalidateSeedFillContourCache();
}

void TriangleSelectorGUI::DetachOwnedGlResources(std::vector<unsigned int>& bufferIds)
{
    m_iva_enforcers.DetachGpuBuffers(bufferIds);
    m_iva_blockers.DetachGpuBuffers(bufferIds);
    for (GLModel& seedFill : m_iva_seed_fills)
        seedFill.DetachGpuBuffers(bufferIds);
#ifdef PRUSASLICER_TRIANGLE_SELECTOR_DEBUG
    for (GLModel& model : m_varrays)
        model.DetachGpuBuffers(bufferIds);
#endif
    m_paint_contour.DetachGpuBuffers(bufferIds);
    InvalidatePointerPreview();
    InvalidateSeedFillContourCache();
}

bool TriangleSelectorGUI::HasOwnedGlResources() const
{
    if (m_iva_enforcers.HasGpuBuffers() || m_iva_blockers.HasGpuBuffers() || m_paint_contour.HasGpuBuffers())
        return true;
    if (std::any_of(m_iva_seed_fills.begin(), m_iva_seed_fills.end(),
                    [](const GLModel& model) { return model.HasGpuBuffers(); }))
        return true;
#ifdef PRUSASLICER_TRIANGLE_SELECTOR_DEBUG
    if (std::any_of(m_varrays.begin(), m_varrays.end(),
                    [](const GLModel& model) { return model.HasGpuBuffers(); }))
        return true;
#endif
    return false;
}

ColorRGBA TriangleSelectorGUI::get_seed_fill_color(const ColorRGBA& base_color)
{
    // BBS
    return {
        base_color.r() * 1.25f < 1.f ? base_color.r() * 1.25f : 1.f,
        base_color.g() * 1.25f < 1.f ? base_color.g() * 1.25f : 1.f,
        base_color.b() * 1.25f < 1.f ? base_color.b() * 1.25f : 1.f,
        1.f};
}

void TriangleSelectorGUI::render(ImGuiWrapper* imgui, const Transform3d& matrix)
{
    if (m_update_render_data) {
        update_render_data();
        m_update_render_data = false;
    }

    auto* shader = wxGetApp().get_current_shader();
    if (! shader)
        return;
    assert(shader->get_name() == "gouraud" || shader->get_name() == "mm_gouraud");
    shader->set_uniform("use_vertex_color", false);

    for (auto iva : {std::make_pair(&m_iva_enforcers, enforcers_color),
                     std::make_pair(&m_iva_blockers, blockers_color)}) {
        iva.first->set_color(iva.second);
        iva.first->render();
    }

    for (auto& iva : m_iva_seed_fills) {
        size_t           color_idx = &iva - &m_iva_seed_fills.front();
        const ColorRGBA& color     = TriangleSelectorGUI::get_seed_fill_color(color_idx == 1 ? enforcers_color :
            color_idx == 2 ? blockers_color :
            GLVolume::NEUTRAL_COLOR);
        iva.set_color(color);
        iva.render();
    }

    render_paint_contour(matrix);

#ifdef PRUSASLICER_TRIANGLE_SELECTOR_DEBUG
    if (imgui)
        render_debug(imgui);
    else
        assert(false); // If you want debug output, pass ptr to ImGuiWrapper.
#endif
}

void TriangleSelectorGUI::update_render_data()
{
    int              enf_cnt = 0;
    int              blc_cnt = 0;
    std::vector<int> seed_fill_cnt(m_iva_seed_fills.size(), 0);

    for (auto* iva : { &m_iva_enforcers, &m_iva_blockers }) {
        iva->reset();
    }

    for (auto& iva : m_iva_seed_fills) {
        iva.reset();
    }

    GLModel::Geometry iva_enforcers_data;
    iva_enforcers_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    GLModel::Geometry iva_blockers_data;
    iva_blockers_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    std::array<GLModel::Geometry, 3> iva_seed_fills_data;
    for (auto& data : iva_seed_fills_data)
        data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };

    // small value used to offset triangles along their normal to avoid z-fighting
    static const float offset = 0.001f;

    for (const Triangle &tr : m_triangles) {
        if (!tr.valid() || tr.is_split() || (tr.get_state() == EnforcerBlockerType::NONE && !tr.is_selected_by_seed_fill()))
            continue;

        int tr_state = int(tr.get_state());
        GLModel::Geometry &iva = tr.is_selected_by_seed_fill()                   ? iva_seed_fills_data[tr_state] :
                                 tr.get_state() == EnforcerBlockerType::ENFORCER ? iva_enforcers_data :
                                                                                   iva_blockers_data;
        int                  &cnt = tr.is_selected_by_seed_fill()                   ? seed_fill_cnt[tr_state] :
                                    tr.get_state() == EnforcerBlockerType::ENFORCER ? enf_cnt :
                                                                                      blc_cnt;
        const Vec3f          &v0  = m_vertices[tr.verts_idxs[0]].v;
        const Vec3f          &v1  = m_vertices[tr.verts_idxs[1]].v;
        const Vec3f          &v2  = m_vertices[tr.verts_idxs[2]].v;
        //FIXME the normal may likely be pulled from m_triangle_selectors, but it may not be worth the effort
        // or the current implementation may be more cache friendly.
        const Vec3f           n   = (v1 - v0).cross(v2 - v1).normalized();
        // small value used to offset triangles along their normal to avoid z-fighting
        const Vec3f    offset_n   = offset * n;
        iva.add_vertex(v0 + offset_n, n);
        iva.add_vertex(v1 + offset_n, n);
        iva.add_vertex(v2 + offset_n, n);
        iva.add_triangle((unsigned int)cnt, (unsigned int)cnt + 1, (unsigned int)cnt + 2);
        cnt += 3;
    }

    if (!iva_enforcers_data.is_empty())
        m_iva_enforcers.init_from(std::move(iva_enforcers_data));
    if (!iva_blockers_data.is_empty())
        m_iva_blockers.init_from(std::move(iva_blockers_data));
    for (size_t i = 0; i < m_iva_seed_fills.size(); ++i) {
        if (!iva_seed_fills_data[i].is_empty())
            m_iva_seed_fills[i].init_from(std::move(iva_seed_fills_data[i]));
    }

    update_paint_contour();
}

// BBS
bool TrianglePatch::is_fragment() const
{
    return this->area < TriangleSelectorPatch::gap_area;
}

float TriangleSelectorPatch::gap_area = TriangleSelectorPatch::GapAreaMin;

TriangleSelectorPatch::TriangleSelectorPatch(const TriangleMesh& mesh, const std::vector<ColorRGBA> ebtColors,
                                             float edgeLimit, bool useRenderChunks)
    : TriangleSelectorGUI(mesh, edgeLimit)
    , m_ebt_colors(ebtColors)
    , m_useRenderChunks(useRenderChunks)
{
    if (m_useRenderChunks)
        BuildRenderChunkLayout();
}

TriangleSelectorPatch::~TriangleSelectorPatch()
{
#ifndef NDEBUG
    for (const RenderChunk& chunk : m_renderChunks) {
        assert(chunk.geometryVbo == 0);
        assert(chunk.colorVbo == 0);
    }
    assert(std::all_of(m_vertices_VBO_ids.begin(), m_vertices_VBO_ids.end(), [](unsigned int id) { return id == 0; }));
    assert(std::all_of(m_triangle_indices_VBO_ids.begin(), m_triangle_indices_VBO_ids.end(), [](unsigned int id) { return id == 0; }));
    assert(!HasOwnedGlResources());
#endif
}

void TriangleSelectorPatch::set_ebt_colors(const std::vector<ColorRGBA> ebtColors)
{
    if (m_ebt_colors == ebtColors)
        return;

    m_ebt_colors = ebtColors;
    m_allColorDirty = true;
    m_update_render_data = true;
    m_paint_changed = true;
}

void TriangleSelectorPatch::OnSelectorMutation(MutationKind kind, int sourceTriangle)
{
    TriangleSelectorGUI::OnSelectorMutation(kind, sourceTriangle);

    switch (kind) {
    case MutationKind::State:
        if (sourceTriangle >= 0)
            MarkRootDirty(static_cast<uint32_t>(sourceTriangle), DirtyLevel::State);
        break;
    case MutationKind::Topology:
        if (sourceTriangle >= 0)
            MarkRootDirty(static_cast<uint32_t>(sourceTriangle), DirtyLevel::Topology);
        break;
    case MutationKind::AllState:
        m_allColorDirty = true;
        break;
    case MutationKind::IndexRebuild:
        break;
    case MutationKind::FullReset:
        MarkAllChunksTopologyDirty();
        break;
    }

    m_update_render_data = true;
    m_paint_changed = true;
}

void TriangleSelectorPatch::BuildRenderChunkLayout()
{
    m_renderChunks.clear();
    m_sourceToChunk.assign(static_cast<size_t>(m_orig_size_indices), 0);
    m_rootDrawInfo.assign(static_cast<size_t>(m_orig_size_indices), RootDrawInfo());
    m_rootDirty.assign(static_cast<size_t>(m_orig_size_indices), DirtyLevel::Clean);
    m_dirtyRoots.clear();
    m_dirtyChunks.clear();
    m_renderChunksInitialized = false;
    _renderChunkWireframeLayout.reset();
    m_update_render_data = true;
    m_paint_changed = true;

    if (m_orig_size_indices <= 0 || m_mesh.its.vertices.empty()) {
        m_chunkInDirtyList.clear();
        return;
    }

    Vec3f minimum = m_mesh.its.vertices.front();
    Vec3f maximum = minimum;
    for (const stl_vertex& vertex : m_mesh.its.vertices) {
        minimum = minimum.cwiseMin(vertex);
        maximum = maximum.cwiseMax(vertex);
    }
    const Vec3f extent = maximum - minimum;

    std::vector<std::pair<uint32_t, uint32_t>> sortedRoots;
    sortedRoots.reserve(static_cast<size_t>(m_orig_size_indices));
    for (int source = 0; source < m_orig_size_indices; ++source) {
        const stl_triangle_vertex_indices& indices = m_mesh.its.indices[static_cast<size_t>(source)];
        const Vec3f centroid = (m_mesh.its.vertices[indices[0]] + m_mesh.its.vertices[indices[1]] +
                                m_mesh.its.vertices[indices[2]]) / 3.0f;
        sortedRoots.emplace_back(CalculateMortonCode(centroid, minimum, extent), static_cast<uint32_t>(source));
    }

    // Ties are broken by root index, so the order is total and the parallel sort gives the same layout.
    tbb::parallel_sort(sortedRoots.begin(), sortedRoots.end(), [](const std::pair<uint32_t, uint32_t>& left,
                                                                  const std::pair<uint32_t, uint32_t>& right) {
        return left.first != right.first ? left.first < right.first : left.second < right.second;
    });

    const size_t chunkCount = (sortedRoots.size() + RENDER_CHUNK_ROOT_TARGET - 1) / RENDER_CHUNK_ROOT_TARGET;
    m_renderChunks.resize(chunkCount);
    for (size_t chunkId = 0; chunkId < chunkCount; ++chunkId)
    {
        const size_t firstRoot = chunkId * RENDER_CHUNK_ROOT_TARGET;
        m_renderChunks[chunkId].sourceRoots.reserve(std::min(RENDER_CHUNK_ROOT_TARGET, sortedRoots.size() - firstRoot));
    }
    for (size_t sortedIndex = 0; sortedIndex < sortedRoots.size(); ++sortedIndex) {
        const uint32_t source = sortedRoots[sortedIndex].second;
        const uint32_t chunkId = static_cast<uint32_t>(sortedIndex / RENDER_CHUNK_ROOT_TARGET);
        m_renderChunks[chunkId].sourceRoots.push_back(source);
        m_sourceToChunk[source] = chunkId;
    }

    m_chunkInDirtyList.assign(chunkCount, 0);
    MarkAllChunksTopologyDirty();
}

std::optional<ColorRGBA> TriangleSelectorPatch::FinalRenderColorForState(EnforcerBlockerType state) const
{
    const int colorIndex = static_cast<int>(state);
    if (colorIndex < 0 || static_cast<size_t>(colorIndex) >= m_ebt_colors.size())
        return std::nullopt;

    return adjust_color_for_rendering(m_ebt_colors[static_cast<size_t>(colorIndex)]);
}

void TriangleSelectorPatch::AppendTriangleColor(std::vector<uint8_t>& colors, EnforcerBlockerType state) const
{
    const Rgba8 rgba = RenderColorForState8(state);

    for (size_t vertexIndex = 0; vertexIndex < 3; ++vertexIndex)
        colors.insert(colors.end(), rgba.begin(), rgba.end());

}

void TriangleSelectorPatch::AppendTriangleLeaves(int triangleIndex, bool showWireframe, ChunkBuildResult& result) const
{
    if (triangleIndex < 0 || triangleIndex >= static_cast<int>(m_triangles.size()))
        return;

    const Triangle& triangle = m_triangles[triangleIndex];
    if (!triangle.valid())
        return;

    if (triangle.is_split()) {
        for (int childIndex = 0; childIndex <= triangle.number_of_split_sides(); ++childIndex)
            AppendTriangleLeaves(triangle.children[childIndex], showWireframe, result);
        return;
    }

    for (size_t vertexIndex = 0; vertexIndex < 3; ++vertexIndex) {
        const Vec3f& vertex = m_vertices[triangle.verts_idxs[vertexIndex]].v;
        result.geometryStaging.push_back(vertex.x());
        result.geometryStaging.push_back(vertex.y());
        result.geometryStaging.push_back(vertex.z());
        if (showWireframe) {
            result.geometryStaging.push_back(vertexIndex == 0 ? 1.0f : 0.0f);
            result.geometryStaging.push_back(vertexIndex == 1 ? 1.0f : 0.0f);
            result.geometryStaging.push_back(vertexIndex == 2 ? 1.0f : 0.0f);
        }
    }

    AppendTriangleColor(result.colorsRgba, triangle.get_state());
    result.vertexCount += 3;
}

size_t TriangleSelectorPatch::CountLeafTriangles(int triangleIndex) const
{
    if (triangleIndex < 0 || triangleIndex >= static_cast<int>(m_triangles.size()))
        return 0;

    const Triangle& triangle = m_triangles[triangleIndex];
    if (!triangle.valid())
        return 0;

    if (triangle.is_split()) {
        size_t leafCount = 0;
        for (int childIndex = 0; childIndex <= triangle.number_of_split_sides(); ++childIndex)
            leafCount += CountLeafTriangles(triangle.children[childIndex]);
        return leafCount;
    }
    return 1;
}

TriangleSelectorPatch::ChunkBuildPlan TriangleSelectorPatch::MakeChunkBuildPlan(uint32_t chunkId, bool showWireframe) const
{
    ChunkBuildPlan plan;
    plan.chunkId = chunkId;
    if (chunkId >= m_renderChunks.size())
        return plan;

    const RenderChunk& chunk = m_renderChunks[chunkId];
    for (uint32_t source : chunk.sourceRoots)
        plan.leafCount += CountLeafTriangles(static_cast<int>(source));

    // Unreachable in practice: memory runs out long before a chunk hits the GLsizei draw limit.
    if (plan.leafCount > MAX_SUPPORTED_CHUNK_VERTICES / 3)
    {
        BOOST_LOG_TRIVIAL(error) << "TriangleSelectorPatch: render chunk exceeds GLsizei draw limit. chunk=" << chunkId
                                 << ", leaf_count=" << plan.leafCount
                                 << ", max_leaf_count=" << (MAX_SUPPORTED_CHUNK_VERTICES / 3);
    }
    assert(plan.leafCount <= MAX_SUPPORTED_CHUNK_VERTICES / 3);
    plan.vertexCount = plan.leafCount * 3;

    const size_t floatsPerVertex = showWireframe ? 6 : 3;
    const size_t geometryBytes   = plan.vertexCount * floatsPerVertex * sizeof(float);
    const size_t colorBytes      = plan.vertexCount * 4;
    const size_t rootInfoBytes   = chunk.sourceRoots.size() * sizeof(RootDrawInfo);
    plan.stagingBytes = geometryBytes + colorBytes + rootInfoBytes;
    return plan;
}

TriangleSelectorPatch::ChunkBuildResult TriangleSelectorPatch::BuildChunkCpu(const ChunkBuildPlan& plan, bool showWireframe) const
{
    ChunkBuildResult result;
    if (plan.chunkId >= m_renderChunks.size())
        return result;

    const RenderChunk& chunk = m_renderChunks[plan.chunkId];
    const size_t floatsPerVertex = showWireframe ? 6 : 3;
    result.geometryStaging.reserve(plan.vertexCount * floatsPerVertex);
    result.colorsRgba.reserve(plan.vertexCount * 4);
    result.rootDrawInfos.reserve(chunk.sourceRoots.size());

    for (uint32_t source : chunk.sourceRoots) {
        RootDrawInfo drawInfo;
        drawInfo.firstVertex = result.vertexCount;
        AppendTriangleLeaves(static_cast<int>(source), showWireframe, result);
        drawInfo.vertexCount = result.vertexCount - drawInfo.firstVertex;
        result.rootDrawInfos.push_back(drawInfo);
    }

    return result;
}

void TriangleSelectorPatch::RebuildRenderChunks(const std::vector<uint32_t>& chunkIds, bool showWireframe,
                                                const ChunkBatchLimits& limits)
{
    std::vector<ChunkBuildPlan> plans;
    plans.reserve(chunkIds.size());
    for (uint32_t chunkId : chunkIds)
        plans.push_back(MakeChunkBuildPlan(chunkId, showWireframe));

    size_t batchBegin = 0;
    while (batchBegin < plans.size())
    {
        // Greedy batch bounded by chunk count and estimated staging bytes.
        size_t batchEnd = batchBegin;
        size_t batchStagingBytes = 0;
        while (batchEnd < plans.size())
        {
            const ChunkBuildPlan& plan = plans[batchEnd];
            if (batchEnd > batchBegin && (batchEnd - batchBegin >= limits.maxChunks ||
                batchStagingBytes + plan.stagingBytes > limits.maxStagingBytes))
                break;
            // An oversized single chunk still proceeds alone.
            batchStagingBytes += plan.stagingBytes;
            ++batchEnd;
        }

        std::vector<ChunkBuildResult> results(batchEnd - batchBegin);
        if (results.size() == 1)
            results[0] = BuildChunkCpu(plans[batchBegin], showWireframe);
        else
            tbb::parallel_for(tbb::blocked_range<size_t>(0, results.size()), [&](const tbb::blocked_range<size_t>& range) {
                for (size_t resultIndex = range.begin(); resultIndex != range.end(); ++resultIndex)
                    results[resultIndex] = BuildChunkCpu(plans[batchBegin + resultIndex], showWireframe);
            });
        for (size_t resultIndex = 0; resultIndex < results.size(); ++resultIndex)
            UploadChunk(plans[batchBegin + resultIndex].chunkId, std::move(results[resultIndex]));
        // Batch results leave scope here, releasing this batch's geometry staging only.
        batchBegin = batchEnd;
    }
}

void TriangleSelectorPatch::RefreshRenderColorLut()
{
    m_renderColorLut.assign(m_ebt_colors.size(), Rgba8{0, 0, 0, 0});
    for (size_t stateIndex = 0; stateIndex < m_ebt_colors.size(); ++stateIndex) {
        const std::optional<ColorRGBA> color = FinalRenderColorForState(static_cast<EnforcerBlockerType>(stateIndex));
        if (color.has_value())
            m_renderColorLut[stateIndex] = Rgba8{color->r_uchar(), color->g_uchar(), color->b_uchar(), color->a_uchar()};
    }
}

TriangleSelectorPatch::Rgba8 TriangleSelectorPatch::RenderColorForState8(EnforcerBlockerType state) const noexcept
{
    const int stateIndex = static_cast<int>(state);
    if (stateIndex < 0 || static_cast<size_t>(stateIndex) >= m_renderColorLut.size())
        return Rgba8{0, 0, 0, 0};
    return m_renderColorLut[static_cast<size_t>(stateIndex)];
}

void TriangleSelectorPatch::render(ImGuiWrapper* imgui, const Transform3d& matrix)
{
    const bool requestedWireframe = m_need_wireframe && wxGetApp().plater()->is_wireframe_enabled() &&
                                    wxGetApp().plater()->is_show_wireframe();
    const bool useLegacyRenderer = m_filter_state || !m_useRenderChunks;

    if (useLegacyRenderer && (!_legacyWireframeLayout.has_value() || *_legacyWireframeLayout != requestedWireframe)) {
        m_update_render_data = true;
        m_paint_changed = true;
    } else if (!useLegacyRenderer &&
               (!_renderChunkWireframeLayout.has_value() || *_renderChunkWireframeLayout != requestedWireframe)) {
        m_update_render_data = true;
    }
    if (m_update_render_data) {
        update_render_data();
        m_update_render_data = false;
    }

    auto* shader = wxGetApp().get_current_shader();
    if (!shader)
        return;
    assert(shader->get_name() == "gouraud" || shader->get_name() == "mm_gouraud");

    if (useLegacyRenderer) {
        shader->set_uniform("show_wireframe", requestedWireframe);
        shader->set_uniform("use_vertex_color", false);
        for (size_t bufferIndex = 0; bufferIndex < m_triangle_patches.size(); ++bufferIndex) {
            if (this->has_VBOs(bufferIndex)) {
                const TrianglePatch& patch = m_triangle_patches[bufferIndex];
                ColorRGBA color;
                if (patch.is_fragment() && !patch.neighbor_types.empty()) {
                    size_t color_idx = (size_t)*patch.neighbor_types.begin();
                    if (color_idx >= m_ebt_colors.size())
                        continue;
                    color = m_ebt_colors[color_idx];
                    color.a(0.85);
                }
                else {
                    size_t color_idx = (size_t)patch.type;
                    if (color_idx >= m_ebt_colors.size())
                        continue;
                    color = m_ebt_colors[color_idx];
                }
                //to make black not too hard too see
                ColorRGBA new_color = adjust_color_for_rendering(color);
                shader->set_uniform("uniform_color", new_color);
                this->render(static_cast<int>(bufferIndex), requestedWireframe);
            }
        }
    } else if (m_renderChunksInitialized && _renderChunkWireframeLayout.has_value()) {
        // Draw interprets the buffers with the layout they were uploaded in.
        const bool uploadedWireframe = *_renderChunkWireframeLayout;
        shader->set_uniform("show_wireframe", uploadedWireframe);
        RenderChunks(uploadedWireframe);
    }

    render_paint_contour(matrix);
}

void TriangleSelectorPatch::update_triangles_per_type()
{
    //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", enter");
    m_triangle_patches.resize(m_ebt_colors.size());
    for (int i = 0; i < m_triangle_patches.size(); i++) {
        auto& patch = m_triangle_patches[i];
        patch.type = (EnforcerBlockerType)i;
        patch.triangle_indices.reserve(m_triangles.size() / 3);
    }

    bool using_wireframe = (m_need_wireframe && wxGetApp().plater()->is_wireframe_enabled() && wxGetApp().plater()->is_show_wireframe()) ? true : false;

    for (auto& triangle : m_triangles) {
        if (!triangle.valid() || triangle.is_split())
            continue;

        int state = (int)triangle.get_state();
        if (state < 0 || state >= int(m_triangle_patches.size()))
            continue;
        auto& patch = m_triangle_patches[state];
        //patch.triangle_indices.insert(patch.triangle_indices.end(), triangle.verts_idxs.begin(), triangle.verts_idxs.end());
        for (int i = 0; i < 3; ++i) {
            int j = triangle.verts_idxs[i];
            int index = using_wireframe?int(patch.patch_vertices.size()/6) : int(patch.patch_vertices.size()/3);
            //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: i=%2%, j=%3%, index=%4%, v[%5%,%6%,%7%]")%__LINE__%i%j%index%m_vertices[j].v(0)%m_vertices[j].v(1)%m_vertices[j].v(2);
            patch.patch_vertices.emplace_back(m_vertices[j].v(0));
            patch.patch_vertices.emplace_back(m_vertices[j].v(1));
            patch.patch_vertices.emplace_back(m_vertices[j].v(2));
            if (using_wireframe) {
                if (i == 0) {
                    patch.patch_vertices.emplace_back(1.0);
                    patch.patch_vertices.emplace_back(0.0);
                    patch.patch_vertices.emplace_back(0.0);
                }
                else if (i == 1) {
                    patch.patch_vertices.emplace_back(0.0);
                    patch.patch_vertices.emplace_back(1.0);
                    patch.patch_vertices.emplace_back(0.0);
                }
                else {
                    patch.patch_vertices.emplace_back(0.0);
                    patch.patch_vertices.emplace_back(0.0);
                    patch.patch_vertices.emplace_back(1.0);
                }
            }
            patch.triangle_indices.emplace_back( index);
        }
        //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: state=%2%, vertice size=%3%, triangle size %4%")%__LINE__%state%patch.patch_vertices.size()%patch.triangle_indices.size();
    }
    //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("exit");
}

void TriangleSelectorPatch::update_selector_triangles()
{
    for (TrianglePatch& patch : m_triangle_patches) {
        if (!patch.is_fragment() || patch.neighbor_types.empty())
            continue;

        EnforcerBlockerType type = *patch.neighbor_types.begin();
        for (int facet_idx : patch.facet_indices) {
            SetLeafState(facet_idx, type, CleanupMode::Deferred);
        }
    }
    FlushDeferredCleanup();
}

void TriangleSelectorPatch::update_triangles_per_patch()
{
    const NeighborCache& neighborCache = EnsureNeighborCache();
    std::vector<bool>  visited(m_triangles.size(), false);

    bool using_wireframe = (m_need_wireframe && wxGetApp().plater()->is_wireframe_enabled() && wxGetApp().plater()->is_show_wireframe()) ? true : false; 

    auto get_all_touching_triangles = [this](int facet_idx, const Vec3i32& neighbors, const Vec3i32& neighbors_propagated) -> std::vector<int> {
        assert(facet_idx != -1 && facet_idx < int(m_triangles.size()));
        assert(this->verify_triangle_neighbors(m_triangles[facet_idx], neighbors));
        std::vector<int> touching_triangles;
        Vec3i32            vertices = { m_triangles[facet_idx].verts_idxs[0], m_triangles[facet_idx].verts_idxs[1], m_triangles[facet_idx].verts_idxs[2] };
        append_touching_subtriangles(neighbors(0), vertices(1), vertices(0), touching_triangles);
        append_touching_subtriangles(neighbors(1), vertices(2), vertices(1), touching_triangles);
        append_touching_subtriangles(neighbors(2), vertices(0), vertices(2), touching_triangles);

        for (int neighbor_idx : neighbors_propagated)
            if (neighbor_idx != -1 && !m_triangles[neighbor_idx].is_split())
                touching_triangles.emplace_back(neighbor_idx);

        return touching_triangles;
    };

    auto calc_fragment_area = [this](const TrianglePatch& patch, float max_limit_area, int stride) {
        double total_area = 0.f;
        const std::vector<int>& ti = patch.triangle_indices;
        /*for (int i = 0; i < ti.size() / 3; i++) {
            total_area += std::abs((m_vertices[ti[i]].v - m_vertices[ti[i + 1]].v)
                .cross(m_vertices[ti[i]].v - m_vertices[ti[i + 2]].v).norm()) / 2;
            if (total_area >= max_limit_area)
                break;
        }*/
        const std::vector<float>& vertices = patch.patch_vertices;
        for (int i = 0; i < ti.size(); i+=3) {
            stl_vertex v0(vertices[ti[i] * stride], vertices[ti[i] * stride + 1], vertices[ti[i] * stride + 2]);
            stl_vertex v1(vertices[ti[i + 1] * stride], vertices[ti[i + 1] * stride + 1], vertices[ti[i + 1] * stride + 2]);
            stl_vertex v2(vertices[ti[i + 2] * stride], vertices[ti[i + 2] * stride + 1], vertices[ti[i + 2] * stride + 2]);
            total_area += std::abs((v0 - v1).cross(v0 - v2).norm()) / 2;
            if (total_area >= max_limit_area)
                break;
        }

        return total_area;
    };

    int start_facet_idx = 0;
    while (1) {
        for (; start_facet_idx < visited.size(); start_facet_idx++) {
            if (!visited[start_facet_idx] && m_triangles[start_facet_idx].valid() && !m_triangles[start_facet_idx].is_split())
                break;
        }

        if (start_facet_idx >= m_triangles.size())
            break;

        EnforcerBlockerType start_facet_state = m_triangles[start_facet_idx].get_state();
        TrianglePatch patch;
        std::queue<int> facet_queue;
        facet_queue.push(start_facet_idx);
        while (!facet_queue.empty()) {
            int current_facet = facet_queue.front();
            facet_queue.pop();
            assert(!m_triangles[current_facet].is_split());

            if (!visited[current_facet]) {
                Triangle& triangle = m_triangles[current_facet];
                for (int i = 0; i < 3; ++i) {
                    int j = triangle.verts_idxs[i];
                    int index = using_wireframe?int(patch.patch_vertices.size()/6) : int(patch.patch_vertices.size()/3);
                    patch.patch_vertices.emplace_back(m_vertices[j].v(0));
                    patch.patch_vertices.emplace_back(m_vertices[j].v(1));
                    patch.patch_vertices.emplace_back(m_vertices[j].v(2));
                    if (using_wireframe) {
                        if (i == 0) {
                            patch.patch_vertices.emplace_back(1.0);
                            patch.patch_vertices.emplace_back(0.0);
                            patch.patch_vertices.emplace_back(0.0);
                        }
                        else if (i == 1) {
                            patch.patch_vertices.emplace_back(0.0);
                            patch.patch_vertices.emplace_back(1.0);
                            patch.patch_vertices.emplace_back(0.0);
                        }
                        else {
                            patch.patch_vertices.emplace_back(0.0);
                            patch.patch_vertices.emplace_back(0.0);
                            patch.patch_vertices.emplace_back(1.0);
                        }
                    }
                    patch.triangle_indices.emplace_back( index);
                }
                //patch.triangle_indices.insert(patch.triangle_indices.end(), triangle.verts_idxs.begin(), triangle.verts_idxs.end());
                patch.facet_indices.push_back(current_facet);

                std::vector<int> touching_triangles = get_all_touching_triangles(
                    current_facet, neighborCache.neighbors[current_facet], neighborCache.propagated[current_facet]);
                for (const int tr_idx : touching_triangles) {
                    if (tr_idx < 0)
                        continue;

                    if (m_triangles[tr_idx].get_state() != start_facet_state) {
                        patch.neighbor_types.insert(m_triangles[tr_idx].get_state());
                        continue;
                    }

                    // should check visited state after color for neight types
                    if (visited[tr_idx])
                        continue;

                    assert(!m_triangles[tr_idx].is_split());
                    facet_queue.push(tr_idx);
                }
            }

            visited[current_facet] = true;
        }

        patch.area = calc_fragment_area(patch, GapAreaMax, using_wireframe?6:3);
        patch.type = start_facet_state;
        m_triangle_patches.emplace_back(std::move(patch));
     }
}

void TriangleSelectorPatch::set_filter_state(bool is_filter_state)
{
    if (!m_filter_state && is_filter_state) {
        m_filter_state = is_filter_state;
        this->release_geometry();
        m_paint_changed = true;
        update_render_data();
    }

    if (m_filter_state && !is_filter_state)
        this->release_geometry();

    m_filter_state = is_filter_state;
    m_update_render_data = true;
}

void TriangleSelectorPatch::update_render_data()
{
    const bool showWireframe = m_need_wireframe && wxGetApp().plater()->is_wireframe_enabled() &&
                               wxGetApp().plater()->is_show_wireframe();
    if (m_useRenderChunks && !m_filter_state) {
        UpdateRenderChunks(showWireframe);
        m_paint_changed = false;
        update_paint_contour();
        return;
    }

    if (m_paint_changed || m_triangle_patches.empty()) {
        this->release_geometry();

        /*m_patch_vertices.reserve(m_vertices.size() * 3);
        for (const Vertex& vr : m_vertices) {
            m_patch_vertices.emplace_back(vr.v.x());
            m_patch_vertices.emplace_back(vr.v.y());
            m_patch_vertices.emplace_back(vr.v.z());
        }
        this->finalize_vertices();*/

        if (m_filter_state)
            update_triangles_per_patch();
        else
            update_triangles_per_type();
        this->finalize_triangle_indices();

        _legacyWireframeLayout = showWireframe;
        m_paint_changed = false;
    }

    //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", before paint_contour");
    update_paint_contour();
}

void TriangleSelectorPatch::EnsureVboCapacity(unsigned int target, unsigned int& vboId, size_t& capacityBytes,
                                              size_t requiredBytes, unsigned int usage)
{
    if (requiredBytes == 0)
        return;

    if (vboId == 0)
        glsafe(::glGenBuffers(1, &vboId));

    glsafe(::glBindBuffer(static_cast<GLenum>(target), vboId));
    if (requiredBytes > capacityBytes) {
        const size_t grownCapacity = capacityBytes == 0 ? requiredBytes : capacityBytes + capacityBytes / 2;
        capacityBytes = std::max(requiredBytes, grownCapacity);
        glsafe(::glBufferData(static_cast<GLenum>(target), static_cast<GLsizeiptr>(capacityBytes), nullptr,
                              static_cast<GLenum>(usage)));
    }
}

void TriangleSelectorPatch::UploadChunk(uint32_t chunkId, ChunkBuildResult&& result)
{
    if (chunkId >= m_renderChunks.size())
        return;

    RenderChunk& chunk = m_renderChunks[chunkId];
    if (result.rootDrawInfos.size() != chunk.sourceRoots.size())
        return;

    for (size_t rootIndex = 0; rootIndex < chunk.sourceRoots.size(); ++rootIndex)
        m_rootDrawInfo[chunk.sourceRoots[rootIndex]] = result.rootDrawInfos[rootIndex];

    chunk.vertexCount = result.vertexCount;
    chunk.colorsRgba = std::move(result.colorsRgba);

    const size_t geometryBytes = result.geometryStaging.size() * sizeof(float);
    EnsureVboCapacity(GL_ARRAY_BUFFER, chunk.geometryVbo, chunk.geometryCapacityBytes, geometryBytes, GL_DYNAMIC_DRAW);
    if (geometryBytes > 0) {
        glsafe(::glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(geometryBytes), result.geometryStaging.data()));
    }

    const size_t colorBytes = chunk.colorsRgba.size() * sizeof(uint8_t);
    EnsureVboCapacity(GL_ARRAY_BUFFER, chunk.colorVbo, chunk.colorCapacityBytes, colorBytes, GL_DYNAMIC_DRAW);
    if (colorBytes > 0) {
        glsafe(::glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(colorBytes), chunk.colorsRgba.data()));
    }

    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
}

void TriangleSelectorPatch::MarkRootDirty(uint32_t source, DirtyLevel level)
{
    if (source >= m_rootDirty.size() || level == DirtyLevel::Clean)
        return;

    DirtyLevel& current = m_rootDirty[source];
    if (current == DirtyLevel::Clean)
        m_dirtyRoots.push_back(source);

    if (static_cast<uint8_t>(level) > static_cast<uint8_t>(current))
        current = level;
}

void TriangleSelectorPatch::MarkAllChunksTopologyDirty()
{
    for (uint32_t chunkId = 0; chunkId < m_renderChunks.size(); ++chunkId) {
        RenderChunk& chunk = m_renderChunks[chunkId];
        chunk.dirty = DirtyLevel::Topology;
        if (m_chunkInDirtyList[chunkId] == 0) {
            m_chunkInDirtyList[chunkId] = 1;
            m_dirtyChunks.push_back(chunkId);
        }
    }
}

void TriangleSelectorPatch::AggregateDirtyRoots()
{
    // ClearRenderDirtyState empties these at the end of every update.
    assert(std::all_of(m_renderChunks.begin(), m_renderChunks.end(),
                       [](const RenderChunk& chunk) { return chunk.stateDirtyRoots.empty(); }));

    for (uint32_t source : m_dirtyRoots) {
        if (source >= m_sourceToChunk.size())
            continue;

        const uint32_t chunkId = m_sourceToChunk[source];
        if (chunkId >= m_renderChunks.size())
            continue;

        RenderChunk& chunk = m_renderChunks[chunkId];
        const DirtyLevel rootLevel = m_rootDirty[source];
        if (static_cast<uint8_t>(rootLevel) > static_cast<uint8_t>(chunk.dirty))
            chunk.dirty = rootLevel;
        if (rootLevel == DirtyLevel::State)
            chunk.stateDirtyRoots.push_back(source);
        if (m_chunkInDirtyList[chunkId] == 0) {
            m_chunkInDirtyList[chunkId] = 1;
            m_dirtyChunks.push_back(chunkId);
        }
    }
}

void TriangleSelectorPatch::RewriteTriangleColors(int triangleIndex, RenderChunk& chunk, uint32_t& vertexOffset) const
{
    if (triangleIndex < 0 || triangleIndex >= static_cast<int>(m_triangles.size()))
        return;

    const Triangle& triangle = m_triangles[triangleIndex];
    if (!triangle.valid())
        return;

    if (triangle.is_split()) {
        for (int childIndex = 0; childIndex <= triangle.number_of_split_sides(); ++childIndex)
            RewriteTriangleColors(triangle.children[childIndex], chunk, vertexOffset);
        return;
    }

    const Rgba8 rgba = RenderColorForState8(triangle.get_state());
    for (size_t vertexIndex = 0; vertexIndex < 3; ++vertexIndex) {
        const size_t byteOffset = static_cast<size_t>(vertexOffset) * 4;
        if (byteOffset + rgba.size() > chunk.colorsRgba.size())
            return;
        std::copy(rgba.begin(), rgba.end(), chunk.colorsRgba.begin() + byteOffset);
        ++vertexOffset;
    }
}

void TriangleSelectorPatch::RewriteRootColors(uint32_t source)
{
    if (source >= m_sourceToChunk.size() || source >= m_rootDrawInfo.size())
        return;

    const uint32_t chunkId = m_sourceToChunk[source];
    if (chunkId >= m_renderChunks.size())
        return;

    RenderChunk& chunk = m_renderChunks[chunkId];
    const RootDrawInfo& drawInfo = m_rootDrawInfo[source];
    uint32_t vertexOffset = drawInfo.firstVertex;
    RewriteTriangleColors(static_cast<int>(source), chunk, vertexOffset);
    assert(vertexOffset == drawInfo.firstVertex + drawInfo.vertexCount);
    chunk.dirtyColorRanges.push_back({drawInfo.firstVertex, drawInfo.firstVertex + drawInfo.vertexCount});
}

void TriangleSelectorPatch::RewriteChunkColors(uint32_t chunkId)
{
    if (chunkId >= m_renderChunks.size())
        return;

    RenderChunk& chunk = m_renderChunks[chunkId];
    chunk.colorsRgba.assign(static_cast<size_t>(chunk.vertexCount) * 4, 0);
    for (uint32_t source : chunk.sourceRoots) {
        const RootDrawInfo& drawInfo = m_rootDrawInfo[source];
        uint32_t vertexOffset = drawInfo.firstVertex;
        RewriteTriangleColors(static_cast<int>(source), chunk, vertexOffset);
        assert(vertexOffset == drawInfo.firstVertex + drawInfo.vertexCount);
    }
    chunk.dirtyColorRanges.push_back({0, chunk.vertexCount});
}

void TriangleSelectorPatch::MergeDirtyRanges(RenderChunk& chunk) const
{
    if (chunk.dirtyColorRanges.size() < 2)
        return;

    std::sort(chunk.dirtyColorRanges.begin(), chunk.dirtyColorRanges.end(), [](const DirtyRange& left,
                                                                               const DirtyRange& right) {
        return left.beginVertex < right.beginVertex;
    });

    size_t outputIndex = 0;
    for (size_t inputIndex = 1; inputIndex < chunk.dirtyColorRanges.size(); ++inputIndex) {
        DirtyRange& output = chunk.dirtyColorRanges[outputIndex];
        const DirtyRange& input = chunk.dirtyColorRanges[inputIndex];
        if (input.beginVertex <= output.endVertex) {
            output.endVertex = std::max(output.endVertex, input.endVertex);
        } else {
            ++outputIndex;
            chunk.dirtyColorRanges[outputIndex] = input;
        }
    }
    chunk.dirtyColorRanges.resize(outputIndex + 1);
}

void TriangleSelectorPatch::UploadColorAdaptive(uint32_t chunkId, ColorUploadReason reason)
{
    if (chunkId >= m_renderChunks.size())
        return;

    RenderChunk& chunk = m_renderChunks[chunkId];
    if (chunk.colorsRgba.empty() || chunk.dirtyColorRanges.empty())
        return;

    // Ranges are already merged by PrepareChunkColorsCpu.
    size_t dirtyBytes = 0;
    for (const DirtyRange& range : chunk.dirtyColorRanges)
        dirtyBytes += static_cast<size_t>(range.endVertex - range.beginVertex) * 4;

    const size_t totalBytes = chunk.colorsRgba.size();
    EnsureVboCapacity(GL_ARRAY_BUFFER, chunk.colorVbo, chunk.colorCapacityBytes, totalBytes, GL_DYNAMIC_DRAW);
    if (reason == ColorUploadReason::AllColor || chunk.dirtyColorRanges.size() > MAX_COLOR_UPLOAD_RANGES ||
        dirtyBytes * 2 >= totalBytes) {
        glsafe(::glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(totalBytes), chunk.colorsRgba.data()));
    } else {
        assert(reason == ColorUploadReason::State);
        for (const DirtyRange& range : chunk.dirtyColorRanges) {
            const size_t byteOffset = static_cast<size_t>(range.beginVertex) * 4;
            const size_t byteSize = static_cast<size_t>(range.endVertex - range.beginVertex) * 4;
            glsafe(::glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(byteOffset), static_cast<GLsizeiptr>(byteSize),
                                   chunk.colorsRgba.data() + byteOffset));
        }
    }
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
}

void TriangleSelectorPatch::ClearRenderDirtyState()
{
    for (uint32_t source : m_dirtyRoots) {
        if (source < m_rootDirty.size())
            m_rootDirty[source] = DirtyLevel::Clean;
    }
    for (RenderChunk& chunk : m_renderChunks) {
        chunk.dirty = DirtyLevel::Clean;
        chunk.stateDirtyRoots.clear();
        chunk.dirtyColorRanges.clear();
    }
    std::fill(m_chunkInDirtyList.begin(), m_chunkInDirtyList.end(), 0);
    m_dirtyRoots.clear();
    m_dirtyChunks.clear();
    m_allColorDirty = false;
}

void TriangleSelectorPatch::UpdateRenderChunks(bool showWireframe)
{
    const bool wireframeLayoutChanged = !_renderChunkWireframeLayout.has_value() ||
                                        *_renderChunkWireframeLayout != showWireframe;
    if (wireframeLayoutChanged && m_renderChunksInitialized)
        MarkAllChunksTopologyDirty();

    // Main thread only: workers read the LUT, never GUI color config.
    RefreshRenderColorLut();
    const ChunkBatchLimits limits;

    if (!m_renderChunksInitialized) {
        std::vector<uint32_t> chunkIds;
        chunkIds.reserve(m_renderChunks.size());
        for (uint32_t chunkId = 0; chunkId < m_renderChunks.size(); ++chunkId)
            chunkIds.push_back(chunkId);
        RebuildRenderChunks(chunkIds, showWireframe, limits);
        m_renderChunksInitialized = true;
        _renderChunkWireframeLayout = showWireframe;
        ClearRenderDirtyState();
        return;
    }

    AggregateDirtyRoots();

    // Freeze the dirty input into two disjoint task tables.
    std::vector<uint32_t>    geometryChunkIds;
    std::vector<ChunkColorTask> colorTasks;
    if (m_allColorDirty) {
        geometryChunkIds.reserve(m_renderChunks.size());
        colorTasks.reserve(m_renderChunks.size());
        for (uint32_t chunkId = 0; chunkId < m_renderChunks.size(); ++chunkId) {
            if (m_renderChunks[chunkId].dirty == DirtyLevel::Topology)
                geometryChunkIds.push_back(chunkId);
            else
                colorTasks.push_back({chunkId, ColorUploadReason::AllColor});
        }
    } else {
        geometryChunkIds.reserve(m_dirtyChunks.size());
        colorTasks.reserve(m_dirtyChunks.size());
        for (uint32_t chunkId : m_dirtyChunks) {
            if (m_renderChunks[chunkId].dirty == DirtyLevel::Topology)
                geometryChunkIds.push_back(chunkId);
            else
                colorTasks.push_back({chunkId, ColorUploadReason::State});
        }
    }

    // Geometry rebuild first: color workers then read a stable m_rootDrawInfo.
    RebuildRenderChunks(geometryChunkIds, showWireframe, limits);
    UpdateChunkColors(colorTasks, limits);

    _renderChunkWireframeLayout = showWireframe;
    ClearRenderDirtyState();
}

void TriangleSelectorPatch::PrepareChunkColorsCpu(const ChunkColorTask& task)
{
    RenderChunk& chunk = m_renderChunks[task.chunkId];

    // stateDirtyRoots were aggregated on the main thread; this task owns the chunk.
    chunk.dirtyColorRanges.clear();
    const size_t rangeUpperBound = task.reason == ColorUploadReason::AllColor ?
        size_t(1) : chunk.stateDirtyRoots.size();
    chunk.dirtyColorRanges.reserve(rangeUpperBound);

    if (task.reason == ColorUploadReason::AllColor)
        RewriteChunkColors(task.chunkId);
    else
        for (uint32_t sourceRoot : chunk.stateDirtyRoots)
            RewriteRootColors(sourceRoot);

    MergeDirtyRanges(chunk);
}

void TriangleSelectorPatch::UpdateChunkColors(const std::vector<ChunkColorTask>& tasks, const ChunkBatchLimits& limits)
{
    size_t batchBegin = 0;
    while (batchBegin < tasks.size())
    {
        const size_t batchEnd   = std::min(batchBegin + limits.maxChunks, tasks.size());
        const size_t batchCount = batchEnd - batchBegin;
        if (batchCount == 1)
            PrepareChunkColorsCpu(tasks[batchBegin]);
        else
            tbb::parallel_for(tbb::blocked_range<size_t>(0, batchCount), [&](const tbb::blocked_range<size_t>& range) {
                for (size_t taskIndex = range.begin(); taskIndex != range.end(); ++taskIndex)
                    PrepareChunkColorsCpu(tasks[batchBegin + taskIndex]);
            });
        for (size_t taskIndex = batchBegin; taskIndex < batchEnd; ++taskIndex)
            UploadColorAdaptive(tasks[taskIndex].chunkId, tasks[taskIndex].reason);
        batchBegin = batchEnd;
    }
}

void TriangleSelectorPatch::RenderChunks(bool showWireframe)
{
    GLShaderProgram* shader = wxGetApp().get_current_shader();
    if (shader == nullptr)
        return;

    shader->set_uniform("use_vertex_color", true);
    const GLint positionId = shader->get_attrib_location("v_position");
    const GLint barycentricId = shader->get_attrib_location("v_barycentric");
    const GLint colorId = shader->get_attrib_location("v_color");
    const GLsizei geometryStride = static_cast<GLsizei>((showWireframe ? 6 : 3) * sizeof(float));

    for (const RenderChunk& chunk : m_renderChunks) {
        if (chunk.vertexCount == 0 || chunk.geometryVbo == 0 || chunk.colorVbo == 0)
            continue;

        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, chunk.geometryVbo));
        if (positionId != -1) {
            glsafe(::glVertexAttribPointer(positionId, 3, GL_FLOAT, GL_FALSE, geometryStride, nullptr));
            glsafe(::glEnableVertexAttribArray(positionId));
        }
        if (barycentricId != -1) {
            // Keep the array enabled even without wireframe for the AMD Vega painter workaround.
            // Position3 is a valid dummy stream when the shader does not consume barycentric values.
            const void* barycentricOffset = showWireframe ?
                reinterpret_cast<const void*>(3 * sizeof(float)) : nullptr;
            glsafe(::glVertexAttribPointer(barycentricId, 3, GL_FLOAT, GL_FALSE, geometryStride, barycentricOffset));
            glsafe(::glEnableVertexAttribArray(barycentricId));
        }

        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, chunk.colorVbo));
        if (colorId != -1) {
            glsafe(::glVertexAttribPointer(colorId, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4 * sizeof(uint8_t), nullptr));
            glsafe(::glEnableVertexAttribArray(colorId));
        }

        glsafe(::glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(chunk.vertexCount)));
        if (positionId != -1)
            glsafe(::glDisableVertexAttribArray(positionId));
        if (barycentricId != -1)
            glsafe(::glDisableVertexAttribArray(barycentricId));
        if (colorId != -1)
            glsafe(::glDisableVertexAttribArray(colorId));
    }
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
    shader->set_uniform("use_vertex_color", false);
}

void TriangleSelectorPatch::render(int triangle_indices_idx, bool show_wireframe)
{
    assert(triangle_indices_idx < this->m_triangle_indices_VBO_ids.size());
    assert(this->m_triangle_patches.size() == this->m_triangle_indices_VBO_ids.size());
    //assert(this->m_vertices_VBO_id != 0);
    assert(this->m_triangle_patches.size() == this->m_vertices_VBO_ids.size());
    assert(this->m_vertices_VBO_ids[triangle_indices_idx] != 0);
    assert(this->m_triangle_indices_VBO_ids[triangle_indices_idx] != 0);

    GLShaderProgram *shader = wxGetApp().get_current_shader();
    if (shader == nullptr)
        return;

    // the following binding is needed to set the vertex attributes
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, this->m_vertices_VBO_ids[triangle_indices_idx]));
    const GLint positionId = shader->get_attrib_location("v_position");
    const GLsizei geometryStride = static_cast<GLsizei>((show_wireframe ? 6 : 3) * sizeof(float));
    if (positionId != -1) {
        glsafe(::glVertexAttribPointer(positionId, 3, GL_FLOAT, GL_FALSE, geometryStride, nullptr));
        glsafe(::glEnableVertexAttribArray(positionId));
    }
    const GLint barycentricId = shader->get_attrib_location("v_barycentric");
    // Orca: This is required even if wireframe is not displayed, otherwise on AMD Vega GPUs the painter gizmo won't render properly
    if (barycentricId != -1) {
        const void* barycentricOffset = show_wireframe ?
            reinterpret_cast<const void*>(3 * sizeof(float)) : nullptr;
        glsafe(::glVertexAttribPointer(barycentricId, 3, GL_FLOAT, GL_FALSE, geometryStride, barycentricOffset));
        glsafe(::glEnableVertexAttribArray(barycentricId));
    }

    // Render using the Vertex Buffer Objects.
    if (this->m_triangle_indices_sizes[triangle_indices_idx] > 0) {
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->m_triangle_indices_VBO_ids[triangle_indices_idx]));
        glsafe(::glDrawElements(GL_TRIANGLES, GLsizei(this->m_triangle_indices_sizes[triangle_indices_idx]), GL_UNSIGNED_INT, nullptr));
        glsafe(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
        //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: triangle_indices_idx %2%, bind indices vbo, buffer id %3%")%__LINE__%triangle_indices_idx%this->m_triangle_indices_VBO_ids[triangle_indices_idx];
    }

    if (positionId != -1)
        glsafe(::glDisableVertexAttribArray(positionId));
    if (barycentricId != -1)
        glsafe(::glDisableVertexAttribArray(barycentricId));

    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
}

void TriangleSelectorPatch::ReleaseRenderChunks()
{
    for (RenderChunk& chunk : m_renderChunks) {
        if (chunk.geometryVbo != 0) {
            glsafe(::glDeleteBuffers(1, &chunk.geometryVbo));
        }
        if (chunk.colorVbo != 0) {
            glsafe(::glDeleteBuffers(1, &chunk.colorVbo));
        }
        chunk.geometryVbo = 0;
        chunk.colorVbo = 0;
        chunk.geometryCapacityBytes = 0;
        chunk.colorCapacityBytes = 0;
    }
    m_renderChunksInitialized = false;
    _renderChunkWireframeLayout.reset();
    m_update_render_data = true;
}

void TriangleSelectorPatch::ReleaseGlResources()
{
    ReleaseRenderChunks();
    release_geometry();
    ReleaseOwnedGlResources();
}

std::vector<unsigned int> TriangleSelectorPatch::DetachGlResources()
{
    std::vector<unsigned int> bufferIds;
    bufferIds.reserve(m_renderChunks.size() * 2 + m_vertices_VBO_ids.size() + m_triangle_indices_VBO_ids.size() + 2);
    for (RenderChunk& chunk : m_renderChunks) {
        if (chunk.geometryVbo != 0)
            bufferIds.push_back(chunk.geometryVbo);
        if (chunk.colorVbo != 0)
            bufferIds.push_back(chunk.colorVbo);
        chunk.geometryVbo = 0;
        chunk.colorVbo = 0;
        chunk.geometryCapacityBytes = 0;
        chunk.colorCapacityBytes = 0;
    }
    for (unsigned int& vboId : m_vertices_VBO_ids) {
        if (vboId != 0)
            bufferIds.push_back(vboId);
        vboId = 0;
    }
    for (unsigned int& vboId : m_triangle_indices_VBO_ids) {
        if (vboId != 0)
            bufferIds.push_back(vboId);
        vboId = 0;
    }
    DetachOwnedGlResources(bufferIds);
    m_renderChunksInitialized = false;
    _renderChunkWireframeLayout.reset();
    _legacyWireframeLayout.reset();
    m_update_render_data = true;
    m_paint_changed = true;
    clear();
    return bufferIds;
}

void TriangleSelectorPatch::release_geometry()
{
    /*if (m_vertices_VBO_id) {
        glsafe(::glDeleteBuffers(1, &m_vertices_VBO_id));
        m_vertices_VBO_id = 0;
    }*/
    for (auto& vertice_VBO_id : m_vertices_VBO_ids) {
        glsafe(::glDeleteBuffers(1, &vertice_VBO_id));
        vertice_VBO_id = 0;
    }
    for (auto& triangle_indices_VBO_id : m_triangle_indices_VBO_ids) {
        glsafe(::glDeleteBuffers(1, &triangle_indices_VBO_id));
        triangle_indices_VBO_id = 0;
    }
    this->clear();
    _legacyWireframeLayout.reset();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: released geometry")%__LINE__;
}

void TriangleSelectorPatch::finalize_vertices()
{
    /*assert(m_vertices_VBO_id == 0);
    if (!this->m_patch_vertices.empty()) {
        glsafe(::glGenBuffers(1, &this->m_vertices_VBO_id));
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, this->m_vertices_VBO_id));
        glsafe(::glBufferData(GL_ARRAY_BUFFER, this->m_patch_vertices.size() * sizeof(float), this->m_patch_vertices.data(), GL_STATIC_DRAW));
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
        this->m_patch_vertices.clear();
    }*/
}

void TriangleSelectorPatch::finalize_triangle_indices()
{
    m_vertices_VBO_ids.resize(m_triangle_patches.size());
    m_triangle_indices_VBO_ids.resize(m_triangle_patches.size());
    m_triangle_indices_sizes.resize(m_triangle_patches.size());
    assert(std::all_of(m_triangle_indices_VBO_ids.cbegin(), m_triangle_indices_VBO_ids.cend(), [](const auto& ti_VBO_id) { return ti_VBO_id == 0; }));

    for (size_t buffer_idx = 0; buffer_idx < m_triangle_patches.size(); ++buffer_idx) {
        std::vector<float>& patch_vertices = m_triangle_patches[buffer_idx].patch_vertices;
        if (!patch_vertices.empty()) {
            glsafe(::glGenBuffers(1, &m_vertices_VBO_ids[buffer_idx]));
            glsafe(::glBindBuffer(GL_ARRAY_BUFFER, m_vertices_VBO_ids[buffer_idx]));
            glsafe(::glBufferData(GL_ARRAY_BUFFER, patch_vertices.size() * sizeof(float), patch_vertices.data(), GL_STATIC_DRAW));
            glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
            //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: buffer_idx %2%, vertices size %3%, buffer id %4%")%__LINE__%buffer_idx%patch_vertices.size()%m_vertices_VBO_ids[buffer_idx];
            patch_vertices.clear();
        }

        std::vector<int>& triangle_indices = m_triangle_patches[buffer_idx].triangle_indices;
        m_triangle_indices_sizes[buffer_idx] = triangle_indices.size();
        if (!triangle_indices.empty()) {
            glsafe(::glGenBuffers(1, &m_triangle_indices_VBO_ids[buffer_idx]));
            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_triangle_indices_VBO_ids[buffer_idx]));
            glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, triangle_indices.size() * sizeof(int), triangle_indices.data(), GL_STATIC_DRAW));
            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
            //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: buffer_idx %2%, vertices size %3%, buffer id %4%")%__LINE__%buffer_idx%triangle_indices.size()%m_triangle_indices_VBO_ids[buffer_idx];
            triangle_indices.clear();
        }
    }
}

#ifdef PRUSASLICER_TRIANGLE_SELECTOR_DEBUG
void TriangleSelectorGUI::render_debug(ImGuiWrapper* imgui)
{
    imgui->begin(std::string("TriangleSelector dialog (DEV ONLY)"),
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    static float edge_limit = 1.f;
    imgui->text("Edge limit (mm): ");
    imgui->slider_float("", &edge_limit, 0.1f, 8.f);
    set_edge_limit(edge_limit);
    imgui->checkbox("Show split triangles: ", m_show_triangles);
    imgui->checkbox("Show invalid triangles: ", m_show_invalid);

    int valid_triangles = m_triangles.size() - m_invalid_triangles;
    imgui->text("Valid triangles: " + std::to_string(valid_triangles) +
                  "/" + std::to_string(m_triangles.size()));
    imgui->text("Vertices: " + std::to_string(m_vertices.size()));
    if (imgui->button("Force garbage collection"))
        garbage_collect();

    if (imgui->button("Serialize - deserialize")) {
        auto map = serialize();
        deserialize(map);
    }

    imgui->end();

    if (! m_show_triangles)
        return;

    enum vtype {
        ORIGINAL = 0,
        SPLIT,
        INVALID
    };

    for (auto& va : m_varrays)
        va.reset();

    std::array<int, 3> cnts;

    ::glScalef(1.01f, 1.01f, 1.01f);

    std::array<GLModel::Geometry, 3> varrays_data;
    for (auto& data : varrays_data)
        data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3, GLModel::Geometry::EIndexType::UINT };

    for (int tr_id=0; tr_id<int(m_triangles.size()); ++tr_id) {
        const Triangle& tr = m_triangles[tr_id];
        GLModel::Geometry* va = nullptr;
        int* cnt = nullptr;
        if (tr_id < m_orig_size_indices) {
            va = &varrays_data[ORIGINAL];
            cnt = &cnts[ORIGINAL];
        }
        else if (tr.valid()) {
            va = &varrays_data[SPLIT];
            cnt = &cnts[SPLIT];
        }
        else {
            if (! m_show_invalid)
                continue;
            va = &varrays_data[INVALID];
            cnt = &cnts[INVALID];
        }

        for (int i = 0; i < 3; ++i) {
            va->add_vertex(m_vertices[tr.verts_idxs[i]].v, Vec3f(0.0f, 0.0f, 1.0f));
        }
        va->add_uint_triangle((unsigned int)*cnt, (unsigned int)*cnt + 1, (unsigned int)*cnt + 2);
        *cnt += 3;
    }

    for (int i = 0; i < 3; ++i) {
        if (!varrays_data[i].is_empty())
            m_varrays[i].init_from(std::move(varrays_data[i]));
    }

    GLShaderProgram* curr_shader = wxGetApp().get_current_shader();
    if (curr_shader != nullptr)
        curr_shader->stop_using();

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader != nullptr) {
        shader->start_using();

        const Camera& camera = wxGetApp().plater()->get_camera();
        shader->set_uniform("view_model_matrix", camera.get_view_matrix());
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    ::glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );
    for (vtype i : {ORIGINAL, SPLIT, INVALID}) {
        GLModel& va = m_varrays[i];
        switch (i) {
        case ORIGINAL: va.set_color({ 0.0f, 0.0f, 1.0f, 1.0f }); break;
        case SPLIT:    va.set_color({ 1.0f, 0.0f, 0.0f, 1.0f }); break;
        case INVALID:  va.set_color({ 1.0f, 1.0f, 0.0f, 1.0f }); break;
        }
        va.render();
    }
    ::glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );

        shader->stop_using();
    }

    if (curr_shader != nullptr)
        curr_shader->start_using();
}
#endif // PRUSASLICER_TRIANGLE_SELECTOR_DEBUG

void TriangleSelectorGUI::update_paint_contour()
{
    if (m_pointerPreviewEnabled && !m_pointerPreviewDirty)
        return;

    if (!m_pointerPreviewEnabled) {
        // Fill contour cache: same preview membership and tree identity keep the GLModel valid.
        // Empty previews cache an empty model, so both hit cases can return directly.
        const SeedFillContourKey currentKey{GetSeedFillPreviewRevision(), GetTopologyRevision(), GetTriangleIndexRevision()};
        if (m_seedFillContourKey.has_value() && *m_seedFillContourKey == currentKey)
            return;
    }

    m_paint_contour.reset();

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };

    if (m_pointerPreviewEnabled) {
        m_pointerPreviewDirty = false;
        if (!m_pointerPreviewVertices.has_value())
            return;

        init_data.reserve_vertices(3);
        init_data.reserve_indices(6);
        init_data.color = ColorRGBA::WHITE();
        for (const Vec3f& vertex : *m_pointerPreviewVertices)
            init_data.add_vertex(vertex);
        init_data.add_line(0, 1);
        init_data.add_line(1, 2);
        init_data.add_line(2, 0);
        m_paint_contour.init_from(std::move(init_data));
        return;
    }

    const std::vector<Vec2i32> contour_edges = this->get_seed_fill_contour();
    init_data.reserve_vertices(2 * contour_edges.size());
    init_data.reserve_indices(2 * contour_edges.size());
    init_data.color = ColorRGBA::WHITE();
    // vertices + indices
    unsigned int vertices_count = 0;
    for (const Vec2i32& edge : contour_edges) {
        init_data.add_vertex(m_vertices[edge(0)].v);
        init_data.add_vertex(m_vertices[edge(1)].v);
        vertices_count += 2;
        init_data.add_line(vertices_count - 2, vertices_count - 1);
    }

    if (!init_data.is_empty())
        m_paint_contour.init_from(std::move(init_data));

    m_seedFillContourKey = SeedFillContourKey{GetSeedFillPreviewRevision(), GetTopologyRevision(),
                                              GetTriangleIndexRevision()};
}

void TriangleSelectorGUI::render_paint_contour(const Transform3d& matrix)
{
    auto* curr_shader = wxGetApp().get_current_shader();
    if (curr_shader != nullptr)
        curr_shader->stop_using();

    auto* contour_shader = wxGetApp().get_shader("mm_contour");
    if (contour_shader != nullptr) {
        contour_shader->start_using();

        contour_shader->set_uniform("offset", OpenGLManager::get_gl_info().is_mesa() ? 0.0005 : 0.00001);
        const Camera& camera = wxGetApp().plater()->get_camera();
        contour_shader->set_uniform("view_model_matrix", camera.get_view_matrix() * matrix);
        contour_shader->set_uniform("projection_matrix", camera.get_projection_matrix());

        m_paint_contour.render();

        contour_shader->stop_using();
    }

    if (curr_shader != nullptr)
        curr_shader->start_using();
}

} // namespace Slic3r::GUI
