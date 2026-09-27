#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/NozzleFilamentPresets.hpp"
#include "libslic3r/PerHeadProcess.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ProjectSchemaVersion.hpp"
#include "libslic3r/Semver.hpp"

#include "snapmaker_high_flow_fixture.hpp"
#include "test_utils.hpp"

using namespace Slic3r;

// Process speeds follow the nozzle size of the tool head that prints (libslic3r/PerHeadProcess.hpp):
// the key set, the composition of the per-head table, the source rule on the shipped Snapmaker
// vendor and the pass-through of PresetBundle::full_config_for_print.

namespace {

const std::string DD_STANDARD  = "Direct Drive Standard";
const std::string DD_HIGH_FLOW = "Direct Drive High Flow";

const char *const U1_02         = "Snapmaker U1 (0.2 nozzle)";
const char *const U1_04         = "Snapmaker U1 (0.4 nozzle)";
const char *const STD_020_04    = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)";
const char *const HQ_020_04     = "0.20mm High Quality @Snapmaker U1 (0.4 nozzle)";
const char *const STD_012_02    = "0.12mm Standard @Snapmaker U1 (0.2 nozzle)";
const char *const HQ_010_02     = "0.10mm High Quality @Snapmaker U1 (0.2 nozzle)";
const char *const STD_030_06    = "0.30mm Standard @Snapmaker U1 (0.6 nozzle)";
const char *const STD_018_06    = "0.18mm Standard @Snapmaker U1 (0.6 nozzle)";
const char *const PLA_04        = "Generic PLA";

// A four-head printer that declares Standard and High Flow for every head, with the given flow
// type per head; `stats` adds extruder_nozzle_stats, which makes the narrowing emit one slot per
// (head x flow type) instead of one per head.
DynamicPrintConfig four_head_printer(const std::vector<NozzleVolumeType> &flows, bool stats)
{
    DynamicPrintConfig config;
    config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = std::vector<double>(4, 0.4);
    config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = std::vector<int>(4, int(etDirectDrive));
    std::vector<int> flow_values;
    for (NozzleVolumeType flow : flows)
        flow_values.emplace_back(int(flow));
    config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = flow_values;
    config.option<ConfigOptionStrings>("extruder_variant_list", true)->values   = std::vector<std::string>(4, DD_STANDARD + "," + DD_HIGH_FLOW);
    if (stats)
        config.option<ConfigOptionStrings>("extruder_nozzle_stats", true)->values = std::vector<std::string>(4, "Standard#1|High Flow#1");
    return config;
}

// The selected preset's columns in a full config: the flow-only layout of 0.20mm Standard.
void add_flow_only_process(DynamicPrintConfig &config)
{
    config.option<ConfigOptionInts>("print_extruder_id", true)->values          = {1, 1};
    config.option<ConfigOptionStrings>("print_extruder_variant", true)->values  = {DD_STANDARD, DD_HIGH_FLOW};
    config.option<ConfigOptionFloatsNullable>("outer_wall_speed", true)->values = {200., 500.};
    config.option<ConfigOptionFloatsNullable>("default_acceleration", true)->values = {10000., 10000.};
    config.option<ConfigOptionFloatsNullable>("travel_speed", true)->values         = {500., 550.};
    config.option<ConfigOptionBoolsNullable>("enable_overhang_speed", true)->values        = {1, 1};
    config.option<ConfigOptionFloatsOrPercentsNullable>("small_perimeter_speed", true)->values = {FloatOrPercent{50., true}, FloatOrPercent{60., true}};
}

// A single-column process preset of another nozzle size with distinct values.
std::unique_ptr<Preset> single_column_source()
{
    auto preset = std::make_unique<Preset>(Preset::TYPE_PRINT, "0.12mm Standard @Test (0.2 nozzle)");
    DynamicPrintConfig &config = preset->config;
    config.option<ConfigOptionInts>("print_extruder_id", true)->values          = {1};
    config.option<ConfigOptionStrings>("print_extruder_variant", true)->values  = {DD_STANDARD};
    config.option<ConfigOptionFloatsNullable>("outer_wall_speed", true)->values = {60.};
    config.option<ConfigOptionFloatsNullable>("default_acceleration", true)->values = {4000.};
    config.option<ConfigOptionFloatsNullable>("travel_speed", true)->values         = {300.};
    config.option<ConfigOptionBoolsNullable>("enable_overhang_speed", true)->values        = {0};
    config.option<ConfigOptionFloatsOrPercentsNullable>("small_perimeter_speed", true)->values = {FloatOrPercent{25., false}};
    return preset;
}

