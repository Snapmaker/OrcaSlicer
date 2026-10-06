#include <catch2/catch.hpp>

#include <map>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "slic3r/Utils/ExternalEditor.hpp"
#include "slic3r/Utils/FreeCADLauncher.hpp"

using namespace Slic3r::FreeCADLauncher;
namespace ExternalEditor = Slic3r::ExternalEditor;

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
    env.platform       = Platform::Windows;
    env.program_files  = { "C:\\Program Files", "C:\\Program Files (x86)" };
    env.local_app_data = "C:\\Users\\me\\AppData\\Local";
    env.exists         = exists_set(existing);
    env.list_dirs      = dirs_map(dirs);
    return env;
}

// What the FreeCAD 1.0.2 installer writes (HKLM\...\Uninstall\FreeCAD102), as found on a real machine.
RegistryInstall freecad_102()
{
    RegistryInstall entry;
    entry.display_name     = "FreeCAD 1.0.2";
    entry.display_version  = "1.0.2";
    entry.display_icon     = "C:\\Program Files\\FreeCAD 1.0\\bin\\FreeCAD.exe";
    entry.uninstall_string = "\"C:\\Program Files\\FreeCAD 1.0\\Uninstall-FreeCAD.exe\"";
    return entry;
}

struct TempDir
{
    boost::filesystem::path path;
    TempDir() : path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("edge_freecad_%%%%-%%%%-%%%%"))
    {
        boost::filesystem::create_directories(path);
    }
    ~TempDir()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(path, ec);
    }
};

void write_file(const boost::filesystem::path &path, const std::string &text)
{
    boost::filesystem::create_directories(path.parent_path());
    boost::nowide::ofstream out(path.string(), std::ios::binary);
    out << text;
}

