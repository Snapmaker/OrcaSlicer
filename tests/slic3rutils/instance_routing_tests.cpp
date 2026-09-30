#include <catch2/catch.hpp>

#include <optional>
#include <string>

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
    const std::string plain = "c:\dev\edgeslicertest\blender-themes-212-213\edgeslicer.exe";
    CHECK(normalize_exe_path_key("C:\Dev\EdgeSlicerTest\Blender-Themes-212-213\EdgeSlicer.exe") == plain);
    CHECK(normalize_exe_path_key("C:/Dev/EdgeSlicerTest/blender-themes-212-213/EdgeSlicer.exe") == plain);
    CHECK(normalize_exe_path_key("\\?\C:\Dev\EdgeSlicerTest\blender-themes-212-213\EdgeSlicer.exe") == plain);
    CHECK(normalize_exe_path_key("\\?\UNC\server\share\EdgeSlicer.exe") == "\\server\share\edgeslicer.exe");
    CHECK(normalize_exe_path_key("C:\Program Files\EdgeSlicer\\") == "c:\program files\edgeslicer");
    // Different installs stay different.
    CHECK(normalize_exe_path_key("C:\Program Files\EdgeSlicer\EdgeSlicer.exe") != plain);
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
