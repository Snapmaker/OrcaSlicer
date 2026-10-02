#include "FreeCADLauncher.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iterator>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Slic3r {
namespace FreeCADLauncher {

using namespace ExternalEditor;

const char *ADDON_FOLDER  = "EdgeSlicerBridge";
const char *LOCATION_FILE = "edgeslicer_location.txt";

namespace {

// Windows paths are joined with a backslash, the others with a slash.
std::string join(const std::string &dir, const std::string &name, Platform platform)
{
    return strip_trailing_separators(dir) + (platform == Platform::Windows ? "\\" : "/") + name;
}

// Strips surrounding quotes and a trailing ",<icon index>" from a registry path value.
std::string clean_registry_path(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    if (!value.empty() && value.front() == '"') {
        size_t close = value.find('"', 1);
        return value.substr(1, close == std::string::npos ? std::string::npos : close - 1);
    }
    size_t comma = value.rfind(',');
    if (comma != std::string::npos && comma + 1 < value.size() &&
        std::all_of(value.begin() + comma + 1, value.end(), [](unsigned char c) { return std::isdigit(c) || c == '-'; }))
        value.erase(comma);
    return value;
}

std::string registry_version(const RegistryInstall &entry)
{
    if (!entry.display_version.empty())
        return entry.display_version;
    // "FreeCAD 0.21.2" -> "0.21.2"
    size_t space = entry.display_name.rfind(' ');
    return space == std::string::npos ? std::string() : entry.display_name.substr(space + 1);
}

} // namespace

bool parse_install_dir_version(const std::string &dir_name, std::vector<int> &version)
{
    return parse_dir_version(dir_name, "FreeCAD ", version, 2, 3);
}

std::string newest_install_dir(const std::vector<std::string> &dir_names)
{
    return newest_versioned_dir(dir_names, "FreeCAD ", 2, 3);
}

std::string macos_bundle_binary(const std::string &path)
{
    std::string trimmed = strip_trailing_separators(path);
    if (ends_with_nocase(trimmed, ".app"))
        return trimmed + "/Contents/MacOS/FreeCAD";
    return path;
}

std::string resolve_custom_path(const std::string &path, Platform platform, const FileExists &exists)
{
    if (path.empty())
        return {};
    if (platform == Platform::MacOS) {
        std::string exe = macos_bundle_binary(path);
        return exists(exe) ? exe : std::string();
    }
    if (platform == Platform::Windows) {
        if (ends_with_nocase(path, ".exe"))
            return exists(path) ? path : std::string();
        // The install folder or its bin folder.
        for (const char *tail : { "bin\\FreeCAD.exe", "FreeCAD.exe" }) {
            std::string exe = join(path, tail, platform);
            if (exists(exe))
                return exe;
        }
        return {};
    }
    return exists(path) ? path : std::string();
}

std::string freecad_exe_from_association(const std::string &exe)
{
    return to_lower(file_name(exe)) == "freecad.exe" ? exe : std::string();
}

std::string freecad_exe_from_registry(const RegistryInstall &entry, const FileExists &exists)
{
    std::vector<std::string> candidates;
    std::string icon = clean_registry_path(entry.display_icon);
    if (!icon.empty() && to_lower(file_name(icon)) == "freecad.exe")
        candidates.push_back(icon);
    if (!entry.install_location.empty())
        candidates.push_back(join(clean_registry_path(entry.install_location), "bin\\FreeCAD.exe", Platform::Windows));
    std::string uninstaller = clean_registry_path(entry.uninstall_string);
    if (!uninstaller.empty() && !parent_dir(uninstaller).empty())
        candidates.push_back(join(parent_dir(uninstaller), "bin\\FreeCAD.exe", Platform::Windows));
    for (const std::string &exe : candidates)
        if (exists(exe))
            return exe;
    return {};
}

std::vector<std::string> discover(const std::string &custom_path, const Environment &env)
{
    if (std::string exe = resolve_custom_path(custom_path, env.platform, env.exists); !exe.empty())
        return { exe };

    switch (env.platform) {
    case Platform::Windows: {
        if (std::string exe = freecad_exe_from_association(env.fcstd_association); !exe.empty() && env.exists(exe))
            return { exe };
        // Newest registered install first.
        std::string best, best_version;
        for (const RegistryInstall &entry : env.registry) {
            if (!starts_with_nocase(entry.display_name, "FreeCAD"))
                continue;
            std::string exe = freecad_exe_from_registry(entry, env.exists);
            if (exe.empty())
                continue;
            std::string version = registry_version(entry);
            if (best.empty() || compare_versions(version, best_version) > 0) {
                best         = exe;
                best_version = version;
            }
        }
        if (!best.empty())
            return { best };
        std::vector<std::string> roots = env.program_files;
        if (!env.local_app_data.empty())
            roots.push_back(join(env.local_app_data, "Programs", Platform::Windows));
        for (const std::string &root : roots) {
            std::string newest = newest_install_dir(env.list_dirs(root));
            for (const std::string &dir : { newest, std::string("FreeCAD") }) {
                if (dir.empty())
                    continue;
                std::string exe = join(join(root, dir, Platform::Windows), "bin\\FreeCAD.exe", Platform::Windows);
                if (env.exists(exe))
                    return { exe };
            }
        }
        return {};
    }
    case Platform::MacOS: {
        std::vector<std::string> bundles = { "/Applications/FreeCAD.app" };
        if (!env.home.empty())
            bundles.push_back(strip_trailing_separators(env.home) + "/Applications/FreeCAD.app");
        // "FreeCAD 1.0.app" and the like, newest first.
        std::vector<std::string> versioned;
        for (const std::string &name : env.list_dirs("/Applications"))
            if (ends_with_nocase(name, ".app"))
                versioned.push_back(name.substr(0, name.size() - 4));
        if (std::string newest = newest_install_dir(versioned); !newest.empty())
            bundles.push_back("/Applications/" + newest + ".app");
        for (const std::string &bundle : bundles) {
            std::string exe = macos_bundle_binary(bundle);
            if (env.exists(exe))
                return { exe };
        }
        if (!env.freecad_on_path.empty())
            return { env.freecad_on_path };
        return {};
    }
    case Platform::Linux: {
        if (!env.freecad_on_path.empty())
            return { env.freecad_on_path };
        if (env.exists("/snap/bin/freecad"))
            return { "/snap/bin/freecad" };
        const std::string flatpak_id = "org.freecad.FreeCAD";
        bool flatpak = env.exists("/var/lib/flatpak/app/" + flatpak_id) ||
                       (!env.home.empty() && env.exists(strip_trailing_separators(env.home) + "/.local/share/flatpak/app/" + flatpak_id));
        if (flatpak)
            return { "flatpak", "run", flatpak_id };
        return {};
    }
    }
    return {};
}

std::vector<std::string> edit_session_args(const std::vector<std::string> &freecad_command, const std::string &session_macro)
{
    std::vector<std::string> argv = freecad_command;
    argv.insert(argv.end(), { "--single-instance", session_macro });
    return argv;
}

std::string python_string_literal(const std::string &utf8)
{
    std::string out = "\"";
    for (char c : utf8) {
        unsigned char u = static_cast<unsigned char>(c);
        if (c == '\\')
            out += "\\\\";
        else if (c == '"')
            out += "\\\"";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else if (u < 0x20 || u == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", u);
            out += buf;
        } else
            out += c; // UTF-8 bytes pass through; the macro is UTF-8 source.
    }
    return out + "\"";
}

std::string session_macro(const std::string &bridge_dir, const std::string &session_json)
{
    return "# -*- coding: utf-8 -*-\n"
           "# Written by EdgeSlicer for one \"Edit in FreeCAD\" session. FreeCAD runs it when it is given\n"
           "# this file: it opens the part, and saving the document sends the part back to EdgeSlicer.\n"
           "import importlib.util\n"
           "import os\n"
           "import sys\n"
           "\n"
           "\n"
           "def _edgeslicer_bridge(bridge_dir):\n"
           "    # The copy that came with this EdgeSlicer, unless the installed add-on is as new.\n"
           "    spec = importlib.util.spec_from_file_location(\"edgeslicer_bridge\", os.path.join(bridge_dir, \"edgeslicer_bridge.py\"))\n"
           "    bundled = importlib.util.module_from_spec(spec)\n"
           "    spec.loader.exec_module(bundled)\n"
           "    loaded = sys.modules.get(\"edgeslicer_bridge\")\n"
           "    if loaded is not None and tuple(getattr(loaded, \"VERSION\", (0,))) >= tuple(bundled.VERSION):\n"
           "        return loaded\n"
           "    sys.modules[\"edgeslicer_bridge\"] = bundled\n"
           "    return bundled\n"
           "\n"
           "\n"
           "_edgeslicer_bridge(" + python_string_literal(bridge_dir) + ").start_edit_session(" + python_string_literal(session_json) + ")\n";
}

bool is_versioned_user_dir(const std::string &name)
{
    // v<digits>-<digits>
    if (name.size() < 4 || (name[0] != 'v' && name[0] != 'V'))
        return false;
    size_t dash = name.find('-');
    if (dash == std::string::npos || dash == 1 || dash + 1 >= name.size())
        return false;
    for (size_t i = 1; i < name.size(); ++i)
        if (i != dash && !std::isdigit(static_cast<unsigned char>(name[i])))
            return false;
    return true;
}

std::vector<std::string> addon_mod_dirs(Platform platform, const std::string &base, const std::string &xdg_data_home,
                                        const FileExists &exists, const ListDirs &list_dirs)
{
    std::vector<std::string> out;
    if (base.empty())
        return out;
    auto add_user_root = [&](const std::string &root) {
        out.push_back(join(root, "Mod", platform));
        std::vector<std::string> names = list_dirs(root);
        std::sort(names.begin(), names.end());
        for (const std::string &name : names)
            if (is_versioned_user_dir(name))
                out.push_back(join(join(root, name, platform), "Mod", platform));
    };
    const std::string home = strip_trailing_separators(base);
    switch (platform) {
    case Platform::Windows: add_user_root(home + "\\FreeCAD"); break;
    case Platform::MacOS: add_user_root(home + "/Library/Application Support/FreeCAD"); break;
    case Platform::Linux: {
        const std::string data = xdg_data_home.empty() ? home + "/.local/share" : strip_trailing_separators(xdg_data_home);
        add_user_root(data + "/FreeCAD");
        if (exists(home + "/.FreeCAD"))
            out.push_back(home + "/.FreeCAD/Mod");
        if (exists(home + "/.var/app/org.freecad.FreeCAD"))
            add_user_root(home + "/.var/app/org.freecad.FreeCAD/data/FreeCAD");
        break;
    }
    }
    return out;
}

namespace {

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

// Recursive copy that skips Python byte-code caches.
void copy_tree(const boost::filesystem::path &from, const boost::filesystem::path &to, boost::system::error_code &ec)
{
    boost::filesystem::create_directories(to, ec);
    if (ec)
        return;
    for (boost::filesystem::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const boost::filesystem::path &src  = it->path();
        const std::string              name = src.filename().string();
        if (name == "__pycache__")
            continue;
        if (boost::filesystem::is_directory(src, ec))
            copy_tree(src, to / src.filename(), ec);
        else
            boost::filesystem::copy_file(src, to / src.filename(), boost::filesystem::copy_option::overwrite_if_exists, ec);
        if (ec)
            return;
    }
}

} // namespace

bool install_addon(const std::string &source_dir, const std::vector<std::string> &mod_dirs, const std::string &edgeslicer_exe, InstallReport &report)
{
    report = InstallReport{};
    boost::system::error_code ec;
    const boost::filesystem::path source = to_path(source_dir);
    if (!boost::filesystem::is_directory(source, ec)) {
        report.errors.push_back("the add-on is missing from this installation: " + source_dir);
        return false;
    }
    for (const std::string &mod_dir : mod_dirs) {
        const boost::filesystem::path target = to_path(mod_dir) / ADDON_FOLDER;
        ec.clear();
        // An older copy may hold files this version no longer has.
        boost::filesystem::remove_all(target, ec);
        ec.clear();
        copy_tree(source, target, ec);
        if (!ec && !edgeslicer_exe.empty()) {
            boost::nowide::ofstream out(from_path(target / LOCATION_FILE), std::ios::binary | std::ios::trunc);
            out << edgeslicer_exe << "\n";
            if (!out)
                ec = boost::system::errc::make_error_code(boost::system::errc::io_error);
        }
        if (ec)
            report.errors.push_back(from_path(target) + ": " + ec.message());
        else
            report.installed.push_back(from_path(target));
    }
    return !report.installed.empty() && report.errors.empty();
}

// -------------------------------------------------------------------------------------------
// Platform adapters
// -------------------------------------------------------------------------------------------

namespace {

#ifdef _WIN32
std::string reg_string(HKEY key, const wchar_t *name)
{
    wchar_t buf[2048];
    DWORD   size = sizeof(buf);
    DWORD   type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ))
        return {};
    buf[std::min<size_t>(size / sizeof(wchar_t), std::size(buf) - 1)] = 0;
    return boost::nowide::narrow(buf);
}