std::vector<PerHeadProcess::Source> sources_with_head(size_t head, const Preset &preset, size_t heads = 4)
{
    std::vector<PerHeadProcess::Source> out(heads);
    for (size_t h = 0; h < heads; ++h)
        out[h].head = h;
    out[head].preset  = &preset;
    out[head].derived = true;
    out[head].reason  = PerHeadProcess::Reason::Derived;
    out[head].composed_keys.assign(PerHeadProcess::composed_keys().begin(), PerHeadProcess::composed_keys().end());
    return out;
}

std::vector<double> floats_of(const DynamicPrintConfig &config, const std::string &key)
{
    const auto *option = dynamic_cast<const ConfigOptionVectorBase *>(config.option(key));
    REQUIRE(option != nullptr);
    std::vector<double> out;
    for (const std::string &value : option->vserialize())
        out.emplace_back(std::atof(value.c_str()));
    return out;
}

std::unique_ptr<PresetBundle> load_snapmaker_bundle()
{
    auto bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(PROFILES_DIR, "Snapmaker", PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr,
                                          /*allow_cache=*/false);
    return bundle;
}

// The U1 0.4 preset with the given sizes and preferred layer heights on its tool heads, the
// process preset `process`, PLA on every slot, the process rule on.
void select_u1(PresetBundle &bundle, const std::vector<double> &sizes, const std::vector<double> &heights, const char *process = STD_020_04)
{
    REQUIRE(bundle.printers.select_preset_by_name(U1_04, true));
    REQUIRE(bundle.prints.select_preset_by_name(process, true));
    bundle.process_follows_nozzle = true;
    bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter", true)->values       = sizes;
    bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("extruder_layer_height", true)->values = heights;
    bundle.filament_presets = std::vector<std::string>(4, PLA_04);
    REQUIRE(bundle.filaments.select_preset_by_name(PLA_04, true));
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2, 3, 4};
    bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = std::vector<int>(4, int(nvtStandard));
}

const Preset &machine(const PresetBundle &bundle, const char *name)
{
    const Preset *preset = bundle.printers.find_preset(name, false);
    REQUIRE(preset != nullptr);
    return *preset;
}

std::vector<std::string> source_names(const std::vector<PerHeadProcess::Source> &sources)
{
    std::vector<std::string> out;
    for (const PerHeadProcess::Source &source : sources)
        out.emplace_back(source.derived ? source.preset->name : std::string());
    return out;
}

} // namespace

TEST_CASE("The composed key set is the 32 speed, acceleration and jerk keys of the variant keys", "[PerHeadProcess][phs_key_set]")
{
    const std::set<std::string> &composed = PerHeadProcess::composed_keys();
    CHECK(composed.size() == 32);
    CHECK(print_options_with_variant.size() == 44);
    for (const std::string &key : composed) {
        INFO(key);
        CHECK(print_options_with_variant.count(key) == 1);
    }
    // Uniform: the travel keys, the behaviour switches and the small perimeter threshold.
    for (const char *key : {"travel_speed", "travel_speed_z", "initial_layer_travel_speed", "travel_acceleration", "initial_layer_travel_acceleration",
                            "travel_jerk", "initial_layer_travel_jerk", "enable_overhang_speed", "slowdown_for_curled_perimeters", "small_perimeter_threshold"}) {
        INFO(key);
        CHECK(composed.count(key) == 0);
        CHECK(print_options_with_variant.count(key) == 1);
    }
    CHECK(composed.count("print_extruder_id") == 0);
    CHECK(composed.count("print_extruder_variant") == 0);
}

