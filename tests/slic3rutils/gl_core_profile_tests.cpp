// Headless smoke test for the OpenGL core-profile switch (libslic3r_gui, libvgcode stage 2).
//
// Creates a hidden, never-focused GLFW window with a forward-compatible 3.3 core context - the kind
// of context macOS hands out - and runs the real start-up path on it: OpenGLManager::init_gl()
// (GLAD, the default VAO, every shader in resources/shaders/140), then draws into an off-screen
// framebuffer the three ways the app does and reads the pixels back:
//   * GLModel (its own VAO, then the default VAO bound back),
//   * a plain VBO draw with no VAO of its own (what the legacy G-code viewer does),
//   * the dashed_thick_lines geometry shader that replaces wide and stippled lines.
// Each step must leave the GL error flag clean. Where no GL context can be made (a headless CI box
// without a display) the test says so and passes.
// libvgcode stage 3 adds a fourth draw: a small G-code (processor -> LibVGCodeWrapper -> libvgcode)
// rendered top-down off screen, then libvgcode shut down, after which the app's GL must still work
// (libvgcode shares the GLAD loader and must not unload it).

#include <catch2/catch.hpp>

#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/LibVGCode/LibVGCodeWrapper.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <sstream>

#include <array>
#include <memory>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {

struct HiddenCoreContext
{
    GLFWwindow* window{ nullptr };

    HiddenCoreContext()
    {
        if (glfwInit() == GLFW_FALSE)
            return;
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        window = glfwCreateWindow(64, 64, "gl_core_profile_test", nullptr, nullptr);
        if (window != nullptr)
            glfwMakeContextCurrent(window);
    }
    ~HiddenCoreContext()
    {
        if (window != nullptr) {
            glfwMakeContextCurrent(nullptr);
            glfwDestroyWindow(window);
        }
        glfwTerminate();
    }
};

struct ResourcesDirOverride
{
    std::string old_res;
    explicit ResourcesDirOverride(const std::string& dir) : old_res(Slic3r::resources_dir()) { Slic3r::set_resources_dir(dir); }
    ~ResourcesDirOverride() { Slic3r::set_resources_dir(old_res); }
};

// An RGBA8 colour target with depth, bound for drawing and reading.
struct OffscreenTarget
{
    GLuint fbo{ 0 }, color{ 0 }, depth{ 0 };
    explicit OffscreenTarget(int size)
    {
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glGenRenderbuffers(1, &color);
        glBindRenderbuffer(GL_RENDERBUFFER, color);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, size, size);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
        glGenRenderbuffers(1, &depth);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size, size);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
        glViewport(0, 0, size, size);
    }
    ~OffscreenTarget()
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteRenderbuffers(1, &depth);
        glDeleteRenderbuffers(1, &color);
        glDeleteFramebuffers(1, &fbo);
    }
    bool complete() const { return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE; }
    void clear() const
    {
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    static std::array<unsigned char, 4> pixel(int x, int y)
    {
        std::array<unsigned char, 4> rgba{ 0, 0, 0, 0 };
        glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        return rgba;
    }
};

GLenum drain_gl_errors()
{
    GLenum first = GL_NO_ERROR;
    for (int i = 0; i < 16; ++i) {
        const GLenum e = glGetError();
        if (e == GL_NO_ERROR)
            break;
        if (first == GL_NO_ERROR)
            first = e;
    }
    return first;
}

GLint bound_vao()
{
    GLint vao = -1;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    return vao;
}

} // namespace