std::string read_file(const boost::filesystem::path &path)
{
    boost::nowide::ifstream in(path.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("Shared external-editor helpers", "[ExternalEditor]")
{
    std::vector<int> v;
    REQUIRE(ExternalEditor::parse_dir_version("FreeCAD 0.21.2", "FreeCAD ", v, 2, 3));
    CHECK(v == std::vector<int>{ 0, 21, 2 });
    CHECK(ExternalEditor::parse_dir_version("freecad 1.0", "FreeCAD ", v, 2, 3));
    CHECK_FALSE(ExternalEditor::parse_dir_version("FreeCAD 1.0.2.4", "FreeCAD ", v, 2, 3));
    CHECK_FALSE(ExternalEditor::parse_dir_version("FreeCAD 1..2", "FreeCAD ", v, 2, 3));
    CHECK_FALSE(ExternalEditor::parse_dir_version("FreeCAD", "FreeCAD ", v, 2, 3));
    CHECK(ExternalEditor::compare_versions("1.0.2", "1.1") < 0);
    CHECK(ExternalEditor::compare_versions("1.10", "1.9") > 0);
    CHECK(ExternalEditor::compare_versions("0.21", "0.21.0") == 0);
    CHECK(ExternalEditor::ends_with_nocase("C:\\X\\FreeCAD.EXE", ".exe"));
    CHECK(ExternalEditor::file_name("C:/a/b\\FreeCAD.exe") == "FreeCAD.exe");
}

TEST_CASE("FreeCAD install folders compare by version", "[FreeCADLauncher]")
{
    std::vector<int> version;
    REQUIRE(parse_install_dir_version("FreeCAD 1.0", version));
    CHECK(version == std::vector<int>{ 1, 0 });
    CHECK_FALSE(parse_install_dir_version("FreeCAD", version));
    CHECK_FALSE(parse_install_dir_version("FreeCAD Link", version));
    CHECK(newest_install_dir({ "FreeCAD 0.21", "FreeCAD 1.0", "FreeCAD 0.19", "Other" }) == "FreeCAD 1.0");
    CHECK(newest_install_dir({ "FreeCAD 1.0", "FreeCAD 1.0.2" }) == "FreeCAD 1.0.2");
    CHECK(newest_install_dir({ "Other" }).empty());
}

TEST_CASE("A typed FreeCAD location may be the exe, its bin folder or the install folder", "[FreeCADLauncher]")
{
    const std::string exe    = "D:\\Apps\\FreeCAD 1.0\\bin\\FreeCAD.exe";
    const auto        exists = exists_set({ exe });
    CHECK(resolve_custom_path(exe, Platform::Windows, exists) == exe);
    CHECK(resolve_custom_path("D:\\Apps\\FreeCAD 1.0", Platform::Windows, exists) == exe);
    CHECK(resolve_custom_path("D:\\Apps\\FreeCAD 1.0\\", Platform::Windows, exists) == exe);
    CHECK(resolve_custom_path("D:\\Apps\\FreeCAD 1.0\\bin", Platform::Windows, exists) == exe);
    CHECK(resolve_custom_path("D:\\Apps\\Missing\\FreeCAD.exe", Platform::Windows, exists).empty());
    CHECK(resolve_custom_path("", Platform::Windows, exists).empty());
    CHECK(resolve_custom_path("/Volumes/X/FreeCAD.app/", Platform::MacOS, exists_set({ "/Volumes/X/FreeCAD.app/Contents/MacOS/FreeCAD" })) ==
          "/Volumes/X/FreeCAD.app/Contents/MacOS/FreeCAD");
    CHECK(resolve_custom_path("/opt/FreeCAD.AppImage", Platform::Linux, exists_set({ "/opt/FreeCAD.AppImage" })) == "/opt/FreeCAD.AppImage");
}

TEST_CASE("Registry entries map to FreeCAD.exe", "[FreeCADLauncher]")
{
    const std::string exe = "C:\\Program Files\\FreeCAD 1.0\\bin\\FreeCAD.exe";
    CHECK(freecad_exe_from_registry(freecad_102(), exists_set({ exe })) == exe);

    // An icon with an index and no exe there: the uninstaller's folder still leads to it.
    RegistryInstall entry = freecad_102();
    entry.display_icon    = "C:\\Program Files\\FreeCAD 1.0\\FreeCAD.ico,0";
    CHECK(freecad_exe_from_registry(entry, exists_set({ exe })) == exe);

    // 0.21-style entry with InstallLocation only.
    RegistryInstall old;
    old.display_name     = "FreeCAD 0.21.2";
    old.install_location = "C:\\Program Files\\FreeCAD 0.21";
    CHECK(freecad_exe_from_registry(old, exists_set({ "C:\\Program Files\\FreeCAD 0.21\\bin\\FreeCAD.exe" })) ==
          "C:\\Program Files\\FreeCAD 0.21\\bin\\FreeCAD.exe");
    CHECK(freecad_exe_from_registry(old, exists_set({})).empty());

    // The .FCStd handler is used only when it is FreeCAD itself.
    CHECK(freecad_exe_from_association(exe) == exe);
    CHECK(freecad_exe_from_association("C:\\Program Files\\FreeCAD 1.0\\bin\\FreeCADCmd.exe").empty());
    CHECK(freecad_exe_from_association("C:\\Program Files\\7-Zip\\7zFM.exe").empty());
}

TEST_CASE("Windows FreeCAD discovery order", "[FreeCADLauncher]")
{
    const std::string fc10   = "C:\\Program Files\\FreeCAD 1.0\\bin\\FreeCAD.exe";
    const std::string fc021  = "C:\\Program Files\\FreeCAD 0.21\\bin\\FreeCAD.exe";
    const std::string custom = "D:\\Portable\\FreeCAD\\bin\\FreeCAD.exe";
    const std::map<std::string, std::vector<std::string>> program_files = { { "C:\\Program Files", { "FreeCAD 0.21", "FreeCAD 1.0" } } };

    SECTION("a custom path that exists wins")
    {
        auto env = windows_env({ custom, fc10 }, program_files);
        CHECK(discover("D:\\Portable\\FreeCAD", env) == std::vector<std::string>{ custom });
    }
    SECTION("a missing custom path falls through")
    {
        auto env = windows_env({ fc10 }, program_files);
        CHECK(discover("D:\\Gone\\FreeCAD.exe", env) == std::vector<std::string>{ fc10 });
    }
    SECTION("the .FCStd association comes first")
    {
        auto env              = windows_env({ fc021, fc10 }, program_files);
        env.fcstd_association = fc021;
        env.registry          = { freecad_102() };
        CHECK(discover("", env) == std::vector<std::string>{ fc021 });
    }
    SECTION("the newest registered install comes before scanning folders")
    {
        RegistryInstall old;
        old.display_name     = "FreeCAD 0.21.2";
        old.display_version  = "0.21.2";
        old.install_location = "C:\\Program Files\\FreeCAD 0.21";
        auto env             = windows_env({ fc021, fc10 });
        env.registry         = { old, freecad_102() };
        CHECK(discover("", env) == std::vector<std::string>{ fc10 });
        env.registry = { freecad_102(), old };
        CHECK(discover("", env) == std::vector<std::string>{ fc10 });
    }
    SECTION("0.21 and 1.0 side by side in Program Files: 1.0 is picked")
    {
        auto env = windows_env({ fc021, fc10 }, program_files);
        CHECK(discover("", env) == std::vector<std::string>{ fc10 });
    }
    SECTION("a per-user install in LOCALAPPDATA\\Programs")
    {
        const std::string user = "C:\\Users\\me\\AppData\\Local\\Programs\\FreeCAD 1.0\\bin\\FreeCAD.exe";
        auto env = windows_env({ user }, { { "C:\\Users\\me\\AppData\\Local\\Programs", { "FreeCAD 1.0" } } });
        CHECK(discover("", env) == std::vector<std::string>{ user });
    }
    SECTION("an unversioned FreeCAD folder (portable or weekly build)")
    {
        const std::string plain = "C:\\Program Files\\FreeCAD\\bin\\FreeCAD.exe";
        auto env = windows_env({ plain }, { { "C:\\Program Files", { "FreeCAD" } } });
        CHECK(discover("", env) == std::vector<std::string>{ plain });
    }
    SECTION("nothing installed")
    {
        CHECK(discover("", windows_env({})).empty());
    }
}

TEST_CASE("macOS and Linux FreeCAD discovery", "[FreeCADLauncher]")
{
    Environment mac;
    mac.platform  = Platform::MacOS;
    mac.home      = "/Users/lance";
    mac.list_dirs = dirs_map({ { "/Applications", { "FreeCAD 0.21.app", "FreeCAD 1.0.app", "Safari.app" } } });
    mac.exists    = exists_set({ "/Users/lance/Applications/FreeCAD.app/Contents/MacOS/FreeCAD" });
    CHECK(discover("", mac) == std::vector<std::string>{ "/Users/lance/Applications/FreeCAD.app/Contents/MacOS/FreeCAD" });
    mac.exists = exists_set({ "/Applications/FreeCAD 0.21.app/Contents/MacOS/FreeCAD", "/Applications/FreeCAD 1.0.app/Contents/MacOS/FreeCAD" });
    CHECK(discover("", mac) == std::vector<std::string>{ "/Applications/FreeCAD 1.0.app/Contents/MacOS/FreeCAD" });
    mac.exists          = exists_set({});
    mac.freecad_on_path = "/opt/homebrew/bin/freecad";
    CHECK(discover("", mac) == std::vector<std::string>{ "/opt/homebrew/bin/freecad" });

    Environment lin;
    lin.platform        = Platform::Linux;
    lin.home            = "/home/lance";
    lin.list_dirs       = dirs_map({});
    lin.freecad_on_path = "/usr/bin/freecad";
    lin.exists          = exists_set({ "/snap/bin/freecad" });
    CHECK(discover("", lin) == std::vector<std::string>{ "/usr/bin/freecad" });
    lin.freecad_on_path.clear();
    CHECK(discover("", lin) == std::vector<std::string>{ "/snap/bin/freecad" });
    lin.exists = exists_set({ "/home/lance/.local/share/flatpak/app/org.freecad.FreeCAD" });
    CHECK(discover("", lin) == std::vector<std::string>{ "flatpak", "run", "org.freecad.FreeCAD" });
    lin.exists = exists_set({});
    CHECK(discover("", lin).empty());
}

TEST_CASE("Edit session arguments and macro", "[FreeCADLauncher]")
{
    const std::string macro = "C:\\Users\\me\\AppData\\Roaming\\EdgeSlicer\\freecad_bridge\\1-2\\edgeslicer_edit.FCMacro";
    CHECK(edit_session_args({ "C:\\Program Files\\FreeCAD 1.0\\bin\\FreeCAD.exe" }, macro) ==
          std::vector<std::string>{ "C:\\Program Files\\FreeCAD 1.0\\bin\\FreeCAD.exe", "--single-instance", macro });
    CHECK(edit_session_args({ "flatpak", "run", "org.freecad.FreeCAD" }, "/tmp/s/m.FCMacro") ==
          std::vector<std::string>{ "flatpak", "run", "org.freecad.FreeCAD", "--single-instance", "/tmp/s/m.FCMacro" });

    CHECK(python_string_literal("C:\\Users\\me") == "\"C:\\\\Users\\\\me\"");
    CHECK(python_string_literal("say \"hi\"\n") == "\"say \\\"hi\\\"\\n\"");
    CHECK(python_string_literal(std::string("a\x01" "b")) == "\"a\\x01b\"");
    // UTF-8 passes through; the macro declares itself UTF-8.
    CHECK(python_string_literal("Halterung \xc3\xa4") == "\"Halterung \xc3\xa4\"");

    const std::string text = session_macro("C:\\EdgeSlicer\\resources\\freecad\\EdgeSlicerBridge", "C:\\s\\session.json");
    CHECK(text.rfind("# -*- coding: utf-8 -*-\n", 0) == 0);
    CHECK(text.find("_edgeslicer_bridge(\"C:\\\\EdgeSlicer\\\\resources\\\\freecad\\\\EdgeSlicerBridge\").start_edit_session(\"C:\\\\s\\\\session.json\")") != std::string::npos);
}

TEST_CASE("FreeCAD user Mod folders for 0.21, 1.0 and versioned 1.1+", "[FreeCADLauncher]")
{
    CHECK(is_versioned_user_dir("v1-1"));
    CHECK(is_versioned_user_dir("v1-10"));
    CHECK_FALSE(is_versioned_user_dir("Mod"));
    CHECK_FALSE(is_versioned_user_dir("v1"));
    CHECK_FALSE(is_versioned_user_dir("v-1"));
    CHECK_FALSE(is_versioned_user_dir("v1-x"));

    const std::string appdata = "C:\\Users\\me\\AppData\\Roaming";
    // 0.21 / 1.0: one unversioned user folder.
    CHECK(addon_mod_dirs(Platform::Windows, appdata, "", exists_set({}), dirs_map({ { appdata + "\\FreeCAD", { "Mod", "Macro" } } })) ==
          std::vector<std::string>{ appdata + "\\FreeCAD\\Mod" });
    // 1.1+ alongside: every versioned folder gets a copy too.
    CHECK(addon_mod_dirs(Platform::Windows, appdata + "\\", "", exists_set({}), dirs_map({ { appdata + "\\FreeCAD", { "v1-2", "Mod", "v1-1" } } })) ==
          std::vector<std::string>{ appdata + "\\FreeCAD\\Mod", appdata + "\\FreeCAD\\v1-1\\Mod", appdata + "\\FreeCAD\\v1-2\\Mod" });
    CHECK(addon_mod_dirs(Platform::MacOS, "/Users/me", "", exists_set({}), dirs_map({})) ==
          std::vector<std::string>{ "/Users/me/Library/Application Support/FreeCAD/Mod" });
    CHECK(addon_mod_dirs(Platform::Linux, "/home/me", "", exists_set({ "/home/me/.FreeCAD", "/home/me/.var/app/org.freecad.FreeCAD" }), dirs_map({})) ==
          std::vector<std::string>{ "/home/me/.local/share/FreeCAD/Mod", "/home/me/.FreeCAD/Mod", "/home/me/.var/app/org.freecad.FreeCAD/data/FreeCAD/Mod" });
    CHECK(addon_mod_dirs(Platform::Linux, "/home/me", "/data/xdg", exists_set({}), dirs_map({})) ==
          std::vector<std::string>{ "/data/xdg/FreeCAD/Mod" });
    CHECK(addon_mod_dirs(Platform::Windows, "", "", exists_set({}), dirs_map({})).empty());
}

TEST_CASE("Installing the FreeCAD add-on into a temporary user folder", "[FreeCADLauncher]")
{
    TempDir tmp;
    const boost::filesystem::path source = tmp.path / "resources" / "freecad" / ADDON_FOLDER;
    write_file(source / "InitGui.py", "# init\n");
    write_file(source / "edgeslicer_bridge.py", "VERSION = (1, 0, 0)\n");
    write_file(source / "Resources" / "icons" / "EdgeSlicerSend.svg", "<svg/>");
    write_file(source / "__pycache__" / "edgeslicer_bridge.cpython-311.pyc", "stale");

    // The real folder layout: %APPDATA%\FreeCAD with a 1.0 Mod folder and a 1.1 versioned folder.
    const boost::filesystem::path appdata = tmp.path / "Roaming";
    boost::filesystem::create_directories(appdata / "FreeCAD" / "Mod" / "SomeoneElsesAddon");
    boost::filesystem::create_directories(appdata / "FreeCAD" / "v1-1");
    // An older copy of ours with a file this version no longer ships.
    write_file(appdata / "FreeCAD" / "Mod" / ADDON_FOLDER / "old_module.py", "old");

    const std::vector<std::string> mod_dirs =
        addon_mod_dirs(ExternalEditor::this_platform() == Platform::Windows ? Platform::Windows : Platform::MacOS, appdata.string(), "",
                       &ExternalEditor::real_exists, &ExternalEditor::real_list_dirs);
    InstallReport report;
    const std::string exe = "C:\\Program Files\\EdgeSlicer\\EdgeSlicer.exe";
    if (ExternalEditor::this_platform() == Platform::Windows) {
        REQUIRE(mod_dirs.size() == 2);
        REQUIRE(install_addon(source.string(), mod_dirs, exe, report));
        CHECK(report.errors.empty());
        CHECK(report.installed.size() == 2);
        for (const boost::filesystem::path root : { appdata / "FreeCAD" / "Mod", appdata / "FreeCAD" / "v1-1" / "Mod" }) {
            const boost::filesystem::path addon = root / ADDON_FOLDER;
            CHECK(boost::filesystem::exists(addon / "InitGui.py"));
            CHECK(read_file(addon / "edgeslicer_bridge.py") == "VERSION = (1, 0, 0)\n");
            CHECK(boost::filesystem::exists(addon / "Resources" / "icons" / "EdgeSlicerSend.svg"));
            CHECK_FALSE(boost::filesystem::exists(addon / "__pycache__"));
            CHECK(read_file(addon / LOCATION_FILE) == exe + "\n");
        }
        CHECK_FALSE(boost::filesystem::exists(appdata / "FreeCAD" / "Mod" / ADDON_FOLDER / "old_module.py"));
        // Other add-ons are left alone.
        CHECK(boost::filesystem::exists(appdata / "FreeCAD" / "Mod" / "SomeoneElsesAddon"));
    }

    // A missing source reports an error and installs nothing.
    InstallReport missing;
    CHECK_FALSE(install_addon((tmp.path / "nowhere").string(), { (tmp.path / "Mod").string() }, exe, missing));
    CHECK(missing.installed.empty());
    CHECK_FALSE(missing.errors.empty());
    CHECK_FALSE(boost::filesystem::exists(tmp.path / "Mod" / ADDON_FOLDER));
}
