#include "AOGLUtils.hpp"

namespace Slic3r { namespace GUI { namespace AO {

bool consume_errors(bool* context_lost)
{
    bool   error = false;
    GLenum code;
    while ((code = glGetError()) != GL_NO_ERROR) {
        error = true;
        if (context_lost && code == GL_CONTEXT_LOST)
            *context_lost = true;
    }
    return error;
}

void AOGLStateGuard::forget_texture(unsigned int name)
{
    if (!name)
        return;
    for (auto& texture : m_textures)
        if (texture == GLint(name))
            texture = 0;
    if (m_sample_texture == GLint(name))
        m_sample_texture = 0;
}

void AOGLStateGuard::forget_framebuffer(unsigned int name)
{
    if (name && m_read_fbo == GLint(name))
        m_read_fbo = 0;
    if (name && m_draw_fbo == GLint(name))
        m_draw_fbo = 0;
}

void AOGLStateGuard::forget_vertex_array(unsigned int name)
{
    if (name && m_vao == GLint(name))
        m_vao = 0;
}

void ao_texture_parameters()
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

AOGLStateGuard::AOGLStateGuard()
{
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &m_read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_draw_fbo);
    glGetIntegerv(GL_VIEWPORT, m_viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &m_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &m_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &m_array_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &m_active);
    for (int i = 0; i < 5; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_textures[i]);
    }
    glActiveTexture(GL_TEXTURE3);
    if (GLEW_VERSION_4_0)
        glGetIntegerv(GL_TEXTURE_BINDING_2D_MULTISAMPLE, &m_sample_texture);
    glActiveTexture(m_active);
    glGetIntegerv(GL_DEPTH_FUNC, &m_depth_func);
    glGetIntegerv(GL_POLYGON_MODE, m_polygon_mode);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &m_depth_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, m_color_mask);
    m_multisample = glIsEnabled(GL_MULTISAMPLE);
    if (GLEW_VERSION_4_0) {
        m_sample_mask    = glIsEnabled(GL_SAMPLE_MASK);
        m_sample_shading = glIsEnabled(GL_SAMPLE_SHADING);
    }
    glGetIntegerv(GL_BLEND_SRC_RGB, &m_blend_src[0]);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &m_blend_src[1]);
    glGetIntegerv(GL_BLEND_DST_RGB, &m_blend_dst[0]);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &m_blend_dst[1]);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &m_blend_eq[0]);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &m_blend_eq[1]);
    for (size_t i = 0; i < m_caps.size(); ++i)
        m_enabled[i] = glIsEnabled(m_caps[i]);
}

void AOGLStateGuard::fullscreen() const
{
    for (GLenum cap : m_caps)
        glDisable(cap);
    if (GLEW_VERSION_4_0) {
        glDisable(GL_SAMPLE_MASK);
        glDisable(GL_SAMPLE_SHADING);
    }
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

AOGLStateGuard::~AOGLStateGuard()
{
    if (!m_restore_state)
        return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_draw_fbo);
    glViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
    glUseProgram(m_program);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_array_buffer);
    for (int i = 0; i < 5; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
    }
    glActiveTexture(GL_TEXTURE3);
    if (GLEW_VERSION_4_0)
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_sample_texture);
    glActiveTexture(m_active);
    glDepthFunc(m_depth_func);
    if (m_polygon_mode[0] == m_polygon_mode[1])
        glPolygonMode(GL_FRONT_AND_BACK, m_polygon_mode[0]);
    else {
        glPolygonMode(GL_FRONT, m_polygon_mode[0]);
        glPolygonMode(GL_BACK, m_polygon_mode[1]);
    }
    glDepthMask(m_depth_mask);
    glColorMask(m_color_mask[0], m_color_mask[1], m_color_mask[2], m_color_mask[3]);
    glBlendFuncSeparate(m_blend_src[0], m_blend_dst[0], m_blend_src[1], m_blend_dst[1]);
    glBlendEquationSeparate(m_blend_eq[0], m_blend_eq[1]);
    if (m_multisample)
        glEnable(GL_MULTISAMPLE);
    else
        glDisable(GL_MULTISAMPLE);
    if (GLEW_VERSION_4_0) {
        if (m_sample_shading)
            glEnable(GL_SAMPLE_SHADING);
        else
            glDisable(GL_SAMPLE_SHADING);
        if (m_sample_mask)
            glEnable(GL_SAMPLE_MASK);
        else
            glDisable(GL_SAMPLE_MASK);
    }
    for (size_t i = 0; i < m_caps.size(); ++i) {
        if (m_enabled[i])
            glEnable(m_caps[i]);
        else
            glDisable(m_caps[i]);
    }
}

}}} // namespace Slic3r::GUI::AO
