#include <catch2/catch_all.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/NozzleFilamentPresets.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ProjectSchemaVersion.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Utils.hpp"

#include "test_utils.hpp"

using namespace Slic3r;

// Filament presets follow the nozzle size of their tool head (libslic3r/NozzleFilamentPresets.hpp).
// The cases load the shipped Snapmaker vendor; the census below guards the U1 layout the rule needs
// (one preset per nozzle size, all children of one "@U1 base" preset, sharing the alias).

namespace {

const char *const U1_02 = "Snapmaker U1 (0.2 nozzle)";
const char *const U1_04 = "Snapmaker U1 (0.4 nozzle)";
const char *const U1_06 = "Snapmaker U1 (0.6 nozzle)";
const char *const U1_08 = "Snapmaker U1 (0.8 nozzle)";
const char *const U1_PROCESS_04 = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)";

const char *const PLA_02 = "Generic PLA @U1 0.2 nozzle";
const char *const PLA_04 = "Generic PLA";
const char *const PLA_06 = "Generic PLA @U1 0.6 nozzle";
const char *const PLA_08 = "Generic PLA @U1 0.8 nozzle";
const char *const TPU_04 = "Generic TPU";

// Every case loads its own bundle, so no case depends on what another one selected.
std::unique_ptr<PresetBundle> load_snapmaker_bundle()
{
    auto bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(PROFILES_DIR, "Snapmaker", PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr,
                                          /*allow_cache=*/false);
    return bundle;
}

const Preset &system_filament(const PresetBundle &bundle, const std::string &name)
{
    const Preset *preset = const_cast<PresetBundle &>(bundle).filaments.find_preset(name, false, true);
    REQUIRE(preset != nullptr);
    return *preset;
}

const Preset &machine(const PresetBundle &bundle, const std::string &name)
{
    const Preset *preset = const_cast<PresetBundle &>(bundle).printers.find_preset(name, false, true);
    REQUIRE(preset != nullptr);
    return *preset;
}

// The U1 printer preset of 0.4 with the rule on and the given sizes on its tool heads, filament i
// on tool head i.
void select_u1(PresetBundle &bundle, const std::vector<double> &head_sizes, const std::vector<std::string> &slots, const char *printer = U1_04)
{
    REQUIRE(bundle.printers.select_preset_by_name(printer, true));
    if (std::string(printer) == U1_04)
        REQUIRE(bundle.prints.select_preset_by_name(U1_PROCESS_04, true));
    bundle.nozzle_filament_enabled = true;
    bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = head_sizes;
    bundle.filament_presets = slots;
    REQUIRE(bundle.filaments.select_preset_by_name(slots.front(), true));
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = std::vector<int>(slots.size(), 1);
}

// A user preset made from `parent`, as "Save as" leaves it in the collection.
void add_user_filament(PresetBundle &bundle, const std::string &name, const std::string &parent)
{
    DynamicPrintConfig config = system_filament(bundle, parent).config;
    config.option<ConfigOptionString>("inherits", true)->value = parent;
    Preset &preset = bundle.filaments.load_preset(std::string(), name, std::move(config), /*select=*/false);
    preset.is_visible = true;
}

// The loader fills the rename map; a case that renames afterwards refreshes it through the
// protected member.
struct RenameMapAccess : PresetCollection
{
    using PresetCollection::update_map_system_profile_renamed;
};
void refresh_rename_map(PresetCollection &presets) { (presets.*(&RenameMapAccess::update_map_system_profile_renamed))(); }

NozzleFilament::SlotTarget target(const PresetBundle &bundle, size_t slot, const std::vector<size_t> &heads = {})
{
    return NozzleFilament::target_for_slot(bundle, NozzleFilament::state(bundle), slot, heads);
}

} // namespace

TEST_CASE("The U1 filaments come as one preset per nozzle size and family", "[NozzleFilament][Profiles][nozzle_filament_census]")
{
    auto bundle = load_snapmaker_bundle();

    const std::vector<std::string> machines = { U1_02, U1_04, U1_06, U1_08 };
    // family (parent, alias) -> machine preset -> members
    std::map<std::pair<std::string, std::string>, std::map<std::string, std::vector<std::string>>> families;
    size_t presets = 0;
    for (const Preset &preset : bundle->filaments) {
        if (!preset.is_system)
            continue;
        const auto *list = preset.config.option<ConfigOptionStrings>("compatible_printers");
        if (list == nullptr || list->values.empty())
            continue;
        if (std::find(machines.begin(), machines.end(), list->values.front()) == machines.end())
            continue;
        ++presets;
        INFO(preset.name);
        // Exactly one machine preset, no condition, a parent and an alias: a family member.
        CHECK(list->values.size() == 1);
        CHECK(preset.compatible_printers_condition().empty());
        CHECK_FALSE(preset.system_inherits.empty());
        CHECK_FALSE(preset.alias.empty());
        families[{ preset.system_inherits, preset.alias }][list->values.front()].push_back(preset.name);
    }
    CHECK(presets == 129);
    CHECK(families.size() == 43);

    std::map<std::string, size_t> coverage;
    for (const auto &[family, members] : families) {
        std::string pattern;
        for (const std::string &machine_name : machines) {
            const auto it = members.find(machine_name);
            INFO(family.second << " on " << machine_name);
            // At most one version per (family, machine preset).
            if (it != members.end())
                CHECK(it->second.size() == 1);
            pattern += it == members.end() ? '.' : 'X';
        }
        ++coverage[pattern];
    }
    CHECK(coverage["XXXX"] == 18);
    CHECK(coverage[".XXX"] == 15);
    CHECK(coverage[".X.."] == 9);
    CHECK(coverage["XXX."] == 1);
    CHECK(coverage.size() == 4);
}

TEST_CASE("version_for finds the preset of a family for a machine preset", "[NozzleFilament][nozzle_filament_version]")
{
    auto                    bundle    = load_snapmaker_bundle();
    const PresetCollection &filaments = bundle->filaments;

    auto version_name = [&](const std::string &preset, const std::string &machine_name) {
        const Preset *version = NozzleFilament::version_for(filaments, system_filament(*bundle, preset), machine(*bundle, machine_name));
        return version == nullptr ? std::string() : version->name;
    };

    CHECK(version_name(PLA_04, U1_02) == PLA_02);
    CHECK(version_name(PLA_02, U1_04) == PLA_04);
    CHECK(version_name(PLA_02, U1_08) == PLA_08);
    CHECK(version_name(PLA_06, U1_06) == PLA_06);

    CHECK(version_name("Snapmaker PLA SnapSpeed @U1", U1_06) == "Snapmaker PLA SnapSpeed @U1 0.6 nozzle");
    CHECK(version_name("Snapmaker PLA SnapSpeed @U1 0.6 nozzle", U1_04) == "Snapmaker PLA SnapSpeed @U1");
    // The A250 preset shares the alias, not the parent: no version of the U1 family, and the U1
    // presets are no versions of it.
    CHECK(system_filament(*bundle, "Snapmaker PLA SnapSpeed").alias == system_filament(*bundle, "Snapmaker PLA SnapSpeed @U1").alias);
    CHECK(version_name("Snapmaker PLA SnapSpeed", U1_04).empty());
    CHECK(version_name("Snapmaker PLA SnapSpeed @U1", "Snapmaker A250 (0.4 nozzle)").empty());

    // No 0.2 version of the TPU.
    CHECK(version_name(TPU_04, U1_02).empty());
    CHECK(version_name(TPU_04, U1_06) == "Generic TPU @U1 0.6 nozzle");

    // The same parent alone makes no family.
    CHECK(system_filament(*bundle, "Snapmaker TPE @J1").system_inherits == system_filament(*bundle, "Snapmaker TPU @J1").system_inherits);
    CHECK(version_name("Snapmaker TPE @J1", "Snapmaker J1 (0.4 nozzle)") != "Snapmaker TPU @J1");
    CHECK(version_name("Snapmaker TPU @J1", "Snapmaker J1 (0.4 nozzle)") != "Snapmaker TPE @J1");

    SECTION("the nozzle size a preset is pinned to") {
        CHECK_THAT(NozzleFilament::preset_nozzle_size(*bundle, system_filament(*bundle, PLA_02)), Catch::Matchers::WithinAbs(0.2, 1e-9));
        CHECK_THAT(NozzleFilament::preset_nozzle_size(*bundle, system_filament(*bundle, PLA_04)), Catch::Matchers::WithinAbs(0.4, 1e-9));
        CHECK_THAT(NozzleFilament::home_nozzle_size(machine(*bundle, U1_08).config), Catch::Matchers::WithinAbs(0.8, 1e-9));
        CHECK(NozzleFilament::head_machine_preset(bundle->printers, "Snapmaker U1", "0.6") == &machine(*bundle, U1_06));
        CHECK(NozzleFilament::head_machine_preset(bundle->printers, "Snapmaker U1", 0.6) == &machine(*bundle, U1_06));
        CHECK(NozzleFilament::head_machine_preset(bundle->printers, "Snapmaker U1", 0.5) == nullptr);
    }
}

TEST_CASE("The gate of the nozzle size rule", "[NozzleFilament][nozzle_filament_state]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, { 0.4, 0.4, 0.4, 0.4 }, { PLA_04, PLA_04, PLA_04, PLA_04 });

    SECTION("every head on the size of the printer preset: the rule is on, nothing is mixed") {
        const NozzleFilament::State state = NozzleFilament::state(*bundle);
        CHECK(state.rule_on);
        CHECK_FALSE(state.mixed);
        REQUIRE(state.head_machine.size() == 4);
        for (const Preset *head : state.head_machine)
            CHECK(head == &bundle->printers.get_edited_preset());
        CHECK(state.slot_head == std::vector<size_t>{ 0, 1, 2, 3 });
    }
    SECTION("one head off the size") {
        bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.2, 0.4, 0.4, 0.4 };
        const NozzleFilament::State state = NozzleFilament::state(*bundle);
        CHECK(state.rule_on);
        CHECK(state.mixed);
        CHECK(state.head_machine[0] == &machine(*bundle, U1_02));
        CHECK(state.head_machine[1] == &bundle->printers.get_edited_preset());
    }
    SECTION("a size without a profile mixes nothing") {
        bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.5, 0.4, 0.4, 0.4 };
        const NozzleFilament::State state = NozzleFilament::state(*bundle);
        CHECK(state.rule_on);
        CHECK_FALSE(state.mixed);
        CHECK(state.head_machine[0] == &bundle->printers.get_edited_preset());
    }
    SECTION("the flag is off") {
        bundle->nozzle_filament_enabled = false;
        bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.2, 0.4, 0.4, 0.4 };
        const NozzleFilament::State state = NozzleFilament::state(*bundle);
        CHECK_FALSE(state.rule_on);
        CHECK_FALSE(state.mixed);
        CHECK(target(*bundle, 0).reason == NozzleFilament::Reason::Skipped);
    }
    SECTION("single extruder multi material") {
        bundle->printers.get_edited_preset().config.option<ConfigOptionBool>("single_extruder_multi_material", true)->value = true;
        CHECK_FALSE(NozzleFilament::state(*bundle).rule_on);
    }
    SECTION("one nozzle") {
        bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.4 };
        CHECK_FALSE(NozzleFilament::state(*bundle).rule_on);
    }
    SECTION("a manual project map is followed, slots beyond the heads and mixed slots have no head") {
        bundle->filament_presets.assign(6, PLA_04);
        bundle->project_config.option<ConfigOptionInts>("filament_map", true)->values = { 2, 1, 4, 3, 1, 2 };
        CHECK(NozzleFilament::state(*bundle).slot_head == std::vector<size_t>{ 0, 1, 2, 3, NozzleFilament::no_head, NozzleFilament::no_head });
        bundle->project_config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
        CHECK(NozzleFilament::state(*bundle).slot_head == std::vector<size_t>{ 1, 0, 3, 2, NozzleFilament::no_head, NozzleFilament::no_head });
        bundle->project_config.option<ConfigOptionBools>("filament_is_mixed", true)->values = { false, false, true, false, false, false };
        CHECK(NozzleFilament::state(*bundle).slot_head == std::vector<size_t>{ 1, 0, NozzleFilament::no_head, 2, NozzleFilament::no_head, NozzleFilament::no_head });
    }
}

