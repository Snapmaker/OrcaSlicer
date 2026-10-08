#include "libslic3r/libslic3r.h"
#include "OpenGLManager.hpp"

#include "GUI.hpp"
#include "I18N.hpp"
#include "3DScene.hpp"

#include "libslic3r/Platform.hpp"
#include "libslic3r/AppConfig.hpp"

#include <glad/gl.h>

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <wx/glcanvas.h>
#include <wx/msgdlg.h>
#include <wx/log.h>
#include <wx/utils.h>

#include <functional>
#include <map>
#include <sstream>

#ifdef __APPLE__
// Part of hack to remove crash when closing the application on OSX 10.9.5 when building against newer wxWidgets
#include <wx/platinfo.h>

#include "../Utils/MacDarkMode.hpp"
#endif // __APPLE__

namespace Slic3r {
namespace GUI {

// A safe wrapper around glGetString to report a "N/A" string in case glGetString returns nullptr.
std::string gl_get_string_safe(GLenum param, const std::string& default_value)
{
    const char* value = (const char*)::glGetString(param);
    return std::string((value != nullptr) ? value : default_value);
}

const std::string& OpenGLManager::GLInfo::get_version() const
{
    if (!m_detected)
        detect();

    return m_version;
}

const std::string& OpenGLManager::GLInfo::get_glsl_version() const
{
    if (!m_detected)
        detect();

    return m_glsl_version;
}

const std::string& OpenGLManager::GLInfo::get_vendor() const
{
    if (!m_detected)
        detect();

    return m_vendor;
}

const std::string& OpenGLManager::GLInfo::get_renderer() const
{
    if (!m_detected)
        detect();

    return m_renderer;
}

bool OpenGLManager::GLInfo::is_mesa() const
{
    return boost::icontains(m_version, "mesa");
}

bool OpenGLManager::GLInfo::is_core_profile() const
{
    // EDGE: no lazy detect() here (upstream does not have one either). GLModel::reset() asks this on
    // every reset, also long before a context exists (bed and plate models are built at start-up),
    // and detecting then would cache "N/A" for the whole session. init_gl() detects explicitly.
    return m_detected && m_core_profile;
}

int OpenGLManager::GLInfo::get_max_tex_size() const
{
    if (!m_detected)
        detect();

    // clamp to avoid the texture generation become too slow and use too much GPU memory
#ifdef __APPLE__
    // and use smaller texture for non retina systems
    return (Slic3r::GUI::mac_max_scaling_factor() > 1.0) ? std::min(m_max_tex_size, 8192) : std::min(m_max_tex_size / 2, 4096);
#else
    // and use smaller texture for older OpenGL versions
    return is_version_greater_or_equal_to(3, 0) ? std::min(m_max_tex_size, 8192) : std::min(m_max_tex_size / 2, 4096);
#endif // __APPLE__
}

float OpenGLManager::GLInfo::get_max_anisotropy() const
{
    if (!m_detected)
        detect();

    return m_max_anisotropy;
}

static bool version_greater_or_equal_to(const std::string& version, unsigned int major, unsigned int minor);

void OpenGLManager::GLInfo::detect() const
{
    *const_cast<std::string*>(&m_version) = gl_get_string_safe(GL_VERSION, "N/A");
    *const_cast<std::string*>(&m_glsl_version) = gl_get_string_safe(GL_SHADING_LANGUAGE_VERSION, "N/A");
    *const_cast<std::string*>(&m_vendor) = gl_get_string_safe(GL_VENDOR, "N/A");
    *const_cast<std::string*>(&m_renderer) = gl_get_string_safe(GL_RENDERER, "N/A");

    BOOST_LOG_TRIVIAL(info) << boost::format("got opengl version %1%, glsl version %2%, vendor %3%, renderer %4%")%m_version %m_glsl_version %m_vendor %m_renderer<< std::endl;

    int* max_tex_size = const_cast<int*>(&m_max_tex_size);
    glsafe(::glGetIntegerv(GL_MAX_TEXTURE_SIZE, max_tex_size));

    *max_tex_size /= 2;

    if (Slic3r::total_physical_memory() / (1024 * 1024 * 1024) < 6)
        *max_tex_size /= 2;

    if (GLAD_GL_EXT_texture_filter_anisotropic) {
        float* max_anisotropy = const_cast<float*>(&m_max_anisotropy);
        glsafe(::glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, max_anisotropy));
    }

