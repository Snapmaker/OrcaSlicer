#include "AOFullscreenPass.hpp"
#include "../GLShader.hpp"

#include <GL/glew.h>

namespace Slic3r { namespace GUI { namespace AO {

void ao_draw_fullscreen(GLShaderProgram&          shader,
                        unsigned int              target,
                        int                       w,
                        int                       h,
                        unsigned int              vao,
                        const AOUniformValues&    values,
                        bool                      scene_origin,
                        bool                      multisample,
                        const std::array<int, 2>& viewport_origin)
{
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    glViewport(scene_origin ? viewport_origin[0] : 0, scene_origin ? viewport_origin[1] : 0, w, h);
    glBindVertexArray(vao);
    shader.start_using();
    shader.set_uniform("depth_texture", 0);
    shader.set_uniform("normal_texture", 1);
    shader.set_uniform("ao_texture", 2);
    shader.set_uniform("sample_depth_texture", 3);
    shader.set_uniform("edge_texture", 3);
    shader.set_uniform("confidence_texture", 4);
    shader.set_uniform("composite_factor_texture", 4);
    shader.set_uniform("capture_sample", values.capture_sample);
    shader.set_uniform("inv_projection", *values.inverse_projection);
    shader.set_uniform("projection", *values.projection);
    shader.set_uniform("full_size", std::array<int, 2>{{values.full_width, values.full_height}});
    shader.set_uniform("ao_size", std::array<int, 2>{{values.ao_width, values.ao_height}});
    shader.set_uniform("perspective", values.perspective);
    shader.set_uniform("radius", values.radius);
    shader.set_uniform("thickness", values.thickness);
    shader.set_uniform("intensity", values.intensity);
    shader.set_uniform("ao_strength", values.strength);
    shader.set_uniform("slice_count", values.slice_count);
    shader.set_uniform("step_count", values.step_count);
    shader.set_uniform("high_sampling_noise", values.high_sampling_noise);
    shader.set_uniform("debug_view", values.debug_view);
    if (multisample)
        glEnable(GL_MULTISAMPLE);
    shader.set_uniform("viewport_origin",
                       std::array<int, 2>{{scene_origin ? viewport_origin[0] : 0, scene_origin ? viewport_origin[1] : 0}});
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

}}} // namespace Slic3r::GUI::AO
