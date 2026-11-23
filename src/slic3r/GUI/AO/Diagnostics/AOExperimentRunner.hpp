#ifndef slic3r_GUI_AO_Diagnostics_AOExperimentRunner_hpp_
#define slic3r_GUI_AO_Diagnostics_AOExperimentRunner_hpp_

#include "../AOConfig.hpp"
#include "../AOPlan.hpp"
#include "../AOTypes.hpp"

#include <string>

namespace Slic3r { namespace GUI { namespace AO {

// Comparison experiment state machine and typed recipes. States: Idle -> Measure (benchmark
// 30 warmup + 180 frames) -> Capture (one-shot capture) -> Validate -> Next/Complete. Any stage
// may fail or cancel. Recipes only override the typed request; kernel experiments keep their
// original baselines and reuse the public inputs and backends.
class AOExperimentRunner
{
public:
    // Historical comparison modes; DefaultDirections is the no-mode "default/directions" pair.
    enum class Mode {
        DefaultDirections,
        Backends,
        LowLegacy,
        Qualities,
        FsReconstruction,
        PixelFs,
        Pixel,
        Split,
        Confidence,
        Composite,
        FsSampling,
        FsDenoise,
        FsFilter
    };
    static Mode parse_mode(const std::string& value);

    AOExperimentRunner()  = default;
    ~AOExperimentRunner() = default;

    // Enters Measure for run 0 after writing the run header; false when the directory fails.
    bool start(const std::string& root, Mode mode);
    // Writes the status trailer and returns to Idle.
    void finish(const std::string& reason);

    bool               active() const { return m_stage != 0; }
    int                stage() const { return m_stage; } // 0 idle, 1 measuring, 2 capturing
    int                run() const { return m_run; }
    Mode               mode() const { return m_mode; }
    bool               mode_is_low() const { return flag_low(); }
    bool               mode_is_qualities() const { return flag_qualities(); }
    const std::string& root() const { return m_root; }

    // Scene signature guard: the first call stores, later calls compare.
    bool signature_changed(const std::string& signature);

    // Applies this run's recipe to a request built from product settings + startup options.
    void apply_overrides(AOExecutionRequest& request, AOSettings& settings) const;
    // Human-readable description for status headers.
    std::string description() const;
    // Directory name of the current run ("1-cs-low-legacy", ...).
    std::string run_name(int run) const;
    // Requested-vs-actual validation for the completed capture run.
    bool validate_actual(const AOResult& result) const;

    // Stage transitions (sequenced by the controller).
    void enter_capture(const std::string& capture_directory);
    bool advance(int total_runs); // Returns false when the experiment completed.

private:
    bool flag_low() const { return m_mode == Mode::LowLegacy; }
    bool flag_qualities() const { return m_mode == Mode::Qualities; }
    bool flag_backends() const { return m_mode == Mode::Qualities || m_mode == Mode::Backends; }
    bool flag_reconstruction() const { return m_mode == Mode::FsReconstruction; }
    bool flag_pixel_fs() const { return m_mode == Mode::PixelFs; }
    bool flag_pixel() const { return m_mode == Mode::PixelFs || m_mode == Mode::Pixel; }
    bool flag_split() const { return m_mode == Mode::Split; }
    bool flag_confidence() const { return m_mode == Mode::Confidence; }
    bool flag_composite() const { return m_mode == Mode::Composite; }
    bool flag_sampling() const { return m_mode == Mode::FsSampling; }
    bool flag_denoise() const { return m_mode == Mode::FsDenoise; }
    bool flag_filter() const { return m_mode == Mode::FsSampling || m_mode == Mode::FsDenoise || m_mode == Mode::FsFilter; }

    int         m_stage{0};
    int         m_run{0};
    Mode        m_mode{Mode::DefaultDirections};
    std::string m_root;
    std::string m_signature;
};

}}} // namespace Slic3r::GUI::AO
#endif
