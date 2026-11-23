#include "GLAOFragmentBackend.hpp"
#include "AOGLUtils.hpp"
#include "../GLShader.hpp"

#include <GL/glew.h>
#include <array>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI { namespace AO {

bool GLAOFragmentBackend::prepare(const AOFSPlan& plan, int viewport_width, int viewport_height)
{
    const bool size_matches = m_raw && m_filtered && m_full && m_eval_width == plan.eval_width && m_eval_height == plan.eval_height &&
                              m_full_width == viewport_width && m_full_height == viewport_height;
    const bool edge_matches = m_edge_ready == (plan.edge_filter && !m_edge_failed);
    if (size_matches && edge_matches)
        return true;

    m_eval_width  = plan.eval_width;
    m_eval_height = plan.eval_height;
    m_full_width  = viewport_width;
    m_full_height = viewport_height;
    m_failure.clear();

    const bool want_edge = plan.edge_filter && !m_edge_failed;

    glActiveTexture(GL_TEXTURE0);
    std::array<unsigned int, 3> textures{{m_raw, m_filtered, m_full}};
    glDeleteTextures(3, textures.data());
    std::array<unsigned int, 3> fbos{{m_raw_fbo, m_filtered_fbo, m_full_fbo}};
    glDeleteFramebuffers(3, fbos.data());
    glDeleteTextures(1, &m_edge);
    m_edge = 0;
    glGenTextures(3, textures.data());
    glGenFramebuffers(3, fbos.data());
    m_raw                             = textures[0];
    m_filtered                        = textures[1];
    m_full                            = textures[2];
    m_raw_fbo                         = fbos[0];
    m_filtered_fbo                    = fbos[1];
    m_full_fbo                        = fbos[2];
    const std::array<int, 2> sizes[3] = {{m_eval_width, m_eval_height}, {m_eval_width, m_eval_height}, {m_full_width, m_full_height}};
    for (int i = 0; i < 3; ++i) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        ao_texture_parameters();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, sizes[i][0], sizes[i][1], 0, GL_RED, GL_FLOAT, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[i], 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        bool       lost     = false;
        const bool error    = consume_errors(&lost);
        if (!complete || error) {
            if (lost)
                forget_context();
            else
                release();
            m_failure = lost ? "OpenGL context lost" : "AO texture allocation or framebuffer validation failed";
            return false;
        }
    }
    m_edge_ready = false;
    if (want_edge) {
        glGenTextures(1, &m_edge);
        glBindTexture(GL_TEXTURE_2D, m_edge);
        ao_texture_parameters();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_full_width, m_full_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, m_raw_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_edge, 0);
        const GLenum buffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(2, buffers);
        const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        bool       lost     = false;
        const bool error    = consume_errors(&lost);
        if (!complete || error) {
            if (lost) {
                forget_context();
                m_failure = "OpenGL context lost";
                return false;
            }
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glDeleteTextures(1, &m_edge);
            m_edge        = 0;
            m_edge_ready  = false;
            m_edge_failed = true;
            BOOST_LOG_TRIVIAL(warning) << "FS edge filter unavailable; retaining geometry filter";
            return false; // The caller must regenerate and validate the geometry plan.
        }
        m_edge_ready = true;
    }
    return true;
}

