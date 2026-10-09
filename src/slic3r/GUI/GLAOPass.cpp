#include "GLAOPass.hpp"
#include "GLShader.hpp"

#include <GL/glew.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <vector>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {
bool AOBenchmark::request(const std::string& root)
{
    if (m_active)
        return false;
    const auto id = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    m_directory = (std::filesystem::path(root) / ("run-" + std::to_string(id))).string();
    std::error_code error;
    if (!std::filesystem::create_directories(m_directory, error) || error)
        return false;
    std::ofstream status(std::filesystem::path(m_directory) / "status.txt");
    status << "Running: 30 warmup + 180 measured frames. Keep camera, settings and scene fixed.\n";
    status.close();
    if (!status)
        return false;
    m_completed = false;
    m_rows.clear();
    m_rows.reserve(210);
    m_signature.clear();
    m_pipeline.clear();
    m_submitted = m_skipped = 0;
    m_slot = -1;
    m_started = std::chrono::steady_clock::now();
    m_active = true;
    BOOST_LOG_TRIVIAL(info) << "AO benchmark started: " << m_directory;
    return true;
}

void AOBenchmark::begin(const std::string& signature)
{
    if (!m_active)
        return;
    if (!(GLEW_VERSION_3_3 || GLEW_ARB_timer_query)) {
        finish("ABORTED: GPU timestamp queries unavailable");
        return;
    }
    if (std::chrono::steady_clock::now() - m_started > std::chrono::seconds(120)) {
        finish("ABORTED: 120 second timeout");
        return;
    }
    if (m_signature.empty()) {
        m_signature = signature;
        std::ofstream metadata(std::filesystem::path(m_directory) / "metadata.txt");
        metadata << "benchmark_schema=9\n"
                 << signature << "\nGPU: " << reinterpret_cast<const char*>(glGetString(GL_RENDERER))
                 << "\nVendor: " << reinterpret_cast<const char*>(glGetString(GL_VENDOR))
                 << "\nGL: " << reinterpret_cast<const char*>(glGetString(GL_VERSION))
                 << "\nScope: GPU frame after camera setup, before SwapBuffers; CPU same submission region.\n"
                 << "No image capture. Scene cache refresh forced. No VSync changes.\n"
                 << "CPU stage stamps precede existing GPU query submissions; wall time includes driver calls and scheduling.\n"
                 << "CPU and GPU intervals overlap: do not add them. elapsed_ms is CPU frame-start time since benchmark request.\n";
    } else if (signature != m_signature) {
        finish("ABORTED: camera, viewport or AO settings changed");
        return;
    }
    for (auto& slot : m_slots) {
        if (!slot.pending)
            continue;
        GLint ready = GL_FALSE;
        glGetQueryObjectiv(slot.queries[FRAME_END], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready)
            continue;
        Row row{slot.frame, slot.mask, {}, slot.cpu_ms, slot.elapsed_ms, slot.cpu_stamps};
        GLuint64 origin = 0;
        glGetQueryObjectui64v(slot.queries[0], GL_QUERY_RESULT, &origin);
        for (int i = 0; i < QUERY_COUNT; ++i) {
            if ((slot.mask & (1u << i)) == 0)
                continue;
            GLuint64 stamp = 0;
            glGetQueryObjectui64v(slot.queries[i], GL_QUERY_RESULT, &stamp);
            row.stamps[i] = double(stamp - origin) / 1e6;
        }
        m_rows.push_back(row);
        slot.pending = false;
    }
    if (m_rows.size() == 210) {
        finish("COMPLETE");
        return;
    }
    m_slot = -1;
    if (m_submitted >= 210)
        return; // Continue rendering only to drain pending queries without waiting.
    for (int i = 0; i < int(m_slots.size()); ++i) {
        auto& slot = m_slots[i];
        if (slot.pending)
            continue;
        if (!slot.queries[0])
            glGenQueries(QUERY_COUNT, slot.queries.data());
        slot.mask = 0;
        slot.frame = m_submitted++;
        m_slot = i;
        m_cpu_start = std::chrono::steady_clock::now();
        slot.elapsed_ms = std::chrono::duration<double, std::milli>(m_cpu_start - m_started).count();
        mark(0);
        return;
    }
    ++m_skipped;
}

void AOBenchmark::pipeline(const std::string& description)
{
    if (!m_active)
        return;
    if (m_pipeline.empty()) {
        m_pipeline = description;
        std::ofstream out(std::filesystem::path(m_directory) / "metadata.txt", std::ios::app);
        out << description << '\n';
    } else if (m_pipeline != description)
        finish("ABORTED: actual AO pipeline changed");
}

void AOBenchmark::mark(int index)
{
    if (!m_active || m_slot < 0)
        return;
    auto& slot = m_slots[m_slot];
    slot.cpu_stamps[index] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_cpu_start).count();
    glQueryCounter(slot.queries[index], GL_TIMESTAMP);
    slot.mask |= 1u << index;
}

void AOBenchmark::end()
{
    if (!m_active || m_slot < 0)
        return;
    mark(FRAME_END);
    auto& slot = m_slots[m_slot];
    slot.cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_cpu_start).count();
    slot.pending = true;
    m_slot = -1;
}

void AOBenchmark::finish(const std::string& reason)
{
    if (!m_active)
        return;
    m_active = false;
    m_slot = -1;
    std::sort(m_rows.begin(), m_rows.end(), [](const Row& a, const Row& b) { return a.frame < b.frame; });
    std::ofstream csv(std::filesystem::path(m_directory) / "frames.csv");
    csv.imbue(std::locale::classic());
    csv << "frame,warmup,cpu_submit_ms,gpu_frame_ms,main_scene_ms,ao_total_ms,depth_copy_ms,receivers_ms,normals_ms,gtao_ms,filter_ms,"
           "upsample_ms,composite_ms,color_present_ms,depth_return_ms,filter_first_ms,filter_refine_ms,color_resolve_ms,window_present_ms,"
           "before_main_ms,after_main_ms"
        << ",elapsed_ms,cpu_ao_submit_ms,cpu_gtao_submit_ms,cpu_filter_first_submit_ms,cpu_filter_refine_submit_ms,cpu_depth_copy_submit_"
           "ms,cpu_normals_submit_ms"
        << ",cs_depth_prefilter_ms,cs_evaluate_ms,cs_denoise_first_ms,cs_denoise_second_ms,cs_denoise_third_ms,depth_resolve_ms,sample_depth_copy_ms,fs_denoise_second_ms,fs_denoise_third_ms,composite_classify_ms,composite_fast_ms,composite_edges_ms,receiver_first_ms,receiver_sample_ms\n";
    for (const auto& row : m_rows) {
        csv << row.frame << ',' << (row.frame < 30 ? 1 : 0) << ',' << std::setprecision(9) << row.cpu_ms;
        auto interval = [&](int a, int b) {
            csv << ',';
            if ((row.mask & (1u << a)) && (row.mask & (1u << b)))
                csv << row.stamps[b] - row.stamps[a];
        };
        interval(0, 15);
        interval(1, 14);
        interval(2, 10);
        for (int i = 2; i < 9; ++i)
            if ((row.mask & (1u << 19)) && (i == 5 || i == 6))
                csv << ',';
            else
                interval(i, i + 1);
        interval(11, 12);
        interval(12, 13);
        interval(6, 16);
        interval(16, 17); // Empty for half-resolution AO, which does not run refinement.
        interval(11, 18);
        interval(18, 12);
        interval(0, 1);
        interval(14, FRAME_END);
        csv << ',' << row.elapsed_ms;
        auto cpu_interval = [&](int a, int b) {
            csv << ',';
            if ((row.mask & (1u << a)) && (row.mask & (1u << b)))
                csv << row.cpu_stamps[b] - row.cpu_stamps[a];
        };
        cpu_interval(2, 10);
        if (row.mask & (1u << 19))
            csv << ',';
        else
            cpu_interval(5, 6);
        cpu_interval(6, 16);
        cpu_interval(16, 17);
        cpu_interval(2, 3);
        cpu_interval(4, 5);
        interval(19, 20);
        interval(20, 21);
        interval(21, 22);
        interval(22, 23);
        interval(23, 24);
        interval(2, 25);
        interval(25, 29); // Empty when no sample-depth copy was executed.
        interval(16, (row.mask & (1u << 26)) ? 26 : 17);
        interval(26, 17);
        interval(8, 27);
        interval(27, 28);
        interval(28, 9);
        interval(3, 30);
        if (row.mask & (1u << 29)) interval(30, 4); else csv << ',';
        csv << '\n';
    }
    csv.close();
    std::ofstream status(std::filesystem::path(m_directory) / "status.txt");
    status << (csv ? reason : "ABORTED: CSV write failed") << "\nrows=" << m_rows.size()
           << "\nquery_ring_skipped_frames=" << m_skipped << '\n';
    BOOST_LOG_TRIVIAL(info) << "AO benchmark " << reason << ": " << m_directory;
    status.close();
    m_completed = reason == "COMPLETE" && bool(csv) && bool(status);
    release();
}

void AOBenchmark::release()
{
    for (auto& slot : m_slots)
        if (slot.queries[0])
            glDeleteQueries(QUERY_COUNT, slot.queries.data());
    forget();
}

void AOBenchmark::forget()
{
    m_slots = {};
    m_active = false;
    m_slot = -1;
}

