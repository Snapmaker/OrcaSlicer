#include "libslic3r/libslic3r.h"
#include "SoftShadowRenderer.hpp"

#include "3DScene.hpp"
#include "Camera.hpp"
#include "GLShader.hpp"
#include "GUI_App.hpp"
#include "OpenGLManager.hpp"
#include "PartPlate.hpp"

#include <GL/glew.h>

#include <boost/functional/hash.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace Slic3r {
namespace GUI {

namespace {

// Direction towards the light in world space, tilted ~36 deg so the shadow falls towards the default camera.
const Vec3d TO_LIGHT_DIR = Vec3d(-0.4, 0.6, 1.0).normalized();
// Light size (penumbra width per mm of blocker height), shared by the ground and object shadows;
// small enough that thin features (spikes) keep a tapering shadow
constexpr float LIGHT_SPREAD = 0.02f;
constexpr double MAX_SEARCH_MM = 25.0;
// Narrowest penumbra in shadow map texels; wider values (3) caused jagged rims and dark spots in concave corners
constexpr double MIN_PENUMBRA_TEXELS = 1.5;
// Meshes above this triangle count always use their coarsest ready LOD in the shadow map
constexpr std::size_t SHADOW_LOD_MIN_TRIANGLES = 200000;
// While dragging the mask uses fewer samples only when a full quality mask costs more than this
constexpr float DRAG_MASK_BUDGET_MS = 3.0f;
constexpr double DEPTH_BIAS_MM = 0.05;
constexpr double FRUSTUM_PADDING_MM = 1.0;
// Room around the casters while dragging, so the frozen light frustum rarely has to be refitted
constexpr double FROZEN_FRUSTUM_MARGIN_MM = 20.0;
constexpr double FROZEN_FRUSTUM_MARGIN_RATIO = 0.25;
constexpr double GROUND_Z = 0.0;
constexpr unsigned int SHADOW_MAP_SIZE = 2048;
constexpr unsigned int MASK_SIZE = 1024;
// Smallest area covered by the shadow map; fixed instead of size x 0.1 mm, which grew with the map size
// and cancelled the gain of a larger map in small scenes
constexpr double MIN_SHADOW_EXTENT_MM = 200.0;
constexpr double MIN_MASK_TEXEL_MM = 0.15;
constexpr double FOOTPRINT_MARGIN_TEXELS = 2.0;
// Mask texels added to the shadow reach for linear filtering and rounding
constexpr double SHADOW_MARGIN_FILTER_TEXELS = 1.0;
constexpr int BLOCKER_SAMPLES_HQ = 16;
constexpr int PCF_SAMPLES_HQ = 32;
// While dragging the mask uses fewer samples, only when the full quality mask is measured to be expensive
constexpr int BLOCKER_SAMPLES_LQ = 8;
constexpr int PCF_SAMPLES_LQ = 16;
// Object shadows are shaded at half resolution and upsampled by depth and normal
constexpr int OBJECT_SHADOW_RESOLUTION_SCALE = 2;
constexpr float RECEIVER_Z = -0.01f;
constexpr float STRENGTH_LIGHT = 0.55f;
constexpr float STRENGTH_DARK = 0.75f;
constexpr float GPU_TIME_SMOOTHING = 0.1f;
// Object shadows run once per screen pixel every frame, so they use fewer samples and a lighter shadow
// Denser PCF averages out the shadow map texel staircase on sharp object shadows
constexpr int OBJECT_BLOCKER_SAMPLES = 12;
constexpr int OBJECT_PCF_SAMPLES = 24;
constexpr double OBJECT_NORMAL_OFFSET_TEXELS = 3.0;
// Pixels added around the projected object bounds; normals read neighbouring depth pixels
constexpr int OBJECT_RECT_MARGIN = 4;
constexpr double OBJECT_DEPTH_BIAS_MM = 0.1;
// Extra normal offset per mm of view depth, covering the error of positions reconstructed from 24-bit depth
constexpr float OBJECT_RECONSTRUCT_OFFSET = 0.0005f;
// Relative view depth difference above which a half resolution sample is not blended across a silhouette
constexpr float UPSAMPLE_DEPTH_TOLERANCE = 0.02f;
// Steepest receiver slope (world dz per dxy in light space) used by the receiver plane depth bias
constexpr double OBJECT_MAX_RECEIVER_SLOPE = 5.0;
constexpr float OBJECT_STRENGTH_LIGHT = 0.45f;
constexpr float OBJECT_STRENGTH_DARK = 0.45f;
constexpr int GUARDED_TEXTURE_UNITS = 4;
// Tiles per axis of the local top map (about 5 to 10 mm per tile for usual plates)
constexpr unsigned int LOCAL_TOP_SIZE = 32;
// Intermediate size of the two-step GPU min reduction
constexpr unsigned int LOCAL_TOP_MID_SIZE = 256;
// Widest ground mask blur spacing in texels, used inside wide penumbrae to remove grain
constexpr float MASK_BLUR_MAX_STEP = 4.0f;

/** @brief OpenGL states touched by the soft shadow passes, restored on destruction. */
class ShadowStateGuard
{
public:
    ShadowStateGuard()
    {
        _blend = glIsEnabled(GL_BLEND);
        _cullFace = glIsEnabled(GL_CULL_FACE);
        _depthTest = glIsEnabled(GL_DEPTH_TEST);
        _scissorTest = glIsEnabled(GL_SCISSOR_TEST);
        glsafe(::glGetIntegerv(GL_SCISSOR_BOX, _scissorBox.data()));
        glsafe(::glGetBooleanv(GL_DEPTH_WRITEMASK, &_depthMask));
        glsafe(::glGetBooleanv(GL_COLOR_WRITEMASK, _colorMask.data()));
        glsafe(::glGetFloatv(GL_COLOR_CLEAR_VALUE, _clearColor.data()));
        glsafe(::glGetIntegerv(GL_DEPTH_FUNC, &_depthFunc));
        glsafe(::glGetIntegerv(GL_FRONT_FACE, &_frontFace));
        glsafe(::glGetIntegerv(GL_CULL_FACE_MODE, &_cullFaceMode));
        glsafe(::glGetIntegerv(GL_BLEND_SRC_RGB, &_blendSrc));
        glsafe(::glGetIntegerv(GL_BLEND_DST_RGB, &_blendDst));
        glsafe(::glGetIntegerv(GL_VIEWPORT, _viewport.data()));
        glsafe(::glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &_drawFramebuffer));
        glsafe(::glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &_readFramebuffer));
        glsafe(::glGetIntegerv(GL_ACTIVE_TEXTURE, &_activeTexture));
        for (int unit = 0; unit < GUARDED_TEXTURE_UNITS; ++unit)
        {
            glsafe(::glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit)));
            glsafe(::glGetIntegerv(GL_TEXTURE_BINDING_2D, &_textures[unit]));
        }
        glsafe(::glActiveTexture(static_cast<GLenum>(_activeTexture)));
    }

    ~ShadowStateGuard()
    {
        SetEnabled(GL_BLEND, _blend);
        SetEnabled(GL_CULL_FACE, _cullFace);
        SetEnabled(GL_DEPTH_TEST, _depthTest);
        SetEnabled(GL_SCISSOR_TEST, _scissorTest);
        glsafe(::glScissor(_scissorBox[0], _scissorBox[1], _scissorBox[2], _scissorBox[3]));
        glsafe(::glDepthMask(_depthMask));
        glsafe(::glColorMask(_colorMask[0], _colorMask[1], _colorMask[2], _colorMask[3]));
        glsafe(::glClearColor(_clearColor[0], _clearColor[1], _clearColor[2], _clearColor[3]));
        glsafe(::glDepthFunc(static_cast<GLenum>(_depthFunc)));
        glsafe(::glFrontFace(static_cast<GLenum>(_frontFace)));
        glsafe(::glCullFace(static_cast<GLenum>(_cullFaceMode)));
        glsafe(::glBlendFunc(static_cast<GLenum>(_blendSrc), static_cast<GLenum>(_blendDst)));
        glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(_drawFramebuffer)));
        glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(_readFramebuffer)));
        glsafe(::glViewport(_viewport[0], _viewport[1], _viewport[2], _viewport[3]));
        for (int unit = 0; unit < GUARDED_TEXTURE_UNITS; ++unit)
        {
            glsafe(::glBindSampler(static_cast<GLuint>(unit), 0));
            glsafe(::glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit)));
            glsafe(::glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(_textures[unit])));
        }
        glsafe(::glActiveTexture(static_cast<GLenum>(_activeTexture)));
    }

    ShadowStateGuard(const ShadowStateGuard&) = delete;
    ShadowStateGuard& operator=(const ShadowStateGuard&) = delete;