TEST_CASE("The quality class of a process preset is the text between the layer height and the printer", "[PerHeadProcess][phs_quality_class]")
{
    CHECK(PerHeadProcess::quality_class(STD_020_04) == "Standard");
    CHECK(PerHeadProcess::quality_class(HQ_010_02) == "High Quality");
    CHECK(PerHeadProcess::quality_class("0.40mm Strength @Snapmaker U1 (0.8 nozzle)") == "Strength");
    CHECK(PerHeadProcess::quality_class("0.10mm Color Mixing @Snapmaker U1 (0.4 nozzle)") == "Color Mixing");
    CHECK(PerHeadProcess::quality_class("My process").empty());
    CHECK(PerHeadProcess::quality_class("0.20mm Standard").empty());
}

TEST_CASE("The composer writes one column per slot of the narrowing and takes the composed keys of a derived head from its source", "[PerHeadProcess][phs_compose_layout]")
{
    const std::unique_ptr<Preset> source = single_column_source();

    SECTION("one slot per tool head: the flow type of each head") {
        DynamicPrintConfig full = four_head_printer({nvtStandard, nvtStandard, nvtHighFlow, nvtStandard}, /*stats=*/false);
        add_flow_only_process(full);
        std::vector<PerHeadProcess::Source> sources = sources_with_head(1, *source);

        REQUIRE(PerHeadProcess::compose(full, {}, sources));
        CHECK(full.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{1, 2, 3, 4});
        CHECK(full.option<ConfigOptionStrings>("print_extruder_variant")->values == std::vector<std::string>{DD_STANDARD, DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD});
        CHECK(full.option<ConfigOptionInts>(PerHeadProcess::source_column_key)->values == std::vector<int>{0, 0, 1, 0});
        // Composed keys: head 2 from the source, the other heads from the selected preset's column of their flow type.
        CHECK(floats_of(full, "outer_wall_speed") == std::vector<double>{200., 60., 500., 200.});
        CHECK(floats_of(full, "default_acceleration") == std::vector<double>{10000., 4000., 10000., 10000.});
        // A percent value is copied as the percent.
        CHECK(full.option<ConfigOptionFloatsOrPercentsNullable>("small_perimeter_speed")->values[1] == FloatOrPercent{25., false});
        CHECK(full.option<ConfigOptionFloatsOrPercentsNullable>("small_perimeter_speed")->values[2] == FloatOrPercent{60., true});
        // Uniform keys: the selected preset's column on every head, the source's value ignored.
        CHECK(floats_of(full, "travel_speed") == std::vector<double>{500., 500., 550., 500.});
        CHECK(full.option<ConfigOptionBoolsNullable>("enable_overhang_speed")->values == std::vector<unsigned char>{1, 1, 1, 1});
        CHECK(sources[1].fallback_variants.empty());

        // The narrowing of Print::apply reads every slot as an exact match.
        std::vector<std::vector<NozzleVolumeType>> volume_types;
        const int count = full.get_extruder_nozzle_volume_count(4, volume_types);
        REQUIRE(count == 4);
        const std::vector<int> variant_index = full.update_values_to_printer_extruders(full, 4, count, volume_types, print_options_with_variant,
                                                                                       "print_extruder_id", "print_extruder_variant");
        CHECK(variant_index == std::vector<int>{0, 1, 2, 3});
        CHECK(floats_of(full, "outer_wall_speed") == std::vector<double>{200., 60., 500., 200.});
        CHECK(full.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{1, 2, 3, 4});
        // The recorded source columns hand the overrides of the selected preset's column space to the right slot.
        std::vector<int> remapped = variant_index;
        for (int &index : remapped)
            index = full.option<ConfigOptionInts>(PerHeadProcess::source_column_key)->get_at(size_t(index));
        CHECK(remapped == std::vector<int>{0, 0, 1, 0});
    }

    SECTION("one slot per (tool head x flow type) when the printer declares nozzle stats") {
        DynamicPrintConfig full = four_head_printer({nvtStandard, nvtStandard, nvtHighFlow, nvtStandard}, /*stats=*/true);
        add_flow_only_process(full);
        std::vector<PerHeadProcess::Source> sources = sources_with_head(1, *source);

        REQUIRE(PerHeadProcess::compose(full, {}, sources));
        CHECK(full.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{1, 1, 2, 2, 3, 3, 4, 4});
        CHECK(full.option<ConfigOptionInts>(PerHeadProcess::source_column_key)->values == std::vector<int>{0, 1, 0, 1, 0, 1, 0, 1});
        // Head 2's Standard column from the source; its High Flow column from the selected preset,
        // since the source has no High Flow column: noted as a fallback.
        CHECK(floats_of(full, "outer_wall_speed") == std::vector<double>{200., 500., 60., 500., 200., 500., 200., 500.});
        CHECK(floats_of(full, "default_acceleration") == std::vector<double>{10000., 10000., 4000., 10000., 10000., 10000., 10000., 10000.});
        CHECK(sources[1].fallback_variants == std::vector<int>{int(nvtHighFlow)});

        std::vector<std::vector<NozzleVolumeType>> volume_types;
        const int count = full.get_extruder_nozzle_volume_count(4, volume_types);
        REQUIRE(count == 8);
        const std::vector<int> variant_index = full.update_values_to_printer_extruders(full, 4, count, volume_types, print_options_with_variant,
                                                                                       "print_extruder_id", "print_extruder_variant");
        CHECK(variant_index == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7});
        CHECK(floats_of(full, "outer_wall_speed") == std::vector<double>{200., 500., 60., 500., 200., 500., 200., 500.});
    }

    SECTION("a key the user edited keeps the selected preset's value on every head") {
        DynamicPrintConfig full = four_head_printer({nvtStandard, nvtStandard, nvtHighFlow, nvtStandard}, /*stats=*/false);
        add_flow_only_process(full);
        std::vector<PerHeadProcess::Source> sources = sources_with_head(1, *source);
        REQUIRE(PerHeadProcess::compose(full, {"outer_wall_speed"}, sources));
        CHECK(floats_of(full, "outer_wall_speed") == std::vector<double>{200., 200., 500., 200.});
        CHECK(floats_of(full, "default_acceleration") == std::vector<double>{10000., 4000., 10000., 10000.});
    }
}

