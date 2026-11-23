#ifndef slic3r_GUI_AO_AOGLUtils_hpp_
#define slic3r_GUI_AO_AOGLUtils_hpp_

#include <GL/glew.h>

#include <array>

namespace Slic3r { namespace GUI { namespace AO {

// Drains and reports pending GL errors; true when any error was found.
bool consume_errors(bool* context_lost = nullptr);

// NEAREST/CLAMP parameters shared by all AO 2D textures.
void ao_texture_parameters();

// Saves every piece of GL state the AO passes touch and restores it on scope exit. The AO passes
// use their own VAO and texture units 0..4 (3: MSAA depth, 4: optional confidence).
class AOGLStateGuard
{
public:
    AOGLStateGuard();
    ~AOGLStateGuard();
    AOGLStateGuard(const AOGLStateGuard&)            = delete;
    AOGLStateGuard& operator=(const AOGLStateGuard&) = delete;
    // Lost context: skip restoration, the objects are gone.
    void forget_context() { m_restore_state = false; }
    // Disables caps/masks the fullscreen passes rely on.
    void fullscreen() const;
    // Never restore names deleted during resource preparation.
    void forget_texture(unsigned int name);
    void forget_framebuffer(unsigned int name);
    void forget_vertex_array(unsigned int name);

private:
    bool  m_restore_state{true};
    GLint m_read_fbo{}, m_draw_fbo{}, m_viewport[4]{}, m_program{}, m_vao{}, m_array_buffer{}, m_active{}, m_textures[5]{},
        m_sample_texture{};
    GLint                     m_depth_func{}, m_blend_src[2]{}, m_blend_dst[2]{}, m_blend_eq[2]{}, m_polygon_mode[2]{};
    GLboolean                 m_depth_mask{}, m_color_mask[4]{}, m_multisample{}, m_sample_mask{}, m_sample_shading{};
    std::array<GLenum, 10>    m_caps{{GL_DEPTH_TEST, GL_BLEND, GL_STENCIL_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_SAMPLE_ALPHA_TO_COVERAGE,
                                      GL_SAMPLE_COVERAGE, GL_COLOR_LOGIC_OP, GL_POLYGON_OFFSET_FILL, GL_RASTERIZER_DISCARD}};
    std::array<GLboolean, 10> m_enabled{};
};

}}} // namespace Slic3r::GUI::AO
#endif
