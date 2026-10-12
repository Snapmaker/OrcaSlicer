#include "GLAOFeature.hpp"
#include "AOGLUtils.hpp"
#include "../GLShader.hpp"

#include <GL/glew.h>
#include <algorithm>
#include <optional>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI { namespace AO {

namespace {
class FrameObserver final : public AOFrameObserver
{
public:
    FrameObserver(AOFrameObserver& consumer, const AOFrameInput& input, unsigned int epoch)
        : m_consumer(consumer), m_input(input), m_epoch(epoch)
    {}
    void mark(AOStage stage) override { m_consumer.mark(stage); }
    void execution_ready(const AOExecutionView& view) override { m_consumer.execution_ready(view); }
    bool context_lost() const override { return m_consumer.context_lost(); }
    void stage_failed(AOStage stage, const std::string& reason, AOStatus status) override
    {
        m_consumer.stage_failed(stage, reason, status);
    }
    void resource_ready(AOStage stage, const AOResourceView& borrowed) override
    {
        AOResourceView view = borrowed;
        view.context        = m_input.context;
        view.generation     = m_epoch;
        view.valid_width    = std::min(view.width, std::max(1, m_input.viewport[2] >> view.level));
        view.valid_height   = std::min(view.height, std::max(1, m_input.viewport[3] >> view.level));
        if (view.kind == AOResourceKind::Framebuffer)
            view.role = AOResourceRole::Depth;
        if (!view.format && view.kind == AOResourceKind::Texture2D)
            view.format = view.ao_encoding == AOEncoding::R8UIEncoded ? GL_R8UI : GL_R8;
        m_consumer.resource_ready(stage, view);
    }

private:
    AOFrameObserver&    m_consumer;
    const AOFrameInput& m_input;
    unsigned int        m_epoch;
};

bool separable_projection(const Matrix4d& inverse)
{
    return inverse(0, 1) == 0.0 && inverse(0, 2) == 0.0 && inverse(0, 3) == 0.0 && inverse(1, 0) == 0.0 && inverse(1, 2) == 0.0 &&
           inverse(1, 3) == 0.0 && inverse(2, 0) == 0.0 && inverse(2, 1) == 0.0 && inverse(3, 0) == 0.0 && inverse(3, 1) == 0.0;
}
} // namespace

bool GLAOFeature::fail(const std::string& reason)
{
    m_visual_signature.clear();
    if (m_failure.empty())
        BOOST_LOG_TRIVIAL(warning) << "Ambient occlusion disabled: " << reason;
    m_failure = reason;
    return false;
}

bool GLAOFeature::has_resources() const
{
    return m_inputs.vao() != 0 || m_inputs.normals_texture() != 0 || m_fragment.raw_texture() != 0 || m_compute.output() != 0 ||
           m_compositor.split_texture() != 0;
}

AOBackendAvailability GLAOFeature::compute_availability(const AOExecutionRequest& request,
                                                        const AOSettings&         settings,
                                                        const GLAOProgramCatalog& catalog,
                                                        const AOFrameInput&       input)
{
    AOBackendAvailability availability;
    availability.fs_edge_programs_ready       = catalog.fs_edge_ready() && !m_fragment.edge_failed();
    availability.fs_fast_reconstruction_ready = catalog.fs_fast_reconstruction_ready() && separable_projection(input.inverse_projection);
    availability.fs_geometry_ready            = catalog.refs().evaluate && catalog.refs().filter_geometry;
    if (request.backend != BackendPreference::Fragment) {
        const auto caps                    = m_compute.capabilities();
        availability.cs_supported          = caps.supported;
        availability.cs_unavailable_reason = caps.reason;
        availability.cs_programs_ready     = catalog.cs_ready(request.cs_low_reference && settings.quality == AOSettings::Quality::Low);
        availability.cs_health_reason      = m_compute.failure_reason();
    }
    return availability;
}

bool GLAOFeature::validate_fs_programs(const AOFSPlan& plan, const GLAOProgramCatalog::Refs& refs)
{
    if (plan.edge_filter) {
        if (!refs.edge_evaluate || !refs.edge_denoise || (plan.fast_reconstruction && !refs.edge_evaluate_xy))
            return fail("FS AO edge evaluation or filter shader is unavailable");
    } else if (!refs.evaluate || !refs.filter_geometry)
        return fail("FS AO geometry evaluation or filter shader is unavailable");
    if (plan.upsample && !refs.upsample)
        return fail("FS AO upsample shader is unavailable");
    return true;
}