    // Profiles exist from 3.2 on; a legacy 2.x context (the macOS compatibility fallback) is never
    // core. Ask the context itself; a driver that leaves the mask empty is core when it does not
    // advertise ARB_compatibility (upstream's test).
    if (version_greater_or_equal_to(m_version, 3, 2)) {
        GLint mask = 0;
        ::glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &mask);
        if (::glGetError() != GL_NO_ERROR)
            mask = 0;
        *const_cast<bool*>(&m_core_profile) = (mask & GL_CONTEXT_CORE_PROFILE_BIT) != 0 || (mask == 0 && !GLAD_GL_ARB_compatibility);
    }

    *const_cast<bool*>(&m_detected) = true;
}

static bool version_greater_or_equal_to(const std::string& version, unsigned int major, unsigned int minor)
{
    if (version == "N/A")
        return false;

    std::vector<std::string> tokens;
    boost::split(tokens, version, boost::is_any_of(" "), boost::token_compress_on);

    if (tokens.empty())
        return false;

    std::vector<std::string> numbers;
    boost::split(numbers, tokens[0], boost::is_any_of("."), boost::token_compress_on);

    unsigned int gl_major = 0;
    unsigned int gl_minor = 0;

    if (numbers.size() > 0)
        gl_major = ::atoi(numbers[0].c_str());

    if (numbers.size() > 1)
        gl_minor = ::atoi(numbers[1].c_str());

    if (gl_major < major)
        return false;
    else if (gl_major > major)
        return true;
    else
        return gl_minor >= minor;
}

bool OpenGLManager::GLInfo::is_version_greater_or_equal_to(unsigned int major, unsigned int minor) const
{
    if (!m_detected)
        detect();

    return version_greater_or_equal_to(m_version, major, minor);
}

bool OpenGLManager::GLInfo::is_glsl_version_greater_or_equal_to(unsigned int major, unsigned int minor) const
{
    if (!m_detected)
        detect();

    return version_greater_or_equal_to(m_glsl_version, major, minor);
}

// If formatted for github, plaintext with OpenGL extensions enclosed into <details>.
// Otherwise HTML formatted for the system info dialog.
std::string OpenGLManager::GLInfo::to_string(bool for_github) const
{
    if (!m_detected)
        detect();

    std::stringstream out;

    const bool format_as_html = ! for_github;
    std::string h2_start = format_as_html ? "<b>" : "";
    std::string h2_end = format_as_html ? "</b>" : "";
    std::string b_start = format_as_html ? "<b>" : "";
    std::string b_end = format_as_html ? "</b>" : "";
    std::string line_end = format_as_html ? "<br>" : "\n";

    out << h2_start << "OpenGL installation" << h2_end << line_end;
    out << b_start << "GL version:   " << b_end << m_version << line_end;
    out << b_start << "Profile:      " << b_end << (m_core_profile ? "Core" : (version_greater_or_equal_to(m_version, 3, 2) ? "Compatibility" : "Legacy (no profile)")) << line_end;
    out << b_start << "Vendor:       " << b_end << m_vendor << line_end;
    out << b_start << "Renderer:     " << b_end << m_renderer << line_end;
    out << b_start << "GLSL version: " << b_end << m_glsl_version << line_end;

    {
        std::vector<std::string> extensions_list;
        std::string extensions_str = get_extensions_string();
        boost::split(extensions_list, extensions_str, boost::is_any_of(" "), boost::token_compress_on);

        if (!extensions_list.empty()) {
            if (for_github)
                out << "<details>\n<summary>Installed extensions:</summary>\n";
            else
                out << h2_start << "Installed extensions:" << h2_end << line_end;

            std::sort(extensions_list.begin(), extensions_list.end());
            for (const std::string& ext : extensions_list)
                if (! ext.empty())
                    out << ext << line_end;

            if (for_github)
                out << "</details>\n";
        }
    }

    return out.str();
}