namespace {
std::array<int, 2> ao_samples(GLAOPass::Quality quality, int override_slices)
{
    return quality == GLAOPass::Quality::Low    ? std::array<int, 2>{{2, 3}} :
           quality == GLAOPass::Quality::Medium ? std::array<int, 2>{{3, 4}} :
                                                  std::array<int, 2>{{override_slices == 3 ? 3 : 4, 8}};
}

// Static editor views have no temporal accumulation. High spends its GPU headroom on
// more angular samples; lower presets retain the upstream sampling budget.
std::array<int, 2> cs_ao_samples(GLAOPass::Quality quality, int override_slices)
{
    if (quality == GLAOPass::Quality::Low)
        return {{1, 2}};
    if (quality == GLAOPass::Quality::Medium)
        return {{2, 2}};
    if (override_slices == 3 || override_slices == 6)
        return {{override_slices, 3}};
    const char* sampling = std::getenv("ORCA_AO_CS_SAMPLING");
    // Preserve the explicit diagnostic baseline; an unset request uses static High.
    const bool legacy = sampling && (std::string(sampling) == "default" || std::string(sampling) == "legacy");
    return {{legacy ? 3 : 6, 3}};
}

// The pass uses its own VAO and texture units 0..4 (3: MSAA depth, 4: optional confidence).
struct AOState
{
    GLint     read_fbo{}, draw_fbo{}, viewport[4]{}, program{}, vao{}, array_buffer{}, active{}, textures[5]{}, sample_texture{};
    GLint     depth_func{}, blend_src[2]{}, blend_dst[2]{}, blend_eq[2]{}, polygon_mode[2]{};
    GLboolean depth_mask{}, color_mask[4]{}, multisample{}, sample_mask{}, sample_shading{};
    const std::array<GLenum, 10> caps{{GL_DEPTH_TEST, GL_BLEND, GL_STENCIL_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_SAMPLE_ALPHA_TO_COVERAGE,
                                       GL_SAMPLE_COVERAGE, GL_COLOR_LOGIC_OP, GL_POLYGON_OFFSET_FILL, GL_RASTERIZER_DISCARD}};
    std::array<GLboolean, 10>    enabled{};
    AOState()
    {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        for (int i = 0; i < 5; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures[i]);
        }
        glActiveTexture(GL_TEXTURE3);
        if (GLEW_VERSION_4_0)
            glGetIntegerv(GL_TEXTURE_BINDING_2D_MULTISAMPLE, &sample_texture);
        glActiveTexture(active);
        glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
        glGetIntegerv(GL_POLYGON_MODE, polygon_mode);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
        multisample = glIsEnabled(GL_MULTISAMPLE);
        if (GLEW_VERSION_4_0) {
            sample_mask = glIsEnabled(GL_SAMPLE_MASK);
            sample_shading = glIsEnabled(GL_SAMPLE_SHADING);
        }
        glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src[0]);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src[1]);
        glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst[0]);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst[1]);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_eq[0]);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_eq[1]);
        for (size_t i = 0; i < caps.size(); ++i)
            enabled[i] = glIsEnabled(caps[i]);
    }
    void fullscreen() const
    {
        for (GLenum cap : caps)
            glDisable(cap);
        if (GLEW_VERSION_4_0) {
            glDisable(GL_SAMPLE_MASK);
            glDisable(GL_SAMPLE_SHADING);
        }
        glDepthMask(GL_FALSE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    ~AOState()
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glUseProgram(program);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, array_buffer);
        for (int i = 0; i < 5; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, textures[i]);
        }
        glActiveTexture(GL_TEXTURE3);
        if (GLEW_VERSION_4_0)
            glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, sample_texture);
        glActiveTexture(active);
        glDepthFunc(depth_func);
        if (polygon_mode[0] == polygon_mode[1])
            glPolygonMode(GL_FRONT_AND_BACK, polygon_mode[0]);
        else {
            glPolygonMode(GL_FRONT, polygon_mode[0]);
            glPolygonMode(GL_BACK, polygon_mode[1]);
        }
        glDepthMask(depth_mask);
        glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
        glBlendFuncSeparate(blend_src[0], blend_dst[0], blend_src[1], blend_dst[1]);
        glBlendEquationSeparate(blend_eq[0], blend_eq[1]);
        if (multisample)
            glEnable(GL_MULTISAMPLE);
        else
            glDisable(GL_MULTISAMPLE);
        if (GLEW_VERSION_4_0) {
            if (sample_shading) glEnable(GL_SAMPLE_SHADING); else glDisable(GL_SAMPLE_SHADING);
            if (sample_mask)
                glEnable(GL_SAMPLE_MASK);
            else
                glDisable(GL_SAMPLE_MASK);
        }
        for (size_t i = 0; i < caps.size(); ++i) {
            if (enabled[i])
                glEnable(caps[i]);
            else
                glDisable(caps[i]);
        }
    }
};

void texture_parameters()
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
bool consume_errors()
{
    bool error = false;
    while (glGetError() != GL_NO_ERROR)
        error = true;
    return error;
}

// Optional, local-only capture for diagnosing AO on a real scene. The normal
// rendering path does not read any textures back from the GPU.
bool write_gray_bmp(const std::filesystem::path& path, const std::vector<unsigned char>& pixels, int w, int h, bool rgb = false)
{
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    const unsigned int stride = (unsigned int(w * 3) + 3u) & ~3u;
    const unsigned int size   = 54u + stride * unsigned int(h);
    auto               put16  = [&out](unsigned int v) {
        out.put(char(v));
        out.put(char(v >> 8));
    };
    auto put32 = [&put16](unsigned int v) {
        put16(v);
        put16(v >> 16);
    };
    out.write("BM", 2);
    put32(size);
    put32(0);
    put32(54);
    put32(40);
    put32(unsigned int(w));
    put32(unsigned int(h));
    put16(1);
    put16(24);
    put32(0);
    put32(stride * unsigned int(h));
    put32(0);
    put32(0);
    put32(0);
    put32(0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c)
                out.put(char(pixels[rgb ? (size_t(y) * w + x) * 3 + 2 - c : size_t(y) * w + x]));
        }
        for (unsigned int p = unsigned int(w * 3); p < stride; ++p)
            out.put(0);
    }
    return bool(out);
}

// Resolve the color target first: glReadPixels cannot read a multisample FBO.
// This captures the AO insertion point, before later UI/selection overlays.
bool capture_ao_color(const std::filesystem::path& path, unsigned int target, const std::array<int, 4>& viewport)
{
    AOState saved;
    saved.fullscreen();
    GLint old_rbo = 0, old_pack = 0, old_buffer = 0, old_row = 0, old_rows = 0, old_pixels = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_rbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_buffer);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &old_row);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &old_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &old_pixels);
    const GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    GLint color_buffer = 0, old_read_buffer = 0;
    glGetIntegerv(GL_DRAW_BUFFER0, &color_buffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
    glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
    glReadBuffer(color_buffer);
    GLuint fbo = 0, rbo = 0;
    glGenFramebuffers(1, &fbo);
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, viewport[2], viewport[3]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
    const bool complete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete)
        glBlitFramebuffer(viewport[0], viewport[1], viewport[0] + viewport[2], viewport[1] + viewport[3], 0, 0, viewport[2], viewport[3],
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glReadBuffer(old_read_buffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    std::vector<unsigned char> pixels(size_t(viewport[2]) * viewport[3] * 3);
    if (complete)
        glReadPixels(0, 0, viewport[2], viewport[3], GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    glBindBuffer(GL_PIXEL_PACK_BUFFER, old_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, old_pack);
    glPixelStorei(GL_PACK_ROW_LENGTH, old_row);
    glPixelStorei(GL_PACK_SKIP_ROWS, old_rows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, old_pixels);
    glBindRenderbuffer(GL_RENDERBUFFER, old_rbo);
    glDeleteRenderbuffers(1, &rbo);
    glDeleteFramebuffers(1, &fbo);
    if (srgb)
        glEnable(GL_FRAMEBUFFER_SRGB);
    return complete && write_gray_bmp(path, pixels, viewport[2], viewport[3], true);
}

// Read each covered color sample before resolve, only for explicitly requested captures.
bool capture_ao_sample_colors(const std::filesystem::path& path,
                              const char*                  stage,
                              unsigned int                 target,
                              const std::array<int, 4>&    viewport,
                              int                          samples,
                              unsigned int                 vao)
{
    AOState saved;
    saved.fullscreen();
    const GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_FRAMEBUFFER_SRGB);
    GLint old_pack = 0, old_buffer = 0, old_row = 0, old_rows = 0, old_pixels = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_buffer);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &old_row);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &old_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &old_pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    GLint color_buffer = 0, old_read = 0;
    glGetIntegerv(GL_DRAW_BUFFER0, &color_buffer);
    glGetIntegerv(GL_READ_BUFFER, &old_read);
    glReadBuffer(color_buffer);
    GLint  color_encoding = GL_LINEAR;
    GLenum attachment     = GLenum(color_buffer);
    if (target == 0) {
        if (attachment == GL_BACK)
            attachment = GL_BACK_LEFT;
        if (attachment == GL_FRONT)
            attachment = GL_FRONT_LEFT;
    }
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &color_encoding);
    GLuint fbo[2]{}, texture[2]{};
    glGenFramebuffers(2, fbo);
    glGenTextures(2, texture);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, texture[0]);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_RGBA8, viewport[2], viewport[3], GL_TRUE);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[0]);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, texture[0], 0);
    bool ok = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok)
        glBlitFramebuffer(viewport[0], viewport[1], viewport[0] + viewport[2], viewport[1] + viewport[3], 0, 0, viewport[2], viewport[3],
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glReadBuffer(old_read);
    glBindTexture(GL_TEXTURE_2D, texture[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, viewport[2], viewport[3], 0, GL_RGBA, GL_FLOAT, nullptr);
    texture_parameters();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture[1], 0);
    ok &= glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    GLShaderProgram                shader;
    GLShaderProgram::ShaderSources sources;
    sources[0] = R"(#version 140
        void main() {
            vec2 p=vec2((gl_VertexID<<1)&2, gl_VertexID&2);
            gl_Position=vec4(p*2.0-1.0,0,1);
        })";
    sources[1] = R"(#version 400
        uniform sampler2DMS colors;
        uniform int sample_index;
        out vec4 out_color;
        void main() { out_color=texelFetch(colors,ivec2(gl_FragCoord.xy),sample_index); }
        )";
    ok &= shader.init_from_texts("ao_capture_sample_color", sources);
    if (ok) {
        shader.start_using();
        shader.set_uniform("colors", 3);
        glBindVertexArray(vao);
        glViewport(0, 0, viewport[2], viewport[3]);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        std::vector<float> pixels(size_t(viewport[2]) * viewport[3] * 3);
        for (int sample = 0; sample < samples; ++sample) {
            shader.set_uniform("sample_index", sample);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glReadPixels(0, 0, viewport[2], viewport[3], GL_RGB, GL_FLOAT, pixels.data());
            std::ofstream file(path / ("sample-" + std::to_string(sample) + "-color-" + stage + ".pfm"), std::ios::binary);
            file << "PF\n" << viewport[2] << ' ' << viewport[3] << "\n-1.0\n";
            file.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size() * sizeof(float)));
            ok &= bool(file);
        }
        std::ofstream metadata(path / (std::string("sample-color-") + stage + "-info.txt"));
        metadata << "format RGBA8\nframebuffer_srgb " << int(srgb) << "\nsource_color_encoding "
                 << (color_encoding == GL_SRGB ? "srgb" : "linear") << '\n';
        ok &= bool(metadata);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, old_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, old_pack);
    glPixelStorei(GL_PACK_ROW_LENGTH, old_row);
    glPixelStorei(GL_PACK_SKIP_ROWS, old_rows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, old_pixels);
    glDeleteFramebuffers(2, fbo);
    glDeleteTextures(2, texture);
    if (srgb)
        glEnable(GL_FRAMEBUFFER_SRGB);
    const bool error = consume_errors();
    return ok && !error;
}

