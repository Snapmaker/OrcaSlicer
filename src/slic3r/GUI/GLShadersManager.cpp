#include "libslic3r/libslic3r.h"
#include "libslic3r/Platform.hpp"
#include "GLShadersManager.hpp"
#include "3DScene.hpp"
#include "GUI_App.hpp"

#include <cassert>
#include <algorithm>
#include <cstdlib>
#include <string_view>
using namespace std::literals;

#include <boost/log/trivial.hpp>
#include <GL/glew.h>

namespace Slic3r {

std::pair<bool, std::string> GLShadersManager::init()
{
    std::string error;

    auto append_shader = [this, &error](const std::string& name, const GLShaderProgram::ShaderFilenames& filenames,
        const std::initializer_list<std::string_view> &defines = {}) {
        m_shaders.push_back(std::make_unique<GLShaderProgram>());
        if (!m_shaders.back()->init_from_files(name, filenames, defines)) {
            error += name + "\n";
            // if any error happens while initializating the shader, we remove it from the list
            m_shaders.pop_back();
            return false;
        }
        return true;
    };

    auto appendOptionalShader = [&append_shader, &error](const std::string& name,
                                                          const GLShaderProgram::ShaderFilenames& filenames,
                                                          const std::initializer_list<std::string_view>& defines = {}) {
        const size_t errorLength = error.size();
        if (append_shader(name, filenames, defines))
            return;

        error.erase(errorLength);
        BOOST_LOG_TRIVIAL(warning) << "Optional shader unavailable: " << name;
    };

    assert(m_shaders.empty());

    bool valid = true;

    const std::string prefix = GUI::wxGetApp().is_gl_version_greater_or_equal_to(3, 1) ? "140/" : "110/";
    // imgui shader
    valid &= append_shader("imgui", { prefix + "imgui.vs", prefix + "imgui.fs" });
    // basic shader, used to render all what was previously rendered using the immediate mode
    valid &= append_shader("flat", { prefix + "flat.vs", prefix + "flat.fs" });
    // used to render selected geometry into the unified mask and stencil fallback
    appendOptionalShader("selection_mask", { prefix + "flat.vs", prefix + "selection_mask.fs" });
    // basic shader with plane clipping, used to render volumes in picking pass
    valid &= append_shader("flat_clip", { prefix + "flat_clip.vs", prefix + "flat_clip.fs" });
    // basic shader for textures, used to render textures
    valid &= append_shader("flat_texture", { prefix + "flat_texture.vs", prefix + "flat_texture.fs" });
    // used to render 3D scene background
    valid &= append_shader("background", { prefix + "background.vs", prefix + "background.fs" });
    // used to composite the selection fill and outline over the completed scene
    appendOptionalShader("selection_composite", { prefix + "background.vs", prefix + "selection_composite.fs" });
    // used to extract the selection edge from the low-resolution selection mask
    appendOptionalShader("selection_edge", { prefix + "background.vs", prefix + "selection_edge.fs" });
    // used to area-downsample the full-resolution selection mask before edge extraction
    appendOptionalShader("selection_area_downsample", { prefix + "background.vs", prefix + "selection_area_downsample.fs" });
    // used to apply directional Gaussian blur to selection edge textures
    appendOptionalShader("selection_gaussian", { prefix + "background.vs", prefix + "selection_gaussian.fs" });
    // used to render bed axes and model, selection hints, gcode sequential view marker model, preview shells, options in gcode preview
    valid &= append_shader("gouraud_light", { prefix + "gouraud_light.vs", prefix + "gouraud_light.fs" });
    // used to render gcode toolpaths with GPU-generated geometry (data tables in TBOs)
    // and gcode option markers (seams, tool changes, ...) with GPU-side placement.
    // The shaders need GLSL 140 / texture buffers; register them only when the
    // context can use them (on older GL the gpu path pipeline stays disabled
    // and the files only exist in the 140/ directory anyway).
    if (GUI::wxGetApp().is_gl_version_greater_or_equal_to(3, 1)) {
        valid &= append_shader("gpu_path", { prefix + "gpu_path.vs", prefix + "gpu_path.fs" });
        valid &= append_shader("gpu_path_marker", { prefix + "gpu_path_marker.vs", prefix + "gpu_path_marker.fs" });
    }
    // used to bake and composite PCSS soft shadows on the build plate (sampler objects need GL 3.3)
    if (GUI::wxGetApp().is_gl_version_greater_or_equal_to(3, 3))
    {
        appendOptionalShader("shadow_ground_mask", { prefix + "shadow_ground_mask.vs", prefix + "shadow_ground_mask.fs" });
        appendOptionalShader("shadow_receiver", { prefix + "shadow_receiver.vs", prefix + "shadow_receiver.fs" });
        appendOptionalShader("shadow_mask_blur", { prefix + "background.vs", prefix + "shadow_mask_blur.fs" });
        appendOptionalShader("shadow_depth_min", { prefix + "background.vs", prefix + "shadow_depth_min.fs" });
        // screen-space pass that darkens object pixels lying in the shadow
        appendOptionalShader("shadow_screen", { prefix + "background.vs", prefix + "shadow_screen.fs" });
        appendOptionalShader("shadow_screen_composite", { prefix + "background.vs", prefix + "shadow_screen_composite.fs" });
    }
    //used to render thumbnail
    valid &= append_shader("thumbnail", { prefix + "thumbnail.vs", prefix + "thumbnail.fs"});
    // used to render printbed
    valid &= append_shader("printbed", { prefix + "printbed.vs", prefix + "printbed.fs" });
    // used to render options in gcode preview
    if (GUI::wxGetApp().is_gl_version_greater_or_equal_to(3, 3)) {
        valid &= append_shader("gouraud_light_instanced", { prefix + "gouraud_light_instanced.vs", prefix + "gouraud_light_instanced.fs" });
    }

    // used to render objects in 3d editor
    valid &= append_shader("gouraud", { prefix + "gouraud.vs", prefix + "gouraud.fs" }
#if ENABLE_ENVIRONMENT_MAP
        , { "ENABLE_ENVIRONMENT_MAP"sv }
#endif // ENABLE_ENVIRONMENT_MAP
        );
    // used to render variable layers heights in 3d editor
    valid &= append_shader("variable_layer_height", { prefix + "variable_layer_height.vs", prefix + "variable_layer_height.fs" });
    // used to render highlight contour around selected triangles inside the multi-material gizmo
    valid &= append_shader("mm_contour", { prefix + "mm_contour.vs", prefix + "mm_contour.fs" });
    // Used to render painted triangles inside the multi-material gizmo. Triangle normals are computed inside fragment shader.
    // For Apple's on Arm CPU computed triangle normals inside fragment shader using dFdx and dFdy has the opposite direction.
    // Because of this, objects had darker colors inside the multi-material gizmo.
    // Based on https://stackoverflow.com/a/66206648, the similar behavior was also spotted on some other devices with Arm CPU.
    // Since macOS 12 (Monterey), this issue with the opposite direction on Apple's Arm CPU seems to be fixed, and computed
    // triangle normals inside fragment shader have the right direction.
    if (platform_flavor() == PlatformFlavor::OSXOnArm && wxPlatformInfo::Get().GetOSMajorVersion() < 12)
        valid &= append_shader("mm_gouraud", { prefix + "mm_gouraud.vs", prefix + "mm_gouraud.fs" }, { "FLIP_TRIANGLE_NORMALS"sv });
    else
        valid &= append_shader("mm_gouraud", { prefix + "mm_gouraud.vs", prefix + "mm_gouraud.fs" });

    // Diagnostic variants are selected at launch, including automated comparisons.
    // Keep their files available without compiling every experiment in normal sessions.
    const auto ao_option = [](const char* key) -> std::string_view {
        const char* value = std::getenv(key);
        return value ? std::string_view(value) : std::string_view();
    };
    const auto ao_comparison = ao_option("ORCA_AO_COMPARISON_MODE");
    const auto ao_composite = ao_option("ORCA_AO_COMPOSITE");
    const bool ao_split = ao_composite == "split" || ao_comparison == "split";
    const bool ao_confidence = ao_composite == "confidence" || ao_comparison == "confidence";
    const bool ao_reference = ao_composite == "reference" || ao_comparison == "composite";
    const bool ao_fast_reconstruction = ao_option("ORCA_AO_FS_RECONSTRUCTION") == "fast" || ao_comparison == "fseval";
    const bool ao_legacy_low = ao_option("ORCA_AO_CS_LOW") == "legacy" || ao_comparison == "cslow";

    if (GUI::wxGetApp().is_gl_version_greater_or_equal_to(3, 1)) {
        // Files and registered programs use functional AO stage names.
        appendOptionalShader("ao_reconstruct_view_normals", {"140/ao_fullscreen_triangle.vs", "140/ao_reconstruct_view_normals.fs"});
        if (ao_confidence)
            appendOptionalShader("ao_reconstruct_view_normals_confidence",
                                 {"140/ao_fullscreen_triangle.vs", "140/ao_reconstruct_view_normals.fs"}, {"AO_CONFIDENCE_OUTPUT"});
        appendOptionalShader("ao_evaluate_visibility_reference_fs",
                             {"140/ao_fullscreen_triangle.vs", "140/ao_evaluate_visibility_reference.fs"});
        appendOptionalShader("ao_denoise_visibility_geometry_reference",
                             {"140/ao_fullscreen_triangle.vs", "140/ao_denoise_visibility_geometry_reference.fs"});
        appendOptionalShader("ao_upsample_visibility_bilateral",
                             {"140/ao_fullscreen_triangle.vs", "140/ao_upsample_visibility_bilateral.fs"});
        appendOptionalShader("ao_composite_color_pixel", {"140/ao_fullscreen_triangle.vs", "140/ao_composite_color_pixel.fs"});
        if (ao_confidence)
            appendOptionalShader("ao_composite_color_pixel_confidence",
                                 {"140/ao_fullscreen_triangle.vs", "140/ao_composite_color_pixel.fs"}, {"AO_PRECOMPUTED_CONFIDENCE"});
        if (ao_reference)
            appendOptionalShader("ao_composite_color_pixel_reference",
                                 {"140/ao_fullscreen_triangle.vs", "140/ao_composite_color_pixel_reference.fs"});
        appendOptionalShader("ao_evaluate_visibility_fs", {"140/ao_fullscreen_triangle.vs", "140/ao_evaluate_visibility.fs"});
        appendOptionalShader("ao_denoise_visibility_geometry", {"140/ao_fullscreen_triangle.vs", "140/ao_denoise_visibility_geometry.fs"});
        appendOptionalShader("ao_evaluate_visibility_edges_fs", {"140/ao_fullscreen_triangle.vs", "140/ao_evaluate_visibility.fs"},
                             {"AO_EDGE_OUTPUT"});
        if (ao_fast_reconstruction)
            appendOptionalShader("ao_evaluate_visibility_edges_xy_fs", {"140/ao_fullscreen_triangle.vs", "140/ao_evaluate_visibility.fs"},
                                 {"AO_EDGE_OUTPUT", "AO_SEPARABLE_XY"});
        appendOptionalShader("ao_denoise_visibility_edge_guided_fs",
                             {"140/ao_fullscreen_triangle.vs", "140/ao_denoise_visibility_edge_guided.fs"});
        appendOptionalShader("ao_render_receiver_depth", {"140/ao_render_receiver_depth.vs", "140/ao_render_receiver_depth.fs"});
        if (GUI::wxGetApp().is_gl_version_greater_or_equal_to(4, 0)) {
            appendOptionalShader("ao_composite_color_sample", {"140/ao_fullscreen_triangle.vs", "140/ao_composite_color_sample.fs"});
            if (ao_confidence)
                appendOptionalShader("ao_composite_color_sample_confidence",
                                     {"140/ao_fullscreen_triangle.vs", "140/ao_composite_color_sample.fs"}, {"AO_PRECOMPUTED_CONFIDENCE"});
            if (ao_split) {
                appendOptionalShader("ao_classify_sample_surfaces", {"140/ao_fullscreen_triangle.vs", "140/ao_composite_color_sample.fs"},
                                     {"AO_CLASSIFY_SURFACE"});
                appendOptionalShader("ao_composite_color_pixel_fast", {"140/ao_fullscreen_triangle.vs", "140/ao_composite_color_sample.fs"},
                                     {"AO_COMPOSITE_FAST"});
                appendOptionalShader("ao_composite_color_sample_edges",
                                     {"140/ao_fullscreen_triangle.vs", "140/ao_composite_color_sample.fs"}, {"AO_COMPOSITE_EDGES"});
            }
            if (ao_reference)
                appendOptionalShader("ao_composite_color_sample_reference",
                                     {"140/ao_fullscreen_triangle.vs", "140/ao_composite_color_sample_reference.fs"});
        }
    }
    if (GUI::wxGetApp().is_gl_version_greater_or_equal_to(4, 3)) {
        const std::pair<std::string, std::string> stages[] = {{"ao_prefilter_depth_mips", "ao_prefilter_depth_mips"},
                                                              {"ao_evaluate_visibility_edges_cs", "ao_evaluate_visibility_edges"},
                                                              {"ao_denoise_visibility_edge_guided_cs",
                                                               "ao_denoise_visibility_edge_guided"}};
        for (const auto& stage : stages) {
            GLShaderProgram::ShaderFilenames files{};
            files[static_cast<size_t>(GLShaderProgram::EShaderType::Compute)] = "430/" + stage.second + ".cs";
            appendOptionalShader(stage.first, files);
            if (stage.first == "ao_evaluate_visibility_edges_cs" && ao_legacy_low)
                appendOptionalShader("ao_evaluate_visibility_edges_legacy_low_cs", files, {"AO_LEGACY_LOW"});
        }
    }
    return {valid, error};
}

void GLShadersManager::shutdown() { m_shaders.clear(); }

GLShaderProgram* GLShadersManager::get_shader(const std::string& shader_name)
{
    auto it = std::find_if(m_shaders.begin(), m_shaders.end(), [&shader_name](std::unique_ptr<GLShaderProgram>& p) { return p->get_name() == shader_name; });
    return (it != m_shaders.end()) ? it->get() : nullptr;
}

GLShaderProgram* GLShadersManager::get_current_shader()
{
    GLint id = 0;
    glsafe(::glGetIntegerv(GL_CURRENT_PROGRAM, &id));
    if (id == 0)
        return nullptr;

    auto it = std::find_if(m_shaders.begin(), m_shaders.end(), [id](std::unique_ptr<GLShaderProgram>& p) { return static_cast<GLint>(p->get_id()) == id; });
    return (it != m_shaders.end()) ? it->get() : nullptr;
}

} // namespace Slic3r