TEST_CASE("Without a derived source the composer leaves the config untouched", "[PerHeadProcess][phs_compose_noop]")
{
    DynamicPrintConfig full = four_head_printer({nvtStandard, nvtStandard, nvtHighFlow, nvtStandard}, /*stats=*/false);
    add_flow_only_process(full);
    const std::string before = full.opt_serialize("outer_wall_speed") + "|" + full.opt_serialize("print_extruder_id") + "|" + full.opt_serialize("print_extruder_variant");

    std::vector<PerHeadProcess::Source> none(4);
    CHECK_FALSE(PerHeadProcess::compose(full, {}, none));
    // A derived head whose every composed key is edited changes nothing either.
    const std::unique_ptr<Preset> source = single_column_source();
    std::vector<PerHeadProcess::Source> all_kept = sources_with_head(1, *source);
    all_kept[1].kept_keys = std::move(all_kept[1].composed_keys);
    all_kept[1].composed_keys.clear();
    CHECK_FALSE(PerHeadProcess::compose(full, {}, all_kept));

    CHECK(full.opt_serialize("outer_wall_speed") + "|" + full.opt_serialize("print_extruder_id") + "|" + full.opt_serialize("print_extruder_variant") == before);
    CHECK(full.option(PerHeadProcess::source_column_key) == nullptr);
}

TEST_CASE("A composed table missing a slot's column falls back to its first column", "[PerHeadProcess][phs_miss_guard]")
{
    DynamicPrintConfig full = four_head_printer({nvtStandard, nvtStandard, nvtHighFlow, nvtStandard}, /*stats=*/false);
    add_flow_only_process(full);
    const std::unique_ptr<Preset>       source  = single_column_source();
    std::vector<PerHeadProcess::Source> sources = sources_with_head(1, *source);
    REQUIRE(PerHeadProcess::compose(full, {}, sources));
    // Drop the column of tool head 4.
    for (const char *key : {"print_extruder_id", "print_extruder_variant", "outer_wall_speed", "default_acceleration", "travel_speed", "enable_overhang_speed", "small_perimeter_speed"}) {
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(full.option(key));
        REQUIRE(option != nullptr);
        option->resize(3);
    }
    std::vector<std::vector<NozzleVolumeType>> volume_types;
    const int count = full.get_extruder_nozzle_volume_count(4, volume_types);
    const std::vector<int> variant_index = full.update_values_to_printer_extruders(full, 4, count, volume_types, print_options_with_variant,
                                                                                   "print_extruder_id", "print_extruder_variant");
    // The miss is logged (a warning, not asserted here) and takes column 0.
    CHECK(variant_index == std::vector<int>{0, 1, 2, 0});
    CHECK(floats_of(full, "outer_wall_speed") == std::vector<double>{200., 60., 500., 200.});
}