bool capture_ao_texture(const std::filesystem::path& path, unsigned int texture, int w, int h, int channel = -1)
{
    std::vector<unsigned char> pixels(size_t(w) * h);
    GLint                      old_pack = 4, old_pack_buffer = 0, old_active = 0, old_texture = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, texture);
    if (channel < 0)
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, pixels.data());
    else {
        std::vector<unsigned char> rgba(pixels.size() * 4);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = rgba[i * 4 + channel];
    }
    glBindTexture(GL_TEXTURE_2D, old_texture);
    glActiveTexture(old_active);
    glPixelStorei(GL_PACK_ALIGNMENT, old_pack);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, old_pack_buffer);
    return write_gray_bmp(path, pixels, w, h);
}

bool capture_ao_depth(const std::filesystem::path& path, unsigned int fbo, int w, int h, std::vector<float>& pixels)
{
    pixels.resize(size_t(w) * h);
    GLint old_pack = 4, old_pack_buffer = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack_buffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, pixels.data());
    glPixelStorei(GL_PACK_ALIGNMENT, old_pack);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, old_pack_buffer);
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out << "Pf\n" << w << ' ' << h << "\n-1.0\n";
    out.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size() * sizeof(float)));
    return bool(out);
}

bool capture_ao_normals(const std::filesystem::path& path, unsigned int texture, int w, int h, const std::vector<float>& depth)
{
    GLint old_pack = 4, old_buffer = 0, old_texture = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_buffer);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, texture);
    std::vector<float> encoded(size_t(w) * h * 2), normals(size_t(w) * h * 3, 0.0f);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_FLOAT, encoded.data());
    glBindTexture(GL_TEXTURE_2D, old_texture);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, old_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, old_pack);
    for (size_t i = 0; i < depth.size(); ++i) {
        if (depth[i] >= 1.0f)
            continue;
        float x = encoded[2 * i], y = encoded[2 * i + 1], z = 1.0f - std::abs(x) - std::abs(y);
        if (z < 0.0f) {
            const float old_x = x;
            x                 = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
            y                 = (1.0f - std::abs(old_x)) * (y >= 0.0f ? 1.0f : -1.0f);
        }
        const float length = std::sqrt(x * x + y * y + z * z);
        normals[3 * i]     = x / length;
        normals[3 * i + 1] = y / length;
        normals[3 * i + 2] = z / length;
    }
    std::ofstream out(path, std::ios::binary);
    out << "PF\n" << w << ' ' << h << "\n-1.0\n";
    out.write(reinterpret_cast<const char*>(normals.data()), std::streamsize(normals.size() * sizeof(float)));
    return bool(out);
}

bool capture_ao_parameters(const std::filesystem::path& path,
                           const GLAOPass::Frame&       frame,
                           const GLAOPass::Settings&    settings,
                           int                          aw,
                           int                          ah,
                           int                          source_samples,
                           bool                         sample_composite,
                           bool                         compute)
{
    std::ofstream out(path);
    out.imbue(std::locale::classic());
    out << std::setprecision(17) << "ORCA_AO_CAPTURE 1\n";
    out << "size " << frame.viewport[2] << ' ' << frame.viewport[3] << ' ' << aw << ' ' << ah << '\n';
    out << "viewport " << frame.viewport[0] << ' ' << frame.viewport[1] << '\n';
    out << "perspective " << frame.perspective << '\n';
    out << "quality " << int(settings.quality) << '\n';
    out << "settings " << settings.radius << ' ' << settings.thickness << ' ' << settings.intensity << ' ' << settings.strength << '\n';
    const auto samples = compute ? cs_ao_samples(settings.quality, settings.cs_slices_override) : ao_samples(settings.quality, settings.fs_slices_override);
    out << "samples " << samples[0] << ' ' << samples[1] << '\n';
    // Explicit column-major order; PFM rows are bottom-to-top, matching OpenGL.
    out << "projection";
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            out << ' ' << frame.projection(r, c);
    out << "\ninverse_projection";
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            out << ' ' << frame.inverse_projection(r, c);
    out << "\nsource_samples " << source_samples << "\nsample_composite " << sample_composite << '\n';
    return bool(out);
}
} // namespace

GLAOPass::Quality GLAOPass::resolve_quality(const std::string& value, const std::string& renderer)
{
    std::string name = renderer;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* software : {"llvmpipe", "softpipe", "swiftshader", "gdi generic", "software"})
        if (name.find(software) != std::string::npos)
            return Quality::Off;
    if (value == "low")
        return Quality::Low;
    if (value == "medium" || value == "auto")
        return Quality::Medium;
    if (value == "high")
        return Quality::High;
    return Quality::Off;
}

float GLAOPass::resolve_strength(const std::string& value)
{
    // AppConfig uses decimal points regardless of the UI/system locale.
    std::istringstream input(value);
    input.imbue(std::locale::classic());
    double strength = 0.0;
    if (!(input >> strength) || !std::isfinite(strength))
        return Settings{}.strength;
    input >> std::ws;
    if (!input.eof())
        return Settings{}.strength;
    return static_cast<float>(std::clamp(strength, 0.0, 1.0));
}

bool GLAOPass::fail(const std::string& reason)
{
    if (m_failure.empty())
        BOOST_LOG_TRIVIAL(warning) << "Ambient occlusion disabled: " << reason;
    m_failure = reason;
    return false;
}

void GLAOPass::release()
{
    m_compute.release();
    glDeleteFramebuffers(2, m_scene_fbo.data());
    glDeleteRenderbuffers(3, m_scene_rbo.data());
    m_scene_fbo     = {};
    m_scene_rbo     = {};
    m_scene_samples = 0;
    glDeleteTextures(1, &m_split_texture);
    glDeleteFramebuffers(1, &m_split_fbo);
    m_split_texture = m_split_fbo = 0;
    m_split_position_samples = 0;
    glDeleteTextures(1, &m_confidence_texture);
    m_confidence_texture = 0;
    glDeleteTextures(1, &m_edge_texture);
    m_edge_texture = 0;
    glDeleteTextures(4, m_texture.data());
    glDeleteFramebuffers(4, m_fbo.data());
    glDeleteTextures(1, &m_depth);
    glDeleteFramebuffers(1, &m_depth_fbo);
    glDeleteTextures(1, &m_sample_depth);
    glDeleteFramebuffers(1, &m_sample_fbo);
    glDeleteVertexArrays(1, &m_vao);
    if (m_timing)
        for (auto& queries : m_queries)
            glDeleteQueries(9, queries.data());
    m_texture = {};
    m_fbo     = {};
    m_queries = {};
    m_pending = {};
    m_depth = m_depth_fbo = m_vao = 0;
    m_width = m_height = m_ao_width = m_ao_height = 0;
    m_depth_validated = m_recording = m_timing = m_has_gpu_sample = false;
    m_samples                                                     = -1;
    m_sample_depth = m_sample_fbo = m_depth_format = 0;
    m_sample_count                                 = 0;
    m_sample_active = m_sample_failed = false;
}

