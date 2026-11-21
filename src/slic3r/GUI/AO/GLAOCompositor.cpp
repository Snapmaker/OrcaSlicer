#include "GLAOCompositor.hpp"
#include "AOGLUtils.hpp"
#include "../GLShader.hpp"

#include <GL/glew.h>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI { namespace AO {

void GLAOCompositor::cleanup_split()
{
    glDeleteTextures(1, &m_split_texture);
    glDeleteFramebuffers(1, &m_split_fbo);
    m_split_texture = m_split_fbo = 0;
    m_split_ready                 = false;
    m_split_position_samples      = 0;
}

bool GLAOCompositor::prepare(const AOPlan& plan, int viewport_width, int viewport_height)
{
    const bool want_split = plan.composite == CompositeVariant::SplitMsaa && !m_split_failed;
    if (!want_split) {
        if (m_split_texture && plan.composite != CompositeVariant::SplitMsaa)
            cleanup_split();
        return true;
    }
    // Rebuild on viewport changes; the previous allocation matches the previous size only.
    if (m_split_texture && m_split_fbo && m_split_width == viewport_width && m_split_height == viewport_height)
        return true;
    if (m_split_texture || m_split_fbo)
        cleanup_split();
    m_split_width  = viewport_width;
    m_split_height = viewport_height;
    glGenTextures(1, &m_split_texture);
    glBindTexture(GL_TEXTURE_2D, m_split_texture);
    ao_texture_parameters();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, viewport_width, viewport_height, 0, GL_RED, GL_FLOAT, nullptr);
    glGenFramebuffers(1, &m_split_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_split_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_split_texture, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    bool       lost     = false;
    const bool error    = consume_errors(&lost);
    if (!complete || error) {
        if (lost) {
            forget_context();
            m_failure = "OpenGL context lost";
            return false;
        }
        cleanup_split();
        m_split_width = m_split_height = 0;
        m_split_failed                 = true;
        m_failure                      = "split composite allocation failed";
        BOOST_LOG_TRIVIAL(warning) << "AO split composite unavailable; using per-sample composite";
        return false;
    }
    m_split_ready = true;
    return true;
}

bool GLAOCompositor::classify_surfaces(const AOPlan&                   plan,
                                       const GLAOProgramCatalog::Refs& shaders,
                                       const AOUniformValues&          uniforms,
                                       unsigned int                    vao,
                                       const AOFrameInput&             input,
                                       int                             sample_count,
                                       AOFrameObserver*                observer)
{
    if (plan.composite != CompositeVariant::SplitMsaa || !m_split_texture || !shaders.split_classify)
        return false;
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, 0);
    auto* classify = shaders.split_classify;
    classify->start_using();
    classify->set_uniform("sample_count", sample_count);
    if (m_split_position_samples != sample_count || m_split_position_target != input.target) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, input.target);
        for (int sample = 0; sample < sample_count; ++sample)
            glGetMultisamplefv(GL_SAMPLE_POSITION, sample, m_split_positions[sample].data());
        m_split_position_samples = sample_count;
        m_split_position_target  = input.target;
    }
    for (int sample = 0; sample < sample_count; ++sample)
        classify->set_uniform(("sample_positions[" + std::to_string(sample) + "]").c_str(), m_split_positions[sample]);
    glDisable(GL_BLEND);
    ao_draw_fullscreen(*classify, m_split_fbo, uniforms.full_width, uniforms.full_height, vao, uniforms, false, false,
                       std::array<int, 2>{{0, 0}});
    if (observer) {
        AOResourceView view;
        view.kind    = AOResourceKind::Texture2D;
        view.gl_name = m_split_texture;
        view.format  = GL_R32F;
        view.width   = uniforms.full_width;
        view.height  = uniforms.full_height;
        observer->mark(AOStage::SplitClassified);
        observer->resource_ready(AOStage::SplitClassified, view);
    }
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, m_split_texture);
    return true;
}

void GLAOCompositor::begin_composite_blend(int debug_view)
{
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(debug_view == 0 ? GL_DST_COLOR : GL_ONE, GL_ZERO, GL_ZERO, GL_ONE);
}

void GLAOCompositor::composite_split_fast(const AOPlan&                   plan,
                                          const GLAOProgramCatalog::Refs& shaders,
                                          const AOUniformValues&          uniforms,
                                          unsigned int                    vao,
                                          const AOFrameInput&             input,
                                          int                             width,
                                          int                             height,
                                          bool                            multisample,
                                          AOFrameObserver*                observer)
{
    if (!shaders.split_fast)
        return;
    ao_draw_fullscreen(*shaders.split_fast, input.target, width, height, vao, uniforms, true, multisample,
                       std::array<int, 2>{{input.viewport[0], input.viewport[1]}});
    if (observer)
        observer->mark(AOStage::SplitComposited);
}

void GLAOCompositor::composite(GLShaderProgram*       shader,
                               const AOUniformValues& uniforms,
                               unsigned int           vao,
                               const AOFrameInput&    input,
                               int                    width,
                               int                    height,
                               bool                   multisample,
                               AOFrameObserver*       observer)
{
    if (!shader)
        return;
    ao_draw_fullscreen(*shader, input.target, width, height, vao, uniforms, true, multisample,
                       std::array<int, 2>{{input.viewport[0], input.viewport[1]}});
    if (observer)
        observer->mark(AOStage::CompositeReady);
}

void GLAOCompositor::composite_diagnostic(
    GLShaderProgram* shader, const AOUniformValues& uniforms, unsigned int vao, unsigned int target_fbo, int width, int height)
{
    if (!shader)
        return;
    ao_draw_fullscreen(*shader, target_fbo, width, height, vao, uniforms, false, false, std::array<int, 2>{{0, 0}});
}

void GLAOCompositor::release()
{
    cleanup_split();
    m_split_failed = false;
    m_split_width = m_split_height = 0;
    m_failure.clear();
}

void GLAOCompositor::forget_context()
{
    m_split_texture = m_split_fbo = 0;
    m_split_ready = m_split_failed = false;
    m_split_width = m_split_height = 0;
    m_split_position_samples       = 0;
    m_failure.clear();
}

}}} // namespace Slic3r::GUI::AO