std::string OpenGLManager::GLInfo::get_extensions_string()
{
    // glGetString(GL_EXTENSIONS) is GL_INVALID_ENUM in a core profile; glGetStringi() exists from 3.0
    // and works on both profiles.
    if (GLAD_GL_VERSION_3_0 && ::glGetStringi != nullptr) {
        GLint count = 0;
        ::glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        std::string out;
        for (GLint i = 0; i < count; ++i) {
            const char* ext = reinterpret_cast<const char*>(::glGetStringi(GL_EXTENSIONS, GLuint(i)));
            if (ext == nullptr)
                continue;
            if (!out.empty())
                out += ' ';
            out += ext;
        }
        while (::glGetError() != GL_NO_ERROR) {}
        return out;
    }
    return gl_get_string_safe(GL_EXTENSIONS, "");
}

OpenGLManager::GLInfo OpenGLManager::s_gl_info;
bool OpenGLManager::s_compressed_textures_supported = false;
bool OpenGLManager::s_force_power_of_two_textures = false;
OpenGLManager::EMultisampleState OpenGLManager::s_multisample = OpenGLManager::EMultisampleState::Unknown;
OpenGLManager::EFramebufferType OpenGLManager::s_framebuffers_type = OpenGLManager::EFramebufferType::Unknown;
unsigned int OpenGLManager::s_default_vao = 0;

#ifdef __APPLE__
// Part of hack to remove crash when closing the application on OSX 10.9.5 when building against newer wxWidgets
OpenGLManager::OSInfo OpenGLManager::s_os_info;
#endif // __APPLE__

OpenGLManager::~OpenGLManager()
{
    m_shaders_manager.shutdown();
    // The VAO dies with its context; just forget it (the context may not be current here).
    s_default_vao = 0;
    if (s_active == this)
        s_active = nullptr;

#ifdef __APPLE__
    // This is an ugly hack needed to solve the crash happening when closing the application on OSX 10.9.5 with newer wxWidgets
    // The crash is triggered inside wxGLContext destructor
    if (s_os_info.major != 10 || s_os_info.minor != 9 || s_os_info.micro != 5)
    {
#endif //__APPLE__
        if (m_context != nullptr)
            delete m_context;
#ifdef __APPLE__
    }
#endif //__APPLE__
}

OpenGLManager* OpenGLManager::s_active = nullptr;

// Logs what the context actually is, not what was asked for: profile and the default
// framebuffer's colour/depth/stencil/sample bits. A missing depth or alpha plane, or a legacy
// context where a core one was expected, shows up here first (owner logs, macOS in particular).
// Uses the raw GL calls and drains glGetError afterwards: some of these queries are invalid on
// one profile or the other, and a debug-build glsafe() must not assert on a diagnostic.
// EDGE: what init_glcontext() asked for, so the start-up log can put it next to the version granted
// (macOS creates a 4.1 core context for a 4.6 request; the request alone said "4.6").
static std::string s_context_request;

