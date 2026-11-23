#include "GLAOReadback.hpp"
#include "../AOGLUtils.hpp"
#include "../../GLShader.hpp"

#include <GL/glew.h>
#include <cmath>
#include <memory>

namespace Slic3r { namespace GUI { namespace AO { namespace AOReadback {

namespace {
thread_local bool readback_context_lost = false;
// Saves/restores the pixel-pack state modified by direct reads.
struct PackState
{
    GLint pack{4}, buffer{0}, row{0}, rows{0}, pixels{0};
    PackState()
    {
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &buffer);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &row);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &rows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &pixels);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    }
    ~PackState()
    {
        if (readback_context_lost)
            return;
        glPixelStorei(GL_PACK_ALIGNMENT, pack);
        glPixelStorei(GL_PACK_ROW_LENGTH, row);
        glPixelStorei(GL_PACK_SKIP_ROWS, rows);
        glPixelStorei(GL_PACK_SKIP_PIXELS, pixels);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
    }
};
} // namespace

bool consume_errors()
{
    bool       lost  = false;
    const bool error = AO::consume_errors(&lost);
    readback_context_lost |= lost;
    return error;
}
bool context_lost() { return readback_context_lost; }
void reset_context_loss() { readback_context_lost = false; }

bool read_color(std::vector<unsigned char>& pixels, unsigned int target, const std::array<int, 4>& viewport)
{
    AOGLStateGuard saved;
    saved.fullscreen();
    GLint old_rbo = 0, old_read_buffer = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_rbo);
    const GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    GLint color_buffer = 0;
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
    pixels.assign(size_t(viewport[2]) * viewport[3] * 3, 0);
    bool read_error = false;
    {
        PackState pack;
        if (complete)
            glReadPixels(0, 0, viewport[2], viewport[3], GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
        read_error = consume_errors();
    }
    if (readback_context_lost) {
        saved.forget_context();
        return false;
    }
    glBindRenderbuffer(GL_RENDERBUFFER, old_rbo);
    glDeleteRenderbuffers(1, &rbo);
    glDeleteFramebuffers(1, &fbo);
    if (readback_context_lost) {
        saved.forget_context();
        return false;
    }
    if (srgb)
        glEnable(GL_FRAMEBUFFER_SRGB);
    return complete && !read_error && !consume_errors() && !pixels.empty();
}

bool read_texture(std::vector<unsigned char>& pixels, unsigned int texture, int w, int h, int channel)
{
    pixels.assign(size_t(w) * h, 0);
    GLint old_active = 0, old_texture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    bool ok = true;
    {
        PackState pack;
        glBindTexture(GL_TEXTURE_2D, texture);
        if (channel < 0)
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, pixels.data());
        else {
            std::vector<unsigned char> rgba(pixels.size() * 4);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            for (size_t i = 0; i < pixels.size(); ++i)
                pixels[i] = rgba[i * 4 + channel];
        }
        ok = !consume_errors();
    }
    if (!readback_context_lost) {
        glBindTexture(GL_TEXTURE_2D, old_texture);
        glActiveTexture(old_active);
    }
    return ok;
}

bool read_depth(std::vector<float>& pixels, unsigned int fbo, int w, int h)
{
    pixels.assign(size_t(w) * h, 0.0f);
    GLint old_read = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    bool ok = true;
    {
        PackState pack;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glReadPixels(0, 0, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, pixels.data());
        ok = !consume_errors();
    }
    if (!readback_context_lost)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
    return ok;
}

bool read_normals(std::vector<float>& normals, unsigned int texture, int w, int h, const std::vector<float>& depth)
{
    if (depth.size() != size_t(w) * h)
        return false;
    PackState pack;
    GLint     old_texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    std::vector<float> encoded(size_t(w) * h * 2);
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_FLOAT, encoded.data());
    const bool error = consume_errors();
    if (!readback_context_lost)
        glBindTexture(GL_TEXTURE_2D, old_texture);
    if (error)
        return false;
    normals.assign(size_t(w) * h * 3, 0.0f);
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
    return true;
}

bool read_compute_texture(std::vector<unsigned char>& pixels, unsigned int texture, int w, int h, bool integer)
{
    pixels.assign(size_t(w) * h, 0);
    // Image-store outputs need the update barrier before a glGetTexImage read, matching the
    // consumer-side synchronization the compute passes themselves use.
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    GLint old_active = 0, old_texture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    bool ok = true;
    {
        PackState pack;
        glBindTexture(GL_TEXTURE_2D, texture);
        glGetTexImage(GL_TEXTURE_2D, 0, integer ? GL_RED_INTEGER : GL_RED, GL_UNSIGNED_BYTE, pixels.data());
        ok = !consume_errors();
    }
    if (!readback_context_lost) {
        glBindTexture(GL_TEXTURE_2D, old_texture);
        glActiveTexture(old_active);
    }
    return ok;
}

