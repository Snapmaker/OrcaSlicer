#ifndef slic3r_GUI_AO_AOTypes_hpp_
#define slic3r_GUI_AO_AOTypes_hpp_

#include "AOConfig.hpp"
#include "libslic3r/Point.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace Slic3r { namespace GUI { namespace AO {

inline constexpr const char* AO_UPSTREAM_VERSION = "a5b1686c7ea37788eeb3576b5be47f7c03db532c";

// Actual executing backend. The request expresses a preference separately.
enum class Backend { Fragment, Compute };
enum class BackendPreference { Auto, Fragment, Compute };
// Independent of backend health: a strict request fails instead of falling back.
enum class FallbackPolicy { Allow, Forbid };

// Composite route for the final blend. Pixel shares the AO factor across covered MSAA samples
// (production default); the others evaluate or blend per sample and are diagnostics/comparisons.
enum class CompositeVariant { Pixel, Sample, SplitMsaa, Confidence, Reference };

enum class FilterMode { Geometry, Connectivity };
enum class FrameTargetKind { Window, CanvasMsaa, WindowFallback };

enum class AOStatus {
    DisabledByUser,    // Quality resolved to Off for this frame.
    UnsupportedByView, // Business state (gizmo/clipping/format) excludes AO.
    Success,
    SuccessWithFallback,
    Failed,    // Execution error; reason carries details.
    Forbidden, // A strict request could not be satisfied; nothing was drawn.
    ContextLost
};

enum class SceneColorState {
    Unchanged,  // Scene colors were not modified.
    Composited, // AO was blended into the scene target.
    NeedsRedraw // Colors may be partially modified; the canvas must redraw the scene.
};

// Response contract: the CS shader family applies the 2.2 output exponent during evaluation and
// composites with intensity 1; the FS family keeps the original intensity parameter.
struct AOResponse
{
    bool  output_exponent_applied{false};
    float composite_intensity{1.0f};
};

// Stable program identity. GL names are runtime handles only; the profile version changes when
// the catalog's program set or defines change, and the generation changes with context rebuilds.
struct AOProgramIdentity
{
    uint32_t profile_version{0};
    uint32_t generation{0};
    bool     operator==(const AOProgramIdentity& other) const
    {
        return profile_version == other.profile_version && generation == other.generation;
    }
};

struct AOActualConfig
{
    bool                executed{false};
    Backend             backend{Backend::Fragment};
    AOSettings::Quality quality{AOSettings::Quality::Off};
    int                 eval_width{0}, eval_height{0};
    int                 slices{0}, steps{0};
    FilterMode          filter{FilterMode::Geometry};
    int                 filter_passes{0};
    CompositeVariant    composite{CompositeVariant::Pixel};
    bool                fast_reconstruction{false};
    bool                cs_low_reference{false};
    AOResponse          response;
    AOProgramIdentity   programs;
    // Runtime handle for diagnostic metadata only; never part of signatures.
    unsigned int cs_evaluate_program{0};
};

struct AOResult
{
    AOStatus          status{AOStatus::DisabledByUser};
    std::string       reason; // Failure/forbidden details; empty on success paths.
    BackendPreference requested_backend{BackendPreference::Auto};
    FallbackPolicy    fallback{FallbackPolicy::Allow};
    bool              fell_back{false};
    std::string       fallback_reason;
    AOActualConfig    actual; // Only meaningful when actual.executed.
    SceneColorState   color_state{SceneColorState::Unchanged};
    // Stable description of the visual configuration (backend, sampling, filtering, composite,
    // response and program identity). Suitable as a scene-cache key; contains no GL names.
    std::string visual_signature;
};

// Deterministic visual configuration signature from actually executed facts. No GL names.
std::string make_visual_signature(const AOActualConfig& actual, const AOSettings& settings);
// Stable frame target labels used by diagnostic metadata.
const char* frame_target_label(FrameTargetKind kind);

// Borrowed GL resource description. Valid only within the documented window: unless stated
// otherwise, contents may be overwritten by the next production stage after the observer
// returns. Readers must copy what they need or apply their own synchronization.
enum class AOResourceKind { Texture2D, Texture2DMultisample, Framebuffer };
enum class DepthSpace { SceneNormalized, MultisampleSceneNormalized, PrefilteredLinearR32F };
enum class AOEncoding { R8UIEncoded, R8Visibility };
enum class AOResourceRole {
    Depth,
    SampleDepth,
    Normals,
    Confidence,
    DepthMips,
    RawAO,
    Edges,
    DenoiseIntermediate,
    FinalAO,
    CompositeFactor
};
enum class NormalEncoding { OctahedralRG16F };

struct AOResourceView
{
    AOResourceKind kind{AOResourceKind::Texture2D};
    AOResourceRole role{AOResourceRole::RawAO};
    unsigned int   gl_name{0};    // Borrowed runtime handle.
    unsigned int   generation{0}; // Owner bumps this on reallocation.
    const void*    context{nullptr};
    unsigned int   format{0}; // Internal format (GLenum value).
    int            width{0}, height{0};
    int            valid_width{0}, valid_height{0};
    int            samples{0};
    int            mip_levels{1};
    int            level{0};
    DepthSpace     depth_space{DepthSpace::SceneNormalized};
    AOEncoding     ao_encoding{AOEncoding::R8Visibility};
    NormalEncoding normal_encoding{NormalEncoding::OctahedralRG16F};
    bool           response_applied{false};
};

// Actual framebuffer/viewport/projection the AO operates on this frame. Experiment switches,
// shader pointers and scene labels are deliberately not part of this structure.
struct AOFrameInput
{
    Matrix4d           projection{Matrix4d::Identity()};
    Matrix4d           inverse_projection{Matrix4d::Identity()};
    std::array<int, 4> viewport{};
    unsigned int       source{0}; // 0 is the window framebuffer, a valid target.
    unsigned int       target{0};
    bool               perspective{true};
    const void*        context{nullptr};
    FrameTargetKind    target_kind{FrameTargetKind::Window};
};

}}} // namespace Slic3r::GUI::AO

#endif