static void log_gl_context_details()
{
    GLint major = 0, minor = 0;
    if (GLAD_GL_VERSION_3_0) {
        ::glGetIntegerv(GL_MAJOR_VERSION, &major);
        ::glGetIntegerv(GL_MINOR_VERSION, &minor);
    }

    std::string profile = "legacy (pre-3.2, no profile)";
    bool        core    = false;
    if (GLAD_GL_VERSION_3_2) {
        GLint mask = 0;
        ::glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &mask);
        core    = (mask & GL_CONTEXT_CORE_PROFILE_BIT) != 0;
        profile = core ? "core" : ((mask & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT) != 0 ? "compatibility" : "unknown");
    }

    GLint red = -1, green = -1, blue = -1, alpha = -1, depth = -1, stencil = -1;
    if (core && GLAD_GL_VERSION_3_0) {
        // GL_*_BITS are gone from the core profile; ask the default framebuffer instead.
        ::glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &red);
        ::glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE, &green);
        ::glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE, &blue);
        ::glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, &alpha);
        ::glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depth);
        ::glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_STENCIL, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencil);
    } else {
        ::glGetIntegerv(GL_RED_BITS, &red);
        ::glGetIntegerv(GL_GREEN_BITS, &green);
        ::glGetIntegerv(GL_BLUE_BITS, &blue);
        ::glGetIntegerv(GL_ALPHA_BITS, &alpha);
        ::glGetIntegerv(GL_DEPTH_BITS, &depth);
        ::glGetIntegerv(GL_STENCIL_BITS, &stencil);
    }
    GLint sample_buffers = -1, samples = -1, max_texture_units = -1;
    ::glGetIntegerv(GL_SAMPLE_BUFFERS, &sample_buffers);
    ::glGetIntegerv(GL_SAMPLES, &samples);
    ::glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &max_texture_units);

    while (::glGetError() != GL_NO_ERROR) {}

    // Logged at warning level on purpose: the default log_severity_level is "warning", and these
    // two lines are what a user's log has to carry for a rendering report to be diagnosable.
    BOOST_LOG_TRIVIAL(warning) << "OpenGL context: version " << OpenGLManager::get_gl_info().get_version()
                            << " (granted " << major << "." << minor
                            << (s_context_request.empty() ? std::string() : ", requested " + s_context_request) << "), profile " << profile
                            << ", GLSL " << OpenGLManager::get_gl_info().get_glsl_version()
                            << ", renderer " << OpenGLManager::get_gl_info().get_renderer()
                            << ", vendor " << OpenGLManager::get_gl_info().get_vendor();
    BOOST_LOG_TRIVIAL(warning) << "OpenGL default framebuffer: RGBA bits " << red << "/" << green << "/" << blue << "/" << alpha
                            << ", depth bits " << depth << ", stencil bits " << stencil
                            << ", sample buffers " << sample_buffers << ", samples " << samples
                            << ", multisample " << (OpenGLManager::can_multisample() ? "enabled" : "disabled")
                            << ", combined texture units " << max_texture_units
                            << ", shader set " << (OpenGLManager::get_gl_info().is_version_greater_or_equal_to(3, 1) ? "140" : "110");
}

