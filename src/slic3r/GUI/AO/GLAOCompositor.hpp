#ifndef slic3r_GUI_AO_GLAOCompositor_hpp_
#define slic3r_GUI_AO_GLAOCompositor_hpp_

#include "AOConfig.hpp"
#include "AOFullscreenPass.hpp"
#include "AOObserver.hpp"
#include "AOPlan.hpp"
#include "GLAOProgramCatalog.hpp"

#include <array>
#include <string>

namespace Slic3r {
class GLShaderProgram;
namespace GUI { namespace AO {

// Final blend stage: the experimental split-MSAA classification pass, its fast composite, and
// the pixel/sample composite draws into the scene target. Owns only split helper resources.
class GLAOCompositor
{
public:
    GLAOCompositor()                                 = default;
    GLAOCompositor(const GLAOCompositor&)            = delete;
    GLAOCompositor& operator=(const GLAOCompositor&) = delete;

    // Allocates the split factor target; a failure only disables that experimental path.
    bool prepare(const AOPlan& plan, int viewport_width, int viewport_height);
    // Split classification (unit 4 output) when the route is active; emits SplitClassified.
    bool classify_surfaces(const AOPlan&                   plan,
                           const GLAOProgramCatalog::Refs& shaders,
                           const AOUniformValues&          uniforms,
                           unsigned int                    vao,
                           const AOFrameInput&             input,
                           int                             sample_count,
                           AOFrameObserver*                observer);
    // Blend equation setup shared by split fast and regular composite draws.
    void begin_composite_blend(int debug_view);
    // Split fast composite into the scene target; emits SplitComposited.
    void composite_split_fast(const AOPlan&                   plan,
                              const GLAOProgramCatalog::Refs& shaders,
                              const AOUniformValues&          uniforms,
                              unsigned int                    vao,
                              const AOFrameInput&             input,
                              int                             width,
                              int                             height,
                              bool                            multisample,
                              AOFrameObserver*                observer);
    // Regular pixel/sample composite into the scene target; emits CompositeReady.
    void composite(GLShaderProgram*       shader,
                   const AOUniformValues& uniforms,
                   unsigned int           vao,
                   const AOFrameInput&    input,
                   int                    width,
                   int                    height,
                   bool                   multisample,
                   AOFrameObserver*       observer);
    // Diagnostic-only composite redraw (debug views 4/5/6) into an arbitrary FBO.
    void composite_diagnostic(
        GLShaderProgram* shader, const AOUniformValues& uniforms, unsigned int vao, unsigned int target_fbo, int width, int height);

    unsigned int split_texture() const { return m_split_texture; }
    unsigned int split_fbo() const { return m_split_fbo; }
    bool         split_failed() const { return m_split_failed; }
    void         note_split_failed() { m_split_failed = true; }
    void         reset_health()
    {
        m_split_failed = false;
        m_failure.clear();
    }
    const std::string& failure_reason() const { return m_failure; }

    void release();
    void forget_context();

private:
    // Viewport tracking: the split target must be rebuilt when the frame resizes, matching the
    // historical full-release-and-realloc behavior on size changes.
    void cleanup_split();

    unsigned int                        m_split_texture{0}, m_split_fbo{0};
    bool                                m_split_ready{false}, m_split_failed{false};
    int                                 m_split_width{0}, m_split_height{0};
    int                                 m_split_position_samples{0};
    unsigned int                        m_split_position_target{0};
    std::array<std::array<float, 2>, 8> m_split_positions{};
    std::string                         m_failure;
};

}} // namespace GUI::AO
} // namespace Slic3r
#endif
