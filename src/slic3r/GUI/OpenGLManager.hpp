#ifndef slic3r_OpenGLManager_hpp_
#define slic3r_OpenGLManager_hpp_

#include "GLShadersManager.hpp"

class wxWindow;
class wxGLCanvas;
class wxGLContext;

namespace Slic3r {
namespace GUI {


class OpenGLManager
{
public:
    enum class EFramebufferType : unsigned char
    {
        Unknown,
        Arb,
        Ext
    };

    class GLInfo
    {
        bool m_detected{ false };
        int m_max_tex_size{ 0 };
        float m_max_anisotropy{ 0.0f };
        bool m_core_profile{ false };

        std::string m_version;
        std::string m_glsl_version;
        std::string m_vendor;
        std::string m_renderer;

    public:
        GLInfo() = default;

        const std::string& get_version() const;
        const std::string& get_glsl_version() const;
        const std::string& get_vendor() const;
        const std::string& get_renderer() const;

        bool is_mesa() const;
        // True for a core-profile context (no wide lines, no fixed-function state, a VAO must be
        // bound to draw). init_glcontext() asks for the highest forward-compatible core profile first
        // (OrcaSlicer #10735), so this is the normal case now: macOS 4.1, Windows/Linux 4.x. It is
        // false only where core was refused and we fell back to a compatibility or default context.
        // Also false until init_gl() has detected the context (it never queries GL by itself).
        bool is_core_profile() const;

        int get_max_tex_size() const;
        float get_max_anisotropy() const;

        bool is_version_greater_or_equal_to(unsigned int major, unsigned int minor) const;
        bool is_glsl_version_greater_or_equal_to(unsigned int major, unsigned int minor) const;

        // If formatted for github, plaintext with OpenGL extensions enclosed into <details>.
        // Otherwise HTML formatted for the system info dialog.
        std::string to_string(bool for_github) const;

        // Space-separated extension list on either profile (glGetString(GL_EXTENSIONS) is gone
        // from core profiles; there it is assembled from glGetStringi()).
        static std::string get_extensions_string();

    private:
        void detect() const;
    };

#ifdef __APPLE__
    // Part of hack to remove crash when closing the application on OSX 10.9.5 when building against newer wxWidgets
    struct OSInfo
    {
        int major{ 0 };
        int minor{ 0 };
        int micro{ 0 };
    };
#endif //__APPLE__

private:
    enum class EMultisampleState : unsigned char
    {
        Unknown,
        Enabled,
        Disabled
    };

    bool m_gl_initialized{ false };
    wxGLContext* m_context{ nullptr };
    GLShadersManager m_shaders_manager;
    static GLInfo s_gl_info;
#ifdef __APPLE__
    // Part of hack to remove crash when closing the application on OSX 10.9.5 when building against newer wxWidgets
    static OSInfo s_os_info;
#endif //__APPLE__
    static bool s_compressed_textures_supported;
    static bool s_force_power_of_two_textures;

    static EMultisampleState s_multisample;
    static EFramebufferType s_framebuffers_type;
    static OpenGLManager* s_active;
    // EDGE: see get_default_vao().
    static unsigned int s_default_vao;

public:
    OpenGLManager() = default;
    ~OpenGLManager();

    bool init_gl(bool popup_error = true);
    // Creates the one GL context every canvas shares. Profile order (EDGE, after OrcaSlicer #10735):
    // the highest forward-compatible core profile 4.6..3.2, then a compatibility profile, then the
    // platform default. EDGESLICER_OPENGL_PROFILE=core|compatibility|default (environment) or
    // "opengl_profile" in the app config picks one by hand; the environment wins.
    wxGLContext* init_glcontext(wxGLCanvas& canvas);

    // EDGE (core profile): no draw or vertex-attribute call is valid without a bound vertex array
    // object. One VAO is created with the context and stays bound; code with its own VAO (GLModel,
    // ImGui, the painter gizmos) binds this one back when it is done, so the remaining plain VBO
    // draws (legacy G-code viewer, 3DBed, ...) keep working. 0 in a compatibility context.
    static unsigned int get_default_vao() { return s_default_vao; }
    static void bind_default_vao();
    // glLineWidth() above 1 is GL_INVALID_VALUE in a forward-compatible core context (macOS, and
    // what we ask for everywhere). Sets it only where it is valid; core-profile lines are 1 px.
    static void set_line_width(float width);
    // Drains glGetError() and logs what it found as "OpenGL error(s) <where>: ...", within a small
    // per-session budget so a broken frame cannot flood the log. first_error: one the caller already
    // read with glGetError() (0 = none). Returns the number of errors.
    static int report_gl_errors(const std::string& where, unsigned int first_error = 0);

    GLShaderProgram* get_shader(const std::string& shader_name) { return m_shaders_manager.get_shader(shader_name); }
    GLShaderProgram* get_current_shader() { return m_shaders_manager.get_current_shader(); }
    // Ultra: the manager whose init_gl() ran last (the GUI's, or the CLI's thumbnail renderer),
    // so render code can find the bound shader without a wxApp.
    static GLShaderProgram* get_active_shader() { return s_active ? s_active->get_current_shader() : nullptr; }

    static bool are_compressed_textures_supported() { return s_compressed_textures_supported; }
    static bool can_multisample() { return s_multisample == EMultisampleState::Enabled; }
    static bool are_framebuffers_supported() { return (s_framebuffers_type != EFramebufferType::Unknown); }
    static EFramebufferType get_framebuffers_type() { return s_framebuffers_type; }
    static wxGLCanvas* create_wxglcanvas(wxWindow& parent);
    static const GLInfo& get_gl_info() { return s_gl_info; }
    static bool force_power_of_two_textures() { return s_force_power_of_two_textures; }

private:
    static void detect_multisample(int* attribList);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_OpenGLManager_hpp_