bool OpenGLManager::init_gl(bool popup_error)
{
    s_active = this;
    if (!m_gl_initialized) {
        // Load GL functions via GLAD (replaces GLEW). GLEW's glewInit() fails
        // with "Missing GL version" on modern GL contexts with some drivers
        // (e.g. NVIDIA) even though the context is valid; GLAD loads through
        // glXGetProcAddress and works. This matches OrcaSlicer / #351.
        int version = gladLoaderLoadGL();
        if (version == 0) {
            BOOST_LOG_TRIVIAL(error) << "Unable to init GLAD OpenGL loader";
            return false;
        }
        BOOST_LOG_TRIVIAL(info) << "GLAD loaded OpenGL " << GLAD_VERSION_MAJOR(version) << "." << GLAD_VERSION_MINOR(version);
        m_gl_initialized = true;
        // Drop whatever the context creation / loader left in the error flag, so the start-up check
        // below only reports what our own initialisation does.
        while (::glGetError() != GL_NO_ERROR) {}
        // Detect now, with the context current: is_core_profile() does not detect by itself.
        s_gl_info.get_version();
        if (s_gl_info.is_core_profile()) {
            // See get_default_vao(): without a bound VAO every draw and glVertexAttribPointer()
            // call is GL_INVALID_OPERATION in a core profile.
            GLuint vao = 0;
            ::glGenVertexArrays(1, &vao);
            ::glBindVertexArray(vao);
            s_default_vao = vao;
        }
        log_gl_context_details();
        if (GLAD_GL_EXT_texture_compression_s3tc)
            s_compressed_textures_supported = true;
        else
            s_compressed_textures_supported = false;

        if (s_gl_info.is_version_greater_or_equal_to(3, 0)) {
            // ARB framebuffer objects are core from 3.0. A core profile (macOS) need not list the
            // extension, and the EXT entry points are gone there, so never pick Ext on >= 3.0.
            s_framebuffers_type = EFramebufferType::Arb;
            BOOST_LOG_TRIVIAL(info) << "OpenGL >= 3.0, Framebuffer Type ARB." << std::endl;
        }
        else if (GLAD_GL_ARB_framebuffer_object) {
            s_framebuffers_type = EFramebufferType::Arb;
            BOOST_LOG_TRIVIAL(info) << "Found Framebuffer Type ARB."<< std::endl;
        }
        else if (GLAD_GL_EXT_framebuffer_object) {
            BOOST_LOG_TRIVIAL(info) << "Found Framebuffer Type Ext."<< std::endl;
            s_framebuffers_type = EFramebufferType::Ext;
        }
        else {
            s_framebuffers_type = EFramebufferType::Unknown;
            BOOST_LOG_TRIVIAL(warning) << "Found Framebuffer Type unknown!"<< std::endl;
        }

        bool valid_version = s_gl_info.is_version_greater_or_equal_to(2, 0);
        if (!valid_version) {
            BOOST_LOG_TRIVIAL(error) << "Found opengl version <= 2.0"<< std::endl;
            // Complain about the OpenGL version.
            if (popup_error) {
                wxString message = from_u8((boost::format(
                    _utf8(L("The application cannot run normally because OpenGL version is lower than 2.0.\n")))).str());
                message += "\n";
                message += _L("Please upgrade your graphics card driver.");
                wxMessageBox(message, _L("Unsupported OpenGL version"), wxOK | wxICON_ERROR);
            }
        }

        if (valid_version)
        {
            // load shaders
            auto [result, error] = m_shaders_manager.init();
            if (!result) {
                BOOST_LOG_TRIVIAL(error) << "Unable to load shaders: "<<error<< std::endl;
                // EDGE: a shader that a core profile rejects must not leave the user stuck with a
                // blank canvas: remember to ask for a compatibility profile next time (what we ran
                // on before the core-profile switch). GUI only (popup_error), never the CLI.
                bool fallback_saved = false;
                if (popup_error && s_gl_info.is_core_profile()) {
                    wxString env;
                    AppConfig* app_config = get_app_config();
                    if (app_config != nullptr && !(wxGetEnv("EDGESLICER_OPENGL_PROFILE", &env) && !env.empty())) {
                        app_config->set("opengl_profile", "compatibility");
                        try {
                            app_config->save();
                            fallback_saved = true;
                            BOOST_LOG_TRIVIAL(error) << "Shaders failed in a core profile: opengl_profile=compatibility saved for the next start";
                        } catch (const std::exception& ex) {
                            BOOST_LOG_TRIVIAL(error) << "Saving opengl_profile failed: " << ex.what();
                        }
                    }
                }
                if (popup_error) {
                    wxString message = from_u8((boost::format(
                        _utf8(L("Unable to load shaders:\n%s"))) % error).str());
                    if (fallback_saved) {
                        message += "\n";
                        message += _L("Please restart the application. It will use the OpenGL compatibility profile from now on.");
                    }
                    wxMessageBox(message, _L("Error loading shaders"), wxOK | wxICON_ERROR);
                }
            }
        }

        // EDGE: the sampler-unit guard (GLShadersManager::init) covers one macOS failure; this
        // covers the rest of what a core profile rejects during start-up (context queries, the
        // default VAO, shader compilation and linking, uniform and sampler set-up).
        if (report_gl_errors("at start-up (context, default VAO, shaders)") == 0)
            BOOST_LOG_TRIVIAL(warning) << "OpenGL start-up check: no GL errors ("
                                       << (s_gl_info.is_core_profile() ? "core" : "compatibility/legacy")
                                       << " profile, default VAO " << s_default_vao << ")";

#ifdef _WIN32
        // Since AMD driver version 22.7.1, there is probably some bug in the driver that causes the issue with the missing
        // texture of the bed (see: https://github.com/prusa3d/PrusaSlicer/issues/8417).
        // It seems that this issue only triggers when mipmaps are generated manually
        // (combined with a texture compression) with texture size not being power of two.
        // When mipmaps are generated through OpenGL function glGenerateMipmap() the driver works fine,
        // but the mipmap generation is quite slow on some machines.
        // There is no an easy way to detect the driver version without using Win32 API because the strings returned by OpenGL
        // have no standardized format, only some of them contain the driver version.
        // Until we do not know that driver will be fixed (if ever) we force the use of power of two textures on all cards
        // 1) containing the string 'Radeon' in the string returned by glGetString(GL_RENDERER)
        // 2) containing the string 'Custom' in the string returned by glGetString(GL_RENDERER)
        const auto& gl_info = OpenGLManager::get_gl_info();
        if (boost::contains(gl_info.get_vendor(), "ATI Technologies Inc.") &&
           (boost::contains(gl_info.get_renderer(), "Radeon") ||
            boost::contains(gl_info.get_renderer(), "Custom")))
            s_force_power_of_two_textures = true;
#endif // _WIN32
    }

    return true;
}

