#include "BlenderLauncher.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>

#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <iterator>
#include <wx/utils.h>
#include <wx/string.h>
#else
#include <boost/process/args.hpp>
#include <boost/process/search_path.hpp>
#include <boost/process/spawn.hpp>
#endif

namespace Slic3r {
namespace BlenderLauncher {

namespace {

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string file_name(const std::string &path)
{
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parent_dir(const std::string &path)
{
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool ends_with(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string strip_trailing_separators(std::string path)
{
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
        path.pop_back();
    return path;
}

} // namespace

bool parse_install_dir_version(const std::string &dir_name, int &major, int &minor)
{
    const std::string prefix = "blender ";
    std::string       lower  = to_lower(dir_name);
    if (lower.compare(0, prefix.size(), prefix) != 0)
        return false;
    std::string rest = lower.substr(prefix.size());
    size_t      dot  = rest.find('.');
    if (dot == 0 || dot == std::string::npos || dot + 1 >= rest.size())
        return false;
    for (size_t i = 0; i < rest.size(); ++i)
        if (i != dot && !std::isdigit(static_cast<unsigned char>(rest[i])))
            return false;
    major = std::atoi(rest.substr(0, dot).c_str());
    minor = std::atoi(rest.substr(dot + 1).c_str());
    return true;
}

std::string newest_install_dir(const std::vector<std::string> &dir_names)
{
    std::string best;
    int         best_major = -1, best_minor = -1;
    for (const std::string &name : dir_names) {
        int major, minor;
        if (!parse_install_dir_version(name, major, minor))
            continue;
        if (major > best_major || (major == best_major && minor > best_minor)) {
            best       = name;
            best_major = major;
            best_minor = minor;
        }
    }
    return best;
}

std::string blender_exe_from_association(const std::string &exe, const FileExists &exists)
{
    std::string name = to_lower(file_name(exe));
    if (name == "blender.exe")
        return exe;
    if (name != "blender-launcher.exe")
        return {};
    std::string sibling = parent_dir(exe) + "\\blender.exe";
    return exists(sibling) ? sibling : exe;
}

std::string macos_bundle_binary(const std::string &path)
{
    std::string trimmed = strip_trailing_separators(path);
    if (ends_with(to_lower(trimmed), ".app"))
        return trimmed + "/Contents/MacOS/Blender";
    return path;
}

std::vector<std::string> discover(const std::string &custom_path, const Environment &env)
{
    if (!custom_path.empty()) {
        std::string exe = env.platform == Platform::MacOS ? macos_bundle_binary(custom_path) : custom_path;
        if (env.exists(exe))
            return { exe };
    }

    switch (env.platform) {
    case Platform::Windows: {
        if (!env.blend_association.empty()) {
            std::string exe = blender_exe_from_association(env.blend_association, env.exists);
            if (!exe.empty() && env.exists(exe))
                return { exe };
        }
        for (const std::string &root : env.program_files) {
            std::string foundation = root + "\\Blender Foundation";
            std::string newest     = newest_install_dir(env.list_dirs(foundation));
            if (!newest.empty()) {
                std::string exe = foundation + "\\" + newest + "\\blender.exe";
                if (env.exists(exe))
                    return { exe };
            }
        }
        for (const std::string &root : env.program_files) {
            std::string exe = root + "\\Steam\\steamapps\\common\\Blender\\blender.exe";
            if (env.exists(exe))
                return { exe };
        }
        return {};
    }
    case Platform::MacOS: {
        std::vector<std::string> bundles = { "/Applications/Blender.app" };
        if (!env.home.empty())
            bundles.push_back(strip_trailing_separators(env.home) + "/Applications/Blender.app");
        for (const std::string &bundle : bundles) {
            std::string exe = macos_bundle_binary(bundle);
            if (env.exists(exe))
                return { exe };
        }
        if (!env.blender_on_path.empty())
            return { env.blender_on_path };
        return {};
    }
    case Platform::Linux: {
        if (!env.blender_on_path.empty())
            return { env.blender_on_path };
        if (env.exists("/snap/bin/blender"))
            return { "/snap/bin/blender" };
        const std::string flatpak_id = "org.blender.Blender";
        bool flatpak = env.exists("/var/lib/flatpak/app/" + flatpak_id) ||
                       (!env.home.empty() && env.exists(strip_trailing_separators(env.home) + "/.local/share/flatpak/app/" + flatpak_id));
        if (flatpak)
            return { "flatpak", "run", flatpak_id };
        return {};
    }
    }
    return {};
}

std::vector<std::string> edit_session_args(const std::vector<std::string> &blender_command, const EditSession &session)
{
    std::vector<std::string> argv = blender_command;
    argv.insert(argv.end(), { "--python", session.script, "--",
                              "--edgeslicer-in", session.input,
                              "--edgeslicer-out", session.output });
    if (!session.name.empty())
        argv.insert(argv.end(), { "--edgeslicer-name", session.name });
    if (!session.edgeslicer_exe.empty())
        argv.insert(argv.end(), { "--edgeslicer-exe", session.edgeslicer_exe });
    return argv;
}

// -------------------------------------------------------------------------------------------
// Platform adapters
// -------------------------------------------------------------------------------------------

namespace {

// Paths are UTF-8 throughout; only Windows needs them widened for the filesystem calls.
boost::filesystem::path to_path(const std::string &utf8)
{
#ifdef _WIN32
    return boost::filesystem::path(boost::nowide::widen(utf8));
#else
    return boost::filesystem::path(utf8);
#endif
}

std::string from_path(const boost::filesystem::path &path)
{
#ifdef _WIN32
    return boost::nowide::narrow(path.wstring());
#else
    return path.string();
#endif
}

bool real_exists(const std::string &path)
{
    boost::system::error_code ec;
    return boost::filesystem::exists(to_path(path), ec);
}

std::vector<std::string> real_list_dirs(const std::string &dir)
{
    std::vector<std::string> names;
    boost::system::error_code ec;
    boost::filesystem::path   root = to_path(dir);
    if (!boost::filesystem::is_directory(root, ec))
        return names;
    for (boost::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (boost::filesystem::is_directory(it->path(), ec))
            names.push_back(from_path(it->path().filename()));
    return names;
}

std::string env_var(const char *name)
{
#ifdef _WIN32
    const wchar_t *value = _wgetenv(boost::nowide::widen(name).c_str());
    return value ? boost::nowide::narrow(value) : std::string();
#else
    const char *value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

#ifdef _WIN32
std::string blend_association()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD   size = static_cast<DWORD>(std::size(buf));
    if (FAILED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, L".blend", nullptr, buf, &size)))
        return {};
    return boost::nowide::narrow(buf);
}
#endif

} // namespace

std::vector<std::string> find_blender(const std::string &custom_path)
{
    Environment env;
    env.exists    = &real_exists;
    env.list_dirs = &real_list_dirs;
#ifdef _WIN32
    env.platform = Platform::Windows;
    for (const char *var : { "ProgramW6432", "ProgramFiles", "ProgramFiles(x86)" }) {
        std::string root = env_var(var);
        if (!root.empty() && std::find(env.program_files.begin(), env.program_files.end(), root) == env.program_files.end())
            env.program_files.push_back(root);
    }
    env.blend_association = blend_association();
#else
#ifdef __APPLE__
    env.platform = Platform::MacOS;
#else
    env.platform = Platform::Linux;
#endif
    env.home            = env_var("HOME");
    env.blender_on_path = boost::process::search_path("blender").string();
#endif
    std::vector<std::string> command = discover(custom_path, env);
    BOOST_LOG_TRIVIAL(info) << "BlenderLauncher: " << (command.empty() ? std::string("Blender not found") : "using " + command.front());
    return command;
}

bool launch(const std::vector<std::string> &argv)
{
    if (argv.empty())
        return false;
#ifdef _WIN32
    // wxExecute takes the arguments as an array, so paths with spaces or quotes need no escaping.
    std::vector<wxString> args;
    for (const std::string &arg : argv)
        args.emplace_back(wxString::FromUTF8(arg.c_str()));
    std::vector<const wchar_t *> raw;
    for (const wxString &arg : args)
        raw.push_back(arg.wc_str());
    raw.push_back(nullptr);
    long pid = ::wxExecute(const_cast<wchar_t **>(raw.data()), wxEXEC_ASYNC);
    if (pid <= 0) {
        BOOST_LOG_TRIVIAL(error) << "BlenderLauncher: failed to start \"" << argv.front() << "\"";
        return false;
    }
#else
    // Same as BambuStudioLauncher: wxExecute is unreliable on macOS, so boost::process is used on
    // both Unix platforms.
    try {
        boost::filesystem::path exe = argv.front().find('/') != std::string::npos ? boost::filesystem::path(argv.front()) :
                                                                                  boost::process::search_path(argv.front());
        if (exe.empty())
            return false;
        boost::process::spawn(exe, boost::process::args(std::vector<std::string>(argv.begin() + 1, argv.end())));
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "BlenderLauncher: failed to start \"" << argv.front() << "\": " << ex.what();
        return false;
    }
#endif
    BOOST_LOG_TRIVIAL(info) << "BlenderLauncher: started \"" << argv.front() << "\"";
    return true;
}

} // namespace BlenderLauncher
} // namespace Slic3r
