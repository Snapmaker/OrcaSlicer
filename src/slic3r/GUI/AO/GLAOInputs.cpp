#include "GLAOInputs.hpp"
#include "AOGLUtils.hpp"
#include "../GLShader.hpp"

#include <GL/glew.h>
#include <array>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI { namespace AO {

bool GLAOInputs::fail(const std::string& reason)
{
    if (m_failure != "OpenGL context lost")
        m_failure = reason;
    return false;
}

bool GLAOInputs::prepare(int width, int height, bool with_confidence)
{
    const bool need_confidence    = with_confidence && !m_confidence_failed;
    const bool confidence_matches = need_confidence == (m_confidence_texture != 0);
    if (m_normals && m_normals_fbo && m_width == width && m_height == height && confidence_matches)
        return true; // Resources match; no reallocation.
    m_width  = width;
    m_height = height;
    if (!m_vao)
        glGenVertexArrays(1, &m_vao);

    if (m_confidence_texture && !need_confidence) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_normals_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glDeleteTextures(1, &m_confidence_texture);
        m_confidence_texture = 0;
    }

    glActiveTexture(GL_TEXTURE0);
    if (!m_normals) {
        glGenTextures(1, &m_normals);
        glBindTexture(GL_TEXTURE_2D, m_normals);
        ao_texture_parameters();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, m_width, m_height, 0, GL_RG, GL_FLOAT, nullptr);
    } else {
        glBindTexture(GL_TEXTURE_2D, m_normals);
        ao_texture_parameters();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, m_width, m_height, 0, GL_RG, GL_FLOAT, nullptr);
        ++m_generation;
    }
    if (!m_normals_fbo)
        glGenFramebuffers(1, &m_normals_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_normals_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_normals, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    bool lost     = false;
    bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (consume_errors(&lost) || !complete) {
        if (lost)
            forget_context();
        else
            release();
        return fail(lost ? "OpenGL context lost" : "AO texture allocation or framebuffer validation failed");
    }
    if (need_confidence && !m_confidence_texture) {
        glGenTextures(1, &m_confidence_texture);
        glBindTexture(GL_TEXTURE_2D, m_confidence_texture);
        ao_texture_parameters();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, m_width, m_height, 0, GL_RED, GL_FLOAT, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_confidence_texture, 0);
        const GLenum buffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(2, buffers);
        complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (consume_errors(&lost) || !complete) {
            if (lost) {
                forget_context();
                return fail("OpenGL context lost");
            }
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glDeleteTextures(1, &m_confidence_texture);
            m_confidence_texture = 0;
            m_confidence_failed  = true;
            complete             = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            if (consume_errors(&lost) || !complete) {
                if (lost)
                    forget_context();
                else
                    release();
                return fail(lost ? "OpenGL context lost" : "AO normal target recovery failed");
            }
            m_failure.clear();
            return false; // Common inputs survived; the feature must choose a direct composite.
        }
    }
    m_failure.clear();
    return true;
}

bool GLAOInputs::allocate_depth(unsigned int format)
{
    m_depth_format = format;
    glDeleteTextures(1, &m_depth);
    glDeleteFramebuffers(1, &m_depth_fbo);
    glGenTextures(1, &m_depth);
    glGenFramebuffers(1, &m_depth_fbo);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_depth);
    ao_texture_parameters();
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
    bool       lost     = false;
    const bool error    = consume_errors(&lost);
    if (lost)
        return fail("OpenGL context lost");
    return !error && complete;
}

bool GLAOInputs::capture_depth(const AOFrameInput& input)
{
    glBindFramebuffer(GL_FRAMEBUFFER, input.source);
    GLint samples = 0;
    glGetIntegerv(GL_SAMPLES, &samples);
    if (m_source != input.source || m_samples != samples)
        m_depth_validated = false;
    m_source  = input.source;
    m_samples = samples;
    auto copy = [&]() {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, input.source);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_depth_fbo);
        glBlitFramebuffer(input.viewport[0], input.viewport[1], input.viewport[0] + m_width, input.viewport[1] + m_height, 0, 0, m_width,
                          m_height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        bool       lost  = false;
        const bool error = consume_errors(&lost);
        if (lost)
            fail("OpenGL context lost");
        return !error;
    };
    if (m_depth_validated)
        return copy() || fail("scene depth blit failed");
    GLint        bits = 0, type = GL_UNSIGNED_NORMALIZED;
    const GLenum attachment = input.source == 0 ? GL_DEPTH : GL_DEPTH_ATTACHMENT;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &bits);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &type);
    bool       lost  = false;
    const bool error = consume_errors(&lost);
    if (lost)
        return fail("OpenGL context lost");
    if (error || bits == 0)
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
        if (m_failure == "OpenGL context lost")
            return false;
    }
    return fail("no matching depth resolve format");
}