// EDGE: which context init_glcontext() should ask for. "auto" (default): core, then compatibility,
// then the platform default. EDGESLICER_OPENGL_PROFILE in the environment wins over the app config's
// "opengl_profile" (which init_gl() also sets by itself when the shaders fail in a core profile).
static std::string requested_gl_profile(std::string& source)
{
    auto normalize = [](std::string value) {
        boost::algorithm::to_lower(value);
        boost::algorithm::trim(value);
        if (value == "core")
            return std::string("core");
        if (value == "compatibility" || value == "compat")
            return std::string("compatibility");
        if (value == "default" || value == "legacy")
            return std::string("default");
        return std::string("auto");
    };
    wxString env;
    if (wxGetEnv("EDGESLICER_OPENGL_PROFILE", &env) && !env.empty()) {
        source = "environment EDGESLICER_OPENGL_PROFILE";
        return normalize(env.ToStdString());
    }
    if (const AppConfig* app_config = get_app_config(); app_config != nullptr && app_config->has("opengl_profile")) {
        source = "app config opengl_profile";
        return normalize(app_config->get("opengl_profile"));
    }
    source = "default";
    return "auto";
}

// Core profile versions to try, highest first (OrcaSlicer #10735: OpenGLVersions::core). macOS
// only offers 3.2 and 4.1 core contexts, both forward-compatible; asking for more gives 4.1.
static const std::vector<std::pair<int, int>> s_core_gl_versions = { {4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0}, {3, 3}, {3, 2} };

wxGLContext* OpenGLManager::init_glcontext(wxGLCanvas& canvas)
{
    if (m_context == nullptr) {
        std::string source;
        const std::string requested = requested_gl_profile(source);

        // Silence wx's error dialog for every refused attempt: refusals are expected on the way down.
        wxLogNull logNo;
        auto try_create = [this, &canvas](const char* what, const std::function<void(wxGLContextAttrs&)>& setup) {
            wxGLContextAttrs attrs;
            attrs.PlatformDefaults();
            setup(attrs);
            attrs.EndList();
            m_context = new wxGLContext(&canvas, nullptr, &attrs);
            if (m_context->IsOK()) {
                // The context is not current yet: the version granted is logged at start-up ("OpenGL context:").
                BOOST_LOG_TRIVIAL(warning) << "init_glcontext: context created for the request " << what;
                s_context_request = what;
                return true;
            }
            delete m_context;
            m_context = nullptr;
            return false;
        };

        // 1) Core profile, highest version first, forward-compatible as macOS requires.
        if (requested == "auto" || requested == "core") {
            for (const auto& [major, minor] : s_core_gl_versions) {
                const std::string what = "forward-compatible core profile " + std::to_string(major) + "." + std::to_string(minor);
                if (try_create(what.c_str(), [major = major, minor = minor](wxGLContextAttrs& a) { a.MajorVersion(major).MinorVersion(minor).CoreProfile().ForwardCompatible(); }))
                    break;
            }
            if (m_context == nullptr)
                BOOST_LOG_TRIVIAL(warning) << "init_glcontext: no core profile context (4.6 down to 3.2) was granted";
        }
        // 2) Compatibility profile: what we ran on before the core switch (macOS: legacy 2.1 + 110 shaders).
        if (m_context == nullptr && requested != "default")
            try_create("compatibility profile", [](wxGLContextAttrs& a) { a.CompatibilityProfile(); });
        // 3) Whatever the platform hands out by default.
        if (m_context == nullptr) {
            BOOST_LOG_TRIVIAL(warning) << "init_glcontext: falling back to the platform default context";
            m_context = new wxGLContext(&canvas);
            s_context_request = "platform default context";
        }
        BOOST_LOG_TRIVIAL(warning) << "init_glcontext: profile request '" << requested << "' (from " << source
                                   << "); the GL context lines at OpenGL start-up show what was granted";

#ifdef __APPLE__
        // Part of hack to remove crash when closing the application on OSX 10.9.5 when building against newer wxWidgets
        s_os_info.major = wxPlatformInfo::Get().GetOSMajorVersion();
        s_os_info.minor = wxPlatformInfo::Get().GetOSMinorVersion();
        s_os_info.micro = wxPlatformInfo::Get().GetOSMicroVersion();
#endif //__APPLE__
    }
    return m_context;
}

