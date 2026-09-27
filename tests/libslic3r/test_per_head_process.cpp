#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <boost/algorithm/string/join.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/NozzleFilamentPresets.hpp"
#include "libslic3r/PerHeadProcess.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/calib.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ProjectSchemaVersion.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Utils.hpp"

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
const char *const STD_024_08    = "0.24mm Standard @Snapmaker U1 (0.8 nozzle)";
const char *const PLA_04        = "Generic PLA";
const char *const J1_04         = "Snapmaker J1 (0.4 nozzle)";
const char *const J1_PLA        = "PolyLite PLA @J1";

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

// The application's loader builds the rename map of every collection after the vendor files are read
// (PresetBundle::update_system_maps, private); the vendor loader alone leaves it empty.
struct RenameMapAccess : PresetCollection
{
    using PresetCollection::update_map_system_profile_renamed;
};
void refresh_rename_map(PresetCollection &presets) { (presets.*(&RenameMapAccess::update_map_system_profile_renamed))(); }

std::unique_ptr<PresetBundle> load_snapmaker_bundle()
{
    auto bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(PROFILES_DIR, "Snapmaker", PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr,
                                          /*allow_cache=*/false);
    refresh_rename_map(bundle->prints);
    return bundle;
}

// The U1 preset `printer` (the 0.4 variant unless given) with the given sizes and preferred layer
// heights on its tool heads, the process preset `process`, PLA on every slot, the process rule on.
void select_u1(PresetBundle &bundle, const std::vector<double> &sizes, const std::vector<double> &heights, const char *process = STD_020_04,
               const char *printer = U1_04)
{
    REQUIRE(bundle.printers.select_preset_by_name(printer, true));
    REQUIRE(bundle.prints.select_preset_by_name(process, true));
    bundle.process_follows_nozzle = true;
    bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter", true)->values       = sizes;
    bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("extruder_layer_height", true)->values = heights;
    bundle.filament_presets = std::vector<std::string>(4, PLA_04);
    REQUIRE(bundle.filaments.select_preset_by_name(PLA_04, true));
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2, 3, 4};
    bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = std::vector<int>(4, int(nvtStandard));
}

// The J1 0.4 preset (two tool heads of its size) with the given sizes and preferred layer heights,
// the process preset `process`, one PLA on both slots, the process rule on.
void select_j1(PresetBundle &bundle, const std::vector<double> &sizes, const std::vector<double> &heights, const char *process)
{
    REQUIRE(bundle.printers.select_preset_by_name(J1_04, true));
    REQUIRE(bundle.prints.select_preset_by_name(process, true));
    bundle.process_follows_nozzle = true;
    bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter", true)->values       = sizes;
    bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("extruder_layer_height", true)->values = heights;
    bundle.filament_presets = std::vector<std::string>(sizes.size(), J1_PLA);
    REQUIRE(bundle.filaments.select_preset_by_name(J1_PLA, true));
    std::vector<int> map;
    for (size_t slot = 0; slot < sizes.size(); ++slot)
        map.emplace_back(int(slot) + 1);
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = map;
    bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = std::vector<int>(sizes.size(), int(nvtStandard));
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

TEST_CASE("The composed key set is the 32 speed, acceleration and jerk keys of the variant keys", "[PerHeadProcess][PerHeadWidth][phs_key_set]")
{
    const std::set<std::string> &composed = PerHeadProcess::composed_keys();
    // 32 speeds, accelerations and jerk keys plus the nine line widths.
    CHECK(composed.size() == 41);
    // 51 value keys (42 speeds, accelerations, jerk and switches plus the nine line widths of the
    // Quality page), the ids, the variants and the marker of the values set per tool head.
    CHECK(print_options_with_variant.size() == 54);
    CHECK(print_options_with_variant.count(PerHeadProcess::override_key) == 1);
    CHECK(composed.count(PerHeadProcess::override_key) == 0);
    // 48 keys can be set per tool head: the value keys without the three uniform switches.
    const std::set<std::string> &editable = PerHeadProcess::head_editable_keys();
    CHECK(editable.size() == 48);
    // The nine line widths: flow-independent, editable per tool head and composed from the width
    // source; the Locked Zag widths stay scalar and outside every set.
    const std::set<std::string> &widths = PerHeadProcess::flow_independent_keys();
    CHECK(widths == std::set<std::string>{"line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width", "top_surface_line_width",
                                          "sparse_infill_line_width", "internal_solid_infill_line_width", "support_line_width", "bridge_line_width"});
    for (const std::string &key : widths) {
        INFO(key);
        CHECK(print_options_with_variant.count(key) == 1);
        CHECK(editable.count(key) == 1);
        CHECK(composed.count(key) == 1);
        const ConfigOptionDef *def = print_config_def.get(key);
        REQUIRE(def != nullptr);
        CHECK(def->type == coFloatsOrPercents);
        CHECK(def->nullable);
        CHECK(def->scalar_when_uniform);
    }
    for (const char *scalar : {"skin_infill_line_width", "skeleton_infill_line_width"}) {
        INFO(scalar);
        CHECK(print_options_with_variant.count(scalar) == 0);
        CHECK(editable.count(scalar) == 0);
        CHECK(widths.count(scalar) == 0);
        CHECK(print_config_def.get(scalar)->type == coFloatOrPercent);
    }
    for (const char *uniform : {"enable_overhang_speed", "slowdown_for_curled_perimeters", "small_perimeter_threshold"}) {
        INFO(uniform);
        CHECK(editable.count(uniform) == 0);
    }
    CHECK(editable.count(PerHeadProcess::override_key) == 0);
    CHECK(editable.count("print_extruder_id") == 0);
    CHECK(editable.count("travel_speed") == 1);
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
    // The J1 names carry no "mm"; a renamed_from spelling of the U1 neither.
    CHECK(PerHeadProcess::quality_class("0.28 Extra Draft @Snapmaker J1 (0.4 nozzle)") == "Extra Draft");
    CHECK(PerHeadProcess::quality_class("0.16 Optimal @Snapmaker J1 (0.4 nozzle)") == "Optimal");
    CHECK(PerHeadProcess::quality_class("0.12 Standard @Snapmaker U1 (0.2 nozzle)") == "Standard");
    CHECK(PerHeadProcess::quality_class("fdm_process_U1").empty());
    CHECK(PerHeadProcess::quality_class("0.20mm @Snapmaker U1 (0.4 nozzle)").empty());
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
        CHECK(sources[1].composed_keys.size() == 41);
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

    SECTION("a preferred layer height that only a preset of another class has, and no preferred height") {
        select_u1(*bundle, {0.4, 0.2, 0.6, 0.4}, {0., 0.10, 0., 0.});
        const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
        REQUIRE(sources.size() == 4);
        // The plate is Standard: the 0.2 mm head prefers 0.10, which only 0.10mm High Quality has, and
        // takes the Standard preset nearest to it (0.12) all the same; the class beats the height.
        // 0.6 without a preference targets 0.20: the nearest Standard is 0.18.
        CHECK(source_names(sources) == std::vector<std::string>{"", STD_012_02, STD_018_06, ""});
        CHECK(sources[1].step == PerHeadProcess::Step::SameQuality);
        CHECK(sources[2].step == PerHeadProcess::Step::SameQuality);
        const Preset *std_012 = bundle->prints.find_preset(STD_012_02, false);
        REQUIRE(std_012 != nullptr);
        const double accel_012 = floats_of(std_012->config, "outer_wall_acceleration").front();
        const DynamicPrintConfig composed = bundle->full_config_for_print(false);
        CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{200., 120., 120., 200.});
        CHECK(floats_of(composed, "default_acceleration") == std::vector<double>{10000., 10000., 10000., 10000.});
        CHECK(floats_of(composed, "outer_wall_acceleration") == std::vector<double>{5000., accel_012, 5000., 5000.});
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
        CHECK(sources[1].composed_keys.size() == 40);
        const DynamicPrintConfig composed = bundle->full_config_for_print(false);
        CHECK(floats_of(composed, "default_acceleration") == std::vector<double>{3000., 3000., 3000., 3000.});
        CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{200., 120., 120., 200.});
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

// The quality rule over both shipped Snapmaker printers. A cell names the preset the tool head of that size prints with under the
// selected preset of the row; the home size of the row is not derived.
namespace {

std::string u1_name(const char *height_and_class, double size)
{
    return std::string(height_and_class) + " @Snapmaker U1 (" + float_to_string_decimal_point(size, 1) + " nozzle)";
}

std::string j1_name(const char *height_and_class, double size)
{
    return std::string(height_and_class) + " @Snapmaker J1 (" + float_to_string_decimal_point(size, 1) + " nozzle)";
}

} // namespace

TEST_CASE("On the shipped U1 presets every tool head of another size prints with the nearest layer height of the plate's quality class", "[PerHeadProcess][Profiles][phs_rule_b]")
{
    auto bundle = load_snapmaker_bundle();
    const std::vector<double> sizes = {0.2, 0.4, 0.6, 0.8};
    struct Row
    {
        const char *selected;
        double      home;
        const char *cells[4]; // 0.2, 0.4, 0.6, 0.8; nullptr for the home size
    };
    const std::vector<Row> rows = {
        {"0.08mm High Quality", 0.2, {nullptr, "0.20mm High Quality", "0.18mm Standard", "0.24mm Standard"}},
        {"0.10mm High Quality", 0.2, {nullptr, "0.20mm High Quality", "0.18mm Standard", "0.24mm Standard"}},
        {"0.12mm Standard",     0.2, {nullptr, "0.12mm Standard",     "0.18mm Standard", "0.24mm Standard"}},
        {"0.08mm Standard",     0.4, {"0.12mm Standard",     nullptr, "0.18mm Standard", "0.24mm Standard"}},
        {"0.10mm Color Mixing", 0.4, {"0.10mm High Quality", nullptr, "0.18mm Standard", "0.24mm Standard"}},
        {"0.12mm Standard",     0.4, {"0.12mm Standard",     nullptr, "0.18mm Standard", "0.24mm Standard"}},
        {"0.16mm Standard",     0.4, {"0.12mm Standard",     nullptr, "0.18mm Standard", "0.24mm Standard"}},
        {"0.20mm High Quality", 0.4, {"0.10mm High Quality", nullptr, "0.18mm Standard", "0.24mm Standard"}},
        {"0.20mm Standard",     0.4, {"0.12mm Standard",     nullptr, "0.18mm Standard", "0.24mm Standard"}},
        {"0.24mm Standard",     0.4, {"0.12mm Standard",     nullptr, "0.24mm Standard", "0.24mm Standard"}},
        {"0.28mm Standard",     0.4, {"0.12mm Standard",     nullptr, "0.30mm Standard", "0.24mm Standard"}},
        {"0.18mm Standard",     0.6, {"0.12mm Standard", "0.16mm Standard", nullptr, "0.24mm Standard"}},
        {"0.24mm Standard",     0.6, {"0.12mm Standard", "0.24mm Standard", nullptr, "0.24mm Standard"}},
        {"0.30mm Standard",     0.6, {"0.12mm Standard", "0.28mm Standard", nullptr, "0.32mm Standard"}},
        {"0.24mm Standard",     0.8, {"0.12mm Standard", "0.24mm Standard", "0.24mm Standard", nullptr}},
        {"0.32mm Standard",     0.8, {"0.12mm Standard", "0.28mm Standard", "0.30mm Standard", nullptr}},
        {"0.40mm Standard",     0.8, {"0.12mm Standard", "0.28mm Standard", "0.30mm Standard", nullptr}},
        {"0.40mm Strength",     0.8, {"0.12mm Standard", "0.28mm Standard", "0.30mm Standard", nullptr}},
    };
    for (const Row &row : rows) {
        const std::string selected = u1_name(row.selected, row.home);
        const std::string printer  = "Snapmaker U1 (" + float_to_string_decimal_point(row.home, 1) + " nozzle)";
        DYNAMIC_SECTION(selected) {
            select_u1(*bundle, sizes, {0., 0., 0., 0.}, selected.c_str(), printer.c_str());
            const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
            REQUIRE(sources.size() == 4);
            const std::string quality = PerHeadProcess::plate_quality_class(*bundle);
            for (size_t head = 0; head < 4; ++head) {
                INFO("tool head " << head + 1 << " (" << sizes[head] << " mm)");
                if (row.cells[head] == nullptr) {
                    CHECK_FALSE(sources[head].derived);
                    CHECK(sources[head].reason == PerHeadProcess::Reason::HomeSize);
                    CHECK(sources[head].step == PerHeadProcess::Step::SelectedPreset);
                    continue;
                }
                REQUIRE(sources[head].derived);
                REQUIRE(sources[head].preset != nullptr);
                CHECK(sources[head].preset->name == u1_name(row.cells[head], sizes[head]));
                CHECK(sources[head].reason == PerHeadProcess::Reason::Derived);
                // The step: the plate's class when the cell has it, else the ladder landed on Standard.
                const std::string cell_class = PerHeadProcess::quality_class(sources[head].preset->name);
                if (cell_class == quality) {
                    CHECK(sources[head].step == PerHeadProcess::Step::SameQuality);
                } else {
                    CHECK(sources[head].step == PerHeadProcess::Step::ClassLadder);
                    CHECK(sources[head].class_used == cell_class);
                }
            }
        }
    }

    SECTION("a user preset without a class in its name takes its parent's class") {
        select_u1(*bundle, sizes, {0., 0., 0., 0.}, HQ_020_04);
        DynamicPrintConfig config = bundle->prints.get_selected_preset().config;
        config.option<ConfigOptionString>("inherits", true)->value = HQ_020_04;
        Preset &user = bundle->prints.load_preset(std::string(), "My fast PLA", std::move(config), /*select=*/true);
        user.is_visible = true;
        CHECK(PerHeadProcess::plate_quality_class(*bundle) == "High Quality");
        const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
        REQUIRE(sources.size() == 4);
        CHECK(source_names(sources) == std::vector<std::string>{HQ_010_02, "", STD_018_06, STD_024_08});
        CHECK(sources[0].step == PerHeadProcess::Step::SameQuality);
        CHECK(sources[2].step == PerHeadProcess::Step::ClassLadder);
    }

    SECTION("the edited layer height does not move the source; a preferred height on one tool head leaves the others") {
        select_u1(*bundle, sizes, {0., 0., 0., 0.});
        // The layer height planner writes its grid into the edited preset: the rule reads the saved 0.20.
        bundle->prints.get_edited_preset().config.option<ConfigOptionFloat>("layer_height", true)->value = 0.28;
        std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
        REQUIRE(sources.size() == 4);
        CHECK(source_names(sources) == std::vector<std::string>{STD_012_02, "", STD_018_06, STD_024_08});
        bool preferred = true;
        CHECK_THAT(PerHeadProcess::target_layer_height(*bundle, 2, &preferred), Catch::Matchers::WithinAbs(0.20, 1e-9));
        CHECK_FALSE(preferred);
        // A preferred height on the 0.6 mm head moves that head alone, inside the class.
        bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("extruder_layer_height", true)->values = {0., 0., 0.30, 0.};
        sources = PerHeadProcess::head_sources(*bundle);
        CHECK(source_names(sources) == std::vector<std::string>{STD_012_02, "", STD_030_06, STD_024_08});
        CHECK_THAT(PerHeadProcess::target_layer_height(*bundle, 2, &preferred), Catch::Matchers::WithinAbs(0.30, 1e-9));
        CHECK(preferred);
    }

    SECTION("a classless plate matches the classless presets of a size by the nearest height, before Standard") {
        // Constructed: system presets whose names carry a height and no class, one for the 0.4 plate
        // and two for the 0.6 mm head (copies of shipped presets, compatible with the same machines).
        select_u1(*bundle, sizes, {0., 0., 0., 0.});
        auto add_system = [&bundle](const char *name, const char *copy_of, bool select) -> Preset & {
            const Preset *source = bundle->prints.find_preset(copy_of, false);
            REQUIRE(source != nullptr);
            DynamicPrintConfig config = source->config;
            Preset &preset = bundle->prints.load_preset(std::string(), name, std::move(config), select);
            preset.is_system  = true;
            preset.is_visible = true;
            return preset;
        };
        add_system("0.30mm @Snapmaker U1 (0.6 nozzle)", STD_030_06, false);
        add_system("0.18mm @Snapmaker U1 (0.6 nozzle)", STD_018_06, false);
        add_system("0.20mm @Snapmaker U1 (0.4 nozzle)", STD_020_04, true);
        CHECK(PerHeadProcess::plate_quality_class(*bundle).empty());
        const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
        REQUIRE(sources.size() == 4);
        CHECK(source_names(sources) == std::vector<std::string>{STD_012_02, "", "0.18mm @Snapmaker U1 (0.6 nozzle)", STD_024_08});
        CHECK(sources[2].step == PerHeadProcess::Step::SameQuality);
        CHECK(sources[0].step == PerHeadProcess::Step::ClassLadder);
        CHECK(sources[0].class_used == "Standard");
    }
}

TEST_CASE("On the shipped J1 presets a plate of one quality gives the other sizes that quality, or Standard when the size lacks it", "[PerHeadProcess][Profiles][phs_rule_b]")
{
    auto bundle = load_snapmaker_bundle();
    struct Row
    {
        const char *selected;   // a 0.4 preset
        const char *cells[3];   // the 0.2, 0.6 and 0.8 mm head
    };
    const std::vector<Row> rows = {
        {"0.08 Extra Fine",  {"0.06 Standard", "0.18 Standard", "0.24 Standard"}},
        {"0.12 Fine",        {"0.10 Standard", "0.18 Standard", "0.24 Standard"}},
        {"0.16 Optimal",     {"0.14 Standard", "0.18 Standard", "0.24 Standard"}},
        {"0.20 Standard",    {"0.14 Standard", "0.18 Standard", "0.24 Standard"}},
        {"0.20 Strength",    {"0.14 Standard", "0.30 Strength", "0.24 Standard"}},
        {"0.24 Draft",       {"0.14 Standard", "0.42 Draft",    "0.48 Draft"}},
        {"0.25 Benchy",      {"0.14 Standard", "0.24 Standard", "0.24 Standard"}},
        {"0.28 Extra Draft", {"0.14 Standard", "0.30 Standard", "0.24 Standard"}},
    };
    const std::vector<double> other_sizes = {0.2, 0.6, 0.8};
    for (const Row &row : rows) {
        const std::string selected = j1_name(row.selected, 0.4);
        DYNAMIC_SECTION(selected) {
            for (size_t i = 0; i < other_sizes.size(); ++i) {
                // A mixed J1: the second tool head changed to another size in the sidebar.
                select_j1(*bundle, {0.4, other_sizes[i]}, {0., 0.}, selected.c_str());
                const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
                REQUIRE(sources.size() == 2);
                INFO("tool head 2 (" << other_sizes[i] << " mm)");
                CHECK_FALSE(sources[0].derived);
                REQUIRE(sources[1].derived);
                REQUIRE(sources[1].preset != nullptr);
                CHECK(sources[1].preset->name == j1_name(row.cells[i], other_sizes[i]));
                const std::string quality = PerHeadProcess::plate_quality_class(*bundle);
                if (PerHeadProcess::quality_class(sources[1].preset->name) == quality) {
                    CHECK(sources[1].step == PerHeadProcess::Step::SameQuality);
                } else {
                    CHECK(sources[1].step == PerHeadProcess::Step::ClassLadder);
                    CHECK(sources[1].class_used == "Standard");
                }
            }
        }
    }
}

// The J1 two-size plate under a class the 0.6 mm size lacks, and under two it has.
TEST_CASE("A J1 plate of two sizes gives the 0.6 mm head the class of the plate when the size has it and Standard otherwise", "[PerHeadProcess][Profiles][phs_rule_b]")
{
    auto bundle = load_snapmaker_bundle();
    select_j1(*bundle, {0.4, 0.6}, {0., 0.}, "0.28 Extra Draft @Snapmaker J1 (0.4 nozzle)");
    std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
    REQUIRE(sources.size() == 2);
    REQUIRE(sources[1].derived);
    CHECK(sources[1].preset->name == "0.30 Standard @Snapmaker J1 (0.6 nozzle)");
    CHECK(sources[1].step == PerHeadProcess::Step::ClassLadder);
    CHECK(sources[1].class_used == "Standard");
    // The composed table carries that preset's outer wall speed on the second head.
    const DynamicPrintConfig composed = bundle->full_config_for_print(false);
    CHECK_THAT(floats_of(composed, "outer_wall_speed")[1], Catch::Matchers::WithinAbs(floats_of(sources[1].preset->config, "outer_wall_speed").front(), 1e-6));

    select_j1(*bundle, {0.4, 0.6}, {0., 0.}, "0.24 Draft @Snapmaker J1 (0.4 nozzle)");
    sources = PerHeadProcess::head_sources(*bundle);
    REQUIRE(sources.size() == 2);
    REQUIRE(sources[1].derived);
    CHECK(sources[1].preset->name == "0.42 Draft @Snapmaker J1 (0.6 nozzle)");
    CHECK(sources[1].step == PerHeadProcess::Step::SameQuality);

    select_j1(*bundle, {0.4, 0.6}, {0., 0.}, "0.20 Strength @Snapmaker J1 (0.4 nozzle)");
    sources = PerHeadProcess::head_sources(*bundle);
    REQUIRE(sources.size() == 2);
    REQUIRE(sources[1].derived);
    CHECK(sources[1].preset->name == "0.30 Strength @Snapmaker J1 (0.6 nozzle)");
    CHECK(sources[1].step == PerHeadProcess::Step::SameQuality);
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
        // The 0.6 mm head prefers 0.18 now: the Standard preset of that height, no longer the recorded 0.30.
        bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("extruder_layer_height", true)->values = {0., 0.12, 0.18, 0.};
        const std::vector<PerHeadProcess::LoadReportEntry> report = PerHeadProcess::load_report(*bundle);
        REQUIRE(report.size() == 1);
        CHECK(report[0].head == 2);
        CHECK(report[0].recorded == STD_030_06);
        CHECK(report[0].current == STD_018_06);
        CHECK(report[0].reason == PerHeadProcess::Reason::Derived);
        CHECK_THAT(report[0].nozzle_size, Catch::Matchers::WithinAbs(0.6, 1e-9));
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

// ---- Values set per tool head: the storage --------

namespace {

const char *const OVERRIDE = PerHeadProcess::override_key;

// The four-head printer as the U1 0.4 machine preset declares it: printer_extruder_id 1,1,..,4,4
// with Standard and High Flow per head.
DynamicPrintConfig u1_like_printer()
{
    DynamicPrintConfig printer = four_head_printer({nvtStandard, nvtStandard, nvtStandard, nvtStandard}, /*stats=*/false);
    std::vector<int>         ids;
    std::vector<std::string> variants;
    for (int head = 1; head <= 4; ++head)
        for (const std::string &variant : {DD_STANDARD, DD_HIGH_FLOW}) {
            ids.emplace_back(head);
            variants.emplace_back(variant);
        }
    printer.option<ConfigOptionInts>("printer_extruder_id", true)->values         = ids;
    printer.option<ConfigOptionStrings>("printer_extruder_variant", true)->values = variants;
    return printer;
}

// A generic three-head printer: one Direct Drive Standard column per head, no printer ids.
DynamicPrintConfig three_head_printer()
{
    DynamicPrintConfig printer;
    printer.option<ConfigOptionFloats>("nozzle_diameter", true)->values           = std::vector<double>(3, 0.4);
    printer.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values      = std::vector<int>(3, int(etDirectDrive));
    printer.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = std::vector<int>(3, int(nvtStandard));
    printer.option<ConfigOptionStrings>("extruder_variant_list", true)->values   = std::vector<std::string>(3, DD_STANDARD);
    return printer;
}

// The flow-only 0.20mm Standard child as a process config of its own (the keys of add_flow_only_process).
DynamicPrintConfig flow_only_process()
{
    DynamicPrintConfig config;
    add_flow_only_process(config);
    return config;
}

const std::vector<std::string> &probe_keys()
{
    static const std::vector<std::string> keys = {"outer_wall_speed", "default_acceleration", "travel_speed", "enable_overhang_speed", "small_perimeter_speed"};
    return keys;
}

std::string serialized(const DynamicPrintConfig &config, const std::vector<std::string> &keys)
{
    std::string out;
    for (const std::string &key : keys)
        out += key + "=" + (config.has(key) ? config.opt_serialize(key) : std::string("<none>")) + ";";
    return out;
}

std::vector<std::string> strings_of(const DynamicPrintConfig &config, const char *key)
{
    const auto *option = config.option<ConfigOptionStrings>(key);
    REQUIRE(option != nullptr);
    return option->values;
}

std::vector<int> ints_of(const DynamicPrintConfig &config, const char *key)
{
    const auto *option = config.option<ConfigOptionInts>(key);
    REQUIRE(option != nullptr);
    return option->values;
}

// Writes `value` into column `column` of a floats key.
void set_column(DynamicPrintConfig &config, const std::string &key, size_t column, double value)
{
    auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
    REQUIRE(option != nullptr);
    const std::unique_ptr<ConfigOption> parsed(option->clone());
    REQUIRE(parsed->deserialize(float_to_string_decimal_point(value)));
    option->set_at(parsed.get(), column, 0);
}

const std::vector<int>         U1_WIDE_IDS      = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4};
const std::vector<std::string> U1_WIDE_VARIANTS = {DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD, DD_HIGH_FLOW,
                                                   DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD, DD_HIGH_FLOW};

// The invariants I1..I5 of the wide layout.
void check_invariants(const DynamicPrintConfig &config)
{
    const PerHeadProcess::Layout layout = PerHeadProcess::layout_of(config);
    const std::vector<std::string> marker = strings_of(config, OVERRIDE);
    CHECK(marker.size() == layout.size());
    // I1
    CHECK(PerHeadProcess::is_wide(config) == PerHeadProcess::marker_names_any(config));
    for (const std::string &key : print_options_with_variant) {
        if (key == "print_extruder_id" || key == "print_extruder_variant")
            continue;
        const auto *option = dynamic_cast<const ConfigOptionVectorBase *>(config.option(key));
        if (option == nullptr)
            continue;
        INFO(key);
        // I2
        CHECK(option->size() == layout.size());
        const std::vector<std::string> values = option->vserialize();
        for (size_t column = 0; column < layout.size(); ++column) {
            if (layout.ids[column] == 0) {
                // I5
                CHECK(marker[column].empty());
                continue;
            }
            const int shared = PerHeadProcess::shared_column(config, PerHeadProcess::variant_names_type(layout.variants[column], nvtHighFlow) ? nvtHighFlow : nvtStandard);
            REQUIRE(shared >= 0);
            // I3
            if (!PerHeadProcess::is_marked(config, column, key) && key != OVERRIDE)
                CHECK(values[column] == values[size_t(shared)]);
        }
    }
    // I4
    for (size_t head = 0; head < 4; ++head)
        for (const std::string &key : PerHeadProcess::head_override_keys(config, head)) {
            const auto *option = dynamic_cast<const ConfigOptionVectorBase *>(config.option(key));
            REQUIRE(option != nullptr);
            const std::vector<std::string> values = option->vserialize();
            const std::vector<int>         columns = PerHeadProcess::head_columns(config, head);
            for (int column : columns) {
                CHECK(PerHeadProcess::is_marked(config, size_t(column), key));
                CHECK(values[size_t(column)] == values[size_t(columns.front())]);
            }
        }
}

} // namespace

// The layout primitive.
TEST_CASE("A process preset is laid out with shared columns and one column per tool head and back", "[PerHeadProcess][PerHeadOverride][pho_relayout]")
{
    SECTION("the two-column 0.20mm Standard child widens to the U1 layout of ten columns") {
        DynamicPrintConfig config = flow_only_process();
        config.set_key_value(OVERRIDE, new ConfigOptionStrings(std::vector<std::string>(2, std::string())));
        const std::string before = serialized(config, probe_keys()) + serialized(config, {"print_extruder_id", "print_extruder_variant", OVERRIDE});
        const DynamicPrintConfig printer = u1_like_printer();

        PerHeadProcess::widen(config, printer);
        CHECK(PerHeadProcess::is_wide(config));
        CHECK(ints_of(config, "print_extruder_id") == U1_WIDE_IDS);
        CHECK(strings_of(config, "print_extruder_variant") == U1_WIDE_VARIANTS);
        CHECK(PerHeadProcess::shared_width(config) == 2);
        // Every head column holds the shared column of its flow.
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{200., 500., 200., 500., 200., 500., 200., 500., 200., 500.});
        CHECK(floats_of(config, "travel_speed") == std::vector<double>{500., 550., 500., 550., 500., 550., 500., 550., 500., 550.});
        CHECK(config.option<ConfigOptionFloatsOrPercentsNullable>("small_perimeter_speed")->values[7] == FloatOrPercent{60., true});
        CHECK(config.option<ConfigOptionBoolsNullable>("enable_overhang_speed")->values.size() == 10);
        CHECK(strings_of(config, OVERRIDE) == std::vector<std::string>(10, std::string()));
        CHECK(PerHeadProcess::shared_column(config, nvtStandard) == 0);
        CHECK(PerHeadProcess::shared_column(config, nvtHighFlow) == 1);
        CHECK(PerHeadProcess::head_columns(config, 2) == std::vector<int>{6, 7});
        CHECK(PerHeadProcess::head_columns(config, 4).empty());

        // Idempotent.
        const std::string once = serialized(config, probe_keys());
        PerHeadProcess::relayout(config, PerHeadProcess::layout_of(config));
        PerHeadProcess::widen(config, printer);
        CHECK(serialized(config, probe_keys()) == once);

        // A value set for tool head 3 lives in both of its columns and is marked there (I4).
        set_column(config, "outer_wall_speed", 6, 90.);
        PerHeadProcess::set_head_value(config, 2, "outer_wall_speed", 6);
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{200., 500., 200., 500., 200., 500., 90., 90., 200., 500.});
        CHECK(PerHeadProcess::is_marked(config, 6, "outer_wall_speed"));
        CHECK(PerHeadProcess::is_marked(config, 7, "outer_wall_speed"));
        CHECK_FALSE(PerHeadProcess::is_marked(config, 0, "outer_wall_speed"));
        CHECK(PerHeadProcess::head_override_keys(config, 2) == std::vector<std::string>{"outer_wall_speed"});
        CHECK(PerHeadProcess::heads_marked_for(config, "outer_wall_speed") == std::vector<size_t>{2});
        check_invariants(config);

        // A relayout to the same layout keeps the value and the marker.
        PerHeadProcess::relayout(config, PerHeadProcess::layout_of(config));
        CHECK(floats_of(config, "outer_wall_speed")[6] == 90.);
        CHECK(PerHeadProcess::is_marked(config, 7, "outer_wall_speed"));

        // Narrowing takes the shared columns and gives the vendor layout back.
        PerHeadProcess::clear_head(config, 2);
        CHECK(PerHeadProcess::marker_empty(config));
        PerHeadProcess::narrow(config);
        CHECK_FALSE(PerHeadProcess::is_wide(config));
        CHECK(serialized(config, probe_keys()) + serialized(config, {"print_extruder_id", "print_extruder_variant", OVERRIDE}) == before);
    }

    SECTION("a one-column preset widens to nine columns whose High Flow head columns take the single shared value") {
        const std::unique_ptr<Preset> source = single_column_source();
        DynamicPrintConfig            config = source->config;
        const std::string             before = serialized(config, probe_keys()) + serialized(config, {"print_extruder_id", "print_extruder_variant"});
        PerHeadProcess::widen(config, u1_like_printer());
        CHECK(ints_of(config, "print_extruder_id") == std::vector<int>{0, 1, 1, 2, 2, 3, 3, 4, 4});
        CHECK(strings_of(config, "print_extruder_variant") == std::vector<std::string>{DD_STANDARD, DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD, DD_HIGH_FLOW});
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>(9, 60.));
        CHECK(PerHeadProcess::shared_width(config) == 1);
        CHECK(PerHeadProcess::shared_column(config, nvtHighFlow) == 0);
        CHECK(PerHeadProcess::head_columns(config, 2) == std::vector<int>{5, 6});
        // I1 holds once a value is set (the widening is the first half of the first per-head edit).
        set_column(config, "outer_wall_speed", 5, 90.);
        PerHeadProcess::set_head_value(config, 2, "outer_wall_speed", 5);
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{60., 60., 60., 60., 60., 90., 90., 60., 60.});
        check_invariants(config);
        PerHeadProcess::clear_head(config, 2);
        CHECK(PerHeadProcess::marker_empty(config));
        PerHeadProcess::narrow(config);
        CHECK(serialized(config, probe_keys()) + serialized(config, {"print_extruder_id", "print_extruder_variant"}) == before);
    }

    SECTION("a generic three-head printer without printer ids gets one shared and three head columns") {
        const std::unique_ptr<Preset> source = single_column_source();
        DynamicPrintConfig            config = source->config;
        PerHeadProcess::widen(config, three_head_printer());
        CHECK(ints_of(config, "print_extruder_id") == std::vector<int>{0, 1, 2, 3});
        CHECK(strings_of(config, "print_extruder_variant") == std::vector<std::string>(4, DD_STANDARD));
        CHECK(floats_of(config, "travel_speed") == std::vector<double>(4, 300.));
        CHECK(PerHeadProcess::head_columns(config, 1) == std::vector<int>{2});
        set_column(config, "travel_speed", 2, 150.);
        PerHeadProcess::set_head_value(config, 1, "travel_speed", 2);
        CHECK(floats_of(config, "travel_speed") == std::vector<double>{300., 300., 150., 300.});
        CHECK(PerHeadProcess::heads_marked_for(config, "travel_speed") == std::vector<size_t>{1});
        check_invariants(config);
    }

    SECTION("the parent laid out like a wide child is the parent itself when the child is narrow") {
        const DynamicPrintConfig parent = flow_only_process();
        DynamicPrintConfig       storage;
        const DynamicPrintConfig &same = PerHeadProcess::reference_in_layout_of(parent, parent, storage);
        CHECK(&same == &parent);
        DynamicPrintConfig child = parent;
        PerHeadProcess::widen(child, u1_like_printer());
        const DynamicPrintConfig &wide = PerHeadProcess::reference_in_layout_of(child, parent, storage);
        CHECK(&wide == &storage);
        CHECK(ints_of(wide, "print_extruder_id") == U1_WIDE_IDS);
        CHECK(floats_of(wide, "outer_wall_speed") == floats_of(child, "outer_wall_speed"));
    }
}

