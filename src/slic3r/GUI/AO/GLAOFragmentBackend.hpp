#ifndef slic3r_GUI_AO_GLAOFragmentBackend_hpp_
#define slic3r_GUI_AO_GLAOFragmentBackend_hpp_

#include "AOConfig.hpp"
#include "AOFullscreenPass.hpp"
#include "AOObserver.hpp"
#include "AOPlan.hpp"
#include "GLAOProgramCatalog.hpp"

#include <string>

namespace Slic3r {
class GLShaderProgram;
namespace GUI { namespace AO {

// FS backend: half/full-resolution visibility evaluation, the geometry or edge-guided filter
// chain and the bilateral upsample. Working textures are owned here; the raw evaluation buffer
// stays intact for diagnostics until the next frame.
class GLAOFragmentBackend
{
public:
    GLAOFragmentBackend()                                      = default;
    GLAOFragmentBackend(const GLAOFragmentBackend&)            = delete;
    GLAOFragmentBackend& operator=(const GLAOFragmentBackend&) = delete;

    // (Re)allocates the working targets for the plan. An edge-MRT failure only disables the
    // edge path for this session (edge_failed) and the plan must be regenerated with geometry.
    bool prepare(const AOFSPlan& plan, int viewport_width, int viewport_height);
    // Evaluation + filter chain. Emits EvaluationReady/FilterFirstPass/FilterRefined with views
    // (FilterRefined carries the edge guide when the edge path ran). Returns the filtered
    // texture (evaluation size) or 0 on failure.
    unsigned int evaluate_and_filter(const AOFSPlan&                 plan,
                                     const GLAOProgramCatalog::Refs& shaders,
                                     const AOUniformValues&          uniforms,
                                     unsigned int                    vao,
                                     unsigned int                    normals_texture,
                                     AOFrameObserver*                observer);
    // Upsamples the filtered texture to full resolution when the plan evaluated at half size;
    // leaves the result bound at texture unit 2 either way and returns it.
    unsigned int upsample(const AOFSPlan&                 plan,
                          const GLAOProgramCatalog::Refs& shaders,
                          const AOUniformValues&          uniforms,
                          unsigned int                    vao,
                          unsigned int                    filtered_texture);

    unsigned int raw_texture() const { return m_raw; }
    unsigned int filtered_texture() const { return m_filtered; }
    unsigned int full_texture() const { return m_full; }
    unsigned int edge_texture() const { return m_edge; }
    unsigned int raw_fbo() const { return m_raw_fbo; }
    unsigned int filtered_fbo() const { return m_filtered_fbo; }
    unsigned int full_fbo() const { return m_full_fbo; }
    bool         edge_failed() const { return m_edge_failed; }
    void         note_edge_failed() { m_edge_failed = true; }
    void         reset_health()
    {
        m_edge_failed = false;
        m_failure.clear();
    }
    int                eval_width() const { return m_eval_width; }
    int                eval_height() const { return m_eval_height; }
    bool               full_res() const { return m_eval_width == m_full_width && m_eval_height == m_full_height; }
    const std::string& failure_reason() const { return m_failure; }

    void release();
    void forget_context();

private:
    unsigned int m_raw{0}, m_filtered{0}, m_full{0}, m_edge{0};
    unsigned int m_raw_fbo{0}, m_filtered_fbo{0}, m_full_fbo{0};
    int          m_eval_width{0}, m_eval_height{0}, m_full_width{0}, m_full_height{0};
    bool         m_edge_ready{false}, m_edge_failed{false};
    std::string  m_failure;
};

}} // namespace GUI::AO
} // namespace Slic3r
#endif