TEST_CASE("On the shipped U1 presets each off-size tool head follows the process preset of its size chosen by intent", "[PerHeadProcess][Profiles][phs_vendor_sources]")
{
    auto bundle = load_snapmaker_bundle();

    SECTION("preferred layer heights with an exact preset of the same class") {
        select_u1(*bundle, {0.4, 0.2, 0.6, 0.4}, {0., 0.12, 0.30, 0.});
        const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
        REQUIRE(sources.size() == 4);
        CHECK(source_names(sources) == std::vector<std::string>{"", STD_012_02, STD_030_06, ""});
        CHECK(sources[0].reason == PerHeadProcess::Reason::HomeSize);
        CHECK(sources[1].reason == PerHeadProcess::Reason::Derived);
        CHECK(sources[2].reason == PerHeadProcess::Reason::Derived);
        CHECK(sources[3].reason == PerHeadProcess::Reason::HomeSize);
        CHECK(sources[1].composed_keys.size() == 32);
        CHECK(sources[1].kept_keys.empty());

        // One slot per tool head on the U1 (no nozzle stats): the off-size heads carry the 0.2 / 0.6
        // Standard speeds, the 0.4 heads the selected preset's Standard column.
        const DynamicPrintConfig composed = bundle->full_config_for_print(false);
        CHECK(composed.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{1, 2, 3, 4});
        CHECK(composed.option<ConfigOptionInts>(PerHeadProcess::source_column_key)->values == std::vector<int>{0, 0, 0, 0});
        CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{200., 120., 120., 200.});
        CHECK(floats_of(composed, "default_acceleration") == std::vector<double>{10000., 10000., 10000., 10000.});
        // The travel keys stay the selected preset's.
        CHECK(floats_of(composed, "travel_speed") == std::vector<double>{500., 500., 500., 500.});

        // What the project stores is the selected preset's layout, no composition, no transient key.
        const DynamicPrintConfig stored = bundle->full_config_secure();
        CHECK(stored.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{1, 1});
        CHECK(stored.option(PerHeadProcess::source_column_key) == nullptr);
        CHECK(floats_of(stored, "outer_wall_speed") == std::vector<double>{200., 500.});

        // With the preference off the print config is the plain full config.
        bundle->process_follows_nozzle = false;
        CHECK(PerHeadProcess::head_sources(*bundle)[1].reason == PerHeadProcess::Reason::Off);
        const DynamicPrintConfig plain = bundle->full_config_for_print(false);
        CHECK(plain.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{1, 1});
        CHECK(plain.option(PerHeadProcess::source_column_key) == nullptr);
        CHECK(plain.equals(bundle->full_config(false)));
    }

    SECTION("a preferred layer height with one exact preset of another class, and no preferred height") {
        select_u1(*bundle, {0.4, 0.2, 0.6, 0.4}, {0., 0.10, 0., 0.});
        const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
        REQUIRE(sources.size() == 4);
        // 0.10 has one preset, High Quality; 0.6 without a preference targets 0.20: the nearest Standard is 0.18.
        CHECK(source_names(sources) == std::vector<std::string>{"", HQ_010_02, STD_018_06, ""});
        const DynamicPrintConfig composed = bundle->full_config_for_print(false);
        CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{200., 60., 120., 200.});
        CHECK(floats_of(composed, "default_acceleration") == std::vector<double>{10000., 4000., 10000., 10000.});
        CHECK(floats_of(composed, "outer_wall_acceleration") == std::vector<double>{5000., 2000., 5000., 5000.});
    }

    SECTION("a quality class the size lacks takes the size's Standard preset") {
        select_u1(*bundle, {0.4, 0.4, 0.6, 0.4}, {0., 0., 0.30, 0.}, HQ_020_04);
        const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
        REQUIRE(sources.size() == 4);
        CHECK(source_names(sources) == std::vector<std::string>{"", "", STD_030_06, ""});
        const DynamicPrintConfig composed = bundle->full_config_for_print(false);
        CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{60., 60., 120., 60.});
        CHECK(floats_of(composed, "default_acceleration") == std::vector<double>{4000., 4000., 10000., 4000.});
    }

    SECTION("a value the user changed in the selected preset is kept on every tool head") {
        select_u1(*bundle, {0.4, 0.2, 0.6, 0.4}, {0., 0.10, 0.30, 0.});
        bundle->prints.get_edited_preset().config.option<ConfigOptionFloatsNullable>("default_acceleration")->values = {3000., 3000.};
        const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
        REQUIRE(sources.size() == 4);
        CHECK(sources[1].kept_keys == std::vector<std::string>{"default_acceleration"});
        CHECK(sources[1].composed_keys.size() == 31);
        const DynamicPrintConfig composed = bundle->full_config_for_print(false);
        CHECK(floats_of(composed, "default_acceleration") == std::vector<double>{3000., 3000., 3000., 3000.});
        CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{200., 60., 120., 200.});
    }

    SECTION("a High Flow tool head of another size takes the selected preset's High Flow column when its source has none") {
        select_u1(*bundle, {0.4, 0.4, 0.6, 0.4}, {0., 0., 0.30, 0.});
        Test::u1_0_6_declares_high_flow(*bundle);
        bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {int(nvtStandard), int(nvtStandard), int(nvtHighFlow), int(nvtStandard)};
        std::vector<PerHeadProcess::Source> sources;
        const DynamicPrintConfig composed = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
        REQUIRE(sources.size() == 4);
        CHECK(sources[2].derived);
        CHECK(sources[2].fallback_variants == std::vector<int>{int(nvtHighFlow)});
        CHECK(composed.option<ConfigOptionStrings>("print_extruder_variant")->values == std::vector<std::string>{DD_STANDARD, DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD});
        // 500 is the selected preset's High Flow column, not the 0.6 preset's Standard 120.
        CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{200., 200., 500., 200.});
    }

    SECTION("the source rule alone: explicit names, an installed default and a size without presets") {
        select_u1(*bundle, {0.4, 0.2, 0.6, 0.4}, {0., 0.12, 0.30, 0.});
        const Preset &selected = bundle->prints.get_selected_preset();
        PerHeadProcess::Reason reason = PerHeadProcess::Reason::HomeSize;

        Preset *hq = bundle->prints.find_preset(HQ_010_02, false, true);
        REQUIRE(hq != nullptr);
        hq->is_visible = true;
        const Preset *chosen = PerHeadProcess::source_for_head(*bundle, machine(*bundle, U1_02), 0.12, selected, HQ_010_02, reason);
        REQUIRE(chosen != nullptr);
        CHECK(chosen->name == HQ_010_02);
        CHECK(reason == PerHeadProcess::Reason::Explicit);

        chosen = PerHeadProcess::source_for_head(*bundle, machine(*bundle, U1_02), 0.12, selected, "0.12mm Standard @Snapmaker U1 (0.2 nozzle) renamed", reason);
        REQUIRE(chosen != nullptr);
        CHECK(chosen->name == STD_012_02);
        CHECK(reason == PerHeadProcess::Reason::NotInstalled);

        // A machine preset no process preset lists: the default named by the machine, when installed, else nothing.
        Preset odd(Preset::TYPE_PRINTER, "Snapmaker U1 (0.5 nozzle)");
        odd.config.option<ConfigOptionString>("printer_model", true)->value          = "Snapmaker U1";
        odd.config.option<ConfigOptionString>("default_print_profile", true)->value  = STD_012_02;
        chosen = PerHeadProcess::source_for_head(*bundle, odd, 0.12, selected, std::string(), reason);
        REQUIRE(chosen != nullptr);
        CHECK(chosen->name == STD_012_02);
        CHECK(reason == PerHeadProcess::Reason::Derived);
        odd.config.option<ConfigOptionString>("default_print_profile", true)->value = "0.12mm Standard @Snapmaker U1 (0.5 nozzle)";
        chosen = PerHeadProcess::source_for_head(*bundle, odd, 0.12, selected, std::string(), reason);
        CHECK(chosen == nullptr);
        CHECK(reason == PerHeadProcess::Reason::NoProcessPreset);
    }

    SECTION("a tool head of a size without a machine preset and a detached process preset are not derived") {
        select_u1(*bundle, {0.4, 0.5, 0.6, 0.4}, {0., 0., 0.30, 0.});
        std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
        REQUIRE(sources.size() == 4);
        CHECK(sources[1].reason == PerHeadProcess::Reason::NoMachinePreset);
        CHECK_FALSE(sources[1].derived);
        CHECK(sources[2].derived);

        // A process preset without a system parent: every key counts as edited.
        DynamicPrintConfig detached = bundle->prints.get_selected_preset().config;
        detached.option<ConfigOptionString>("inherits", true)->value = "";
        Preset &user = bundle->prints.load_preset(std::string(), "My detached process", std::move(detached), /*select=*/true);
        user.is_visible = true;
        sources = PerHeadProcess::head_sources(*bundle);
        REQUIRE(sources.size() == 4);
        CHECK(sources[2].reason == PerHeadProcess::Reason::NoParent);
        CHECK_FALSE(sources[2].derived);
        const DynamicPrintConfig composed = bundle->full_config_for_print(false);
        CHECK(composed.option(PerHeadProcess::source_column_key) == nullptr);
    }
}