private:
    static void SetEnabled(GLenum cap, GLboolean enabled)
    {
        if (enabled == GL_TRUE)
            glsafe(::glEnable(cap));
        else
            glsafe(::glDisable(cap));
    }

    GLboolean _blend{ GL_FALSE };
    GLboolean _cullFace{ GL_FALSE };
    GLboolean _depthTest{ GL_FALSE };
    GLboolean _scissorTest{ GL_FALSE };
    std::array<GLint, 4> _scissorBox{ { 0, 0, 0, 0 } };
    GLboolean _depthMask{ GL_TRUE };
    std::array<GLboolean, 4> _colorMask{ { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE } };
    std::array<GLfloat, 4> _clearColor{ { 0.0f, 0.0f, 0.0f, 0.0f } };
    GLint _depthFunc{ GL_LESS };
    GLint _frontFace{ GL_CCW };
    GLint _cullFaceMode{ GL_BACK };
    GLint _blendSrc{ GL_SRC_ALPHA };
    GLint _blendDst{ GL_ONE_MINUS_SRC_ALPHA };
    std::array<GLint, 4> _viewport{ { 0, 0, 0, 0 } };
    GLint _drawFramebuffer{ 0 };
    GLint _readFramebuffer{ 0 };
    GLint _activeTexture{ GL_TEXTURE0 };
    std::array<GLint, GUARDED_TEXTURE_UNITS> _textures{ { 0, 0, 0, 0 } };
};

/** @brief Texture storage matching the default framebuffer depth, required by glBlitFramebuffer. */
struct DepthCopyFormat
{
    GLint internalFormat{ 0 };
    GLenum format{ GL_NONE };
    GLenum type{ GL_NONE };
    GLenum attachment{ GL_NONE };
};

/**
 * @brief Queries the depth/stencil layout of the default framebuffer bound for reading.
 * @return false when the layout has no matching texture format.
 */
bool QueryDefaultDepthFormat(DepthCopyFormat& out)
{
    GLint depthType = GL_NONE;
    glsafe(::glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_DEPTH, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &depthType));
    if (depthType == GL_NONE)
        return false;

    GLint depthBits = 0;
    GLint componentType = GL_NONE;
    glsafe(::glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_DEPTH, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depthBits));
    glsafe(::glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_DEPTH, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &componentType));

    GLint stencilType = GL_NONE;
    GLint stencilBits = 0;
    glsafe(::glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_STENCIL, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &stencilType));
    if (stencilType != GL_NONE)
        glsafe(::glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_STENCIL, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencilBits));
    if (stencilBits != 0 && stencilBits != 8)
        return false;

    const bool hasStencil = stencilBits == 8;
    if (componentType == GL_FLOAT && depthBits == 32)
    {
        out.internalFormat = hasStencil ? GL_DEPTH32F_STENCIL8 : GL_DEPTH_COMPONENT32F;
        out.format = hasStencil ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT;
        out.type = hasStencil ? GL_FLOAT_32_UNSIGNED_INT_24_8_REV : GL_FLOAT;
    }
    else if (depthBits == 24)
    {
        out.internalFormat = hasStencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24;
        out.format = hasStencil ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT;
        out.type = hasStencil ? GL_UNSIGNED_INT_24_8 : GL_UNSIGNED_INT;
    }
    else if ((depthBits == 32 || depthBits == 16) && !hasStencil)
    {
        out.internalFormat = depthBits == 32 ? GL_DEPTH_COMPONENT32 : GL_DEPTH_COMPONENT16;
        out.format = GL_DEPTH_COMPONENT;
        out.type = GL_UNSIGNED_INT;
    }
    else
    {
        return false;
    }
    out.attachment = hasStencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
    return true;
}

/** @brief Grows a rectangle by margin pixels and clamps it to [0, width) x [0, height). */
SoftShadowPixelRect ExpandRect(const SoftShadowPixelRect& rect, int margin, int width, int height)
{
    SoftShadowPixelRect result;
    result.x0 = std::max(0, rect.x0 - margin);
    result.y0 = std::max(0, rect.y0 - margin);
    result.x1 = std::min(width, rect.x1 + margin);
    result.y1 = std::min(height, rect.y1 + margin);
    return result;
}

/**
 * @brief Projects a world box to viewport pixels.
 * @return The covered rectangle, the whole viewport when a corner is behind the camera, empty when off screen.
 */
SoftShadowPixelRect ProjectBoxToScreen(const BoundingBoxf3& box, const Matrix4d& viewProjection, int width, int height)
{
    SoftShadowPixelRect full;
    full.x1 = width;
    full.y1 = height;

    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();
    for (int i = 0; i < 8; ++i)
    {
        const Vec4d corner((i & 1) != 0 ? box.max.x() : box.min.x(), (i & 2) != 0 ? box.max.y() : box.min.y(),
                           (i & 4) != 0 ? box.max.z() : box.min.z(), 1.0);
        const Vec4d clip = viewProjection * corner;
        if (clip.w() <= 1.0e-6)
            return full;

        const double px = (clip.x() / clip.w() * 0.5 + 0.5) * static_cast<double>(width);
        const double py = (clip.y() / clip.w() * 0.5 + 0.5) * static_cast<double>(height);
        minX = std::min(minX, px);
        minY = std::min(minY, py);
        maxX = std::max(maxX, px);
        maxY = std::max(maxY, py);
    }

    SoftShadowPixelRect rect;
    rect.x0 = static_cast<int>(std::floor(std::max(minX, 0.0)));
    rect.y0 = static_cast<int>(std::floor(std::max(minY, 0.0)));
    rect.x1 = static_cast<int>(std::ceil(std::min(maxX, static_cast<double>(width))));
    rect.y1 = static_cast<int>(std::ceil(std::min(maxY, static_cast<double>(height))));
    return rect;
}

/** @brief Returns true when inner lies completely inside outer. */
bool BoxContains(const BoundingBoxf3& outer, const BoundingBoxf3& inner)
{
    return outer.defined && inner.defined && outer.min.x() <= inner.min.x() && outer.min.y() <= inner.min.y() &&
           outer.min.z() <= inner.min.z() && outer.max.x() >= inner.max.x() && outer.max.y() >= inner.max.y() &&
           outer.max.z() >= inner.max.z();
}

/** @brief Returns the 8 corners of a bounding box. */
std::array<Vec3d, 8> GetBoxCorners(const BoundingBoxf3& box)
{
    std::array<Vec3d, 8> corners;
    for (int i = 0; i < 8; ++i)
    {
        corners[i] = Vec3d((i & 1) != 0 ? box.max.x() : box.min.x(),
                           (i & 2) != 0 ? box.max.y() : box.min.y(),
                           (i & 4) != 0 ? box.max.z() : box.min.z());
    }
    return corners;
}

/** @brief Projects a point along the light direction onto the ground plane. */
Vec3d ProjectToGround(const Vec3d& point)
{
    const double t = (point.z() - GROUND_Z) / TO_LIGHT_DIR.z();
    return point - TO_LIGHT_DIR * t;
}

/** @brief Texel sizes in mm: shadow map texel in light space, mask texel on the ground. */
struct ShadowTexels
{
    double shadow{ 0.0 };
    double mask{ 0.0 };
};

/**
 * @brief Ground distance the soft shadow of a caster box can reach beyond its hard shadow.
 * Fix: besides the physical penumbra it covers the minimum PCF radius, the mask blur spread and filtering,
 * all of which grow with the texel sizes of wide layouts; a fixed allowance cut such shadows at the mask edge.
 */