bool GLAOPass::prepare(const Frame& frame, const Settings& settings, const Shaders& shaders)
{
    if (settings.quality == Quality::Off || !m_failure.empty())
        return false;
    if (!GLEW_VERSION_3_1)
        return fail("OpenGL 3.1 is required");
    for (auto* shader : shaders)
        if (!shader)
            return fail("a required AO shader is unavailable");
    const int w = frame.viewport[2], h = frame.viewport[3];
    if (w <= 0 || h <= 0)
        return false;
    const auto request = settings.force_fs ? GLAOCompute::Request::FS : settings.force_cs ? GLAOCompute::Request::CS : GLAOCompute::parse_request(std::getenv("ORCA_AO_BACKEND"));
    std::array<unsigned int, 3> compute_programs{};
    for (size_t i = 0; i < 3; ++i)
        if (frame.compute_shaders[i])
            compute_programs[i] = frame.compute_shaders[i]->get_id();
    GLAOCompute::Capabilities caps;
    // Auto and explicit CS use the same context-local capability checks.
    if (request != GLAOCompute::Request::FS)
        caps = m_compute.capabilities();
    const bool programs_ready = std::all_of(compute_programs.begin(), compute_programs.end(), [](unsigned int p) { return p != 0; });
    const auto selection = GLAOCompute::select(request, caps, programs_ready && m_compute.failure_reason().empty(), benchmark.active());
    const bool use_edges = settings.fs_edge_filter && !selection.use_cs && settings.quality == Quality::High && frame.edge_evaluate &&
                           frame.edge_denoise && !m_edge_filter_failed;
    const bool edge_changed   = use_edges != m_use_edge_filter;
    m_use_edge_filter         = use_edges;
    // Nonzero projection offsets changed a few horizon decisions in replay; keep
    // those and generalized projection matrices on the original reconstruction.
    const auto& inverse = frame.inverse_projection;
    m_fast_reconstruction = frame.fast_reconstruction && frame.edge_evaluate_xy && use_edges &&
                            inverse(0, 1) == 0.0 && inverse(0, 2) == 0.0 && inverse(0, 3) == 0.0 &&
                            inverse(1, 0) == 0.0 && inverse(1, 2) == 0.0 && inverse(1, 3) == 0.0 &&
                            inverse(2, 0) == 0.0 && inverse(2, 1) == 0.0 && inverse(3, 0) == 0.0 && inverse(3, 1) == 0.0;
    m_fs_edge_passes          = settings.fs_edge_passes == 3 ? 3 : 2;
    const bool split_ready = !frame.pixel_composite && frame.split_composite && frame.split_classify && frame.split_fast && frame.split_edges && !m_split_failed;
    const bool split_changed = split_ready != m_split_ready;
    m_split_ready = split_ready;
    const bool use_confidence = !frame.pixel_composite && !frame.split_composite && frame.precomputed_confidence && frame.confidence_normal && frame.confidence_composite &&
                                (!frame.sample_composite || frame.confidence_sample_composite) && !m_confidence_failed;
    const bool confidence_changed = use_confidence != m_use_confidence;
    m_use_confidence              = use_confidence;
    const bool backend_changed    = m_use_compute != selection.use_cs;
    m_use_compute                 = selection.use_cs;
    m_backend_reason              = selection.reason;
    if (!m_compute.failure_reason().empty() && request != GLAOCompute::Request::FS)
        m_backend_reason = m_compute.failure_reason();
    if (!selection.benchmark_allowed)
        benchmark.finish("ABORTED: forced CS unavailable: " + m_backend_reason);
    const char* denoise        = std::getenv("ORCA_AO_CS_DENOISE");
    int         denoise_passes = settings.quality == Quality::High ? 3 : 2;
    if (denoise && denoise[0] >= '1' && denoise[0] <= '3' && denoise[1] == '\0')
        denoise_passes = denoise[0] - '0';
    const bool denoise_changed  = m_denoise_passes != denoise_passes;
    m_denoise_passes            = denoise_passes;
    m_effective_samples         = m_use_compute ? cs_ao_samples(settings.quality, settings.cs_slices_override) :
                                                  ao_samples(settings.quality, settings.fs_slices_override);
    int aw                      = m_use_compute || settings.quality == Quality::High ? w : (w + 1) / 2;
    int ah                      = m_use_compute || settings.quality == Quality::High ? h : (h + 1) / 2;
    m_shaders                   = shaders;
    m_quality = settings.quality;
    m_cs_low_reference = frame.cs_low_reference;
    m_composite_reference = frame.composite_reference;
    if (!backend_changed && !edge_changed && !confidence_changed && !split_changed && !(m_use_compute && denoise_changed) &&
        m_width == w && m_height == h && m_ao_width == aw && m_ao_height == ah) {
        // Matching resources do not imply matching shader programs. Refresh the
        // compute bindings even when no texture allocation is needed (e.g. Low A/B).
        if (!m_use_compute || m_compute.prepare(w, h, compute_programs, m_denoise_passes))
            return true;
        // A failed refresh follows the normal state-safe FS fallback below.
    }
    AOState state;
    if (consume_errors())
        BOOST_LOG_TRIVIAL(warning) << "GL error before AO resource preparation";
    release();
    // Decide the backend before binding any new pass-owned FBOs/textures.
    // A recursive retry would save those bindings and then delete their objects.
    if (m_use_compute && !m_compute.prepare(w, h, compute_programs, m_denoise_passes)) {
        m_backend_reason = m_compute.failure_reason();
        benchmark.finish("ABORTED: CS initialization failed: " + m_backend_reason);
        m_use_compute       = false;
        m_effective_samples = ao_samples(settings.quality, settings.fs_slices_override);
        aw                  = settings.quality == Quality::High ? w : (w + 1) / 2;
        ah                  = settings.quality == Quality::High ? h : (h + 1) / 2;
        BOOST_LOG_TRIVIAL(warning) << "XeGTAO falling back to FS: " << m_backend_reason;
    }
    m_width     = w;
    m_height    = h;
    m_ao_width  = aw;
    m_ao_height = ah;
    glGenVertexArrays(1, &m_vao);
    glGenTextures(4, m_texture.data());
    glGenFramebuffers(4, m_fbo.data());
    glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < 4; ++i) {
        if (m_use_compute && i != 0)
            continue;
        const bool full = i == 0 || i == 3;
        glBindTexture(GL_TEXTURE_2D, m_texture[i]);
        texture_parameters();
        glTexImage2D(GL_TEXTURE_2D, 0, i == 0 ? GL_RG16F : GL_R8, full ? w : aw, full ? h : ah, 0, i == 0 ? GL_RG : GL_RED, GL_FLOAT,
                     nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture[i], 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        const bool error    = consume_errors();
        if (!complete || error) {
            release();
            return fail("AO texture allocation or framebuffer validation failed");
        }
    }
    if (m_split_ready) {
        glGenTextures(1, &m_split_texture);
        glBindTexture(GL_TEXTURE_2D, m_split_texture);
        texture_parameters();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
        glGenFramebuffers(1, &m_split_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_split_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_split_texture, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        const bool error = consume_errors();
        if (!complete || error) {
            glDeleteTextures(1, &m_split_texture);
            glDeleteFramebuffers(1, &m_split_fbo);
            m_split_texture = m_split_fbo = 0;
            m_split_ready = false;
            m_split_failed = true;
            benchmark.finish("ABORTED: split composite allocation failed");
            BOOST_LOG_TRIVIAL(warning) << "AO split composite unavailable; using per-sample composite";
        }
    }
    if (m_use_confidence) {
        glGenTextures(1, &m_confidence_texture);
        glBindTexture(GL_TEXTURE_2D, m_confidence_texture);
        texture_parameters();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[0]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_confidence_texture, 0);
        const GLenum buffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(2, buffers);
        const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        const bool error = consume_errors();
        if (!complete || error) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glDeleteTextures(1, &m_confidence_texture);
            m_confidence_texture = 0;
            m_use_confidence = false;
            m_confidence_failed = true;
            benchmark.finish("ABORTED: confidence allocation failed");
            BOOST_LOG_TRIVIAL(warning) << "AO confidence precompute unavailable; retaining direct composite";
        }
    }
    if (m_use_edge_filter) {
        glGenTextures(1, &m_edge_texture);
        glBindTexture(GL_TEXTURE_2D, m_edge_texture);
        texture_parameters();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[1]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_edge_texture, 0);
        const GLenum buffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(2, buffers);
        const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        const bool error = consume_errors();
        if (!complete || error) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glDeleteTextures(1, &m_edge_texture);
            m_edge_texture = 0;
            m_use_edge_filter = false;
            m_edge_filter_failed = true;
            benchmark.finish("ABORTED: FS edge filter allocation failed");
            BOOST_LOG_TRIVIAL(warning) << "FS edge filter unavailable; retaining geometry filter";
        }
    }
    m_timing = GLEW_VERSION_3_3 || GLEW_ARB_timer_query;
    if (m_timing)
        for (auto& queries : m_queries)
            glGenQueries(9, queries.data());
    return true;
}

