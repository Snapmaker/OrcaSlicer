#ifndef slic3r_GUI_AO_AOFullscreenPass_hpp_
#define slic3r_GUI_AO_AOFullscreenPass_hpp_

#include "AOTypes.hpp"

#include <array>

namespace Slic3r {
class GLShaderProgram;
namespace GUI { namespace AO {

// Uniform bundle shared by every AO fullscreen stage. Values mirror the historical per-stage
// shader interface; intensity follows the plan response (CS composites at 1.0, FS keeps the
// parameter) and slice/step counts always use the FS sampling table.
struct AOUniformValues
{
    const Matrix4d* projection{nullptr};
    const Matrix4d* inverse_projection{nullptr};
    bool            perspective{true};
    int             full_width{0}, full_height{0};
    int             ao_width{0}, ao_height{0};
    float           radius{2.5f}, thickness{1.0f};
    float           intensity{1.0f}, strength{0.5f};
    int             slice_count{3}, step_count{8};
    bool            high_sampling_noise{false};
    int             debug_view{0};
    bool            capture_sample{false};
};

// Binds the target, viewport and VAO, applies the common uniforms and draws the fullscreen
// triangle. scene_origin places the viewport at the frame viewport origin (composite stages);
// multisample enables GL_MULTISAMPLE for sample-aware composite draws.
void ao_draw_fullscreen(GLShaderProgram&          shader,
                        unsigned int              target,
                        int                       w,
                        int                       h,
                        unsigned int              vao,
                        const AOUniformValues&    values,
                        bool                      scene_origin,
                        bool                      multisample,
                        const std::array<int, 2>& viewport_origin);

}} // namespace GUI::AO
} // namespace Slic3r
#endif