double ShadowReach(const BoundingBoxf3& box, const ShadowTexels& texels)
{
    // Light depth from the box top down to the ground, the largest blocker distance of this caster
    const double depthToGround = std::max(box.max.z() - GROUND_Z, 0.0) / TO_LIGHT_DIR.z();
    const double penumbra = std::min(depthToGround * static_cast<double>(LIGHT_SPREAD), MAX_SEARCH_MM);
    const double pcfRadius = std::max(penumbra, MIN_PENUMBRA_TEXELS * texels.shadow);
    // Blur taps reach two steps out; a step is a quarter of the penumbra, between 1 and MASK_BLUR_MAX_STEP texels
    const double maxBlurStep = static_cast<double>(MASK_BLUR_MAX_STEP) * texels.mask;
    const double blurStep = std::min(std::max(0.25 * penumbra, texels.mask), maxBlurStep);
    // A light space width stretches by 1 / cos(light angle) on the ground
    return pcfRadius / TO_LIGHT_DIR.z() + 2.0 * blurStep + SHADOW_MARGIN_FILTER_TEXELS * texels.mask;
}

/** @brief Returns the ground-plane XY rectangle that may receive the shadow of a box. */
BoundingBoxf GetGroundFootprint(const BoundingBoxf3& box, double margin)
{
    BoundingBoxf footprint;
    const std::array<Vec3d, 8> corners = GetBoxCorners(box);
    for (const Vec3d& corner : corners)
    {
        footprint.merge(Vec2d(corner.x(), corner.y()));
        const Vec3d ground = ProjectToGround(corner);
        footprint.merge(Vec2d(ground.x(), ground.y()));
    }
    footprint.min -= Vec2d(margin, margin);
    footprint.max += Vec2d(margin, margin);
    return footprint;
}

/** @brief Clip planes of an orthographic projection, in the glOrtho convention. */
struct OrthoBounds
{
    double left{ -1.0 };
    double right{ 1.0 };
    double bottom{ -1.0 };
    double top{ 1.0 };
    double nearZ{ -1.0 };
    double farZ{ 1.0 };
};

/** @brief Builds a column-major orthographic projection equal to glOrtho. */
Matrix4d MakeOrtho(const OrthoBounds& b)
{
    Matrix4d m = Matrix4d::Identity();
    m(0, 0) = 2.0 / (b.right - b.left);
    m(1, 1) = 2.0 / (b.top - b.bottom);
    m(2, 2) = -2.0 / (b.farZ - b.nearZ);
    m(0, 3) = -(b.right + b.left) / (b.right - b.left);
    m(1, 3) = -(b.top + b.bottom) / (b.top - b.bottom);
    m(2, 3) = -(b.farZ + b.nearZ) / (b.farZ - b.nearZ);
    return m;
}

/** @brief Creates a 2D texture with the given storage and clamp mode. */
unsigned int CreateTexture2D(GLint internalFormat, GLenum format, GLenum type, unsigned int size)
{
    GLuint texture = 0;
    glsafe(::glGenTextures(1, &texture));
    glsafe(::glBindTexture(GL_TEXTURE_2D, texture));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, static_cast<GLsizei>(size), static_cast<GLsizei>(size), 0,
                          format, type, nullptr));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    return static_cast<unsigned int>(texture);
}

/** @brief Creates a depth sampler; compare enables hardware PCF. */
unsigned int CreateDepthSampler(bool compare)
{
    GLuint sampler = 0;
    const GLfloat border[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glsafe(::glGenSamplers(1, &sampler));
    glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, compare ? GL_LINEAR : GL_NEAREST));
    glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, compare ? GL_LINEAR : GL_NEAREST));
    glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER));
    glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER));
    glsafe(::glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, border));
    glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_MODE, compare ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE));
    glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL));
    return static_cast<unsigned int>(sampler);
}

/** @brief Returns true when the currently bound draw framebuffer is complete. */
bool IsDrawFramebufferComplete()
{
    const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    return status == GL_FRAMEBUFFER_COMPLETE;
}

/** @brief Returns true when a LOD model was generated and handed over for rendering. */
bool IsLodModelReady(const std::shared_ptr<GLModel>& model)
{
    return model != nullptr && !model->is_render_disabled() && model->is_initialized();
}

/** @brief Returns the coarsest LOD model of a volume that is ready, or nullptr when none is. */
GLModel* GetReadyLodModel(const GLVolume& volume)
{
    if (IsLodModelReady(volume.m_modelSmall))
        return volume.m_modelSmall.get();
    if (IsLodModelReady(volume.m_modelMiddle))
        return volume.m_modelMiddle.get();
    return nullptr;
}

/**
 * @brief Picks the shadow map geometry of a volume, independent of the camera driven LOD of the main view.
 * @return The LOD model for heavy meshes, or nullptr to render the full detail geometry.
 */
GLModel* SelectShadowLod(const GLVolume& volume)
{
    // Only heavy meshes: light ones are cheap anyway, and simplification would cut off thin tips
    const bool heavy = volume.model.indices_count() / 3 > SHADOW_LOD_MIN_TRIANGLES;
    return heavy ? GetReadyLodModel(volume) : nullptr;
}

/** @brief Returns true when the volume should cast a shadow. */
bool IsShadowCaster(const GLVolume& volume, bool renderSlaAuxiliaries)
{
    if (!volume.is_active || !volume.visible || volume.is_modifier)
        return false;
    if (!renderSlaAuxiliaries && volume.composite_id.volume_id < 0)
        return false;
    if (volume.force_transparent || volume.color.is_transparent())
        return false;
    return true;
}

} // namespace

void SoftShadowGpuTimer::Poll()
{
    for (std::size_t i = 0; i < _queries.size(); ++i)
    {
        if (!_pending[i] || static_cast<int>(i) == _activeSlot)
            continue;

        GLint available = 0;
        glsafe(::glGetQueryObjectiv(_queries[i], GL_QUERY_RESULT_AVAILABLE, &available));
        if (available == 0)
            continue;

        GLuint64 elapsedNs = 0;
        glsafe(::glGetQueryObjectui64v(_queries[i], GL_QUERY_RESULT, &elapsedNs));
        const float elapsedMs = static_cast<float>(static_cast<double>(elapsedNs) * 1.0e-6);
        // The first sample is taken as is, so decisions based on it do not wait for the average to settle
        if (_timeMs > 0.0f)
            _timeMs += (elapsedMs - _timeMs) * GPU_TIME_SMOOTHING;
        else
            _timeMs = elapsedMs;
        _pending[i] = false;
    }
}

void SoftShadowGpuTimer::Begin()
{
    if (_activeSlot >= 0)
        return;

    if (_queries[0] == 0)
        glsafe(::glGenQueries(static_cast<GLsizei>(_queries.size()), _queries.data()));

    Poll();
    if (_pending[_next])
        return;

    _activeSlot = static_cast<int>(_next);
    glsafe(::glBeginQuery(GL_TIME_ELAPSED, _queries[_next]));
}

void SoftShadowGpuTimer::End()
{
    if (_activeSlot < 0)
        return;

    glsafe(::glEndQuery(GL_TIME_ELAPSED));
    _pending[_activeSlot] = true;
    _next = (_next + 1) % static_cast<unsigned int>(_queries.size());
    _activeSlot = -1;
}

void SoftShadowGpuTimer::Release()
{
    if (_queries[0] != 0)
        glsafe(::glDeleteQueries(static_cast<GLsizei>(_queries.size()), _queries.data()));
    _queries.fill(0);
    _pending.fill(false);
    _next = 0;
    _activeSlot = -1;
    _timeMs = 0.0f;
}

bool SoftShadowRenderer::IsSupported()
{
    if (!wxGetApp().is_gl_version_greater_or_equal_to(3, 3))
        return false;
    if (OpenGLManager::get_framebuffers_type() != OpenGLManager::EFramebufferType::Arb)
        return false;
    return wxGetApp().get_shader("shadow_ground_mask") != nullptr && wxGetApp().get_shader("shadow_receiver") != nullptr;
}