TEST_CASE("OpenGL core profile: start-up, shaders, GLModel, plain VBO and dashed-line draws", "[GL][CoreProfile]")
{
    HiddenCoreContext ctx;
    if (ctx.window == nullptr) {
        WARN("No OpenGL 3.3 core context available here (no display or driver); core-profile smoke test skipped.");
        return;
    }

    ResourcesDirOverride resources(SLIC3R_TEST_RESOURCES_DIR);
    auto manager = std::make_unique<OpenGLManager>();
    REQUIRE(manager->init_gl(/*popup_error=*/false));

    const OpenGLManager::GLInfo& info = OpenGLManager::get_gl_info();
    INFO("GL " << info.get_version() << ", GLSL " << info.get_glsl_version() << ", " << info.get_renderer());
    REQUIRE(info.is_core_profile());
    REQUIRE(info.is_version_greater_or_equal_to(3, 3));
    REQUIRE(OpenGLManager::get_default_vao() != 0);
    CHECK(bound_vao() == GLint(OpenGLManager::get_default_vao()));
    CHECK(OpenGLManager::get_framebuffers_type() == OpenGLManager::EFramebufferType::Arb);
    // Extensions come from glGetStringi() in a core profile.
    CHECK_FALSE(OpenGLManager::GLInfo::get_extensions_string().empty());

    // Every shader the 140 set ships has to compile and link in a forward-compatible core profile.
    for (const char* name : { "imgui", "flat", "flat_clip", "flat_texture", "background", "dashed_thick_lines", "gouraud_light",
                              "thumbnail", "printbed", "gouraud_light_instanced", "gouraud", "variable_layer_height", "mm_contour",
                              "mm_gouraud", "texture_displacement_shaded", "texture_displacement_uvcheck", "selection_mask",
                              "selection_composite", "selection_edge", "selection_area_downsample", "selection_gaussian" }) {
        INFO("shader " << name);
        CHECK(manager->get_shader(name) != nullptr);
    }
    CHECK(drain_gl_errors() == GL_NO_ERROR);

    const int size = 32;
    OffscreenTarget target(size);
    REQUIRE(target.complete());
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    GLShaderProgram* flat = manager->get_shader("flat");
    REQUIRE(flat != nullptr);

    SECTION("GLModel draws through its own VAO and binds the default one back")
    {
        GLModel::Geometry g;
        g.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        g.add_vertex(Vec3f(-1.f, -1.f, 0.f));
        g.add_vertex(Vec3f(3.f, -1.f, 0.f));
        g.add_vertex(Vec3f(-1.f, 3.f, 0.f));
        g.add_triangle(0, 1, 2);
        GLModel model;
        model.init_from(std::move(g));
        model.set_color(ColorRGBA(1.f, 0.f, 0.f, 1.f));

        target.clear();
        flat->start_using();
        flat->set_uniform("view_model_matrix", Transform3d::Identity());
        flat->set_uniform("projection_matrix", Transform3d::Identity());
        model.render();
        flat->stop_using();

        CHECK(drain_gl_errors() == GL_NO_ERROR);
        CHECK(bound_vao() == GLint(OpenGLManager::get_default_vao()));
        const auto px = OffscreenTarget::pixel(size / 2, size / 2);
        CHECK(int(px[0]) == 255);
        CHECK(int(px[1]) == 0);
        CHECK(int(px[2]) == 0);
        model.reset();
        CHECK(drain_gl_errors() == GL_NO_ERROR);
    }

    SECTION("a plain VBO draw (legacy G-code viewer style) works on the default VAO")
    {
        const std::array<float, 9> tri = { -1.f, -1.f, 0.f, 3.f, -1.f, 0.f, -1.f, 3.f, 0.f };
        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri.data(), GL_STATIC_DRAW);

        target.clear();
        flat->start_using();
        flat->set_uniform("view_model_matrix", Transform3d::Identity());
        flat->set_uniform("projection_matrix", Transform3d::Identity());
        flat->set_uniform("uniform_color", ColorRGBA(0.f, 1.f, 0.f, 1.f));
        const GLint position_id = flat->get_attrib_location("v_position");
        REQUIRE(position_id != -1);
        glVertexAttribPointer(position_id, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
        glEnableVertexAttribArray(position_id);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisableVertexAttribArray(position_id);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        flat->stop_using();
        glDeleteBuffers(1, &vbo);

        CHECK(drain_gl_errors() == GL_NO_ERROR);
        const auto px = OffscreenTarget::pixel(size / 2, size / 2);
        CHECK(int(px[0]) == 0);
        CHECK(int(px[1]) == 255);
        CHECK(int(px[2]) == 0);
    }

    SECTION("dashed_thick_lines (geometry shader) draws a thick line")
    {
        GLShaderProgram* lines = manager->get_shader("dashed_thick_lines");
        REQUIRE(lines != nullptr);
        GLModel::Geometry g;
        g.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        g.add_vertex(Vec3f(-1.f, 0.f, 0.f));
        g.add_vertex(Vec3f(1.f, 0.f, 0.f));
        g.add_line(0, 1);
        GLModel model;
        model.init_from(std::move(g));
        model.set_color(ColorRGBA(0.f, 0.f, 1.f, 1.f));

        target.clear();
        lines->start_using();
        lines->set_uniform("view_model_matrix", Transform3d::Identity());
        lines->set_uniform("projection_matrix", Transform3d::Identity());
        lines->set_uniform("viewport_size", Vec2d(double(size), double(size)));
        lines->set_uniform("width", 4.0f);
        lines->set_uniform("gap_size", 0.0f);
        lines->set_uniform("dash_size", 0.0f);
        model.render();
        lines->stop_using();

        CHECK(drain_gl_errors() == GL_NO_ERROR);
        // The line runs along y = 0 (the middle row); with width 4 it covers the rows next to it too.
        const auto px = OffscreenTarget::pixel(size / 2, size / 2);
        CHECK(int(px[2]) > 128);
        const auto far_px = OffscreenTarget::pixel(size / 2, 2);
        CHECK(int(far_px[2]) == 0);
        model.reset();
    }

    SECTION("libvgcode renders a small G-code scene, then shuts down without unloading the shared GL loader")
    {
        // A square perimeter on two layers, processed by EdgeSlicer's GCodeProcessor.
        std::ostringstream o;
        const char nl = '\n';
        o << "; generated by OrcaSlicer 2.4.2 on 2026-01-01 at 00:00:00 UTC" << nl << "G28" << nl << "G90" << nl << "M83" << nl << "G92 E0" << nl;
        for (int layer = 1; layer <= 2; ++layer) {
            o << "; CHANGE_LAYER" << nl << "; Z_HEIGHT: " << 0.2 * layer << nl << "; LAYER_HEIGHT: 0.2" << nl
              << "G1 Z" << 0.2 * layer << " F600" << nl << "G1 X10 Y10 F6000" << nl;
            o << "; FEATURE: Outer wall" << nl << "; LINE_WIDTH: 2" << nl << "G1 X60 Y10 E2 F1800" << nl << "G1 X60 Y60 E2" << nl
              << "G1 X10 Y60 E2" << nl << "G1 X10 Y10 E2" << nl;
        }
        // An OrcaSlicer-produced G-code must carry its config block.
        // (The processor refuses a block with suspiciously few values, so pad it.)
        o << "; CONFIG_BLOCK_START" << nl << "; filament_diameter = 1.75" << nl << "; filament_density = 1.24" << nl;
        for (int i = 0; i < 80; ++i)
            o << "; layer_height = 0.2" << nl;
        o << "; CONFIG_BLOCK_END" << nl;
        const bool was_bbl = GCodeProcessor::s_IsBBLPrinter;
        GCodeProcessor::s_IsBBLPrinter = true;
        const boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("edge_vgcode_gl_%%%%%%%%.gcode");
        {
            boost::nowide::ofstream out(path.string());
            out << o.str();
        }
        GCodeProcessor processor;
        processor.process_file(path.string());
        boost::filesystem::remove(path);
        GCodeProcessor::s_IsBBLPrinter = was_bbl;
        const GCodeProcessorResult& result = processor.get_result();
        REQUIRE(result.moves.size() > 8);

        const int     big = 256;
        OffscreenTarget scene(big);
        REQUIRE(scene.complete());
        {
            libvgcode::Viewer viewer;
            REQUIRE_NOTHROW(viewer.init(reinterpret_cast<const char*>(glGetString(GL_VERSION))));
            libvgcode::GCodeInputData data = libvgcode::convert(result, { "#FF8000" }, {}, viewer);
            viewer.load(std::move(data));
            CHECK(viewer.get_layers_zs().size() == 2);
            CHECK(drain_gl_errors() == GL_NO_ERROR);
            viewer.set_view_type(libvgcode::EViewType::FeatureType);

            // Top-down orthographic camera over X/Y 0..70 mm.
            Matrix4f view = Matrix4f::Identity();
            view(2, 3) = -100.f;
            const float l = 0.f, r = 70.f, b = 0.f, t = 70.f, n = 1.f, f = 200.f;
            Matrix4f proj = Matrix4f::Zero();
            proj(0, 0) = 2.f / (r - l);
            proj(1, 1) = 2.f / (t - b);
            proj(2, 2) = -2.f / (f - n);
            proj(0, 3) = -(r + l) / (r - l);
            proj(1, 3) = -(t + b) / (t - b);
            proj(2, 3) = -(f + n) / (f - n);
            proj(3, 3) = 1.f;

            scene.clear();
            glEnable(GL_DEPTH_TEST);
            viewer.render(libvgcode::convert(view), libvgcode::convert(proj));
            glDisable(GL_DEPTH_TEST);
            CHECK(drain_gl_errors() == GL_NO_ERROR);
            // libvgcode restores the VAO it found bound (the app's default VAO).
            CHECK(bound_vao() == GLint(OpenGLManager::get_default_vao()));

            // The wall along y = 10 mm (pixel row 10/70 * 256 ~ 37) is lit; the middle of the square is not.
            int lit = 0;
            for (int y = 30; y <= 44; ++y) {
                const auto px = OffscreenTarget::pixel(big / 2, y);
                if (int(px[0]) + int(px[1]) + int(px[2]) > 60)
                    ++lit;
            }
            CHECK(lit > 0);
            const auto centre = OffscreenTarget::pixel(big / 2, big / 2);
            CHECK(int(centre[0]) + int(centre[1]) + int(centre[2]) == 0);

            viewer.shutdown();
        }
        CHECK(drain_gl_errors() == GL_NO_ERROR);

        // GL entry points still loaded after libvgcode's shutdown: the app keeps drawing.
        REQUIRE(glGetString != nullptr);
        CHECK(glGetString(GL_VERSION) != nullptr);
        GLModel::Geometry g;
        g.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        g.add_vertex(Vec3f(-1.f, -1.f, 0.f));
        g.add_vertex(Vec3f(3.f, -1.f, 0.f));
        g.add_vertex(Vec3f(-1.f, 3.f, 0.f));
        g.add_triangle(0, 1, 2);
        GLModel model;
        model.init_from(std::move(g));
        model.set_color(ColorRGBA(1.f, 1.f, 1.f, 1.f));
        scene.clear();
        flat->start_using();
        flat->set_uniform("view_model_matrix", Transform3d::Identity());
        flat->set_uniform("projection_matrix", Transform3d::Identity());
        model.render();
        flat->stop_using();
        CHECK(drain_gl_errors() == GL_NO_ERROR);
        CHECK(int(OffscreenTarget::pixel(big / 2, big / 2)[0]) == 255);
        model.reset();
    }

    CHECK(OpenGLManager::report_gl_errors("in the core-profile smoke test") == 0);
    manager.reset();
}
