#include "AOExperimentRunner.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace Slic3r { namespace GUI { namespace AO {

AOExperimentRunner::Mode AOExperimentRunner::parse_mode(const std::string& value)
{
    if (value == "backends")
        return Mode::Backends;
    if (value == "cslow")
        return Mode::LowLegacy;
    if (value == "qualities")
        return Mode::Qualities;
    if (value == "fseval")
        return Mode::FsReconstruction;
    if (value == "pixelfs")
        return Mode::PixelFs;
    if (value == "pixel")
        return Mode::Pixel;
    if (value == "split")
        return Mode::Split;
    if (value == "confidence")
        return Mode::Confidence;
    if (value == "composite")
        return Mode::Composite;
    if (value == "fssampling")
        return Mode::FsSampling;
    if (value == "fsdenoise")
        return Mode::FsDenoise;
    if (value == "fsfilter")
        return Mode::FsFilter;
    return Mode::DefaultDirections; // Includes unset and unknown modes.
}

bool AOExperimentRunner::start(const std::string& root, Mode mode)
{
    m_root  = root;
    m_mode  = mode;
    m_run   = 0;
    m_stage = 1;
    m_signature.clear();
    std::ofstream status(std::filesystem::path(m_root) / "status.txt");
    status << (flag_qualities() ? "RUNNING: six quality configurations; " : "RUNNING: three alternating ") << description()
           << "; 30 warmup + 180 measured per run.\n"
           << "Images captured after timing. Do not change view, settings or scene.\n";
    status.close();
    return bool(status);
}

void AOExperimentRunner::finish(const std::string& reason)
{
    if (m_stage == 0)
        return;
    std::ofstream status(std::filesystem::path(m_root) / "status.txt");
    status << reason << "\ncompleted_runs=" << m_run << '\n';
    m_stage = 0;
}

bool AOExperimentRunner::signature_changed(const std::string& signature)
{
    if (m_signature.empty()) {
        m_signature = signature;
        return false;
    }
    return m_signature != signature;
}

std::string AOExperimentRunner::description() const
{
    return flag_low()            ? "CS Low legacy/normalized unoccluded slice" :
           flag_qualities()      ? "CS/FS Low, Medium, High" :
           flag_reconstruction() ? "FS matrix/separable XY reconstruction" :
           flag_pixel()          ? (flag_pixel_fs() ? "FS sample/pixel composite" : "CS sample/pixel composite") :
           flag_split()          ? "CS direct/split MSAA composite" :
           flag_confidence()     ? "CS direct/precomputed confidence" :
           flag_composite()      ? "CS reference/optimized composite" :
           flag_sampling()       ? "FS 4x8/3x8 with three denoise passes" :
           flag_denoise()        ? "FS connectivity 2/3 passes" :
           flag_filter()         ? "FS geometry/connectivity" :
           flag_backends()       ? "CS/FS" :
                                   "default/directions";
}

std::string AOExperimentRunner::run_name(int run) const
{
    const char*        quality_names[] = {"low", "medium", "high"};
    std::ostringstream name;
    name << run + 1;
    if (flag_low())
        name << (run % 2 ? "-cs-low-normalized" : "-cs-low-legacy");
    else if (flag_qualities())
        name << (run % 2 ? "-fs-" : "-cs-") << quality_names[run / 2];
    else if (flag_reconstruction())
        name << (run % 2 ? "-fs-xy" : "-fs-matrix");
    else if (flag_pixel())
        name << (run % 2 ? "-composite-pixel" : "-composite-sample");
    else if (flag_split())
        name << (run % 2 ? "-composite-split" : "-composite-direct");
    else if (flag_confidence())
        name << (run % 2 ? "-confidence-precomputed" : "-confidence-direct");
    else if (flag_composite())
        name << (run % 2 ? "-composite-optimized" : "-composite-reference");
    else if (flag_sampling())
        name << (run % 2 ? "-fs-3x8" : "-fs-4x8");
    else if (flag_denoise())
        name << (run % 2 ? "-fs-edges3" : "-fs-edges2");
    else if (flag_filter())
        name << (run % 2 ? "-fs-edges" : "-fs-geometry");
    else if (flag_backends())
        name << (run % 2 ? "-fs" : "-cs");
    else
        name << (run % 2 ? "-directions" : "-default");
    return name.str();
}

