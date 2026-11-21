#ifndef slic3r_GUI_AO_AOObserver_hpp_
#define slic3r_GUI_AO_AOObserver_hpp_

#include "AOTypes.hpp"
#include "AOPlan.hpp"
#include "AOFullscreenPass.hpp"

namespace Slic3r { namespace GUI { namespace AO {

// Semantic timing/observation points of a frame. Numeric values of timestamped stages are the
// stable benchmark/CSV index contract (schema 10); the exporter maps these to its columns and
// algorithm code never maintains the indices itself. Observation-only stages (>=100) carry no
// timestamp and exist for capture sequencing.
enum class AOStage {
    BenchmarkFrameStart  = 0,
    MainSceneBegin       = 1,
    AOBegin              = 2,
    ReceiverBegin        = 3,
    ReceiverDepthReady   = 4, // Resolved receiver depth complete.
    NormalsReady         = 5,
    EvaluationReady      = 6,
    FilterFirstPass      = 16,
    FilterThirdPass      = 26,
    FilterRefined        = 17,
    FilterChainReady     = 7,
    UpsampleReady        = 8,
    CompositeReady       = 9,
    AOEnd                = 10,
    ColorResolveBegin    = 11,
    WindowPresented      = 12,
    MainSceneEnd         = 14,
    BenchmarkFrameEnd    = 15,
    ColorResolveEnd      = 18, // Owned MSAA color resolved to the single-sample buffer.
    ComputeBegin         = 19,
    ComputePrefiltered   = 20,
    ComputeEvaluated     = 21,
    ComputeDenoiseFirst  = 22,
    ComputeDenoiseSecond = 23,
    ComputeDenoiseThird  = 24,
    ResolvedDepthCopied  = 25,
    SplitClassified      = 27,
    SplitComposited      = 28,
    SampleDepthCopied    = 29,
    PostReceiverDraw     = 30,
    // Observation-only: no benchmark timestamp, used to sequence captures.
    ColorBeforeComposite   = 100,
    ColorCompositeComplete = 101,
};

// True when the stage has a benchmark/compact timestamp slot.
bool stage_has_timestamp(AOStage stage);

// Borrowed execution facts for observation. No backend object or resource owner escapes.
// Uniform matrix pointers and programs are valid only during this frame's callbacks.
struct AOExecutionView
{
    bool              ready{false}, rendered{false};
    AOPlan            plan;
    AOProgramIdentity programs;
    AOUniformValues   uniforms;
    std::string       fallback_reason;
    unsigned int      vao{0}, sample_fbo{0}, split_texture{0}, cs_evaluate_program{0};
    GLShaderProgram*  composite_program{nullptr};
    GLShaderProgram*  sample_program{nullptr};
    int               source_samples{0}, sample_count{0}, color_sample_count{0};
    int               cs_padded_width{0}, cs_padded_height{0};
    bool              pixel_route{false}, sample_route{false}, split_route{false};
};

// Observation protocol published by the production AO core. Implementations must return
// promptly: after a callback returns, production execution may overwrite the reported
// resources. Diagnostics must copy what they need inside the callback.
class AOFrameObserver
{
public:
    virtual ~AOFrameObserver() = default;
    // Timing/sequencing mark; also the safe point to read stage outputs.
    virtual void mark(AOStage stage) = 0;
    virtual void execution_ready(const AOExecutionView&) {}
    virtual bool context_lost() const { return false; }
    // Output resource of a just-completed stage (valid until the next mark).
    virtual void resource_ready(AOStage stage, const AOResourceView& view) {}
    // Execution cancelled or failed after it started; reason is diagnostic text.
    virtual void stage_failed(AOStage stage, const std::string& reason, AOStatus status = AOStatus::Failed)
    {
        (void) stage;
        (void) reason;
        (void) status;
    }
};

}}} // namespace Slic3r::GUI::AO
#endif
