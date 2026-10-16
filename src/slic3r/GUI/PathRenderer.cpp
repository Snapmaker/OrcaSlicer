#include "PathRenderer.hpp"
#include "PathData.hpp"
#include "GLShader.hpp"
#include "GLModel.hpp"     // diamond() marker template
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "Camera.hpp"
#include "GCodeViewer.hpp" // color tables (Range_Colors, Extrusion_Role_Colors)
#include "3DScene.hpp"     // glsafe

#include <boost/log/trivial.hpp>

#include <GL/glew.h>

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace GUI {

namespace {

// texture stages used by the gpu_path shaders
constexpr unsigned int STAGE_COLOR_RAMP = 0;
constexpr unsigned int STAGE_NODE_TABLE = 1;
constexpr unsigned int STAGE_WIDTH_HEIGHT_TABLE = 2;
constexpr unsigned int STAGE_ATTRIBUTE_TABLE = 3;
constexpr unsigned int STAGE_STEP_TABLE = 4;

// texels of the linear color-gradient ramp
constexpr size_t GRADIENT_TEXELS = 256;

std::vector<float> BuildGradientData(const std::vector<ColorRGBA>& colors)
{
    std::vector<float> data(GRADIENT_TEXELS * 4, 0.0f);
    if (colors.empty())
        return data;

    for (size_t i = 0; i < GRADIENT_TEXELS; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(GRADIENT_TEXELS - 1);
        const float scaled = t * static_cast<float>(colors.size() - 1);
        const size_t low = std::clamp<size_t>(static_cast<size_t>(scaled), 0, colors.size() - 1);
        const size_t high = std::clamp<size_t>(low + 1, 0, colors.size() - 1);
        const float frac = scaled - static_cast<float>(low);
        const ColorRGBA& c0 = colors[low];
        const ColorRGBA& c1 = colors[high];
        data[i * 4 + 0] = c0.r() + (c1.r() - c0.r()) * frac;
        data[i * 4 + 1] = c0.g() + (c1.g() - c0.g()) * frac;
        data[i * 4 + 2] = c0.b() + (c1.b() - c0.b()) * frac;
        data[i * 4 + 3] = c0.a() + (c1.a() - c0.a()) * frac;
    }
    return data;
}

std::vector<float> BuildPaletteData(const std::vector<ColorRGBA>& colors)
{
    std::vector<float> data(colors.size() * 4, 0.0f);
    for (size_t i = 0; i < colors.size(); ++i) {
        data[i * 4 + 0] = colors[i].r();
        data[i * 4 + 1] = colors[i].g();
        data[i * 4 + 2] = colors[i].b();
        data[i * 4 + 3] = colors[i].a();
    }
    return data;
}

} // namespace

// ----------------------------------------------------------------------------
// LutTexture
// ----------------------------------------------------------------------------

LutTexture::~LutTexture()
{
    Reset();
}

void LutTexture::Upload(const float* data, size_t texelCount, bool nearestFilter)
{
    if (data == nullptr || texelCount == 0)
        return;

    if (_textureId == 0)
        glsafe(::glGenTextures(1, &_textureId));
    glsafe(::glActiveTexture(GL_TEXTURE0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _textureId));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, GLsizei(texelCount), 1, 0, GL_RGBA, GL_FLOAT, data));
    const GLint filter = nearestFilter ? GL_NEAREST : GL_LINEAR;
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    _texelCount = texelCount;
}

void LutTexture::Upload(const std::vector<float>& data, size_t texelCount, bool nearestFilter)
{
    Upload(data.data(), texelCount, nearestFilter);
}

void LutTexture::Bind(unsigned int stage) const
{
    if (_textureId == 0)
        return;

    glsafe(::glActiveTexture(GL_TEXTURE0 + stage));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _textureId));
}

void LutTexture::Reset()
{
    if (_textureId != 0) {
        glsafe(::glDeleteTextures(1, &_textureId));
        _textureId = 0;
    }
    _texelCount = 0;
}

// ----------------------------------------------------------------------------
// InstanceMesh
// ----------------------------------------------------------------------------

InstanceMesh::~InstanceMesh()
{
    Reset();
}