GLAOFeature::Preparation GLAOFeature::prepare(const AOExecutionRequest& request,
                                              const AOSettings&         settings,
                                              const AOFrameInput&       input,
                                              const GLAOProgramCatalog& catalog,
                                              AOFrameObserver*          observer)
{
    m_prepared = false;
    m_catalog  = &catalog;
    m_forbidden_reason.clear();
    if (input.viewport[2] != m_prepare_width || input.viewport[3] != m_prepare_height) {
        m_prepare_width  = input.viewport[2];
        m_prepare_height = input.viewport[3];
        m_fragment.reset_health();
        m_inputs.reset_health();
        m_compositor.reset_health();
        m_compute.retry_resource_failure();
        if (m_failure.find("allocation") != std::string::npos)
            m_failure.clear();
    }
    if (!(m_program_identity == catalog.identity())) {
        m_program_identity = catalog.identity();
        if (m_failure != "OpenGL context lost")
            m_failure.clear();
        m_compute.reset_health();
        m_fragment.reset_health();
        m_inputs.reset_health();
        m_compositor.reset_health();
    }
    if (m_failure == "OpenGL context lost")
        return Preparation::ContextLost;
    if (settings.quality == AOSettings::Quality::Off || !m_failure.empty())
        return Preparation::Failed;
    if (!GLEW_VERSION_3_1) {
        fail("OpenGL 3.1 is required");
        return Preparation::Failed;
    }
    const int w = input.viewport[2], h = input.viewport[3];
    if (w <= 0 || h <= 0)
        return Preparation::Failed;
    if (!catalog.common_ready()) {
        fail("a required common AO shader is unavailable");
        return Preparation::Failed;
    }
    // Keep the caller's scene target and bindings intact through every allocation path.
    AOGLStateGuard state;
    bool           pending_loss = false;
    consume_errors(&pending_loss);
    if (pending_loss) {
        state.forget_context();
        fail("OpenGL context lost");
        if (observer)
            observer->stage_failed(AOStage::AOBegin, m_failure, AOStatus::ContextLost);
        return Preparation::ContextLost;
    }
    const auto& refs = catalog.refs();
    // Reference comparisons abort rather than silently changing their MSAA path.
    if (request.composite == CompositeVariant::Reference &&
        (!refs.composite_pixel_reference || (GLEW_VERSION_4_0 && !refs.composite_sample_reference))) {
        m_forbidden_reason = "reference composite programs unavailable";
        return Preparation::Forbidden;
    }

    AOExecutionRequest resolved = request;
    // Program-driven degradations: missing experiment programs downgrade the route, matching
    // the historical silent fallbacks (split -> per-sample, confidence -> direct).
    if (resolved.composite == CompositeVariant::Confidence && (!catalog.confidence_ready(true) || m_inputs.confidence_failed()))
        resolved.composite = CompositeVariant::Sample;
    if (resolved.composite == CompositeVariant::SplitMsaa && (!catalog.split_ready() || m_compositor.split_failed()))
        resolved.composite = CompositeVariant::Sample;
    if (resolved.composite != request.composite && request.fallback == FallbackPolicy::Forbid) {
        m_forbidden_reason = "requested composite programs or resources unavailable";
        return Preparation::Forbidden;
    }

    const AOBackendAvailability availability = compute_availability(resolved, settings, catalog, input);
    AOPlanDecision              decision     = make_plan(resolved, settings, w, h, availability);
    if (decision.forbidden) {
        m_forbidden_reason = "forced CS unavailable: " + decision.fallback_reason;
        return Preparation::Forbidden;
    }
    m_plan      = decision.plan;
    m_fell_back = decision.fell_back || resolved.composite != request.composite;
    // Historical selection/fallback reason (empty on a healthy CS plan); recorded in the
    // benchmark pipeline description and backend-info metadata.
    m_backend_reason = decision.fallback_reason;
    if (resolved.composite != request.composite)
        m_backend_reason = "requested composite programs or resources unavailable";

    if (m_plan.backend() == Backend::Fragment && !validate_fs_programs(m_plan.fs(), refs))
        return Preparation::Failed;

    // Saved bindings may refer to work resources invalidated by this preparation.
    for (unsigned int texture : {m_inputs.depth_texture(), m_inputs.normals_texture(), m_inputs.confidence_texture(),
                                 m_inputs.sample_depth_texture(), m_fragment.raw_texture(), m_fragment.filtered_texture(),
                                 m_fragment.full_texture(), m_fragment.edge_texture(), m_compositor.split_texture()})
        state.forget_texture(texture);
    for (unsigned int fbo : {m_inputs.depth_fbo(), m_inputs.normals_fbo(), m_inputs.sample_fbo(), m_fragment.raw_fbo(),
                             m_fragment.filtered_fbo(), m_fragment.full_fbo(), m_compositor.split_fbo()})
        state.forget_framebuffer(fbo);
    state.forget_vertex_array(m_inputs.vao());
    for (int index = 0; index < 6; ++index)
        state.forget_texture(m_compute.texture(index));
    const auto preparation_failed = [&](const std::string& reason) {
        const bool lost = reason == "OpenGL context lost";
        if (lost)
            state.forget_context();
        if (observer)
            observer->stage_failed(AOStage::AOBegin, reason, lost ? AOStatus::ContextLost : AOStatus::Failed);
        fail(reason);
        return lost ? Preparation::ContextLost : Preparation::Failed;
    };
    if (m_inputs.width() != w || m_inputs.height() != h)
        m_inputs.release();
    m_inputs.set_context(input.context);
    if (!m_inputs.prepare(w, h, m_plan.composite == CompositeVariant::Confidence)) {
        if (m_inputs.failure_reason().empty() && m_inputs.confidence_failed() && m_plan.composite == CompositeVariant::Confidence) {
            if (request.fallback == FallbackPolicy::Forbid) {
                m_forbidden_reason = "confidence allocation failed";
                return Preparation::Forbidden;
            }
            m_plan.composite = CompositeVariant::Sample;
            m_fell_back      = true;
            m_backend_reason = "confidence allocation failed";
            if (!m_inputs.prepare(w, h, false))
                return preparation_failed(m_inputs.failure_reason());
        } else
            return preparation_failed(m_inputs.failure_reason());
    }
    const auto prepare_fragment = [&]() {
        if (!validate_fs_programs(m_plan.fs(), refs))
            return false;
        if (m_fragment.prepare(m_plan.fs(), w, h))
            return true;
        if (!m_fragment.failure_reason().empty() || !m_fragment.edge_failed())
            return false;
        if (request.fallback == FallbackPolicy::Forbid) {
            m_forbidden_reason = "FS edge filter allocation failed";
            return false;
        }
        AOExecutionRequest geometry_request = resolved;
        geometry_request.backend            = BackendPreference::Fragment;
        geometry_request.composite          = m_plan.composite;
        geometry_request.fs_edge_filter     = false;
        m_plan                              = make_plan(geometry_request, settings, w, h, availability).plan;
        m_fell_back                         = true;
        m_backend_reason                    = "FS edge filter allocation failed";
        return validate_fs_programs(m_plan.fs(), refs) && m_fragment.prepare(m_plan.fs(), w, h);
    };
    if (m_plan.backend() == Backend::Compute) {
        if (m_fragment.raw_texture())
            m_fragment.release();
        std::array<unsigned int, 3> programs{{refs.compute[0]->get_id(),
                                              (m_plan.cs().legacy_low ? refs.compute_evaluate_legacy_low : refs.compute[1])->get_id(),
                                              refs.compute[2]->get_id()}};
        if (!m_compute.prepare(w, h, programs, m_plan.cs().denoise_passes)) {
            m_backend_reason = m_compute.failure_reason().empty() ? "invalid CS parameters" : m_compute.failure_reason();
            if (m_backend_reason == "OpenGL context lost")
                return preparation_failed(m_backend_reason);
            if (request.fallback == FallbackPolicy::Forbid) {
                m_forbidden_reason = "CS initialization failed: " + m_backend_reason;
                return Preparation::Forbidden;
            }
            if (observer)
                observer->stage_failed(AOStage::AOBegin, "CS initialization failed: " + m_backend_reason);
            AOExecutionRequest fs_request = resolved;
            fs_request.backend            = BackendPreference::Fragment;
            fs_request.composite          = m_plan.composite;
            m_plan                        = make_plan(fs_request, settings, w, h, availability).plan;
            m_fell_back                   = true;
            if (!prepare_fragment())
                return preparation_failed(m_fragment.failure_reason().empty() ? m_failure : m_fragment.failure_reason());
            BOOST_LOG_TRIVIAL(warning) << "AO falling back to FS: " << m_backend_reason;
        }
    } else if (!prepare_fragment()) {
        if (!m_forbidden_reason.empty())
            return Preparation::Forbidden;
        return preparation_failed(m_fragment.failure_reason().empty() ? m_failure : m_fragment.failure_reason());
    }
    if (m_plan.backend() == Backend::Fragment && m_compute.output())
        m_compute.release();
    if (!m_compositor.prepare(m_plan, w, h)) {
        if (!m_compositor.split_failed())
            return preparation_failed(m_compositor.failure_reason());
        if (request.fallback == FallbackPolicy::Forbid) {
            m_forbidden_reason = "split composite allocation failed";
            return Preparation::Forbidden;
        }
        m_plan.composite = CompositeVariant::Sample;
        m_fell_back      = true;
        m_backend_reason = "split composite allocation failed";
    }

    m_width    = w;
    m_height   = h;
    m_prepared = true;
    return Preparation::Prepared;
}

