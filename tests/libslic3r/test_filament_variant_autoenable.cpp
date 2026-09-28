#include <catch2/catch.hpp>

#include "libslic3r/PresetBundle.hpp"

#include <set>
#include <string>
#include <vector>

using namespace Slic3r;

// Regression tests for the filament-visibility gap: a vendor update that adds new printer-variant
// presets to an already-ticked filament (e.g. "Panchroma CoPE @BBL H2D" presets added after the
// user ticked "Panchroma CoPE" for H2C/H2S) must not leave the new variants hidden. See
// PresetBundle::load_installed_filaments, which builds the FilamentVariantCandidate list and
// calls filaments_to_auto_enable().

namespace {

FilamentVariantCandidate candidate(std::string name, std::string group_key, bool is_system, bool compatible)
{
    FilamentVariantCandidate c;
    c.name                              = std::move(name);
    c.group_key                         = std::move(group_key);
    c.is_system                         = is_system;
    c.compatible_with_installed_printer = compatible;
    return c;
}

} // namespace

TEST_CASE("filaments_to_auto_enable enables a new variant of an already-enabled group", "[PresetBundle][FilamentVariants]")
{
    // Same alias/group ("BBL\x1fPanchroma CoPE"): H2C variant already enabled by the user,
    // H2D variant newly added by a vendor bundle update and not yet recorded anywhere.
    std::vector<FilamentVariantCandidate> candidates = {
        candidate("Panchroma CoPE @BBL H2C", "BBL\x1fPanchroma CoPE", true, true),
        candidate("Panchroma CoPE @BBL H2D", "BBL\x1fPanchroma CoPE", true, true),
    };
    std::set<std::string> already_enabled = {"Panchroma CoPE @BBL H2C"};

    std::vector<std::string> enabled = filaments_to_auto_enable(candidates, already_enabled);
    REQUIRE(enabled == std::vector<std::string>{"Panchroma CoPE @BBL H2D"});
}

TEST_CASE("filaments_to_auto_enable leaves a fully unticked group alone", "[PresetBundle][FilamentVariants]")
{
    // The user deliberately unticked every variant of this filament: nothing should come back.
    std::vector<FilamentVariantCandidate> candidates = {
        candidate("Panchroma CoPE @BBL H2C", "BBL\x1fPanchroma CoPE", true, true),
        candidate("Panchroma CoPE @BBL H2D", "BBL\x1fPanchroma CoPE", true, true),
    };
    std::set<std::string> already_enabled; // nothing enabled at all

    std::vector<std::string> enabled = filaments_to_auto_enable(candidates, already_enabled);
    REQUIRE(enabled.empty());
}

TEST_CASE("filaments_to_auto_enable does not enable a preset incompatible with any installed printer", "[PresetBundle][FilamentVariants]")
{
    std::vector<FilamentVariantCandidate> candidates = {
        candidate("Panchroma CoPE @BBL H2C", "BBL\x1fPanchroma CoPE", true, true),
        // New variant exists in the vendor bundle but its only compatible printer was never installed.
        candidate("Panchroma CoPE @BBL X1", "BBL\x1fPanchroma CoPE", true, false),
    };
    std::set<std::string> already_enabled = {"Panchroma CoPE @BBL H2C"};

    std::vector<std::string> enabled = filaments_to_auto_enable(candidates, already_enabled);
    REQUIRE(enabled.empty());
}

TEST_CASE("filaments_to_auto_enable never touches user presets", "[PresetBundle][FilamentVariants]")
{
    std::vector<FilamentVariantCandidate> candidates = {
        candidate("Panchroma CoPE @BBL H2C", "BBL\x1fPanchroma CoPE", true, true),
        // Same group_key but is_system=false: must never be auto-enabled, and must never count
        // towards seeding the group either (it can't, since the loop that builds candidates in
        // PresetBundle::load_installed_filaments skips user presets entirely, but the pure
        // function itself should also be safe if ever called with one included).
        candidate("Panchroma CoPE (my copy) @BBL H2D", "BBL\x1fPanchroma CoPE", false, true),
    };
    std::set<std::string> already_enabled = {"Panchroma CoPE @BBL H2C"};

    std::vector<std::string> enabled = filaments_to_auto_enable(candidates, already_enabled);
    REQUIRE(enabled.empty());
}

TEST_CASE("filaments_to_auto_enable keeps different vendors with the same alias separate", "[PresetBundle][FilamentVariants]")
{
    // "Generic PLA" exists for both BBL and Snapmaker; only the BBL group was ticked.
    std::vector<FilamentVariantCandidate> candidates = {
        candidate("Generic PLA @BBL X1", "BBL\x1fGeneric PLA", true, true),
        candidate("Generic PLA @BBL H2D", "BBL\x1fGeneric PLA", true, true),
        candidate("Generic PLA @Snapmaker U1", "Snapmaker\x1fGeneric PLA", true, true),
    };
    std::set<std::string> already_enabled = {"Generic PLA @BBL X1"};

    std::vector<std::string> enabled = filaments_to_auto_enable(candidates, already_enabled);
    REQUIRE(enabled == std::vector<std::string>{"Generic PLA @BBL H2D"});
}

TEST_CASE("filaments_to_auto_enable is a no-op once every variant is already recorded", "[PresetBundle][FilamentVariants]")
{
    std::vector<FilamentVariantCandidate> candidates = {
        candidate("Panchroma CoPE @BBL H2C", "BBL\x1fPanchroma CoPE", true, true),
        candidate("Panchroma CoPE @BBL H2D", "BBL\x1fPanchroma CoPE", true, true),
    };
    std::set<std::string> already_enabled = {"Panchroma CoPE @BBL H2C", "Panchroma CoPE @BBL H2D"};

    std::vector<std::string> enabled = filaments_to_auto_enable(candidates, already_enabled);
    REQUIRE(enabled.empty());
}
