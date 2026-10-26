#ifndef slic3r_GLCanvasAttribs_hpp_
#define slic3r_GLCanvasAttribs_hpp_

#include <vector>

#include <wx/glcanvas.h>

namespace Slic3r {
namespace GUI {

// Zero-terminated wx attribute list of the 3D canvas pixel format. Without multisample only the
// sample buffers and samples are dropped; everything before them, the stencil buffer included, stays.
inline std::vector<int> gl_canvas_attrib_list(bool multisample)
{
    std::vector<int> attribs = {
        WX_GL_RGBA,
        WX_GL_DOUBLEBUFFER,
        // RGB channels each should be allocated with 8 bit depth. One should almost certainly get these bit depths by default.
        WX_GL_MIN_RED, 			8,
        WX_GL_MIN_GREEN, 		8,
        WX_GL_MIN_BLUE, 		8,
        // Requesting an 8 bit alpha channel. Interestingly, the NVIDIA drivers would most likely work with some alpha plane, but glReadPixels would not return
        // the alpha channel on NVIDIA if not requested when the GL context is created.
        WX_GL_MIN_ALPHA, 		8,
        WX_GL_DEPTH_SIZE, 		24,
        //BBS: turn on stencil buffer for outline
        WX_GL_STENCIL_SIZE,     8,
    };
    if (multisample) {
        // 1 is GL_TRUE
        attribs.insert(attribs.end(), { WX_GL_SAMPLE_BUFFERS, 1, WX_GL_SAMPLES, 4 });
    }
    // wx reads the list up to this terminator.
    attribs.push_back(0);
    return attribs;
}

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLCanvasAttribs_hpp_