TEST_CASE("A pass over a tool head that prints no filament slot has no targets while the rule is on", "[NozzleFilament][nozzle_filament_no_slot]")
{
    // Three filaments, a size change on tool head 4: the trigger passes a tool head no slot maps
    // to. The empty list is not the gate (Plater's log line tells the two apart by State::rule_on).
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, { 0.4, 0.4, 0.4, 0.8 }, { PLA_04, PLA_04, PLA_04 });
    REQUIRE(NozzleFilament::state(*bundle).rule_on);
    CHECK(NozzleFilament::state(*bundle).slot_head == std::vector<size_t>{ 0, 1, 2 });
    CHECK(bundle->nozzle_filament_targets({ 3 }).empty());
    CHECK(bundle->nozzle_filament_targets({ 2, 3 }).size() == 1);
    CHECK(bundle->nozzle_filament_targets().size() == 3);
    // Off the gate the list is empty for every tool head.
    bundle->nozzle_filament_enabled = false;
    REQUIRE_FALSE(NozzleFilament::state(*bundle).rule_on);
    CHECK(bundle->nozzle_filament_targets({ 2 }).empty());
    CHECK(bundle->nozzle_filament_targets().empty());
}

TEST_CASE("target_for_slot names the preset a slot should hold", "[NozzleFilament][nozzle_filament_target]")
{
    using NozzleFilament::Reason;
    auto bundle = load_snapmaker_bundle();

    SECTION("system presets: switched, unchanged, no version, the version of the printer preset") {
        select_u1(*bundle, { 0.2, 0.4, 0.6, 0.2 }, { PLA_04, PLA_04, PLA_02, TPU_04 });
        const NozzleFilament::SlotTarget first = target(*bundle, 0);
        CHECK(first.reason == Reason::Switched);
        CHECK(first.from == PLA_04);
        CHECK(first.to == PLA_02);
        CHECK(first.head == 0);
        CHECK_THAT(first.head_size, Catch::Matchers::WithinAbs(0.2, 1e-9));
        CHECK_THAT(first.preset_size, Catch::Matchers::WithinAbs(0.4, 1e-9));
        CHECK(first.switches());

        const NozzleFilament::SlotTarget second = target(*bundle, 1);
        CHECK(second.reason == Reason::Unchanged);
        CHECK(second.to == PLA_04);
        CHECK_FALSE(second.size_agnostic);
        CHECK_FALSE(second.switches());

        // A third size in the slot: straight to the version of the tool head.
        CHECK(target(*bundle, 2).reason == Reason::Switched);
        CHECK(target(*bundle, 2).to == PLA_06);

        // No 0.2 TPU: the slot keeps the version of the printer preset.
        const NozzleFilament::SlotTarget fourth = target(*bundle, 3);
        CHECK(fourth.reason == Reason::NoVersion);
        CHECK(fourth.to == TPU_04);
        CHECK_FALSE(fourth.switches());

        // ... and a slot that held the 0.8 TPU gets that version of the printer preset.
        bundle->filament_presets[3] = "Generic TPU @U1 0.8 nozzle";
        CHECK(target(*bundle, 3).reason == Reason::HomeVersionUsed);
        CHECK(target(*bundle, 3).to == TPU_04);
        CHECK(target(*bundle, 3).switches());

        // A tool head of the home size takes the wrong size back.
        bundle->filament_presets[1] = PLA_08;
        CHECK(target(*bundle, 1).reason == Reason::Switched);
        CHECK(target(*bundle, 1).to == PLA_04);
    }

    SECTION("slots without a tool head, empty and unknown names are skipped") {
        select_u1(*bundle, { 0.2, 0.2, 0.2, 0.2 }, { PLA_04, PLA_04, PLA_04, PLA_04, PLA_04, PLA_04 });
        CHECK(target(*bundle, 4).reason == Reason::Skipped);
        CHECK(target(*bundle, 5).reason == Reason::Skipped);
        CHECK(target(*bundle, 9).reason == Reason::Skipped);
        bundle->project_config.option<ConfigOptionBools>("filament_is_mixed", true)->values = { false, true, false, false, false, false };
        CHECK(target(*bundle, 1).reason == Reason::Skipped);
        CHECK(target(*bundle, 0).reason == Reason::Switched);
        bundle->filament_presets[2].clear();
        CHECK(target(*bundle, 2).reason == Reason::Skipped);
        bundle->filament_presets[3] = "No such filament";
        CHECK(target(*bundle, 3).reason == Reason::Skipped);
        bundle->filament_presets[0] = PresetBundle::ORCA_DEFAULT_FILAMENT_PLACEHOLDER;
        CHECK(target(*bundle, 0).reason == Reason::Skipped);
    }

    SECTION("a renamed preset is resolved first") {
        for (Preset &preset : bundle->filaments)
            if (preset.name == PLA_04)
                preset.renamed_from = { "Generic PLA of old" };
        refresh_rename_map(bundle->filaments);
        select_u1(*bundle, { 0.2, 0.4, 0.4, 0.4 }, { PLA_04, PLA_04, PLA_04, PLA_04 });
        bundle->filament_presets[0] = "Generic PLA of old";
        bundle->filament_presets[1] = "Generic PLA of old";
        const NozzleFilament::SlotTarget first = target(*bundle, 0);
        CHECK(first.reason == Reason::Switched);
        CHECK(first.from == "Generic PLA of old");
        CHECK(first.to == PLA_02);
        const NozzleFilament::SlotTarget second = target(*bundle, 1);
        CHECK(second.reason == Reason::Unchanged);
        CHECK(second.to == PLA_04);
        CHECK_FALSE(second.switches());
    }

    SECTION("system presets do not depend on the path, and the way back restores them") {
        select_u1(*bundle, { 0.4, 0.4, 0.4, 0.4 }, { PLA_04, PLA_04, PLA_04, PLA_04 });
        auto set_head = [&](double size) {
            bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->values[0] = size;
            const NozzleFilament::SlotTarget t = target(*bundle, 0);
            if (t.switches())
                bundle->filament_presets[0] = t.to;
        };
        set_head(0.6);
        CHECK(bundle->filament_presets[0] == PLA_06);
        set_head(0.2);
        CHECK(bundle->filament_presets[0] == PLA_02);
        const std::string by_detour = bundle->filament_presets[0];
        set_head(0.4);
        CHECK(bundle->filament_presets[0] == PLA_04);
        set_head(0.2);
        CHECK(bundle->filament_presets[0] == by_detour);
        set_head(0.4);
        CHECK(bundle->filament_presets[0] == PLA_04);
        CHECK(bundle->filament_presets[1] == PLA_04);
    }

    SECTION("user presets stay unless exactly one counterpart exists") {
        add_user_filament(*bundle, "My PLA", PLA_04);
        select_u1(*bundle, { 0.2, 0.4, 0.4, 0.4 }, { "My PLA", "My PLA", PLA_04, PLA_04 });
        NozzleFilament::SlotTarget first = target(*bundle, 0);
        CHECK(first.reason == Reason::UserPresetKept);
        CHECK(first.to == "My PLA");
        CHECK(first.suggestion == PLA_02);
        CHECK_THAT(first.preset_size, Catch::Matchers::WithinAbs(0.4, 1e-9));
        CHECK(target(*bundle, 1).reason == Reason::Unchanged);

        add_user_filament(*bundle, "My PLA fine", PLA_02);
        first = target(*bundle, 0);
        CHECK(first.reason == Reason::Switched);
        CHECK(first.to == "My PLA fine");

        add_user_filament(*bundle, "My other PLA fine", PLA_02);
        first = target(*bundle, 0);
        CHECK(first.reason == Reason::UserPresetKept);
        CHECK(first.suggestion == PLA_02);

        // No version for the size at all: kept, nothing to suggest.
        add_user_filament(*bundle, "My TPU", TPU_04);
        bundle->filament_presets[0] = "My TPU";
        first = target(*bundle, 0);
        CHECK(first.reason == Reason::UserPresetKept);
        CHECK(first.suggestion.empty());
    }

    SECTION("a user preset that was left for its size returns with the size") {
        add_user_filament(*bundle, "My PLA", PLA_04);
        select_u1(*bundle, { 0.2, 0.4, 0.4, 0.4 }, { "My PLA", PLA_04, PLA_04, PLA_04 });
        auto set_head = [&](double size) { bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->values[0] = size; };
        CHECK(target(*bundle, 0).reason == Reason::UserPresetKept);

        // The user picks the 0.2 version by hand (Plater::priv::on_select_preset notes the outgoing preset).
        NozzleFilament::remember_user_preset(*bundle, 0, "My PLA");
        CHECK(bundle->nozzle_filament_memory.size() == 1);
        CHECK(bundle->nozzle_filament_memory.begin()->first.second == U1_04);
        bundle->filament_presets[0] = PLA_02;
        CHECK(target(*bundle, 0).reason == Reason::Unchanged);

        set_head(0.4);
        NozzleFilament::SlotTarget first = target(*bundle, 0);
        CHECK(first.reason == Reason::Switched);
        CHECK(first.to == "My PLA");
        // A slot of another material is not answered from the memory.
        bundle->filament_presets[1] = TPU_04;
        CHECK(target(*bundle, 1).reason == Reason::Unchanged);
        // Without the memory the way back ends on the system preset.
        bundle->nozzle_filament_memory.clear();
        first = target(*bundle, 0);
        CHECK(first.to == PLA_04);

        // A user preset that fits its tool head and is replaced is a decision, and is not noted.
        bundle->filament_presets[0] = "My PLA";
        NozzleFilament::remember_user_preset(*bundle, 0, "My PLA");
        NozzleFilament::remember_user_preset(*bundle, 1, PLA_04);
        CHECK(bundle->nozzle_filament_memory.empty());

        // A pass notes what it leaves: with a second 0.4 user preset the way back would be ambiguous.
        add_user_filament(*bundle, "My PLA fine", PLA_02);
        set_head(0.2);
        CHECK(bundle->apply_nozzle_filament_targets(bundle->nozzle_filament_targets({})) == 1);
        CHECK(bundle->filament_presets[0] == "My PLA fine");
        add_user_filament(*bundle, "My second PLA", PLA_04);
        set_head(0.4);
        first = target(*bundle, 0);
        CHECK(first.reason == Reason::Switched);
        CHECK(first.to == "My PLA");
        bundle->nozzle_filament_memory.clear();
        CHECK(target(*bundle, 0).reason == Reason::UserPresetKept);

        // With the rule off nothing is noted.
        set_head(0.2);
        bundle->filament_presets[0] = "My PLA";
        bundle->nozzle_filament_enabled = false;
        NozzleFilament::remember_user_preset(*bundle, 0, "My PLA");
        CHECK(bundle->nozzle_filament_memory.empty());
    }

    SECTION("a preset that restricts no printer fits every head and is flagged on an off-size one") {
        DynamicPrintConfig config = system_filament(*bundle, PLA_04).config;
        config.option<ConfigOptionString>("inherits", true)->value.clear();
        config.option<ConfigOptionStrings>("compatible_printers", true)->values.clear();
        bundle->filaments.load_preset(std::string(), "Root PLA", std::move(config), false).is_visible = true;
        select_u1(*bundle, { 0.2, 0.4, 0.4, 0.4 }, { "Root PLA", "Root PLA", PLA_04, PLA_04 });
        CHECK(target(*bundle, 0).reason == Reason::Unchanged);
        CHECK(target(*bundle, 0).size_agnostic);
        CHECK(target(*bundle, 1).reason == Reason::Unchanged);
        CHECK_FALSE(target(*bundle, 1).size_agnostic);
    }

    SECTION("unsaved changes stay with the preset while another slot names it") {
        select_u1(*bundle, { 0.2, 0.4, 0.4, 0.4 }, { PLA_04, PLA_04, PLA_04, PLA_04 });
        bundle->filaments.get_edited_preset().config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values[0] += 1.;
        REQUIRE(bundle->filaments.current_is_dirty());
        // Slots 2 to 4 keep naming the preset: slot 1 switches, the changes stay where they are.
        CHECK(target(*bundle, 0).reason == Reason::Switched);

        // Every slot that names it would leave: kept.
        bundle->filament_presets = { PLA_04, TPU_04, TPU_04, TPU_04 };
        CHECK(target(*bundle, 0).reason == Reason::DirtyKept);
        CHECK(target(*bundle, 0).to == PLA_04);
        CHECK_FALSE(target(*bundle, 0).switches());

        // Two slots would leave, but the pass moves one tool head only: the other keeps the preset.
        bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.2, 0.2, 0.4, 0.4 };
        bundle->filament_presets = { PLA_04, PLA_04, TPU_04, TPU_04 };
        CHECK(target(*bundle, 0).reason == Reason::DirtyKept);
        CHECK(target(*bundle, 0, { 0 }).reason == Reason::Switched);

        // A clean edited preset switches.
        bundle->filaments.discard_current_changes();
        CHECK(target(*bundle, 0).reason == Reason::Switched);
    }
}

