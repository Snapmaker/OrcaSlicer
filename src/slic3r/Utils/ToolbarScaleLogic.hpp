#ifndef slic3r_Utils_ToolbarScaleLogic_hpp_
#define slic3r_Utils_ToolbarScaleLogic_hpp_

// The 3D view's top toolbars shrink to fit the canvas width (GLCanvas3D::
// _check_and_update_toolbar_icon_scale). Kept free of wxWidgets and OpenGL so
// tests/slic3rutils/toolbar_scale_tests.cpp can cover it.

#include <cmath>

namespace Slic3r {
namespace ToolbarScale {

// stored: GUI_App::toolbar_icon_scale(), the saved auto-fit ("toolkit_size") in logical units.
// fitted: the logical scale at which the top toolbars fill the canvas, i.e. the fit computed in
//         framebuffer pixels divided by the canvas's framebuffer pixels per point (2 on Retina).
// Both must be logical: comparing `fitted` with stored * pixels-per-point (as before) made a
// stored scale of exactly half the fit look unchanged on Retina, so the toolbar stayed at
// half size until the canvas width changed a lot.
inline bool auto_scale_changed(float stored, float fitted)
{
    return std::fabs(fitted - stored) > 0.05f; // 5 % or more
}

} // namespace ToolbarScale
} // namespace Slic3r

#endif // slic3r_Utils_ToolbarScaleLogic_hpp_