// The head lookups of the engine never read a shared column.
TEST_CASE("On the wide layout every tool head resolves to its own column and never to a shared one", "[PerHeadProcess][PerHeadOverride][pho_lookup]")
{
    DynamicPrintConfig config = flow_only_process();
    PerHeadProcess::widen(config, u1_like_printer());
    for (int head = 1; head <= 4; ++head)
        for (NozzleVolumeType flow : {nvtStandard, nvtHighFlow}) {
            const int index = config.get_index_for_extruder(head, "print_extruder_id", etDirectDrive, flow, "print_extruder_variant");
            INFO("head " << head << " flow " << int(flow));
            CHECK(index == 2 + 2 * (head - 1) + (flow == nvtHighFlow ? 1 : 0));
            REQUIRE(index >= 0);
            CHECK(ints_of(config, "print_extruder_id")[size_t(index)] == head);
        }
    // A fifth head has no column: the lookup misses, it does not fall onto a shared column.
    CHECK(config.get_index_for_extruder(5, "print_extruder_id", etDirectDrive, nvtStandard, "print_extruder_variant") < 0);
}

// A preset with values set per tool head follows the tool heads of the printer (a
// printer selection, a changed extruder count). The tool heads that stay keep their values and
// marks, a gained tool head takes the shared columns, a lost one drops its column and its marks.
TEST_CASE("A preset with values set per tool head is laid out for the tool heads of another printer", "[PerHeadProcess][PerHeadOverride][pho_relayout_printer]")
{
    // A generic printer with `heads` Direct Drive Standard tool heads and no printer ids.
    auto generic_printer = [](size_t heads) {
        DynamicPrintConfig printer;
        printer.option<ConfigOptionFloats>("nozzle_diameter", true)->values           = std::vector<double>(heads, 0.4);
        printer.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values      = std::vector<int>(heads, int(etDirectDrive));
        printer.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = std::vector<int>(heads, int(nvtStandard));
        printer.option<ConfigOptionStrings>("extruder_variant_list", true)->values   = std::vector<std::string>(heads, DD_STANDARD);
        return printer;
    };
    auto layout_for = [](const DynamicPrintConfig &config, const DynamicPrintConfig &printer) {
        return PerHeadProcess::wide_layout(config, printer);
    };

    // Two tool heads; outer wall 30 set for tool head 2.
    DynamicPrintConfig config = flow_only_process();
    config.set_key_value(OVERRIDE, new ConfigOptionStrings(std::vector<std::string>(2, std::string())));
    PerHeadProcess::widen(config, generic_printer(2));
    REQUIRE(ints_of(config, "print_extruder_id") == std::vector<int>{0, 0, 1, 2});
    REQUIRE(strings_of(config, "print_extruder_variant") == std::vector<std::string>{DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD, DD_STANDARD});
    set_column(config, "outer_wall_speed", 3, 30.);
    PerHeadProcess::set_head_value(config, 1, "outer_wall_speed", 3);
    REQUIRE(floats_of(config, "outer_wall_speed") == std::vector<double>{200., 500., 200., 30.});
    // Laid out for its printer already: nothing to do.
    CHECK(layout_for(config, generic_printer(2)) == PerHeadProcess::layout_of(config));

    SECTION("a third tool head takes the shared columns; tool head 2 keeps its value and its mark") {
        const PerHeadProcess::Layout to = layout_for(config, generic_printer(3));
        REQUIRE(to != PerHeadProcess::layout_of(config));
        PerHeadProcess::relayout(config, to);
        CHECK(ints_of(config, "print_extruder_id") == std::vector<int>{0, 0, 1, 2, 3});
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{200., 500., 200., 30., 200.});
        CHECK(floats_of(config, "travel_speed") == std::vector<double>{500., 550., 500., 500., 500.});
        CHECK(PerHeadProcess::head_columns(config, 1) == std::vector<int>{3});
        CHECK(PerHeadProcess::head_columns(config, 2) == std::vector<int>{4});
        CHECK(PerHeadProcess::is_marked(config, 3, "outer_wall_speed"));
        CHECK_FALSE(PerHeadProcess::is_marked(config, 4, "outer_wall_speed"));
        CHECK(PerHeadProcess::heads_marked_for(config, "outer_wall_speed") == std::vector<size_t>{1});
        check_invariants(config);
        // Idempotent: laid out for the three-head printer now.
        CHECK(layout_for(config, generic_printer(3)) == PerHeadProcess::layout_of(config));

        // A value set for the new tool head lands in its own column, the other heads keep theirs (I3).
        set_column(config, "outer_wall_speed", 4, 50.);
        PerHeadProcess::set_head_value(config, 2, "outer_wall_speed", 4);
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{200., 500., 200., 30., 50.});
        CHECK(PerHeadProcess::heads_marked_for(config, "outer_wall_speed") == std::vector<size_t>{1, 2});
        check_invariants(config);

        // Back to two tool heads: the third one's column and mark go, tool head 2 keeps both.
        PerHeadProcess::relayout(config, layout_for(config, generic_printer(2)));
        CHECK(ints_of(config, "print_extruder_id") == std::vector<int>{0, 0, 1, 2});
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{200., 500., 200., 30.});
        CHECK(PerHeadProcess::heads_marked_for(config, "outer_wall_speed") == std::vector<size_t>{1});
        check_invariants(config);
    }

    SECTION("a printer without tool head 2 drops its value and its mark; the marker is empty and the preset narrows") {
        PerHeadProcess::relayout(config, layout_for(config, generic_printer(1)));
        CHECK(ints_of(config, "print_extruder_id") == std::vector<int>{0, 0, 1});
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{200., 500., 200.});
        CHECK(PerHeadProcess::marker_empty(config));
        PerHeadProcess::narrow(config);
        CHECK_FALSE(PerHeadProcess::is_wide(config));
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{200., 500.});
    }

    SECTION("the U1 layout of a preset laid out for a generic printer: a head's value is its value in both flows (I4)") {
        PerHeadProcess::relayout(config, layout_for(config, u1_like_printer()));
        CHECK(ints_of(config, "print_extruder_id") == U1_WIDE_IDS);
        CHECK(strings_of(config, "print_extruder_variant") == U1_WIDE_VARIANTS);
        // Tool head 2 keeps its value in its Standard column and carries it into its High Flow
        // column, new on this printer, with the mark; the unmarked keys of that column take the
        // shared High Flow value.
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{200., 500., 200., 500., 30., 30., 200., 500., 200., 500.});
        CHECK(floats_of(config, "travel_speed") == std::vector<double>{500., 550., 500., 550., 500., 550., 500., 550., 500., 550.});
        CHECK(PerHeadProcess::is_marked(config, 4, "outer_wall_speed"));
        CHECK(PerHeadProcess::is_marked(config, 5, "outer_wall_speed"));
        CHECK(PerHeadProcess::head_override_keys(config, 1) == std::vector<std::string>{"outer_wall_speed"});
        CHECK(PerHeadProcess::head_override_keys(config, 2).empty());
        check_invariants(config);
    }
}

// The load normaliser on inconsistent widths.
TEST_CASE("The load normaliser repairs value keys and ids of different widths", "[PerHeadProcess][PerHeadOverride][pho_normalise_widths]")
{
    const DynamicPrintConfig parent  = flow_only_process();
    const DynamicPrintConfig printer = u1_like_printer();
    DynamicPrintConfig       config  = parent;
    PerHeadProcess::widen(config, printer);
    set_column(config, "outer_wall_speed", 6, 90.);
    PerHeadProcess::set_head_value(config, 2, "outer_wall_speed", 6);

    SECTION("a value key narrower than the ids is widened by the column walk") {
        config.set_key_value("travel_speed", new ConfigOptionFloatsNullable({500., 550.}));
        PerHeadProcess::normalise(config, &parent, &printer);
        CHECK(floats_of(config, "travel_speed") == std::vector<double>{500., 550., 500., 550., 500., 550., 500., 550., 500., 550.});
        CHECK(floats_of(config, "outer_wall_speed")[6] == 90.);
        check_invariants(config);
    }

    SECTION("ids narrower than a value key are rebuilt from the parent and the printer when the marker has the key's width") {
        config.set_key_value("print_extruder_id", new ConfigOptionInts({1, 1}));
        config.set_key_value("print_extruder_variant", new ConfigOptionStrings({DD_STANDARD, DD_HIGH_FLOW}));
        PerHeadProcess::normalise(config, &parent, &printer);
        CHECK(ints_of(config, "print_extruder_id") == U1_WIDE_IDS);
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{200., 500., 200., 500., 200., 500., 90., 90., 200., 500.});
        CHECK(PerHeadProcess::is_marked(config, 6, "outer_wall_speed"));
        check_invariants(config);
    }

    SECTION("without a marker of the key's width the key is cut to the ids") {
        config.set_key_value("print_extruder_id", new ConfigOptionInts({1, 1}));
        config.set_key_value("print_extruder_variant", new ConfigOptionStrings({DD_STANDARD, DD_HIGH_FLOW}));
        config.set_key_value(OVERRIDE, new ConfigOptionStrings(std::vector<std::string>(2, std::string())));
        PerHeadProcess::normalise(config, &parent, &printer);
        CHECK(ints_of(config, "print_extruder_id") == std::vector<int>{1, 1});
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{200., 500.});
        CHECK_FALSE(PerHeadProcess::is_wide(config));
        CHECK(PerHeadProcess::marker_empty(config));
    }

    SECTION("a wide layout whose marker names nothing is narrowed, a marker naming a uniform key is dropped") {
        PerHeadProcess::clear_head(config, 2);
        auto *marker = config.option<ConfigOptionStrings>(OVERRIDE);
        marker->values[4] = "enable_overhang_speed";
        PerHeadProcess::normalise(config, &parent, &printer);
        CHECK_FALSE(PerHeadProcess::is_wide(config));
        CHECK(ints_of(config, "print_extruder_id") == std::vector<int>{1, 1});
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{200., 500.});
    }
}