void GLAOFeature::build_uniforms(const AOSettings& settings)
{
    const auto fs_samples          = fs_ao_samples(settings.quality, m_request_snapshot.fs_slices_override);
    m_uniforms                     = AOUniformValues{};
    m_uniforms.projection          = &m_frame_input.projection;
    m_uniforms.inverse_projection  = &m_frame_input.inverse_projection;
    m_uniforms.perspective         = m_frame_input.perspective;
    m_uniforms.full_width          = m_width;
    m_uniforms.full_height         = m_height;
    m_uniforms.ao_width            = m_plan.backend() == Backend::Compute ? m_plan.cs().width : m_plan.fs().eval_width;
    m_uniforms.ao_height           = m_plan.backend() == Backend::Compute ? m_plan.cs().height : m_plan.fs().eval_height;
    m_uniforms.radius              = settings.radius;
    m_uniforms.thickness           = settings.thickness;
    m_uniforms.intensity           = m_plan.backend() == Backend::Compute ? 1.0f : settings.intensity;
    m_uniforms.strength            = settings.strength;
    m_uniforms.slice_count         = fs_samples[0];
    m_uniforms.step_count          = fs_samples[1];
    m_uniforms.high_sampling_noise = settings.quality == AOSettings::Quality::High;
    m_uniforms.debug_view          = settings.debug_view;
    m_uniforms.capture_sample      = settings.debug_view == 5 || settings.debug_view == 6;
}

GLShaderProgram* GLAOFeature::select_sample_composite_shader(const GLAOProgramCatalog::Refs& refs) const
{
    if (m_split_active)
        return refs.split_edges;
    if (m_plan.composite == CompositeVariant::Confidence)
        return refs.composite_sample_confidence;
    if (m_plan.composite == CompositeVariant::Reference)
        return refs.composite_sample_reference;
    return refs.composite_sample;
}