// The project records what each tool head printed with; a load compares and reports, never switches.
TEST_CASE("A U1 whose tool heads all carry the nozzle size of the printer preset leaves the record empty", "[PerHeadProcess][Profiles][phs_record_uniform]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.});
    auto *record = bundle->project_config.option<ConfigOptionStrings>(PerHeadProcess::record_key, true);
    REQUIRE(record != nullptr);
    REQUIRE(record->values.empty());

    // No head is derived: the record stays [], not one empty entry per head, so a project saved
    // without the key and this plate carry the same value and the apply after a load changes nothing.
    std::vector<PerHeadProcess::Source> sources = PerHeadProcess::record_sources(*bundle);
    REQUIRE(sources.size() == 4);
    for (const PerHeadProcess::Source &source : sources) {
        CHECK_FALSE(source.derived);
        CHECK(source.reason == PerHeadProcess::Reason::HomeSize);
    }
    CHECK(record->values.empty());
    CHECK(bundle->full_config_for_print(false).option<ConfigOptionStrings>(PerHeadProcess::record_key)->values.empty());
    CHECK(PerHeadProcess::load_report(*bundle).empty());

    SECTION("a record of one empty entry per head, as an earlier build wrote it, is reduced to []") {
        record->values = std::vector<std::string>(4, std::string());
        PerHeadProcess::record_sources(*bundle);
        CHECK(record->values.empty());
    }

    SECTION("the record is written only when it changes") {
        record->values = {"", STD_012_02, "", ""};
        bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter", true)->values       = {0.4, 0.2, 0.4, 0.4};
        bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("extruder_layer_height", true)->values = {0., 0.12, 0., 0.};
        // The rule gives what the record holds: the values are left as they are.
        sources = PerHeadProcess::record_sources(*bundle);
        REQUIRE(sources.size() == 4);
        CHECK(sources[1].derived);
        CHECK(record->values == std::vector<std::string>{"", STD_012_02, "", ""});
        // Back to uniform heads: the record is emptied, not filled with empty entries.
        bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = {0.4, 0.4, 0.4, 0.4};
        PerHeadProcess::record_sources(*bundle);
        CHECK(record->values.empty());
    }
}

