#ifndef slic3r_GUI_AO_GLAOFeature_hpp_
#define slic3r_GUI_AO_GLAOFeature_hpp_

#include "AOConfig.hpp"
#include "AOFullscreenPass.hpp"
#include "AOObserver.hpp"
#include "AOPlan.hpp"
#include "AOTypes.hpp"
#include "GLAOCompositor.hpp"
#include "GLAOComputeBackend.hpp"
#include "GLAOFragmentBackend.hpp"
#include "GLAOInputs.hpp"
#include "GLAOProgramCatalog.hpp"

#include <functional>
#include <string>

namespace Slic3r {
class GLShaderProgram;
namespace GUI { namespace AO {

// Public AO entry point: resolves the request against availability, prepares the modules,
// executes the plan (with complete-plan fallback), composites into the scene target and reports
// the actual execution facts. Owns the working modules; programs stay with GLShadersManager and
// the full-frame target stays with the canvas.
class GLAOFeature
{
public:
    GLAOFeature()                              = default;
    GLAOFeature(const GLAOFeature&)            = delete;
    GLAOFeature& operator=(const GLAOFeature&) = delete;

    enum class Preparation { Prepared, Failed, Forbidden, ContextLost };

    // Resolves the plan and prepares module resources for the frame. Does not draw. The
    // catalog is borrowed for the frame and must outlive the matching render() call.
    Preparation prepare(const AOExecutionRequest& request,
                        const AOSettings&         settings,
                        const AOFrameInput&       input,
                        const GLAOProgramCatalog& catalog,
                        AOFrameObserver*          observer);
    // Executes the prepared plan: inputs, receiver depth, evaluation, filtering, composite.
    AOResult render(const AOExecutionRequest&    request,
                    const AOSettings&            settings,
                    const AOFrameInput&          input,
                    const std::function<void()>& receiver_draw,
                    AOFrameObserver*             observer);

    const AOPlan&      plan() const { return m_plan; }
    bool               plan_valid() const { return m_prepared; }
    bool               has_resources() const;
    const std::string& visual_signature() const { return m_visual_signature; }
    const std::string& failure_reason() const { return m_failure; }
    // Reason of the last Forbidden preparation (strict requests only; not sticky).
    const std::string& forbidden_reason() const { return m_forbidden_reason; }
    // Backend routing reason of the prepared plan (capability, health or fallback).
    const std::string& backend_reason() const { return m_backend_reason; }

    void release();
    // After the old context is released/destroyed, discard its names and allow a fresh probe.
    void forget_context();

private:
    void                  publish_execution(AOFrameObserver* observer) const;
    AOBackendAvailability compute_availability(const AOExecutionRequest& request,
                                               const AOSettings&         settings,
                                               const GLAOProgramCatalog& catalog,
                                               const AOFrameInput&       input);
    // Validates the FS programs a plan needs (edge or geometry chain, upsample); mirrors the
    // historical validate_fs_shaders and MUST run before committing any FS plan, including
    // fallbacks, so render never dereferences a missing program.
    bool             validate_fs_programs(const AOFSPlan& plan, const GLAOProgramCatalog::Refs& refs);
    bool             fail(const std::string& reason);
    void             build_uniforms(const AOSettings& settings);
    GLShaderProgram* select_sample_composite_shader(const GLAOProgramCatalog::Refs& refs) const;
    GLShaderProgram* select_pixel_composite_shader(const GLAOProgramCatalog::Refs& refs) const;
    void             record_actual(const AOExecutionRequest& request, const AOSettings& settings, AOResult& result) const;

    GLAOInputs                m_inputs;
    GLAOComputeBackend        m_compute;
    GLAOFragmentBackend       m_fragment;
    GLAOCompositor            m_compositor;
    const GLAOProgramCatalog* m_catalog{nullptr};
    AOPlan                    m_plan;
    bool                      m_prepared{false};
    AOProgramIdentity         m_program_identity;
    unsigned int              m_resource_epoch{0};
    int                       m_width{0}, m_height{0};
    int                       m_prepare_width{0}, m_prepare_height{0};
    std::string               m_failure;
    std::string               m_visual_signature;
    std::string               m_forbidden_reason;
    std::string               m_backend_reason;
    bool                      m_fell_back{false};
    AOExecutionRequest        m_request_snapshot;
    AOFrameInput              m_frame_input;
    AOUniformValues           m_uniforms;
    bool                      m_pixel_active{false}, m_split_active{false}, m_sample_active{false};
    int                       m_color_sample_count{0};
    GLShaderProgram*          m_composite_shader{nullptr};
    GLShaderProgram*          m_sample_composite_shader{nullptr};
    bool                      m_last_render_clean{false};
};

}} // namespace GUI::AO
} // namespace Slic3r
#endif