wxGLCanvas* OpenGLManager::create_wxglcanvas(wxWindow& parent)
{
    int attribList[] = {
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
        WX_GL_SAMPLE_BUFFERS, 	GL_TRUE,
        WX_GL_SAMPLES, 			4,
        0
    };

    if (s_multisample == EMultisampleState::Unknown) {
        detect_multisample(attribList);
//        // debug output
//        std::cout << "Multisample " << (can_multisample() ? "enabled" : "disabled") << std::endl;
    }

    if (! can_multisample())
        attribList[12] = 0;

    BOOST_LOG_TRIVIAL(info) << "create_wxglcanvas: pixel format requests RGBA 8/8/8/8, depth 24"
                            << (can_multisample() ? ", stencil 8, 4x multisample" : " (stencil and multisample dropped: multisample unsupported)");

    return new wxGLCanvas(&parent, wxID_ANY, attribList, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS);
}

void OpenGLManager::detect_multisample(int* attribList)
{
    int wxVersion = wxMAJOR_VERSION * 10000 + wxMINOR_VERSION * 100 + wxRELEASE_NUMBER;
    bool enable_multisample = wxVersion >= 30003;
    s_multisample =
        enable_multisample &&
        // Disable multi-sampling on ChromeOS, as the OpenGL virtualization swaps Red/Blue channels with multi-sampling enabled,
        // at least on some platforms.
        platform_flavor() != PlatformFlavor::LinuxOnChromium &&
        wxGLCanvas::IsDisplaySupported(attribList)
        ? EMultisampleState::Enabled : EMultisampleState::Disabled;
    // Alternative method: it was working on previous version of wxWidgets but not with the latest, at least on Windows
    // s_multisample = enable_multisample && wxGLCanvas::IsExtensionSupported("WGL_ARB_multisample");
}

void OpenGLManager::bind_default_vao()
{
    if (s_gl_info.is_core_profile())
        glsafe(::glBindVertexArray(s_default_vao));
}

void OpenGLManager::set_line_width(float width)
{
    if (!s_gl_info.is_core_profile() || width <= 1.0f)
        glsafe(::glLineWidth(width));
}

static const char* gl_error_name(GLenum error)
{
    switch (error) {
    case GL_INVALID_ENUM:                  return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE:                 return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION:             return "GL_INVALID_OPERATION";
    case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
    case GL_OUT_OF_MEMORY:                 return "GL_OUT_OF_MEMORY";
    case GL_STACK_OVERFLOW:                return "GL_STACK_OVERFLOW";
    case GL_STACK_UNDERFLOW:               return "GL_STACK_UNDERFLOW";
    default:                               return "unknown GL error";
    }
}

int OpenGLManager::report_gl_errors(const std::string& where, unsigned int first_error)
{
    // The flag holds one error per kind until read; a bounded loop also ends on a lost context,
    // where some drivers keep returning GL_CONTEXT_LOST.
    std::map<GLenum, int> errors;
    int count = 0;
    if (first_error != GL_NO_ERROR) {
        ++errors[GLenum(first_error)];
        ++count;
    }
    for (int i = 0; i < 16; ++i) {
        const GLenum error = ::glGetError();
        if (error == GL_NO_ERROR)
            break;
        ++errors[error];
        ++count;
    }
    if (count == 0)
        return 0;

    static int reports_left = 40;
    if (reports_left > 0) {
        --reports_left;
        std::ostringstream out;
        for (const auto& [error, n] : errors)
            out << " " << gl_error_name(error) << boost::format(" (0x%04X)") % error << (n > 1 ? " x" + std::to_string(n) : std::string());
        BOOST_LOG_TRIVIAL(warning) << "OpenGL error(s) " << where << ":" << out.str()
                                   << " [" << (s_gl_info.is_core_profile() ? "core" : "compatibility/legacy") << " profile, GL "
                                   << s_gl_info.get_version() << "]";
        if (reports_left == 0)
            BOOST_LOG_TRIVIAL(warning) << "OpenGL error reports: limit reached, further errors are not logged this session";
    }
    return count;
}

} // namespace GUI
} // namespace Slic3r
