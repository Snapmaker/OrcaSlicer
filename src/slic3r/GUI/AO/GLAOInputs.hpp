#ifndef slic3r_GUI_AO_GLAOInputs_hpp_
#define slic3r_GUI_AO_GLAOInputs_hpp_

#include "AOConfig.hpp"
#include "AOFullscreenPass.hpp"
#include "AOTypes.hpp"

#include <functional>
#include <string>

namespace Slic3r {
class GLShaderProgram;
namespace GUI { namespace AO {

// Common per-frame inputs: the resolved scene depth, the multisampled depth copy, the view
// normal reconstruction target (optionally with a confidence MRT) and the shared fullscreen VAO.
// Owns targets and GL state for receiver draws; receiver callbacks only draw geometry.
class GLAOInputs
{
public:
    GLAOInputs()                             = default;
    GLAOInputs(const GLAOInputs&)            = delete;
    GLAOInputs& operator=(const GLAOInputs&) = delete;

    // Allocates the VAO and the normals target for the given size; with_confidence adds the
    // confidence MRT. A failed confidence allocation only disables that path (confidence_failed).
    bool prepare(int width, int height, bool with_confidence);
    // Resolved depth: blits the scene depth and validates/copies the format once per source.
    bool capture_depth(const AOFrameInput& input);
    // Multisampled depth copy for per-sample evaluation; consults the plan's sample path.
    bool capture_sample_depth(const AOFrameInput& input, bool sample_path_allowed);
    void bind_resolved_receiver_target();
    void bind_sample_receiver_target(); // Binds the MSAA depth FBO and the sample texture at unit 3.
    // Normal reconstruction (unit 0: resolved depth, unit 4 afterwards: confidence when active).
    bool draw_normals(GLShaderProgram* shader, const AOUniformValues& uniforms);

    unsigned int vao() const { return m_vao; }
    unsigned int depth_fbo() const { return m_depth_fbo; }
    unsigned int depth_texture() const { return m_depth; }
    unsigned int sample_fbo() const { return m_sample_fbo; }
    unsigned int sample_depth_texture() const { return m_sample_depth; }
    bool         sample_depth_active() const { return m_sample_active; }
    int          sample_count() const { return m_sample_count; }
    unsigned int normals_fbo() const { return m_normals_fbo; }
    unsigned int normals_texture() const { return m_normals; }
    unsigned int confidence_texture() const { return m_confidence_texture; }
    bool         confidence_active() const { return m_confidence_texture != 0; }
    int          width() const { return m_width; }
    int          height() const { return m_height; }
    int          source_samples() const { return m_samples; }
    unsigned int depth_format() const { return m_depth_format; }

    bool confidence_failed() const { return m_confidence_failed; }
    bool sample_depth_failed() const { return m_sample_failed; }
    void note_confidence_failed() { m_confidence_failed = true; }
    void reset_health()
    {
        m_confidence_failed = m_sample_failed = false;
        m_failure.clear();
    }
    const std::string& failure_reason() const { return m_failure; }

    void         release();
    void         forget_context();
    unsigned int generation() const { return m_generation; }
    const void*  context() const { return m_context; }
    void         set_context(const void* context) { m_context = context; }

private:
    bool fail(const std::string& reason);
    bool allocate_depth(unsigned int format);

    unsigned int m_vao{0};
    unsigned int m_depth_fbo{0}, m_depth{0}, m_depth_format{0};
    unsigned int m_sample_fbo{0}, m_sample_depth{0};
    int          m_sample_count{0};
    bool         m_sample_active{false}, m_sample_failed{false};
    unsigned int m_normals_fbo{0}, m_normals{0};
    unsigned int m_confidence_texture{0};
    bool         m_confidence_failed{false};
    int          m_width{0}, m_height{0};
    unsigned int m_source{0};
    int          m_samples{-1};
    bool         m_depth_validated{false};
    std::string  m_failure;
    unsigned int m_generation{0};
    const void*  m_context{nullptr};
};

}} // namespace GUI::AO
} // namespace Slic3r
#endif
