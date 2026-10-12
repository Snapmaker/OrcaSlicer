#include "AODiagnosticsController.hpp"
#include "../AOFullscreenPass.hpp"
#include "../../GLShader.hpp"
#include "GLAOReadback.hpp"
#include "../AOGLUtils.hpp"

#include <GL/glew.h>
#include <boost/log/trivial.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace Slic3r { namespace GUI { namespace AO {

void AODiagnosticsController::begin_frame(const AOExecutionRequest&  request,
                                          const AOSettings&          settings,
                                          const AOFrameInput&        input,
                                          const AODiagnosticRequest& diagnostics)
{
    m_execution = AOExecutionView{};
    m_profiler.stop_frame_timing();
    m_context_lost = false;
    AOReadback::reset_context_loss();
    m_request     = request;
    m_settings    = settings;
    m_input       = input;
    m_diagnostics = diagnostics;
    m_capture_ok  = true;
    m_depth_before.clear();
    m_depth_after.clear();
    m_capture_path.clear();
    // Captures never run while the benchmark measures.
    if (m_profiler.benchmark_active())
        m_capture.cancel();
}

void AODiagnosticsController::mark(AOStage stage)
{
    switch (stage) {
    case AOStage::AOBegin: {
        // Claim the capture request only once the AO render actually starts (a failed
        // preparation never starts a partial product set; canvas finalization cancels a failed frame).
        if (m_capture_path.empty() && m_capture.capture_requested()) {
            m_capture_path = m_capture.begin_frame();
            if (!m_capture_path.empty()) {
                std::error_code error;
                std::filesystem::create_directories(m_capture_path, error);
                if (error) {
                    BOOST_LOG_TRIVIAL(warning) << "AO capture directory unavailable: " << error.message();
                    m_capture_path.clear();
                } else {
                    std::filesystem::remove(std::filesystem::path(m_capture_path) / "capture-complete.txt", error);
                    if (error) {
                        BOOST_LOG_TRIVIAL(warning) << "AO capture cannot clear previous completion marker: " << error.message();
                        m_capture_path.clear();
                    }
                }
            }
        }
        if (m_execution.ready && m_profiler.benchmark_active())
            m_profiler.benchmark_pipeline(pipeline_description());
        if (m_capture_path.empty() && (m_diagnostics.frame_timing_requested || m_profiler.benchmark_active()))
            m_profiler.begin_frame_timing(); // Marks AOBegin on both rings.
        else
            m_profiler.mark(AOStage::AOBegin);
        break;
    }
    case AOStage::AOEnd: m_profiler.end_frame_timing(); break;
    default: m_profiler.mark(stage); break;
    }
    if (stage == AOStage::ColorBeforeComposite)
        capture_precomposite_products();
    if (stage == AOStage::ColorCompositeComplete)
        capture_postcomposite_products();
}

void AODiagnosticsController::canvas_mark(AOStage stage) { m_profiler.mark(stage); }

void AODiagnosticsController::stage_failed(AOStage stage, const std::string& reason, AOStatus status)
{
    (void) stage;
    m_profiler.cancel_frame_timing();
    if (m_profiler.benchmark_active())
        m_profiler.benchmark_finish("ABORTED: " + reason, status != AOStatus::ContextLost);
    if (status == AOStatus::ContextLost) {
        m_context_lost = true;
        forget();
    }
}

bool AODiagnosticsController::context_lost() const { return m_context_lost || AOReadback::context_lost(); }

void AODiagnosticsController::resource_ready(AOStage stage, const AOResourceView& view)
{
    if (m_capture_path.empty() || context_lost())
        return;
    capture_stage_products(stage, view);
}