// Mainline data (one column per tool head, no shared columns) on a printer that uses the selector.
TEST_CASE("A per-head process layout without shared columns gets them from the parent and a marker for the columns that differ", "[PerHeadProcess][PerHeadOverride][pho_normalise_mainline]")
{
    const DynamicPrintConfig parent = flow_only_process();
    DynamicPrintConfig       config = parent;
    config.set_key_value("print_extruder_id", new ConfigOptionInts({1, 2, 3, 4}));
    config.set_key_value("print_extruder_variant", new ConfigOptionStrings(std::vector<std::string>(4, DD_STANDARD)));
    config.set_key_value("outer_wall_speed", new ConfigOptionFloatsNullable({200., 60., 200., 200.}));
    config.set_key_value("default_acceleration", new ConfigOptionFloatsNullable(std::vector<double>(4, 10000.)));
    config.set_key_value("travel_speed", new ConfigOptionFloatsNullable(std::vector<double>(4, 500.)));
    config.set_key_value("enable_overhang_speed", new ConfigOptionBoolsNullable(std::vector<unsigned char>(4, 1)));
    config.set_key_value("small_perimeter_speed", new ConfigOptionFloatsOrPercentsNullable(std::vector<FloatOrPercent>(4, FloatOrPercent{50., true})));
    config.erase(OVERRIDE);

    PerHeadProcess::normalise(config, &parent, nullptr);
    CHECK(ints_of(config, "print_extruder_id") == std::vector<int>{0, 0, 1, 2, 3, 4});
    CHECK(strings_of(config, "print_extruder_variant") == std::vector<std::string>{DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD, DD_STANDARD, DD_STANDARD, DD_STANDARD});
    // The shared columns are the parent's; head 2 keeps its own value and is marked for it.
    CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{200., 500., 200., 60., 200., 200.});
    CHECK(PerHeadProcess::head_override_keys(config, 1) == std::vector<std::string>{"outer_wall_speed"});
    CHECK(PerHeadProcess::head_override_keys(config, 0).empty());
    CHECK(PerHeadProcess::head_override_keys(config, 2).empty());
    check_invariants(config);
}

// The dirty list of a wide preset against its parent names the head's columns alone.
TEST_CASE("A value set for one tool head is dirty in that head's columns against the parent and nowhere else", "[PerHeadProcess][PerHeadOverride][Profiles][pho_dirty_columns]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.});
    DynamicPrintConfig &edited = bundle->prints.get_edited_preset().config;
    PerHeadProcess::widen(edited, bundle->printers.get_edited_preset().config);
    REQUIRE(PerHeadProcess::is_wide(edited));
    REQUIRE(ints_of(edited, "print_extruder_id") == U1_WIDE_IDS);
    set_column(edited, "outer_wall_speed", 6, 90.);
    PerHeadProcess::set_head_value(edited, 2, "outer_wall_speed", 6);

    const std::vector<std::string> dirty = bundle->prints.current_different_from_parent_options(true);
    std::vector<std::string> speed_entries, id_entries, other;
    for (const std::string &entry : dirty) {
        if (entry.rfind("outer_wall_speed#", 0) == 0)
            speed_entries.emplace_back(entry);
        else if (entry.rfind("print_extruder_id", 0) == 0 || entry.rfind("print_extruder_variant", 0) == 0)
            id_entries.emplace_back(entry);
        else if (entry.rfind(OVERRIDE, 0) != 0)
            other.emplace_back(entry);
    }
    CHECK(speed_entries == std::vector<std::string>{"outer_wall_speed#6", "outer_wall_speed#7"});
    // The widened ids and variants equal the parent's laid out like the child.
    CHECK(id_entries.empty());
    INFO("other dirty entries: " << boost::algorithm::join(other, " "));
    CHECK(other.empty());
    // The same lists drive the composer's edited keys: the head value does not make the key edited.
    CHECK(PerHeadProcess::all_edited_keys(*bundle).count("outer_wall_speed") == 0);
    set_column(edited, "inner_wall_speed", 0, 123.);
    PerHeadProcess::set_shared_value(edited, "inner_wall_speed", nvtStandard);
    CHECK(PerHeadProcess::all_edited_keys(*bundle).count("inner_wall_speed") == 1);
}

// A user preset wider than its parent is saved and reloaded column by column.
TEST_CASE("A user process preset with values set for a tool head reloads with every column and its marker", "[PerHeadProcess][PerHeadOverride][Profiles][pho_user_preset_round_trip]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.});
    Preset *parent = bundle->prints.find_preset(STD_020_04, false, true);
    REQUIRE(parent != nullptr);
    const DynamicPrintConfig &printer = bundle->printers.get_edited_preset().config;

    const double head_3_value = GENERATE(90., 200.);
    CAPTURE(head_3_value);

    ScopedTemporaryDir temp_dir("orca_per_head_override");
    const std::string  name = "Wide walls @Snapmaker U1 (0.4 nozzle)";
    {
        Preset child(Preset::TYPE_PRINT, name, false);
        child.config   = parent->config;
        child.version  = parent->version;
        child.inherits() = parent->name;
        child.file     = (temp_dir.path() / PRESET_PRINT_NAME / (name + ".json")).string();
        PerHeadProcess::widen(child.config, printer);
        REQUIRE(ints_of(child.config, "print_extruder_id") == U1_WIDE_IDS);
        set_column(child.config, "outer_wall_speed", 6, head_3_value);
        PerHeadProcess::set_head_value(child.config, 2, "outer_wall_speed", 6);
        child.save(&parent->config);
        REQUIRE(boost::filesystem::exists(child.file));
    }

    // The parent changes before the reload; the saved head value is kept, the shared columns follow.
    const double parent_wall = 250.;
    parent->config.option<ConfigOptionFloatsNullable>("outer_wall_speed")->values[0] = parent_wall;

    PresetsConfigSubstitutions substitutions;
    bundle->prints.load_presets(temp_dir.path().string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);
    const Preset *loaded = bundle->prints.find_preset(name, false);
    REQUIRE(loaded != nullptr);
    const DynamicPrintConfig &config = loaded->config;
    CHECK(ints_of(config, "print_extruder_id") == U1_WIDE_IDS);
    CHECK(strings_of(config, "print_extruder_variant") == U1_WIDE_VARIANTS);
    const std::vector<double> walls = floats_of(config, "outer_wall_speed");
    REQUIRE(walls.size() == 10);
    CHECK(walls[6] == head_3_value);
    CHECK(walls[7] == head_3_value);
    // Every other head column equals the shared column of its flow, which follows the changed parent.
    CHECK(walls[0] == parent_wall);
    for (size_t column : {2, 4, 8})
        CHECK(walls[column] == parent_wall);
    for (size_t column : {1, 3, 5, 9})
        CHECK(walls[column] == 500.);
    // An untouched key is as wide as the layout and holds the shared value on every head.
    const std::vector<double> inner = floats_of(config, "inner_wall_speed");
    REQUIRE(inner.size() == 10);
    for (size_t column = 2; column < 10; ++column)
        CHECK(inner[column] == inner[column % 2]);
    const std::vector<std::string> marker = strings_of(config, OVERRIDE);
    REQUIRE(marker.size() == 10);
    CHECK(marker[6] == "outer_wall_speed");
    CHECK(marker[7] == "outer_wall_speed");
    CHECK(marker[0].empty());
    CHECK(marker[2].empty());
    check_invariants(config);
}

// The project round trip.
TEST_CASE("A project stores the values set per tool head with their marker and loads them back", "[PerHeadProcess][PerHeadOverride][Profiles][pho_project_round_trip]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.});
    bundle->project_config.option<ConfigOptionStrings>("filament_colour", true)->values = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};

    SECTION("a project without values set per tool head keeps the vendor layout") {
        const DynamicPrintConfig stored = bundle->full_config_secure();
        CHECK(ints_of(stored, "print_extruder_id") == std::vector<int>{1, 1});
        CHECK(PerHeadProcess::marker_empty(stored));
        // full_config_secure writes the diff list only when some preset differs from its system
        // preset (add_if_some_non_empty): the key is absent, or its process entry is empty.
        const auto *different = stored.option<ConfigOptionStrings>("different_settings_to_system");
        CHECK((different == nullptr || different->values.empty() || different->values.front().empty()));
    }

    SECTION("a value set for tool head 3 survives the round trip in both of its columns") {
        DynamicPrintConfig &edited = bundle->prints.get_edited_preset().config;
        PerHeadProcess::widen(edited, bundle->printers.get_edited_preset().config);
        set_column(edited, "outer_wall_speed", 6, 90.);
        PerHeadProcess::set_head_value(edited, 2, "outer_wall_speed", 6);

        const DynamicPrintConfig stored = bundle->full_config_secure();
        CHECK(ints_of(stored, "print_extruder_id") == U1_WIDE_IDS);
        CHECK(floats_of(stored, "outer_wall_speed")[6] == 90.);
        CHECK(strings_of(stored, OVERRIDE)[7] == "outer_wall_speed");
        CHECK(project_schema_version_for(stored) == 2);
        // The diff list of the process names every widened key, the ids, the variants and the marker.
        const std::vector<std::string> different = strings_of(stored, "different_settings_to_system");
        REQUIRE_FALSE(different.empty());
        std::vector<std::string> keys;
        Slic3r::unescape_strings_cstyle(different[0], keys);
        for (const char *key : {"outer_wall_speed", "inner_wall_speed", "travel_speed", "print_extruder_id", "print_extruder_variant", OVERRIDE}) {
            INFO(key);
            CHECK(std::find(keys.begin(), keys.end(), key) != keys.end());
        }

        auto          reloaded = load_snapmaker_bundle();
        PresetBundle &second   = *reloaded;
        DynamicPrintConfig project = stored;
        Preset::normalize(project);
        second.load_config_model("per_head_override.3mf", std::move(project), Semver());
        const DynamicPrintConfig &loaded = second.prints.get_edited_preset().config;
        CHECK(ints_of(loaded, "print_extruder_id") == U1_WIDE_IDS);
        CHECK(strings_of(loaded, "print_extruder_variant") == U1_WIDE_VARIANTS);
        CHECK(floats_of(loaded, "outer_wall_speed") == floats_of(edited, "outer_wall_speed"));
        CHECK(floats_of(loaded, "inner_wall_speed").size() == 10);
        CHECK(floats_of(loaded, "inner_wall_speed") == floats_of(edited, "inner_wall_speed"));
        CHECK(strings_of(loaded, OVERRIDE) == strings_of(edited, OVERRIDE));
        check_invariants(loaded);
    }
}

// ---- Values set per tool head: the composer, the overrides and the fallback ------

// The semantics of set_shared_value and clear.
TEST_CASE("An edit under All tool heads skips a head with a value of its own and clearing a head restores the shared value", "[PerHeadProcess][PerHeadOverride][pho_shared_and_clear]")
{
    DynamicPrintConfig config = flow_only_process();
    PerHeadProcess::widen(config, u1_like_printer());
    // Tool head 3 has its own outer wall speed.
    set_column(config, "outer_wall_speed", 6, 90.);
    PerHeadProcess::set_head_value(config, 2, "outer_wall_speed", 6);

    SECTION("All writes the shared column and every unmarked head column of the flow; the marked head keeps its value") {
        set_column(config, "outer_wall_speed", 0, 150.);
        PerHeadProcess::set_shared_value(config, "outer_wall_speed", nvtStandard);
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{150., 500., 150., 500., 150., 500., 90., 90., 150., 500.});
        check_invariants(config);
        // Clearing the head gives it the shared value of each flow, not the parent's and not head 1's.
        PerHeadProcess::clear_head_value(config, 2, "outer_wall_speed");
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{150., 500., 150., 500., 150., 500., 150., 500., 150., 500.});
        CHECK(PerHeadProcess::marker_empty(config));
        PerHeadProcess::narrow(config);
        CHECK(ints_of(config, "print_extruder_id") == std::vector<int>{1, 1});
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{150., 500.});
    }

    SECTION("with every head set, clearing one gives it the shared value (150), not the parent's 200 nor head 1's value") {
        set_column(config, "outer_wall_speed", 0, 150.);
        PerHeadProcess::set_shared_value(config, "outer_wall_speed", nvtStandard);
        const std::vector<double> own = {10., 20., 30., 40.};
        for (size_t head = 0; head < 4; ++head) {
            const int column = PerHeadProcess::head_columns(config, head).front();
            set_column(config, "outer_wall_speed", size_t(column), own[head]);
            PerHeadProcess::set_head_value(config, head, "outer_wall_speed", column);
        }
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{150., 500., 10., 10., 20., 20., 30., 30., 40., 40.});
        check_invariants(config);
        PerHeadProcess::clear_head_value(config, 1, "outer_wall_speed");
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{150., 500., 10., 10., 150., 500., 30., 30., 40., 40.});
        CHECK(PerHeadProcess::heads_marked_for(config, "outer_wall_speed") == std::vector<size_t>{0, 2, 3});
        // An edit under All now reaches head 2 alone among the heads.
        set_column(config, "outer_wall_speed", 0, 160.);
        PerHeadProcess::set_shared_value(config, "outer_wall_speed", nvtStandard);
        CHECK(floats_of(config, "outer_wall_speed") == std::vector<double>{160., 500., 10., 10., 160., 500., 30., 30., 40., 40.});
        PerHeadProcess::clear_all_heads(config);
        CHECK(PerHeadProcess::marker_empty(config));
        for (size_t column = 2; column < 10; ++column)
            CHECK(floats_of(config, "outer_wall_speed")[column] == (column % 2 == 0 ? 160. : 500.));
    }

    SECTION("a one-column parent: the single shared column feeds both flow columns of every head") {
        const std::unique_ptr<Preset> source = single_column_source();
        DynamicPrintConfig            one = source->config;
        PerHeadProcess::widen(one, u1_like_printer());
        set_column(one, "outer_wall_speed", 3, 90.);
        PerHeadProcess::set_head_value(one, 1, "outer_wall_speed", 3);
        set_column(one, "outer_wall_speed", 0, 75.);
        PerHeadProcess::set_shared_value(one, "outer_wall_speed", nvtHighFlow);
        CHECK(floats_of(one, "outer_wall_speed") == std::vector<double>{75., 75., 75., 90., 90., 75., 75., 75., 75.});
        check_invariants(one);
    }
}

// How an override of an object, part or layer range is read per slot.
TEST_CASE("An override is read once for every slot, by the flow of the slot, or slot by slot", "[PerHeadProcess][PerHeadOverride][pho_override_rule]")
{
    auto target_of = [](std::vector<double> values) {
        ConfigOptionFloatsNullable target;
        target.values = std::move(values);
        return target;
    };
    const std::vector<int> selected_columns = {2, 4, 7, 8}; // the wide columns of four slots: Std, Std, HF, Std

    SECTION("a one-value override applies to every slot") {
        ConfigOptionFloatsNullable target = target_of({1., 2., 3., 4.});
        ConfigOptionFloatsNullable source; source.values = {90.};
        set_variant_override(target, source, selected_columns, 1, VariantOverrideRule());
        CHECK(target.values == std::vector<double>(4, 90.));
    }
    SECTION("an override as wide as the shared columns is read by the flow of each slot") {
        ConfigOptionFloatsNullable target = target_of({1., 2., 3., 4.});
        ConfigOptionFloatsNullable source; source.values = {90., 400.};
        VariantOverrideRule rule;
        rule.flow_index = {0, 0, 1, 0};
        rule.flow_count = 2;
        set_variant_override(target, source, selected_columns, 1, rule);
        CHECK(target.values == std::vector<double>{90., 90., 400., 90.});
    }
    SECTION("a slot whose flow has no shared column keeps its value") {
        ConfigOptionFloatsNullable target = target_of({1., 2., 3., 4.});
        ConfigOptionFloatsNullable source; source.values = {90., 400.};
        VariantOverrideRule rule;
        rule.flow_index = {0, -1, 1, 0};
        rule.flow_count = 2;
        set_variant_override(target, source, selected_columns, 1, rule);
        CHECK(target.values == std::vector<double>{90., 2., 400., 90.});
    }
    SECTION("with as many shared columns as slots the override is read by column, as mainline does") {
        ConfigOptionFloatsNullable target = target_of({1., 2.});
        ConfigOptionFloatsNullable source; source.values = {90., 400.};
        VariantOverrideRule rule;
        rule.flow_index = {0, 0};
        rule.flow_count = 2;
        set_variant_override(target, source, {0, 0}, 1, rule);
        CHECK(target.values == std::vector<double>{90., 90.});
    }
    SECTION("on a composed table an override as wide as the slot table is read slot by slot") {
        ConfigOptionFloatsNullable target = target_of({1., 2., 3., 4.});
        ConfigOptionFloatsNullable source; source.values = {10., 20., 30., 40.};
        VariantOverrideRule rule;
        rule.composed = true;
        set_variant_override(target, source, {0, 0, 1, 0}, 1, rule);
        CHECK(target.values == std::vector<double>{10., 20., 30., 40.});
    }
    SECTION("the flow-only space of a layout") {
        DynamicPrintConfig config = flow_only_process();
        CHECK(PerHeadProcess::flow_space_variants(config) == std::vector<std::string>{DD_STANDARD, DD_HIGH_FLOW});
        PerHeadProcess::widen(config, u1_like_printer());
        CHECK(PerHeadProcess::flow_space_variants(config) == std::vector<std::string>{DD_STANDARD, DD_HIGH_FLOW});
        config.set_key_value("print_extruder_id", new ConfigOptionInts({1, 2, 3, 4}));
        config.set_key_value("print_extruder_variant", new ConfigOptionStrings(std::vector<std::string>(4, DD_STANDARD)));
        CHECK(PerHeadProcess::flow_space_variants(config).empty());
        config.set_key_value("print_extruder_id", new ConfigOptionInts({1}));
        config.set_key_value("print_extruder_variant", new ConfigOptionStrings({DD_STANDARD}));
        CHECK(PerHeadProcess::flow_space_variants(config) == std::vector<std::string>{DD_STANDARD});
    }
}

// A tool head the wide layout has no column for takes the shared column of its flow.
TEST_CASE("A tool head without a column in the wide layout is narrowed onto the shared column of its flow", "[PerHeadProcess][PerHeadOverride][pho_miss_shared]")
{
    DynamicPrintConfig full = four_head_printer({nvtStandard, nvtStandard, nvtStandard, nvtStandard}, /*stats=*/false);
    // A fifth tool head on the printer.
    full.option<ConfigOptionFloats>("nozzle_diameter", true)->values.emplace_back(0.4);
    full.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values.emplace_back(int(etDirectDrive));
    full.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values.emplace_back(int(nvtStandard));
    full.option<ConfigOptionStrings>("extruder_variant_list", true)->values.emplace_back(DD_STANDARD + "," + DD_HIGH_FLOW);
    add_flow_only_process(full);
    PerHeadProcess::widen(full, u1_like_printer());
    set_column(full, "outer_wall_speed", 6, 90.);
    PerHeadProcess::set_head_value(full, 2, "outer_wall_speed", 6);
    REQUIRE(ints_of(full, "print_extruder_id") == U1_WIDE_IDS);

    std::vector<std::vector<NozzleVolumeType>> volume_types;
    const int count = full.get_extruder_nozzle_volume_count(5, volume_types);
    REQUIRE(count == 5);
    const std::vector<int> variant_index = full.update_values_to_printer_extruders(full, 5, count, volume_types, print_options_with_variant,
                                                                                   "print_extruder_id", "print_extruder_variant");
    // Heads 1-4 hit their columns; head 5 falls onto the shared Standard column, not onto head 1's.
    CHECK(variant_index == std::vector<int>{2, 4, 6, 8, 0});
    CHECK(floats_of(full, "outer_wall_speed") == std::vector<double>{200., 200., 90., 200., 200.});
}