void InstanceMesh::InitUnitPrism()
{
    if (IsInitialized())
        return;

    // 10 zero vertices: the shader computes the real positions from
    // gl_VertexID; only the index pattern matters here
    constexpr size_t VERTEX_COUNT = 10;
    constexpr float VERTICES[VERTEX_COUNT * 3] = { 0.0f };

    // start diamond v0..v3 (top/right/bottom/left), end diamond v4..v7,
    // miter fill v8..v9
    const unsigned int INDICES[] = {
        // start cap
        0, 2, 1,   0, 3, 2,
        // sides
        0, 1, 5,   0, 5, 4,
        1, 2, 6,   1, 6, 5,
        2, 3, 7,   2, 7, 6,
        3, 0, 4,   3, 4, 7,
        // end cap
        4, 5, 6,   4, 6, 7,
        // miter fill
        0, 3, 8,   8, 3, 2,
        0, 9, 1,   1, 9, 2,
    };
    _indexCount = sizeof(INDICES) / sizeof(INDICES[0]);
    _hasAttributes = false;

    glsafe(::glGenBuffers(1, &_vertexBufferId));
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, _vertexBufferId));
    glsafe(::glBufferData(GL_ARRAY_BUFFER, sizeof(VERTICES), VERTICES, GL_STATIC_DRAW));
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));

    glsafe(::glGenBuffers(1, &_indexBufferId));
    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _indexBufferId));
    glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(INDICES), INDICES, GL_STATIC_DRAW));
    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
}

void InstanceMesh::InitDiamond(unsigned int resolution)
{
    if (IsInitialized())
        return;

    GLModel::Geometry geometry = diamond(resolution);

    _indexCount = geometry.indices_count();
    _hasAttributes = true;
    _vertexStrideBytes = GLModel::Geometry::vertex_stride_bytes(geometry.format);

    glsafe(::glGenBuffers(1, &_vertexBufferId));
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, _vertexBufferId));
    glsafe(::glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(geometry.vertices.size() * sizeof(float)),
        geometry.vertices.data(), GL_STATIC_DRAW));
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));

    glsafe(::glGenBuffers(1, &_indexBufferId));
    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _indexBufferId));
    glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(geometry.indices.size() * sizeof(unsigned int)),
        geometry.indices.data(), GL_STATIC_DRAW));
    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
}

void InstanceMesh::DrawInstanced(unsigned int instanceCount)
{
    if (!IsInitialized() || instanceCount == 0)
        return;

    int positionId = -1;
    int normalId = -1;
    if (_hasAttributes) {
        // attribute binding follows the GLModel convention: query the
        // locations from the shader currently in use
        GLShaderProgram* shader = wxGetApp().get_current_shader();
        if (shader == nullptr)
            return;
        positionId = shader->get_attrib_location("v_position");
        normalId = shader->get_attrib_location("v_normal");
        if (positionId == -1)
            return;

        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, _vertexBufferId));
        if (normalId != -1) {
            glsafe(::glVertexAttribPointer(normalId, 3, GL_FLOAT, GL_FALSE, GLsizei(_vertexStrideBytes),
                (const void*)(3 * sizeof(float))));
            glsafe(::glEnableVertexAttribArray(normalId));
        }
        glsafe(::glVertexAttribPointer(positionId, 3, GL_FLOAT, GL_FALSE, GLsizei(_vertexStrideBytes), nullptr));
        glsafe(::glEnableVertexAttribArray(positionId));
    }

    // the prism template has no attributes at all: the geometry comes from
    // gl_VertexID
    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _indexBufferId));
    glsafe(::glDrawElementsInstanced(GL_TRIANGLES, GLsizei(_indexCount), GL_UNSIGNED_INT, nullptr, instanceCount));
    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));

    if (_hasAttributes) {
        if (normalId != -1)
            glsafe(::glDisableVertexAttribArray(normalId));
        glsafe(::glDisableVertexAttribArray(positionId));
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
    }
}

void InstanceMesh::Reset()
{
    if (_vertexBufferId != 0) {
        glsafe(::glDeleteBuffers(1, &_vertexBufferId));
        _vertexBufferId = 0;
    }
    if (_indexBufferId != 0) {
        glsafe(::glDeleteBuffers(1, &_indexBufferId));
        _indexBufferId = 0;
    }
    _indexCount = 0;
}

// ----------------------------------------------------------------------------
// PathRenderer
// ----------------------------------------------------------------------------

PathRenderer::~PathRenderer()
{
    Reset();
}

void PathRenderer::EnsureMeshes()
{
    if (!_prismMesh.IsInitialized())
        _prismMesh.InitUnitPrism();
    if (!_markerMesh.IsInitialized())
        _markerMesh.InitDiamond(16);
}