void AODiagnosticsController::capture_stage_products(AOStage stage, const AOResourceView& view)
{
    const std::filesystem::path path(m_capture_path);
    switch (stage) {
    case AOStage::ResolvedDepthCopied: {
        std::vector<float> depth;
        if (AOReadback::read_depth(depth, view.gl_name, view.width, view.height)) {
            m_depth_before = depth;
            m_capture_ok &= AOCaptureWriter::write_pfm((path / "depth-before-receiver.pfm").string(), depth, view.width, view.height, 1);
        } else
            m_capture_ok = false;
        break;
    }
    case AOStage::ReceiverBegin: {
        // The sample route is final by now (old code wrote the parameters after the
        // MSAA-comparison checks).
        write_capture_parameters();
        break;
    }
    case AOStage::ReceiverDepthReady: {
        std::vector<float> depth;
        if (AOReadback::read_depth(depth, view.gl_name, view.width, view.height)) {
            m_depth_after = depth;
            m_capture_ok &= AOCaptureWriter::write_pfm((path / "depth-after-receiver.pfm").string(), depth, view.width, view.height, 1);
            if (m_depth_before.size() != depth.size()) {
                m_capture_ok = false;
                break;
            }
            std::vector<unsigned char> receiver_mask(depth.size());
            for (size_t i = 0; i < depth.size(); ++i)
                receiver_mask[i] = depth[i] < m_depth_before[i] ? 255 : 0;
            m_capture_ok &= AOCaptureWriter::write_gray_bmp((path / "receiver-mask.bmp").string(), receiver_mask, view.width, view.height);
        } else
            m_capture_ok = false;
        break;
    }
    case AOStage::NormalsReady: {
        if (view.role == AOResourceRole::Confidence) {
            std::vector<unsigned char> confidence;
            if (AOReadback::read_texture(confidence, view.gl_name, view.width, view.height))
                m_capture_ok &= AOCaptureWriter::write_gray_bmp((path / "confidence-precomputed.bmp").string(), confidence, view.width,
                                                                view.height);
            else
                m_capture_ok = false;
        } else {
            std::vector<float> normals;
            if (AOReadback::read_normals(normals, view.gl_name, view.width, view.height, m_depth_after))
                m_capture_ok &= AOCaptureWriter::write_pfm((path / "normals-view.pfm").string(), normals, view.width, view.height, 3);
            else
                m_capture_ok = false;
        }
        break;
    }
    case AOStage::EvaluationReady: {
        if (m_execution.ready && m_execution.plan.backend() == Backend::Fragment) {
            std::vector<unsigned char> raw;
            if (AOReadback::read_texture(raw, view.gl_name, view.width, view.height))
                m_capture_ok &= AOCaptureWriter::write_gray_bmp((path / "ao-raw.bmp").string(), raw, view.width, view.height);
            else
                m_capture_ok = false;
        }
        break;
    }
    case AOStage::FilterFirstPass: {
        if (m_execution.ready && m_execution.plan.backend() == Backend::Fragment && m_execution.plan.fs().edge_filter) {
            std::vector<unsigned char> first;
            if (AOReadback::read_texture(first, view.gl_name, view.width, view.height))
                m_capture_ok &= AOCaptureWriter::write_gray_bmp((path / "fs-denoise-first.bmp").string(), first, view.width, view.height);
            else
                m_capture_ok = false;
        }
        break;
    }
    case AOStage::FilterThirdPass: {
        std::vector<unsigned char> second;
        if (AOReadback::read_texture(second, view.gl_name, view.width, view.height))
            m_capture_ok &= AOCaptureWriter::write_gray_bmp((path / "fs-denoise-second.bmp").string(), second, view.width, view.height);
        else
            m_capture_ok = false;
        break;
    }
    case AOStage::FilterRefined: {
        if (view.format == GL_RGBA8) {
            for (int channel = 0; channel < 4; ++channel) {
                std::vector<unsigned char> edges;
                if (AOReadback::read_texture(edges, view.gl_name, view.width, view.height, channel))
                    m_capture_ok &= AOCaptureWriter::write_gray_bmp((path / ("fs-edge-" + std::to_string(channel) + ".bmp")).string(),
                                                                    edges, view.width, view.height);
                else
                    m_capture_ok = false;
            }
        }
        break;
    }
    case AOStage::ComputePrefiltered: {
        for (int level = 0; level < view.mip_levels; ++level) {
            std::vector<float> mip;
            const int          w = view.width >> level, h = view.height >> level;
            if (AOReadback::read_compute_mip(mip, view.gl_name, level, w, h))
                m_capture_ok &= AOCaptureWriter::write_pfm((path / ("cs-depth-mip-" + std::to_string(level) + ".pfm")).string(), mip, w, h,
                                                           1);
            else
                m_capture_ok = false;
        }
        break;
    }
    case AOStage::ComputeEvaluated: {
        std::vector<unsigned char> data;
        if (AOReadback::read_compute_texture(data, view.gl_name, view.width, view.height, true)) {
            // First invocation carries the raw encoded texture, the second the packed edges.
            const std::string name = view.role == AOResourceRole::RawAO ? "cs-raw-encoded.pgm" : "cs-edges-packed.pgm";
            m_capture_ok &= AOCaptureWriter::write_pgm((path / name).string(), data, view.width, view.height);
            if (view.role == AOResourceRole::Edges) {
                for (int direction = 0; direction < 4; ++direction) {
                    auto channel = data;
                    for (auto& value : channel)
                        value = ((value >> (6 - direction * 2)) & 3) * 85;
                    m_capture_ok &= AOCaptureWriter::write_pgm((path / ("cs-edge-" + std::to_string(direction) + ".pgm")).string(), channel,
                                                               view.width, view.height);
                }
            }
        } else
            m_capture_ok = false;
        break;
    }
    case AOStage::ComputeDenoiseFirst:
    case AOStage::ComputeDenoiseSecond:
    case AOStage::ComputeDenoiseThird: {
        std::vector<unsigned char> data;
        if (view.ao_encoding == AOEncoding::R8Visibility) {
            // Final visibility output (2- or 3-pass chain).
            if (AOReadback::read_compute_texture(data, view.gl_name, view.width, view.height, false))
                m_capture_ok &= AOCaptureWriter::write_pgm((path / "cs-final.pgm").string(), data, view.width, view.height);
            else
                m_capture_ok = false;
        } else if (AOReadback::read_compute_texture(data, view.gl_name, view.width, view.height, true))
            m_capture_ok &= AOCaptureWriter::write_pgm((path / (stage == AOStage::ComputeDenoiseFirst ? "cs-denoise-first-encoded.pgm" :
                                                                                                        "cs-denoise-second-encoded.pgm"))
                                                           .string(),
                                                       data, view.width, view.height);
        else
            m_capture_ok = false;
        break;
    }
    case AOStage::FilterChainReady: {
        write_capture_parameters(); // May have changed after an execution-time fallback.
        write_backend_info();
        if (m_execution.ready && m_execution.plan.backend() == Backend::Compute) {
            write_compute_info();
        } else {
            std::vector<unsigned char> filtered;
            if (AOReadback::read_texture(filtered, view.gl_name, view.width, view.height))
                m_capture_ok &= AOCaptureWriter::write_gray_bmp((path / "ao-filtered.bmp").string(), filtered, view.width, view.height);
            else
                m_capture_ok = false;
        }
        break;
    }
    case AOStage::UpsampleReady: {
        std::vector<unsigned char> final_ao;
        if (AOReadback::read_texture(final_ao, view.gl_name, view.width, view.height))
            m_capture_ok &= AOCaptureWriter::write_gray_bmp((path / "ao-final-full.bmp").string(), final_ao, view.width, view.height);
        else
            m_capture_ok = false;
        break;
    }
    default: break;
    }
}