// On the shipped U1 presets a value set for a tool head beats the
// preset of its nozzle size, applies with the preference off, and does not make its key edited.
TEST_CASE("On a mixed plate a value set for one tool head beats the process preset of its nozzle size and applies with the preference off", "[PerHeadProcess][PerHeadOverride][Profiles][pho_compose_head_value]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.2, 0.6, 0.4}, {0., 0.12, 0.30, 0.});
    const std::string plain_before = serialized(bundle->full_config_for_print(false), probe_keys());

    DynamicPrintConfig &edited = bundle->prints.get_edited_preset().config;
    PerHeadProcess::widen(edited, bundle->printers.get_edited_preset().config);
    REQUIRE(ints_of(edited, "print_extruder_id") == U1_WIDE_IDS);
    // The wide layout without a value set composes what the vendor layout composes.
    CHECK(serialized(bundle->full_config_for_print(false), probe_keys()) == plain_before);

    // Tool head 2 (0.2 mm, derived): outer wall 90; tool head 3 (0.6 mm, derived) untouched.
    set_column(edited, "outer_wall_speed", 4, 90.);
    PerHeadProcess::set_head_value(edited, 1, "outer_wall_speed", 4);
    const Preset *source_02 = bundle->prints.find_preset(STD_012_02, false);
    REQUIRE(source_02 != nullptr);
    const double source_sparse = floats_of(source_02->config, "sparse_infill_speed").front();
    const double selected_sparse = floats_of(edited, "sparse_infill_speed").front();
    REQUIRE(source_sparse != selected_sparse);

    SECTION("T-E: the composed table holds the head value and the source's other speeds") {
        std::vector<PerHeadProcess::Source> sources;
        const DynamicPrintConfig composed = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
        CHECK(ints_of(composed, "print_extruder_id") == std::vector<int>{1, 2, 3, 4});
        CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{200., 90., 120., 200.});
        CHECK(floats_of(composed, "sparse_infill_speed") == std::vector<double>{selected_sparse, source_sparse, floats_of(composed, "sparse_infill_speed")[2], selected_sparse});
        CHECK(floats_of(composed, "default_acceleration") == std::vector<double>{10000., 10000., 10000., 10000.});
        // The flow-only space of the selected preset is recorded for the overrides.
        CHECK(ints_of(composed, PerHeadProcess::source_flow_key) == std::vector<int>{0, 0, 0, 0});
        CHECK(composed.opt_int(PerHeadProcess::flow_count_key) == 2);
        REQUIRE(sources.size() == 4);
        CHECK(sources[1].overridden_keys == std::vector<std::string>{"outer_wall_speed"});
        CHECK(sources[0].overridden_keys.empty());
        // The head value does not make its key edited; an All-edit does.
        CHECK(PerHeadProcess::all_edited_keys(*bundle).count("outer_wall_speed") == 0);
        CHECK(sources[1].kept_keys.empty());
        set_column(edited, "inner_wall_speed", 0, 123.);
        PerHeadProcess::set_shared_value(edited, "inner_wall_speed", nvtStandard);
        CHECK(PerHeadProcess::all_edited_keys(*bundle) == std::set<std::string>{"inner_wall_speed"});
        const DynamicPrintConfig again = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
        CHECK(floats_of(again, "inner_wall_speed") == std::vector<double>(4, 123.));
        CHECK(floats_of(again, "outer_wall_speed") == std::vector<double>{200., 90., 120., 200.});
        CHECK(sources[1].kept_keys == std::vector<std::string>{"inner_wall_speed"});
    }

    SECTION("T-F: with the preference off the stored columns narrow to the slots exactly") {
        bundle->process_follows_nozzle = false;
        DynamicPrintConfig full = bundle->full_config_for_print(false);
        CHECK(ints_of(full, "print_extruder_id") == U1_WIDE_IDS);
        std::vector<std::vector<NozzleVolumeType>> volume_types;
        const int count = full.get_extruder_nozzle_volume_count(4, volume_types);
        const std::vector<int> variant_index = full.update_values_to_printer_extruders(full, 4, count, volume_types, print_options_with_variant,
                                                                                       "print_extruder_id", "print_extruder_variant");
        CHECK(variant_index == std::vector<int>{2, 4, 6, 8});
        CHECK(floats_of(full, "outer_wall_speed") == std::vector<double>{200., 90., 200., 200.});
        CHECK(floats_of(full, "sparse_infill_speed") == std::vector<double>(4, selected_sparse));
    }
}

// The transfer of rows between presets of different layouts.
TEST_CASE("Rows transferred between a preset with values per tool head and a narrow one land in the column that stands for them", "[PerHeadProcess][PerHeadOverride][pho_transfer]")
{
    const DynamicPrintConfig printer = u1_like_printer();
    DynamicPrintConfig       wide    = flow_only_process();
    PerHeadProcess::widen(wide, printer);
    set_column(wide, "outer_wall_speed", 6, 90.);
    PerHeadProcess::set_head_value(wide, 2, "outer_wall_speed", 6);
    set_column(wide, "travel_speed", 0, 450.);
    PerHeadProcess::set_shared_value(wide, "travel_speed", nvtStandard);

    SECTION("from a wide source into a narrow target: the target is widened, head 3 carries the value and the marker, the shared row lands everywhere") {
        DynamicPrintConfig target = flow_only_process();
        PerHeadProcess::transfer_columns(target, wide, {"outer_wall_speed#6", "outer_wall_speed#7", "travel_speed#0"}, printer);
        REQUIRE(PerHeadProcess::is_wide(target));
        CHECK(ints_of(target, "print_extruder_id") == U1_WIDE_IDS);
        CHECK(floats_of(target, "outer_wall_speed") == std::vector<double>{200., 500., 200., 500., 200., 500., 90., 90., 200., 500.});
        CHECK(PerHeadProcess::head_override_keys(target, 2) == std::vector<std::string>{"outer_wall_speed"});
        CHECK(floats_of(target, "travel_speed") == std::vector<double>{450., 550., 450., 550., 450., 550., 450., 550., 450., 550.});
        check_invariants(target);
    }

    SECTION("an unmarked head row of a wide source carries the shared value and is not transferred as a head value") {
        DynamicPrintConfig target = flow_only_process();
        PerHeadProcess::transfer_columns(target, wide, {"outer_wall_speed#2"}, printer);
        // Nothing marked: the target is back in the vendor layout.
        CHECK_FALSE(PerHeadProcess::is_wide(target));
        CHECK(floats_of(target, "outer_wall_speed") == std::vector<double>{200., 500.});
    }

    SECTION("from a narrow source into a wide target: the row lands in the shared column and every unmarked head column of its flow") {
        DynamicPrintConfig source = flow_only_process();
        set_column(source, "outer_wall_speed", 0, 175.);
        set_column(source, "outer_wall_speed", 1, 600.);
        DynamicPrintConfig target = wide;
        PerHeadProcess::transfer_columns(target, source, {"outer_wall_speed#0", "outer_wall_speed#1"}, printer);
        CHECK(floats_of(target, "outer_wall_speed") == std::vector<double>{175., 600., 175., 600., 175., 600., 90., 90., 175., 600.});
        CHECK(PerHeadProcess::head_override_keys(target, 2) == std::vector<std::string>{"outer_wall_speed"});
        check_invariants(target);
    }
}

// ---- The speeds of each tool head on a plate of four nozzle sizes ------------------------------
// Tool heads of 0.2, 0.4 (High Flow), 0.6 and 0.8 mm under 0.20mm High Quality of the 0.4 mm printer:
// the Speed page (PerHeadProcess::source_column), the nozzle tab hint and the composed table agree.

namespace {

// That plate on the shipped U1 presets.
void select_owner_plate(PresetBundle &bundle)
{
    select_u1(bundle, {0.2, 0.4, 0.6, 0.8}, {0., 0., 0., 0.}, HQ_020_04);
    bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {int(nvtStandard), int(nvtHighFlow), int(nvtStandard),
                                                                                                  int(nvtStandard)};
}

// The column of a process preset that holds the values of `flow`: the column whose variant names
// it, the single column of a one-column preset, -1 when the preset has several columns and none
// for the flow.
int flow_column(const Preset &preset, NozzleVolumeType flow)
{
    const auto *variants = preset.config.option<ConfigOptionStrings>("print_extruder_variant");
    if (variants == nullptr || variants->values.size() <= 1)
        return 0;
    const std::string &wanted = flow == nvtHighFlow ? DD_HIGH_FLOW : DD_STANDARD;
    for (size_t column = 0; column < variants->values.size(); ++column)
        if (variants->values[column] == wanted)
            return int(column);
    return -1;
}

// A column of a process key; a key narrower than the column holds one value for every column (the
// way ConfigOptionVector::get_at and the composer read it).
double value_at(const DynamicPrintConfig &config, const std::string &key, size_t column)
{
    const std::vector<double> values = floats_of(config, key);
    REQUIRE_FALSE(values.empty());
    return values[column < values.size() ? column : 0];
}

const std::vector<std::string> &headline_keys()
{
    static const std::vector<std::string> keys = {"outer_wall_speed", "inner_wall_speed", "sparse_infill_speed", "internal_solid_infill_speed",
                                                  "top_surface_speed", "default_acceleration"};
    return keys;
}

} // namespace

TEST_CASE("On a plate of 0.2, 0.4 High Flow, 0.6 and 0.8 mm tool heads the 0.6 and 0.8 mm heads print with the speeds the page shows for them", "[PerHeadProcess][Profiles][hs_mixed_sources]")
{
    auto bundle = load_snapmaker_bundle();
    select_owner_plate(*bundle);
    const DynamicPrintConfig &printer = bundle->printers.get_edited_preset().config;

    const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
    REQUIRE(sources.size() == 4);
    // No preferred layer height: the 0.20 target. The 0.2 mm size has High Quality presets (the
    // nearest is 0.10); the 0.6 and 0.8 mm sizes have none of that class and walk the ladder to
    // Standard, the height nearest to 0.20 (0.18 and 0.24).
    CHECK(source_names(sources)[0] == HQ_010_02);
    CHECK(source_names(sources)[2] == STD_018_06);
    CHECK(source_names(sources)[3] == STD_024_08);
    CHECK(sources[0].step == PerHeadProcess::Step::SameQuality);
    CHECK(sources[2].step == PerHeadProcess::Step::ClassLadder);
    CHECK(sources[2].class_used == "Standard");
    CHECK(sources[2].reason == PerHeadProcess::Reason::Derived);
    CHECK(sources[3].reason == PerHeadProcess::Reason::Derived);
    CHECK(sources[2].kept_keys.empty());
    CHECK(sources[3].kept_keys.empty());

    const DynamicPrintConfig composed = bundle->full_config_for_print(false);
    REQUIRE(composed.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{1, 2, 3, 4});
    const Preset &selected = bundle->prints.get_selected_preset();
    for (size_t head : {size_t(0), size_t(2), size_t(3)}) {
        REQUIRE(sources[head].derived);
        REQUIRE(sources[head].preset != nullptr);
        const Preset &source = *sources[head].preset;
        // The column the Speed page reads under the head (TabPrint::head_display_source) and the
        // nozzle tab hint reads (Sidebar::update_nozzle_process_hints).
        const int column = PerHeadProcess::source_column(source, head, nvtStandard, printer);
        CHECK(column == flow_column(source, nvtStandard));
        REQUIRE(column >= 0);
        for (const std::string &key : headline_keys()) {
            INFO("tool head " << head + 1 << " (" << source.name << "), " << key);
            const double shown   = value_at(source.config, key, size_t(column));
            const double printed = value_at(composed, key, head);
            CHECK_THAT(printed, Catch::Matchers::WithinAbs(shown, 1e-6));
        }
    }
    // The 0.8 mm head does not print with the selected preset's values: its outer wall and sparse
    // infill differ in the shipped data (0.24mm Standard against 0.20mm High Quality).
    REQUIRE(value_at(sources[3].preset->config, "outer_wall_speed", 0) != value_at(selected.config, "outer_wall_speed", 0));
    CHECK(value_at(composed, "outer_wall_speed", 3) != value_at(selected.config, "outer_wall_speed", 0));
    CHECK(value_at(composed, "sparse_infill_speed", 3) != value_at(selected.config, "sparse_infill_speed", 0));
}

// The High Flow rule of head_sources: a High Flow tool head of the printer preset's size is derived
// too, so it does not print the Standard column of a preset without a High Flow column (notice N4).
TEST_CASE("A High Flow tool head whose process preset has no High Flow column prints with the High Flow column of the preset of its size that has one", "[PerHeadProcess][Profiles][hs_high_flow_source]")
{
    auto bundle = load_snapmaker_bundle();
    select_owner_plate(*bundle);
    const DynamicPrintConfig &printer  = bundle->printers.get_edited_preset().config;
    const Preset             &selected = bundle->prints.get_selected_preset();
    // One column: no High Flow values of its own.
    REQUIRE(selected.config.option<ConfigOptionStrings>("print_extruder_variant")->values.size() == 1);

    std::vector<PerHeadProcess::Source> sources;
    const DynamicPrintConfig composed = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
    REQUIRE(sources.size() == 4);
    CHECK(sources[1].derived);
    REQUIRE(sources[1].preset != nullptr);
    CHECK(sources[1].preset->name == STD_020_04);
    CHECK(sources[1].fallback_variants.empty());

    // The expected source, looked up by name: the checks below compare against it whatever the
    // head was given.
    const Preset *standard = bundle->prints.find_preset(STD_020_04, false);
    REQUIRE(standard != nullptr);
    const int high_flow = flow_column(*standard, nvtHighFlow);
    REQUIRE(high_flow > 0);
    // The page and the hint read the High Flow column of the source for the head.
    CHECK(PerHeadProcess::source_column(*sources[1].preset, 1, nvtHighFlow, printer) == high_flow);
    CHECK(composed.option<ConfigOptionStrings>("print_extruder_variant")->values[1] == DD_HIGH_FLOW);
    for (const std::string &key : headline_keys()) {
        INFO(key);
        CHECK_THAT(value_at(composed, key, 1), Catch::Matchers::WithinAbs(value_at(standard->config, key, size_t(high_flow)), 1e-6));
    }
    // 500 mm/s in the shipped 0.20mm Standard, against the 60 mm/s of 0.20mm High Quality.
    CHECK(value_at(composed, "outer_wall_speed", 1) != value_at(selected.config, "outer_wall_speed", 0));
    // The other tool heads keep their sources.
    CHECK(source_names(sources)[0] == HQ_010_02);
    CHECK(source_names(sources)[2] == STD_018_06);
    CHECK(source_names(sources)[3] == STD_024_08);
}

// A wide preset gives every head a High Flow column, copied from the shared Standard column when the
// preset has no High Flow speeds; the High Flow rule reads the shared columns, not that copy.
TEST_CASE("The High Flow rule decides from the shared columns of a preset with values set per tool head", "[PerHeadProcess][PerHeadOverride][Profiles][hs_high_flow_wide]")
{
    auto bundle = load_snapmaker_bundle();
    select_owner_plate(*bundle);
    const DynamicPrintConfig &printer = bundle->printers.get_edited_preset().config;
    DynamicPrintConfig       &edited  = bundle->prints.get_edited_preset().config;
    const Preset             *standard = bundle->prints.find_preset(STD_020_04, false);
    REQUIRE(standard != nullptr);
    const int high_flow = flow_column(*standard, nvtHighFlow);
    REQUIRE(high_flow > 0);

    // Narrow: one Standard column, no High Flow values.
    CHECK_FALSE(PerHeadProcess::has_high_flow_values(edited, 1, printer));
    CHECK(PerHeadProcess::has_high_flow_values(standard->config, 1, printer));

    // Wide, as the Speed page stores a value set for a tool head: every head owns a High Flow
    // column, the shared columns are still the one Standard column.
    PerHeadProcess::widen(edited, printer);
    REQUIRE(ints_of(edited, "print_extruder_id") == std::vector<int>{0, 1, 1, 2, 2, 3, 3, 4, 4});
    REQUIRE(PerHeadProcess::column_for_head(edited, 1, nvtHighFlow, printer) >= 0);
    CHECK(PerHeadProcess::shared_variants(edited) == std::vector<std::string>{DD_STANDARD});
    CHECK_FALSE(PerHeadProcess::has_high_flow_values(edited, 1, printer));

    SECTION("widened without a value set: the rule holds as on the narrow preset") {
        const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
        REQUIRE(sources.size() == 4);
        CHECK(sources[1].reason == PerHeadProcess::Reason::HighFlow);
        CHECK(sources[1].derived);
        REQUIRE(sources[1].preset != nullptr);
        CHECK(sources[1].preset->name == STD_020_04);
        CHECK(PerHeadProcess::reads_high_flow(sources[1], nvtHighFlow, printer));
    }

    SECTION("a value set for the 0.8 mm head: the High Flow head keeps its source, the 0.8 mm head its value") {
        const std::vector<int> columns = PerHeadProcess::head_columns(edited, 3);
        REQUIRE_FALSE(columns.empty());
        set_column(edited, "outer_wall_speed", size_t(columns.front()), 40.);
        PerHeadProcess::set_head_value(edited, 3, "outer_wall_speed", columns.front());
        std::vector<PerHeadProcess::Source> sources;
        const DynamicPrintConfig composed = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
        REQUIRE(sources.size() == 4);
        CHECK(sources[1].reason == PerHeadProcess::Reason::HighFlow);
        REQUIRE(sources[1].preset != nullptr);
        CHECK(sources[1].preset->name == STD_020_04);
        CHECK(sources[1].overridden_keys.empty());
        CHECK(sources[3].overridden_keys == std::vector<std::string>{"outer_wall_speed"});
        // The composed table: the 0.4 mm High Flow head prints 0.20mm Standard's High Flow column
        // (500), the 0.8 mm head its value.
        REQUIRE(composed.option<ConfigOptionStrings>("print_extruder_variant")->values[1] == DD_HIGH_FLOW);
        for (const std::string &key : headline_keys()) {
            INFO(key);
            CHECK_THAT(value_at(composed, key, 1), Catch::Matchers::WithinAbs(value_at(standard->config, key, size_t(high_flow)), 1e-6));
        }
        CHECK(value_at(composed, "outer_wall_speed", 3) == 40.);
    }

    SECTION("a widened preset that has High Flow speeds keeps them: a uniform 0.4 mm plate passes through") {
        select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.}, STD_020_04);
        bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {int(nvtStandard), int(nvtHighFlow), int(nvtStandard), int(nvtStandard)};
        DynamicPrintConfig &wide = bundle->prints.get_edited_preset().config;
        PerHeadProcess::widen(wide, bundle->printers.get_edited_preset().config);
        CHECK(PerHeadProcess::shared_variants(wide) == std::vector<std::string>{DD_STANDARD, DD_HIGH_FLOW});
        CHECK(PerHeadProcess::has_high_flow_values(wide, 1, bundle->printers.get_edited_preset().config));
        const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
        REQUIRE(sources.size() == 4);
        for (const PerHeadProcess::Source &source : sources) {
            CHECK(source.reason == PerHeadProcess::Reason::HomeSize);
            CHECK_FALSE(source.derived);
        }
    }
}

TEST_CASE("The High Flow rule applies to a High Flow tool head without High Flow speeds alone", "[PerHeadProcess][Profiles][hs_high_flow_rule]")
{
    auto bundle = load_snapmaker_bundle();
    const DynamicPrintConfig &printer = bundle->printers.get_edited_preset().config;

    SECTION("the owner's plate: reason, keys and what the page reads for the head") {
        select_owner_plate(*bundle);
        const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
        REQUIRE(sources.size() == 4);
        CHECK(sources[1].reason == PerHeadProcess::Reason::HighFlow);
        CHECK(sources[1].derived);
        REQUIRE(sources[1].preset != nullptr);
        CHECK(sources[1].preset->name == STD_020_04);
        CHECK(sources[1].composed_keys.size() == 41);
        CHECK(sources[1].kept_keys.empty());
        CHECK(PerHeadProcess::reads_high_flow(sources[1], nvtHighFlow, printer));
        // A Standard head of the home size is not derived; the other sizes keep the size rule.
        CHECK(sources[0].reason == PerHeadProcess::Reason::Derived);
        CHECK(sources[2].reason == PerHeadProcess::Reason::Derived);
        CHECK_FALSE(PerHeadProcess::reads_high_flow(sources[2], nvtStandard, printer));
        // The record names the preset like any derived source.
        const std::vector<PerHeadProcess::Source> recorded = PerHeadProcess::record_sources(*bundle);
        const auto *record = bundle->project_config.option<ConfigOptionStrings>(PerHeadProcess::record_key);
        REQUIRE(record != nullptr);
        REQUIRE(record->values.size() == 4);
        CHECK(record->values[1] == STD_020_04);
        CHECK(recorded[1].reason == PerHeadProcess::Reason::HighFlow);
    }

    SECTION("a process preset with a High Flow column serves its High Flow tool head itself: nothing is derived, the config passes through") {
        select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.}, STD_020_04);
        bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {int(nvtStandard), int(nvtHighFlow), int(nvtStandard), int(nvtStandard)};
        const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
        REQUIRE(sources.size() == 4);
        for (const PerHeadProcess::Source &source : sources) {
            CHECK(source.reason == PerHeadProcess::Reason::HomeSize);
            CHECK_FALSE(source.derived);
        }
        CHECK(bundle->full_config_for_print(false).equals(bundle->full_config(false)));
    }

    SECTION("a Standard tool head under a preset without a High Flow column is left alone") {
        select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.}, HQ_020_04);
        const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
        REQUIRE(sources.size() == 4);
        for (const PerHeadProcess::Source &source : sources)
            CHECK_FALSE(source.derived);
        CHECK(bundle->full_config_for_print(false).equals(bundle->full_config(false)));
    }

    SECTION("with the preference off the rule is off: the head is Off, the config passes through") {
        select_owner_plate(*bundle);
        bundle->process_follows_nozzle = false;
        const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
        REQUIRE(sources.size() == 4);
        CHECK(sources[1].reason == PerHeadProcess::Reason::Off);
        CHECK_FALSE(sources[1].derived);
        CHECK(bundle->full_config_for_print(false).equals(bundle->full_config(false)));
    }

    SECTION("a size without a preset that has High Flow values keeps its source and the selected preset's High Flow column") {
        // The 0.6 mm size declares High Flow (test fixture); no 0.6 preset has a High Flow column.
        select_u1(*bundle, {0.4, 0.4, 0.6, 0.4}, {0., 0., 0.30, 0.});
        Test::u1_0_6_declares_high_flow(*bundle);
        bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {int(nvtStandard), int(nvtStandard), int(nvtHighFlow), int(nvtStandard)};
        std::vector<PerHeadProcess::Source> sources;
        const DynamicPrintConfig composed = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
        REQUIRE(sources.size() == 4);
        CHECK(sources[2].reason == PerHeadProcess::Reason::Derived);
        CHECK(sources[2].preset->name == STD_030_06);
        CHECK(sources[2].fallback_variants == std::vector<int>{int(nvtHighFlow)});
        CHECK_FALSE(PerHeadProcess::reads_high_flow(sources[2], nvtHighFlow, printer));
        CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{200., 200., 500., 200.});
    }

    SECTION("the source rule with a filter: the accepted presets alone, the nearest of any class before the default") {
        select_owner_plate(*bundle);
        const Preset &selected = bundle->prints.get_selected_preset();
        const Preset &u1_04    = machine(*bundle, U1_04);
        PerHeadProcess::Reason reason = PerHeadProcess::Reason::HomeSize;
        auto has_high_flow = [&printer](const Preset &candidate) { return PerHeadProcess::column_for_head(candidate.config, 1, nvtHighFlow, printer) >= 0; };
        // 0.20mm Standard is the one 0.4 mm preset with a High Flow column: exact height, the machine's default.
        const Preset *chosen = PerHeadProcess::source_for_head(*bundle, u1_04, 0., selected, std::string(), reason, has_high_flow);
        REQUIRE(chosen != nullptr);
        CHECK(chosen->name == STD_020_04);
        CHECK(reason == PerHeadProcess::Reason::Derived);
        // A preferred layer height no accepted preset has: the nearest accepted one, whatever its class (step 2b).
        chosen = PerHeadProcess::source_for_head(*bundle, u1_04, 0.12, selected, std::string(), reason, has_high_flow);
        REQUIRE(chosen != nullptr);
        CHECK(chosen->name == STD_020_04);
        // A filter nothing passes: no preset, the machine's default included.
        chosen = PerHeadProcess::source_for_head(*bundle, u1_04, 0., selected, std::string(), reason, [](const Preset &) { return false; });
        CHECK(chosen == nullptr);
        CHECK(reason == PerHeadProcess::Reason::NoProcessPreset);
        // Without a filter the rule is what it was.
        chosen = PerHeadProcess::source_for_head(*bundle, u1_04, 0.12, selected, std::string(), reason);
        REQUIRE(chosen != nullptr);
        CHECK(chosen->name == "0.12mm Standard @Snapmaker U1 (0.4 nozzle)");
    }
}