TEST_CASE("filament_heads decides the tool head of a filament as the engine does", "[NozzleFilament][nozzle_filament_heads]")
{
    // The results tests/slic3rutils/test_high_flow_notices.cpp pins for the function this one replaced.
    CHECK(NozzleFilament::filament_heads(6, 4, { 1, 1, 1, 1, 1, 1 }, false) == std::vector<size_t>{ 0, 1, 2, 3, 0, 0 });
    CHECK(NozzleFilament::filament_heads(6, 4, {}, false, 2) == std::vector<size_t>{ 0, 1, 2, 3, 2, 2 });
    CHECK(NozzleFilament::filament_heads(5, 4, {}, false, 7) == std::vector<size_t>{ 0, 1, 2, 3, 0 });
    CHECK(NozzleFilament::filament_heads(3, 0, { 1, 2, 3 }, true) == std::vector<size_t>{ 0, 0, 0 });
    CHECK(NozzleFilament::filament_heads(5, 4, { 2, 1, 4, 4, 3 }, true) == std::vector<size_t>{ 1, 0, 3, 3, 2 });
    CHECK(NozzleFilament::filament_heads(5, 4, { 2, 0, 9 }, true) == std::vector<size_t>{ 1, 1, 2, 3, 0 });
}

// ---- The per slot half of the preset layer (PresetBundle) ----

namespace {

namespace fs = boost::filesystem;

// Heads 0.2 / 0.4 / 0.4 / 0.4 on the 0.4 printer preset, the flags computed.
void select_reference_mix(PresetBundle &bundle, const std::vector<std::string> &slots)
{
    select_u1(bundle, { 0.2, 0.4, 0.4, 0.4 }, slots);
    bundle.update_compatible(PresetSelectCompatibleType::Never);
}

std::vector<bool> fits_per_slot(const PresetBundle &bundle, const std::string &name)
{
    const Preset *preset = bundle.filaments.find_preset(name, false);
    REQUIRE(preset != nullptr);
    std::vector<bool> out;
    for (size_t slot = 0; slot < bundle.filament_presets.size(); ++slot)
        out.push_back(bundle.filament_slot_fits(*preset, slot));
    return out;
}

std::vector<bool> selectable_per_slot(const PresetBundle &bundle, const std::string &name)
{
    const Preset *preset = bundle.filaments.find_preset(name, false);
    REQUIRE(preset != nullptr);
    std::vector<bool> out;
    for (size_t slot = 0; slot < bundle.filament_presets.size(); ++slot)
        out.push_back(bundle.filament_slot_selectable(*preset, slot));
    return out;
}

std::vector<std::string> aliases(const PresetBundle &bundle)
{
    std::vector<std::string> out;
    for (const std::string &name : bundle.filament_presets) {
        const Preset *preset = bundle.filaments.find_preset(name, false);
        out.push_back(preset == nullptr ? std::string() : preset->alias);
    }
    return out;
}

std::map<std::string, bool> compatibility_flags(const PresetBundle &bundle)
{
    std::map<std::string, bool> out;
    for (const Preset &preset : bundle.filaments)
        out[preset.name] = preset.is_compatible;
    return out;
}

struct TempDir
{
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / fs::unique_path("orca-nozzle-filament-%%%%-%%%%")) { fs::create_directories(path); }
    ~TempDir() { boost::system::error_code ec; fs::remove_all(path, ec); }
};

// resources_dir() / data_dir() are process-wide: restored however the case leaves.
struct ScopedDirs
{
    std::string prev_data { data_dir() }, prev_rsrc { resources_dir() };
    ScopedDirs(const fs::path &data, const fs::path &rsrc) { set_data_dir(data.string()); set_resources_dir(rsrc.string()); }
    ~ScopedDirs() { set_data_dir(prev_data); set_resources_dir(prev_rsrc); }
};

void copy_tree(const fs::path &from, const fs::path &to)
{
    fs::create_directories(to);
    for (fs::recursive_directory_iterator it(from), end; it != end; ++it) {
        const fs::path target = to / fs::relative(it->path(), from);
        if (fs::is_directory(it->path()))
            fs::create_directories(target);
        else
            fs::copy_file(it->path(), target);
    }
}

} // namespace

TEST_CASE("A filament slot is rated against the machine preset of its tool head", "[NozzleFilament][nozzle_filament_predicates]")
{
    auto bundle = load_snapmaker_bundle();
    add_user_filament(*bundle, "My PLA", PLA_04);
    select_reference_mix(*bundle, { PLA_02, PLA_04, PLA_04, PLA_04 });

    // The flag keeps mainline's meaning: the 0.2 version is not compatible with the 0.4 printer preset.
    CHECK_FALSE(system_filament(*bundle, PLA_02).is_compatible);
    CHECK(system_filament(*bundle, PLA_04).is_compatible);

    CHECK(fits_per_slot(*bundle, PLA_02) == std::vector<bool>{ true, false, false, false });
    CHECK(selectable_per_slot(*bundle, PLA_02) == std::vector<bool>{ true, false, false, false });
    CHECK(fits_per_slot(*bundle, PLA_04) == std::vector<bool>{ false, true, true, true });
    CHECK(selectable_per_slot(*bundle, PLA_04) == std::vector<bool>{ false, true, true, true });
    // No 0.2 TPU exists: the 0.4 one may print on the 0.2 head.
    CHECK(fits_per_slot(*bundle, TPU_04) == std::vector<bool>{ false, true, true, true });
    CHECK(selectable_per_slot(*bundle, TPU_04) == std::vector<bool>{ true, true, true, true });
    // A user preset is never hidden by its size.
    CHECK(fits_per_slot(*bundle, "My PLA") == std::vector<bool>{ false, true, true, true });
    CHECK(selectable_per_slot(*bundle, "My PLA") == std::vector<bool>{ true, true, true, true });
    // A preset of another size than any head.
    CHECK(fits_per_slot(*bundle, PLA_08) == std::vector<bool>{ false, false, false, false });
    CHECK(selectable_per_slot(*bundle, PLA_08) == std::vector<bool>{ false, false, false, false });

    SECTION("a slot without a tool head and a printer without mixed sizes read the flag") {
        bundle->filament_presets.push_back(PLA_04);
        CHECK(bundle->filament_slot_fits(system_filament(*bundle, PLA_04), 4));
        CHECK_FALSE(bundle->filament_slot_fits(system_filament(*bundle, PLA_02), 4));
        CHECK_FALSE(bundle->filament_slot_selectable(system_filament(*bundle, PLA_02), 4));
        bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.4, 0.4, 0.4, 0.4 };
        CHECK(fits_per_slot(*bundle, PLA_02) == std::vector<bool>{ false, false, false, false, false });
        CHECK(fits_per_slot(*bundle, PLA_04) == std::vector<bool>{ true, true, true, true, true });
    }

    SECTION("a library preset excluded from the active printer fits nowhere") {
        static VendorProfile library = [] { VendorProfile v(PresetBundle::ORCA_FILAMENT_LIBRARY); v.name = PresetBundle::ORCA_FILAMENT_LIBRARY; return v; }();
        DynamicPrintConfig config = system_filament(*bundle, PLA_04).config;
        config.option<ConfigOptionStrings>("compatible_printers", true)->values.clear();
        Preset &preset = bundle->filaments.load_preset(std::string(), "Library PLA", std::move(config), false);
        preset.is_visible = true;
        preset.is_system  = true;
        preset.vendor     = &library;
        bundle->update_compatible(PresetSelectCompatibleType::Never);
        CHECK(fits_per_slot(*bundle, "Library PLA") == std::vector<bool>{ true, true, true, true });
        for (Preset &p : bundle->filaments)
            if (p.name == "Library PLA")
                p.m_excluded_from = { U1_04 };
        bundle->update_compatible(PresetSelectCompatibleType::Never);
        CHECK(fits_per_slot(*bundle, "Library PLA") == std::vector<bool>{ false, false, false, false });
        CHECK(selectable_per_slot(*bundle, "Library PLA") == std::vector<bool>{ false, false, false, false });
    }

    SECTION("a condition is evaluated against the machine preset of the tool head") {
        DynamicPrintConfig config = system_filament(*bundle, PLA_04).config;
        config.option<ConfigOptionStrings>("compatible_printers", true)->values.clear();
        config.option<ConfigOptionString>("compatible_printers_condition", true)->value = "nozzle_diameter[0]==0.2";
        bundle->filaments.load_preset(std::string(), "Fine only", std::move(config), false).is_visible = true;
        // The 0.2 head second, so that the vector of the edited printer preset starts with 0.4.
        bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.4, 0.2, 0.4, 0.4 };
        bundle->update_compatible(PresetSelectCompatibleType::Never);
        CHECK(fits_per_slot(*bundle, "Fine only") == std::vector<bool>{ false, true, false, false });
    }
}

TEST_CASE("update_compatible keeps and repairs the slots per tool head", "[NozzleFilament][nozzle_filament_update_compatible]")
{
    auto bundle = load_snapmaker_bundle();

    SECTION("a correct set stays, and the editor stays on the 0.2 version a slot holds") {
        select_reference_mix(*bundle, { PLA_02, PLA_04, TPU_04, PLA_04 });
        REQUIRE(bundle->filaments.get_selected_preset_name() == PLA_02);
        bundle->update_compatible(PresetSelectCompatibleType::Always);
        CHECK(bundle->filament_presets == std::vector<std::string>{ PLA_02, PLA_04, TPU_04, PLA_04 });
        CHECK(bundle->filaments.get_selected_preset_name() == PLA_02);
        CHECK_FALSE(bundle->filaments.get_edited_preset().is_compatible);
    }
    SECTION("the editor leaves the 0.2 version when no slot holds it fittingly") {
        select_reference_mix(*bundle, { TPU_04, PLA_04, PLA_04, PLA_04 });
        REQUIRE(bundle->filaments.select_preset_by_name(PLA_02, true));
        bundle->update_compatible(PresetSelectCompatibleType::Always);
        CHECK(bundle->filaments.get_selected_preset_name() != PLA_02);
        CHECK(bundle->filaments.get_edited_preset().is_compatible);
        CHECK(bundle->filament_presets == std::vector<std::string>{ TPU_04, PLA_04, PLA_04, PLA_04 });
    }
    SECTION("a scrambled set is repaired without changing any material") {
        select_reference_mix(*bundle, { PLA_04, PLA_02, "Generic TPU @U1 0.8 nozzle", "Snapmaker PLA SnapSpeed @U1 0.6 nozzle" });
        const std::vector<std::string> before = aliases(*bundle);
        bundle->update_compatible(PresetSelectCompatibleType::Always);
        CHECK(bundle->filament_presets == std::vector<std::string>{ PLA_02, PLA_04, TPU_04, "Snapmaker PLA SnapSpeed @U1" });
        CHECK(aliases(*bundle) == before);
    }
    SECTION("Never touches nothing") {
        select_reference_mix(*bundle, { PLA_04, PLA_02, PLA_08, PLA_06 });
        bundle->update_compatible(PresetSelectCompatibleType::Never);
        CHECK(bundle->filament_presets == std::vector<std::string>{ PLA_04, PLA_02, PLA_08, PLA_06 });
    }
    SECTION("an unknown name gets a preset for the size of its tool head") {
        select_reference_mix(*bundle, { PLA_02, PLA_04, PLA_04, PLA_04 });
        bundle->filament_presets[0] = "No such filament";
        bundle->filament_presets[1] = "No such filament";
        bundle->update_compatible(PresetSelectCompatibleType::Always);
        CHECK(fits_per_slot(*bundle, bundle->filament_presets[0])[0]);
        CHECK(fits_per_slot(*bundle, bundle->filament_presets[1])[1]);
        CHECK(bundle->filament_presets[0] != bundle->filament_presets[1]);
    }
}