void AODiagnosticsController::write_capture_parameters()
{
    if (!m_execution.ready)
        return;
    const auto&        plan  = m_execution.plan;
    const auto&        input = m_input;
    const int          ao_w  = plan.backend() == Backend::Compute ? plan.cs().width : plan.fs().eval_width;
    const int          ao_h  = plan.backend() == Backend::Compute ? plan.cs().height : plan.fs().eval_height;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(17) << "ORCA_AO_CAPTURE 1\n";
    out << "size " << input.viewport[2] << ' ' << input.viewport[3] << ' ' << ao_w << ' ' << ao_h << '\n';
    out << "viewport " << input.viewport[0] << ' ' << input.viewport[1] << '\n';
    out << "perspective " << input.perspective << '\n';
    out << "quality " << int(m_settings.quality) << '\n';
    out << "settings " << m_settings.radius << ' ' << m_settings.thickness << ' ' << m_settings.intensity << ' ' << m_settings.strength
        << '\n';
    const auto samples = plan.backend() == Backend::Compute ?
                             cs_ao_samples(m_settings.quality, m_request.cs_slices_override, m_request.cs_legacy_high_sampling) :
                             fs_ao_samples(m_settings.quality, m_request.fs_slices_override);
    out << "samples " << samples[0] << ' ' << samples[1] << '\n';
    // Explicit column-major order; PFM rows are bottom-to-top, matching OpenGL.
    out << "projection";
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            out << ' ' << input.projection(r, c);
    out << "\ninverse_projection";
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            out << ' ' << input.inverse_projection(r, c);
    const int source_samples = m_execution.source_samples;
    out << "\nsource_samples " << source_samples << "\nsample_composite " << (m_execution.sample_route && m_settings.debug_view == 0)
        << '\n';
    out << "scene_view " << m_diagnostics.scene_view << "\nexplosion_ratio " << m_diagnostics.explosion_ratio << '\n';
    out << "frame_target " << frame_target_label(input.target_kind) << "\ncapture_samples " << m_diagnostics.capture_samples
        << "\ncapture_completion " << (m_diagnostics.defer_capture_completion ? "after_frame_presentation" : "after_ao") << '\n';
    m_capture_ok &= AOCaptureWriter::write_text((std::filesystem::path(m_capture_path) / "capture-info.txt").string(), out.str());
}