bool SoftShadowRenderer::CreateShadowMapResources()
{
    _shadowDepthTexture = CreateTexture2D(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, _shadowMapSize);
    _rawDepthSampler = CreateDepthSampler(false);
    _compareDepthSampler = CreateDepthSampler(true);

    GLuint framebuffer = 0;
    glsafe(::glGenFramebuffers(1, &framebuffer));
    _shadowFramebuffer = static_cast<unsigned int>(framebuffer);
    // Fix: bound to both targets, so glReadBuffer below applies to this framebuffer instead of the window one
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, framebuffer));
    glsafe(::glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, _shadowDepthTexture, 0));
    glsafe(::glDrawBuffer(GL_NONE));
    glsafe(::glReadBuffer(GL_NONE));
    if (_shadowDepthTexture == 0 || _rawDepthSampler == 0 || _compareDepthSampler == 0 || !IsDrawFramebufferComplete())
        return false;

    _localTopTexture = CreateTexture2D(GL_R32F, GL_RED, GL_FLOAT, LOCAL_TOP_SIZE);
    if (_localTopTexture == 0)
        return false;

    // Render targets of the GPU reduction; without them the shaders search without a local top
    GLuint localTopFramebuffer = 0;
    glsafe(::glGenFramebuffers(1, &localTopFramebuffer));
    _localTopFramebuffer = static_cast<unsigned int>(localTopFramebuffer);
    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, localTopFramebuffer));
    glsafe(::glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _localTopTexture, 0));
    const bool localTopComplete = IsDrawFramebufferComplete();

    _localTopMidTexture = CreateTexture2D(GL_R32F, GL_RED, GL_FLOAT, LOCAL_TOP_MID_SIZE);
    GLuint localTopMidFramebuffer = 0;
    glsafe(::glGenFramebuffers(1, &localTopMidFramebuffer));
    _localTopMidFramebuffer = static_cast<unsigned int>(localTopMidFramebuffer);
    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, localTopMidFramebuffer));
    glsafe(::glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _localTopMidTexture, 0));
    if (!localTopComplete || _localTopMidTexture == 0 || !IsDrawFramebufferComplete())
    {
        BOOST_LOG_TRIVIAL(warning) << "Soft shadows: GPU local top reduction unavailable, searching without it";
        if (_localTopFramebuffer != 0)
            glsafe(::glDeleteFramebuffers(1, &_localTopFramebuffer));
        if (_localTopMidFramebuffer != 0)
            glsafe(::glDeleteFramebuffers(1, &_localTopMidFramebuffer));
        if (_localTopMidTexture != 0)
            glsafe(::glDeleteTextures(1, &_localTopMidTexture));
        _localTopFramebuffer = 0;
        _localTopMidFramebuffer = 0;
        _localTopMidTexture = 0;
    }
    return true;
}

bool SoftShadowRenderer::CreateMaskResources()
{
    // R = visibility, G = blur step derived from the penumbra width
    _maskTexture = CreateTexture2D(GL_RG8, GL_RG, GL_UNSIGNED_BYTE, _maskSize);

    GLuint framebuffer = 0;
    glsafe(::glGenFramebuffers(1, &framebuffer));
    _maskFramebuffer = static_cast<unsigned int>(framebuffer);
    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer));
    glsafe(::glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _maskTexture, 0));
    // Fix: with a depth buffer, overlapping instance footprints shade each mask texel only once
    GLuint depthBuffer = 0;
    glsafe(::glGenRenderbuffers(1, &depthBuffer));
    _maskDepthRenderbuffer = static_cast<unsigned int>(depthBuffer);
    glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, depthBuffer));
    glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, static_cast<GLsizei>(_maskSize),
                                   static_cast<GLsizei>(_maskSize)));
    glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, 0));
    glsafe(::glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthBuffer));
    if (_maskTexture == 0 || !IsDrawFramebufferComplete())
        return false;

    _maskBlurTexture = CreateTexture2D(GL_RG8, GL_RG, GL_UNSIGNED_BYTE, _maskSize);
    GLuint blurFramebuffer = 0;
    glsafe(::glGenFramebuffers(1, &blurFramebuffer));
    _maskBlurFramebuffer = static_cast<unsigned int>(blurFramebuffer);
    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blurFramebuffer));
    glsafe(::glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _maskBlurTexture, 0));
    return _maskBlurTexture != 0 && IsDrawFramebufferComplete();
}

bool SoftShadowRenderer::EnsureResources()
{
    if (_shadowFramebuffer != 0 && _maskFramebuffer != 0)
        return true;
    if (_resourcesFailed)
        return false;

    GLint maxTextureSize = 0;
    glsafe(::glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize));
    const unsigned int maxSize = static_cast<unsigned int>(std::max(maxTextureSize, 0));
    _shadowMapSize = std::min(SHADOW_MAP_SIZE, maxSize);
    _maskSize = std::min(MASK_SIZE, maxSize);
    if (_shadowMapSize < 512 || _maskSize < 512)
    {
        _resourcesFailed = true;
        BOOST_LOG_TRIVIAL(warning) << "Soft shadows disabled: max texture size " << maxTextureSize << " is too small";
        return false;
    }

    bool complete = false;
    {
        ShadowStateGuard stateGuard;
        complete = CreateShadowMapResources() && CreateMaskResources();
    }

    if (!complete)
    {
        Release();
        _resourcesFailed = true;
        BOOST_LOG_TRIVIAL(warning) << "Soft shadows disabled: unable to create complete framebuffers";
        return false;
    }
    return true;
}

bool SoftShadowRenderer::CollectCasters(const SoftShadowFrameInput& input)
{
    _casters.clear();
    _instanceBoxes.clear();
    _casterBox = BoundingBoxf3();

    std::map<std::pair<int, int>, std::size_t> instanceIndices;
    for (GLVolume* volume : input.volumes->volumes)
    {
        if (volume == nullptr || !IsShadowCaster(*volume, input.renderSlaAuxiliaries))
            continue;

        const BoundingBoxf3& box = volume->transformed_bounding_box();
        if (!box.defined)
            continue;

        _casters.push_back(volume);
        _casterBox.merge(box);

        const std::pair<int, int> key(volume->composite_id.object_id, volume->composite_id.instance_id);
        const std::map<std::pair<int, int>, std::size_t>::const_iterator it = instanceIndices.find(key);
        if (it == instanceIndices.end())
        {
            instanceIndices.emplace(key, _instanceBoxes.size());
            _instanceBoxes.push_back(box);
        }
        else
        {
            _instanceBoxes[it->second].merge(box);
        }
    }
    return !_casters.empty();
}

std::size_t SoftShadowRenderer::ComputeSignature() const
{
    // Fix: the drag state is not hashed, so a click without moving no longer rebuilds twice;
    // the end of a real drag is still caught by the settled check in Update()
    std::size_t seed = 0;
    boost::hash_combine(seed, _casters.size());
    for (const GLVolume* volume : _casters)
    {
        boost::hash_combine(seed, volume);
        const Transform3d world = volume->world_matrix();
        for (int i = 0; i < 16; ++i)
            boost::hash_combine(seed, world.data()[i]);

        const BoundingBoxf3& box = volume->transformed_bounding_box();
        for (int i = 0; i < 3; ++i)
        {
            boost::hash_combine(seed, box.min[i]);
            boost::hash_combine(seed, box.max[i]);
        }
        boost::hash_combine(seed, volume->model.indices_count());
        // A LOD finishing in the background changes the shadow geometry and must trigger a rebuild
        boost::hash_combine(seed, static_cast<const void*>(SelectShadowLod(*volume)));
    }
    return seed;
}

