// The wx attribute list of the 3D canvas: the no-multisample fallback must only drop the sample
// buffers and samples. It used to truncate the list at an index that, after the stencil entry was
// inserted, pointed at WX_GL_STENCIL_SIZE, so the canvas silently lost its stencil buffer.
#include <catch2/catch_test_macros.hpp>

#include <wx/glcanvas.h>

#include <vector>

#include "slic3r/GUI/GLCanvasAttribs.hpp"

using Slic3r::GUI::gl_canvas_attrib_list;

namespace {

// Value following `key` in a wx attribute list, or -1 when the key is absent before the terminator.
int attrib_value(const std::vector<int>& attribs, int key)
{
    for (size_t i = 0; i + 1 < attribs.size() && attribs[i] != 0; ++i)
        if (attribs[i] == key)
            return attribs[i + 1];
    return -1;
}

} // namespace

TEST_CASE("GL canvas attribute list with multisample", "[GLCanvas]")
{
    const std::vector<int> attribs = gl_canvas_attrib_list(true);
    const std::vector<int> expected = {
        WX_GL_RGBA, WX_GL_DOUBLEBUFFER,
        WX_GL_MIN_RED, 8, WX_GL_MIN_GREEN, 8, WX_GL_MIN_BLUE, 8, WX_GL_MIN_ALPHA, 8,
        WX_GL_DEPTH_SIZE, 24, WX_GL_STENCIL_SIZE, 8,
        WX_GL_SAMPLE_BUFFERS, 1, WX_GL_SAMPLES, 4,
        0};
    CHECK(attribs == expected);
}

TEST_CASE("GL canvas attribute list without multisample keeps the stencil buffer", "[GLCanvas]")
{
    const std::vector<int> attribs = gl_canvas_attrib_list(false);
    REQUIRE(!attribs.empty());
    CHECK(attribs.back() == 0);
    CHECK(attrib_value(attribs, WX_GL_STENCIL_SIZE) == 8);
    CHECK(attrib_value(attribs, WX_GL_DEPTH_SIZE) == 24);
    CHECK(attrib_value(attribs, WX_GL_MIN_ALPHA) == 8);
    CHECK(attrib_value(attribs, WX_GL_SAMPLE_BUFFERS) == -1);
    CHECK(attrib_value(attribs, WX_GL_SAMPLES) == -1);

    // Exactly the multisample list minus the sample entries.
    std::vector<int> with_samples = gl_canvas_attrib_list(true);
    with_samples.erase(with_samples.end() - 5, with_samples.end() - 1);
    CHECK(attribs == with_samples);
}