void AODiagnosticsController::write_backend_info()
{
    if (!m_execution.ready)
        return;
    const auto&        plan = m_execution.plan;
    std::ostringstream out;
    out << "actual_backend=" << (plan.backend() == Backend::Compute ? "cs" : "fs") << "\nfallback_reason=" << m_execution.fallback_reason
        << "\nupstream=" << AO_UPSTREAM_VERSION << "\nframe_target=" << frame_target_label(m_input.target_kind)
        << "\ncolor_sample_capture=" << m_diagnostics.capture_samples << "\nscene_view=" << m_diagnostics.scene_view
        << "\nexplosion_ratio=" << m_diagnostics.explosion_ratio
        << "\ncs_evaluate_program=" << (plan.backend() == Backend::Compute ? m_execution.cs_evaluate_program : 0)
        << "\nsample_capture_version=3\nsample_surface_diagnostics=" << (m_execution.sample_route ? "available" : "not_applicable")
        << "\ncolor_sample_count=" << m_execution.color_sample_count
        << "\ndenoise_passes=" << (plan.backend() == Backend::Compute ? plan.cs().denoise_passes : 0)
        << "\nactual_slices=" << (plan.backend() == Backend::Compute ? plan.cs().slices : plan.fs().slices)
        << "\nactual_steps=" << (plan.backend() == Backend::Compute ? plan.cs().steps : plan.fs().steps) << "\ncomposite_variant="
        << (m_execution.pixel_route                        ? "pixel" :
            m_execution.split_route                        ? "split_msaa" :
            plan.composite == CompositeVariant::Confidence ? "confidence_precomputed" :
            plan.composite == CompositeVariant::Reference  ? "reference" :
                                                             "optimized")
        << "\ncs_low_normalization="
        << (plan.backend() == Backend::Compute && m_settings.quality == AOSettings::Quality::Low ?
                (plan.cs().legacy_low ? "legacy" : "unoccluded_slice") :
                "not_applicable")
        << "\nfs_reconstruction="
        << (plan.backend() == Backend::Compute ? "not_applicable" :
            plan.fs().fast_reconstruction      ? "separable_xy" :
                                                 "matrix")
        << "\nfs_filter="
        << (plan.backend() == Backend::Compute ? "not_applicable" :
            plan.fs().edge_filter              ? (plan.fs().edge_passes == 3 ? "connectivity_3pass" : "connectivity_2pass") :
                                                 "geometry")
        << '\n';
    m_capture_ok &= AOCaptureWriter::write_text((std::filesystem::path(m_capture_path) / "backend-info.txt").string(), out.str());
}