void PathRenderer::UpdateColorRamps(const ViewSettings& settings)
{
    // gradient ramp for the physical-quantity views (built once)
    if (!_gradientRamp.IsValid())
        _gradientRamp.Upload(BuildGradientData(GCodeViewer::Range_Colors), GRADIENT_TEXELS, false);

    // role palette for the FeatureType view (built once)
    if (!_roleRamp.IsValid()) {
        const std::vector<float> data = BuildPaletteData(GCodeViewer::Extrusion_Role_Colors);
        _roleRamp.Upload(data, GCodeViewer::Extrusion_Role_Colors.size(), true);
    }

    // tool palette for the Tool / ColorPrint views (rebuilt on color change)
    if (!_toolRamp.IsValid() || settings.toolColors != _toolRampColors) {
        _toolRamp.Reset();
        if (!settings.toolColors.empty()) {
            const std::vector<float> data = BuildPaletteData(settings.toolColors);
            _toolRamp.Upload(data, settings.toolColors.size(), true);
            _toolRampColors = settings.toolColors;
        }
    }

    // option palette for the markers (built once)
    if (!_optionRamp.IsValid()) {
        const std::vector<float> data = BuildPaletteData(GCodeViewer::Options_Colors);
        _optionRamp.Upload(data, GCodeViewer::Options_Colors.size(), true);
    }
}