GLShaderProgram* GLAOFeature::select_pixel_composite_shader(const GLAOProgramCatalog::Refs& refs) const
{
    if (m_plan.composite == CompositeVariant::Confidence)
        return refs.composite_pixel_confidence;
    if (m_plan.composite == CompositeVariant::Reference)
        return refs.composite_pixel_reference;
    return refs.composite_pixel;
}

AOResult GLAOFeature::render(const AOExecutionRequest&    request,
                             const AOSettings&            settings,
                             const AOFrameInput&          input,
                             const std::function<void()>& receiver_draw,
                             AOFrameObserver*             observer)
{
    AOResult result;
    result.requested_backend = request.backend;
    result.fallback          = request.fallback;
    if (settings.quality == AOSettings::Quality::Off) {
        result.status = AOStatus::DisabledByUser;
        return result;
    }
    if (!m_prepared || !m_failure.empty() || !m_catalog) {
        result.status      = AOStatus::Failed;
        result.reason      = m_failure.empty() ? "AO was not prepared" : m_failure;
        result.color_state = SceneColorState::Unchanged;
        return result;
    }
    m_frame_input      = input;
    m_request_snapshot = request;
    ++m_resource_epoch;
    std::optional<FrameObserver> frame_observer;
    if (observer) {
        frame_observer.emplace(*observer, input, m_resource_epoch);
        observer = &*frame_observer;
    }

    AOGLStateGuard state;
    bool           prior_loss = false;
    if (consume_errors(&prior_loss)) {
        if (prior_loss) {
            state.forget_context();
            fail("OpenGL context lost");
            result.status = AOStatus::ContextLost;
            result.reason = m_failure;
            if (observer)
                observer->stage_failed(AOStage::AOBegin, result.reason, result.status);
            return result;
        }
        BOOST_LOG_TRIVIAL(warning) << "GL error before AO depth capture";
    }
    state.fullscreen();

    const auto& refs = m_catalog->refs();
    build_uniforms(settings);

    // Set the requested variant before recording per-run metadata. Unsupported sample layouts
    // abort strict requests immediately after the depth copy below.
    m_pixel_active      = m_plan.composite == CompositeVariant::Pixel && settings.debug_view == 0;
    m_split_active      = m_plan.composite == CompositeVariant::SplitMsaa && settings.debug_view == 0;
    m_last_render_clean = false;
    publish_execution(observer);
    if (observer)
        observer->mark(AOStage::AOBegin);

    const auto lost_result = [&]() {
        state.forget_context();
        fail("OpenGL context lost");
        result.status = AOStatus::ContextLost;
        result.reason = m_failure;
        if (observer)
            observer->stage_failed(AOStage::AOBegin, result.reason, result.status);
        return result;
    };
    if (!m_inputs.capture_depth(input)) {
        // Historical contract: depth resolve failures latch AO off until the context changes.
        fail(m_inputs.failure_reason());
        result.status      = m_inputs.failure_reason() == "OpenGL context lost" ? AOStatus::ContextLost : AOStatus::Failed;
        result.reason      = m_inputs.failure_reason();
        result.color_state = SceneColorState::Unchanged;
        if (result.status == AOStatus::ContextLost)
            state.forget_context();
        return result;
    }
    if (observer) {
        observer->mark(AOStage::ResolvedDepthCopied); // Diagnostics: pre-receiver depth + parameters.
        AOResourceView view;
        view.kind        = AOResourceKind::Framebuffer;
        view.gl_name     = m_inputs.depth_fbo();
        view.width       = m_width;
        view.height      = m_height;
        view.depth_space = DepthSpace::SceneNormalized;
        view.role        = AOResourceRole::Depth;
        view.context     = input.context;
        view.generation  = m_inputs.generation();
        observer->resource_ready(AOStage::ResolvedDepthCopied, view);
    }
    if (observer && observer->context_lost())
        return lost_result();
    const bool sample_path_allowed = m_plan.composite != CompositeVariant::Pixel && refs.composite_sample;
    m_sample_active                = !m_pixel_active && m_inputs.capture_sample_depth(input, sample_path_allowed);
    if (m_inputs.failure_reason() == "OpenGL context lost")
        return lost_result();
    if (m_sample_active && observer) {
        observer->mark(AOStage::SampleDepthCopied);
        AOResourceView view;
        view.kind        = AOResourceKind::Texture2DMultisample;
        view.gl_name     = m_inputs.sample_depth_texture();
        view.samples     = m_inputs.sample_count();
        view.width       = m_width;
        view.height      = m_height;
        view.depth_space = DepthSpace::MultisampleSceneNormalized;
        view.role        = AOResourceRole::SampleDepth;
        observer->resource_ready(AOStage::SampleDepthCopied, view);
    }
    // Color sample capture is independent of whether AO uses a copied sample-depth texture.
    m_color_sample_count = input.source != 0 && input.source == input.target && m_inputs.source_samples() > 1 ? m_inputs.source_samples() :
                                                                                                                0;
    if ((request.require_msaa_comparison && (m_color_sample_count <= 1 || settings.debug_view != 0)) ||
        (request.require_sample_composite && !m_sample_active) ||
        (request.fallback == FallbackPolicy::Forbid && m_plan.composite != CompositeVariant::Pixel && settings.debug_view == 0 &&
         !m_sample_active)) {
        result.status      = AOStatus::Forbidden;
        result.reason      = "requested MSAA composite unavailable";
        result.color_state = SceneColorState::Unchanged;
        if (observer)
            observer->stage_failed(AOStage::AOBegin, result.reason);
        return result;
    }
    if (m_plan.composite != CompositeVariant::Pixel && settings.debug_view == 0 && !m_sample_active) {
        if (m_plan.composite != CompositeVariant::Reference)
            m_plan.composite = CompositeVariant::Pixel;
        m_pixel_active   = true;
        m_fell_back      = true;
        m_backend_reason = "sample depth unavailable; using pixel composite";
    }
    m_split_active = m_plan.composite == CompositeVariant::SplitMsaa && m_sample_active && m_inputs.sample_count() > 1 &&
                     m_inputs.sample_count() <= 8 && settings.debug_view == 0;
    if (m_plan.composite == CompositeVariant::SplitMsaa && !m_split_active && observer)
        observer->stage_failed(AOStage::AOBegin, "split composite requires its programs and 2..8 MSAA samples");

    publish_execution(observer);
    if (observer) {
        observer->mark(AOStage::ReceiverBegin);
        // Diagnostics: capture parameters (the sample route is final at this point).
        AOResourceView view;
        view.kind        = AOResourceKind::Framebuffer;
        view.gl_name     = m_inputs.depth_fbo();
        view.width       = m_width;
        view.height      = m_height;
        view.depth_space = DepthSpace::SceneNormalized;
        observer->resource_ready(AOStage::ReceiverBegin, view);
    }
    if (observer && observer->context_lost())
        return lost_result();
    m_inputs.bind_resolved_receiver_target();
    if (receiver_draw)
        receiver_draw();
    if (observer)
        observer->mark(AOStage::PostReceiverDraw);
    if (m_sample_active) {
        m_inputs.bind_sample_receiver_target();
        if (receiver_draw)
            receiver_draw();
    }
    if (observer) {
        observer->mark(AOStage::ReceiverDepthReady); // Diagnostics: post-receiver depth + mask.
        AOResourceView view;
        view.kind        = AOResourceKind::Framebuffer;
        view.gl_name     = m_inputs.depth_fbo();
        view.width       = m_width;
        view.height      = m_height;
        view.depth_space = DepthSpace::SceneNormalized;
        observer->resource_ready(AOStage::ReceiverDepthReady, view);
    }

    if (observer && observer->context_lost())
        return lost_result();
    state.fullscreen();
    m_inputs.draw_normals(m_plan.composite == CompositeVariant::Confidence ? refs.normal_confidence : refs.normal, m_uniforms);
    if (observer) {
        observer->mark(AOStage::NormalsReady);
        AOResourceView normals;
        normals.kind            = AOResourceKind::Texture2D;
        normals.gl_name         = m_inputs.normals_texture();
        normals.format          = GL_RG16F;
        normals.width           = m_width;
        normals.height          = m_height;
        normals.normal_encoding = NormalEncoding::OctahedralRG16F;
        normals.role            = AOResourceRole::Normals;
        observer->resource_ready(AOStage::NormalsReady, normals);
        if (m_inputs.confidence_texture()) {
            AOResourceView confidence  = normals;
            confidence.gl_name         = m_inputs.confidence_texture();
            confidence.format          = GL_R32F;
            confidence.role            = AOResourceRole::Confidence;
            confidence.normal_encoding = NormalEncoding::OctahedralRG16F;
            observer->resource_ready(AOStage::NormalsReady, confidence);
        }
    }
    if (observer && observer->context_lost())
        return lost_result();
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_inputs.normals_texture());

    unsigned int filtered_texture = 0;
    if (m_plan.backend() == Backend::Compute) {
        if (observer)
            observer->mark(AOStage::ComputeBegin);
        GLAOComputeBackend::Parameters parameters;
        const auto                     inverse = input.inverse_projection.cast<float>().eval();
        std::copy(inverse.data(), inverse.data() + 16, parameters.inverse.begin());
        parameters.width          = m_width;
        parameters.height         = m_height;
        parameters.radius         = settings.radius;
        parameters.perspective    = input.perspective;
        parameters.denoise_passes = m_plan.cs().denoise_passes;
        parameters.slices         = m_plan.cs().slices;
        parameters.steps          = m_plan.cs().steps;
        const int denoise_passes  = m_plan.cs().denoise_passes;
        // Emits a resource view without a timestamp mark (used when several outputs share
        // one stage mark so a query is never recorded twice).
        const auto emit_view = [&](AOStage stage, unsigned int texture, AOEncoding encoding, bool response_applied) {
            if (!observer)
                return;
            AOResourceView view;
            view.kind             = AOResourceKind::Texture2D;
            view.gl_name          = texture;
            view.width            = m_width;
            view.height           = m_height;
            view.ao_encoding      = encoding;
            view.response_applied = response_applied;
            view.role    = stage == AOStage::ComputeEvaluated   ? (response_applied ? AOResourceRole::RawAO : AOResourceRole::Edges) :
                           encoding == AOEncoding::R8Visibility ? AOResourceRole::FinalAO :
                                                                  AOResourceRole::DenoiseIntermediate;
            view.format  = encoding == AOEncoding::R8Visibility ? GL_R8 : GL_R8UI;
            view.context = input.context;
            observer->resource_ready(stage, view);
        };
        const auto stage_view = [&](AOStage stage, unsigned int texture, AOEncoding encoding, bool response_applied) {
            if (!observer)
                return;
            observer->mark(stage);
            emit_view(stage, texture, encoding, response_applied);
        };
        const bool cs_success = m_compute.render(
            m_inputs.depth_texture(), m_inputs.normals_texture(), parameters,
            [&](int stage) {
                if (!observer)
                    return;
                if (stage == 0) {
                    AOResourceView mips;
                    mips.kind        = AOResourceKind::Texture2D;
                    mips.gl_name     = m_compute.texture(0);
                    mips.format      = GL_R32F;
                    mips.width       = m_compute.padded_width();
                    mips.height      = m_compute.padded_height();
                    mips.mip_levels  = 5;
                    mips.depth_space = DepthSpace::PrefilteredLinearR32F;
                    mips.role        = AOResourceRole::DepthMips;
                    observer->mark(AOStage::ComputePrefiltered);
                    observer->resource_ready(AOStage::ComputePrefiltered, mips);
                } else if (stage == 1) {
                    // One timestamp for the evaluation stage; raw and edges are two views of it.
                    observer->mark(AOStage::ComputeEvaluated);
                    emit_view(AOStage::ComputeEvaluated, m_compute.texture(1), AOEncoding::R8UIEncoded, true);
                    emit_view(AOStage::ComputeEvaluated, m_compute.texture(2), AOEncoding::R8UIEncoded, false);
                    observer->mark(AOStage::EvaluationReady);
                } else {
                    const int pass = stage - 2; // 0-based denoise pass.
                    if (pass == denoise_passes - 1)
                        stage_view(pass == 0 ? AOStage::ComputeDenoiseFirst :
                                   pass == 1 ? AOStage::ComputeDenoiseSecond :
                                               AOStage::ComputeDenoiseThird,
                                   m_compute.texture(4), AOEncoding::R8Visibility, true);
                    else if (pass == 0)
                        stage_view(AOStage::ComputeDenoiseFirst, m_compute.texture(3), AOEncoding::R8UIEncoded, true);
                    else if (denoise_passes == 3 && pass == 1)
                        stage_view(AOStage::ComputeDenoiseSecond, m_compute.texture(5), AOEncoding::R8UIEncoded, true);
                    else
                        stage_view(denoise_passes == 3 ? AOStage::ComputeDenoiseThird :
                                                         (pass == 1 ? AOStage::ComputeDenoiseSecond : AOStage::ComputeDenoiseFirst),
                                   m_compute.texture(4), AOEncoding::R8Visibility, true);
                }
            },
            [observer]() { return observer && observer->context_lost(); });
        if (cs_success) {
            filtered_texture = m_compute.output();
        } else {
            m_backend_reason = m_compute.failure_reason().empty() ? "invalid CS parameters" : m_compute.failure_reason();
            if (observer)
                observer->stage_failed(AOStage::AOBegin, "CS dispatch failed: " + m_backend_reason,
                                       m_backend_reason == "OpenGL context lost" ? AOStatus::ContextLost : AOStatus::Failed);
            if (m_backend_reason == "OpenGL context lost") {
                state.forget_context();
                fail(m_backend_reason);
                result.status      = AOStatus::ContextLost;
                result.reason      = m_backend_reason;
                result.color_state = SceneColorState::Unchanged;
                return result;
            }
            m_compute.disable(m_backend_reason);
            if (request.fallback == FallbackPolicy::Forbid) {
                result.status      = AOStatus::Forbidden;
                result.reason      = "CS dispatch failed: " + m_backend_reason;
                result.color_state = SceneColorState::Unchanged;
                return result;
            }
            // Complete standard FS replacement plan (documented behavior change: the historical
            // fallback re-allocated full-resolution targets instead of the preset sizes).
            AOExecutionRequest fs_request = m_request_snapshot;
            fs_request.backend            = BackendPreference::Fragment;
            fs_request.composite          = m_plan.composite;
            AOBackendAvailability fs_availability;
            fs_availability.fs_edge_programs_ready       = m_catalog->fs_edge_ready() && !m_fragment.edge_failed();
            fs_availability.fs_fast_reconstruction_ready = m_catalog->fs_fast_reconstruction_ready() &&
                                                           separable_projection(input.inverse_projection);
            fs_availability.fs_geometry_ready = m_catalog->refs().evaluate && m_catalog->refs().filter_geometry;
            AOPlanDecision fs_decision        = make_plan(fs_request, settings, m_width, m_height, fs_availability);
            m_plan                            = fs_decision.plan;
            m_fell_back                       = true;
            if (!validate_fs_programs(m_plan.fs(), refs)) {
                result.status      = AOStatus::Failed;
                result.reason      = m_failure;
                result.color_state = SceneColorState::Unchanged;
                return result;
            }
            bool fs_prepared = m_fragment.prepare(m_plan.fs(), m_width, m_height);
            if (!fs_prepared && m_fragment.edge_failed() && m_fragment.failure_reason().empty()) {
                fs_request.fs_edge_filter = false;
                m_plan                    = make_plan(fs_request, settings, m_width, m_height, fs_availability).plan;
                fs_prepared               = validate_fs_programs(m_plan.fs(), refs) && m_fragment.prepare(m_plan.fs(), m_width, m_height);
            }
            if (!fs_prepared) {
                fail(m_fragment.failure_reason().empty() ? "FS fallback allocation failed" : m_fragment.failure_reason());
                const bool lost = m_failure == "OpenGL context lost";
                if (lost)
                    state.forget_context();
                result.status      = lost ? AOStatus::ContextLost : AOStatus::Failed;
                result.reason      = m_failure;
                result.color_state = SceneColorState::Unchanged;
                if (observer)
                    observer->stage_failed(AOStage::AOBegin, result.reason, result.status);
                return result;
            }
            build_uniforms(settings); // Evaluation size and response changed with the plan.
            BOOST_LOG_TRIVIAL(warning) << "XeGTAO falling back to FS: " << m_backend_reason;
        }
    }
    if (m_plan.backend() == Backend::Fragment) {
        // Preparing a runtime fallback allocates textures on unit 0. Restore the
        // scene depth before any FS pass samples it, including the final composite.
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_inputs.depth_texture());
        // Observers must see the replacement plan before its first stage callback.
        publish_execution(observer);
        // Unified comparisons measure the validated reuse programs; the reference
        // evaluation/filter variants are only registered, never selected here.
        filtered_texture = m_fragment.evaluate_and_filter(m_plan.fs(), refs, m_uniforms, m_inputs.vao(), m_inputs.normals_texture(),
                                                          observer);
    }
    publish_execution(observer);
    if (observer) {
        observer->mark(AOStage::FilterChainReady); // Diagnostics: backend metadata + filtered AO.
        AOResourceView view;
        view.kind             = AOResourceKind::Texture2D;
        view.gl_name          = filtered_texture;
        view.width            = m_plan.backend() == Backend::Compute ? m_plan.cs().width : m_plan.fs().eval_width;
        view.height           = m_plan.backend() == Backend::Compute ? m_plan.cs().height : m_plan.fs().eval_height;
        view.ao_encoding      = AOEncoding::R8Visibility;
        view.response_applied = m_plan.backend() == Backend::Compute ? m_plan.cs().response.output_exponent_applied :
                                                                       m_plan.fs().response.output_exponent_applied;
        observer->resource_ready(AOStage::FilterChainReady, view);
    }
    unsigned int final_texture = filtered_texture;
    if (m_plan.backend() == Backend::Fragment)
        final_texture = m_fragment.upsample(m_plan.fs(), refs, m_uniforms, m_inputs.vao(), filtered_texture);
    else {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, final_texture);
    }
    if (observer) {
        observer->mark(AOStage::UpsampleReady);
        AOResourceView view;
        view.kind             = AOResourceKind::Texture2D;
        view.gl_name          = final_texture;
        view.role             = AOResourceRole::FinalAO;
        view.width            = m_width;
        view.height           = m_height;
        view.ao_encoding      = AOEncoding::R8Visibility;
        view.response_applied = m_plan.backend() == Backend::Compute ? m_plan.cs().response.output_exponent_applied :
                                                                       m_plan.fs().response.output_exponent_applied;
        observer->resource_ready(AOStage::UpsampleReady, view);
    }
    if (observer && observer->context_lost())
        return lost_result();
    bool evaluation_lost = false;
    if (consume_errors(&evaluation_lost)) {
        if (evaluation_lost)
            state.forget_context();
        fail(evaluation_lost ? "OpenGL context lost" : "AO evaluation failed");
        result.status      = evaluation_lost ? AOStatus::ContextLost : AOStatus::Failed;
        result.reason      = m_failure;
        result.color_state = SceneColorState::Unchanged;
        if (observer)
            observer->stage_failed(AOStage::EvaluationReady, result.reason, result.status);
        return result;
    }
    m_sample_composite_shader = select_sample_composite_shader(refs);
    m_composite_shader        = m_sample_active && (settings.debug_view == 0 || settings.debug_view == 5 || settings.debug_view == 6) ?
                                    m_sample_composite_shader :
                                    select_pixel_composite_shader(refs);
    publish_execution(observer);
    if (observer)
        observer->mark(AOStage::ColorBeforeComposite); // Diagnostics: pre-blend scene color/samples.

    if (observer && observer->context_lost())
        return lost_result();
    const bool composite_multisample = (m_sample_active || (m_pixel_active && m_inputs.source_samples() > 1)) && settings.debug_view == 0;
    if (m_split_active)
        m_compositor.classify_surfaces(m_plan, refs, m_uniforms, m_inputs.vao(), input, m_inputs.sample_count(), observer);
    m_compositor.begin_composite_blend(settings.debug_view);
    m_sample_composite_shader = select_sample_composite_shader(refs);
    m_composite_shader        = m_sample_active && (settings.debug_view == 0 || settings.debug_view == 5 || settings.debug_view == 6) ?
                                    m_sample_composite_shader :
                                    select_pixel_composite_shader(refs);
    if (m_split_active)
        m_compositor.composite_split_fast(m_plan, refs, m_uniforms, m_inputs.vao(), input, m_width, m_height, composite_multisample,
                                          observer);
    m_compositor.composite(m_composite_shader, m_uniforms, m_inputs.vao(), input, m_width, m_height, composite_multisample, observer);
    if (observer)
        observer->mark(AOStage::AOEnd);

    bool composite_lost = false;
    m_last_render_clean = !consume_errors(&composite_lost);
    if (composite_lost) {
        state.forget_context();
        fail("OpenGL context lost");
        result.status      = AOStatus::ContextLost;
        result.reason      = m_failure;
        result.color_state = SceneColorState::NeedsRedraw;
        if (observer)
            observer->stage_failed(AOStage::CompositeReady, result.reason, result.status);
        return result;
    }
    publish_execution(observer);
    if (observer)
        observer->mark(AOStage::ColorCompositeComplete); // Diagnostics: post-blend products.
    if (observer && observer->context_lost())
        return lost_result();
    if (m_last_render_clean) {
        result.status          = m_fell_back ? AOStatus::SuccessWithFallback : AOStatus::Success;
        result.fell_back       = m_fell_back;
        result.fallback_reason = m_backend_reason;
        result.color_state     = SceneColorState::Composited;
        record_actual(request, settings, result);
        m_visual_signature = result.visual_signature;
    } else {
        if (composite_lost)
            state.forget_context();
        fail(composite_lost ? "OpenGL context lost" : "AO composite failed");
        result.status      = composite_lost ? AOStatus::ContextLost : AOStatus::Failed;
        result.reason      = m_failure;
        result.color_state = SceneColorState::NeedsRedraw;
        if (observer)
            observer->stage_failed(AOStage::CompositeReady, result.reason, result.status);
    }
    return result;
}

