#include <catch2/catch.hpp>

#include <map>
#include <string>
#include <vector>

#include "slic3r/Utils/BlenderLauncher.hpp"

using namespace Slic3r::BlenderLauncher;

namespace {

FileExists exists_set(const std::vector<std::string> &existing)
{
    return [existing](const std::string &path) {
        for (const auto &p : existing)
            if (p == path)
                return true;
        return false;
    };
}

ListDirs dirs_map(const std::map<std::string, std::vector<std::string>> &dirs)
{
    return [dirs](const std::string &dir) {
        auto it = dirs.find(dir);
        return it == dirs.end() ? std::vector<std::string>() : it->second;
    };
}

Environment windows_env(const std::vector<std::string> &existing, const std::map<std::string, std::vector<std::string>> &dirs = {})
{
    Environment env;
    env.platform      = Platform::Windows;
    env.program_files = { "C:\\Program Files", "C:\\Program Files (x86)" };
    env.exists        = exists_set(existing);
    env.list_dirs     = dirs_map(dirs);
    return env;
}

} // namespace

TEST_CASE("Install folder versions compare numerically", "[BlenderLauncher]")
{
    int major = 0, minor = 0;
    REQUIRE(parse_install_dir_version("Blender 4.2", major, minor));
    CHECK(major == 4);
    CHECK(minor == 2);
    CHECK_FALSE(parse_install_dir_version("Blender", major, minor));
    CHECK_FALSE(parse_install_dir_version("Blender Launcher", major, minor));
    CHECK_FALSE(parse_install_dir_version("Blender 4.", major, minor));
    CHECK(newest_install_dir({ "Blender 3.6", "Blender 4.9", "Blender 4.10", "Other" }) == "Blender 4.10");
    CHECK(newest_install_dir({ "Other" }).empty());
}

TEST_CASE("The .blend association maps the launcher to blender.exe", "[BlenderLauncher]")
{
    const std::string dir = "C:\\Program Files\\Blender Foundation\\Blender 4.2";
    CHECK(blender_exe_from_association(dir + "\\blender-launcher.exe", exists_set({ dir + "\\blender.exe" })) == dir + "\\blender.exe");
    // Without blender.exe beside it the launcher itself still starts Blender.
    CHECK(blender_exe_from_association(dir + "\\blender-launcher.exe", exists_set({})) == dir + "\\blender-launcher.exe");
    // An unrelated .blend handler is never used.
    CHECK(blender_exe_from_association("C:\\Tools\\viewer.exe", exists_set({ "C:\\Tools\\viewer.exe" })).empty());
}

TEST_CASE("Windows discovery order", "[BlenderLauncher]")
{
    const std::string foundation = "C:\\Program Files\\Blender Foundation";
    const std::string newest     = foundation + "\\Blender 4.2\\blender.exe";
    const std::string custom     = "D:\\Apps\\Blender\\blender.exe";

    SECTION("custom path wins when it exists")
    {
        auto env = windows_env({ custom, newest }, { { foundation, { "Blender 4.2" } } });
        CHECK(discover(custom, env) == std::vector<std::string>{ custom });
    }
    SECTION("a missing custom path falls through to discovery")
    {
        auto env = windows_env({ newest }, { { foundation, { "Blender 4.2" } } });
        CHECK(discover(custom, env) == std::vector<std::string>{ newest });
    }
    SECTION("the association comes before scanning Program Files")
    {
        const std::string assoc_dir = "E:\\Blender 4.5";
        auto env = windows_env({ assoc_dir + "\\blender.exe", newest }, { { foundation, { "Blender 4.2" } } });
        env.blend_association = assoc_dir + "\\blender-launcher.exe";
        CHECK(discover("", env) == std::vector<std::string>{ assoc_dir + "\\blender.exe" });
    }
    SECTION("the newest installed version is picked")
    {
        auto env = windows_env({ foundation + "\\Blender 3.6\\blender.exe", newest }, { { foundation, { "Blender 3.6", "Blender 4.2" } } });
        CHECK(discover("", env) == std::vector<std::string>{ newest });
    }
    SECTION("Steam is tried last")
    {
        const std::string steam = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Blender\\blender.exe";
        auto env = windows_env({ steam });
        CHECK(discover("", env) == std::vector<std::string>{ steam });
    }
    SECTION("nothing installed")
    {
        CHECK(discover("", windows_env({})).empty());
    }
}

TEST_CASE("macOS discovery uses the binary inside the bundle", "[BlenderLauncher]")
{
    Environment env;
    env.platform  = Platform::MacOS;
    env.home      = "/Users/lance";
    env.list_dirs = dirs_map({});

    CHECK(macos_bundle_binary("/Applications/Blender.app/") == "/Applications/Blender.app/Contents/MacOS/Blender");
    CHECK(macos_bundle_binary("/opt/blender") == "/opt/blender");

    env.exists = exists_set({ "/Users/lance/Applications/Blender.app/Contents/MacOS/Blender" });
    CHECK(discover("", env) == std::vector<std::string>{ "/Users/lance/Applications/Blender.app/Contents/MacOS/Blender" });

    env.exists = exists_set({ "/Volumes/X/Blender.app/Contents/MacOS/Blender", "/Applications/Blender.app/Contents/MacOS/Blender" });
    CHECK(discover("/Volumes/X/Blender.app", env) == std::vector<std::string>{ "/Volumes/X/Blender.app/Contents/MacOS/Blender" });
}

TEST_CASE("Linux discovery falls back to Snap and Flatpak", "[BlenderLauncher]")
{
    Environment env;
    env.platform  = Platform::Linux;
    env.home      = "/home/lance";
    env.list_dirs = dirs_map({});

    env.blender_on_path = "/usr/bin/blender";
    env.exists          = exists_set({ "/snap/bin/blender" });
    CHECK(discover("", env) == std::vector<std::string>{ "/usr/bin/blender" });

    env.blender_on_path.clear();
    CHECK(discover("", env) == std::vector<std::string>{ "/snap/bin/blender" });

    env.exists = exists_set({ "/home/lance/.local/share/flatpak/app/org.blender.Blender" });
    CHECK(discover("", env) == std::vector<std::string>{ "flatpak", "run", "org.blender.Blender" });

    env.exists = exists_set({});
    CHECK(discover("", env).empty());
}

TEST_CASE("Edit session arguments", "[BlenderLauncher]")
{
    EditSession session;
    session.script         = "C:\\EdgeSlicer\\resources\\blender\\edgeslicer_bridge.py";
    session.input          = "C:\\Users\\me\\bridge\\part.stl";
    session.output         = "C:\\Users\\me\\bridge\\edited.stl";
    session.name           = "Bracket v2";
    session.edgeslicer_exe = "C:\\Program Files\\EdgeSlicer\\EdgeSlicer.exe";

    std::vector<std::string> expected = { "flatpak", "run", "org.blender.Blender",
                                          "--python", session.script, "--",
                                          "--edgeslicer-in", session.input,
                                          "--edgeslicer-out", session.output,
                                          "--edgeslicer-name", session.name,
                                          "--edgeslicer-exe", session.edgeslicer_exe };
    CHECK(edit_session_args({ "flatpak", "run", "org.blender.Blender" }, session) == expected);

    session.name.clear();
    session.edgeslicer_exe.clear();
    CHECK(edit_session_args({ "blender" }, session) ==
          std::vector<std::string>{ "blender", "--python", session.script, "--", "--edgeslicer-in", session.input, "--edgeslicer-out", session.output });
}