void AODiagnosticsController::write_compute_info()
{
    if (!m_execution.ready)
        return;
    const auto&        plan = m_execution.plan;
    std::ostringstream meta;
    meta << "backend=cs\nupstream=" << AO_UPSTREAM_VERSION << "\nprecision=fp32\npadded_size=" << m_execution.cs_padded_width << 'x'
         << m_execution.cs_padded_height
         << "\nedge_order=left,right,top(+Y),bottom(-Y)\nencoded_visibility_scale=1.5\ndenoise_passes=" << plan.cs().denoise_passes
         << "\nworking_texture_bytes="
         << (size_t(m_execution.cs_padded_width) * m_execution.cs_padded_height * 4 * 341 / 256 +
             size_t(m_input.viewport[2]) * m_input.viewport[3] * (plan.cs().denoise_passes == 3 ? 5 : 4))
         << '\n';
    m_capture_ok &= AOCaptureWriter::write_text((std::filesystem::path(m_capture_path) / "cs-info.txt").string(), meta.str());
}

void AODiagnosticsController::capture_precomposite_products()
{
    if (m_capture_path.empty() || !m_execution.ready || context_lost())
        return;
    const std::filesystem::path path(m_capture_path);
    std::vector<unsigned char>  color;
    if (AOReadback::read_color(color, m_input.target, m_input.viewport))
        m_capture_ok &= AOCaptureWriter::write_gray_bmp((path / "color-before-ao.bmp").string(), color, m_input.viewport[2],
                                                        m_input.viewport[3], true);
    else
        m_capture_ok = false;
    if (m_diagnostics.capture_samples && m_execution.color_sample_count > 1) {
        AOReadback::SampleColorReader reader(m_input.target, m_input.viewport, m_execution.color_sample_count, m_execution.vao);
        if (reader.valid()) {
            for (int sample = 0; sample < m_execution.color_sample_count; ++sample) {
                std::vector<float> pixels;
                if (reader.read(sample, pixels))
                    m_capture_ok &= AOCaptureWriter::write_pfm((path / ("sample-" + std::to_string(sample) + "-color-before.pfm")).string(),
                                                               pixels, m_input.viewport[2], m_input.viewport[3], 3);
                else
                    m_capture_ok = false;
            }
            std::ostringstream metadata;
            metadata << "format RGBA8\nframebuffer_srgb " << int(reader.srgb_framebuffer()) << "\nsource_color_encoding "
                     << (reader.linear_source() ? "linear" : "srgb") << '\n';
            m_capture_ok &= AOCaptureWriter::write_text((path / "sample-color-before-info.txt").string(), metadata.str());
        } else
            m_capture_ok = false;
    }
}

