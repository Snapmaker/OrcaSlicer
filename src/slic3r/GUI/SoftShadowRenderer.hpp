#pragma once

#include "GLModel.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace Slic3r {

class GLShaderProgram;
class GLVolume;
class GLVolumeCollection;

namespace GUI {

struct Camera;
class PartPlateList;

/** @brief Per-frame inputs of the soft shadow update. */
struct SoftShadowFrameInput
{
    GLVolumeCollection* volumes{ nullptr };
    bool interactive{ false };
    bool renderSlaAuxiliaries{ true };
};

/** @brief Pixel rectangle [x0, x1) x [y0, y1), relative to the viewport or target origin. */
struct SoftShadowPixelRect
{
    int x0{ 0 };
    int y0{ 0 };
    int x1{ 0 };
    int y1{ 0 };

    bool IsEmpty() const { return x1 <= x0 || y1 <= y0; }
};

/** @brief Non-blocking GPU timer built on a ring of GL_TIME_ELAPSED queries. */
class SoftShadowGpuTimer
{
public:
    /** @brief Starts timing when a free query slot is available. */
    void Begin();

    /** @brief Ends the timing started by Begin(). */
    void End();

    /** @brief Releases all query objects; the GL context must be current. */
    void Release();

    /** @brief Returns the smoothed GPU time in milliseconds. */
    float GetTimeMs() const { return _timeMs; }

    /** @brief Collects finished query results without waiting; call once per frame so GetTimeMs() stays current. */
    void Poll();

private:
    std::array<unsigned int, 4> _queries{ { 0, 0, 0, 0 } };
    std::array<bool, 4> _pending{ { false, false, false, false } };
    unsigned int _next{ 0 };
    int _activeSlot{ -1 };
    float _timeMs{ 0.0f };
};

/**
 * @brief PCSS soft shadows: a cached build-plate mask rebuilt only when the casters change, plus a
 * screen-space pass that darkens object pixels lying in shadow.
 */
class SoftShadowRenderer
{
public:
    SoftShadowRenderer() = default;
    SoftShadowRenderer(const SoftShadowRenderer&) = delete;
    SoftShadowRenderer& operator=(const SoftShadowRenderer&) = delete;

    /** @brief Returns true when GL 3.3, ARB framebuffers and both shadow shaders are available. */
    static bool IsSupported();

    /**
     * @brief Rebuilds the shadow map and ground mask when the scene signature changed.
     * @param input Casters and interaction state; input.volumes must not be null.
     * @return true when a valid mask is available for RenderReceivers().
     */
    bool Update(const SoftShadowFrameInput& input);

    /**
     * @brief Composites the cached mask over all plates.
     * @param camera Camera of the current frame.
     * @param plates Plates used as shadow receivers.
     * @param darkMode Selects the shadow strength for the dark theme.
     */
    void RenderReceivers(const Camera& camera, PartPlateList& plates, bool darkMode);

    /**
     * @brief Darkens visible object pixels lying in shadow, computed once per screen pixel.
     * Call right after the opaque objects are drawn and before the plates, so the depth holds objects only.
     * @param camera Camera of the current frame.
     * @param darkMode Selects the shadow strength for the dark theme.
     */
    void RenderObjectShadows(const Camera& camera, bool darkMode);

    /** @brief Releases all GL objects; the GL context must be current. */
    void Release();

    /** @brief Returns true when GL resources are allocated. */
    bool HasResources() const { return _shadowFramebuffer != 0 || _maskFramebuffer != 0; }