// ---- The chosen source: a process preset chosen for a tool head in the project ----

namespace {

const char *const STD_024_06   = "0.24mm Standard @Snapmaker U1 (0.6 nozzle)";
const char *const STRENGTH_08  = "0.40mm Strength @Snapmaker U1 (0.8 nozzle)";
const char *const CHOICE       = PerHeadProcess::choice_key;

std::vector<std::string> choices_of(const PresetBundle &bundle)
{
    const auto *option = bundle.project_config.option<ConfigOptionStrings>(CHOICE);
    return option == nullptr ? std::vector<std::string>() : option->values;
}

std::vector<std::string> candidate_names(const std::vector<PerHeadProcess::Candidate> &candidates, PerHeadProcess::Candidate::Group group)
{
    std::vector<std::string> out;
    for (const PerHeadProcess::Candidate &candidate : candidates)
        if (candidate.group == group)
            out.emplace_back(candidate.preset->name);
    return out;
}

// A user process preset `name` inheriting `parent` with `outer_wall` in every column, loaded into the bundle.
Preset &add_user_process(PresetBundle &bundle, const char *name, const char *parent, double outer_wall, bool select = false)
{
    const Preset *base = bundle.prints.find_preset(parent, false);
    REQUIRE(base != nullptr);
    DynamicPrintConfig config = base->config;
    config.option<ConfigOptionString>("inherits", true)->value = parent;
    auto *speeds = dynamic_cast<ConfigOptionVectorBase *>(config.option("outer_wall_speed"));
    REQUIRE(speeds != nullptr);
    std::string values;
    for (size_t column = 0; column < speeds->size(); ++column)
        values += (values.empty() ? "" : ",") + float_to_string_decimal_point(outer_wall);
    REQUIRE(speeds->deserialize(values));
    Preset &preset = bundle.prints.load_preset(std::string(), name, std::move(config), select);
    preset.is_visible = true;
    return preset;
}

// Stores a project config in a 3MF with one object and loads the config back, as Open project does.
DynamicPrintConfig project_round_trip(const DynamicPrintConfig &project, Semver &file_version)
{
    Model model;
    REQUIRE(load_stl((std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl").c_str(), &model));
    model.add_default_instances();
    ScopedTemporaryDir backup_dir("orca_speed_picker");
    model.set_backup_path(backup_dir.string());
    DynamicPrintConfig  stored = project;
    ScopedTemporaryFile temp(".3mf");
    StoreParams         store_params;
    const std::string   path = temp.string();
    store_params.path     = path.c_str();
    store_params.model    = &model;
    store_params.config   = &stored;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    PlateData *plate = new PlateData();
    plate->plate_index = 0;
    store_params.plate_data_list.push_back(plate);
    REQUIRE(store_bbs_3mf(store_params));

    Model                     dst_model;
    ScopedTemporaryDir        dst_backup_dir("orca_speed_picker_dst");
    dst_model.set_backup_path(dst_backup_dir.string());
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs             dst_plates;
    std::vector<Preset*>      project_presets;
    bool is_bbl_3mf = false, is_orca_3mf = false;
    REQUIRE(load_bbs_3mf(path.c_str(), &dst_config, &context, &dst_model, &dst_plates, &project_presets, &is_bbl_3mf, &is_orca_3mf,
                         &file_version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    CHECK(context.substitutions.empty());
    release_PlateData_list(dst_plates);
    delete plate;
    Preset::normalize(dst_config);
    return dst_config;
}

} // namespace

// The composer with a choice.
TEST_CASE("A process preset chosen for a tool head supplies its speeds, below a value set for the head and a value changed under All tool heads", "[PerHeadProcess][Profiles][phs_choice]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.2, 0.6, 0.4}, {0., 0.12, 0.30, 0.});
    CHECK_FALSE(PerHeadProcess::any_chosen(*bundle));
    PerHeadProcess::set_chosen(*bundle, 1, HQ_010_02);
    CHECK(PerHeadProcess::any_chosen(*bundle));
    CHECK(PerHeadProcess::chosen_of(*bundle, 1) == HQ_010_02);
    CHECK(PerHeadProcess::chosen_of(*bundle, 2).empty());
    CHECK(choices_of(*bundle) == std::vector<std::string>{"", HQ_010_02});

    std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
    REQUIRE(sources.size() == 4);
    CHECK(sources[1].chosen == HQ_010_02);
    CHECK(sources[1].chosen_state == PerHeadProcess::ChosenState::Applied);
    CHECK(sources[1].step == PerHeadProcess::Step::Chosen);
    CHECK(sources[1].reason == PerHeadProcess::Reason::Explicit);
    REQUIRE(sources[1].derived);
    CHECK(sources[1].preset->name == HQ_010_02);
    REQUIRE(sources[1].automatic != nullptr);
    CHECK(sources[1].automatic->name == STD_012_02);
    CHECK(sources[2].chosen_state == PerHeadProcess::ChosenState::None);
    CHECK(sources[2].preset->name == STD_030_06);
    CHECK(sources[2].automatic == nullptr);
    DynamicPrintConfig composed = bundle->full_config_for_print(false);
    CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{200., 60., 120., 200.});
    CHECK(floats_of(composed, "default_acceleration") == std::vector<double>{10000., 4000., 10000., 10000.});

    SECTION("a value set for the tool head beats the choice") {
        DynamicPrintConfig &process = bundle->prints.get_edited_preset().config;
        PerHeadProcess::widen(process, bundle->printers.get_edited_preset().config);
        const std::vector<int> columns = PerHeadProcess::head_columns(process, 1);
        REQUIRE_FALSE(columns.empty());
        set_column(process, "outer_wall_speed", size_t(columns.front()), 77.);
        PerHeadProcess::set_head_value(process, 1, "outer_wall_speed", columns.front());
        composed = bundle->full_config_for_print(false);
        CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{200., 77., 120., 200.});
        CHECK(floats_of(composed, "default_acceleration") == std::vector<double>{10000., 4000., 10000., 10000.});
    }

    SECTION("a value changed under All tool heads applies to the chosen head too") {
        bundle->prints.get_edited_preset().config.option<ConfigOptionFloatsNullable>("default_acceleration")->values = {3000., 3000.};
        sources = PerHeadProcess::head_sources(*bundle);
        CHECK(sources[1].kept_keys == std::vector<std::string>{"default_acceleration"});
        composed = bundle->full_config_for_print(false);
        CHECK(floats_of(composed, "default_acceleration") == std::vector<double>{3000., 3000., 3000., 3000.});
        CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{200., 60., 120., 200.});
    }

    SECTION("the automatic item clears the choice and the vector of empty entries reduces to []") {
        PerHeadProcess::set_chosen(*bundle, 1, std::string());
        CHECK(choices_of(*bundle).empty());
        CHECK_FALSE(PerHeadProcess::any_chosen(*bundle));
        CHECK(PerHeadProcess::head_sources(*bundle)[1].preset->name == STD_012_02);
        // set_chosen grows and never shrinks: a choice on the last head keeps the earlier entries.
        PerHeadProcess::set_chosen(*bundle, 3, HQ_020_04);
        PerHeadProcess::set_chosen(*bundle, 1, HQ_010_02);
        PerHeadProcess::set_chosen(*bundle, 3, std::string());
        CHECK(choices_of(*bundle) == std::vector<std::string>{"", HQ_010_02, "", ""});
    }
}

// A choice on a head of the home size, and one that names the selected preset.
TEST_CASE("A tool head of the printer preset's size takes a chosen preset, and a choice naming the selected preset composes nothing and is kept", "[PerHeadProcess][Profiles][phs_choice]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.});
    PerHeadProcess::set_chosen(*bundle, 0, HQ_020_04);
    std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
    REQUIRE(sources.size() == 4);
    CHECK(sources[0].chosen_state == PerHeadProcess::ChosenState::Applied);
    CHECK(sources[0].automatic == nullptr);
    CHECK(sources[1].reason == PerHeadProcess::Reason::HomeSize);
    DynamicPrintConfig composed = bundle->full_config_for_print(false);
    CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{60., 200., 200., 200.});
    // The record names the chosen preset; the choice key is not written by the pre-apply pass.
    PerHeadProcess::record_sources(*bundle);
    CHECK(bundle->project_config.option<ConfigOptionStrings>(PerHeadProcess::record_key)->values == std::vector<std::string>{HQ_020_04, "", "", ""});
    CHECK(choices_of(*bundle) == std::vector<std::string>{HQ_020_04});
    CHECK(PerHeadProcess::load_report(*bundle).empty());
    // The candidates of a home-size head list the selected preset, marked.
    const std::vector<PerHeadProcess::Candidate> candidates = PerHeadProcess::picker_candidates(*bundle, 0);
    bool selected_listed = false;
    for (const PerHeadProcess::Candidate &candidate : candidates)
        if (candidate.preset->name == STD_020_04) {
            selected_listed = true;
            CHECK(candidate.is_selected);
            CHECK(candidate.is_automatic);
        }
    CHECK(selected_listed);

    // The plate takes the chosen preset: the choice is the selected preset now.
    REQUIRE(bundle->prints.select_preset_by_name(HQ_020_04, true));
    sources = PerHeadProcess::head_sources(*bundle);
    CHECK(sources[0].chosen_state == PerHeadProcess::ChosenState::SameAsSelected);
    CHECK(sources[0].step == PerHeadProcess::Step::Chosen);
    CHECK_FALSE(sources[0].derived);
    CHECK(sources[0].reason == PerHeadProcess::Reason::HomeSize);
    CHECK(choices_of(*bundle) == std::vector<std::string>{HQ_020_04});
    CHECK(bundle->full_config_for_print(false).equals(bundle->full_config(false)));

    REQUIRE(bundle->prints.select_preset_by_name(STD_020_04, true));
    sources = PerHeadProcess::head_sources(*bundle);
    CHECK(sources[0].chosen_state == PerHeadProcess::ChosenState::Applied);
    CHECK(sources[0].derived);
}

// A choice applies with the preference off.
TEST_CASE("A chosen preset applies with the preference off while the other tool heads print the selected preset", "[PerHeadProcess][Profiles][phs_choice]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.2, 0.6, 0.4}, {0., 0.12, 0.30, 0.});
    bundle->process_follows_nozzle = false;
    CHECK_FALSE(PerHeadProcess::active(*bundle));
    PerHeadProcess::set_chosen(*bundle, 1, HQ_010_02);
    CHECK(PerHeadProcess::active(*bundle));
    const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
    REQUIRE(sources.size() == 4);
    CHECK(sources[1].chosen_state == PerHeadProcess::ChosenState::Applied);
    CHECK(sources[1].derived);
    CHECK(sources[1].automatic == nullptr);
    CHECK(sources[2].reason == PerHeadProcess::Reason::Off);
    CHECK_FALSE(sources[2].derived);
    const DynamicPrintConfig composed = bundle->full_config_for_print(false);
    CHECK_FALSE(composed.equals(bundle->full_config(false)));
    CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{200., 60., 200., 200.});
}

// User presets in the picker, and the trap of an empty compatible_printers list.
TEST_CASE("A user process preset is listed for the tool heads it is made for and composes when chosen", "[PerHeadProcess][Profiles][phs_choice][phs_picker]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.2, 0.6, 0.4}, {0., 0.12, 0.30, 0.});
    add_user_process(*bundle, "My 0.2 fast", STD_012_02, 99.);
    CHECK(candidate_names(PerHeadProcess::picker_candidates(*bundle, 1), PerHeadProcess::Candidate::User) == std::vector<std::string>{"My 0.2 fast"});
    CHECK(candidate_names(PerHeadProcess::picker_candidates(*bundle, 2), PerHeadProcess::Candidate::User).empty());
    PerHeadProcess::set_chosen(*bundle, 1, "My 0.2 fast");
    std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
    REQUIRE(sources.size() == 4);
    CHECK(sources[1].chosen_state == PerHeadProcess::ChosenState::Applied);
    CHECK(floats_of(bundle->full_config_for_print(false), "outer_wall_speed") == std::vector<double>{200., 99., 120., 200.});

    // A user preset without a compatible_printers list and without a condition is compatible with
    // everything by itself; its system root (a 0.4 preset) says what it is made for.
    Preset &loose = add_user_process(*bundle, "Loose 0.4", STD_020_04, 88.);
    loose.config.option<ConfigOptionStrings>("compatible_printers", true)->values.clear();
    loose.config.option<ConfigOptionString>("compatible_printers_condition", true)->value.clear();
    std::string why;
    CHECK(PerHeadProcess::fits(*bundle, loose, 0, &why));
    CHECK(why.empty());
    CHECK_FALSE(PerHeadProcess::fits(*bundle, loose, 1, &why));
    CHECK(why == "size 0.4");
    CHECK(candidate_names(PerHeadProcess::picker_candidates(*bundle, 1), PerHeadProcess::Candidate::User) == std::vector<std::string>{"My 0.2 fast"});
    CHECK(candidate_names(PerHeadProcess::picker_candidates(*bundle, 0), PerHeadProcess::Candidate::User) == std::vector<std::string>{"Loose 0.4"});
    PerHeadProcess::set_chosen(*bundle, 1, "Loose 0.4");
    sources = PerHeadProcess::head_sources(*bundle);
    CHECK(sources[1].chosen_state == PerHeadProcess::ChosenState::Unfit);
    CHECK(sources[1].reason == PerHeadProcess::Reason::Unfit);
    CHECK(sources[1].chosen_reason == "size 0.4");
    CHECK(sources[1].preset->name == STD_012_02);
}

// A wide user preset is read at its shared column on every tool head.
TEST_CASE("A chosen user preset with values set per tool head gives every tool head its shared column", "[PerHeadProcess][PerHeadOverride][Profiles][phs_choice]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.});
    const DynamicPrintConfig &printer = bundle->printers.get_edited_preset().config;
    Preset &wide = add_user_process(*bundle, "Wide user", STD_020_04, 150.);
    PerHeadProcess::widen(wide.config, printer);
    REQUIRE(PerHeadProcess::is_wide(wide.config));
    const std::vector<int> columns = PerHeadProcess::head_columns(wide.config, 1);
    REQUIRE_FALSE(columns.empty());
    set_column(wide.config, "outer_wall_speed", size_t(columns.front()), 50.);
    PerHeadProcess::set_head_value(wide.config, 1, "outer_wall_speed", columns.front());
    PerHeadProcess::set_chosen(*bundle, 1, "Wide user");
    PerHeadProcess::set_chosen(*bundle, 2, "Wide user");
    std::vector<PerHeadProcess::Source> sources;
    const DynamicPrintConfig composed = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
    REQUIRE(sources.size() == 4);
    // Head 2's own column (50) is a value set for a head of the printer the preset was saved on, not read.
    CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{200., 150., 150., 200.});
    const int shared = PerHeadProcess::shared_column(wide.config, nvtStandard);
    CHECK(PerHeadProcess::source_column(wide, 1, nvtStandard, printer) == shared);
    CHECK(PerHeadProcess::source_column(wide, 2, nvtStandard, printer) == shared);
    CHECK(PerHeadProcess::composed_column(sources[1], nvtStandard, printer) == shared);
    CHECK_THAT(value_at(wide.config, "outer_wall_speed", size_t(shared)), Catch::Matchers::WithinAbs(150., 1e-9));
}

// The choice in the project, and what a load resets.
TEST_CASE("A chosen preset is saved with the project, loads back unchanged and never survives the load of another project", "[PerHeadProcess][Profiles][phs_choice]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.2, 0.6, 0.4}, {0., 0.12, 0.30, 0.});
    bundle->project_config.option<ConfigOptionStrings>("filament_colour", true)->values = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    PerHeadProcess::set_chosen(*bundle, 2, STD_024_06);
    const std::vector<std::string> choice = {"", "", STD_024_06};
    const std::vector<std::string> record = {"", STD_012_02, STD_024_06, ""};
    PerHeadProcess::record_sources(*bundle);
    CHECK(bundle->project_config.option<ConfigOptionStrings>(PerHeadProcess::record_key)->values == record);
    CHECK(choices_of(*bundle) == choice);
    const DynamicPrintConfig stored   = bundle->full_config_secure();
    const DynamicPrintConfig composed = bundle->full_config_for_print(false);
    CHECK(stored.option<ConfigOptionStrings>(CHOICE)->values == choice);
    CHECK(project_schema_version_for(stored) == 2);

    Semver             file_version;
    DynamicPrintConfig loaded = project_round_trip(stored, file_version);
    CHECK(loaded.option<ConfigOptionStrings>(CHOICE)->values == choice);
    CHECK(loaded.option<ConfigOptionStrings>(PerHeadProcess::record_key)->values == record);
    DynamicPrintConfig without_keys = loaded;
    without_keys.erase(CHOICE);
    without_keys.erase(PerHeadProcess::record_key);

    auto          reloaded = load_snapmaker_bundle();
    PresetBundle &second   = *reloaded;
    second.process_follows_nozzle = true;
    second.load_config_model("speed_picker.3mf", DynamicPrintConfig(loaded), file_version);
    CHECK(second.prints.get_edited_preset().name == STD_020_04);
    CHECK(choices_of(second) == choice);
    CHECK(PerHeadProcess::load_report(second).empty());
    const std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(second);
    REQUIRE(sources.size() == 4);
    CHECK(sources[2].chosen_state == PerHeadProcess::ChosenState::Applied);
    CHECK(sources[2].preset->name == STD_024_06);
    const DynamicPrintConfig recomposed = second.full_config_for_print(false);
    CHECK(floats_of(recomposed, "outer_wall_speed") == floats_of(composed, "outer_wall_speed"));
    CHECK(floats_of(recomposed, "default_acceleration") == floats_of(composed, "default_acceleration"));
    CHECK(recomposed.option<ConfigOptionInts>("print_extruder_id")->values == composed.option<ConfigOptionInts>("print_extruder_id")->values);

    // A project without the keys (saved before them, or by a build without the picker): both empty,
    // the rule applies; the previous project's choice and record are gone.
    second.load_config_model("older.3mf", DynamicPrintConfig(without_keys), file_version);
    CHECK(choices_of(second).empty());
    CHECK(second.project_config.option<ConfigOptionStrings>(PerHeadProcess::record_key)->values.empty());
    CHECK(PerHeadProcess::head_sources(second)[2].preset->name == STD_030_06);
    // The load of a project with the keys after that brings them, then a published load drops them.
    second.load_config_model("speed_picker.3mf", DynamicPrintConfig(loaded), file_version);
    CHECK(choices_of(second) == choice);
    PublishedConfig published;
    published.published = true;
    second.load_config_model("published.3mf", DynamicPrintConfig(loaded), file_version, &published);
    CHECK(choices_of(second).empty());
    CHECK(second.project_config.option<ConfigOptionStrings>(PerHeadProcess::record_key)->values.empty());
}

// A size change makes the choice dormant and a change back revives it.
TEST_CASE("A choice that no longer fits the tool head's nozzle size stays in the project as inactive and applies again when it fits", "[PerHeadProcess][Profiles][phs_choice]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.2, 0.6, 0.4}, {0., 0., 0., 0.});
    PerHeadProcess::set_chosen(*bundle, 1, STD_012_02);
    auto sizes = [&bundle](std::vector<double> values) {
        bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = std::move(values);
    };
    std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
    REQUIRE(sources.size() == 4);
    CHECK(sources[1].chosen_state == PerHeadProcess::ChosenState::Applied);

    sizes({0.4, 0.6, 0.6, 0.4});
    sources = PerHeadProcess::head_sources(*bundle);
    CHECK(sources[1].chosen_state == PerHeadProcess::ChosenState::Unfit);
    CHECK(sources[1].reason == PerHeadProcess::Reason::Unfit);
    CHECK(sources[1].chosen_reason == "size 0.2");
    CHECK(sources[1].chosen == STD_012_02);
    REQUIRE(sources[1].derived);
    CHECK(sources[1].preset->name == STD_018_06);
    CHECK(sources[1].step == PerHeadProcess::Step::SameQuality);
    CHECK(choices_of(*bundle) == std::vector<std::string>{"", STD_012_02});
    // The record names what prints; the choice is untouched by the pre-apply pass.
    PerHeadProcess::record_sources(*bundle);
    CHECK(bundle->project_config.option<ConfigOptionStrings>(PerHeadProcess::record_key)->values == std::vector<std::string>{"", STD_018_06, STD_018_06, ""});
    CHECK(choices_of(*bundle) == std::vector<std::string>{"", STD_012_02});

    sizes({0.4, 0.2, 0.6, 0.4});
    sources = PerHeadProcess::head_sources(*bundle);
    CHECK(sources[1].chosen_state == PerHeadProcess::ChosenState::Applied);
    CHECK(sources[1].preset->name == STD_012_02);

    // A size without a machine preset accepts nothing.
    sizes({0.4, 0.5, 0.6, 0.4});
    sources = PerHeadProcess::head_sources(*bundle);
    CHECK(sources[1].chosen_state == PerHeadProcess::ChosenState::Unfit);
    CHECK(sources[1].chosen_reason == "no machine preset");
    CHECK_FALSE(sources[1].derived);
    CHECK(PerHeadProcess::picker_candidates(*bundle, 1).empty());
}