void GLAOPass::forget_context()
{
    m_split_texture = m_split_fbo = 0;
    m_quality = Quality::Off;
    m_pixel_active = false;
    m_fast_reconstruction = false;
    m_split_ready = m_split_active = m_split_failed = false;
    m_split_position_samples = 0;
    m_confidence_texture = 0;
    m_use_confidence = m_confidence_failed = false;
    m_edge_texture = 0;
    m_use_edge_filter = m_edge_filter_failed = false;
    m_compute.forget_context();
    m_use_compute = false;
    m_backend_reason.clear();
    m_scene_fbo     = {};
    m_scene_rbo     = {};
    m_scene_samples = 0;
    m_texture       = {};
    m_fbo           = {};
    m_queries       = {};
    m_pending       = {};
    m_shaders       = {};
    m_gpu_ms        = {};
    m_depth = m_depth_fbo = m_vao = m_source = m_query_slot = 0;
    m_width = m_height = m_ao_width = m_ao_height = 0;
    m_samples                                     = -1;
    m_depth_validated = m_timing = m_recording = m_has_gpu_sample = false;
    m_sample_depth = m_sample_fbo = m_depth_format = 0;
    m_sample_count                                 = 0;
    m_sample_active = m_sample_failed = false;
    m_failure.clear();
}

bool GLAOPass::render_scene(const Frame& frame, const std::function<bool(const Frame&)>& draw_scene)
{
    AOState saved;
    glBindFramebuffer(GL_FRAMEBUFFER, frame.target);
    GLint samples = 0, depth_bits = 0, stencil_bits = 0;
    glGetIntegerv(GL_SAMPLES, &samples);
    if (frame.source != 0 || frame.target != 0 || samples <= 1 || !GLEW_VERSION_4_0 || !frame.sample_composite)
        return draw_scene(frame);
    GLint red_bits = 0, green_bits = 0, blue_bits = 0, color_encoding = GL_LINEAR, color_buffer = 0;
    glGetIntegerv(GL_RED_BITS, &red_bits);
    glGetIntegerv(GL_GREEN_BITS, &green_bits);
    glGetIntegerv(GL_BLUE_BITS, &blue_bits);
    glGetIntegerv(GL_DRAW_BUFFER0, &color_buffer);
    const GLenum color_attachment = color_buffer == GL_BACK  ? GL_BACK_LEFT :
                                    color_buffer == GL_FRONT ? GL_FRONT_LEFT :
                                                               GLenum(color_buffer);
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, color_attachment, GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &color_encoding);
    // Other window formats keep resolved AO until their presentation conversion is validated.
    if (red_bits != 8 || green_bits != 8 || blue_bits != 8 || color_encoding != GL_LINEAR)
        return draw_scene(frame);
    glGetIntegerv(GL_DEPTH_BITS, &depth_bits);
    glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);
    // Match the window depth format for the later depth-only transfer.
    const GLenum depth_format = stencil_bits     ? (depth_bits == 32 ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8) :
                                depth_bits == 32 ? GL_DEPTH_COMPONENT32F :
                                depth_bits == 16 ? GL_DEPTH_COMPONENT16 :
                                                   GL_DEPTH_COMPONENT24;
    GLint        old_rbo      = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_rbo);
    if (m_scene_samples != samples) {
        glDeleteFramebuffers(2, m_scene_fbo.data());
        glDeleteRenderbuffers(3, m_scene_rbo.data());
        glGenFramebuffers(2, m_scene_fbo.data());
        glGenRenderbuffers(3, m_scene_rbo.data());
        glBindFramebuffer(GL_FRAMEBUFFER, m_scene_fbo[0]);
        glBindRenderbuffer(GL_RENDERBUFFER, m_scene_rbo[0]);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, m_width, m_height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_scene_rbo[0]);
        glBindRenderbuffer(GL_RENDERBUFFER, m_scene_rbo[1]);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, depth_format, m_width, m_height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, stencil_bits ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                  m_scene_rbo[1]);
        bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, m_scene_fbo[1]);
        glBindRenderbuffer(GL_RENDERBUFFER, m_scene_rbo[2]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, m_width, m_height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_scene_rbo[2]);
        complete &= glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindRenderbuffer(GL_RENDERBUFFER, old_rbo);
        if (!complete || consume_errors())
            return fail("AO scene target allocation failed");
        m_scene_samples = samples;
    }
    Frame scene  = frame;
    scene.source = scene.target = m_scene_fbo[0];
    scene.viewport[0] = scene.viewport[1] = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, scene.target);
    glViewport(0, 0, m_width, m_height);
    const std::string capture_directory = m_capture_directory;
    if (!draw_scene(scene))
        return false;
    std::error_code capture_error;
    const auto      complete_path = std::filesystem::path(capture_directory) / "capture-complete.txt";
    const bool      captured      = !capture_directory.empty() && std::filesystem::exists(complete_path, capture_error);
    if (captured)
        std::filesystem::remove(complete_path, capture_error);
    saved.fullscreen();
    benchmark.mark(11);
    // Resolve only once in the FBO layout. Do not copy multisample colors to the
    // default framebuffer: its sample numbering/pattern need not match ours.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scene.target);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_scene_fbo[1]);
    glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    benchmark.mark(18);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_scene_fbo[1]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, frame.target);
    glBlitFramebuffer(0, 0, m_width, m_height, frame.viewport[0], frame.viewport[1], frame.viewport[0] + m_width,
                      frame.viewport[1] + m_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    // Preserve geometric depth for subsequent decorations/transparent volumes.
    benchmark.mark(12);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scene.target);
    glBlitFramebuffer(0, 0, m_width, m_height, frame.viewport[0], frame.viewport[1], frame.viewport[0] + m_width,
                      frame.viewport[1] + m_height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    benchmark.mark(13);
    if (consume_errors())
        return fail("AO scene presentation failed");
    if (captured && !capture_error &&
        capture_ao_color(std::filesystem::path(capture_directory) / "color-presented.bmp", frame.target, frame.viewport)) {
        std::ofstream complete(complete_path);
        complete << "AO capture complete\n" << m_width << ' ' << m_height << '\n';
    }
    return true;
}

bool GLAOPass::allocate_depth(unsigned int format)
{
    m_depth_format = format;
    glDeleteTextures(1, &m_depth);
    glDeleteFramebuffers(1, &m_depth_fbo);
    glGenTextures(1, &m_depth);
    glGenFramebuffers(1, &m_depth_fbo);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_depth);
    texture_parameters();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    const bool   packed = format == GL_DEPTH24_STENCIL8 || format == GL_DEPTH32F_STENCIL8;
    const GLenum type   = format == GL_DEPTH32F_STENCIL8  ? GL_FLOAT_32_UNSIGNED_INT_24_8_REV :
                          packed                          ? GL_UNSIGNED_INT_24_8 :
                          format == GL_DEPTH_COMPONENT32F ? GL_FLOAT :
                                                            GL_UNSIGNED_INT;
    glTexImage2D(GL_TEXTURE_2D, 0, format, m_width, m_height, 0, packed ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT, type, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, m_depth_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, packed ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_depth, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    return !consume_errors() && complete;
}

bool GLAOPass::capture_depth(const Frame& frame)
{
    glBindFramebuffer(GL_FRAMEBUFFER, frame.source);
    GLint samples = 0;
    glGetIntegerv(GL_SAMPLES, &samples);
    if (m_source != frame.source || m_samples != samples)
        m_depth_validated = false;
    m_source  = frame.source;
    m_samples = samples;
    auto copy = [&]() {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, frame.source);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_depth_fbo);
        glBlitFramebuffer(frame.viewport[0], frame.viewport[1], frame.viewport[0] + m_width, frame.viewport[1] + m_height, 0, 0, m_width,
                          m_height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        return !consume_errors();
    };
    if (m_depth_validated)
        return copy() || fail("scene depth blit failed");
    GLint        bits = 0, type = GL_UNSIGNED_NORMALIZED;
    const GLenum attachment = frame.source == 0 ? GL_DEPTH : GL_DEPTH_ATTACHMENT;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &bits);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &type);
    if (consume_errors() || bits == 0)
        return fail("cannot determine the scene depth format");
    const std::array<GLenum, 6> candidates{{GL_DEPTH_COMPONENT24, GL_DEPTH24_STENCIL8, GL_DEPTH_COMPONENT32F, GL_DEPTH32F_STENCIL8,
                                            GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT32}};
    for (GLenum format : candidates) {
        const bool floating       = format == GL_DEPTH_COMPONENT32F || format == GL_DEPTH32F_STENCIL8;
        const int  candidate_bits = floating || format == GL_DEPTH_COMPONENT32 ? 32 : format == GL_DEPTH_COMPONENT16 ? 16 : 24;
        if (candidate_bits != bits || floating != (type == GL_FLOAT))
            continue;
        if (allocate_depth(format) && copy()) {
            m_depth_validated = true;
            BOOST_LOG_TRIVIAL(info) << "AO depth format " << format << ", samples " << samples << ", " << m_width << "x" << m_height;
            return true;
        }
    }
    return fail("no matching depth resolve format");
}