bool GLAOInputs::capture_sample_depth(const AOFrameInput& input, bool sample_path_allowed)
{
    // Never index a window's colors with sample IDs from a copied texture. Strict sample
    // operations require a canvas-owned MSAA target of known layout.
    if (!sample_path_allowed || !GLEW_VERSION_4_0 || m_samples <= 1 || input.source == 0 || input.source != input.target || m_sample_failed)
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
        bool       lost     = false;
        const bool error    = consume_errors(&lost);
        if (!complete || error) {
            if (lost)
                return fail("OpenGL context lost");
            m_sample_failed = true;
            BOOST_LOG_TRIVIAL(warning) << "AO sample depth unavailable; using resolved composite";
            return false;
        }
        m_sample_count = m_samples;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, input.source);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_sample_fbo);
    glBlitFramebuffer(input.viewport[0], input.viewport[1], input.viewport[0] + m_width, input.viewport[1] + m_height, 0, 0, m_width,
                      m_height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    bool lost = false;
    if (consume_errors(&lost)) {
        if (lost)
            return fail("OpenGL context lost");
        m_sample_failed = true;
        BOOST_LOG_TRIVIAL(warning) << "AO sample depth copy failed; using resolved composite";
        return false;
    }
    m_sample_active = true;
    return true;
}

void GLAOInputs::bind_resolved_receiver_target()
{
    glBindFramebuffer(GL_FRAMEBUFFER, m_depth_fbo);
    glViewport(0, 0, m_width, m_height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glBindVertexArray(m_vao);
}

void GLAOInputs::bind_sample_receiver_target()
{
    glBindFramebuffer(GL_FRAMEBUFFER, m_sample_fbo);
    glEnable(GL_MULTISAMPLE);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_sample_depth);
}

bool GLAOInputs::draw_normals(GLShaderProgram* shader, const AOUniformValues& uniforms)
{
    if (!shader)
        return false;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_depth);
    // Do not bind a render target as an input, even to an unused sampler.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, 0); // Never leave the confidence output bound as input.
    ao_draw_fullscreen(*shader, m_normals_fbo, m_width, m_height, m_vao, uniforms, false, false, std::array<int, 2>{{0, 0}});
    if (m_confidence_texture) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, m_confidence_texture);
    }
    return true;
}

void GLAOInputs::release()
{
    glDeleteTextures(1, &m_normals);
    glDeleteFramebuffers(1, &m_normals_fbo);
    m_normals = m_normals_fbo = 0;
    glDeleteTextures(1, &m_confidence_texture);
    m_confidence_texture = 0;
    glDeleteTextures(1, &m_depth);
    glDeleteFramebuffers(1, &m_depth_fbo);
    m_depth = m_depth_fbo = m_depth_format = 0;
    glDeleteTextures(1, &m_sample_depth);
    glDeleteFramebuffers(1, &m_sample_fbo);
    m_sample_depth = m_sample_fbo = 0;
    glDeleteVertexArrays(1, &m_vao);
    m_vao   = 0;
    m_width = m_height = 0;
    m_depth_validated  = false;
    m_samples          = -1;
    m_sample_count     = 0;
    m_sample_active = m_sample_failed = false;
    m_failure.clear();
    ++m_generation;
}

void GLAOInputs::forget_context()
{
    m_vao = m_depth_fbo = m_depth = m_depth_format = 0;
    m_sample_fbo = m_sample_depth = 0;
    m_sample_count                = 0;
    m_sample_active = m_sample_failed = false;
    m_normals = m_normals_fbo = m_confidence_texture = 0;
    m_confidence_failed                              = false;
    m_width = m_height = 0;
    m_source           = 0;
    m_samples          = -1;
    m_depth_validated  = false;
    m_failure.clear();
    ++m_generation;
}

}}} // namespace Slic3r::GUI::AO