TEST_CASE("The nozzle size rule is inert outside its gate", "[NozzleFilament][nozzle_filament_gate]")
{
    SECTION("Preset::is_compatible is the same with the rule on and off, mixed or not") {
        const std::vector<double> head_sizes = GENERATE(std::vector<double>{ 0.4, 0.4, 0.4, 0.4 }, std::vector<double>{ 0.2, 0.4, 0.6, 0.8 });
        auto on  = load_snapmaker_bundle();
        auto off = load_snapmaker_bundle();
        for (PresetBundle *bundle : { on.get(), off.get() }) {
            add_user_filament(*bundle, "My PLA", PLA_02);
            select_u1(*bundle, head_sizes, { PLA_04, PLA_02, PLA_06, TPU_04 });
        }
        off->nozzle_filament_enabled = false;
        for (PresetSelectCompatibleType type : { PresetSelectCompatibleType::Never, PresetSelectCompatibleType::OnlyIfWasCompatible, PresetSelectCompatibleType::Always }) {
            on->update_compatible(type);
            off->update_compatible(type);
            CHECK(compatibility_flags(*on) == compatibility_flags(*off));
        }
    }

    SECTION("every head on the size of the printer preset: the slots come out as with the rule off") {
        auto on  = load_snapmaker_bundle();
        auto off = load_snapmaker_bundle();
        for (PresetBundle *bundle : { on.get(), off.get() })
            select_u1(*bundle, { 0.4, 0.4, 0.4, 0.4 }, { PLA_02, PLA_04, "No such filament", "Generic TPU @U1 0.8 nozzle" });
        off->nozzle_filament_enabled = false;
        REQUIRE(NozzleFilament::state(*on).rule_on);
        REQUIRE_FALSE(NozzleFilament::state(*on).mixed);

        on->update_compatible(PresetSelectCompatibleType::Always);
        off->update_compatible(PresetSelectCompatibleType::Always);
        CHECK(on->filament_presets == off->filament_presets);
        CHECK(on->filaments.get_selected_preset_name() == off->filaments.get_selected_preset_name());

        AppConfig config_on, config_off;
        for (AppConfig *config : { &config_on, &config_off }) {
            config->set_printer_setting(U1_04, PRESET_PRINT_NAME, U1_PROCESS_04);
            config->set_printer_setting(U1_04, PRESET_FILAMENT_NAME, PLA_04);
            config->set_printer_setting(U1_04, "filament_01", PLA_02);
            config->set_printer_setting(U1_04, "filament_02", "No such filament");
            config->set_printer_setting(U1_04, "filament_03", PLA_04);
        }
        on->update_selections(config_on);
        off->update_selections(config_off);
        CHECK(on->filament_presets == off->filament_presets);
        CHECK(on->filament_presets.size() == 4);

        on->set_num_filaments(6);
        off->set_num_filaments(6);
        CHECK(on->filament_presets == off->filament_presets);
        CHECK(on->filament_presets.size() == 6);
    }

    SECTION("a printer of another vendor is left alone") {
        // As the application loads it: the filament library first, the vendor against it.
        PresetBundle library;
        library.load_vendor_configs_from_json(PROFILES_DIR, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr, /*allow_cache=*/false);
        PresetBundle bundle;
        bundle.load_vendor_configs_from_json(PROFILES_DIR, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent, &library, /*allow_cache=*/false);
        REQUIRE(bundle.printers.select_preset_by_name("Bambu Lab H2D 0.4 nozzle", true));
        bundle.nozzle_filament_enabled = true;
        auto *diameters = bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
        REQUIRE(diameters != nullptr);
        REQUIRE(diameters->values.size() == 2);
        diameters->values[1] = 0.2;
        bundle.filament_presets.assign(2, bundle.filaments.first_visible().name);
        const NozzleFilament::State state = NozzleFilament::state(bundle);
        CHECK_FALSE(state.rule_on);
        CHECK_FALSE(state.mixed);
        CHECK(bundle.nozzle_filament_targets().empty());
    }

    SECTION("a Snapmaker printer preset without a vendor fails closed") {
        auto bundle = load_snapmaker_bundle();
        DynamicPrintConfig config = machine(*bundle, U1_04).config;
        config.option<ConfigOptionString>("inherits", true)->value.clear();
        bundle->printers.load_preset(std::string(), "Detached U1", std::move(config), false).is_visible = true;
        select_u1(*bundle, { 0.2, 0.4, 0.4, 0.4 }, { PLA_04, PLA_04, PLA_04, PLA_04 }, "Detached U1");
        CHECK_FALSE(NozzleFilament::state(*bundle).rule_on);
        CHECK(bundle->nozzle_filament_targets().empty());

        // A user preset made from the system one has the vendor of its parent.
        DynamicPrintConfig child = machine(*bundle, U1_04).config;
        child.option<ConfigOptionString>("inherits", true)->value = U1_04;
        bundle->printers.load_preset(std::string(), "My U1", std::move(child), false).is_visible = true;
        select_u1(*bundle, { 0.2, 0.4, 0.4, 0.4 }, { PLA_04, PLA_04, PLA_04, PLA_04 }, "My U1");
        CHECK(NozzleFilament::state(*bundle).rule_on);
        CHECK(NozzleFilament::state(*bundle).mixed);
    }

    SECTION("the flag is off") {
        auto bundle = load_snapmaker_bundle();
        select_reference_mix(*bundle, { PLA_04, PLA_04, PLA_04, PLA_04 });
        bundle->nozzle_filament_enabled = false;
        CHECK(bundle->nozzle_filament_targets().empty());
        CHECK(fits_per_slot(*bundle, PLA_04) == std::vector<bool>{ true, true, true, true });
        CHECK(fits_per_slot(*bundle, PLA_02) == std::vector<bool>{ false, false, false, false });
        bundle->update_compatible(PresetSelectCompatibleType::Always);
        CHECK(bundle->filament_presets == std::vector<std::string>{ PLA_04, PLA_04, PLA_04, PLA_04 });
    }
}

TEST_CASE("After a printer preset change the kept names yield the versions of the new size", "[NozzleFilament][nozzle_filament_printer_change]")
{
    auto bundle = load_snapmaker_bundle();

    // The 0.4 version of every family, and whether the family has a 0.2 version.
    std::vector<std::pair<std::string, bool>> families;
    for (const Preset &preset : bundle->filaments) {
        const auto *list = preset.config.option<ConfigOptionStrings>("compatible_printers");
        if (!preset.is_system || list == nullptr || list->values != std::vector<std::string>{ U1_04 })
            continue;
        families.emplace_back(preset.name, NozzleFilament::version_for(bundle->filaments, preset, machine(*bundle, U1_02)) != nullptr);
    }
    REQUIRE(families.size() == 43);

    // Tab::select_preset restores the filament names after the printer preset changed; every head
    // is on the size of the new printer preset, so nothing is mixed and the rule alone moves the slots.
    size_t switched = 0, no_version = 0;
    for (size_t first = 0; first < families.size(); first += 4) {
        std::vector<std::string> slots;
        for (size_t i = first; i < std::min(first + 4, families.size()); ++i)
            slots.push_back(families[i].first);
        select_u1(*bundle, std::vector<double>(4, 0.2), slots, U1_02);
        REQUIRE_FALSE(NozzleFilament::state(*bundle).mixed);
        const std::vector<NozzleFilament::SlotTarget> targets = bundle->nozzle_filament_targets();
        REQUIRE(targets.size() == slots.size());
        for (const NozzleFilament::SlotTarget &target : targets) {
            INFO(target.from);
            if (families[first + target.slot].second) {
                CHECK(target.reason == NozzleFilament::Reason::Switched);
                CHECK(NozzleFilament::preset_nozzle_size(*bundle, system_filament(*bundle, target.to)) == 0.2);
                CHECK(system_filament(*bundle, target.to).alias == system_filament(*bundle, target.from).alias);
                ++switched;
            } else {
                CHECK(target.reason == NozzleFilament::Reason::NoVersion);
                CHECK(target.to == target.from);
                ++no_version;
            }
        }
        CHECK(bundle->apply_nozzle_filament_targets(targets) == size_t(std::count_if(targets.begin(), targets.end(), [](const auto &t) { return t.switches(); })));
    }
    CHECK(switched == 19);
    CHECK(no_version == 24);
}

TEST_CASE("Broken slots are repaired per tool head", "[NozzleFilament][nozzle_filament_repair]")
{
    auto bundle = load_snapmaker_bundle();

    SECTION("the loop that ends update_selections and load_selections") {
        select_u1(*bundle, { 0.2, 0.8, 0.4, 0.4 }, { PLA_02, PLA_08, PLA_04, PLA_04 });
        bundle->update_compatible(PresetSelectCompatibleType::Never);
        // Not installed any more: update_compatible() does not mind, the loop does.
        for (Preset &preset : bundle->filaments)
            if (preset.name == PLA_02 || preset.name == PLA_08)
                preset.is_visible = false;
        REQUIRE(bundle->repair_filament_slots_per_head());
        CHECK(bundle->filament_presets[0] != PLA_02);
        CHECK(bundle->filament_presets[1] != PLA_08);
        CHECK(bundle->filament_presets[0] != bundle->filament_presets[1]);
        CHECK(fits_per_slot(*bundle, bundle->filament_presets[0])[0]);
        CHECK(fits_per_slot(*bundle, bundle->filament_presets[1])[1]);
        CHECK(bundle->filament_presets[2] == PLA_04);
        CHECK(bundle->filament_presets[3] == PLA_04);
    }
    SECTION("a slot of the wrong size gets the version of its material") {
        select_u1(*bundle, { 0.2, 0.8, 0.4, 0.4 }, { PLA_04, PLA_04, PLA_04, PLA_04 });
        bundle->update_compatible(PresetSelectCompatibleType::Never);
        REQUIRE(bundle->repair_filament_slots_per_head());
        CHECK(bundle->filament_presets == std::vector<std::string>{ PLA_02, PLA_08, PLA_04, PLA_04 });
    }
    SECTION("nothing mixed: mainline's loop has to run") {
        select_u1(*bundle, { 0.4, 0.4, 0.4, 0.4 }, { PLA_02, PLA_04, PLA_04, PLA_04 });
        CHECK_FALSE(bundle->repair_filament_slots_per_head());
        CHECK(bundle->filament_presets.front() == PLA_02);
    }
    SECTION("update_selections on a printer preset with mixed sizes") {
        select_u1(*bundle, { 0.4, 0.2, 0.8, 0.4 }, { PLA_04, PLA_04, PLA_04, PLA_04 });
        AppConfig config;
        config.set_printer_setting(U1_04, PRESET_PRINT_NAME, U1_PROCESS_04);
        config.set_printer_setting(U1_04, PRESET_FILAMENT_NAME, PLA_04);
        config.set_printer_setting(U1_04, "filament_01", "No such filament");
        config.set_printer_setting(U1_04, "filament_02", "No such filament either");
        config.set_printer_setting(U1_04, "filament_03", TPU_04);
        bundle->update_selections(config);
        REQUIRE(bundle->filament_presets.size() == 4);
        CHECK(bundle->filament_presets[0] == PLA_04);
        CHECK(fits_per_slot(*bundle, bundle->filament_presets[1])[1]);
        CHECK(fits_per_slot(*bundle, bundle->filament_presets[2])[2]);
        CHECK(bundle->filament_presets[1] != bundle->filament_presets[2]);
        CHECK(bundle->filament_presets[3] == TPU_04);
    }
}