bool GLAOPass::capture_sample_depth(const Frame& frame)
{
    // Diagnostic A/B switch; the launcher restores the parent's environment.
    const char* disabled = std::getenv("ORCA_AO_DISABLE_SAMPLE_COMPOSITE");
    if (disabled && std::string(disabled) == "1")
        return false;
    // Never index a window's colors with sample IDs from a copied texture. Window
    // MSAA scenes must go through render_scene; other formats use resolved AO.
    if (!frame.sample_composite || !GLEW_VERSION_4_0 || m_samples <= 1 || frame.source == 0 || frame.source != frame.target ||
        m_sample_failed)
        return false;
    if (!m_sample_depth || m_sample_count != m_samples) {
        glDeleteTextures(1, &m_sample_depth);
        glDeleteFramebuffers(1, &m_sample_fbo);
        glGenTextures(1, &m_sample_depth);
        glGenFramebuffers(1, &m_sample_fbo);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_sample_depth);
        glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, m_samples, m_depth_format, m_width, m_height, GL_TRUE);
        glBindFramebuffer(GL_FRAMEBUFFER, m_sample_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D_MULTISAMPLE, m_sample_depth, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        const bool error    = consume_errors();
        if (!complete || error) {
            m_sample_failed = true;
            BOOST_LOG_TRIVIAL(warning) << "AO sample depth unavailable; using resolved composite";
            return false;
        }
        m_sample_count = m_samples;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, frame.source);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_sample_fbo);
    glBlitFramebuffer(frame.viewport[0], frame.viewport[1], frame.viewport[0] + m_width, frame.viewport[1] + m_height, 0, 0, m_width,
                      m_height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    if (consume_errors()) {
        m_sample_failed = true;
        BOOST_LOG_TRIVIAL(warning) << "AO sample depth copy failed; using resolved composite";
        return false;
    }
    return true;
}

GLShaderProgram* GLAOPass::sample_composite_shader(const Frame& frame) const
{
    return m_split_active ? frame.split_edges : m_use_confidence ? frame.confidence_sample_composite : frame.sample_composite;
}