void SoftShadowRenderer::FitLightFrustum(const BoundingBoxf3& fitBox)
{
    // Light basis: view +Z points towards the light, so the light looks down -Z.
    const Vec3d axisZ = TO_LIGHT_DIR;
    const Vec3d axisX = Vec3d::UnitY().cross(axisZ).normalized();
    const Vec3d axisY = axisZ.cross(axisX);
    Matrix3d rotation;
    rotation.row(0) = axisX.transpose();
    rotation.row(1) = axisY.transpose();
    rotation.row(2) = axisZ.transpose();
    _lightView = Transform3d::Identity();
    _lightView.linear() = rotation;

    BoundingBoxf3 lightBox;
    const std::array<Vec3d, 8> corners = GetBoxCorners(fitBox);
    for (const Vec3d& corner : corners)
    {
        lightBox.merge(Vec3d(_lightView * corner));
        lightBox.merge(Vec3d(_lightView * ProjectToGround(corner)));
    }

    // The margin depends on the texel sizes, which depend on the area including the margin. It is small
    // against the area, so two refinements from a zero margin converge; the tallest caster bounds all reaches
    const BoundingBoxf hardShadow = GetGroundFootprint(fitBox, 0.0);
    const double maskMinExtent = static_cast<double>(_maskSize) * MIN_MASK_TEXEL_MM;
    double reach = 0.0;
    for (int i = 0; i < 2; ++i)
    {
        ShadowTexels texels;
        texels.mask = std::max(std::max(hardShadow.size().x(), hardShadow.size().y()) + 2.0 * reach, maskMinExtent) /
                      static_cast<double>(_maskSize);
        texels.shadow = (std::max({ lightBox.size().x(), lightBox.size().y(), MIN_SHADOW_EXTENT_MM }) + 2.0 * reach) /
                        static_cast<double>(_shadowMapSize);
        // Same margin as the footprints, so no footprint reaches past the mask
        reach = ShadowReach(fitBox, texels) + FOOTPRINT_MARGIN_TEXELS * texels.mask;
    }

    const BoundingBoxf ground = GetGroundFootprint(fitBox, reach);
    const double maskSide = std::max({ ground.size().x(), ground.size().y(), maskMinExtent });
    const Vec2d maskCenter = ground.center();
    _maskRect = { { static_cast<float>(maskCenter.x() - 0.5 * maskSide), static_cast<float>(maskCenter.y() - 0.5 * maskSide),
                    static_cast<float>(maskCenter.x() + 0.5 * maskSide), static_cast<float>(maskCenter.y() + 0.5 * maskSide) } };

    // Every ground texel of the mask must lie inside the depth range, otherwise it reads as fully occluded
    for (int i = 0; i < 4; ++i)
    {
        const Vec3d groundCorner(_maskRect[(i & 1) != 0 ? 2 : 0], _maskRect[(i & 2) != 0 ? 3 : 1], GROUND_Z);
        const double lightZ = (_lightView * groundCorner).z();
        lightBox.min.z() = std::min(lightBox.min.z(), lightZ);
        lightBox.max.z() = std::max(lightBox.max.z(), lightZ);
    }

    // The padding keeps the mask margin inside the map (ground distances only shrink in light space)
    const double side = std::max({ lightBox.size().x(), lightBox.size().y(), MIN_SHADOW_EXTENT_MM }) + 2.0 * reach;
    const Vec3d center = lightBox.center();
    OrthoBounds bounds;
    bounds.left = center.x() - 0.5 * side;
    bounds.right = center.x() + 0.5 * side;
    bounds.bottom = center.y() - 0.5 * side;
    bounds.top = center.y() + 0.5 * side;
    bounds.nearZ = -lightBox.max.z() - FRUSTUM_PADDING_MM;
    bounds.farZ = -lightBox.min.z() + FRUSTUM_PADDING_MM;
    _lightProjection = MakeOrtho(bounds);
    _lightExtent = Vec2f(static_cast<float>(side), static_cast<float>(side));
    _depthSpan = static_cast<float>(bounds.farZ - bounds.nearZ);
}

void SoftShadowRenderer::UpdateLightFrustum(bool interactive)
{
    if (!interactive)
    {
        _frustumFrozen = false;
        FitLightFrustum(_casterBox);
        return;
    }

    // Keep the frustum of the drag start while the casters stay inside it
    if (_frustumFrozen && BoxContains(_frozenFitBox, _casterBox))
        return;

    const Vec3d size = _casterBox.size();
    const double margin = std::max(FROZEN_FRUSTUM_MARGIN_MM, FROZEN_FRUSTUM_MARGIN_RATIO * std::max(size.x(), size.y()));
    _frozenFitBox = _casterBox;
    _frozenFitBox.min -= Vec3d(margin, margin, margin);
    _frozenFitBox.max += Vec3d(margin, margin, margin);
    _frozenFitBox.min.z() = _casterBox.min.z();
    FitLightFrustum(_frozenFitBox);
    _frustumFrozen = true;
}

void SoftShadowRenderer::BuildLocalTopMap()
{
    // Fix: exact reduction also while dragging; caster boxes spread the top of a tall part over its whole box
    // and left holes in object shadows. Without the reduction the shaders search without a local top.
    _localTopValid = _localTopTexture != 0 && BuildLocalTopMapFromShadowMap();
}

bool SoftShadowRenderer::BuildLocalTopMapFromShadowMap()
{
    GLShaderProgram* const shader = wxGetApp().get_shader("shadow_depth_min");
    if (shader == nullptr || _localTopFramebuffer == 0 || _localTopMidFramebuffer == 0 ||
        _shadowMapSize < LOCAL_TOP_MID_SIZE)
        return false;

    EnsureFullscreenQuad();
    glsafe(::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glActiveTexture(GL_TEXTURE0));
    glsafe(::glBindSampler(0, 0));

    shader->start_using();
    shader->set_uniform("shadow_depth", 0);
    // Step 1: shadow map -> 256x256, small blocks so many fragments run in parallel
    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _localTopMidFramebuffer));
    glsafe(::glViewport(0, 0, static_cast<GLsizei>(LOCAL_TOP_MID_SIZE), static_cast<GLsizei>(LOCAL_TOP_MID_SIZE)));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _shadowDepthTexture));
    shader->set_uniform("block_size", static_cast<int>(_shadowMapSize / LOCAL_TOP_MID_SIZE));
    _fullscreenQuad.render();

    // Step 2: 256x256 -> 32x32
    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _localTopFramebuffer));
    glsafe(::glViewport(0, 0, static_cast<GLsizei>(LOCAL_TOP_SIZE), static_cast<GLsizei>(LOCAL_TOP_SIZE)));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _localTopMidTexture));
    shader->set_uniform("block_size", static_cast<int>(LOCAL_TOP_MID_SIZE / LOCAL_TOP_SIZE));
    _fullscreenQuad.render();
    shader->stop_using();
    return true;
}

bool SoftShadowRenderer::RenderShadowMap()
{
    GLShaderProgram* const shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return false;

    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _shadowFramebuffer));
    glsafe(::glViewport(0, 0, static_cast<GLsizei>(_shadowMapSize), static_cast<GLsizei>(_shadowMapSize)));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE));
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthFunc(GL_LESS));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));

    shader->start_using();
    shader->set_uniform("projection_matrix", _lightProjection);
    // Performance: render() re-reads the filament colors from the config per volume, read them once per map
    ModelObjectPtrs& modelObjects = wxGetApp().model().objects;
    std::vector<ColorRGBA> extruderColors = ::get_extruders_colors();
    for (GLVolume* volume : _casters)
    {
        shader->set_uniform("view_model_matrix", _lightView * volume->world_matrix());
        // Fix: the geometry no longer follows the camera LOD, so a cached map never depends on an earlier view
        GLModel* const lodModel = SelectShadowLod(*volume);
        if (lodModel != nullptr)
        {
            lodModel->render();
            continue;
        }
        // simple_render picks the camera LOD level for unpainted meshes; force full detail (painted ones ignore it)
        const LODLevel cameraLod = volume->m_curLodLevel;
        volume->m_curLodLevel = LODLevel::High;
        volume->simple_render(shader, modelObjects, extruderColors);
        volume->m_curLodLevel = cameraLod;
    }
    shader->stop_using();

    glsafe(::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
    return true;
}

void SoftShadowRenderer::BuildFootprintModel()
{
    GLModel::Geometry geometry;
    geometry.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    geometry.reserve_vertices(4 * _instanceBoxes.size());
    geometry.reserve_indices(6 * _instanceBoxes.size());

    ShadowTexels texels;
    texels.mask = static_cast<double>(_maskRect[2] - _maskRect[0]) / static_cast<double>(_maskSize);
    texels.shadow = static_cast<double>(_lightExtent.x()) / static_cast<double>(_shadowMapSize);
    unsigned int vertexId = 0;
    for (const BoundingBoxf3& box : _instanceBoxes)
    {
        // Each instance only needs room for its own soft shadow
        const BoundingBoxf footprint = GetGroundFootprint(box, ShadowReach(box, texels) + FOOTPRINT_MARGIN_TEXELS * texels.mask);
        geometry.add_vertex(Vec3f(static_cast<float>(footprint.min.x()), static_cast<float>(footprint.min.y()), 0.0f));
        geometry.add_vertex(Vec3f(static_cast<float>(footprint.max.x()), static_cast<float>(footprint.min.y()), 0.0f));
        geometry.add_vertex(Vec3f(static_cast<float>(footprint.max.x()), static_cast<float>(footprint.max.y()), 0.0f));
        geometry.add_vertex(Vec3f(static_cast<float>(footprint.min.x()), static_cast<float>(footprint.max.y()), 0.0f));
        geometry.add_triangle(vertexId, vertexId + 1, vertexId + 2);
        geometry.add_triangle(vertexId, vertexId + 2, vertexId + 3);
        vertexId += 4;
    }

    _footprintModel.reset();
    _footprintModel.init_from(std::move(geometry));
}