TEST_CASE("A new filament slot starts with the version for its tool head", "[NozzleFilament][nozzle_filament_new_slot]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, { 0.4, 0.4, 0.4, 0.6 }, { PLA_04, PLA_04, PLA_04 });
    bundle->update_compatible(PresetSelectCompatibleType::Never);

    SECTION("with a colour") {
        bundle->set_num_filaments(4, std::string("#FF0000"));
        CHECK(bundle->filament_presets == std::vector<std::string>{ PLA_04, PLA_04, PLA_04, PLA_06 });
    }
    SECTION("with a list of colours, beyond the tool heads") {
        bundle->set_num_filaments(5, std::vector<std::string>{ "#FF0000", "#00FF00" });
        CHECK(bundle->filament_presets == std::vector<std::string>{ PLA_04, PLA_04, PLA_04, PLA_06, PLA_04 });
    }
    SECTION("a version that is not installed is not taken here") {
        for (Preset &preset : bundle->filaments)
            if (preset.name == PLA_06)
                preset.is_visible = false;
        bundle->set_num_filaments(4);
        CHECK(bundle->filament_presets == std::vector<std::string>{ PLA_04, PLA_04, PLA_04, PLA_04 });
    }
    SECTION("the flag is off") {
        bundle->nozzle_filament_enabled = false;
        bundle->set_num_filaments(4);
        CHECK(bundle->filament_presets == std::vector<std::string>{ PLA_04, PLA_04, PLA_04, PLA_04 });
    }
}

TEST_CASE("Applying the targets installs a version that was not installed", "[NozzleFilament][nozzle_filament_install]")
{
    AppConfig config;
    config.set(AppConfig::SECTION_FILAMENTS, PLA_04, "true");
    {
        auto bundle = load_snapmaker_bundle();
        select_reference_mix(*bundle, { PLA_04, PLA_04, PLA_04, PLA_04 });
        for (Preset &preset : bundle->filaments)
            preset.set_visible_from_appconfig(config);
        REQUIRE_FALSE(system_filament(*bundle, PLA_02).is_visible);

        std::vector<NozzleFilament::SlotTarget> targets = bundle->nozzle_filament_targets({ 0 });
        REQUIRE(targets.size() == 1);
        REQUIRE(targets.front().to == PLA_02);
        // A slot that changed since the targets were computed is left alone.
        bundle->filament_presets[0] = TPU_04;
        CHECK(bundle->apply_nozzle_filament_targets(targets, &config) == 0);
        CHECK_FALSE(system_filament(*bundle, PLA_02).is_visible);
        bundle->filament_presets[0] = PLA_04;
        CHECK(bundle->apply_nozzle_filament_targets(targets, &config) == 1);
        CHECK(bundle->filament_presets == std::vector<std::string>{ PLA_02, PLA_04, PLA_04, PLA_04 });
        CHECK(system_filament(*bundle, PLA_02).is_visible);
        // Applied: nothing left to do.
        CHECK(bundle->apply_nozzle_filament_targets(bundle->nozzle_filament_targets(), &config) == 0);
    }
    // The next start reads the installed filaments from the application config.
    auto reloaded = load_snapmaker_bundle();
    for (Preset &preset : reloaded->filaments)
        preset.set_visible_from_appconfig(config);
    CHECK(system_filament(*reloaded, PLA_02).is_visible);
    CHECK(system_filament(*reloaded, PLA_04).is_visible);
    CHECK_FALSE(system_filament(*reloaded, PLA_06).is_visible);
}