void read_uninstall_entries(HKEY root, REGSAM view, std::vector<RegistryInstall> &out)
{
    HKEY uninstall = nullptr;
    if (RegOpenKeyExW(root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | view, &uninstall) != ERROR_SUCCESS)
        return;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = static_cast<DWORD>(std::size(name));
        if (RegEnumKeyExW(uninstall, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        HKEY entry = nullptr;
        if (RegOpenKeyExW(uninstall, name, 0, KEY_READ | view, &entry) != ERROR_SUCCESS)
            continue;
        RegistryInstall install;
        install.display_name = reg_string(entry, L"DisplayName");
        if (starts_with_nocase(install.display_name, "FreeCAD")) {
            install.display_version  = reg_string(entry, L"DisplayVersion");
            install.display_icon     = reg_string(entry, L"DisplayIcon");
            install.install_location = reg_string(entry, L"InstallLocation");
            install.uninstall_string = reg_string(entry, L"UninstallString");
            out.push_back(std::move(install));
        }
        RegCloseKey(entry);
    }
    RegCloseKey(uninstall);
}
#endif

} // namespace

std::vector<std::string> find_freecad(const std::string &custom_path)
{
    Environment env;
    env.exists    = &real_exists;
    env.list_dirs = &real_list_dirs;
    env.platform  = this_platform();
#ifdef _WIN32
    env.program_files     = program_files_roots();
    env.local_app_data    = env_var("LOCALAPPDATA");
    env.fcstd_association = association_executable(".FCStd");
    for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
        for (REGSAM view : { KEY_WOW64_64KEY, KEY_WOW64_32KEY })
            read_uninstall_entries(root, view, env.registry);
#else
    env.home            = env_var("HOME");
    env.freecad_on_path = search_path("freecad");
    if (env.freecad_on_path.empty())
        env.freecad_on_path = search_path("FreeCAD");
#endif
    std::vector<std::string> command = discover(custom_path, env);
    BOOST_LOG_TRIVIAL(info) << "FreeCADLauncher: " << (command.empty() ? std::string("FreeCAD not found") : "using " + command.front());
    return command;
}

std::vector<std::string> find_addon_mod_dirs()
{
    const Platform platform = this_platform();
    const std::string base  = platform == Platform::Windows ? env_var("APPDATA") : env_var("HOME");
    return addon_mod_dirs(platform, base, platform == Platform::Linux ? env_var("XDG_DATA_HOME") : std::string(), &real_exists, &real_list_dirs);
}

bool launch(const std::vector<std::string> &argv) { return ExternalEditor::launch(argv, "FreeCADLauncher"); }

} // namespace FreeCADLauncher
} // namespace Slic3r