void PathRenderer::Render(PathLayerStack& stack, const GCodeProcessorResult& result, const ViewSettings& settings)
{
    if (stack.LayerCount() == 0)
        return;

    EnsureMeshes();

    GLShaderProgram* shader = wxGetApp().get_shader("gpu_path");
    if (shader == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "PathRenderer::Render: gpu_path shader not found";
        return;
    }

    // rebuild the visibility lists / per-move attributes if anything changed
    stack.RefreshVisibleSteps();
    stack.RefreshMoveAttributes(result);

    UpdateColorRamps(settings);

    Camera& camera = wxGetApp().plater()->get_camera();
    const Transform3d& viewMatrix = camera.get_view_matrix();

    // travel/wipe stay thin prisms exactly like the Bambu pipeline (travel
    // forced to 0.1mm in the tables, wipe 0.05mm from the processor), so the
    // screen-constant line floor is disabled; markers keep a small fixed
    // safety floor for zero-width retraction moves
    const float minLineSize = 0.0f;
    const float markerMinSize = 0.3f;

    const float viewType = static_cast<float>(settings.viewType);
    // value-gradient views: Height..VolumetricRate, LayerTime, LayerTimeLog
    const bool isRangeView = (viewType >= 1.0f && viewType <= 6.0f) || viewType >= 10.0f;

    shader->start_using();
    shader->set_uniform("view_model_matrix", viewMatrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("normal_matrix", (Matrix3d)viewMatrix.matrix().block(0, 0, 3, 3).inverse().transpose());
    shader->set_uniform("emission_factor", 0.25f);

    shader->set_uniform("u_view_config", std::array<float, 4>{
        isRangeView ? 1.0f : 0.0f,
        settings.rangeValid ? 1.0f : 0.0f,
        settings.topLayerOnly ? 1.0f : 0.0f,
        viewType });
    shader->set_uniform("u_range_bounds", std::array<float, 2>{ settings.rangeMin, settings.rangeMax });
    shader->set_uniform("u_range_is_log", (viewType > 10.5f && viewType < 11.5f) ? 1.0f : 0.0f);
    shader->set_uniform("u_min_line_size", minLineSize);

    // the ramp matching the active view: roles (FeatureType), tools
    // (Tool / ColorPrint) or the value gradient
    const LutTexture* ramp = &_gradientRamp;
    float paletteCount = 1.0f;
    if (viewType < 0.5f) {
        ramp = &_roleRamp;
        paletteCount = static_cast<float>(settings.roleColorCount);
    }
    else if (viewType > 6.5f && viewType < 8.5f) {
        ramp = &_toolRamp;
        paletteCount = static_cast<float>(settings.toolColorCount);
    }
    ramp->Bind(STAGE_COLOR_RAMP);
    shader->set_uniform("s_color_ramp", int(STAGE_COLOR_RAMP));
    shader->set_uniform("u_palette_config", std::array<float, 2>{ paletteCount, 0.0f });

    RenderLayers(stack, result, *shader, settings);

    shader->stop_using();

    // option markers (seams, retractions, tool changes, ...) pass
    GLShaderProgram* markerShader = wxGetApp().get_shader("gpu_path_marker");
    if (markerShader != nullptr) {
        markerShader->start_using();
        markerShader->set_uniform("view_model_matrix", viewMatrix);
        markerShader->set_uniform("projection_matrix", camera.get_projection_matrix());
        markerShader->set_uniform("normal_matrix", (Matrix3d)viewMatrix.matrix().block(0, 0, 3, 3).inverse().transpose());
        markerShader->set_uniform("emission_factor", 0.25f);
        markerShader->set_uniform("u_min_marker_size", markerMinSize);
        // sequential playback: lower-layer markers are dimmed like the paths
        markerShader->set_uniform("u_top_layer_only", settings.topLayerOnly ? 1.0f : 0.0f);
        markerShader->set_uniform("u_palette_config", std::array<float, 2>{
            static_cast<float>(GCodeViewer::Options_Colors.size()), 0.0f });

        _optionRamp.Bind(STAGE_COLOR_RAMP);
        markerShader->set_uniform("s_option_ramp", int(STAGE_COLOR_RAMP));

        RenderMarkers(stack, result, *markerShader, settings);

        markerShader->stop_using();
    }
}

void PathRenderer::RenderLayers(PathLayerStack& stack, const GCodeProcessorResult& result,
                                GLShaderProgram& shader, const ViewSettings& /*settings*/)
{
    const auto window = stack.LayerWindow();
    // sequential playback end: the bottom slider is a playback of the
    // CURRENT layer only — it clips the top layer of the window and never
    // affects the layers below (they stay fully drawn, dimmed by the shader
    // while playback is active)
    const uint32_t sidLast = stack.MoveWindow().second;

    // draw from the top layer down
    for (uint32_t li = window.second + 1; li-- > window.first; ) {
        PathLayerData& layer = stack.Layer(li);
        const bool isTop = (li == window.second);
        const unsigned int stepCount = isTop
            ? layer.PathStepCountUpTo(sidLast)
            : static_cast<unsigned int>(layer.PathStepRecordCount());
        if (stepCount == 0)
            continue;

        layer.UploadTables(result);

        shader.set_uniform("u_is_top_layer", isTop ? 1.0f : 0.0f);

        layer.NodeTable().Bind(STAGE_NODE_TABLE);
        shader.set_uniform("s_node_table", int(STAGE_NODE_TABLE));
        layer.WidthHeightTable().Bind(STAGE_WIDTH_HEIGHT_TABLE);
        shader.set_uniform("s_width_height_table", int(STAGE_WIDTH_HEIGHT_TABLE));
        layer.AttributeTable().Bind(STAGE_ATTRIBUTE_TABLE);
        shader.set_uniform("s_attribute_table", int(STAGE_ATTRIBUTE_TABLE));
        layer.PathStepTable().Bind(STAGE_STEP_TABLE);
        shader.set_uniform("s_step_table", int(STAGE_STEP_TABLE));

        _prismMesh.DrawInstanced(stepCount);
    }

    // restore the active texture unit: later passes (imgui, shells) bind
    // their textures without switching the active unit
    glsafe(::glActiveTexture(GL_TEXTURE0));
}

void PathRenderer::RenderMarkers(PathLayerStack& stack, const GCodeProcessorResult& result,
                                 GLShaderProgram& shader, const ViewSettings& /*settings*/)
{
    const auto window = stack.LayerWindow();
    // like the paths, the playback window only clips the markers of the
    // current (top) layer
    const uint32_t sidLast = stack.MoveWindow().second;

    for (uint32_t li = window.second + 1; li-- > window.first; ) {
        PathLayerData& layer = stack.Layer(li);
        const bool isTop = (li == window.second);
        const unsigned int markerCount = isTop
            ? layer.MarkerStepCountUpTo(sidLast)
            : static_cast<unsigned int>(layer.MarkerStepRecordCount());
        if (markerCount == 0)
            continue;

        layer.UploadTables(result);

        shader.set_uniform("u_is_top_layer", isTop ? 1.0f : 0.0f);

        layer.NodeTable().Bind(STAGE_NODE_TABLE);
        shader.set_uniform("s_node_table", int(STAGE_NODE_TABLE));
        layer.WidthHeightTable().Bind(STAGE_WIDTH_HEIGHT_TABLE);
        shader.set_uniform("s_width_height_table", int(STAGE_WIDTH_HEIGHT_TABLE));
        layer.AttributeTable().Bind(STAGE_ATTRIBUTE_TABLE);
        shader.set_uniform("s_attribute_table", int(STAGE_ATTRIBUTE_TABLE));
        layer.MarkerStepTable().Bind(STAGE_STEP_TABLE);
        shader.set_uniform("s_marker_table", int(STAGE_STEP_TABLE));

        _markerMesh.DrawInstanced(markerCount);
    }

    glsafe(::glActiveTexture(GL_TEXTURE0));
}

void PathRenderer::Reset()
{
    _prismMesh.Reset();
    _markerMesh.Reset();
    _gradientRamp.Reset();
    _roleRamp.Reset();
    _toolRamp.Reset();
    _optionRamp.Reset();
    _toolRampColors.clear();
}

} // namespace GUI
} // namespace Slic3r
