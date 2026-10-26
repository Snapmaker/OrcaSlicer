#ifndef slic3r_PathRenderer_hpp_
#define slic3r_PathRenderer_hpp_

#include "libslic3r/Color.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <cstddef>
#include <vector>

namespace Slic3r {
class GLShaderProgram;

namespace GUI {

class PathLayerStack;

// 1D lookup texture (stored as 1 x N RGBA32F) used for the color ramp of the
// active view: value gradients use a 256-texel linear-filtered ramp, indexed
// palettes (roles / tools) use one nearest-filtered texel per color.
class LutTexture
{
public:
    LutTexture() = default;
    ~LutTexture();

    LutTexture(const LutTexture&) = delete;
    LutTexture& operator=(const LutTexture&) = delete;
    LutTexture(LutTexture&&) = delete;
    LutTexture& operator=(LutTexture&&) = delete;

    void Upload(const float* data, size_t texelCount, bool nearestFilter);
    void Upload(const std::vector<float>& data, size_t texelCount, bool nearestFilter);
    void Bind(unsigned int stage) const;
    void Reset();

    bool IsValid() const { return _textureId != 0; }
    size_t TexelCount() const { return _texelCount; }

private:
    unsigned int _textureId{ 0 };
    size_t _texelCount{ 0 };
};

// Template mesh drawn once per path step with instancing: the vertex data is
// irrelevant (all zeros), the real positions are generated in the vertex
// shader from gl_VertexID and the data tables.
class InstanceMesh
{
public:
    InstanceMesh() = default;
    ~InstanceMesh();

    InstanceMesh(const InstanceMesh&) = delete;
    InstanceMesh& operator=(const InstanceMesh&) = delete;
    InstanceMesh(InstanceMesh&&) = delete;
    InstanceMesh& operator=(InstanceMesh&&) = delete;

    // builds the 10-vertex / 48-index unit prism (2 diamonds + miter fill);
    // the vertices are irrelevant (all zeros), the real positions come from
    // gl_VertexID and the data tables
    void InitUnitPrism();
    // builds the marker template mesh (position + normal attributes, e.g.
    // the diamond used for option markers)
    void InitDiamond(unsigned int resolution);
    // draws the mesh `instanceCount` times (one instance per path step or
    // marker); must be called with the matching shader in use
    void DrawInstanced(unsigned int instanceCount);
    void Reset();

    bool IsInitialized() const { return _indexBufferId != 0; }

private:
    unsigned int _vertexBufferId{ 0 };
    unsigned int _indexBufferId{ 0 };
    size_t _indexCount{ 0 };
    // meshes with real vertex attributes (markers) need pointer setup at
    // draw time; attribute-less templates (the prism) only need the indices
    bool _hasAttributes{ false };
    size_t _vertexStrideBytes{ 0 };
};

// Draws the toolpaths of a PathLayerStack with GPU-generated geometry (see
// resources/shaders/140/gpu_path.*). Per frame, the CPU only maintains the
// visibility tables and decides which layers/steps are drawn; every vertex
// and every color lookup happens in the shaders.
class PathRenderer
{
public:
    // View-dependent inputs computed by the viewer (the same data its legend
    // uses), kept in a plain struct to avoid a header dependency on it.
    struct ViewSettings
    {
        unsigned int viewType{ 0 };          // GCodeViewer::EViewType index
        std::vector<ColorRGBA> toolColors;   // already adjusted for rendering
        float rangeMin{ 0.0f };
        float rangeMax{ 0.0f };              // pre-logged for LayerTimeLog
        bool rangeValid{ false };
        bool topLayerOnly{ false };          // sequential playback active
        size_t roleColorCount{ 1 };
        size_t toolColorCount{ 0 };
    };

    PathRenderer() = default;
    ~PathRenderer();

    PathRenderer(const PathRenderer&) = delete;
    PathRenderer& operator=(const PathRenderer&) = delete;

    // Renders all layers inside the stack's layer window. Must be called
    // with an active GL context (e.g. from GCodeViewer::render_toolpaths()).
    void Render(PathLayerStack& stack, const GCodeProcessorResult& result, const ViewSettings& settings);

    void Reset();

private:
    void EnsureMeshes();
    void UpdateColorRamps(const ViewSettings& settings);
    void RenderLayers(PathLayerStack& stack, const GCodeProcessorResult& result,
                      GLShaderProgram& shader, const ViewSettings& settings);
    void RenderMarkers(PathLayerStack& stack, const GCodeProcessorResult& result,
                       GLShaderProgram& shader, const ViewSettings& settings);

    InstanceMesh _prismMesh;
    InstanceMesh _markerMesh;
    LutTexture _gradientRamp;
    LutTexture _roleRamp;
    LutTexture _toolRamp;
    LutTexture _optionRamp;
    std::vector<ColorRGBA> _toolRampColors; // colors the tool ramp was built from
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_PathRenderer_hpp_