// A missing and a renamed chosen preset.
TEST_CASE("A chosen preset that is not installed leaves the rule in charge and a renamed one resolves to its new name", "[PerHeadProcess][Profiles][phs_choice]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.2, 0.6, 0.4}, {0., 0., 0., 0.});
    PerHeadProcess::set_chosen(*bundle, 1, "0.12mm fast PETG @Somewhere");
    std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
    REQUIRE(sources.size() == 4);
    CHECK(sources[1].chosen_state == PerHeadProcess::ChosenState::NotInstalled);
    CHECK(sources[1].reason == PerHeadProcess::Reason::NotInstalled);
    CHECK(sources[1].chosen == "0.12mm fast PETG @Somewhere");
    REQUIRE(sources[1].derived);
    CHECK(sources[1].preset->name == STD_012_02);
    CHECK(choices_of(*bundle) == std::vector<std::string>{"", "0.12mm fast PETG @Somewhere"});
    CHECK(PerHeadProcess::resolve_chosen(*bundle, "0.12mm fast PETG @Somewhere") == nullptr);

    // The pre-"mm" spelling the vendor file names in renamed_from.
    const char *old_name = "0.12 Standard @Snapmaker U1 (0.2 nozzle)";
    const Preset *resolved = PerHeadProcess::resolve_chosen(*bundle, old_name);
    REQUIRE(resolved != nullptr);
    CHECK(resolved->name == STD_012_02);
    PerHeadProcess::set_chosen(*bundle, 1, old_name);
    sources = PerHeadProcess::head_sources(*bundle);
    CHECK(sources[1].chosen_state == PerHeadProcess::ChosenState::Applied);
    CHECK(sources[1].preset->name == STD_012_02);
    PerHeadProcess::resolve_renamed_choices(*bundle);
    CHECK(choices_of(*bundle) == std::vector<std::string>{"", STD_012_02});
}

// The candidates of the picker.
TEST_CASE("The picker lists the presets of a tool head's size with the automatic one and the flow column marked", "[PerHeadProcess][Profiles][phs_picker]")
{
    auto bundle = load_snapmaker_bundle();
    select_owner_plate(*bundle);
    using Candidate = PerHeadProcess::Candidate;
    const std::vector<Candidate> head_1 = PerHeadProcess::picker_candidates(*bundle, 0);
    CHECK(candidate_names(head_1, Candidate::System) == std::vector<std::string>{"0.08mm High Quality @Snapmaker U1 (0.2 nozzle)", HQ_010_02, STD_012_02});
    CHECK(candidate_names(head_1, Candidate::User).empty());
    for (const Candidate &candidate : head_1) {
        CHECK(candidate.is_automatic == (candidate.preset->name == HQ_010_02));
        CHECK_FALSE(candidate.is_selected);
        CHECK(candidate.has_flow_column);
    }
    const std::vector<std::string> head_4 = candidate_names(PerHeadProcess::picker_candidates(*bundle, 3), Candidate::System);
    CHECK(std::find(head_4.begin(), head_4.end(), STRENGTH_08) != head_4.end());
    CHECK(std::find(head_4.begin(), head_4.end(), STD_024_08) != head_4.end());
    // The High Flow head of the home size: the selected preset among the candidates, marked; the
    // High Flow column only 0.20mm Standard has; automatic is the High Flow rule's preset.
    const std::vector<Candidate> head_2 = PerHeadProcess::picker_candidates(*bundle, 1);
    REQUIRE(head_2.size() >= 2);
    for (const Candidate &candidate : head_2) {
        INFO(candidate.preset->name);
        CHECK(candidate.has_flow_column == (candidate.preset->name == STD_020_04));
        CHECK(candidate.is_selected == (candidate.preset->name == HQ_020_04));
        CHECK(candidate.is_automatic == (candidate.preset->name == STD_020_04));
    }
    // A bundle preset is absent, a user preset present. The collection is a deque kept sorted by
    // name: the second load inserts before "My 0.8" and re-points a reference to it, so the first
    // preset is named by its literal.
    add_user_process(*bundle, "My 0.8", STD_024_08, 70.);
    Preset &local = add_user_process(*bundle, "Local 0.8", STD_024_08, 71.);
    local.bundle_id = "abc"; // what a preset of a local or subscribed bundle carries
    REQUIRE(local.is_from_bundle());
    CHECK(candidate_names(PerHeadProcess::picker_candidates(*bundle, 3), Candidate::User) == std::vector<std::string>{"My 0.8"});
}

// What the command line does with the two keys.
TEST_CASE("The command line empties the record of a loaded project and keeps its choice", "[PerHeadProcess][phs_choice]")
{
    DynamicPrintConfig config;
    config.option<ConfigOptionStrings>(PerHeadProcess::record_key, true)->values = {"", STD_012_02, STD_024_06, ""};
    config.option<ConfigOptionStrings>(CHOICE, true)->values                     = {"", "", STD_024_06};
    const std::vector<std::string> lines = PerHeadProcess::command_line_record(config);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].find("extruder 2 printed with " + std::string(STD_012_02)) != std::string::npos);
    CHECK(lines[2].find("extruder 3 is set to take its speeds and line widths from " + std::string(STD_024_06)) != std::string::npos);
    CHECK(config.option<ConfigOptionStrings>(PerHeadProcess::record_key)->values.empty());
    CHECK(config.option<ConfigOptionStrings>(CHOICE)->values == std::vector<std::string>{"", "", STD_024_06});
    CHECK(PerHeadProcess::command_line_record(config).size() == 1);
}

// A chosen head at High Flow serves its own Standard column when the preset has no High Flow one.
TEST_CASE("A chosen preset without a High Flow column gives a High Flow tool head its own Standard values, not the selected preset's", "[PerHeadProcess][Profiles][phs_choice]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.});
    const DynamicPrintConfig &printer = bundle->printers.get_edited_preset().config;
    bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {int(nvtStandard), int(nvtHighFlow), int(nvtStandard), int(nvtStandard)};
    PerHeadProcess::set_chosen(*bundle, 1, HQ_020_04);
    std::vector<PerHeadProcess::Source> sources;
    DynamicPrintConfig composed = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
    REQUIRE(sources.size() == 4);
    CHECK(sources[1].chosen_state == PerHeadProcess::ChosenState::Applied);
    CHECK(sources[1].own_standard_variants == std::vector<int>{int(nvtHighFlow)});
    CHECK(sources[1].fallback_variants.empty());
    CHECK_FALSE(PerHeadProcess::reads_high_flow(sources[1], nvtHighFlow, printer));
    CHECK(composed.option<ConfigOptionStrings>("print_extruder_variant")->values[1] == DD_HIGH_FLOW);
    // 60 is High Quality's Standard outer wall, not the 500 of the selected preset's High Flow column.
    CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{200., 60., 200., 200.});
    CHECK(PerHeadProcess::composed_column(sources[1], nvtHighFlow, printer) == 0);

    // An automatic 0.6 mm head at High Flow keeps the fallback to the selected preset's High Flow column.
    select_u1(*bundle, {0.4, 0.4, 0.6, 0.4}, {0., 0., 0.30, 0.});
    Test::u1_0_6_declares_high_flow(*bundle);
    bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {int(nvtStandard), int(nvtStandard), int(nvtHighFlow), int(nvtStandard)};
    PerHeadProcess::set_chosen(*bundle, 1, std::string());
    composed = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
    REQUIRE(sources.size() == 4);
    CHECK(sources[2].chosen_state == PerHeadProcess::ChosenState::None);
    CHECK(sources[2].fallback_variants == std::vector<int>{int(nvtHighFlow)});
    CHECK(PerHeadProcess::composed_column(sources[2], nvtHighFlow, printer) == -1);
    CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{200., 200., 500., 200.});
    // The same head with the 0.6 preset chosen by name: its own Standard column, 120.
    PerHeadProcess::set_chosen(*bundle, 2, STD_024_06);
    composed = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
    CHECK(sources[2].own_standard_variants == std::vector<int>{int(nvtHighFlow)});
    CHECK(floats_of(composed, "outer_wall_speed") == std::vector<double>{200., 200., 120., 200.});
}

// A choice under a detached selected preset.
TEST_CASE("A choice stays inactive while the selected preset has no system parent and applies once it has one again", "[PerHeadProcess][Profiles][phs_choice]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.2, 0.6, 0.4}, {0., 0., 0., 0.});
    PerHeadProcess::set_chosen(*bundle, 1, HQ_010_02);
    DynamicPrintConfig detached = bundle->prints.get_selected_preset().config;
    detached.option<ConfigOptionString>("inherits", true)->value = "";
    Preset &user = bundle->prints.load_preset(std::string(), "My detached process", std::move(detached), /*select=*/true);
    user.is_visible = true;
    std::vector<PerHeadProcess::Source> sources = PerHeadProcess::head_sources(*bundle);
    REQUIRE(sources.size() == 4);
    CHECK(sources[1].chosen_state == PerHeadProcess::ChosenState::Inactive);
    CHECK(sources[1].reason == PerHeadProcess::Reason::NoParent);
    CHECK(sources[1].chosen_reason == "no parent");
    CHECK_FALSE(sources[1].derived);
    CHECK(choices_of(*bundle) == std::vector<std::string>{"", HQ_010_02});
    CHECK(bundle->full_config_for_print(false).equals(bundle->full_config(false)));

    REQUIRE(bundle->prints.select_preset_by_name(STD_020_04, true));
    sources = PerHeadProcess::head_sources(*bundle);
    CHECK(sources[1].chosen_state == PerHeadProcess::ChosenState::Applied);
    CHECK(sources[1].preset->name == HQ_010_02);
}

// ---- The chosen flow: a High Flow tool head printing the Standard speeds (PerHeadProcess::flow_key) ----

namespace {

const char *const FLOW = PerHeadProcess::flow_key;

std::vector<std::string> flows_of(const PresetBundle &bundle)
{
    const auto *option = bundle.project_config.option<ConfigOptionStrings>(FLOW);
    return option == nullptr ? std::vector<std::string>() : option->values;
}

} // namespace

TEST_CASE("A High Flow tool head set to the Standard speeds prints the selected preset's Standard column on that head alone", "[PerHeadProcess][Profiles][phs_flow_choice]")
{
    auto bundle = load_snapmaker_bundle();
    select_owner_plate(*bundle); // 0.2 / 0.4 High Flow / 0.6 / 0.8, 0.20mm High Quality (one Standard column)
    const DynamicPrintConfig &printer  = bundle->printers.get_edited_preset().config;
    const Preset             &selected = bundle->prints.get_selected_preset();
    // Without an entry the head prints its nozzle's flow: the High Flow rule gives it the High Flow
    // column of 0.20mm Standard (500 mm/s).
    const DynamicPrintConfig plain = bundle->full_config_for_print(false);
    CHECK(PerHeadProcess::nozzle_flow(bundle->project_config, 1) == nvtHighFlow);
    CHECK(PerHeadProcess::effective_flow(*bundle, 1) == nvtHighFlow);
    CHECK_FALSE(PerHeadProcess::flow_chosen(*bundle, 1));
    CHECK_FALSE(PerHeadProcess::any_flow_chosen(bundle->project_config));
    CHECK(value_at(plain, "outer_wall_speed", 1) == 500.);

    PerHeadProcess::set_chosen_flow(*bundle, 1, nvtStandard);
    CHECK(flows_of(*bundle) == std::vector<std::string>{"", "Standard"});
    CHECK(PerHeadProcess::chosen_flow_of(*bundle, 1) == "Standard");
    CHECK(PerHeadProcess::effective_flow(*bundle, 1) == nvtStandard);
    CHECK(PerHeadProcess::flow_chosen(*bundle, 1));
    CHECK(PerHeadProcess::any_flow_chosen(bundle->project_config));
    // The nozzle stays High Flow: the filament side follows it.
    CHECK(PerHeadProcess::nozzle_flow(bundle->project_config, 1) == nvtHighFlow);

    std::vector<PerHeadProcess::Source> sources;
    const DynamicPrintConfig composed = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
    REQUIRE(sources.size() == 4);
    // Treated like a Standard head of the home size: the High Flow rule does not apply, the head
    // prints the selected preset.
    CHECK(sources[1].flow == nvtStandard);
    CHECK(sources[1].flow_chosen);
    CHECK_FALSE(sources[1].derived);
    CHECK(sources[1].reason == PerHeadProcess::Reason::HomeSize);
    CHECK(sources[1].step == PerHeadProcess::Step::SelectedPreset);
    REQUIRE(sources[1].preset != nullptr);
    CHECK(sources[1].preset->name == HQ_020_04);
    CHECK_FALSE(PerHeadProcess::reads_high_flow(sources[1], sources[1].flow, printer));
    // The slot stays the nozzle's (Print::apply narrows by the nozzle's flow) and holds the selected
    // preset's Standard values.
    REQUIRE(composed.option<ConfigOptionStrings>("print_extruder_variant")->values.size() == 4);
    CHECK(composed.option<ConfigOptionStrings>("print_extruder_variant")->values[1] == DD_HIGH_FLOW);
    for (const std::string &key : headline_keys()) {
        INFO(key);
        CHECK_THAT(value_at(composed, key, 1), Catch::Matchers::WithinAbs(value_at(selected.config, key, 0), 1e-6));
    }
    CHECK(value_at(composed, "outer_wall_speed", 1) == 60.);
    // Every other tool head prints as without the entry.
    for (size_t head : {size_t(0), size_t(2), size_t(3)}) {
        CHECK(sources[head].flow == nvtStandard);
        CHECK_FALSE(sources[head].flow_chosen);
        for (const std::string &key : headline_keys()) {
            INFO("tool head " << head + 1 << ", " << key);
            CHECK_THAT(value_at(composed, key, head), Catch::Matchers::WithinAbs(value_at(plain, key, head), 1e-6));
        }
    }
    // The record names the derived heads alone: a chosen flow is no preset.
    PerHeadProcess::record_sources(*bundle);
    const auto *record = bundle->project_config.option<ConfigOptionStrings>(PerHeadProcess::record_key);
    REQUIRE(record != nullptr);
    REQUIRE(record->values.size() == 4);
    CHECK(record->values[1].empty());
    CHECK(record->values[2] == STD_018_06);
    CHECK(PerHeadProcess::load_report(*bundle).empty());

    SECTION("a value set for the tool head beats the chosen flow") {
        DynamicPrintConfig &process = bundle->prints.get_edited_preset().config;
        PerHeadProcess::widen(process, printer);
        const std::vector<int> columns = PerHeadProcess::head_columns(process, 1);
        REQUIRE_FALSE(columns.empty());
        set_column(process, "outer_wall_speed", size_t(columns.front()), 77.);
        PerHeadProcess::set_head_value(process, 1, "outer_wall_speed", columns.front());
        const DynamicPrintConfig with_value = bundle->full_config_for_print(false);
        CHECK(value_at(with_value, "outer_wall_speed", 1) == 77.);
        CHECK(value_at(with_value, "sparse_infill_speed", 1) == value_at(selected.config, "sparse_infill_speed", 0));
    }

    SECTION("a chosen preset serves its Standard column to the head, and its High Flow column once the entry is cleared") {
        PerHeadProcess::set_chosen(*bundle, 1, STD_020_04);
        const Preset *standard = bundle->prints.find_preset(STD_020_04, false);
        REQUIRE(standard != nullptr);
        DynamicPrintConfig chosen = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
        REQUIRE(sources.size() == 4);
        CHECK(sources[1].step == PerHeadProcess::Step::Chosen);
        CHECK(sources[1].flow_chosen);
        CHECK(sources[1].own_standard_variants.empty());
        CHECK(value_at(chosen, "outer_wall_speed", 1) == value_at(standard->config, "outer_wall_speed", size_t(flow_column(*standard, nvtStandard))));
        CHECK(value_at(chosen, "outer_wall_speed", 1) == 200.);
        // The toggle back to High Flow: the nozzle's own flow clears the entry.
        PerHeadProcess::set_chosen_flow(*bundle, 1, nvtHighFlow);
        CHECK(flows_of(*bundle).empty());
        CHECK_FALSE(PerHeadProcess::flow_chosen(*bundle, 1));
        chosen = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
        REQUIRE(sources.size() == 4);
        CHECK_FALSE(sources[1].flow_chosen);
        CHECK(value_at(chosen, "outer_wall_speed", 1) == value_at(standard->config, "outer_wall_speed", size_t(flow_column(*standard, nvtHighFlow))));
        CHECK(value_at(chosen, "outer_wall_speed", 1) == 500.);
    }

    SECTION("the entry applies with the preference off, while the other tool heads print the selected preset") {
        bundle->process_follows_nozzle = false;
        CHECK(PerHeadProcess::active(*bundle));
        const DynamicPrintConfig off = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
        REQUIRE(sources.size() == 4);
        CHECK(sources[2].reason == PerHeadProcess::Reason::Off);
        CHECK_FALSE(sources[2].derived);
        CHECK(sources[1].flow_chosen);
        CHECK(sources[1].reason == PerHeadProcess::Reason::Off);
        CHECK(value_at(off, "outer_wall_speed", 1) == 60.);
        CHECK(value_at(off, "outer_wall_speed", 2) == 60.);
        CHECK_FALSE(off.equals(bundle->full_config(false)));
    }
}

TEST_CASE("A chosen flow equal to the nozzle's own is no entry, an entry the nozzle does not offer stays dormant, and a dormant entry changes nothing", "[PerHeadProcess][Profiles][phs_flow_choice]")
{
    auto bundle = load_snapmaker_bundle();
    select_owner_plate(*bundle);
    std::vector<int> &nozzles = bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values;
    // High Flow on the High Flow nozzle, Standard on a Standard nozzle: nothing is stored.
    PerHeadProcess::set_chosen_flow(*bundle, 1, nvtHighFlow);
    CHECK(flows_of(*bundle).empty());
    PerHeadProcess::set_chosen_flow(*bundle, 3, nvtStandard);
    CHECK(flows_of(*bundle).empty());
    CHECK_FALSE(PerHeadProcess::any_flow_chosen(bundle->project_config));

    // "High Flow" written for a Standard nozzle (a file) is not read: the nozzle offers no High Flow
    // speeds. The composed table is the one without the entry, byte for byte.
    const DynamicPrintConfig without = bundle->full_config_for_print(false);
    bundle->project_config.option<ConfigOptionStrings>(FLOW, true)->values = {"", "", "", "High Flow"};
    CHECK(PerHeadProcess::effective_flow(*bundle, 3) == nvtStandard);
    CHECK_FALSE(PerHeadProcess::flow_chosen(*bundle, 3));
    CHECK_FALSE(PerHeadProcess::any_flow_chosen(bundle->project_config));
    CHECK(PerHeadProcess::chosen_flow_of(*bundle, 3) == "High Flow");
    std::vector<PerHeadProcess::Source> sources;
    DynamicPrintConfig with = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
    REQUIRE(sources.size() == 4);
    CHECK_FALSE(sources[3].flow_chosen);
    CHECK(sources[3].flow == nvtStandard);
    REQUIRE(with.option<ConfigOptionStrings>(FLOW) != nullptr);
    CHECK(with.option<ConfigOptionStrings>(FLOW)->values == std::vector<std::string>{"", "", "", "High Flow"});
    DynamicPrintConfig reference = without;
    with.erase(FLOW);
    reference.erase(FLOW);
    CHECK(with.equals(reference));
    // The nozzle turns High Flow: High Flow is its own flow, still no choice in effect.
    nozzles[3] = int(nvtHighFlow);
    CHECK(PerHeadProcess::effective_flow(*bundle, 3) == nvtHighFlow);
    CHECK_FALSE(PerHeadProcess::flow_chosen(*bundle, 3));
    nozzles[3] = int(nvtStandard);

    // An entry naming no flow type is not read.
    bundle->project_config.option<ConfigOptionStrings>(FLOW, true)->values = {"", "Hybrid"};
    CHECK(PerHeadProcess::effective_flow(*bundle, 1) == nvtHighFlow);
    CHECK_FALSE(PerHeadProcess::flow_chosen(*bundle, 1));

    // Standard chosen for the High Flow head 2, then the nozzle turns Standard: the entry is dormant
    // (Standard is the nozzle's own flow), and in effect again once the nozzle is High Flow again.
    PerHeadProcess::set_chosen_flow(*bundle, 1, nvtStandard);
    CHECK(flows_of(*bundle) == std::vector<std::string>{"", "Standard"});
    CHECK(PerHeadProcess::flow_chosen(*bundle, 1));
    nozzles[1] = int(nvtStandard);
    CHECK(PerHeadProcess::effective_flow(*bundle, 1) == nvtStandard);
    CHECK_FALSE(PerHeadProcess::flow_chosen(*bundle, 1));
    CHECK(PerHeadProcess::chosen_flow_of(*bundle, 1) == "Standard");
    CHECK_FALSE(PerHeadProcess::any_flow_chosen(bundle->project_config));
    CHECK(value_at(bundle->full_config_for_print(false), "outer_wall_speed", 1) == 60.);
    nozzles[1] = int(nvtHighFlow);
    CHECK(PerHeadProcess::flow_chosen(*bundle, 1));
    CHECK(PerHeadProcess::any_flow_chosen(bundle->project_config));
    CHECK(value_at(bundle->full_config_for_print(false), "outer_wall_speed", 1) == 60.);
    // set_chosen_flow grows and never shrinks; every entry empty reduces to [].
    PerHeadProcess::set_chosen_flow(*bundle, 1, nvtHighFlow);
    CHECK(flows_of(*bundle).empty());
    CHECK(value_at(bundle->full_config_for_print(false), "outer_wall_speed", 1) == 500.);
}