void AODiagnosticsController::capture_postcomposite_products()
{
    if (m_capture_path.empty() || !m_execution.ready || context_lost())
        return;
    AOGLStateGuard saved;
    saved.fullscreen();
    const std::filesystem::path path(m_capture_path);
    std::vector<unsigned char>  color;
    if (AOReadback::read_color(color, m_input.target, m_input.viewport))
        m_capture_ok &= AOCaptureWriter::write_gray_bmp((path / "color-after-ao.bmp").string(), color, m_input.viewport[2],
                                                        m_input.viewport[3], true);
    else
        m_capture_ok = false;
    if (context_lost()) {
        saved.forget_context();
        m_capture.cancel();
        return;
    }
    if (m_execution.split_route && m_execution.split_texture) {
        std::vector<unsigned char> factor;
        if (AOReadback::read_texture(factor, m_execution.split_texture, m_input.viewport[2], m_input.viewport[3]))
            m_capture_ok &= AOCaptureWriter::write_gray_bmp((path / "composite-fast-factor.bmp").string(), factor, m_input.viewport[2],
                                                            m_input.viewport[3]);
        else
            m_capture_ok = false;
    }
    if (m_diagnostics.capture_samples && m_execution.color_sample_count > 1) {
        AOReadback::SampleColorReader reader(m_input.target, m_input.viewport, m_execution.color_sample_count, m_execution.vao);
        if (reader.valid()) {
            for (int sample = 0; sample < m_execution.color_sample_count; ++sample) {
                std::vector<float> pixels;
                if (reader.read(sample, pixels))
                    m_capture_ok &= AOCaptureWriter::write_pfm((path / ("sample-" + std::to_string(sample) + "-color-after.pfm")).string(),
                                                               pixels, m_input.viewport[2], m_input.viewport[3], 3);
                else
                    m_capture_ok = false;
            }
            std::ostringstream metadata;
            metadata << "format RGBA8\nframebuffer_srgb " << int(reader.srgb_framebuffer()) << "\nsource_color_encoding "
                     << (reader.linear_source() ? "linear" : "srgb") << '\n';
            m_capture_ok &= AOCaptureWriter::write_text((path / "sample-color-after-info.txt").string(), metadata.str());
        } else
            m_capture_ok = false;
    }
    if (context_lost()) {
        saved.forget_context();
        m_capture.cancel();
        return;
    }
    // Composite-confidence diagnostic: one R8 pass of the composite in debug view 4.
    {
        GLuint fbo = 0, rbo = 0;
        GLint  old_rbo = 0;
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_rbo);
        glGenFramebuffers(1, &fbo);
        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_R8, m_input.viewport[2], m_input.viewport[3]);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            draw_composite_diagnostic(m_execution.composite_program, 4, fbo, m_input.viewport[2], m_input.viewport[3]);
            std::vector<unsigned char> pixels;
            if (AOReadback::read_composite_bytes(pixels, fbo, m_input.viewport[2], m_input.viewport[3]))
                m_capture_ok &= AOCaptureWriter::write_gray_bmp((path / "composite-confidence.bmp").string(), pixels, m_input.viewport[2],
                                                                m_input.viewport[3]);
            else
                m_capture_ok = false;
        } else
            m_capture_ok = false;
        if (context_lost()) {
            saved.forget_context();
            m_capture.cancel();
            return;
        }
        glBindRenderbuffer(GL_RENDERBUFFER, old_rbo);
        glDeleteRenderbuffers(1, &rbo);
        glDeleteFramebuffers(1, &fbo);
    }
    if (m_execution.sample_route)
        m_capture_ok &= write_sample_diagnostics(m_execution.vao, m_execution.sample_program, m_execution.sample_count);
    if (context_lost()) {
        saved.forget_context();
        m_capture.cancel();
        return;
    }
    m_capture.products_finished(m_capture_path, m_diagnostics.defer_capture_completion, m_capture_ok && m_execution.rendered,
                                m_input.viewport[2], m_input.viewport[3]);
}