unsigned int GLAOFragmentBackend::evaluate_and_filter(const AOFSPlan&                 plan,
                                                      const GLAOProgramCatalog::Refs& shaders,
                                                      const AOUniformValues&          uniforms,
                                                      unsigned int                    vao,
                                                      unsigned int                    normals_texture,
                                                      AOFrameObserver*                observer)
{
    // Unit 3 held the previous frame's guide; never sample an attached output.
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, normals_texture);
    GLShaderProgram* evaluate = plan.edge_filter ? (plan.fast_reconstruction ? shaders.edge_evaluate_xy : shaders.edge_evaluate) :
                                                   shaders.evaluate;
    ao_draw_fullscreen(*evaluate, m_raw_fbo, m_eval_width, m_eval_height, vao, uniforms, false, false, std::array<int, 2>{{0, 0}});
    if (observer) {
        AOResourceView view;
        view.kind             = AOResourceKind::Texture2D;
        view.gl_name          = m_raw;
        view.role             = AOResourceRole::RawAO;
        view.format           = GL_R8;
        view.width            = m_eval_width;
        view.height           = m_eval_height;
        view.ao_encoding      = AOEncoding::R8Visibility;
        view.response_applied = false;
        observer->mark(AOStage::EvaluationReady);
        observer->resource_ready(AOStage::EvaluationReady, view);
    }
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_raw);
    GLShaderProgram* filter = plan.edge_filter ? shaders.edge_denoise : shaders.filter_geometry;
    if (plan.edge_filter) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, m_edge);
        glActiveTexture(GL_TEXTURE2);
    }
    filter->start_using();
    filter->set_uniform("filter_refine", false);
    ao_draw_fullscreen(*filter, m_filtered_fbo, m_eval_width, m_eval_height, vao, uniforms, false, false, std::array<int, 2>{{0, 0}});
    if (observer) {
        AOResourceView view;
        view.kind             = AOResourceKind::Texture2D;
        view.gl_name          = m_filtered;
        view.role             = AOResourceRole::DenoiseIntermediate;
        view.format           = GL_R8;
        view.width            = m_eval_width;
        view.height           = m_eval_height;
        view.ao_encoding      = AOEncoding::R8Visibility;
        view.response_applied = false;
        observer->mark(AOStage::FilterFirstPass);
        observer->resource_ready(AOStage::FilterFirstPass, view);
    }
    if (full_res()) {
        // Refine only full-resolution output. The raw buffer stays intact for diagnostics;
        // the otherwise unused upsample target holds the result.
        glBindTexture(GL_TEXTURE_2D, m_filtered);
        const bool third_pass = plan.edge_filter && plan.edge_passes == 3;
        filter->set_uniform("filter_refine", !third_pass);
        ao_draw_fullscreen(*filter, m_full_fbo, m_full_width, m_full_height, vao, uniforms, false, false, std::array<int, 2>{{0, 0}});
        if (observer) {
            if (third_pass) {
                AOResourceView second;
                second.gl_name = m_full;
                second.role    = AOResourceRole::DenoiseIntermediate;
                second.format  = GL_R8;
                second.width   = m_full_width;
                second.height  = m_full_height;
                observer->mark(AOStage::FilterThirdPass);
                observer->resource_ready(AOStage::FilterThirdPass, second);
            }
        }
        if (third_pass) {
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, m_full);
            filter->set_uniform("filter_refine", true);
            ao_draw_fullscreen(*filter, m_filtered_fbo, m_full_width, m_full_height, vao, uniforms, false, false,
                               std::array<int, 2>{{0, 0}});
        }
        if (observer) {
            if (m_edge) {
                AOResourceView view;
                view.kind        = AOResourceKind::Texture2D;
                view.gl_name     = m_edge;
                view.role        = AOResourceRole::Edges;
                view.format      = GL_RGBA8;
                view.width       = m_full_width;
                view.height      = m_full_height;
                view.ao_encoding = AOEncoding::R8Visibility;
                observer->mark(AOStage::FilterRefined);
                observer->resource_ready(AOStage::FilterRefined, view);
            } else
                observer->mark(AOStage::FilterRefined);
        }
        filter->set_uniform("filter_refine", false);
        return third_pass ? m_filtered : m_full;
    }
    return m_filtered;
}

unsigned int GLAOFragmentBackend::upsample(const AOFSPlan&                 plan,
                                           const GLAOProgramCatalog::Refs& shaders,
                                           const AOUniformValues&          uniforms,
                                           unsigned int                    vao,
                                           unsigned int                    filtered_texture)
{
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, filtered_texture);
    if (!plan.upsample || !shaders.upsample)
        return filtered_texture;
    ao_draw_fullscreen(*shaders.upsample, m_full_fbo, m_full_width, m_full_height, vao, uniforms, false, false, std::array<int, 2>{{0, 0}});
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_full);
    return m_full;
}

void GLAOFragmentBackend::release()
{
    const std::array<unsigned int, 3> textures{{m_raw, m_filtered, m_full}};
    glDeleteTextures(3, textures.data());
    const std::array<unsigned int, 3> fbos{{m_raw_fbo, m_filtered_fbo, m_full_fbo}};
    glDeleteFramebuffers(3, fbos.data());
    glDeleteTextures(1, &m_edge);
    m_raw = m_filtered = m_full = m_edge = 0;
    m_raw_fbo = m_filtered_fbo = m_full_fbo = 0;
    m_eval_width = m_eval_height = m_full_width = m_full_height = 0;
    m_edge_ready                                                = false;
    m_failure.clear();
}

void GLAOFragmentBackend::forget_context()
{
    m_raw = m_filtered = m_full = m_edge = 0;
    m_raw_fbo = m_filtered_fbo = m_full_fbo = 0;
    m_eval_width = m_eval_height = m_full_width = m_full_height = 0;
    m_edge_ready = m_edge_failed = false;
    m_failure.clear();
}

}}} // namespace Slic3r::GUI::AO