void GLAOFeature::record_actual(const AOExecutionRequest& request, const AOSettings& settings, AOResult& result) const
{
    AOActualConfig& actual = result.actual;
    actual.executed        = true;
    actual.backend         = m_plan.backend();
    actual.quality         = settings.quality;
    if (m_plan.backend() == Backend::Compute) {
        actual.eval_width          = m_plan.cs().width;
        actual.eval_height         = m_plan.cs().height;
        actual.slices              = m_plan.cs().slices;
        actual.steps               = m_plan.cs().steps;
        actual.filter              = FilterMode::Connectivity;
        actual.filter_passes       = m_plan.cs().denoise_passes;
        actual.cs_low_reference    = m_plan.cs().legacy_low;
        actual.response            = m_plan.cs().response;
        actual.cs_evaluate_program = m_compute.evaluation_program();
    } else {
        actual.eval_width          = m_plan.fs().eval_width;
        actual.eval_height         = m_plan.fs().eval_height;
        actual.slices              = m_plan.fs().slices;
        actual.steps               = m_plan.fs().steps;
        actual.filter              = m_plan.fs().edge_filter ? FilterMode::Connectivity : FilterMode::Geometry;
        actual.filter_passes       = m_plan.fs().edge_filter ? m_plan.fs().edge_passes : (m_plan.fs().upsample ? 1 : 2);
        actual.fast_reconstruction = m_plan.fs().fast_reconstruction;
        actual.response            = m_plan.fs().response;
    }
    actual.composite = m_plan.composite;
    if (m_catalog)
        actual.programs = m_catalog->identity();
    result.visual_signature = make_visual_signature(actual, settings);
}