bool SoftShadowRenderer::RenderGroundMask(bool reducedSamples)
{
    GLShaderProgram* const shader = wxGetApp().get_shader("shadow_ground_mask");
    if (shader == nullptr)
        return false;

    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _maskFramebuffer));
    glsafe(::glViewport(0, 0, static_cast<GLsizei>(_maskSize), static_cast<GLsizei>(_maskSize)));
    // Fully lit, no blur
    glsafe(::glClearColor(1.0f, 0.0f, 0.0f, 1.0f));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));

    // All footprints lie at the same depth: the first fragment of a texel wins, later overlapping ones are
    // rejected by the early depth test before the expensive shader runs
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthFunc(GL_LESS));
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glDisable(GL_CULL_FACE));

    glsafe(::glActiveTexture(GL_TEXTURE0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _shadowDepthTexture));
    glsafe(::glBindSampler(0, _rawDepthSampler));
    glsafe(::glActiveTexture(GL_TEXTURE1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _shadowDepthTexture));
    glsafe(::glBindSampler(1, _compareDepthSampler));
    glsafe(::glActiveTexture(GL_TEXTURE2));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _localTopTexture));
    glsafe(::glBindSampler(2, 0));

    const double shadowTexel = static_cast<double>(_lightExtent.x()) / static_cast<double>(_shadowMapSize);
    shader->start_using();
    shader->set_uniform("shadow_depth", 0);
    shader->set_uniform("shadow_depth_cmp", 1);
    shader->set_uniform("local_top", 2);
    shader->set_uniform("use_local_top", _localTopValid);
    shader->set_uniform("light_view_projection", Matrix4d(_lightProjection * _lightView.matrix()));
    shader->set_uniform("mask_rect", _maskRect);
    shader->set_uniform("ground_z", static_cast<float>(GROUND_Z));
    shader->set_uniform("depth_span", _depthSpan);
    shader->set_uniform("light_extent", _lightExtent);
    shader->set_uniform("light_spread", LIGHT_SPREAD);
    shader->set_uniform("min_penumbra", static_cast<float>(MIN_PENUMBRA_TEXELS * shadowTexel));
    // Blur spacing grows with a quarter of the penumbra width, capped at MASK_BLUR_MAX_STEP texels
    const double maskTexel = static_cast<double>(_maskRect[2] - _maskRect[0]) / static_cast<double>(_maskSize);
    shader->set_uniform("penumbra_to_step", static_cast<float>(0.25 / (maskTexel * static_cast<double>(MASK_BLUR_MAX_STEP))));
    shader->set_uniform("max_search", static_cast<float>(MAX_SEARCH_MM));
    shader->set_uniform("depth_bias", static_cast<float>(DEPTH_BIAS_MM / static_cast<double>(_depthSpan)));
    shader->set_uniform("blocker_samples", reducedSamples ? BLOCKER_SAMPLES_LQ : BLOCKER_SAMPLES_HQ);
    shader->set_uniform("pcf_samples", reducedSamples ? PCF_SAMPLES_LQ : PCF_SAMPLES_HQ);
    _footprintModel.render();
    shader->stop_using();
    return true;
}

void SoftShadowRenderer::BlurGroundMask()
{
    GLShaderProgram* const shader = wxGetApp().get_shader("shadow_mask_blur");
    if (shader == nullptr || _maskBlurFramebuffer == 0)
        return;

    EnsureFullscreenQuad();
    glsafe(::glViewport(0, 0, static_cast<GLsizei>(_maskSize), static_cast<GLsizei>(_maskSize)));
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glActiveTexture(GL_TEXTURE0));
    glsafe(::glBindSampler(0, 0));

    shader->start_using();
    shader->set_uniform("source_mask", 0);
    shader->set_uniform("max_step", MASK_BLUR_MAX_STEP);
    // Horizontal: mask -> blur target, vertical: blur target -> mask
    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _maskBlurFramebuffer));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _maskTexture));
    shader->set_uniform("direction", std::array<int, 2>{ { 1, 0 } });
    _fullscreenQuad.render();

    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _maskFramebuffer));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _maskBlurTexture));
    shader->set_uniform("direction", std::array<int, 2>{ { 0, 1 } });
    _fullscreenQuad.render();
    shader->stop_using();
}

void SoftShadowRenderer::EnsureFullscreenQuad()
{
    if (_fullscreenQuad.is_initialized())
        return;

    GLModel::Geometry geometry;
    geometry.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    geometry.add_vertex(Vec3f(-1.0f, -1.0f, 0.0f));
    geometry.add_vertex(Vec3f(1.0f, -1.0f, 0.0f));
    geometry.add_vertex(Vec3f(1.0f, 1.0f, 0.0f));
    geometry.add_vertex(Vec3f(-1.0f, 1.0f, 0.0f));
    geometry.add_triangle(0, 1, 2);
    geometry.add_triangle(0, 2, 3);
    _fullscreenQuad.init_from(std::move(geometry));
}

bool SoftShadowRenderer::Update(const SoftShadowFrameInput& input)
{
    _rebuilt = false;
    if (input.volumes == nullptr || !IsSupported())
        return false;

    // Fix: the result used to be read only by the next settled Begin(), so the first drag after a slow
    // settled mask still ran with the full sample count
    _fullMaskTimer.Poll();

    if (!CollectCasters(input))
    {
        _maskValid = false;
        return false;
    }

    const std::size_t signature = ComputeSignature();
    const bool settled = _lastWasInteractive && !input.interactive;
    if (_maskValid && signature == _lastSignature && !settled)
        return true;

    if (!EnsureResources())
    {
        _maskValid = false;
        return false;
    }

    bool rendered = false;
    {
        ShadowStateGuard stateGuard;
        UpdateLightFrustum(input.interactive);
        BuildFootprintModel();
        rendered = RenderShadowMap();
        if (rendered)
            BuildLocalTopMap();
        if (rendered)
        {
            // Fewer samples while dragging only when a full quality mask is measured to be expensive
            const bool reducedSamples = input.interactive && _fullMaskTimer.GetTimeMs() > DRAG_MASK_BUDGET_MS;
            // Only the settled pass is timed, it is the full quality reference for the drag decision
            if (!input.interactive)
                _fullMaskTimer.Begin();
            rendered = RenderGroundMask(reducedSamples);
            if (rendered)
                BlurGroundMask();
            if (!input.interactive)
                _fullMaskTimer.End();
        }
    }

    _maskValid = rendered;
    _rebuilt = true;
    _lastSignature = signature;
    _lastWasInteractive = input.interactive;
    return _maskValid;
}

void SoftShadowRenderer::RenderReceivers(const Camera& camera, PartPlateList& plates, bool darkMode)
{
    if (!_maskValid || _maskTexture == 0)
        return;

    GLShaderProgram* const shader = wxGetApp().get_shader("shadow_receiver");
    if (shader == nullptr)
        return;

    ShadowStateGuard stateGuard;
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthFunc(GL_LESS));
    glsafe(::glDepthMask(GL_FALSE));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glActiveTexture(GL_TEXTURE0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _maskTexture));

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("receiver_z", RECEIVER_Z);
    shader->set_uniform("shadow_mask", 0);
    shader->set_uniform("mask_rect", _maskRect);
    shader->set_uniform("shadow_strength", darkMode ? STRENGTH_DARK : STRENGTH_LIGHT);
    plates.RenderShadowReceivers();
    shader->stop_using();
}

void SoftShadowRenderer::DisableObjectShadows(const char* reason)
{
    _objectShadowsFailed = true;
    ReleaseSceneDepthResources();
    ReleaseObjectShadowTarget();
    BOOST_LOG_TRIVIAL(warning) << "Soft shadows on objects disabled: " << (reason != nullptr ? reason : "unknown");
}

