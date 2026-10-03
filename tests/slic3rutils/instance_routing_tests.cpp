#include <catch2/catch.hpp>

#include <optional>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "slic3r/Utils/InstanceRouting.hpp"

using namespace Slic3r::InstanceRouting;

TEST_CASE("Hidden start follows SNORCA_HIDDEN, then --hidden, then the preference", "[InstanceRouting]")
{
    // Nothing set: the preference decides.
    CHECK_FALSE(resolve_hidden_start(std::nullopt, false, false));
    CHECK(resolve_hidden_start(std::nullopt, false, true));
    // --hidden wins over a preference that says visible.
    CHECK(resolve_hidden_start(std::nullopt, true, false));
    // The environment beats both, in either direction; "0" means visible.
    CHECK_FALSE(resolve_hidden_start(std::string("0"), true, true));
    CHECK(resolve_hidden_start(std::string("1"), false, false));
    // An empty value is the same as unset.
    CHECK(resolve_hidden_start(std::string(""), false, true));
    CHECK_FALSE(resolve_hidden_start(std::string(""), false, false));
}

TEST_CASE("Executable path keys ignore case, separators and the extended-length prefix", "[InstanceRouting]")
{
    const std::string plain = "c:\\dev\\edgeslicertest\\blender-themes-212-213\\edgeslicer.exe";
    CHECK(normalize_exe_path_key("C:\\Dev\\EdgeSlicerTest\\Blender-Themes-212-213\\EdgeSlicer.exe") == plain);
    CHECK(normalize_exe_path_key("C:/Dev/EdgeSlicerTest/blender-themes-212-213/EdgeSlicer.exe") == plain);
    CHECK(normalize_exe_path_key("\\\\?\\C:\\Dev\\EdgeSlicerTest\\blender-themes-212-213\\EdgeSlicer.exe") == plain);
    CHECK(normalize_exe_path_key("\\\\?\\UNC\\server\\share\\EdgeSlicer.exe") == "\\\\server\\share\\edgeslicer.exe");
    CHECK(normalize_exe_path_key("C:\\Program Files\\EdgeSlicer\\") == "c:\\program files\\edgeslicer");
    // Different installs stay different.
    CHECK(normalize_exe_path_key("C:\\Program Files\\EdgeSlicer\\EdgeSlicer.exe") != plain);
    CHECK(normalize_exe_path_key("") == "");
}

TEST_CASE("Only a visible window of the same executable is a hand-off target", "[InstanceRouting]")
{
    CHECK(is_hand_off_target(42, 42, true));
    // The hidden hub-managed slicer of the same executable is skipped.
    CHECK_FALSE(is_hand_off_target(42, 42, false));
    // A window of another install is never a target.
    CHECK_FALSE(is_hand_off_target(42, 43, true));
    CHECK_FALSE(is_hand_off_target(42, 0, true));
}

TEST_CASE("A launch exits only when a visible instance took its arguments", "[InstanceRouting]")
{
    CHECK(should_hand_off(true, true, true));
    // Lock held by a hidden or hung instance and nobody visible to receive: start normally.
    CHECK_FALSE(should_hand_off(true, true, false));
    // No other instance, or hand-off not wanted: start normally.
    CHECK_FALSE(should_hand_off(true, false, false));
    CHECK_FALSE(should_hand_off(false, true, true));
}

TEST_CASE("Hidden instances do not claim the single-instance lock", "[InstanceRouting]")
{
    CHECK(claims_instance_lock(false));
    CHECK_FALSE(claims_instance_lock(true));
}

TEST_CASE("data-dir InstanceLock is independent of single-instance hand-off", "[InstanceRouting][InstanceLock]")
{
    // Hand-off still uses the routing lock and a visible target. A second
    // visible instance that is allowed to start (single_instance=false) is
    // what the data-dir lock turns read-only; it must not change this path.
    CHECK(should_hand_off(true, true, true));
    CHECK_FALSE(should_hand_off(true, false, true));
    CHECK_FALSE(should_hand_off(false, true, true));
    CHECK_FALSE(claims_instance_lock(true));
    CHECK(claims_instance_lock(false));
}

TEST_CASE("FreeCAD's Send to EdgeSlicer hands its STEP files over", "[InstanceRouting]")
{
    // What edgeslicer_bridge.py starts: EdgeSlicer.exe --single-instance <one file per body>.
    const std::string exe   = R"(C:\Program Files\EdgeSlicer\EdgeSlicer.exe)";
    const std::string body  = R"(C:\Users\me\AppData\Local\Temp\EdgeSlicer-from-FreeCAD\20260930-101500-12\Body.step)";
    const std::string other = "C:\\Users\\me\\AppData\\Local\\Temp\\EdgeSlicer-from-FreeCAD\\20260930-101500-12\\Halterung \xc3\xa4 (2).step";
    const CommandLine cl    = split_command_line({ exe, "--single-instance", body, other });
    REQUIRE(cl.single_instance.has_value());
    CHECK(*cl.single_instance);
    // The switch itself is not passed on; the executable and the files are, in order.
    CHECK(cl.forwarded == std::vector<std::string>{ exe, body, other });

    // The message survives the trip to the running instance (spaces, parentheses, UTF-8).
    std::vector<std::string> received;
    REQUIRE(Slic3r::unescape_strings_cstyle(Slic3r::escape_strings_cstyle(cl.forwarded), received));
    CHECK(received == cl.forwarded);

    // The receiver loads every argument after the executable that is an existing file.
    const auto exists = [&](const std::string &p) { return p == body || p == other || p == exe; };
    std::vector<std::string> files;
    for (size_t i = 1; i < received.size(); ++i)
        if (std::string f = handed_off_file(received[i], exists); !f.empty())
            files.push_back(f);
    CHECK(files == std::vector<std::string>{ body, other });

    // Started from FreeCAD with SNORCA_HIDDEN=0: a new instance shows its window even with "Start hidden" on.
    CHECK_FALSE(resolve_hidden_start(std::string("0"), false, true));
}

TEST_CASE("Command line split keeps the hand-off switch rules", "[InstanceRouting]")
{
    CHECK_FALSE(split_command_line({ "EdgeSlicer.exe", "a.stl" }).single_instance.has_value());
    CHECK(split_command_line({ "EdgeSlicer.exe", "--no-single-instance", "a.stl" }).single_instance == std::optional<bool>(false));
    // The last switch wins, as in DynamicConfig::read_cli().
    CHECK(split_command_line({ "EdgeSlicer.exe", "--single-instance", "--no-single-instance" }).single_instance == std::optional<bool>(false));
    // argv[0] is never taken for a switch.
    CHECK(split_command_line({ "--single-instance" }).forwarded == std::vector<std::string>{ "--single-instance" });
}

TEST_CASE("Handed-off arguments name files only when they exist", "[InstanceRouting]")
{
    const auto exists = [](const std::string &p) { return p == "C:\parts\a b.step"; };
    CHECK(handed_off_file("C:\parts\a b.step", exists) == "C:\parts\a b.step");
    CHECK(handed_off_file("\"C:\parts\a b.step\"", exists) == "C:\parts\a b.step");
    CHECK(handed_off_file("C:\parts\missing.step", exists).empty());
    CHECK(handed_off_file("edgeslicer://open?file=x", exists).empty());
    CHECK(handed_off_file("ab", exists).empty());
}