TEST_CASE("The vendor cache yields the same families as the JSON files", "[NozzleFilament][VendorCache][nozzle_filament_cache]")
{
    TempDir        tmp;
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(tmp.path / "resources" / "profiles");
    copy_tree(fs::path(PROFILES_DIR) / "Snapmaker", user / "Snapmaker");
    fs::copy_file(fs::path(PROFILES_DIR) / "Snapmaker.json", user / "Snapmaker.json");
    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");

    PresetBundle from_json;
    from_json.set_generate_vendor_caches(true);
    from_json.load_vendor_configs_from_json(user.string(), "Snapmaker", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    REQUIRE(fs::exists(user / "Snapmaker.opc"));
    // Without the JSON files only the cache can answer.
    fs::remove_all(user / "Snapmaker");
    PresetBundle from_cache;
    from_cache.load_vendor_configs_from_json(user.string(), "Snapmaker", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);

    using Family = std::tuple<std::string, std::string, std::string>; // name, parent, alias
    auto table = [](const PresetBundle &bundle) {
        std::vector<Family> out;
        for (const Preset &preset : bundle.filaments)
            if (preset.is_system)
                out.emplace_back(preset.name, preset.system_inherits, preset.alias);
        return out;
    };
    const std::vector<Family> json_table = table(from_json);
    REQUIRE(json_table.size() > 129);
    CHECK(table(from_cache) == json_table);
    const Preset *pla = from_cache.filaments.find_preset(PLA_02, false);
    REQUIRE(pla != nullptr);
    CHECK(pla->system_inherits == "Generic PLA @U1 base");
    CHECK(pla->alias == "Generic PLA");
}

TEST_CASE("A project keeps the preset of every slot, whatever nozzle size it was made for", "[NozzleFilament][nozzle_filament_project]")
{
    // Tool head 1 carries a 0.2 mm nozzle and prints the preset for 0.2 mm, the other tool heads
    // print the preset of the same material for 0.4 mm. One filament per tool head: a load fills
    // the slots up to the tool heads anyway.
    const std::vector<std::string> slots = {PLA_02, PLA_04, PLA_04, PLA_04};
    auto          loaded = load_snapmaker_bundle();
    PresetBundle &bundle = *loaded;
    select_u1(bundle, {0.2, 0.4, 0.4, 0.4}, slots);
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2, 3, 4};
    // A project counts its filaments by their colours.
    bundle.project_config.option<ConfigOptionStrings>("filament_colour", true)->values = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    const double small_flow = system_filament(bundle, PLA_02).config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values.front();
    const double home_flow  = system_filament(bundle, PLA_04).config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values.front();
    REQUIRE(small_flow != home_flow); // else the presets cannot tell the slots apart

    // The first value of a filament's columns in a project configuration.
    auto flow_of = [](const DynamicPrintConfig &config, int filament) {
        const auto *flows = config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
        const auto *index = config.option<ConfigOptionInts>("filament_self_index");
        REQUIRE(flows != nullptr);
        if (index == nullptr || index->values.size() != flows->values.size())
            return flows->get_at(size_t(filament - 1));
        const auto it = std::find(index->values.begin(), index->values.end(), filament);
        REQUIRE(it != index->values.end());
        return flows->values[size_t(it - index->values.begin())];
    };

    Model model;
    REQUIRE(load_stl((std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl").c_str(), &model));
    model.add_default_instances();
    ScopedTemporaryDir backup_dir("orca_nozzle_filament");
    model.set_backup_path(backup_dir.string());

    DynamicPrintConfig project = bundle.full_config_secure();
    CHECK(project.option<ConfigOptionStrings>("filament_settings_id")->values == slots);
    CHECK(flow_of(project, 1) == small_flow);
    CHECK(flow_of(project, 2) == home_flow);
    // The rule adds no key and no layout. The number is 2 for every U1 project, because the U1
    // filaments carry a Standard and a High Flow column (test_snapmaker_flow_compat.cpp); the
    // preset of another nozzle size does not raise it.
    CHECK(project_schema_version_for(project) == 2);
    {
        select_u1(bundle, {0.4, 0.4, 0.4, 0.4}, {PLA_04, PLA_04, PLA_04, PLA_04});
        CHECK(project_schema_version_for(bundle.full_config_secure()) == 2);
    }

    ScopedTemporaryFile temp(".3mf");
    StoreParams         store_params;
    const std::string   path = temp.string();
    store_params.path     = path.c_str();
    store_params.model    = &model;
    store_params.config   = &project;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    PlateData *plate = new PlateData();
    plate->plate_index = 0;
    store_params.plate_data_list.push_back(plate);
    REQUIRE(store_bbs_3mf(store_params));

    Model                     dst_model;
    ScopedTemporaryDir        dst_backup_dir("orca_nozzle_filament_dst");
    dst_model.set_backup_path(dst_backup_dir.string());
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs             dst_plates;
    std::vector<Preset*>      project_presets;
    bool   is_bbl_3mf = false, is_orca_3mf = false;
    Semver file_version;
    REQUIRE(load_bbs_3mf(path.c_str(), &dst_config, &context, &dst_model, &dst_plates, &project_presets, &is_bbl_3mf, &is_orca_3mf,
                         &file_version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    CHECK(context.substitutions.empty());
    CHECK(ProjectSchemaRegistry::version_from(dst_config) == 2);
    CHECK_FALSE(ProjectSchemaRegistry::is_newer(dst_config));
    CHECK(dst_config.option<ConfigOptionStrings>("filament_settings_id")->values == slots);
    CHECK(dst_config.option<ConfigOptionFloats>("nozzle_diameter")->values == std::vector<double>{0.2, 0.4, 0.4, 0.4});

    // The load switches nothing, with the rule on or off.
    const bool rule_on = GENERATE(true, false);
    CAPTURE(rule_on);
    auto          reloaded = load_snapmaker_bundle();
    PresetBundle &second   = *reloaded;
    second.nozzle_filament_enabled = rule_on;
    Preset::normalize(dst_config);
    second.load_config_model("nozzle_filament.3mf", std::move(dst_config), file_version);
    CHECK(second.printers.get_edited_preset().name == U1_04);
    CHECK(second.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->values == std::vector<double>{0.2, 0.4, 0.4, 0.4});
    CHECK(second.filament_presets == slots);
    const DynamicPrintConfig again = second.full_config_secure();
    CHECK(flow_of(again, 1) == small_flow);
    CHECK(flow_of(again, 2) == home_flow);

    release_PlateData_list(dst_plates);
    delete plate;
}

TEST_CASE("The nozzle blacklist rates a filament against the nozzle of its tool head", "[NozzleFilament][nozzle_filament_blacklist]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.2, 0.4, 0.4, 0.4}, {PLA_02, PLA_04, TPU_04, PLA_04});

    SECTION("a preset is named by the alias of its system ancestor") {
        CHECK(NozzleFilament::blacklist_name(bundle->filaments, PLA_02) == "Generic PLA");
        CHECK(NozzleFilament::blacklist_name(bundle->filaments, PLA_04) == "Generic PLA");
        // A user preset is the material it was made from, whatever it is called.
        add_user_filament(*bundle, "Workshop spool @home", PLA_06);
        CHECK(NozzleFilament::blacklist_name(bundle->filaments, "Workshop spool @home") == "Generic PLA");
        // An unknown name keeps the rule of mainline: the part in front of the '@'.
        CHECK(NozzleFilament::blacklist_name(bundle->filaments, "  Some PLA @Some printer") == "Some PLA");
        CHECK(NozzleFilament::blacklist_name(bundle->filaments, "@") == "");
    }

    SECTION("size and flow type of the tool head, in one pass") {
        // The blacklist of this case: "Generic PLA" on a 0.2 mm Standard nozzle and on a 0.4 mm
        // High Flow nozzle, "Generic TPU" on every High Flow nozzle.
        const int standard = int(nvtStandard), high_flow = int(nvtHighFlow);
        size_t    lookups  = 0;
        const NozzleFilament::NozzleBlacklist blacklist = [&](double size, int volume_type) {
            ++lookups;
            std::vector<std::string> out;
            if ((std::abs(size - 0.2) < 1e-6 && volume_type == standard) || (std::abs(size - 0.4) < 1e-6 && volume_type == high_flow))
                out.emplace_back("Generic PLA");
            if (volume_type == high_flow)
                out.emplace_back("Generic TPU");
            return out;
        };
        const std::vector<std::string> names = {"Generic PLA", "Generic PLA", "Generic TPU", "Generic PLA"};
        const std::vector<double>      sizes = {0.2, 0.4, 0.4, 0.4};
        using Result = std::map<std::pair<double, int>, std::set<std::string>>;

        // Filament i on tool head i, all Standard: only the PLA of the 0.2 mm head is listed. Rated
        // against the first tool head alone, all three PLA slots would be.
        CHECK(NozzleFilament::blacklisted_by_head(names, {0, 1, 2, 3}, sizes, {standard, standard, standard, standard}, blacklist) ==
              Result{{{0.2, standard}, {"Generic PLA"}}});
        CHECK(lookups == 2); // one per kind of nozzle

        // Tool head 3 High Flow: its TPU is listed, the PLA of the Standard 0.4 mm heads is not.
        CHECK(NozzleFilament::blacklisted_by_head(names, {0, 1, 2, 3}, sizes, {standard, standard, high_flow, standard}, blacklist) ==
              Result{{{0.2, standard}, {"Generic PLA"}}, {{0.4, high_flow}, {"Generic TPU"}}});

        // The map of the plate moves the TPU to a Standard head and a PLA to the High Flow one.
        CHECK(NozzleFilament::blacklisted_by_head(names, {1, 2, 3, 3}, sizes, {standard, standard, high_flow, standard}, blacklist) ==
              Result{{{0.4, high_flow}, {"Generic PLA"}}});

        // A filament without a tool head and a filament without a name are not rated; a short
        // flow type list counts as its first entry.
        CHECK(NozzleFilament::blacklisted_by_head({"Generic PLA", "", "Generic PLA"}, {NozzleFilament::no_head, 0, 0}, sizes, {standard}, blacklist) ==
              Result{{{0.2, standard}, {"Generic PLA"}}});
        CHECK(NozzleFilament::blacklisted_by_head(names, {0, 1, 2, 3}, sizes, {}, NozzleFilament::NozzleBlacklist()).empty());
    }
}

TEST_CASE("The size marker of a filament combo label comes first, so a narrow combo cuts the name and not the size", "[NozzleFilament][nozzle_filament_label]")
{
    // "0.6 mm · Generic PLA": the sidebar combos cut a label at their width, and a name that
    // ended in the marker lost it ("Generic PLA · 0...").
    CHECK(NozzleFilament::size_marked_label("Generic PLA", "0.6 mm") == "0.6 mm \xC2\xB7 Generic PLA");
    // The modified suffix of the Filament tab stays at the end.
    CHECK(NozzleFilament::size_marked_label("Generic PLA (modified)", "0.2 mm") == "0.2 mm \xC2\xB7 Generic PLA (modified)");
    // A preset of the tool head's size carries no marker.
    CHECK(NozzleFilament::size_marked_label("Generic PLA", "") == "Generic PLA");
    // The marker is display only: the name is kept whole, so the stored name of an item can be the preset name.
    CHECK(NozzleFilament::size_marked_label("Snapmaker PLA SnapSpeed @U1 0.6 nozzle", "0.6 mm").find("Snapmaker PLA SnapSpeed @U1 0.6 nozzle") != std::string::npos);
}

// A U1 plate with 0.2 / 0.4 / 0.6 / 0.8 mm nozzles and "Generic PLA" in every slot. The cases make the
// preset-layer calls of a one-tool-head size change (Plater::follow_nozzle_sizes) and of the printer
// sync, on the modified system preset and on a user printer preset saved from it.
namespace {

const char *const USER_U1  = "Snapmaker U1";
const char *const MATTE_02 = "Snapmaker PLA Matte @U1 0.2 nozzle";
const char *const MATTE_04 = "Snapmaker PLA Matte @U1";
const char *const MATTE_06 = "Snapmaker PLA Matte @U1 0.6 nozzle";
const char *const MATTE_08 = "Snapmaker PLA Matte @U1 0.8 nozzle";

// "Save as" of the 0.4 mm system preset: a user preset with the configuration of its parent,
// "inherits" naming the parent, and no vendor of its own.
void add_user_u1(PresetBundle &bundle)
{
    DynamicPrintConfig config = machine(bundle, U1_04).config;
    config.option<ConfigOptionString>("inherits", true)->value = U1_04;
    Preset &preset = bundle.printers.load_preset(std::string(), USER_U1, std::move(config), false);
    preset.is_visible = true;
    REQUIRE_FALSE(preset.is_system);
    REQUIRE(preset.vendor == nullptr);
}

// The plate before the first nozzle changes: the printer preset, the 0.4 mm process, every tool
// head on 0.4 mm, `slots` in the slots, the Filament tab on the first of them, and the default
// project map (not binding: filament i is printed by tool head i).
void start_plate(PresetBundle &bundle, const char *printer, const std::vector<std::string> &slots)
{
    REQUIRE(bundle.printers.select_preset_by_name(printer, true));
    REQUIRE(bundle.prints.select_preset_by_name(U1_PROCESS_04, true));
    bundle.nozzle_filament_enabled = true;
    bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = std::vector<double>(4, 0.4);
    bundle.filament_presets = slots;
    REQUIRE(bundle.filaments.select_preset_by_name(slots.front(), true));
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = std::vector<int>(slots.size(), 1);
    bundle.update_compatible(PresetSelectCompatibleType::Never);
}

// Sidebar::apply_nozzle_diameter or the Printer tab: one tool head gets another size, then the
// pass over the slots of that tool head is computed and applied. Returns the targets of the pass.
std::vector<NozzleFilament::SlotTarget> set_head_size(PresetBundle &bundle, size_t head, double size)
{
    std::vector<double> &diameters = bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter", true)->values;
    REQUIRE(head < diameters.size());
    diameters[head] = size;
    std::vector<NozzleFilament::SlotTarget> targets = bundle.nozzle_filament_targets({ head });
    bundle.apply_nozzle_filament_targets(targets);
    return targets;
}

void check_nothing_left_to_switch(const PresetBundle &bundle)
{
    // What the persistent check (Plater::check_nozzle_filament_versions) would report.
    for (const NozzleFilament::SlotTarget &target : bundle.nozzle_filament_targets()) {
        INFO("filament " << target.slot + 1 << " holds \"" << target.from << "\", target \"" << target.to << "\"");
        CHECK_FALSE(target.switches());
    }
}

void manual_head_changes(bool user_printer)
{
    using NozzleFilament::Reason;
    auto bundle = load_snapmaker_bundle();
    if (user_printer)
        add_user_u1(*bundle);
    const char *printer = user_printer ? USER_U1 : U1_04;
    start_plate(*bundle, printer, { PLA_04, PLA_04, PLA_04, PLA_04 });

    // The gate: a Snapmaker printer preset (for the user preset, through the vendor of its parent).
    const NozzleFilament::State before = NozzleFilament::state(*bundle);
    REQUIRE(before.rule_on);
    CHECK_FALSE(before.mixed);

    // One tool head after the other, as in the sidebar; only the slot of that tool head moves.
    const std::vector<std::pair<size_t, double>> changes { { 0, 0.2 }, { 2, 0.6 }, { 3, 0.8 } };
    const std::vector<std::string>               expected { PLA_02, PLA_04, PLA_06, PLA_08 };
    for (const std::pair<size_t, double> &change : changes) {
        const size_t head = change.first;
        INFO("tool head " << head + 1 << " set to " << change.second << " mm");
        const std::vector<NozzleFilament::SlotTarget> targets = set_head_size(*bundle, head, change.second);
        REQUIRE(targets.size() == 1);
        CHECK(targets.front().slot == head);
        CHECK(targets.front().reason == Reason::Switched);
        CHECK(targets.front().from == PLA_04);
        CHECK(targets.front().to == expected[head]);
        CHECK(bundle->filament_presets[head] == expected[head]);
    }
    CHECK(NozzleFilament::state(*bundle).mixed);
    CHECK(bundle->filament_presets == expected);
    CHECK(bundle->printers.get_edited_preset().name == printer);
    check_nothing_left_to_switch(*bundle);

    // A size changed again: the slot of that tool head takes the version of the new size.
    set_head_size(*bundle, 3, 0.4);
    CHECK(bundle->filament_presets[3] == PLA_04);
    set_head_size(*bundle, 0, 0.8);
    CHECK(bundle->filament_presets[0] == PLA_08);
    CHECK(bundle->filament_presets == std::vector<std::string>{ PLA_08, PLA_04, PLA_06, PLA_04 });
    check_nothing_left_to_switch(*bundle);
}

void printer_sync(bool user_printer)
{
    auto bundle = load_snapmaker_bundle();
    if (user_printer)
        add_user_u1(*bundle);
    const char *printer = user_printer ? USER_U1 : U1_04;
    start_plate(*bundle, printer, { PLA_04, MATTE_04, PLA_04, PLA_04 });
    // The printer preset carries an unsaved change (tool head 1 was set to 0.2 mm by hand: the
    // sidebar shows "* Snapmaker U1"), plus a note the reported sizes do not overwrite, so that a
    // sync that dropped the unsaved changes by selecting the preset again would show.
    const char *const unsaved_note = "unsaved before the sync";
    bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter", true)->values[0] = 0.2;
    bundle->printers.get_edited_preset().config.opt_string("printer_notes", true) = unsaved_note;
    REQUIRE(bundle->printers.current_is_dirty());

    // What the printer reports: a size per tool head, tool head 2 on High Flow. The reference tool
    // head of the sync is tool head 2 (SSWCPProtocol::sync_reference_head: 0.2 mm offers no High
    // Flow, 0.4 mm does), so the plate takes the printer preset of 0.4 mm.
    const std::vector<std::string> reported_sizes { "0.2", "0.4", "0.6", "0.8" };
    const std::vector<int>         reported_flows { nvtStandard, nvtHighFlow, nvtStandard, nvtStandard };
    const Preset *picked = bundle->get_similar_printer_preset({}, reported_sizes[1]);
    REQUIRE(picked != nullptr);
    INFO("the sync picks \"" << picked->name << "\"");

    // The preset in use is kept if it matches the picked size (NozzleFilament::sync_keeps_printer_preset),
    // else the picked one is selected with the slot names kept. Here it is already 0.4 mm, so it
    // stays with its unsaved changes.
    if (!NozzleFilament::sync_keeps_printer_preset(bundle->printers.get_edited_preset(), *picked)) {
        const std::vector<std::string> kept = bundle->filament_presets;
        REQUIRE(bundle->printers.select_preset_by_name(picked->name, true));
        bundle->update_compatible(PresetSelectCompatibleType::OnlyIfWasCompatible);
        bundle->filament_presets = kept;
    }
    CHECK(bundle->printers.get_selected_preset().name == printer);
    CHECK(bundle->printers.get_edited_preset().name == printer);
    CHECK(bundle->printers.get_edited_preset().config.opt_string("printer_notes") == unsaved_note);
    CHECK(bundle->filament_presets == std::vector<std::string>{ PLA_04, MATTE_04, PLA_04, PLA_04 });

    // Every tool head gets its reported size, then one pass over all tool heads is applied
    // (Plater::NozzleFollowScope::finish).
    std::vector<double> &diameters = bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter", true)->values;
    REQUIRE(diameters.size() == reported_sizes.size());
    for (size_t head = 0; head < reported_sizes.size(); ++head)
        diameters[head] = NozzleFilament::parse_nozzle_size(reported_sizes[head]);
    bundle->apply_nozzle_filament_targets(bundle->nozzle_filament_targets());

    // Every slot holds the version of the size of its tool head; the High Flow tool head keeps the
    // 0.4 mm version.
    CHECK(bundle->filament_presets == std::vector<std::string>{ PLA_02, MATTE_04, PLA_06, PLA_08 });
    check_nothing_left_to_switch(*bundle);

    // Then the reported flow types (Plater::apply_reported_nozzle_flow_types), with no pass after
    // them: a target depends on the nozzle size alone, so a pass would move nothing (no-op check).
    bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = reported_flows;
    for (const NozzleFilament::SlotTarget &target : bundle->nozzle_filament_targets({ 1 })) {
        INFO("filament " << target.slot + 1 << " holds \"" << target.from << "\", target \"" << target.to << "\"");
        CHECK_FALSE(target.switches());
    }
    CHECK(bundle->filament_presets == std::vector<std::string>{ PLA_02, MATTE_04, PLA_06, PLA_08 });
    CHECK(bundle->printers.get_edited_preset().name == printer);

    // The High Flow tool head reads the High Flow column of the preset it holds.
    const Preset *held = bundle->filaments.find_preset(bundle->filament_presets[1], false, true);
    REQUIRE(held != nullptr);
    const int standard  = held->config.get_index_for_extruder(1, std::string(), etDirectDrive, nvtStandard, "filament_extruder_variant");
    const int high_flow = held->config.get_index_for_extruder(1, std::string(), etDirectDrive, NozzleVolumeType(reported_flows[1]), "filament_extruder_variant");
    CHECK(standard == 0);
    CHECK(high_flow == 1);
    const auto *max_flow = held->config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    REQUIRE(max_flow != nullptr);
    REQUIRE(high_flow >= 0);
    CHECK(max_flow->get_at(size_t(high_flow)) > max_flow->get_at(0));

    // The sizes of the other slots were followed as well: the 0.2 / 0.6 / 0.8 mm versions of the Matte.
    for (const auto &[head, version] : std::vector<std::pair<size_t, std::string>>{ { 0, MATTE_02 }, { 2, MATTE_06 }, { 3, MATTE_08 } }) {
        bundle->filament_presets[head] = MATTE_04;
        bundle->apply_nozzle_filament_targets(bundle->nozzle_filament_targets({ head }));
        INFO("tool head " << head + 1);
        CHECK(bundle->filament_presets[head] == version);
    }
}

// The plate loaded from a project: tool heads on 0.2-0.8 mm, every slot on 0.4 mm Generic PLA. The load
// keeps the names and the persistent check names slots 1, 3 and 4; a pass over one tool head moves
// that slot alone, the action of the notice moves every slot.
void loaded_plate(bool user_printer)
{
    using NozzleFilament::Reason;
    auto bundle = load_snapmaker_bundle();
    if (user_printer)
        add_user_u1(*bundle);
    const char *printer = user_printer ? USER_U1 : U1_04;
    REQUIRE(bundle->printers.select_preset_by_name(printer, true));
    REQUIRE(bundle->prints.select_preset_by_name(U1_PROCESS_04, true));
    bundle->nozzle_filament_enabled = true;
    bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = { 0.2, 0.4, 0.6, 0.8 };
    bundle->filament_presets = { PLA_04, PLA_04, PLA_04, PLA_04 };
    REQUIRE(bundle->filaments.select_preset_by_name(PLA_04, true));
    bundle->project_config.option<ConfigOptionInts>("filament_map", true)->values = { 1, 2, 3, 4 };
    bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = std::vector<int>(4, int(nvtStandard));
    // load_config_file_config ends in update_compatible(Never): the names of the project stay.
    bundle->update_compatible(PresetSelectCompatibleType::Never);
    CHECK(bundle->filament_presets == std::vector<std::string>{ PLA_04, PLA_04, PLA_04, PLA_04 });
    CHECK(bundle->printers.get_edited_preset().name == printer);

    // What the persistent check reports (and the log of the project load says per slot).
    const std::vector<NozzleFilament::SlotTarget> report = bundle->nozzle_filament_targets();
    REQUIRE(report.size() == 4);
    std::vector<size_t> switching;
    for (const NozzleFilament::SlotTarget &target : report)
        if (target.switches())
            switching.emplace_back(target.slot + 1);
    CHECK(switching == std::vector<size_t>{ 1, 3, 4 });
    CHECK(report[1].reason == Reason::Unchanged);
    CHECK(NozzleFilament::describe_target(report[3], "ProjectLoad", std::vector<int>(4, int(nvtStandard))) ==
          std::string("NozzleFilament: trigger=ProjectLoad filament=4 head=4 size=0.8 flow=Standard from=\"") + PLA_04 + "\" to=\"" + PLA_08 +
              "\" reason=Switched preset_size=0.4");

    // A pass over tool head 4 alone: slot 4 moves, slots 1 and 3 keep waiting.
    const std::vector<NozzleFilament::SlotTarget> one_head = bundle->nozzle_filament_targets({ 3 });
    REQUIRE(one_head.size() == 1);
    CHECK(one_head.front().to == PLA_08);
    CHECK(bundle->apply_nozzle_filament_targets(one_head) == 1);
    CHECK(bundle->filament_presets == std::vector<std::string>{ PLA_04, PLA_04, PLA_04, PLA_08 });

    // The action of the notice, or the sync: one pass over every tool head.
    CHECK(bundle->apply_nozzle_filament_targets(bundle->nozzle_filament_targets()) == 2);
    CHECK(bundle->filament_presets == std::vector<std::string>{ PLA_02, PLA_04, PLA_06, PLA_08 });
    check_nothing_left_to_switch(*bundle);

    // A target computed for a state that moved on is not written.
    CHECK(bundle->apply_nozzle_filament_targets(one_head) == 0);
    CHECK(bundle->filament_presets[3] == PLA_08);
}

} // namespace

TEST_CASE("The printer sync keeps the printer preset in use when it is made for the reported size", "[NozzleFilament][nozzle_filament_sync_keep]")
{
    auto bundle = load_snapmaker_bundle();

    SECTION("the system preset with unsaved sizes (\"* Snapmaker U1\")") {
        start_plate(*bundle, U1_04, { PLA_04, PLA_04, PLA_04, PLA_04 });
        bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = { 0.2, 0.4, 0.6, 0.8 };
        REQUIRE(bundle->printers.current_is_dirty());
        const Preset *picked = bundle->get_similar_printer_preset({}, "0.4");
        REQUIRE(picked != nullptr);
        CHECK(picked->name == U1_04);
        CHECK(NozzleFilament::sync_keeps_printer_preset(bundle->printers.get_edited_preset(), *picked));
    }
    SECTION("a user preset \"Snapmaker U1\" made from it") {
        add_user_u1(*bundle);
        start_plate(*bundle, USER_U1, { PLA_04, PLA_04, PLA_04, PLA_04 });
        const Preset &edited = bundle->printers.get_edited_preset();
        REQUIRE(edited.name == USER_U1);
        // Whichever preset of the model and size the picker names, the user preset stays.
        const Preset *picked = bundle->get_similar_printer_preset({}, "0.4");
        REQUIRE(picked != nullptr);
        CHECK(NozzleFilament::sync_keeps_printer_preset(edited, *picked));
        CHECK(NozzleFilament::sync_keeps_printer_preset(edited, machine(*bundle, U1_04)));
    }
    SECTION("another reference size: the machine preset of that size is selected") {
        start_plate(*bundle, U1_04, { PLA_04, PLA_04, PLA_04, PLA_04 });
        const Preset *picked = bundle->get_similar_printer_preset({}, "0.2");
        REQUIRE(picked != nullptr);
        CHECK(picked->name == U1_02);
        CHECK_FALSE(NozzleFilament::sync_keeps_printer_preset(bundle->printers.get_edited_preset(), *picked));
        CHECK_FALSE(NozzleFilament::sync_keeps_printer_preset(bundle->printers.get_edited_preset(), machine(*bundle, U1_08)));
    }
    SECTION("another printer model of the same size is selected") {
        CHECK_FALSE(NozzleFilament::sync_keeps_printer_preset(machine(*bundle, U1_04), machine(*bundle, "Snapmaker A350 (0.4 nozzle)")));
    }
}

TEST_CASE("The log line of a slot names the trigger, the tool head, its size and flow type, the presets and the reason", "[NozzleFilament][nozzle_filament_log]")
{
    using NozzleFilament::Reason;
    NozzleFilament::SlotTarget target;
    target.slot        = 1;
    target.head        = 1;
    target.from        = PLA_04;
    target.to          = PLA_04;
    target.reason      = Reason::Unchanged;
    target.head_size   = 0.4;
    target.preset_size = 0.4;
    CHECK(NozzleFilament::describe_target(target, "FlowType", { int(nvtStandard), int(nvtHighFlow) }) ==
          "NozzleFilament: trigger=FlowType filament=2 head=2 size=0.4 flow=High Flow from=\"Generic PLA\" to=\"Generic PLA\" reason=Unchanged preset_size=0.4");
    // A tool head beyond the flow types counts as Standard; a kept user preset names the version for the size.
    target.head        = 3;
    target.head_size   = 0.2;
    target.from        = "My PLA";
    target.to          = "My PLA";
    target.reason      = Reason::UserPresetKept;
    target.suggestion  = PLA_02;
    CHECK(NozzleFilament::describe_target(target, "SidebarNozzle", { int(nvtStandard) }) ==
          std::string("NozzleFilament: trigger=SidebarNozzle filament=2 head=4 size=0.2 flow=Standard from=\"My PLA\" to=\"My PLA\" reason=UserPresetKept "
                      "preset_size=0.4 suggestion=\"") + PLA_02 + "\"");
    // A slot without a tool head.
    NozzleFilament::SlotTarget beyond;
    beyond.slot = 4;
    CHECK(NozzleFilament::describe_target(beyond, "Check", {}) == "NozzleFilament: trigger=Check filament=5 head=- size=- flow=- from=\"\" to=\"\" reason=Skipped");
    CHECK(std::string(NozzleFilament::reason_name(Reason::HomeVersionUsed)) == "HomeVersionUsed");
    CHECK(std::string(NozzleFilament::reason_name(Reason::DirtyKept)) == "DirtyKept");
}

TEST_CASE("Repro: the loaded plate of the report is reported at load and moved by the action or by a pass over one tool head", "[NozzleFilament][Repro][nozzle_filament_repro_loaded]")
{
    SECTION("the modified system printer preset (\"* Snapmaker U1\")") { loaded_plate(false); }
    SECTION("a user printer preset \"Snapmaker U1\" made from it") { loaded_plate(true); }
}

TEST_CASE("Repro: a tool head size changed by hand moves the filament of that tool head to the version of the size", "[NozzleFilament][Repro][nozzle_filament_repro_manual]")
{
    SECTION("the modified system printer preset (\"* Snapmaker U1\")") { manual_head_changes(false); }
    SECTION("a user printer preset \"Snapmaker U1\" made from it") { manual_head_changes(true); }
}

TEST_CASE("Repro: a printer sync sets sizes and flow types, and every slot takes the version of its tool head", "[NozzleFilament][Repro][nozzle_filament_repro_sync]")
{
    SECTION("the modified system printer preset (\"* Snapmaker U1\")") { printer_sync(false); }
    SECTION("a user printer preset \"Snapmaker U1\" made from it") { printer_sync(true); }
}

// Snapmaker Orca: adopt_extruder_values_from_size_preset() on a nozzle size change. The U1 sizes
// differ in retraction length (0.4 / 1.5 / 1.4 / 1.5), minimum travel (1 / 1 / 3 / 1), wipe distance
// (2 / 2 / 1 / 2) and nozzle type (0.6: stainless steel); the 0.4 preset has two columns per head.
TEST_CASE("A tool head adopts the per-extruder machine values of its size preset", "[NozzleFilament][SizeChange]")
{
    auto          bundle = load_snapmaker_bundle();
    const Preset *p02    = bundle->printers.find_preset(U1_02, false);
    const Preset *p04    = bundle->printers.find_preset(U1_04, false);
    const Preset *p06    = bundle->printers.find_preset(U1_06, false);
    REQUIRE(p02 != nullptr);
    REQUIRE(p04 != nullptr);
    REQUIRE(p06 != nullptr);
    DynamicPrintConfig config = p04->config;
    const auto values_of = [&config](const char *key) { return static_cast<const ConfigOptionVectorBase *>(config.option(key))->vserialize(); };

    SECTION("0.4 to 0.2 on head 1: the retraction length of both variant columns of the head")
    {
        const std::vector<std::string> changed = adopt_extruder_values_from_size_preset(config, p02->config, &p04->config, 0, nozzle_size_extruder_options());
        CHECK(changed == std::vector<std::string>{"retraction_length"});
        CHECK(extruder_option_columns(config, "retraction_length", 0) == std::vector<size_t>{0, 1});
        const std::vector<std::string> retraction = values_of("retraction_length");
        REQUIRE(retraction.size() == 8);
        CHECK(retraction[0] == "0.4");
        CHECK(retraction[1] == "0.4");
        for (size_t column = 2; column < 8; ++column)
            CHECK(retraction[column] == "1.5");
    }
    SECTION("0.4 to 0.6 on head 3: retraction length, minimum travel, wipe distance and nozzle type")
    {
        const std::vector<std::string> changed = adopt_extruder_values_from_size_preset(config, p06->config, &p04->config, 2, nozzle_size_extruder_options());
        CHECK(changed == std::vector<std::string>{"nozzle_type", "retraction_length", "retraction_minimum_travel", "wipe_distance"});
        const std::vector<std::string> type = values_of("nozzle_type");
        REQUIRE(type.size() == 8);
        CHECK(type[4] == "stainless_steel");
        CHECK(type[5] == "stainless_steel");
        CHECK(type[0] == "hardened_steel");
        CHECK(type[7] == "hardened_steel");
        CHECK(values_of("retraction_length")[4] == "1.4");
        CHECK(values_of("retraction_minimum_travel")[5] == "3");
        CHECK(values_of("wipe_distance")[4] == "1");
        CHECK(values_of("wipe_distance")[6] == "2");
    }
    SECTION("an edit of a value the two size presets agree on survives the size change")
    {
        auto *z_hop = config.option<ConfigOptionFloats>("z_hop");
        REQUIRE(z_hop != nullptr);
        z_hop->values[0] = 0.9;
        const std::vector<std::string> changed = adopt_extruder_values_from_size_preset(config, p02->config, &p04->config, 0, nozzle_size_extruder_options());
        CHECK(changed == std::vector<std::string>{"retraction_length"});
        CHECK(values_of("z_hop")[0] == "0.9");
    }
    SECTION("without the preset of the previous size every value the size preset differs in is adopted")
    {
        auto *z_hop = config.option<ConfigOptionFloats>("z_hop");
        REQUIRE(z_hop != nullptr);
        z_hop->values[0] = 0.9;
        const std::vector<std::string> changed = adopt_extruder_values_from_size_preset(config, p02->config, nullptr, 0, nozzle_size_extruder_options());
        CHECK(changed == std::vector<std::string>{"retraction_length", "z_hop"});
        CHECK(values_of("z_hop")[0] == "0.4");
        CHECK(values_of("z_hop")[1] == "0.4");
    }
    SECTION("a tool head beyond the printer's heads adopts nothing")
    {
        CHECK(adopt_extruder_values_from_size_preset(config, p02->config, &p04->config, 4, nozzle_size_extruder_options()).empty());
    }
}

TEST_CASE("A user filament saved from a slot is pinned to the machine preset of the slot's extruder", "[NozzleFilament][FilamentFlow]")
{
    auto bundle = load_snapmaker_bundle();
    const char *const PETG_04 = "Generic PETG";
    const char *const PETG_06 = "Generic PETG @U1 0.6 nozzle";
    // The 0.6 mm printer preset with a 0.4 mm nozzle on extruder 2.
    select_u1(*bundle, { 0.6, 0.4, 0.6, 0.6 }, { PETG_06, PETG_04, PETG_06, PETG_06 }, U1_06);
    CHECK(NozzleFilament::printer_to_pin(*bundle, 1) == U1_04);
    // A slot of the printer preset's size, and no slot at all: the printer preset.
    CHECK(NozzleFilament::printer_to_pin(*bundle, 0) == U1_06);
    CHECK(NozzleFilament::printer_to_pin(*bundle, -1) == U1_06);
}

TEST_CASE("A second user preset of a material keeps a user preset of another size where it is", "[NozzleFilament][FilamentFlow]")
{
    auto bundle = load_snapmaker_bundle();
    const char *const PETG_04 = "Generic PETG";
    const char *const PETG_06 = "Generic PETG @U1 0.6 nozzle";
    add_user_filament(*bundle, "My PETG @0.6", PETG_06);
    add_user_filament(*bundle, "My PETG", PETG_04);
    // Extruder 2 carries 0.4 mm while its slot holds the 0.6 mm user preset.
    select_u1(*bundle, { 0.6, 0.4, 0.4, 0.4 }, { "My PETG @0.6", "My PETG @0.6", PETG_04, PETG_04 });
    CHECK(NozzleFilament::user_children(*bundle, system_filament(*bundle, PETG_04), machine(*bundle, U1_04)).size() == 1);
    NozzleFilament::SlotTarget second = target(*bundle, 1);
    CHECK(second.reason == NozzleFilament::Reason::Switched);
    CHECK(second.to == "My PETG");

    // A user preset saved with High Flow values from a system preset is a second child.
    add_user_filament(*bundle, "Generic PETG - Copy", PETG_04);
    CHECK(NozzleFilament::user_children(*bundle, system_filament(*bundle, PETG_04), machine(*bundle, U1_04)).size() == 2);
    second = target(*bundle, 1);
    CHECK(second.reason == NozzleFilament::Reason::UserPresetKept);
    CHECK(second.to == "My PETG @0.6");
    CHECK(second.suggestion == PETG_04);
}

TEST_CASE("A user filament saved from a slot is pinned to the machine preset of the slot's extruder with the nozzle size rule off", "[NozzleFilament][FilamentFlow]")
{
    auto bundle = load_snapmaker_bundle();
    const char *const PETG_04 = "Generic PETG";
    const char *const PETG_06 = "Generic PETG @U1 0.6 nozzle";
    select_u1(*bundle, { 0.6, 0.4, 0.6, 0.6 }, { PETG_06, PETG_04, PETG_06, PETG_06 }, U1_06);
    bundle->nozzle_filament_enabled = false;
    CHECK(NozzleFilament::printer_to_pin(*bundle, 1) == U1_04);
    CHECK(NozzleFilament::printer_to_pin(*bundle, 0) == U1_06);
}

TEST_CASE("A filament saved under a new name takes over only the slots whose extruder it fits", "[NozzleFilament][FilamentFlow]")
{
    auto bundle = load_snapmaker_bundle();
    const char *const PETG_04 = "Generic PETG";
    const char *const PETG_06 = "Generic PETG @U1 0.6 nozzle";
    const char *const COPY    = "Generic PETG - Copy";
    // Slot 1 on a 0.6 mm extruder and slot 2 on a 0.4 mm one hold the same preset.
    select_u1(*bundle, { 0.6, 0.4, 0.6, 0.6 }, { PETG_04, PETG_04, PETG_06, PETG_06 }, U1_06);
    add_user_filament(*bundle, COPY, PETG_04);
    Preset *copy = bundle->filaments.find_preset(COPY, false, true);
    REQUIRE(copy != nullptr);

    SECTION("a copy pinned to the 0.4 mm machine preset takes over slot 2 only") {
        copy->config.option<ConfigOptionStrings>("compatible_printers", true)->values = { U1_04 };
        CHECK(NozzleFilament::slots_to_switch(*bundle, PETG_04, *copy) == std::vector<size_t>{ 1 });
    }
    SECTION("a copy restricting no printer takes over both slots") {
        copy->config.option<ConfigOptionStrings>("compatible_printers", true)->values.clear();
        copy->config.option<ConfigOptionString>("compatible_printers_condition", true)->value.clear();
        CHECK(NozzleFilament::slots_to_switch(*bundle, PETG_04, *copy) == std::vector<size_t>{ 0, 1 });
    }
    SECTION("slots holding another preset are left alone") {
        CHECK(NozzleFilament::slots_to_switch(*bundle, "no such preset", *copy).empty());
    }
}

TEST_CASE("High Flow values are offered for a filament a High Flow extruder may print", "[NozzleFilament][FilamentFlow]")
{
    auto bundle = load_snapmaker_bundle();
    const char *const PETG_04 = "Generic PETG";
    const char *const PETG_06 = "Generic PETG @U1 0.6 nozzle";
    // The 0.6 mm printer preset with a 0.4 mm High Flow nozzle on extruder 2.
    select_u1(*bundle, { 0.6, 0.4, 0.6, 0.6 }, { PETG_06, PETG_04, PETG_06, PETG_06 }, U1_06);
    auto &volume_types = bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values;
    volume_types = { int(nvtStandard), int(nvtHighFlow), int(nvtStandard), int(nvtStandard) };
    add_user_filament(*bundle, "My library PETG", PETG_04);
    Preset *library = bundle->filaments.find_preset("My library PETG", false, true);
    REQUIRE(library != nullptr);
    // A preset restricting no printer, as the filament library ships them.
    library->config.option<ConfigOptionStrings>("compatible_printers", true)->values.clear();
    library->config.option<ConfigOptionString>("compatible_printers_condition", true)->value.clear();

    CHECK(NozzleFilament::fits_high_flow_extruder(*bundle, system_filament(*bundle, PETG_04)));
    CHECK(NozzleFilament::fits_high_flow_extruder(*bundle, *library));
    // Pinned to 0.6 mm: no High Flow extruder prints it.
    CHECK_FALSE(NozzleFilament::fits_high_flow_extruder(*bundle, system_filament(*bundle, PETG_06)));

    SECTION("no extruder set to High Flow") {
        volume_types = { int(nvtStandard), int(nvtStandard), int(nvtStandard), int(nvtStandard) };
        CHECK_FALSE(NozzleFilament::fits_high_flow_extruder(*bundle, system_filament(*bundle, PETG_04)));
        CHECK_FALSE(NozzleFilament::fits_high_flow_extruder(*bundle, *library));
    }
}

TEST_CASE("A slot on a High Flow extruder marks a filament with High Flow values after its size", "[NozzleFilament][FilamentFlow]")
{
    const std::string name = "Generic PETG - Copy";
    CHECK(NozzleFilament::size_marked_label(name, NozzleFilament::high_flow_marker("0.4 mm", true)) == "0.4 mm \xC2\xB7 HF \xC2\xB7 Generic PETG - Copy");
    CHECK(NozzleFilament::size_marked_label(name, NozzleFilament::high_flow_marker("", true)) == "HF \xC2\xB7 Generic PETG - Copy");
    // Without High Flow values: the size marker alone, or the name alone.
    CHECK(NozzleFilament::size_marked_label(name, NozzleFilament::high_flow_marker("0.4 mm", false)) == "0.4 mm \xC2\xB7 Generic PETG - Copy");
    CHECK(NozzleFilament::size_marked_label(name, NozzleFilament::high_flow_marker("", false)) == name);
}