bool SoftShadowRenderer::EnsureSceneDepthResources(int width, int height)
{
    if (_sceneDepthFramebuffer != 0 && _sceneDepthWidth == width && _sceneDepthHeight == height)
        return true;

    // The blit needs the exact depth/stencil format of the default framebuffer
    glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, 0));
    DepthCopyFormat format;
    if (!QueryDefaultDepthFormat(format))
    {
        DisableObjectShadows("unsupported default depth format");
        return false;
    }

    ReleaseSceneDepthResources();
    GLuint texture = 0;
    glsafe(::glGenTextures(1, &texture));
    _sceneDepthTexture = static_cast<unsigned int>(texture);
    glsafe(::glActiveTexture(GL_TEXTURE0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, texture));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, format.internalFormat, width, height, 0, format.format, format.type, nullptr));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));

    GLuint framebuffer = 0;
    glsafe(::glGenFramebuffers(1, &framebuffer));
    _sceneDepthFramebuffer = static_cast<unsigned int>(framebuffer);
    // Fix: bound to both targets, so glReadBuffer below applies to this framebuffer instead of the window one
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, framebuffer));
    glsafe(::glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, format.attachment, GL_TEXTURE_2D, texture, 0));
    glsafe(::glDrawBuffer(GL_NONE));
    glsafe(::glReadBuffer(GL_NONE));
    if (!IsDrawFramebufferComplete())
    {
        DisableObjectShadows("scene depth framebuffer incomplete");
        return false;
    }

    _sceneDepthWidth = width;
    _sceneDepthHeight = height;
    return true;
}

void SoftShadowRenderer::ReleaseSceneDepthResources()
{
    if (_sceneDepthFramebuffer != 0)
        glsafe(::glDeleteFramebuffers(1, &_sceneDepthFramebuffer));
    if (_sceneDepthTexture != 0)
        glsafe(::glDeleteTextures(1, &_sceneDepthTexture));
    _sceneDepthFramebuffer = 0;
    _sceneDepthTexture = 0;
    _sceneDepthWidth = 0;
    _sceneDepthHeight = 0;
}

bool SoftShadowRenderer::CopySceneDepth(const std::array<int, 4>& viewport, const SoftShadowPixelRect& copyRect)
{
    if (!EnsureSceneDepthResources(viewport[2], viewport[3]))
        return false;

    // Only the object region is copied, into the same position of the full size texture
    const GLint srcX0 = viewport[0] + copyRect.x0;
    const GLint srcY0 = viewport[1] + copyRect.y0;
    const GLint srcX1 = viewport[0] + copyRect.x1;
    const GLint srcY1 = viewport[1] + copyRect.y1;
    glsafe(::glDisable(GL_SCISSOR_TEST));
    glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, 0));
    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _sceneDepthFramebuffer));
    if (_sceneDepthVerified)
    {
        glsafe(::glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, copyRect.x0, copyRect.y0, copyRect.x1, copyRect.y1,
                                   GL_DEPTH_BUFFER_BIT, GL_NEAREST));
        return true;
    }

    // First copy: drivers may reject depth blits from multisampled framebuffers, detect it once
    while (::glGetError() != GL_NO_ERROR)
    {
    }
    ::glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, copyRect.x0, copyRect.y0, copyRect.x1, copyRect.y1,
                        GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    if (::glGetError() != GL_NO_ERROR)
    {
        DisableObjectShadows("depth blit from the default framebuffer failed");
        return false;
    }
    _sceneDepthVerified = true;
    return true;
}

void SoftShadowRenderer::DrawObjectShadows(GLShaderProgram& shader, const Camera& camera, int resolutionScale,
                                           const SoftShadowPixelRect& shadeRect, bool darkMode)
{
    EnsureFullscreenQuad();
    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _objectShadowFramebuffer));
    glsafe(::glViewport(0, 0, _objectShadowWidth, _objectShadowHeight));
    // The scissor limits both the clear and the shading to the object region
    glsafe(::glEnable(GL_SCISSOR_TEST));
    glsafe(::glScissor(shadeRect.x0, shadeRect.y0, shadeRect.x1 - shadeRect.x0, shadeRect.y1 - shadeRect.y0));
    glsafe(::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
    glsafe(::glClearColor(0.0f, 0.0f, 0.0f, 0.0f));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT));
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glDepthMask(GL_FALSE));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));

    glsafe(::glActiveTexture(GL_TEXTURE0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _sceneDepthTexture));
    glsafe(::glBindSampler(0, 0));
    glsafe(::glActiveTexture(GL_TEXTURE1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _shadowDepthTexture));
    glsafe(::glBindSampler(1, _rawDepthSampler));
    glsafe(::glActiveTexture(GL_TEXTURE2));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _shadowDepthTexture));
    glsafe(::glBindSampler(2, _compareDepthSampler));
    glsafe(::glActiveTexture(GL_TEXTURE3));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _localTopTexture));
    glsafe(::glBindSampler(3, 0));

    const Matrix4d viewProjection = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const double shadowTexel = static_cast<double>(_lightExtent.x()) / static_cast<double>(_shadowMapSize);
    shader.start_using();
    shader.set_uniform("scene_depth", 0);
    shader.set_uniform("shadow_depth", 1);
    shader.set_uniform("shadow_depth_cmp", 2);
    shader.set_uniform("local_top", 3);
    shader.set_uniform("use_local_top", _localTopValid);
    shader.set_uniform("inv_view_projection", Matrix4d(viewProjection.inverse()));
    shader.set_uniform("inv_projection", Matrix4d(camera.get_projection_matrix().matrix().inverse()));
    shader.set_uniform("reconstruct_offset", OBJECT_RECONSTRUCT_OFFSET);
    shader.set_uniform("to_camera", Vec3d(-camera.get_dir_forward()));
    shader.set_uniform("resolution_scale", resolutionScale);
    const Matrix4d lightViewProjection = _lightProjection * _lightView.matrix();
    const Matrix3d lightLinear = lightViewProjection.block<3, 3>(0, 0);
    shader.set_uniform("light_view_projection", lightViewProjection);
    shader.set_uniform("light_normal_matrix", Matrix3d(lightLinear.inverse().transpose()));
    // World slope limit converted to light depth per uv
    shader.set_uniform("max_receiver_slope",
                       static_cast<float>(OBJECT_MAX_RECEIVER_SLOPE * static_cast<double>(_lightExtent.x()) / static_cast<double>(_depthSpan)));
    shader.set_uniform("to_light", TO_LIGHT_DIR);
    shader.set_uniform("depth_span", _depthSpan);
    shader.set_uniform("light_extent", _lightExtent);
    shader.set_uniform("light_spread", LIGHT_SPREAD);
    shader.set_uniform("min_penumbra", static_cast<float>(MIN_PENUMBRA_TEXELS * shadowTexel));
    shader.set_uniform("max_search", static_cast<float>(MAX_SEARCH_MM));
    shader.set_uniform("depth_bias", static_cast<float>(OBJECT_DEPTH_BIAS_MM / static_cast<double>(_depthSpan)));
    shader.set_uniform("normal_offset", static_cast<float>(OBJECT_NORMAL_OFFSET_TEXELS * shadowTexel));
    shader.set_uniform("shadow_strength", darkMode ? OBJECT_STRENGTH_DARK : OBJECT_STRENGTH_LIGHT);
    shader.set_uniform("blocker_samples", OBJECT_BLOCKER_SAMPLES);
    shader.set_uniform("pcf_samples", OBJECT_PCF_SAMPLES);
    _fullscreenQuad.render();
    shader.stop_using();
}