bool AODiagnosticsController::write_sample_diagnostics(unsigned int vao, GLShaderProgram* shader, int samples)
{
    if (!m_execution.ready || !shader || samples <= 0)
        return false;
    // This is deliberately inside the one-shot capture only. No extra targets, sample reads
    // or disk writes occur on ordinary frames.
    const std::filesystem::path path(m_capture_path);
    const int                   width = m_input.viewport[2], height = m_input.viewport[3];
    bool                        ok  = true;
    GLuint                      fbo = 0, rbo = 0;
    GLint                       old_rbo = 0, old_pack = 0, old_buffer = 0, old_row = 0, old_rows = 0, old_pixels = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_rbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_buffer);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &old_row);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &old_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &old_pixels);
    glGenFramebuffers(1, &fbo);
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA32F, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindRenderbuffer(GL_RENDERBUFFER, old_rbo);
        glDeleteRenderbuffers(1, &rbo);
        glDeleteFramebuffers(1, &fbo);
        return false;
    }
    {
        std::ofstream positions(path / "sample-positions.txt");
        positions.imbue(std::locale::classic());
        positions << "ORCA_AO_SAMPLES 1\ncount " << samples << '\n';
        std::ofstream target_positions(path / "sample-target-positions.txt");
        target_positions.imbue(std::locale::classic());
        target_positions << "ORCA_AO_SAMPLES 1\ncount " << samples << '\n';
        std::vector<float> rgba(size_t(width) * height * 4);
        std::vector<float> depth(size_t(width) * height), match(depth.size() * 3);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        for (int sample = 0; sample < samples; ++sample) {
            if (context_lost())
                return false;
            std::array<float, 2> position{};
            glBindFramebuffer(GL_FRAMEBUFFER, m_execution.sample_fbo);
            glGetMultisamplefv(GL_SAMPLE_POSITION, sample, position.data());
            positions << sample << ' ' << position[0] << ' ' << position[1] << '\n';
            // gl_SamplePosition in normal presentation belongs to the actual draw target. Do
            // not assume the depth-copy FBO uses its pattern.
            glBindFramebuffer(GL_FRAMEBUFFER, m_input.target);
            glGetMultisamplefv(GL_SAMPLE_POSITION, sample, position.data());
            target_positions << sample << ' ' << position[0] << ' ' << position[1] << '\n';
            shader->start_using();
            shader->set_uniform("capture_sample_index", sample);
            shader->set_uniform("capture_sample_position", position);
            draw_composite_diagnostic(shader, 5, fbo, width, height);
            std::vector<float> pixels;
            if (AOReadback::read_composite_floats(pixels, fbo, width, height)) {
                for (size_t i = 0; i < depth.size(); ++i) {
                    depth[i] = pixels[4 * i];
                    for (int c = 0; c < 3; ++c)
                        match[3 * i + c] = pixels[4 * i + c + 1];
                }
                ok &= AOCaptureWriter::write_pfm((path / ("sample-" + std::to_string(sample) + "-depth.pfm")).string(), depth, width,
                                                 height, 1);
                ok &= AOCaptureWriter::write_pfm((path / ("sample-" + std::to_string(sample) + "-match.pfm")).string(), match, width,
                                                 height, 3);
            } else
                ok = false;
            draw_composite_diagnostic(shader, 6, fbo, width, height);
            if (AOReadback::read_composite_floats(pixels, fbo, width, height)) {
                for (size_t i = 0; i < depth.size(); ++i)
                    for (int c = 0; c < 3; ++c)
                        match[3 * i + c] = pixels[4 * i + c];
                ok &= AOCaptureWriter::write_pfm((path / ("sample-" + std::to_string(sample) + "-factor.pfm")).string(), match, width,
                                                 height, 3);
            } else
                ok = false;
        }
        ok &= bool(positions) && bool(target_positions);
        if (context_lost())
            return false;
        shader->set_uniform("capture_sample", false);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, old_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, old_pack);
    glPixelStorei(GL_PACK_ROW_LENGTH, old_row);
    glPixelStorei(GL_PACK_SKIP_ROWS, old_rows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, old_pixels);
    glBindRenderbuffer(GL_RENDERBUFFER, old_rbo);
    glDeleteRenderbuffers(1, &rbo);
    glDeleteFramebuffers(1, &fbo);
    return ok;
}

bool AODiagnosticsController::start_comparison(const std::string& root, const std::string& mode)
{
    m_capture.cancel();
    if (!m_runner.start(root, AOExperimentRunner::parse_mode(mode)))
        return false;
    return benchmark_request((std::filesystem::path(root) / m_runner.run_name(0)).string());
}

void AODiagnosticsController::advance_comparison(const AOResult& result)
{
    if (m_runner.stage() == 1 && !benchmark_active()) {
        if (!benchmark_completed()) {
            m_runner.finish("ABORTED: benchmark failed; see run status");
            return;
        }
        m_comparison_capture = (std::filesystem::path(benchmark_directory()) / "capture").string();
        request_capture(m_comparison_capture);
        m_runner.enter_capture(m_comparison_capture);
        return;
    }
    if (m_runner.stage() != 2)
        return;
    std::error_code error;
    const bool captured = std::filesystem::exists(std::filesystem::path(m_comparison_capture) / "capture-complete.txt", error) && !error;
    if (!m_runner.validate_actual(result) || !captured) {
        m_runner.finish("ABORTED: image capture failed or requested backend unavailable");
        return;
    }
    if (!m_runner.advance(6)) {
        m_runner.finish("COMPLETE");
        return;
    }
    if (!benchmark_request((std::filesystem::path(m_runner.root()) / m_runner.run_name(m_runner.run())).string()))
        m_runner.finish("ABORTED: cannot start benchmark");
}