TEST_CASE("A chosen flow is saved with the project, loads back unchanged and is reset by a load without it", "[PerHeadProcess][Profiles][phs_flow_choice]")
{
    auto bundle = load_snapmaker_bundle();
    select_owner_plate(*bundle);
    bundle->project_config.option<ConfigOptionStrings>("filament_colour", true)->values = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    PerHeadProcess::set_chosen_flow(*bundle, 1, nvtStandard);
    const std::vector<std::string> flows    = {"", "Standard"};
    const DynamicPrintConfig       stored   = bundle->full_config_secure();
    const DynamicPrintConfig       composed = bundle->full_config_for_print(false);
    REQUIRE(stored.option<ConfigOptionStrings>(FLOW) != nullptr);
    CHECK(stored.option<ConfigOptionStrings>(FLOW)->values == flows);
    CHECK(value_at(composed, "outer_wall_speed", 1) == 60.);

    Semver             file_version;
    DynamicPrintConfig loaded = project_round_trip(stored, file_version);
    REQUIRE(loaded.option<ConfigOptionStrings>(FLOW) != nullptr);
    CHECK(loaded.option<ConfigOptionStrings>(FLOW)->values == flows);
    DynamicPrintConfig without_key = loaded;
    without_key.erase(FLOW);

    auto          reloaded = load_snapmaker_bundle();
    PresetBundle &second   = *reloaded;
    second.process_follows_nozzle = true;
    second.load_config_model("flow_choice.3mf", DynamicPrintConfig(loaded), file_version);
    CHECK(second.prints.get_edited_preset().name == HQ_020_04);
    CHECK(flows_of(second) == flows);
    CHECK(PerHeadProcess::flow_chosen(second, 1));
    const DynamicPrintConfig recomposed = second.full_config_for_print(false);
    CHECK(floats_of(recomposed, "outer_wall_speed") == floats_of(composed, "outer_wall_speed"));
    CHECK(value_at(recomposed, "outer_wall_speed", 1) == 60.);

    // A project without the key (saved before it, or by a build without the toggle): the entry is
    // gone, the head prints its nozzle's flow again. A published load drops it too.
    second.load_config_model("older.3mf", DynamicPrintConfig(without_key), file_version);
    CHECK(flows_of(second).empty());
    CHECK_FALSE(PerHeadProcess::flow_chosen(second, 1));
    CHECK(value_at(second.full_config_for_print(false), "outer_wall_speed", 1) == 500.);
    second.load_config_model("flow_choice.3mf", DynamicPrintConfig(loaded), file_version);
    CHECK(flows_of(second) == flows);
    PublishedConfig published;
    published.published = true;
    second.load_config_model("published.3mf", DynamicPrintConfig(loaded), file_version, &published);
    CHECK(flows_of(second).empty());
}

TEST_CASE("The command line logs a chosen flow and keeps it", "[PerHeadProcess][phs_flow_choice]")
{
    DynamicPrintConfig config;
    config.option<ConfigOptionStrings>(FLOW, true)->values = {"", "Standard"};
    const std::vector<std::string> lines = PerHeadProcess::command_line_record(config);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("extruder 2 is set to print the Standard speeds") != std::string::npos);
    CHECK(config.option<ConfigOptionStrings>(FLOW)->values == std::vector<std::string>{"", "Standard"});
}


// ---- Line widths per tool head: the width source of the composer ----

namespace {


// The text of column `column` of a key ("105%", "0.42").
std::string text_at(const DynamicPrintConfig &config, const std::string &key, size_t column)
{
    const auto *option = dynamic_cast<const ConfigOptionVectorBase *>(config.option(key));
    REQUIRE(option != nullptr);
    const std::vector<std::string> values = option->vserialize();
    REQUIRE(column < values.size());
    return values[column];
}

// The text of a preset's Standard shared column of a key.
std::string preset_text(const Preset &preset, const std::string &key)
{
    return text_at(preset.config, key, size_t(PerHeadProcess::shared_column(preset.config, nvtStandard)));
}

// Writes the text of a value into column `column` of a key.
void set_column_text(DynamicPrintConfig &config, const std::string &key, size_t column, const std::string &value)
{
    auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
    REQUIRE(option != nullptr);
    const std::unique_ptr<ConfigOption> parsed(option->clone());
    REQUIRE(parsed->deserialize(value));
    option->set_at(parsed.get(), column, 0);
}

const std::vector<std::string> &width_keys()
{
    static const std::vector<std::string> keys(PerHeadProcess::flow_independent_keys().begin(), PerHeadProcess::flow_independent_keys().end());
    return keys;
}

} // namespace

TEST_CASE("A tool head of another size prints the line widths of the preset of its size; a High Flow head keeps the widths of its size preset", "[PerHeadProcess][PerHeadWidth][Profiles][phw_source]")
{
    auto bundle = load_snapmaker_bundle();
    select_owner_plate(*bundle); // 0.2 / 0.4 High Flow / 0.6 / 0.8 under 0.20mm High Quality (0.4)
    const Preset &selected = bundle->prints.get_selected_preset();
    std::vector<PerHeadProcess::Source> sources;
    const DynamicPrintConfig composed = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
    REQUIRE(sources.size() == 4);
    REQUIRE(composed.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{1, 2, 3, 4});

    // Tool head 2: the High Flow rule re-picks the speeds to 0.20mm Standard's High Flow column, the
    // widths stay the selected preset's (a home-size head has no size preset).
    CHECK(sources[1].reason == PerHeadProcess::Reason::HighFlow);
    CHECK(sources[1].size_preset == nullptr);
    CHECK(PerHeadProcess::width_source(sources[1]) == nullptr);
    CHECK(value_at(composed, "outer_wall_speed", 1) == 500.);
    for (const std::string &key : width_keys()) {
        INFO(key);
        CHECK(text_at(composed, key, 1) == preset_text(selected, key));
    }
    CHECK(text_at(composed, "top_surface_line_width", 1) == "105%");
    CHECK(text_at(composed, "sparse_infill_line_width", 1) == "112.5%");

    // Tool heads 1, 3 and 4: the widths of the preset of their size, the size preset being the source.
    for (size_t head : {size_t(0), size_t(2), size_t(3)}) {
        REQUIRE(sources[head].derived);
        REQUIRE(sources[head].preset != nullptr);
        CHECK(sources[head].size_preset == sources[head].preset);
        CHECK(PerHeadProcess::width_source(sources[head]) == sources[head].preset);
        for (const std::string &key : width_keys()) {
            INFO("tool head " << head + 1 << ", " << key);
            CHECK(text_at(composed, key, head) == preset_text(*sources[head].preset, key));
        }
    }
    CHECK(text_at(composed, "line_width", 0) == "110%");
    CHECK(text_at(composed, "line_width", 2) == "103.33%");
    CHECK(text_at(composed, "line_width", 3) == "102.5%");
    CHECK(text_at(composed, "initial_layer_line_width", 0) == "125%");

    // composed_column_for_key: a width reads the width source's Standard shared column, a speed the source column.
    const DynamicPrintConfig &printer = bundle->printers.get_edited_preset().config;
    const Preset *from = nullptr;
    CHECK(PerHeadProcess::composed_column_for_key(sources[2], "line_width", nvtStandard, printer, from) == PerHeadProcess::shared_column(sources[2].preset->config, nvtStandard));
    CHECK(from == sources[2].preset);
    CHECK(PerHeadProcess::composed_column_for_key(sources[1], "line_width", nvtHighFlow, printer, from) == -1);
    CHECK(from == nullptr);
    CHECK(PerHeadProcess::composed_column_for_key(sources[1], "outer_wall_speed", nvtHighFlow, printer, from) == PerHeadProcess::composed_column(sources[1], nvtHighFlow, printer));
    CHECK(from == sources[1].preset);

    SECTION("a preset chosen for the 0.6 mm head supplies its widths and its speeds") {
        PerHeadProcess::set_chosen(*bundle, 2, STD_024_06);
        const Preset *chosen = bundle->prints.find_preset(STD_024_06, false);
        REQUIRE(chosen != nullptr);
        const DynamicPrintConfig with_choice = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
        REQUIRE(sources.size() == 4);
        CHECK(sources[2].step == PerHeadProcess::Step::Chosen);
        CHECK(PerHeadProcess::width_source(sources[2]) == chosen);
        CHECK(text_at(with_choice, "line_width", 2) == preset_text(*chosen, "line_width"));
        CHECK(text_at(with_choice, "line_width", 2) == "103.33%");
        CHECK(value_at(with_choice, "sparse_infill_speed", 2) == 150.);
    }

    SECTION("a width changed under All tool heads applies to every tool head") {
        bundle->prints.get_edited_preset().config.set_key_value("line_width", new ConfigOptionFloatsOrPercentsNullable{FloatOrPercent(110., true)});
        CHECK(PerHeadProcess::all_edited_keys(*bundle).count("line_width") == 1);
        const DynamicPrintConfig edited = bundle->full_config_for_print(false);
        for (size_t head = 0; head < 4; ++head) {
            INFO("tool head " << head + 1);
            CHECK(text_at(edited, "line_width", head) == "110%");
        }
        // The other widths still follow the size presets.
        CHECK(text_at(edited, "outer_wall_line_width", 3) == "102.5%");
    }

    SECTION("a width set for one tool head beats the preset of its size") {
        DynamicPrintConfig &process = bundle->prints.get_edited_preset().config;
        PerHeadProcess::widen(process, printer);
        const std::vector<int> columns = PerHeadProcess::head_columns(process, 3);
        REQUIRE_FALSE(columns.empty());
        set_column_text(process, "sparse_infill_line_width", size_t(columns.front()), "0.9");
        PerHeadProcess::set_head_value(process, 3, "sparse_infill_line_width", columns.front());
        const DynamicPrintConfig with_value = bundle->full_config_for_print(false);
        CHECK(text_at(with_value, "sparse_infill_line_width", 3) == "0.9");
        CHECK(text_at(with_value, "sparse_infill_line_width", 2) == "103.33%");
        CHECK(text_at(with_value, "outer_wall_line_width", 3) == "102.5%");
    }
}

TEST_CASE("A 0.6 mm High Flow head takes its widths from the preset of its size, not from the High Flow sibling the speeds come from", "[PerHeadProcess][PerHeadWidth][Profiles][phw_source]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.4, 0.6, 0.4}, {0., 0., 0., 0.}, "0.24mm Standard @Snapmaker U1 (0.4 nozzle)");
    bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {int(nvtStandard), int(nvtStandard), int(nvtHighFlow), int(nvtStandard)};
    std::vector<PerHeadProcess::Source> sources;
    const DynamicPrintConfig composed = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
    REQUIRE(sources.size() == 4);
    REQUIRE(sources[2].derived);
    REQUIRE(sources[2].size_preset != nullptr);
    // The size rule gives 0.24mm Standard (0.6) (the U1 matrix); whether or not the High Flow rule
    // re-picked the speeds source, the widths come from that preset.
    CHECK(sources[2].size_preset->name == STD_024_06);
    REQUIRE(PerHeadProcess::width_source(sources[2]) != nullptr);
    CHECK(PerHeadProcess::width_source(sources[2])->name == STD_024_06);
    for (const std::string &key : width_keys()) {
        INFO(key);
        CHECK(text_at(composed, key, 2) == preset_text(*PerHeadProcess::width_source(sources[2]), key));
    }
    CHECK(text_at(composed, "line_width", 2) == "103.33%");
    CHECK(text_at(composed, "initial_layer_line_width", 2) == "103.33%");
}

// A plate of one nozzle size never changes its widths through the High Flow rule.
TEST_CASE("On an all-0.4 plate the High Flow head keeps the selected preset's widths while its speeds come from the sibling with a High Flow column", "[PerHeadProcess][PerHeadWidth][Profiles][phw_source]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.}, "0.24mm Standard @Snapmaker U1 (0.4 nozzle)");
    bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {int(nvtStandard), int(nvtHighFlow), int(nvtStandard), int(nvtStandard)};
    const Preset &selected = bundle->prints.get_selected_preset();
    CHECK(preset_text(selected, "top_surface_line_width") == "112.5%");
    std::vector<PerHeadProcess::Source> sources;
    const DynamicPrintConfig composed = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
    if (sources.empty()) {
        // Nothing composed: the selected preset has a High Flow column of its own for the head.
        CHECK(PerHeadProcess::has_high_flow_values(selected.config, 1, bundle->printers.get_edited_preset().config));
        return;
    }
    REQUIRE(sources.size() == 4);
    CHECK(PerHeadProcess::width_source(sources[1]) == nullptr);
    CHECK(sources[1].size_preset == nullptr);
    const auto *ids = composed.option<ConfigOptionInts>("print_extruder_id");
    REQUIRE(ids != nullptr);
    for (size_t column = 0; column < ids->values.size(); ++column)
        for (const std::string &key : width_keys()) {
            INFO("column " << column << ", " << key);
            CHECK(text_at(composed, key, column) == preset_text(selected, key));
        }
    // With the Standard speeds chosen for the High Flow head likewise.
    PerHeadProcess::set_chosen_flow(*bundle, 1, nvtStandard);
    const DynamicPrintConfig standard = bundle->full_config_for_print(false, std::nullopt, std::nullopt, &sources);
    if (const auto *standard_ids = standard.option<ConfigOptionInts>("print_extruder_id"); standard_ids != nullptr)
        for (size_t column = 0; column < standard_ids->values.size(); ++column)
            CHECK(text_at(standard, "top_surface_line_width", column) == "112.5%");
}

// Round trips of a width set per tool head, and the scalar spelling without one.
TEST_CASE("A project stores a line width set for a tool head as a full array without nil and a project without one as a string", "[PerHeadProcess][PerHeadWidth][Profiles][phw_round_trip]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.});
    bundle->project_config.option<ConfigOptionStrings>("filament_colour", true)->values = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    auto json_of = [](const DynamicPrintConfig &config) {
        std::ostringstream os;
        config.save_to_json(os, "project", "Project", "1.0.0.0");
        return nlohmann::json::parse(os.str());
    };

    SECTION("without a width set per tool head every width is one string") {
        const DynamicPrintConfig stored = bundle->full_config_secure();
        const nlohmann::json     j      = json_of(stored);
        for (const std::string &key : width_keys()) {
            INFO(key);
            REQUIRE(j.contains(key));
            CHECK(j[key].is_string());
        }
        CHECK(j["line_width"].get<std::string>() == preset_text(bundle->prints.get_selected_preset(), "line_width"));
    }

    SECTION("a width set for tool head 4 survives the round trip in both of its columns, written without nil") {
        DynamicPrintConfig &edited = bundle->prints.get_edited_preset().config;
        PerHeadProcess::widen(edited, bundle->printers.get_edited_preset().config);
        REQUIRE(ints_of(edited, "print_extruder_id") == U1_WIDE_IDS);
        set_column_text(edited, "sparse_infill_line_width", 8, "0.9");
        PerHeadProcess::set_head_value(edited, 3, "sparse_infill_line_width", 8);

        const DynamicPrintConfig stored = bundle->full_config_secure();
        const nlohmann::json     j      = json_of(stored);
        REQUIRE(j["sparse_infill_line_width"].is_array());
        REQUIRE(j["sparse_infill_line_width"].size() == 10);
        for (const auto &entry : j["sparse_infill_line_width"])
            CHECK(entry.get<std::string>() != "nil");
        CHECK(j["sparse_infill_line_width"][0].get<std::string>() == "112.5%");
        CHECK(j["sparse_infill_line_width"][8].get<std::string>() == "0.9");
        CHECK(j["sparse_infill_line_width"][9].get<std::string>() == "0.9");
        // An untouched width stays one string.
        CHECK(j["line_width"].is_string());
        CHECK(strings_of(stored, OVERRIDE)[8] == "sparse_infill_line_width");

        auto          reloaded = load_snapmaker_bundle();
        PresetBundle &second   = *reloaded;
        DynamicPrintConfig project = stored;
        Preset::normalize(project);
        second.load_config_model("per_head_width.3mf", std::move(project), Semver());
        const DynamicPrintConfig &loaded = second.prints.get_edited_preset().config;
        CHECK(ints_of(loaded, "print_extruder_id") == U1_WIDE_IDS);
        CHECK(text_at(loaded, "sparse_infill_line_width", 8) == "0.9");
        CHECK(text_at(loaded, "sparse_infill_line_width", 9) == "0.9");
        CHECK(text_at(loaded, "sparse_infill_line_width", 0) == "112.5%");
        CHECK(text_at(loaded, "sparse_infill_line_width", 6) == "112.5%");
        CHECK(strings_of(loaded, OVERRIDE) == strings_of(edited, OVERRIDE));
        check_invariants(loaded);
    }
}

TEST_CASE("A user process preset with a line width set for a tool head is saved without nil and reloads with the marker", "[PerHeadProcess][PerHeadWidth][Profiles][phw_round_trip]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.});
    Preset *parent = bundle->prints.find_preset(STD_020_04, false, true);
    REQUIRE(parent != nullptr);
    const DynamicPrintConfig &printer = bundle->printers.get_edited_preset().config;

    ScopedTemporaryDir temp_dir("orca_per_head_width");
    const std::string  name = "Wide sparse @Snapmaker U1 (0.4 nozzle)";
    std::string        file;
    {
        Preset child(Preset::TYPE_PRINT, name, false);
        child.config     = parent->config;
        child.version    = parent->version;
        child.inherits() = parent->name;
        child.file       = (temp_dir.path() / PRESET_PRINT_NAME / (name + ".json")).string();
        file             = child.file;
        PerHeadProcess::widen(child.config, printer);
        set_column_text(child.config, "sparse_infill_line_width", 6, "0.9");
        PerHeadProcess::set_head_value(child.config, 2, "sparse_infill_line_width", 6);
        child.save(&parent->config);
        REQUIRE(boost::filesystem::exists(child.file));
    }
    {
        boost::nowide::ifstream in(file);
        const nlohmann::json    j = nlohmann::json::parse(in);
        REQUIRE(j.contains("sparse_infill_line_width"));
        REQUIRE(j["sparse_infill_line_width"].is_array());
        REQUIRE(j["sparse_infill_line_width"].size() == 10);
        for (const auto &entry : j["sparse_infill_line_width"])
            CHECK(entry.get<std::string>() != "nil");
        CHECK(j["sparse_infill_line_width"][0].get<std::string>() == "112.5%");
        CHECK(j["sparse_infill_line_width"][6].get<std::string>() == "0.9");
        // An older reader takes the first number and the percent flag: the shared value.
        ConfigOptionFloatOrPercent old;
        std::string joined;
        for (const auto &entry : j["sparse_infill_line_width"])
            joined += (joined.empty() ? "" : ",") + entry.get<std::string>();
        REQUIRE(old.deserialize(joined));
        CHECK(old.percent);
        CHECK(old.value == 112.5);
    }
    PresetsConfigSubstitutions substitutions;
    bundle->prints.load_presets(temp_dir.path().string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);
    const Preset *loaded = bundle->prints.find_preset(name, false);
    REQUIRE(loaded != nullptr);
    CHECK(ints_of(loaded->config, "print_extruder_id") == U1_WIDE_IDS);
    CHECK(text_at(loaded->config, "sparse_infill_line_width", 6) == "0.9");
    CHECK(text_at(loaded->config, "sparse_infill_line_width", 7) == "0.9");
    CHECK(text_at(loaded->config, "sparse_infill_line_width", 0) == "112.5%");
    CHECK(strings_of(loaded->config, OVERRIDE)[6] == "sparse_infill_line_width");
    check_invariants(loaded->config);
}

