#include "BlenderLauncher.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace BlenderLauncher {

using namespace ExternalEditor;

bool parse_install_dir_version(const std::string &dir_name, int &major, int &minor)
{
    std::vector<int> version;
    if (!parse_dir_version(dir_name, "Blender ", version, 2, 2))
        return false;
    major = version[0];
    minor = version[1];
    return true;
}

std::string newest_install_dir(const std::vector<std::string> &dir_names)
{
    return newest_versioned_dir(dir_names, "Blender ", 2, 2);
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
    if (ends_with_nocase(trimmed, ".app"))
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

std::vector<std::string> find_blender(const std::string &custom_path)
{
    Environment env;
    env.exists    = &real_exists;
    env.list_dirs = &real_list_dirs;
    env.platform  = this_platform();
    if (env.platform == Platform::Windows) {
        env.program_files     = program_files_roots();
        env.blend_association = association_executable(".blend");
    } else {
        env.home            = env_var("HOME");
        env.blender_on_path = search_path("blender");
    }
    std::vector<std::string> command = discover(custom_path, env);
    BOOST_LOG_TRIVIAL(info) << "BlenderLauncher: " << (command.empty() ? std::string("Blender not found") : "using " + command.front());
    return command;
}

bool launch(const std::vector<std::string> &argv) { return ExternalEditor::launch(argv, "BlenderLauncher"); }

} // namespace BlenderLauncher
} // namespace Slic3r
