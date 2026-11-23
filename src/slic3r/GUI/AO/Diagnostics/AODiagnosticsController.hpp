#ifndef slic3r_GUI_AO_Diagnostics_AODiagnosticsController_hpp_
#define slic3r_GUI_AO_Diagnostics_AODiagnosticsController_hpp_

#include "../AOConfig.hpp"
#include "../AOObserver.hpp"
#include "../AOPlan.hpp"
#include "../AOTypes.hpp"
#include "AOCaptureWriter.hpp"
#include "AOExperimentRunner.hpp"
#include "GLAOProfiler.hpp"

#include <array>
#include <string>

namespace Slic3r {
class GLShaderProgram;
namespace GUI { namespace AO {

// Per-frame diagnostic needs supplied by the canvas adapter at frame start.
struct AODiagnosticRequest
{
    const char* scene_view{"external"}; // Diagnostic tag; static literal from the canvas.
    float       explosion_ratio{1.0f};
    bool        capture_samples{false}; // Per-sample color/depth products requested.
    bool        defer_capture_completion{true};
    bool        frame_timing_requested{false}; // Compact ring samples this frame (panel open).
    bool        measurement_active{false};     // Benchmark consuming this frame.
};

// Assembles capture, experiment and performance consumers behind the observer protocol.
// Consumes only the public feature protocol; never rewrites a live production plan. With no
// consumer active it allocates no queries, performs no readback and builds no reports.
class AODiagnosticsController : public AOFrameObserver
{
public:
    AODiagnosticsController()  = default;
    ~AODiagnosticsController() = default;

    // Frame lifecycle. The feature and frame data are borrowed for the frame only.
    void begin_frame(const AOExecutionRequest&  request,
                     const AOSettings&          settings,
                     const AOFrameInput&        input,
                     const AODiagnosticRequest& diagnostics);
    // Observer protocol invoked by the feature during render.
    void mark(AOStage stage) override;
    void execution_ready(const AOExecutionView& view) override { m_execution = view; }
    bool context_lost() const override;
    void resource_ready(AOStage stage, const AOResourceView& view) override;
    void stage_failed(AOStage stage, const std::string& reason, AOStatus status = AOStatus::Failed) override;

    // Canvas-owned stage marks (main scene, presentation). No compact sampling.
    void canvas_mark(AOStage stage);

    // --- Benchmark facade (schema 10 contract preserved). ---
    bool               benchmark_active() const { return m_profiler.benchmark_active(); }
    bool               benchmark_completed() const { return m_profiler.benchmark_completed(); }
    const std::string& benchmark_directory() const { return m_profiler.benchmark_directory(); }
    bool               benchmark_request(const std::string& root) { return m_profiler.request(root); }
    void               benchmark_begin(const std::string& signature) { m_profiler.benchmark_begin(signature); }
    void               benchmark_end() { m_profiler.benchmark_end(); }
    void benchmark_finish(const std::string& reason, bool context_current = true) { m_profiler.benchmark_finish(reason, context_current); }
    void benchmark_pipeline(const std::string& description) { m_profiler.benchmark_pipeline(description); }
    void benchmark_release() { m_profiler.release(); }
    void benchmark_forget() { m_profiler.forget(); }

    // --- One-shot capture. ---
    void request_capture(const std::string& directory) { m_capture.request(directory); }
    void cancel_capture() { m_capture.cancel(); }
    bool capture_requested() const { return m_capture.capture_requested(); }
    // Completes a deferred capture after the presented frame; writes color-presented and the
    // atomic completion marker.
    bool finalize_capture(const AOFrameInput& presented_input, bool success);

    // --- Comparison experiment (Ctrl+Shift+F10 sequencing). ---
    bool comparison_active() const { return m_runner.active(); }
    int  comparison_stage() const { return m_runner.stage(); }
    // Scene signature guard; stores on first call, compares afterwards.
    bool comparison_signature_changed(const std::string& signature) { return m_runner.signature_changed(signature); }
    // Returns false when the runner could not start (directory or status failure).
    bool start_comparison(const std::string& root, const std::string& mode);
    void finish_comparison(const std::string& reason) { m_runner.finish(reason); }
    // Called after SwapBuffers; advances measure->capture->validate->next.
    void advance_comparison(const AOResult& result);

    // --- Render-statistics panel. ---
    bool                         panel_has_sample() const { return m_profiler.has_gpu_sample(); }
    bool                         panel_timing_supported() const { return m_profiler.timing_supported(); }
    const std::array<double, 8>& panel_gpu_ms() const { return m_profiler.gpu_ms(); }
    std::string                  panel_backend_label(const AOResult& result) const;

    void release();
    void forget();

    const AOExperimentRunner& runner() const { return m_runner; }

private:
    void        write_capture_parameters();
    void        write_backend_info();
    void        write_compute_info();
    void        capture_stage_products(AOStage stage, const AOResourceView& view);
    void        capture_precomposite_products();
    void        capture_postcomposite_products();
    bool        write_sample_diagnostics(unsigned int vao, GLShaderProgram* shader, int samples);
    std::string pipeline_description() const;
    void        draw_composite_diagnostic(GLShaderProgram* shader, int debug_view, unsigned int target, int width, int height);

    AOExecutionView     m_execution;
    AOExecutionRequest  m_request;
    AOSettings          m_settings;
    AOFrameInput        m_input;
    AODiagnosticRequest m_diagnostics;
    GLAOProfiler        m_profiler;
    AOCaptureSession    m_capture;
    AOExperimentRunner  m_runner;

    std::string        m_capture_path;
    bool               m_capture_ok{true};
    bool               m_context_lost{false};
    std::vector<float> m_depth_before, m_depth_after;
    std::string        m_comparison_capture;
};

}} // namespace GUI::AO
} // namespace Slic3r
#endif