bool AODiagnosticsController::finalize_capture(const AOFrameInput& presented_input, bool success)
{
    const std::string directory = m_capture.finalize_directory();
    return m_capture.finalize(success, presented_input.viewport[2], presented_input.viewport[3], [&]() {
        std::vector<unsigned char> color;
        if (!AOReadback::read_color(color, presented_input.target, presented_input.viewport) || AOReadback::consume_errors())
            return false;
        if (!AOCaptureWriter::write_gray_bmp((std::filesystem::path(directory) / "color-presented.bmp").string(), color,
                                             presented_input.viewport[2], presented_input.viewport[3], true))
            return false;
        std::ostringstream info;
        info << "stage=after_imgui_before_swap\nframe_target=" << frame_target_label(presented_input.target_kind) << "\n";
        if (!AOCaptureWriter::write_text((std::filesystem::path(directory) / "presentation-info.txt").string(), info.str()))
            return false;
        return true;
    });
}

std::string AODiagnosticsController::panel_backend_label(const AOResult& result) const
{
    return result.actual.executed && result.actual.backend == Backend::Compute ? "cs" : "fs";
}

std::string AODiagnosticsController::pipeline_description() const
{
    std::ostringstream description;
    description << "actual_backend=" << (m_execution.plan.backend() == Backend::Compute ? "cs" : "fs")
                << "\nfallback_reason=" << m_execution.fallback_reason << "\nframe_target=" << frame_target_label(m_input.target_kind)
                << "\npresentation_stage=after_imgui_before_swap\n"
                << "ao_size="
                << (m_execution.plan.backend() == Backend::Compute ? m_execution.plan.cs().width : m_execution.plan.fs().eval_width) << 'x'
                << (m_execution.plan.backend() == Backend::Compute ? m_execution.plan.cs().height : m_execution.plan.fs().eval_height)
                << "\ncs_denoise_actual=" << (m_execution.plan.backend() == Backend::Compute ? m_execution.plan.cs().denoise_passes : 0)
                << "\ncomposite_variant="
                << (m_execution.pixel_route                                    ? "pixel" :
                    m_execution.split_route                                    ? "split_msaa" :
                    m_execution.plan.composite == CompositeVariant::Confidence ? "confidence_precomputed" :
                    m_execution.plan.composite == CompositeVariant::Reference  ? "reference" :
                                                                                 "optimized")
                << "\ncs_low_normalization="
                << (m_execution.plan.backend() == Backend::Compute && m_settings.quality == AOSettings::Quality::Low ?
                        (m_execution.plan.cs().legacy_low ? "legacy" : "unoccluded_slice") :
                        "not_applicable")
                << "\nfs_reconstruction="
                << (m_execution.plan.backend() == Backend::Compute ? "not_applicable" :
                    m_execution.plan.fs().fast_reconstruction      ? "separable_xy" :
                                                                     "matrix")
                << "\nfs_filter="
                << (m_execution.plan.backend() == Backend::Compute ? "not_applicable" :
                    m_execution.plan.fs().edge_filter              ? (m_execution.plan.fs().edge_passes == 3 ? "connectivity_3pass" :
                                                                                                               "connectivity_2pass") :
                                                                     "geometry")
                << "\nactual_slices="
                << (m_execution.plan.backend() == Backend::Compute ? m_execution.plan.cs().slices : m_execution.plan.fs().slices)
                << "\nactual_steps="
                << (m_execution.plan.backend() == Backend::Compute ? m_execution.plan.cs().steps : m_execution.plan.fs().steps)
                << "\ncs_evaluate_program=" << (m_execution.plan.backend() == Backend::Compute ? m_execution.cs_evaluate_program : 0)
                << "\ncs_implementation=xegtao_glsl_v1\ncs_upstream=" << AO_UPSTREAM_VERSION;
    return description.str();
}

void AODiagnosticsController::draw_composite_diagnostic(GLShaderProgram* shader, int debug_view, unsigned int target, int width, int height)
{
    if (!shader)
        return;
    AOUniformValues uniforms = m_execution.uniforms;
    uniforms.debug_view      = debug_view;
    uniforms.capture_sample  = debug_view == 5 || debug_view == 6;
    ao_draw_fullscreen(*shader, target, width, height, m_execution.vao, uniforms, false, false, std::array<int, 2>{{0, 0}});
}

void AODiagnosticsController::release()
{
    m_profiler.release();
    m_capture.forget();
    m_capture_path.clear();
}

void AODiagnosticsController::forget()
{
    m_profiler.forget();
    m_capture.forget();
    m_capture_path.clear();
}

}}} // namespace Slic3r::GUI::AO