void GLAOPass::draw(int index, unsigned int target, int w, int h, const Frame& frame, const Settings& settings)
{
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    glViewport(0, 0, w, h);
    glBindVertexArray(m_vao);
    auto& shader = *(index == 5 ? frame.split_classify : index == 6 ? frame.split_fast :
                      index == 4 && m_sample_active && (settings.debug_view == 0 || settings.debug_view == 5 || settings.debug_view == 6) ?
                         sample_composite_shader(frame) :
                         m_use_confidence && index == 0 ? frame.confidence_normal :
                         m_use_confidence && index == 4 ? frame.confidence_composite :
                         m_use_edge_filter && index == 1 ? (m_fast_reconstruction ? frame.edge_evaluate_xy : frame.edge_evaluate) :
                         m_use_edge_filter && index == 2 ? frame.edge_denoise : m_shaders[index]);
    shader.start_using();
    shader.set_uniform("depth_texture", 0);
    shader.set_uniform("normal_texture", 1);
    shader.set_uniform("ao_texture", 2);
    shader.set_uniform("sample_depth_texture", 3);
    shader.set_uniform("edge_texture", 3);
    shader.set_uniform("confidence_texture", 4);
    shader.set_uniform("composite_factor_texture", 4);
    shader.set_uniform("capture_sample", settings.debug_view == 5 || settings.debug_view == 6);
    shader.set_uniform("inv_projection", frame.inverse_projection);
    shader.set_uniform("projection", frame.projection);
    shader.set_uniform("full_size", std::array<int, 2>{{m_width, m_height}});
    shader.set_uniform("ao_size", std::array<int, 2>{{m_ao_width, m_ao_height}});
    shader.set_uniform("perspective", frame.perspective);
    shader.set_uniform("radius", settings.radius);
    shader.set_uniform("thickness", settings.thickness);
    shader.set_uniform("intensity", m_use_compute ? 1.0f : settings.intensity);
    shader.set_uniform("ao_strength", settings.strength);
    const auto samples = ao_samples(settings.quality, settings.fs_slices_override);
    shader.set_uniform("slice_count", samples[0]);
    shader.set_uniform("step_count", samples[1]);
    shader.set_uniform("high_sampling_noise", settings.quality == Quality::High);
    shader.set_uniform("debug_view", settings.debug_view);
    if (index == 4 || index == 6)
        glViewport(frame.viewport[0], frame.viewport[1], w, h);
    if ((index == 4 || index == 6) && (m_sample_active || (m_pixel_active && m_samples > 1)) && settings.debug_view == 0)
        glEnable(GL_MULTISAMPLE);
    shader.set_uniform("viewport_origin", std::array<int, 2>{{(index == 4 || index == 6) ? frame.viewport[0] : 0, (index == 4 || index == 6) ? frame.viewport[1] : 0}});
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void GLAOPass::begin_timing()
{
    std::ostringstream description;
    description << "actual_backend=" << backend() << "\nfallback_reason=" << m_backend_reason << "\nao_size=" << m_ao_width << 'x'
                << m_ao_height << "\ncs_denoise_actual=" << (m_use_compute ? m_denoise_passes : 0)
                << "\ncomposite_variant=" << (m_pixel_active ? "pixel" : m_split_active ? "split_msaa" : m_use_confidence ? "confidence_precomputed" : m_composite_reference ? "reference" : "optimized") << "\ncs_low_normalization=" << (m_use_compute && m_quality == Quality::Low ? (m_cs_low_reference ? "legacy" : "unoccluded_slice") : "not_applicable") << "\nfs_reconstruction=" << (m_use_compute ? "not_applicable" : fast_reconstruction_active() ? "separable_xy" : "matrix") << "\nfs_filter=" << (m_use_compute ? "not_applicable" : m_use_edge_filter ? (m_fs_edge_passes == 3 ? "connectivity_3pass" : "connectivity_2pass") : "geometry") << "\nactual_slices=" << m_effective_samples[0] << "\nactual_steps=" << m_effective_samples[1]
                << "\ncs_evaluate_program=" << (m_use_compute ? m_compute.evaluation_program() : 0) << "\ncs_implementation=xegtao_glsl_v1\ncs_upstream=" << GLAOCompute::upstream;
    benchmark.pipeline(description.str());
    benchmark.mark(2);
    m_recording = false;
    if (!m_timing)
        return;
    // Poll all completed slots; never wait for the GPU or force extra frames.
    for (unsigned int slot = 0; slot < m_queries.size(); ++slot)
        if (m_pending[slot]) {
            GLint ready = GL_FALSE;
            glGetQueryObjectiv(m_queries[slot][8], GL_QUERY_RESULT_AVAILABLE, &ready);
            if (ready) {
                std::array<GLuint64, 9> times{};
                for (int i = 0; i < 9; ++i)
                    glGetQueryObjectui64v(m_queries[slot][i], GL_QUERY_RESULT, &times[i]);
                for (int i = 0; i < 7; ++i)
                    m_gpu_ms[i] = double(times[i + 1] - times[i]) / 1e6;
                m_gpu_ms[7]      = double(times[8] - times[0]) / 1e6;
                m_pending[slot]  = false;
                m_has_gpu_sample = true;
            }
        }
    for (unsigned int i = 0; i < m_queries.size(); ++i) {
        m_query_slot = (m_query_slot + 1) % m_queries.size();
        if (!m_pending[m_query_slot]) {
            m_recording = true;
            mark_time(0);
            break;
        }
    }
}
void GLAOPass::mark_time(int index)
{
    benchmark.mark(index + 2);
    if (m_recording)
        glQueryCounter(m_queries[m_query_slot][index], GL_TIMESTAMP);
}
void GLAOPass::end_timing()
{
    mark_time(8);
    if (m_recording)
        m_pending[m_query_slot] = true;
    m_recording = false;
}

bool GLAOPass::render(const Frame& frame, const Settings& settings, const std::function<void()>& receiver_draw)
{
    if (!m_vao || !m_failure.empty())
        return false;
    AOState state;
    if (consume_errors())
        BOOST_LOG_TRIVIAL(warning) << "GL error before AO depth capture";
    state.fullscreen();
    // Set the requested variant before recording per-run metadata. Unsupported
    // sample layouts abort the benchmark immediately after the depth copy below.
    m_pixel_active = frame.pixel_composite && settings.debug_view == 0;
    m_split_active = m_split_ready && settings.debug_view == 0;
    begin_timing();
    if (!capture_depth(frame)) {
        m_recording = false;
        return false;
    }
    benchmark.mark(25); // Split resolved depth from the MSAA sample-depth copy.
    m_sample_active = !m_pixel_active && capture_sample_depth(frame);
    if (m_sample_active)
        benchmark.mark(29);
    // Color sample capture is independent of whether AO uses a copied sample-depth texture.
    const int color_sample_count = frame.source != 0 && frame.source == frame.target && m_samples > 1 ? m_samples : 0;
    if (frame.require_msaa_comparison && (color_sample_count <= 1 || settings.debug_view != 0) && benchmark.active())
        benchmark.finish("ABORTED: pixel/MSAA comparison requires an MSAA scene and normal display");
    m_split_active = m_split_ready && m_sample_active && m_sample_count > 1 && m_sample_count <= 8 && settings.debug_view == 0;
    if (frame.split_composite && !m_split_active && benchmark.active())
        benchmark.finish("ABORTED: split composite requires its programs and 2..8 MSAA samples");
    std::filesystem::path capture_path;
    std::vector<float>    capture_before, capture_after;
    bool                  capture_ok = true;
    if (benchmark.active())
        m_capture_directory.clear();
    if (!m_capture_directory.empty()) {
        capture_path = m_capture_directory;
        m_capture_directory.clear();
        std::error_code error;
        std::filesystem::create_directories(capture_path, error);
        if (error) {
            BOOST_LOG_TRIVIAL(warning) << "AO capture directory unavailable: " << error.message();
            capture_path.clear();
        } else {
            std::filesystem::remove(capture_path / "capture-complete.txt", error);
            if (error) {
                BOOST_LOG_TRIVIAL(warning) << "AO capture cannot clear previous completion marker: " << error.message();
                capture_path.clear();
            } else {
                capture_ok &= capture_ao_depth(capture_path / "depth-before-receiver.pfm", m_depth_fbo, m_width, m_height, capture_before);
                capture_ok &= capture_ao_parameters(capture_path / "capture-info.txt", frame, settings, m_ao_width, m_ao_height, m_samples,
                                                    m_sample_active && settings.debug_view == 0, m_use_compute);
            }
        }
    }
    mark_time(1);
    glBindFramebuffer(GL_FRAMEBUFFER, m_depth_fbo);
    glViewport(0, 0, m_width, m_height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glBindVertexArray(m_vao);
    receiver_draw();
    benchmark.mark(30);
    if (m_sample_active) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_sample_fbo);
        glEnable(GL_MULTISAMPLE);
        receiver_draw();
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_sample_depth);
    }
    if (!capture_path.empty()) {
        capture_ok &= capture_ao_depth(capture_path / "depth-after-receiver.pfm", m_depth_fbo, m_width, m_height, capture_after);
        std::vector<unsigned char> receiver_mask(capture_after.size());
        for (size_t i = 0; i < capture_after.size(); ++i)
            receiver_mask[i] = capture_after[i] < capture_before[i] ? 255 : 0;
        capture_ok &= write_gray_bmp(capture_path / "receiver-mask.bmp", receiver_mask, m_width, m_height);
    }
    mark_time(2);
    state.fullscreen();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_depth);
    // Do not bind a render target as an input, even to an unused sampler.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, 0); // Never leave the confidence output bound as input.
    draw(0, m_fbo[0], m_width, m_height, frame, settings);
    if (m_use_confidence) {
        glBindTexture(GL_TEXTURE_2D, m_confidence_texture);
        if (!capture_path.empty())
            capture_ok &= capture_ao_texture(capture_path / "confidence-precomputed.bmp", m_confidence_texture, m_width, m_height);
    }
    glActiveTexture(GL_TEXTURE2);
    if (!capture_path.empty())
        capture_ok &= capture_ao_normals(capture_path / "normals-view.pfm", m_texture[0], m_width, m_height, capture_after);
    mark_time(3);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_texture[0]);
    unsigned int filtered_texture = m_texture[2];
    if (m_use_compute) {
        GLAOCompute::Parameters parameters;
        const auto              inverse = frame.inverse_projection.cast<float>().eval();
        std::copy(inverse.data(), inverse.data() + 16, parameters.inverse.begin());
        parameters.width          = m_width;
        parameters.height         = m_height;
        parameters.radius         = settings.radius;
        parameters.perspective    = frame.perspective;
        parameters.denoise_passes = m_denoise_passes;
        parameters.slices         = m_effective_samples[0];
        parameters.steps          = m_effective_samples[1];
        benchmark.mark(19);
        const bool success = m_compute.render(m_depth, m_texture[0], parameters, [&](int stage) {
            benchmark.mark(20 + stage);
            if (stage == 1)
                mark_time(4);
        });
        if (success) {
            filtered_texture = m_compute.output();
            if (!capture_path.empty())
                capture_ok &= m_compute.capture(capture_path.string(), m_denoise_passes);
        } else {
            m_backend_reason = m_compute.failure_reason().empty() ? "invalid CS parameters" : m_compute.failure_reason();
            benchmark.finish("ABORTED: CS dispatch failed: " + m_backend_reason);
            if (m_backend_reason == "OpenGL context lost") {
                m_recording = false;
                return fail(m_backend_reason);
            }
            m_compute.disable(m_backend_reason);
            m_use_compute       = false;
            m_effective_samples = ao_samples(settings.quality, settings.fs_slices_override);
            if (!capture_path.empty())
                capture_ok &= capture_ao_parameters(capture_path / "capture-info.txt", frame, settings, m_ao_width, m_ao_height, m_samples,
                                                    m_sample_active && settings.debug_view == 0, false);
            BOOST_LOG_TRIVIAL(warning) << "XeGTAO falling back to FS: " << m_backend_reason;
            glActiveTexture(GL_TEXTURE2);
            for (int i = 1; i < 4; ++i) {
                glBindTexture(GL_TEXTURE_2D, m_texture[i]);
                texture_parameters();
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, m_width, m_height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[i]);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture[i], 0);
                glDrawBuffer(GL_COLOR_ATTACHMENT0);
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                    return fail("FS fallback allocation failed");
            }
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }
    if (!m_use_compute) {
        // Unit 3 held the previous frame's guide; never sample an attached output.
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE2);
        draw(1, m_fbo[1], m_ao_width, m_ao_height, frame, settings);
        if (!capture_path.empty())
            capture_ok &= capture_ao_texture(capture_path / "ao-raw.bmp", m_texture[1], m_ao_width, m_ao_height);
        mark_time(4);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_texture[1]);
        GLShaderProgram* filter_shader = m_use_edge_filter ? frame.edge_denoise : m_shaders[2];
        if (m_use_edge_filter) {
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, m_edge_texture);
            glActiveTexture(GL_TEXTURE2);
        }
        filter_shader->start_using();
        filter_shader->set_uniform("filter_refine", false);
        draw(2, m_fbo[2], m_ao_width, m_ao_height, frame, settings);
        if (!capture_path.empty() && m_use_edge_filter)
            capture_ok &= capture_ao_texture(capture_path / "fs-denoise-first.bmp", m_texture[2], m_width, m_height);
        benchmark.mark(16);
        if (m_ao_width == m_width && m_ao_height == m_height) {
            // Refine only High's full-resolution output. The raw buffer stays intact
            // for diagnostics; the otherwise unused upsample target holds the result.
            glBindTexture(GL_TEXTURE_2D, m_texture[2]);
            const bool third_pass = m_use_edge_filter && m_fs_edge_passes == 3;
            filter_shader->set_uniform("filter_refine", !third_pass);
            draw(2, m_fbo[3], m_width, m_height, frame, settings);
            if (third_pass)
                benchmark.mark(26);
            filtered_texture = m_texture[3];
            if (third_pass) {
                if (!capture_path.empty())
                    capture_ok &= capture_ao_texture(capture_path / "fs-denoise-second.bmp", m_texture[3], m_width, m_height);
                // Ping-pong into the first-pass target; preserve raw AO and the edge guide.
                glBindTexture(GL_TEXTURE_2D, m_texture[3]);
                filter_shader->set_uniform("filter_refine", true);
                draw(2, m_fbo[2], m_width, m_height, frame, settings);
                filtered_texture = m_texture[2];
            }
            benchmark.mark(17);
            filter_shader->set_uniform("filter_refine", false);
        }
    }
    if (!capture_path.empty() && m_use_edge_filter)
        for (int channel = 0; channel < 4; ++channel)
            capture_ok &= capture_ao_texture(capture_path / ("fs-edge-" + std::to_string(channel) + ".bmp"),
                                            m_edge_texture, m_width, m_height, channel);
    if (!capture_path.empty()) {
        std::ofstream metadata(capture_path / "backend-info.txt");
        metadata << "actual_backend=" << backend() << "\nfallback_reason=" << m_backend_reason << "\nupstream=" << GLAOCompute::upstream
                 << "\ncs_evaluate_program=" << (m_use_compute ? m_compute.evaluation_program() : 0) << "\nsample_capture_version=3\nsample_surface_diagnostics=" << (m_sample_active ? "available" : "not_applicable")
                 << "\ncolor_sample_count=" << color_sample_count << "\ndenoise_passes=" << (m_use_compute ? m_denoise_passes : 0) << "\nactual_slices=" << m_effective_samples[0]
                 << "\nactual_steps=" << m_effective_samples[1] << "\ncomposite_variant=" << (m_pixel_active ? "pixel" : m_split_active ? "split_msaa" : m_use_confidence ? "confidence_precomputed" : m_composite_reference ? "reference" : "optimized") << "\ncs_low_normalization=" << (m_use_compute && m_quality == Quality::Low ? (m_cs_low_reference ? "legacy" : "unoccluded_slice") : "not_applicable") << "\nfs_reconstruction=" << (m_use_compute ? "not_applicable" : fast_reconstruction_active() ? "separable_xy" : "matrix") << "\nfs_filter=" << (m_use_compute ? "not_applicable" : m_use_edge_filter ? (m_fs_edge_passes == 3 ? "connectivity_3pass" : "connectivity_2pass") : "geometry") << '\n';
        capture_ok &= bool(metadata);
    }
    if (!capture_path.empty())
        capture_ok &= capture_ao_texture(capture_path / "ao-filtered.bmp", filtered_texture, m_ao_width, m_ao_height);
    mark_time(5);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, filtered_texture);
    if (m_ao_width != m_width || m_ao_height != m_height) {
        draw(3, m_fbo[3], m_width, m_height, frame, settings);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_texture[3]);
    }
    if (!capture_path.empty()) {
        const unsigned int final_texture = m_ao_width != m_width || m_ao_height != m_height ? m_texture[3] : filtered_texture;
        capture_ok &= capture_ao_texture(capture_path / "ao-final-full.bmp", final_texture, m_width, m_height);
    }
    mark_time(6);
    if (consume_errors()) {
        m_recording = false;
        return fail("AO evaluation failed");
    }
    if (!capture_path.empty())
        capture_ok &= capture_ao_color(capture_path / "color-before-ao.bmp", frame.target, frame.viewport);
    if (!capture_path.empty() && color_sample_count > 1)
        capture_ok &= capture_ao_sample_colors(capture_path, "before", frame.target, frame.viewport, color_sample_count, m_vao);
    if (m_split_active) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, 0);
        auto* classify = frame.split_classify;
        classify->start_using();
        classify->set_uniform("sample_count", m_sample_count);
        if (m_split_position_samples != m_sample_count || m_split_position_target != frame.target) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, frame.target);
            for (int sample = 0; sample < m_sample_count; ++sample)
                glGetMultisamplefv(GL_SAMPLE_POSITION, sample, m_split_positions[sample].data());
            m_split_position_samples = m_sample_count;
            m_split_position_target = frame.target;
        }
        for (int sample = 0; sample < m_sample_count; ++sample)
            classify->set_uniform(("sample_positions[" + std::to_string(sample) + "]").c_str(), m_split_positions[sample]);
        glDisable(GL_BLEND);
        draw(5, m_split_fbo, m_width, m_height, frame, settings);
        benchmark.mark(27);
        glBindTexture(GL_TEXTURE_2D, m_split_texture);
    }
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(settings.debug_view == 0 ? GL_DST_COLOR : GL_ONE, GL_ZERO, GL_ZERO, GL_ONE);
    if (m_split_active) {
        draw(6, frame.target, m_width, m_height, frame, settings);
        benchmark.mark(28);
    }
    draw(4, frame.target, m_width, m_height, frame, settings);
    mark_time(7);
    end_timing();
    if (!capture_path.empty()) {
        capture_ok &= capture_ao_color(capture_path / "color-after-ao.bmp", frame.target, frame.viewport);
        if (m_split_active)
            capture_ok &= capture_ao_texture(capture_path / "composite-fast-factor.bmp", m_split_texture, m_width, m_height);
        if (color_sample_count > 1)
            capture_ok &= capture_ao_sample_colors(capture_path, "after", frame.target, frame.viewport, color_sample_count, m_vao);
        AOState capture_state;
        capture_state.fullscreen();
        GLuint fbo = 0, rbo = 0;
        GLint  old_rbo = 0, old_pack = 0, old_buffer = 0, old_row = 0, old_rows = 0, old_pixels = 0;
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_rbo);
        glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_buffer);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &old_row);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &old_rows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &old_pixels);
        glGenFramebuffers(1, &fbo);
        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_R8, m_width, m_height);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            auto diagnostic_frame        = frame;
            diagnostic_frame.viewport[0] = diagnostic_frame.viewport[1] = 0;
            auto diagnostic_settings                                    = settings;
            diagnostic_settings.debug_view                              = 4;
            draw(4, fbo, m_width, m_height, diagnostic_frame, diagnostic_settings);
            std::vector<unsigned char> pixels(size_t(m_width) * m_height);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            glPixelStorei(GL_PACK_SKIP_ROWS, 0);
            glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
            glReadPixels(0, 0, m_width, m_height, GL_RED, GL_UNSIGNED_BYTE, pixels.data());
            capture_ok &= write_gray_bmp(capture_path / "composite-confidence.bmp", pixels, m_width, m_height);
        } else {
            capture_ok = false;
        }
        if (m_sample_active) {
            // This is deliberately inside the one-shot capture only. No extra
            // targets, sample reads or disk writes occur on ordinary frames.
            glBindRenderbuffer(GL_RENDERBUFFER, rbo);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA32F, m_width, m_height);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                std::ofstream positions(capture_path / "sample-positions.txt");
                positions.imbue(std::locale::classic());
                positions << "ORCA_AO_SAMPLES 1\ncount " << m_sample_count << '\n';
                std::ofstream target_positions(capture_path / "sample-target-positions.txt");
                target_positions.imbue(std::locale::classic());
                target_positions << "ORCA_AO_SAMPLES 1\ncount " << m_sample_count << '\n';
                std::vector<float> rgba(size_t(m_width) * m_height * 4);
                std::vector<float> depth(size_t(m_width) * m_height), match(depth.size() * 3);
                auto               sample_frame = frame;
                sample_frame.viewport[0] = sample_frame.viewport[1] = 0;
                auto sample_settings                                = settings;
                sample_settings.debug_view                          = 5;
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glPixelStorei(GL_PACK_ROW_LENGTH, 0);
                glPixelStorei(GL_PACK_SKIP_ROWS, 0);
                glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
                for (int sample = 0; sample < m_sample_count; ++sample) {
                    std::array<float, 2> position{};
                    glBindFramebuffer(GL_FRAMEBUFFER, m_sample_fbo);
                    glGetMultisamplefv(GL_SAMPLE_POSITION, sample, position.data());
                    positions << sample << ' ' << position[0] << ' ' << position[1] << '\n';
                    // gl_SamplePosition in normal presentation belongs to the actual
                    // draw target. Do not assume the depth-copy FBO uses its pattern.
                    glBindFramebuffer(GL_FRAMEBUFFER, frame.target);
                    glGetMultisamplefv(GL_SAMPLE_POSITION, sample, position.data());
                    target_positions << sample << ' ' << position[0] << ' ' << position[1] << '\n';
                    auto* sample_shader = sample_composite_shader(frame);
                    sample_shader->start_using();
                    sample_shader->set_uniform("capture_sample_index", sample);
                    sample_shader->set_uniform("capture_sample_position", position);
                    sample_settings.debug_view = 5;
                    draw(4, fbo, m_width, m_height, sample_frame, sample_settings);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
                    glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_FLOAT, rgba.data());
                    for (size_t i = 0; i < depth.size(); ++i) {
                        depth[i] = rgba[4 * i];
                        for (int c = 0; c < 3; ++c)
                            match[3 * i + c] = rgba[4 * i + c + 1];
                    }
                    const std::string prefix = "sample-" + std::to_string(sample);
                    std::ofstream     depth_file(capture_path / (prefix + "-depth.pfm"), std::ios::binary);
                    depth_file << "Pf\n" << m_width << ' ' << m_height << "\n-1.0\n";
                    depth_file.write(reinterpret_cast<const char*>(depth.data()), std::streamsize(depth.size() * sizeof(float)));
                    std::ofstream match_file(capture_path / (prefix + "-match.pfm"), std::ios::binary);
                    match_file << "PF\n" << m_width << ' ' << m_height << "\n-1.0\n";
                    match_file.write(reinterpret_cast<const char*>(match.data()), std::streamsize(match.size() * sizeof(float)));
                    capture_ok &= bool(depth_file) && bool(match_file);
                    sample_settings.debug_view = 6;
                    draw(4, fbo, m_width, m_height, sample_frame, sample_settings);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
                    glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_FLOAT, rgba.data());
                    for (size_t i = 0; i < depth.size(); ++i)
                        for (int c = 0; c < 3; ++c)
                            match[3 * i + c] = rgba[4 * i + c];
                    std::ofstream factor_file(capture_path / (prefix + "-factor.pfm"), std::ios::binary);
                    factor_file << "PF\n" << m_width << ' ' << m_height << "\n-1.0\n";
                    factor_file.write(reinterpret_cast<const char*>(match.data()), std::streamsize(match.size() * sizeof(float)));
                    capture_ok &= bool(factor_file);
                }
                capture_ok &= bool(positions) && bool(target_positions);
                sample_composite_shader(frame)->set_uniform("capture_sample", false);
            } else
                capture_ok = false;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, old_buffer);
        glPixelStorei(GL_PACK_ALIGNMENT, old_pack);
        glPixelStorei(GL_PACK_ROW_LENGTH, old_row);
        glPixelStorei(GL_PACK_SKIP_ROWS, old_rows);
        glPixelStorei(GL_PACK_SKIP_PIXELS, old_pixels);
        glBindRenderbuffer(GL_RENDERBUFFER, old_rbo);
        glDeleteRenderbuffers(1, &rbo);
        glDeleteFramebuffers(1, &fbo);
    }
    const bool rendered = !consume_errors();
    if (rendered && !capture_path.empty()) {
        if (capture_ok) {
            std::ofstream complete(capture_path / "capture-complete.txt", std::ios::trunc);
            complete << "AO capture complete\n" << m_width << ' ' << m_height << '\n';
            capture_ok = bool(complete);
        }
        if (!capture_ok)
            BOOST_LOG_TRIVIAL(warning) << "AO capture could not write all diagnostic files";
    }
    return rendered || fail("AO composite failed");
}

}} // namespace Slic3r::GUI