bool SoftShadowRenderer::EnsureObjectShadowTarget(int width, int height)
{
    if (_objectShadowFramebuffer != 0 && _objectShadowWidth == width && _objectShadowHeight == height)
        return true;

    ReleaseObjectShadowTarget();
    GLuint texture = 0;
    glsafe(::glGenTextures(1, &texture));
    _objectShadowTexture = static_cast<unsigned int>(texture);
    glsafe(::glActiveTexture(GL_TEXTURE0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, texture));
    // R = shadow, G = view depth, BA = octahedral normal; half floats keep the 2% depth tolerance meaningful
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));

    GLuint framebuffer = 0;
    glsafe(::glGenFramebuffers(1, &framebuffer));
    _objectShadowFramebuffer = static_cast<unsigned int>(framebuffer);
    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer));
    glsafe(::glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0));
    if (!IsDrawFramebufferComplete())
    {
        ReleaseObjectShadowTarget();
        DisableObjectShadows("object shadow framebuffer incomplete");
        return false;
    }

    _objectShadowWidth = width;
    _objectShadowHeight = height;
    return true;
}

void SoftShadowRenderer::ReleaseObjectShadowTarget()
{
    if (_objectShadowFramebuffer != 0)
        glsafe(::glDeleteFramebuffers(1, &_objectShadowFramebuffer));
    if (_objectShadowTexture != 0)
        glsafe(::glDeleteTextures(1, &_objectShadowTexture));
    _objectShadowFramebuffer = 0;
    _objectShadowTexture = 0;
    _objectShadowWidth = 0;
    _objectShadowHeight = 0;
}

void SoftShadowRenderer::CompositeObjectShadows(GLShaderProgram& shader, const Camera& camera, const std::array<int, 4>& viewport,
                                                int resolutionScale, const SoftShadowPixelRect& workRect)
{
    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0));
    glsafe(::glViewport(viewport[0], viewport[1], viewport[2], viewport[3]));
    glsafe(::glEnable(GL_SCISSOR_TEST));
    glsafe(::glScissor(viewport[0] + workRect.x0, viewport[1] + workRect.y0, workRect.x1 - workRect.x0,
                       workRect.y1 - workRect.y0));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_DST_COLOR, GL_ZERO));

    // Units 1 and 2 still carry the depth samplers of the shadow pass
    glsafe(::glBindSampler(1, 0));
    glsafe(::glBindSampler(2, 0));
    glsafe(::glActiveTexture(GL_TEXTURE0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _sceneDepthTexture));
    glsafe(::glActiveTexture(GL_TEXTURE1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _objectShadowTexture));

    shader.start_using();
    shader.set_uniform("scene_depth", 0);
    shader.set_uniform("object_shadow", 1);
    shader.set_uniform("resolution_scale", resolutionScale);
    shader.set_uniform("inv_projection", Matrix4d(camera.get_projection_matrix().matrix().inverse()));
    shader.set_uniform("depth_tolerance", UPSAMPLE_DEPTH_TOLERANCE);
    shader.set_uniform("multisampled", _defaultFramebufferMultisampled);
    // Full resolution normals let the filter keep samples of one face apart from the adjacent face
    const Matrix4d viewProjection = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    shader.set_uniform("inv_view_projection", Matrix4d(viewProjection.inverse()));
    shader.set_uniform("to_camera", Vec3d(-camera.get_dir_forward()));
    _fullscreenQuad.render();
    shader.stop_using();
}

void SoftShadowRenderer::RenderObjectShadows(const Camera& camera, bool darkMode)
{
    if (_objectShadowsFailed || !_maskValid || _shadowDepthTexture == 0)
        return;

    GLShaderProgram* const shader = wxGetApp().get_shader("shadow_screen");
    GLShaderProgram* const compositeShader = wxGetApp().get_shader("shadow_screen_composite");
    if (shader == nullptr || compositeShader == nullptr)
        return;

    GLint drawFramebuffer = 0;
    glsafe(::glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer));
    // Fix: a frame rendered into another framebuffer only skips the pass instead of disabling it for good
    if (drawFramebuffer != 0)
        return;
    // GL_SAMPLES describes the bound draw framebuffer, here the default one
    GLint samples = 0;
    glsafe(::glGetIntegerv(GL_SAMPLES, &samples));
    _defaultFramebufferMultisampled = samples > 1;

    std::array<int, 4> viewport{ { 0, 0, 0, 0 } };
    glsafe(::glGetIntegerv(GL_VIEWPORT, viewport.data()));
    if (viewport[2] <= 0 || viewport[3] <= 0)
        return;

    const int scale = OBJECT_SHADOW_RESOLUTION_SCALE;
    const int targetWidth = std::max(1, (viewport[2] + scale - 1) / scale);
    const int targetHeight = std::max(1, (viewport[3] + scale - 1) / scale);

    // All work is limited to the screen projection of the casters, which covers every receiving object
    const Matrix4d viewProjection = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const SoftShadowPixelRect objectRect = ProjectBoxToScreen(_casterBox, viewProjection, viewport[2], viewport[3]);
    if (objectRect.IsEmpty())
        return;

    const SoftShadowPixelRect workRect = ExpandRect(objectRect, OBJECT_RECT_MARGIN, viewport[2], viewport[3]);
    SoftShadowPixelRect shadeRect;
    shadeRect.x0 = workRect.x0 / scale;
    shadeRect.y0 = workRect.y0 / scale;
    shadeRect.x1 = (workRect.x1 + scale - 1) / scale;
    shadeRect.y1 = (workRect.y1 + scale - 1) / scale;
    // Extra shading pixels for the 3x3 composite filter, plus neighbours read for normals
    shadeRect = ExpandRect(shadeRect, 2, targetWidth, targetHeight);
    const SoftShadowPixelRect copyRect = ExpandRect(workRect, 2 * scale + 2, viewport[2], viewport[3]);

    ShadowStateGuard stateGuard;
    if (!CopySceneDepth(viewport, copyRect) || !EnsureObjectShadowTarget(targetWidth, targetHeight))
        return;

    DrawObjectShadows(*shader, camera, scale, shadeRect, darkMode);
    CompositeObjectShadows(*compositeShader, camera, viewport, scale, workRect);
}

void SoftShadowRenderer::Release()
{
    if (_shadowFramebuffer != 0)
        glsafe(::glDeleteFramebuffers(1, &_shadowFramebuffer));
    if (_maskFramebuffer != 0)
        glsafe(::glDeleteFramebuffers(1, &_maskFramebuffer));
    if (_shadowDepthTexture != 0)
        glsafe(::glDeleteTextures(1, &_shadowDepthTexture));
    if (_maskTexture != 0)
        glsafe(::glDeleteTextures(1, &_maskTexture));
    if (_maskDepthRenderbuffer != 0)
        glsafe(::glDeleteRenderbuffers(1, &_maskDepthRenderbuffer));
    if (_maskBlurFramebuffer != 0)
        glsafe(::glDeleteFramebuffers(1, &_maskBlurFramebuffer));
    if (_maskBlurTexture != 0)
        glsafe(::glDeleteTextures(1, &_maskBlurTexture));
    if (_rawDepthSampler != 0)
        glsafe(::glDeleteSamplers(1, &_rawDepthSampler));
    if (_compareDepthSampler != 0)
        glsafe(::glDeleteSamplers(1, &_compareDepthSampler));
    if (_localTopTexture != 0)
        glsafe(::glDeleteTextures(1, &_localTopTexture));
    if (_localTopFramebuffer != 0)
        glsafe(::glDeleteFramebuffers(1, &_localTopFramebuffer));
    if (_localTopMidFramebuffer != 0)
        glsafe(::glDeleteFramebuffers(1, &_localTopMidFramebuffer));
    if (_localTopMidTexture != 0)
        glsafe(::glDeleteTextures(1, &_localTopMidTexture));

    _shadowFramebuffer = 0;
    _maskFramebuffer = 0;
    _shadowDepthTexture = 0;
    _maskTexture = 0;
    _maskDepthRenderbuffer = 0;
    _maskBlurFramebuffer = 0;
    _maskBlurTexture = 0;
    _rawDepthSampler = 0;
    _compareDepthSampler = 0;
    _localTopTexture = 0;
    _localTopFramebuffer = 0;
    _localTopMidFramebuffer = 0;
    _localTopMidTexture = 0;
    _localTopValid = false;
    _footprintModel.reset();
    ReleaseSceneDepthResources();
    ReleaseObjectShadowTarget();
    _fullscreenQuad.reset();
    _fullMaskTimer.Release();
    _casters.clear();
    _instanceBoxes.clear();
    _maskValid = false;
    _lastSignature = 0;
    _lastWasInteractive = false;
    _frustumFrozen = false;
}

} // namespace GUI
} // namespace Slic3r