    /** @brief Returns true when the last Update() rebuilt the shadow map and mask. */
    bool WasRebuilt() const { return _rebuilt; }

private:
    bool EnsureResources();
    bool CreateShadowMapResources();
    bool CreateMaskResources();
    bool CollectCasters(const SoftShadowFrameInput& input);
    std::size_t ComputeSignature() const;
    void FitLightFrustum(const BoundingBoxf3& fitBox);
    void UpdateLightFrustum(bool interactive);
    bool RenderShadowMap();
    void BuildLocalTopMap(bool interactive);
    void BuildLocalTopMapFromBoxes();
    bool BuildLocalTopMapFromShadowMap();
    void BuildFootprintModel();
    bool RenderGroundMask(bool reducedSamples);
    void BlurGroundMask();
    void EnsureFullscreenQuad();
    bool EnsureSceneDepthResources(int width, int height);
    void ReleaseSceneDepthResources();
    bool CopySceneDepth(const std::array<int, 4>& viewport, const SoftShadowPixelRect& copyRect);
    bool EnsureObjectShadowTarget(int width, int height);
    void ReleaseObjectShadowTarget();
    void DrawObjectShadows(GLShaderProgram& shader, const Camera& camera, int resolutionScale,
                           const SoftShadowPixelRect& shadeRect, bool darkMode);
    void CompositeObjectShadows(GLShaderProgram& shader, const Camera& camera, const std::array<int, 4>& viewport,
                                int resolutionScale, const SoftShadowPixelRect& workRect);
    void DisableObjectShadows(const char* reason);

    unsigned int _shadowFramebuffer{ 0 };
    unsigned int _shadowDepthTexture{ 0 };
    unsigned int _maskFramebuffer{ 0 };
    unsigned int _maskTexture{ 0 };
    // Depth of the mask pass, so overlapping footprints are shaded once per texel
    unsigned int _maskDepthRenderbuffer{ 0 };
    // Ping-pong target of the separable mask blur
    unsigned int _maskBlurFramebuffer{ 0 };
    unsigned int _maskBlurTexture{ 0 };
    unsigned int _rawDepthSampler{ 0 };
    unsigned int _compareDepthSampler{ 0 };
    // Coarse map of the closest-to-light caster depth per tile; bounds the blocker search by nearby casters
    // instead of the tallest object. Reduced from the shadow map when settled, from caster boxes while dragging.
    unsigned int _localTopTexture{ 0 };
    unsigned int _localTopFramebuffer{ 0 };
    // Intermediate level of the two-step GPU reduction (many small blocks keep the GPU busy)
    unsigned int _localTopMidFramebuffer{ 0 };
    unsigned int _localTopMidTexture{ 0 };
    bool _localTopValid{ false };
    unsigned int _shadowMapSize{ 0 };
    unsigned int _maskSize{ 0 };
    bool _resourcesFailed{ false };

    std::vector<GLVolume*> _casters;
    std::vector<BoundingBoxf3> _instanceBoxes;
    BoundingBoxf3 _casterBox;
    GLModel _footprintModel;

    Transform3d _lightView{ Transform3d::Identity() };
    Matrix4d _lightProjection{ Matrix4d::Identity() };
    std::array<float, 4> _maskRect{ { 0.0f, 0.0f, 1.0f, 1.0f } };
    Vec2f _lightExtent{ Vec2f::Ones() };
    float _depthSpan{ 1.0f };

    std::size_t _lastSignature{ 0 };
    bool _lastWasInteractive{ false };
    // While dragging the light frustum stays fixed, so other shadows do not swim with a moving texel grid
    bool _frustumFrozen{ false };
    BoundingBoxf3 _frozenFitBox;
    bool _maskValid{ false };
    bool _rebuilt{ false };

    // GPU time of the settled (full quality) mask pass; decides whether dragging uses fewer mask samples
    SoftShadowGpuTimer _fullMaskTimer;

    // Copy of the scene depth used by the screen-space object shadow pass
    unsigned int _sceneDepthFramebuffer{ 0 };
    unsigned int _sceneDepthTexture{ 0 };
    int _sceneDepthWidth{ 0 };
    int _sceneDepthHeight{ 0 };
    bool _sceneDepthVerified{ false };
    bool _objectShadowsFailed{ false };
    // Silhouette background pixels only receive object shadows when the frame buffer is multisampled
    bool _defaultFramebufferMultisampled{ false };
    GLModel _fullscreenQuad;

    // Intermediate object shadow result: R = shadow amount, G = linear view depth, BA = octahedral normal
    unsigned int _objectShadowFramebuffer{ 0 };
    unsigned int _objectShadowTexture{ 0 };
    int _objectShadowWidth{ 0 };
    int _objectShadowHeight{ 0 };
};

} // namespace GUI
} // namespace Slic3r
