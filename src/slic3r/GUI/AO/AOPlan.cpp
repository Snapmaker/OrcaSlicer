#include "AOPlan.hpp"

#include <algorithm>

namespace Slic3r { namespace GUI { namespace AO {

std::array<int, 2> fs_ao_samples(AOSettings::Quality quality, int slices_override)
{
    return quality == AOSettings::Quality::Low    ? std::array<int, 2>{{2, 3}} :
           quality == AOSettings::Quality::Medium ? std::array<int, 2>{{3, 4}} :
                                                    std::array<int, 2>{{slices_override == 3 ? 3 : 4, 8}};
}

std::array<int, 2> cs_ao_samples(AOSettings::Quality quality, int slices_override, bool legacy_high_sampling)
{
    // Static editor views have no temporal accumulation. High spends its GPU headroom on
    // more angular samples; lower presets retain the upstream sampling budget.
    if (quality == AOSettings::Quality::Low)
        return {{1, 2}};
    if (quality == AOSettings::Quality::Medium)
        return {{2, 2}};
    if (slices_override == 3 || slices_override == 6)
        return {{slices_override, 3}};
    return {{legacy_high_sampling ? 3 : 6, 3}};
}

AOFrameRequirements make_frame_requirements(const AOExecutionRequest& request,
                                            const AOSettings&         settings,
                                            bool                      capture_requested,
                                            bool                      measurement_active,
                                            bool                      sample_diagnostics,
                                            bool                      comparison_active)
{
    AOFrameRequirements requirements;
    const bool          sample_path = request.composite != CompositeVariant::Pixel && settings.debug_view == 0;
    const bool          quality_on  = settings.quality != AOSettings::Quality::Off;
    requirements.sample_capture     = capture_requested && (sample_path || request.require_msaa_comparison || sample_diagnostics);
    requirements.owned_msaa_target  = quality_on && (requirements.sample_capture || request.require_msaa_comparison || sample_path ||
                                                    (sample_diagnostics && comparison_active));
    requirements.strict_layout      = request.require_msaa_comparison || requirements.sample_capture ||
                                 (requirements.owned_msaa_target && measurement_active);
    requirements.defer_capture_completion = true;
    requirements.min_samples              = sample_path ? 2 : 0;
    return requirements;
}

namespace {
AOPlan make_fs_plan(
    const AOExecutionRequest& request, const AOSettings& settings, int width, int height, const AOBackendAvailability& availability)
{
    AOPlan plan;
    plan.work              = AOFSPlan{};
    plan.composite         = request.composite;
    auto& fs               = plan.fs();
    fs.quality             = settings.quality;
    fs.eval_width          = settings.quality == AOSettings::Quality::High ? width : (width + 1) / 2;
    fs.eval_height         = settings.quality == AOSettings::Quality::High ? height : (height + 1) / 2;
    const auto samples     = fs_ao_samples(settings.quality, request.fs_slices_override);
    fs.slices              = samples[0];
    fs.steps               = samples[1];
    fs.edge_filter         = request.fs_edge_filter && settings.quality == AOSettings::Quality::High && availability.fs_edge_programs_ready;
    fs.edge_passes         = request.fs_edge_passes == 3 ? 3 : 2;
    fs.fast_reconstruction = request.fs_fast_reconstruction && fs.edge_filter && availability.fs_fast_reconstruction_ready;
    fs.upsample            = fs.eval_width != width || fs.eval_height != height;
    fs.response.output_exponent_applied = false;
    fs.response.composite_intensity     = settings.intensity;
    return plan;
}

AOPlan make_cs_plan(const AOExecutionRequest& request, const AOSettings& settings, int width, int height)
{
    AOPlan plan;
    plan.work          = AOCSPlan{};
    plan.composite     = request.composite;
    auto& cs           = plan.cs();
    cs.quality         = settings.quality;
    cs.width           = width;
    cs.height          = height;
    const auto samples = cs_ao_samples(settings.quality, request.cs_slices_override, request.cs_legacy_high_sampling);
    cs.slices          = samples[0];
    cs.steps           = samples[1];
    cs.denoise_passes = request.cs_denoise_passes > 0 ? request.cs_denoise_passes : (settings.quality == AOSettings::Quality::High ? 3 : 2);
    cs.legacy_low     = request.cs_low_reference && settings.quality == AOSettings::Quality::Low;
    cs.response.output_exponent_applied = true;
    cs.response.composite_intensity     = 1.0f;
    return plan;
}
} // namespace

AOPlanDecision make_plan(const AOExecutionRequest&    request,
                         const AOSettings&            settings,
                         int                          viewport_width,
                         int                          viewport_height,
                         const AOBackendAvailability& availability)
{
    AOPlanDecision decision;
    const bool     cs_ready = availability.cs_supported && availability.cs_programs_ready && availability.cs_health_reason.empty();
    if (request.backend != BackendPreference::Fragment && cs_ready) {
        decision.plan = make_cs_plan(request, settings, viewport_width, viewport_height);
        return decision;
    }
    if (request.backend == BackendPreference::Fragment) {
        // Diagnostic metadata parity with the historical selection reasons.
        decision.plan            = make_fs_plan(request, settings, viewport_width, viewport_height, availability);
        decision.fallback_reason = "FS explicitly selected";
        if (request.fallback == FallbackPolicy::Forbid && request.fs_edge_filter && settings.quality == AOSettings::Quality::High &&
            !availability.fs_edge_programs_ready) {
            decision.forbidden       = true;
            decision.fallback_reason = "requested FS edge programs or resources unavailable";
        }
        return decision;
    }
    // CS was preferred but cannot run.
    decision.fallback_reason = !availability.cs_health_reason.empty() ? availability.cs_health_reason :
                               !availability.cs_supported             ? availability.cs_unavailable_reason :
                                                                        "CS backend not ready";
    if (request.backend == BackendPreference::Compute) {
        if (request.fallback == FallbackPolicy::Forbid) {
            decision.forbidden = true;
            decision.plan      = make_cs_plan(request, settings, viewport_width, viewport_height);
            return decision;
        }
        decision.fell_back = true;
    }
    decision.plan = make_fs_plan(request, settings, viewport_width, viewport_height, availability);
    return decision;
}

}}} // namespace Slic3r::GUI::AO