void AOExperimentRunner::apply_overrides(AOExecutionRequest& request, AOSettings& settings) const
{
    const int run = m_run;
    if (settings.quality != AOSettings::Quality::Off) {
        if (flag_qualities())
            settings.quality = static_cast<AOSettings::Quality>(1 + run / 2);
        if (flag_low())
            settings.quality = AOSettings::Quality::Low;
    }
    // Composite variant; per-pixel is the production default, "sample"/"optimized" retain the
    // per-sample path. Isolated A/B modes keep their original baselines.
    const bool pixel_composite = flag_low() || flag_reconstruction() || flag_backends() || (flag_pixel() && run % 2);
    if (flag_composite() && run % 2 == 0)
        request.composite = CompositeVariant::Reference;
    else if (flag_confidence() && run % 2)
        request.composite = CompositeVariant::Confidence;
    else if (flag_split() && run % 2)
        request.composite = CompositeVariant::SplitMsaa;
    else if (pixel_composite)
        request.composite = CompositeVariant::Pixel;
    else
        request.composite = CompositeVariant::Sample;
    request.require_msaa_comparison = flag_pixel();
    request.cs_low_reference        = settings.quality == AOSettings::Quality::Low && flag_low() && run % 2 == 0;
    request.fs_fast_reconstruction  = flag_reconstruction() && run % 2;
    // Production High uses 3x8 plus three connectivity passes. Explicit geometry restores the
    // original 4x8 pipeline; the older A/B modes retain their settings.
    request.fs_edge_filter = flag_reconstruction() || flag_pixel_fs() || flag_backends() ||
                             (flag_filter() && (flag_sampling() || flag_denoise() || run % 2));
    request.fs_edge_passes = flag_reconstruction() || flag_pixel_fs() || flag_backends() || flag_sampling() || (flag_denoise() && run % 2) ?
                                 3 :
                                 2;
    request.fs_slices_override = flag_reconstruction() || flag_pixel_fs() || flag_backends() || (flag_sampling() && run % 2) ? 3 : 0;
    const bool force_cs        = !flag_reconstruction() && !flag_pixel_fs() && !flag_filter() && !(flag_backends() && run % 2);
    const bool force_fs        = flag_reconstruction() || flag_pixel_fs() || flag_filter() || (flag_backends() && run % 2);
    request.backend            = force_cs ? BackendPreference::Compute : force_fs ? BackendPreference::Fragment : BackendPreference::Auto;
    // Backend comparisons follow the current quality preset. The isolated CS diagnostic
    // comparisons keep their explicit 3/6-slice sampling contract.
    request.cs_slices_override = 0;
    if (!flag_backends() && !flag_pixel())
        request.cs_slices_override = flag_composite() || flag_confidence() || flag_split() ? 3 : (run % 2 ? 6 : 3);
    // Comparisons are strict: an unavailable route fails the run instead of silently degrading.
    request.fallback = FallbackPolicy::Forbid;
}

bool AOExperimentRunner::validate_actual(const AOResult& result) const
{
    if (!result.actual.executed)
        return false;
    const int run = m_run;
    if (!result.actual.executed)
        return false;
    const bool expected_fs = flag_reconstruction() || flag_pixel_fs() || flag_filter() || (flag_backends() && run % 2);
    if ((result.actual.backend == Backend::Compute) == expected_fs)
        return false;
    const bool edge_active = result.actual.filter == FilterMode::Connectivity;
    if (flag_filter() && edge_active != (flag_sampling() || flag_denoise() || bool(run % 2)))
        return false;
    const int edge_passes = edge_active ? result.actual.filter_passes : 0;
    if (flag_denoise() && edge_passes != (run % 2 ? 3 : 2))
        return false;
    if (flag_sampling() && (result.actual.slices != (run % 2 ? 3 : 4) || edge_passes != 3))
        return false;
    if (flag_confidence() && (result.actual.composite == CompositeVariant::Confidence) != bool(run % 2))
        return false;
    if (flag_split() && (result.actual.composite == CompositeVariant::SplitMsaa) != bool(run % 2))
        return false;
    if (flag_pixel() && (result.actual.composite == CompositeVariant::Pixel) != bool(run % 2))
        return false;
    if (flag_reconstruction() && result.actual.fast_reconstruction != bool(run % 2))
        return false;
    if (flag_qualities() && result.actual.quality != static_cast<AOSettings::Quality>(1 + run / 2))
        return false;
    if (flag_low() && (result.actual.quality != AOSettings::Quality::Low || result.actual.cs_low_reference != (run % 2 == 0)))
        return false;
    return true;
}

void AOExperimentRunner::enter_capture(const std::string& capture_directory)
{
    (void) capture_directory;
    m_stage = 2;
}

bool AOExperimentRunner::advance(int total_runs)
{
    if (++m_run == total_runs)
        return false;
    m_stage = 1;
    return true;
}

}}} // namespace Slic3r::GUI::AO