TEST_CASE("The project records the process preset of every tool head and a load reports a difference", "[PerHeadProcess][Profiles][phs_project_record]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.2, 0.6, 0.4}, {0., 0.12, 0.30, 0.});
    bundle->project_config.option<ConfigOptionStrings>("filament_colour", true)->values = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};

    const std::vector<std::string> record = {"", STD_012_02, STD_030_06, ""};
    const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::record_sources(*bundle);
    REQUIRE(sources.size() == 4);
    CHECK(bundle->project_config.option<ConfigOptionStrings>(PerHeadProcess::record_key)->values == record);
    // The record is a record: the sources are the automatic rule's, the same before and after.
    CHECK(source_names(PerHeadProcess::head_sources(*bundle)) == record);
    CHECK(PerHeadProcess::head_sources(*bundle)[1].reason == PerHeadProcess::Reason::Derived);
    CHECK(PerHeadProcess::load_report(*bundle).empty());

    SECTION("the record reaches the project config, the print config and the stored project") {
        const DynamicPrintConfig stored = bundle->full_config_secure();
        CHECK(stored.option<ConfigOptionStrings>(PerHeadProcess::record_key)->values == record);
        CHECK(stored.option(PerHeadProcess::source_column_key) == nullptr);
        CHECK(project_schema_version_for(stored) == 2);
        const DynamicPrintConfig composed = bundle->full_config_for_print(false);
        CHECK(composed.option<ConfigOptionStrings>(PerHeadProcess::record_key)->values == record);

        Model model;
        REQUIRE(load_stl((std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl").c_str(), &model));
        model.add_default_instances();
        ScopedTemporaryDir backup_dir("orca_per_head_process");
        model.set_backup_path(backup_dir.string());
        DynamicPrintConfig project = stored;
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
        ScopedTemporaryDir        dst_backup_dir("orca_per_head_process_dst");
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
        CHECK(dst_config.option<ConfigOptionStrings>(PerHeadProcess::record_key)->values == record);
        CHECK(ProjectSchemaRegistry::version_from(dst_config) == 2);

        // The load imports the record into the project config and switches nothing.
        auto          reloaded = load_snapmaker_bundle();
        PresetBundle &second   = *reloaded;
        second.process_follows_nozzle = true;
        Preset::normalize(dst_config);
        second.load_config_model("per_head_process.3mf", std::move(dst_config), file_version);
        CHECK(second.prints.get_edited_preset().name == STD_020_04);
        CHECK(second.project_config.option<ConfigOptionStrings>(PerHeadProcess::record_key)->values == record);
        CHECK(PerHeadProcess::load_report(second).empty());
        // With the preference off the report names both derived heads.
        second.process_follows_nozzle = false;
        const std::vector<PerHeadProcess::LoadReportEntry> off = PerHeadProcess::load_report(second);
        REQUIRE(off.size() == 2);
        CHECK(off[0].head == 1);
        CHECK(off[0].recorded == STD_012_02);
        CHECK(off[0].current.empty());
        CHECK(off[0].reason == PerHeadProcess::Reason::Off);
        CHECK(off[0].recorded_installed);
        CHECK(off[1].head == 2);

        release_PlateData_list(dst_plates);
        delete plate;
    }

    SECTION("a changed preferred layer height is reported with the preset the head prints with now") {
        bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("extruder_layer_height", true)->values = {0., 0.10, 0.30, 0.};
        const std::vector<PerHeadProcess::LoadReportEntry> report = PerHeadProcess::load_report(*bundle);
        REQUIRE(report.size() == 1);
        CHECK(report[0].head == 1);
        CHECK(report[0].recorded == STD_012_02);
        CHECK(report[0].current == HQ_010_02);
        CHECK(report[0].reason == PerHeadProcess::Reason::Derived);
        CHECK_THAT(report[0].nozzle_size, Catch::Matchers::WithinAbs(0.2, 1e-9));
        // The next record follows the rule.
        PerHeadProcess::record_sources(*bundle);
        CHECK(PerHeadProcess::load_report(*bundle).empty());
    }

    SECTION("a changed nozzle size and a preset that is no longer installed are reported") {
        bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = {0.4, 0.4, 0.6, 0.4};
        std::vector<PerHeadProcess::LoadReportEntry> report = PerHeadProcess::load_report(*bundle);
        REQUIRE(report.size() == 1);
        CHECK(report[0].head == 1);
        CHECK(report[0].current.empty());
        CHECK(report[0].reason == PerHeadProcess::Reason::HomeSize);

        bundle->project_config.option<ConfigOptionStrings>(PerHeadProcess::record_key)->values = {"", "", "0.30mm Standard @Snapmaker U1 (0.6 nozzle) renamed", ""};
        report = PerHeadProcess::load_report(*bundle);
        REQUIRE(report.size() == 1);
        CHECK(report[0].head == 2);
        CHECK(report[0].current == STD_030_06);
        CHECK_FALSE(report[0].recorded_installed);
    }
}