TEST_CASE("A line width is flow-independent: an edit under All fills every shared column and normalise equalises differing flow columns", "[PerHeadProcess][PerHeadWidth][phw_flow_independent]")
{
    DynamicPrintConfig config = flow_only_process();
    config.option<ConfigOptionFloatsOrPercentsNullable>("line_width", true)->values = {FloatOrPercent(105., true), FloatOrPercent(105., true)};
    // Four heads that declare Standard and High Flow: two columns per head in the wide layout.
    const DynamicPrintConfig printer = four_head_printer(std::vector<NozzleVolumeType>(4, nvtStandard), false);
    PerHeadProcess::widen(config, printer);
    REQUIRE(PerHeadProcess::is_wide(config));
    const PerHeadProcess::Layout layout = PerHeadProcess::layout_of(config);
    REQUIRE(layout.ids == U1_WIDE_IDS);

    SECTION("set_shared_value writes the width into both shared columns and every unmarked head column") {
        set_column_text(config, "line_width", 0, "0.5");
        PerHeadProcess::set_shared_value(config, "line_width", nvtStandard);
        for (size_t column = 0; column < layout.size(); ++column) {
            INFO("column " << column);
            CHECK(text_at(config, "line_width", column) == "0.5");
        }
        // A speed keeps its flow: the Standard shared column fills the Standard head columns alone.
        set_column(config, "outer_wall_speed", 0, 175.);
        PerHeadProcess::set_shared_value(config, "outer_wall_speed", nvtStandard);
        for (size_t column = 0; column < layout.size(); ++column) {
            INFO("column " << column);
            CHECK(floats_of(config, "outer_wall_speed")[column] == (PerHeadProcess::variant_names_type(layout.variants[column], nvtHighFlow) ? 500. : 175.));
        }
    }

    SECTION("normalise takes the Standard column into a differing High Flow column, shared and per head") {
        // A speed set for tool head 1 keeps the layout wide through normalise (a wide layout whose
        // marker names nothing is narrowed, rule I1 of the selector); the width is then read on
        // every column.
        set_column(config, "outer_wall_speed", 2, 175.);
        PerHeadProcess::set_head_value(config, 0, "outer_wall_speed", 2);
        set_column_text(config, "line_width", 1, "0.7"); // the High Flow shared column
        set_column_text(config, "line_width", 3, "0.7"); // the High Flow column of tool head 1
        PerHeadProcess::normalise(config, nullptr, &printer);
        REQUIRE(PerHeadProcess::is_wide(config));
        REQUIRE(PerHeadProcess::layout_of(config).size() == layout.size());
        for (size_t column = 0; column < layout.size(); ++column) {
            INFO("column " << column);
            CHECK(text_at(config, "line_width", column) == "105%");
        }
    }

    SECTION("without a value set per tool head normalise equalises the flow columns and narrows the layout") {
        set_column_text(config, "line_width", 1, "0.7");
        set_column_text(config, "line_width", 3, "0.7");
        PerHeadProcess::normalise(config, nullptr, &printer);
        CHECK_FALSE(PerHeadProcess::is_wide(config));
        const PerHeadProcess::Layout narrowed = PerHeadProcess::layout_of(config);
        REQUIRE(narrowed.size() == 2);
        for (size_t column = 0; column < narrowed.size(); ++column) {
            INFO("column " << column);
            CHECK(text_at(config, "line_width", column) == "105%");
        }
    }
}

// The clear link of one Process tab page: under a tool head the Quality page clears the head's line
// widths and keeps its speeds, the Speed page the reverse; clear_head without a key set clears all.
TEST_CASE("Clearing a tool head for the keys of one page leaves its values of the other page", "[PerHeadProcess][PerHeadWidth][phw_clear_page]")
{
    DynamicPrintConfig config = flow_only_process();
    config.option<ConfigOptionFloatsOrPercentsNullable>("line_width", true)->values = {FloatOrPercent(105., true), FloatOrPercent(105., true)};
    const DynamicPrintConfig printer = four_head_printer(std::vector<NozzleVolumeType>(4, nvtStandard), false);
    PerHeadProcess::widen(config, printer);
    REQUIRE(PerHeadProcess::is_wide(config));
    // Tool head 3: a speed and a width of its own.
    const int column = PerHeadProcess::head_columns(config, 2).front();
    set_column(config, "outer_wall_speed", size_t(column), 90.);
    PerHeadProcess::set_head_value(config, 2, "outer_wall_speed", column);
    set_column_text(config, "line_width", size_t(column), "0.5");
    PerHeadProcess::set_head_value(config, 2, "line_width", column);
    REQUIRE(PerHeadProcess::head_override_keys(config, 2) == std::vector<std::string>{"line_width", "outer_wall_speed"});

    SECTION("the widths of the page: the speed stays") {
        PerHeadProcess::clear_head(config, 2, PerHeadProcess::flow_independent_keys());
        CHECK(PerHeadProcess::head_override_keys(config, 2) == std::vector<std::string>{"outer_wall_speed"});
        for (int c : PerHeadProcess::head_columns(config, 2))
            CHECK(text_at(config, "line_width", size_t(c)) == "105%");
        CHECK(floats_of(config, "outer_wall_speed")[size_t(column)] == 90.);
        check_invariants(config);
    }

    SECTION("the speeds of the page: the width stays") {
        std::set<std::string> speeds;
        for (const std::string &key : PerHeadProcess::head_editable_keys())
            if (PerHeadProcess::flow_independent_keys().count(key) == 0)
                speeds.insert(key);
        PerHeadProcess::clear_head(config, 2, speeds);
        CHECK(PerHeadProcess::head_override_keys(config, 2) == std::vector<std::string>{"line_width"});
        for (int c : PerHeadProcess::head_columns(config, 2))
            CHECK(text_at(config, "line_width", size_t(c)) == "0.5");
        check_invariants(config);
    }

    SECTION("a key set that names nothing of the head changes nothing") {
        PerHeadProcess::clear_head(config, 2, {"sparse_infill_speed"});
        CHECK(PerHeadProcess::head_override_keys(config, 2) == std::vector<std::string>{"line_width", "outer_wall_speed"});
    }

    SECTION("without a key set everything goes") {
        PerHeadProcess::clear_head(config, 2);
        CHECK(PerHeadProcess::head_override_keys(config, 2).empty());
        CHECK(PerHeadProcess::marker_empty(config));
    }
}

// Command-line project: one object on tool head 4 with a 0.9 mm sparse infill width for that head, stored
// in SNAPMAKER_ORCA_PHW_PROJECT_DIR (else a temporary dir) and reloaded to check the columns and marker.
TEST_CASE("The project of a line width set for a tool head is stored for the command line and loads back", "[PerHeadProcess][PerHeadWidth][Profiles][phw_cli_project]")
{
    auto bundle = load_snapmaker_bundle();
    select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.});
    bundle->project_config.option<ConfigOptionStrings>("filament_colour", true)->values = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    DynamicPrintConfig &edited = bundle->prints.get_edited_preset().config;
    PerHeadProcess::widen(edited, bundle->printers.get_edited_preset().config);
    REQUIRE(ints_of(edited, "print_extruder_id") == U1_WIDE_IDS);
    set_column_text(edited, "sparse_infill_line_width", 8, "0.9");
    PerHeadProcess::set_head_value(edited, 3, "sparse_infill_line_width", 8);
    DynamicPrintConfig stored = bundle->full_config_secure();
    REQUIRE(text_at(stored, "sparse_infill_line_width", 8) == "0.9");

    Model model;
    REQUIRE(load_stl((std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl").c_str(), &model));
    model.add_default_instances();
    REQUIRE_FALSE(model.objects.empty());
    // The command line slices a plate only when an instance lies inside its printable area (Model::update_print_volume_state,
    // exit CLI_NO_SUITABLE_OBJECTS otherwise); the mesh of Prusa.stl lies at negative coordinates, so the instance is
    // centred on the bed and set down on it, as the plater does with a loaded model.
    const BoundingBoxf bed(bundle->printers.get_edited_preset().config.opt<ConfigOptionPoints>("printable_area")->values);
    model.center_instances_around_point(bed.center());
    model.objects.front()->ensure_on_bed();
    model.objects.front()->config.set("extruder", 4);
    ScopedTemporaryDir backup_dir("orca_phw_cli");
    model.set_backup_path(backup_dir.string());
    const char *export_dir = std::getenv("SNAPMAKER_ORCA_PHW_PROJECT_DIR");
    ScopedTemporaryDir temp_dir("orca_phw_cli_project");
    const std::string path = ((export_dir != nullptr && *export_dir != 0 ? boost::filesystem::path(export_dir) : temp_dir.path()) / "per_head_width_cli.3mf").string();
    boost::filesystem::create_directories(boost::filesystem::path(path).parent_path());
    StoreParams store_params;
    store_params.path     = path.c_str();
    store_params.model    = &model;
    store_params.config   = &stored;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    PlateData *plate      = new PlateData();
    plate->plate_index    = 0;
    plate->objects_and_instances.emplace_back(0, 0);
    store_params.plate_data_list.push_back(plate);
    REQUIRE(store_bbs_3mf(store_params));
    delete plate;
    INFO("project stored at " << path);
    REQUIRE(boost::filesystem::exists(path));

    Model                     dst_model;
    ScopedTemporaryDir        dst_backup_dir("orca_phw_cli_dst");
    dst_model.set_backup_path(dst_backup_dir.string());
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs             dst_plates;
    std::vector<Preset *>     project_presets;
    Semver                    file_version;
    bool                      is_bbl_3mf = false, is_orca_3mf = false;
    REQUIRE(load_bbs_3mf(path.c_str(), &dst_config, &context, &dst_model, &dst_plates, &project_presets, &is_bbl_3mf, &is_orca_3mf, &file_version,
                         nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    release_PlateData_list(dst_plates);
    Preset::normalize(dst_config);
    CHECK(ints_of(dst_config, "print_extruder_id") == U1_WIDE_IDS);
    CHECK(text_at(dst_config, "sparse_infill_line_width", 8) == "0.9");
    CHECK(text_at(dst_config, "sparse_infill_line_width", 0) == "112.5%");
    CHECK(strings_of(dst_config, OVERRIDE)[8] == "sparse_infill_line_width");
    REQUIRE_FALSE(dst_model.objects.empty());
    CHECK(dst_model.objects.front()->config.opt_int("extruder") == 4);
}

// The pressure advance pattern writes its two line widths in mm (SuggestedConfigCalibPAPattern::
// nozzle_ratio_pairs) into every column, keeps the marker and the preset applies; a scalar write is refused.
TEST_CASE("The pressure advance pattern writes its line widths into every column of the process preset", "[PerHeadProcess][PerHeadWidth][Profiles][phw_pa_pattern]")
{
    const SuggestedConfigCalibPAPattern pattern;
    const double                        nozzle_diameter = 0.4;
    const std::vector<std::pair<std::string, double>> expected_mm = {{"line_width", 0.45}, {"initial_layer_line_width", 0.56}};

    // The loops of the three writers, then what the preset holds and that it applies.
    auto write_and_check = [&](PresetBundle &bundle, const std::vector<int> &ids, const std::vector<std::string> &marker) {
        DynamicPrintConfig &process = bundle.prints.get_edited_preset().config;
        for (const auto &opt : pattern.nozzle_ratio_pairs)
            PerHeadProcess::set_every_column(process, opt.first, FloatOrPercent(nozzle_diameter * opt.second / 100, false));
        for (const auto &[key, mm] : expected_mm) {
            INFO(key);
            const auto *option = process.option<ConfigOptionFloatsOrPercentsNullable>(key);
            REQUIRE(option != nullptr);
            CHECK(option->values.size() == ids.size());
            for (const FloatOrPercent &value : option->values) {
                CHECK_FALSE(value.percent);
                CHECK(value.value == Catch::Approx(mm));
            }
        }
        CHECK(ints_of(process, "print_extruder_id") == ids);
        CHECK(strings_of(process, OVERRIDE) == marker);
        DynamicPrintConfig full;
        REQUIRE_NOTHROW(full = bundle.full_config_for_print(false));
        Print print;
        Model model;
        REQUIRE_NOTHROW(print.apply(model, full));
    };

    SECTION("the selected preset with its two flow columns") {
        auto bundle = load_snapmaker_bundle();
        select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.});
        const DynamicPrintConfig &process = bundle->prints.get_edited_preset().config;
        const std::vector<int>    ids     = ints_of(process, "print_extruder_id");
        REQUIRE(ids.size() == 2);
        write_and_check(*bundle, ids, strings_of(process, OVERRIDE));
    }

    SECTION("a preset with a sparse infill width set for tool head 3 keeps that width and its marker") {
        auto bundle = load_snapmaker_bundle();
        select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.});
        DynamicPrintConfig &process = bundle->prints.get_edited_preset().config;
        PerHeadProcess::widen(process, bundle->printers.get_edited_preset().config);
        set_column_text(process, "sparse_infill_line_width", 6, "0.9");
        PerHeadProcess::set_head_value(process, 2, "sparse_infill_line_width", 6);
        const std::vector<std::string> marker = strings_of(process, OVERRIDE);
        REQUIRE(marker[6] == "sparse_infill_line_width");
        write_and_check(*bundle, U1_WIDE_IDS, marker);
        CHECK(text_at(process, "sparse_infill_line_width", 6) == "0.9");
        CHECK(text_at(process, "sparse_infill_line_width", 0) == "112.5%");
    }

    SECTION("the former scalar write is refused when the preset is applied") {
        auto bundle = load_snapmaker_bundle();
        select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.});
        DynamicPrintConfig &process = bundle->prints.get_edited_preset().config;
        for (const auto &opt : pattern.nozzle_ratio_pairs)
            process.set_key_value(opt.first, new ConfigOptionFloatOrPercent(nozzle_diameter * opt.second / 100, false));
        const auto apply_scalar_widths = [&] {
            DynamicPrintConfig full = bundle->full_config_for_print(false);
            Print              print;
            Model              model;
            print.apply(model, full);
        };
        CHECK_THROWS_AS(apply_scalar_widths(), ConfigurationError);
    }
}

// ---- Line widths per tool head: an older reader of a user preset ----

namespace {

// The process definition as an older reader has it (Snapmaker Orca 2.4, mainline OrcaSlicer, this
// fork before the columns): the nine line widths one value, coFloatOrPercent.
class OldReaderConfig : public DynamicConfig
{
public:
    OldReaderConfig()
    {
        m_def.options = print_config_def.options;
        for (const std::string &key : PerHeadProcess::flow_independent_keys()) {
            ConfigOptionDef &def    = m_def.options.at(key);
            def.type                = coFloatOrPercent;
            def.nullable            = false;
            def.scalar_when_uniform = false;
            def.set_default_value(new ConfigOptionFloatOrPercent(0., false));
        }
    }
    const ConfigDef *def() const override { return &m_def; }

private:
    ConfigDef m_def;
};

} // namespace

// A user preset with a per head width is written as the full array without "nil". A scalar-type
// reader reads its first number (the shared value while all columns share a unit) and no `reason`.
TEST_CASE("An older reader loads a user preset with a line width set for a tool head and reads the shared value", "[PerHeadProcess][PerHeadWidth][Profiles][phw_old_reader]")
{
    ScopedTemporaryDir temp_dir("orca_per_head_width_old_reader");

    SECTION("the file Preset::save writes for a sparse infill width set for tool head 3") {
        auto bundle = load_snapmaker_bundle();
        select_u1(*bundle, {0.4, 0.4, 0.4, 0.4}, {0., 0., 0., 0.});
        Preset *parent = bundle->prints.find_preset(STD_020_04, false, true);
        REQUIRE(parent != nullptr);
        Preset child(Preset::TYPE_PRINT, "Old reader @Snapmaker U1 (0.4 nozzle)", false);
        child.config     = parent->config;
        child.version    = parent->version;
        child.inherits() = parent->name;
        child.file       = (temp_dir.path() / "Old reader @Snapmaker U1 (0.4 nozzle).json").string();
        PerHeadProcess::widen(child.config, bundle->printers.get_edited_preset().config);
        set_column_text(child.config, "sparse_infill_line_width", 6, "0.9");
        PerHeadProcess::set_head_value(child.config, 2, "sparse_infill_line_width", 6);
        child.save(&parent->config);
        REQUIRE(boost::filesystem::exists(child.file));

        OldReaderConfig                    reader;
        std::map<std::string, std::string> key_values;
        std::string                        reason;
        REQUIRE_NOTHROW(reader.load_from_json(child.file, ForwardCompatibilitySubstitutionRule::EnableSilent, key_values, reason));
        CHECK(reason.empty());
        const auto *width = reader.option<ConfigOptionFloatOrPercent>("sparse_infill_line_width");
        REQUIRE(width != nullptr);
        CHECK(width->percent);
        CHECK(width->value == 112.5);
        // The untouched widths of the file, one value each, read as before.
        const auto *line_width = reader.option<ConfigOptionFloatOrPercent>("line_width");
        if (line_width != nullptr) {
            CHECK(line_width->percent);
            CHECK(line_width->value == 112.5);
        }
    }

    SECTION("an absolute shared value with a percent head value reads as a percent: the hazard the Quality page warns about") {
        nlohmann::json j;
        j["name"]                     = "Hazard @Snapmaker U1 (0.4 nozzle)";
        j["sparse_infill_line_width"] = nlohmann::json::array({"0.42", "0.42", "0.42", "0.42", "0.42", "0.42", "110%", "110%", "0.42", "0.42"});
        const std::string file = (temp_dir.path() / "Hazard @Snapmaker U1 (0.4 nozzle).json").string();
        {
            boost::nowide::ofstream out(file);
            out << j.dump(4);
        }
        OldReaderConfig                    reader;
        std::map<std::string, std::string> key_values;
        std::string                        reason;
        REQUIRE_NOTHROW(reader.load_from_json(file, ForwardCompatibilitySubstitutionRule::EnableSilent, key_values, reason));
        CHECK(reason.empty());
        const auto *width = reader.option<ConfigOptionFloatOrPercent>("sparse_infill_line_width");
        REQUIRE(width != nullptr);
        CHECK(width->percent);
        CHECK(width->value == Catch::Approx(0.42));
    }
}

// ---- Line widths per tool head: the seed of a width added to an object ----

// "Add settings" seeds a new override that applies on every head printing the item; override_seed
// gives the value it prints now (head-4 cube: 102.5 % of 0.8 = 0.82 mm, not the preset's 112.5 %).
TEST_CASE("A line width added to an object is seeded with the value the object prints", "[PerHeadProcess][PerHeadWidth][Profiles][phw_override_seed]")
{
    auto bundle = load_snapmaker_bundle();
    select_owner_plate(*bundle); // 0.2 / 0.4 High Flow / 0.6 / 0.8 under 0.20mm High Quality (0.4)
    std::vector<size_t> heads;
    const std::string   key = "sparse_infill_line_width";

    // One tool head: the value its size preset prints; the home-size High Flow head the selected preset's.
    CHECK(PerHeadProcess::override_seed(*bundle, key, {4}, &heads) == "102.5%");
    CHECK(heads.empty());
    CHECK(PerHeadProcess::override_seed(*bundle, key, {1}, &heads) == "110%");
    CHECK(heads.empty());
    CHECK(PerHeadProcess::override_seed(*bundle, key, {2}, &heads) == "112.5%");
    CHECK(heads.empty());
    CHECK(PerHeadProcess::override_seed(*bundle, key, {3}, &heads) == "103.33%");
    // A filament beyond the nozzles and filament 0 name the first head (Print::width_slot); no filament: the shared value.
    CHECK(PerHeadProcess::override_seed(*bundle, key, {9}, &heads) == "110%");
    CHECK(PerHeadProcess::override_seed(*bundle, key, {0}, &heads) == "110%");
    CHECK(PerHeadProcess::override_seed(*bundle, key, {}, &heads) == "112.5%");
    CHECK(heads.empty());
    // The same head twice is one head.
    CHECK(PerHeadProcess::override_seed(*bundle, key, {4, 4}, &heads) == "102.5%");
    CHECK(heads.empty());

    // Several tool heads: the shared value, and the heads that printed another value are named.
    CHECK(PerHeadProcess::override_seed(*bundle, key, {1, 4}, &heads) == "112.5%");
    CHECK(heads == std::vector<size_t>{0, 3});
    CHECK(PerHeadProcess::override_seed(*bundle, key, {2, 4}, &heads) == "112.5%");
    CHECK(heads == std::vector<size_t>{3});
    // Two heads that both print the shared value: nothing to say.
    CHECK(PerHeadProcess::override_seed(*bundle, key, {2, 2}, &heads) == "112.5%");
    CHECK(heads.empty());
    // A key the edited preset lacks: empty, the caller clones.
    CHECK(PerHeadProcess::override_seed(*bundle, "no_such_key", {4}, &heads).empty());
    // A key that is no line width: the shared value, no heads.
    CHECK(PerHeadProcess::override_seed(*bundle, "outer_wall_speed", {1, 4}, &heads) == text_at(bundle->prints.get_edited_preset().config, "outer_wall_speed", size_t(PerHeadProcess::shared_column(bundle->prints.get_edited_preset().config, nvtStandard))));
    CHECK(heads.empty());

    SECTION("a preset chosen for the 0.6 mm head seeds its value") {
        PerHeadProcess::set_chosen(*bundle, 2, STD_024_06);
        CHECK(PerHeadProcess::override_seed(*bundle, key, {3}, &heads) == "103.33%");
        CHECK(PerHeadProcess::override_seed(*bundle, "line_width", {3}, &heads) == "103.33%");
    }

    SECTION("a width changed under All tool heads is what every head prints") {
        bundle->prints.get_edited_preset().config.set_key_value(key, new ConfigOptionFloatsOrPercentsNullable{FloatOrPercent(110., true)});
        CHECK(PerHeadProcess::override_seed(*bundle, key, {4}, &heads) == "110%");
        CHECK(PerHeadProcess::override_seed(*bundle, key, {1, 4}, &heads) == "110%");
        CHECK(heads.empty());
        // The other widths still follow the size presets.
        CHECK(PerHeadProcess::override_seed(*bundle, "line_width", {4}, &heads) == "102.5%");
    }

    SECTION("a width set for one tool head is what that head prints") {
        DynamicPrintConfig       &process = bundle->prints.get_edited_preset().config;
        const DynamicPrintConfig &printer = bundle->printers.get_edited_preset().config;
        PerHeadProcess::widen(process, printer);
        const std::vector<int> columns = PerHeadProcess::head_columns(process, 3);
        REQUIRE_FALSE(columns.empty());
        set_column_text(process, key, size_t(columns.front()), "0.9");
        PerHeadProcess::set_head_value(process, 3, key, columns.front());
        CHECK(PerHeadProcess::override_seed(*bundle, key, {4}, &heads) == "0.9");
        CHECK(PerHeadProcess::override_seed(*bundle, key, {3}, &heads) == "103.33%");
        CHECK(PerHeadProcess::override_seed(*bundle, key, {2, 4}, &heads) == "112.5%");
        CHECK(heads == std::vector<size_t>{3});
    }

    SECTION("with the preference off every head prints the selected preset") {
        bundle->process_follows_nozzle = false;
        CHECK(PerHeadProcess::override_seed(*bundle, key, {4}, &heads) == "112.5%");
        CHECK(PerHeadProcess::override_seed(*bundle, key, {1, 4}, &heads) == "112.5%");
        CHECK(heads.empty());
    }
}