bool read_compute_mip(std::vector<float>& pixels, unsigned int texture, int level, int w, int h)
{
    pixels.assign(size_t(w) * h, 0.0f);
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    GLint old_active = 0, old_texture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    bool ok = true;
    {
        PackState pack;
        glBindTexture(GL_TEXTURE_2D, texture);
        glGetTexImage(GL_TEXTURE_2D, level, GL_RED, GL_FLOAT, pixels.data());
        ok = !consume_errors();
    }
    if (!readback_context_lost) {
        glBindTexture(GL_TEXTURE_2D, old_texture);
        glActiveTexture(old_active);
    }
    return ok;
}

bool read_composite_bytes(std::vector<unsigned char>& pixels, unsigned int fbo, int w, int h)
{
    pixels.assign(size_t(w) * h, 0);
    GLint old_read = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    bool ok = true;
    {
        PackState pack;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glReadPixels(0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, pixels.data());
        ok = !consume_errors();
    }
    if (!readback_context_lost)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
    return ok;
}

bool read_composite_floats(std::vector<float>& pixels, unsigned int fbo, int w, int h)
{
    pixels.assign(size_t(w) * h * 4, 0.0f);
    GLint old_read = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    bool ok = true;
    {
        PackState pack;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, pixels.data());
        ok = !consume_errors();
    }
    if (!readback_context_lost)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
    return ok;
}

SampleColorReader::SampleColorReader(unsigned int target, const std::array<int, 4>& viewport, int samples, unsigned int vao)
{
    AOGLStateGuard saved;
    saved.fullscreen();
    m_srgb       = glIsEnabled(GL_FRAMEBUFFER_SRGB) != GL_FALSE;
    m_vao        = vao;
    m_viewport_w = viewport[2];
    m_viewport_h = viewport[3];
    m_samples    = samples;
    glDisable(GL_FRAMEBUFFER_SRGB);
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
    m_linear_source = color_encoding == GL_LINEAR;
    glGenFramebuffers(2, m_fbo);
    glGenTextures(2, m_texture);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_texture[0]);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_RGBA8, viewport[2], viewport[3], GL_TRUE);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo[0]);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, m_texture[0], 0);
    bool ok = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok)
        glBlitFramebuffer(viewport[0], viewport[1], viewport[0] + viewport[2], viewport[1] + viewport[3], 0, 0, viewport[2], viewport[3],
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glReadBuffer(old_read);
    glBindTexture(GL_TEXTURE_2D, m_texture[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, viewport[2], viewport[3], 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[1]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture[1], 0);
    ok &= glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    auto                           shader = std::make_unique<GLShaderProgram>();
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
    ok &= shader->init_from_texts("ao_capture_sample_color", sources);
    m_shader = shader.release();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo[0]);
    if (ok) {
        m_shader->start_using();
        m_shader->set_uniform("colors", 3);
        glBindVertexArray(m_vao);
        glViewport(0, 0, viewport[2], viewport[3]);
    } else
        consume_errors();
    const bool error = consume_errors();
    m_valid          = ok && !error;
    if (readback_context_lost) {
        saved.forget_context();
        return;
    }
    if (m_srgb)
        glEnable(GL_FRAMEBUFFER_SRGB);
}

bool SampleColorReader::read(int sample, std::vector<float>& pixels)
{
    if (!m_valid || readback_context_lost)
        return false;
    AOGLStateGuard saved;
    saved.fullscreen();
    const bool srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB) != GL_FALSE;
    glDisable(GL_FRAMEBUFFER_SRGB);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_texture[0]);
    pixels.assign(size_t(m_viewport_w) * m_viewport_h * 3, 0.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[1]);
    m_shader->start_using();
    m_shader->set_uniform("sample_index", sample);
    glBindVertexArray(m_vao);
    glViewport(0, 0, m_viewport_w, m_viewport_h);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    bool ok = true;
    {
        PackState pack;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo[1]);
        glReadPixels(0, 0, m_viewport_w, m_viewport_h, GL_RGB, GL_FLOAT, pixels.data());
        ok = !consume_errors();
    }
    if (readback_context_lost) {
        saved.forget_context();
        return false;
    }
    if (srgb)
        glEnable(GL_FRAMEBUFFER_SRGB);
    return ok;
}

SampleColorReader::~SampleColorReader()
{
    if (readback_context_lost) {
        if (m_shader) {
            m_shader->forget_context();
            delete m_shader;
        }
        return;
    }
    if (m_shader)
        delete m_shader;
    glDeleteFramebuffers(2, m_fbo);
    glDeleteTextures(2, m_texture);
    consume_errors();
}

}}}} // namespace Slic3r::GUI::AO::AOReadback
