#ifndef slic3r_GUI_AO_AOPlan_hpp_
#define slic3r_GUI_AO_AOPlan_hpp_

#include "AOTypes.hpp"

#include <string>
#include <variant>

namespace Slic3r { namespace GUI { namespace AO {

// What the request allows and expects for one frame. Built by the canvas adapter from the
// product settings snapshot, the immutable startup options and typed experiment overrides.
struct AOExecutionRequest
{
    BackendPreference backend{BackendPreference::Auto};
    FallbackPolicy    fallback{FallbackPolicy::Allow};
    // Sampling overrides (0 = preset default). Unified comparisons pin these explicitly.
    int  cs_slices_override{0};          // 3 or 6 for CS High experiments.
    int  cs_denoise_passes{0};           // 0 = quality default; 1..3 pins the denoise chain.
    bool cs_legacy_high_sampling{false}; // High default 3 slices instead of 6.
    int  fs_slices_override{3};          // High default 3x8; 0 selects the original 4x8.
    int  fs_edge_passes{3};              // 2 or 3 connectivity passes.
    bool fs_edge_filter{true};           // Full-resolution High; falls back to geometry when unavailable.
    // Composite route; Pixel is the production default.
    CompositeVariant composite{CompositeVariant::Pixel};
    // Strict experiment constraints (violations fail the request instead of degrading).
    bool require_msaa_comparison{false};  // Both pixel/sample runs must use the owned MSAA target.
    bool require_sample_composite{false}; // The sample path must actually engage.
    // Kernel experiment variants (diagnostics only; keep their original baselines).
    bool cs_low_reference{false};       // Legacy Low normalization baseline.
    bool fs_fast_reconstruction{false}; // Separable XY reconstruction requires matching shaders.
};

// Derived from the request: what the canvas must provide before a plan can be generated.
struct AOFrameRequirements
{
    bool owned_msaa_target{false};       // Needs the canvas-owned full-frame MSAA target.
    bool strict_layout{false};           // Owned layout failures abort instead of falling back to the window.
    bool sample_capture{false};          // Per-sample color/depth capture products are requested.
    bool defer_capture_completion{true}; // Completion waits for the presented frame.
    int  min_samples{0};                 // 0 = any; >1 requires a multisampled layout.
};

// Concrete FS work: evaluation size, sampling, filter chain and upsample.
struct AOFSPlan
{
    AOSettings::Quality quality{AOSettings::Quality::Low};
    int                 eval_width{0}, eval_height{0}; // Low/Medium: half resolution rounded up; High: full.
    int                 slices{0}, steps{0};
    bool                edge_filter{false}; // Connectivity filter; geometry otherwise.
    int                 edge_passes{2};     // Connectivity passes (2 or 3).
    bool                fast_reconstruction{false};
    bool                upsample{false}; // eval size != full size.
    AOResponse          response;        // FS keeps the intensity parameter, no output exponent.
};

// Concrete CS work: full-resolution evaluation, XeGTAO MIP chain and denoise.
struct AOCSPlan
{
    AOSettings::Quality quality{AOSettings::Quality::Low};
    int                 width{0}, height{0}; // Padded internally by the backend; the plan records the viewport.
    int                 slices{0}, steps{0};
    int                 denoise_passes{2};
    bool                legacy_low{false};
    AOResponse          response; // Output exponent applied during evaluation; composites at intensity 1.
};

// Complete execution plan. Common composite route plus the backend-specific work.
struct AOPlan
{
    CompositeVariant                 composite{CompositeVariant::Pixel};
    std::variant<AOFSPlan, AOCSPlan> work{AOFSPlan{}};
    Backend         backend() const { return std::holds_alternative<AOCSPlan>(work) ? Backend::Compute : Backend::Fragment; }
    AOFSPlan&       fs() { return std::get<AOFSPlan>(work); }
    const AOFSPlan& fs() const { return std::get<AOFSPlan>(work); }
    AOCSPlan&       cs() { return std::get<AOCSPlan>(work); }
    const AOCSPlan& cs() const { return std::get<AOCSPlan>(work); }
    bool            uses_compute() const { return backend() == Backend::Compute; }
};

// Backend availability snapshot supplied to plan generation (computed from capabilities,
// program readiness and remembered health; never from GPU timings).
struct AOBackendAvailability
{
    bool        cs_supported{false};
    std::string cs_unavailable_reason;
    bool        cs_programs_ready{false};
    // Health reasons collected by the feature (allocation/dispatch failures, sizes).
    std::string cs_health_reason;
    bool        fs_edge_programs_ready{false};       // edge_evaluate + edge_denoise present.
    bool        fs_fast_reconstruction_ready{false}; // Separable XY evaluation present.
    bool        fs_geometry_ready{false};            // evaluate + geometry filter present.
};

// Derives frame requirements from the request (pure).
AOFrameRequirements make_frame_requirements(const AOExecutionRequest& request,
                                            const AOSettings&         settings,
                                            bool                      capture_requested  = false,
                                            bool                      measurement_active = false,
                                            bool                      sample_diagnostics = false,
                                            bool                      comparison_active  = false);

// Generates the concrete plan for an already-selected frame target (pure). Resolves the backend
// preference against availability and the fallback policy; on a forbidden/unsatisfiable CS
// preference the returned plan falls back to FS and the caller must honor the fallback policy.
struct AOPlanDecision
{
    AOPlan      plan;
    bool        fell_back{false};
    std::string fallback_reason;
    bool        forbidden{false}; // Strict CS request that cannot be honored.
};
AOPlanDecision make_plan(const AOExecutionRequest&    request,
                         const AOSettings&            settings,
                         int                          viewport_width,
                         int                          viewport_height,
                         const AOBackendAvailability& availability);

// Frozen preset sampling tables (kept beside plan generation as the single source).
std::array<int, 2> fs_ao_samples(AOSettings::Quality quality, int slices_override); // {slices, steps}
std::array<int, 2> cs_ao_samples(AOSettings::Quality quality, int slices_override, bool legacy_high_sampling);

}}} // namespace Slic3r::GUI::AO
#endif