void GLAOFeature::publish_execution(AOFrameObserver* observer) const
{
    if (!observer)
        return;
    AOExecutionView view;
    view.ready               = true;
    view.rendered            = m_last_render_clean;
    view.plan                = m_plan;
    view.programs            = m_program_identity;
    view.uniforms            = m_uniforms;
    view.fallback_reason     = m_backend_reason;
    view.vao                 = m_inputs.vao();
    view.sample_fbo          = m_inputs.sample_fbo();
    view.split_texture       = m_compositor.split_texture();
    view.source_samples      = m_inputs.source_samples();
    view.sample_count        = m_inputs.sample_count();
    view.color_sample_count  = m_color_sample_count;
    view.pixel_route         = m_pixel_active;
    view.sample_route        = m_sample_active;
    view.split_route         = m_split_active;
    view.composite_program   = m_composite_shader;
    view.sample_program      = m_sample_composite_shader;
    view.cs_evaluate_program = m_compute.evaluation_program();
    view.cs_padded_width     = m_compute.padded_width();
    view.cs_padded_height    = m_compute.padded_height();
    observer->execution_ready(view);
}

void GLAOFeature::release()
{
    m_compute.release();
    m_fragment.release();
    m_compositor.release();
    m_inputs.release();
    m_prepared = false;
}

void GLAOFeature::forget_context()
{
    m_visual_signature.clear();
    m_compute.forget_context();
    m_fragment.forget_context();
    m_compositor.forget_context();
    m_inputs.forget_context();
    m_plan     = AOPlan{};
    m_prepared = false;
    m_width = m_height = 0;
    m_backend_reason.clear();
    m_fell_back = false;
    m_failure.clear();
    m_pixel_active = m_split_active = m_sample_active = false;
    m_color_sample_count                              = 0;
}

}}} // namespace Slic3r::GUI::AO
