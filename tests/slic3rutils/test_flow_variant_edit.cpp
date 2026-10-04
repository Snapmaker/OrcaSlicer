#include <catch2/catch.hpp>

#include <algorithm>

#include "slic3r/GUI/FlowVariantEdit.hpp"

#include "libslic3r/PresetFlowVariant.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

static DynamicPrintConfig make_two_mode_filament_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{12.0, 24.0});
    config.set_key_value("nozzle_temperature", new ConfigOptionInts{210, 230});
    config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.95, 0.88});
    config.set_key_value("enable_pressure_advance", new ConfigOptionBools{true, false});
    return config;
}

static DynamicPrintConfig make_two_mode_process_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("process_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});
    config.set_key_value("outer_wall_speed", new ConfigOptionFloats{60.0, 120.0});
    config.set_key_value("inner_wall_speed", new ConfigOptionFloats{80.0, 160.0});
    return config;
}

TEST_CASE("flow_variant_slots_differ detects Standard vs High flow diverge", "[FlowVariantEdit]")
{
    DynamicPrintConfig filament = make_two_mode_filament_config();
    REQUIRE(flow_variant_slots_differ(filament, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));
    // Same-mode and missing High-flow support are no-ops (Both-enter / Copy confirm stay silent).
    REQUIRE_FALSE(flow_variant_slots_differ(filament, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_STANDARD));
    REQUIRE_FALSE(flow_variant_slots_differ(filament, ConfigFlowDomain::Process, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));

    DynamicPrintConfig process = make_two_mode_process_config();
    REQUIRE(flow_variant_slots_differ(process, ConfigFlowDomain::Process, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));

    DynamicPrintConfig single = DynamicPrintConfig::full_print_config();
    single.set_key_value("filament_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD});
    single.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{12.0});
    REQUIRE_FALSE(flow_variant_slots_differ(single, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));

    REQUIRE(copy_flow_variant_slot(filament, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));
    REQUIRE_FALSE(flow_variant_slots_differ(filament, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));
}

TEST_CASE("copy_flow_variant_slot copies domain keys Standard to High flow", "[FlowVariantEdit]")
{
    DynamicPrintConfig config = make_two_mode_filament_config();
    config.set_key_value("filament_flow_step_size", new ConfigOptionInts{2, 2});
    REQUIRE(flow_variant_slots_differ(config, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));

    REQUIRE(copy_flow_variant_slot(config, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));
    REQUIRE_FALSE(flow_variant_slots_differ(config, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));

    REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{12.0, 12.0});
    REQUIRE(config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{210, 210});
    REQUIRE(config.option<ConfigOptionFloats>("filament_flow_ratio")->values == std::vector<double>{0.95, 0.95});
    REQUIRE(config.option<ConfigOptionBools>("enable_pressure_advance")->values[0] != 0);
    REQUIRE(config.option<ConfigOptionBools>("enable_pressure_advance")->values[1] != 0);
    REQUIRE(config.option<ConfigOptionInts>("filament_flow_step_size")->values == std::vector<int>{2, 2});
}

TEST_CASE("copy_flow_variant_slot copies process keys and leaves filament slots alone", "[FlowVariantEdit]")
{
    DynamicPrintConfig config = make_two_mode_process_config();
    config.set_key_value("filament_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{12.0, 24.0});

    REQUIRE(copy_flow_variant_slot(config, ConfigFlowDomain::Process, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));
    REQUIRE(config.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{60.0, 60.0});
    REQUIRE(config.option<ConfigOptionFloats>("inner_wall_speed")->values == std::vector<double>{80.0, 80.0});
    REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{12.0, 24.0});
}

TEST_CASE("copy_flow_variant_slot is a no-op without a High-flow support entry", "[FlowVariantEdit]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD});
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{12.0});

    REQUIRE_FALSE(copy_flow_variant_slot(config, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));
    REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{12.0});
}

TEST_CASE("replicate_flow_variant_value dual-writes one key across mode indices", "[FlowVariantEdit]")
{
    DynamicPrintConfig config = make_two_mode_filament_config();
    config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values[0] = 18.0;

    REQUIRE(replicate_flow_variant_value(config, "filament_max_volumetric_speed", 0,
                                         {FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW}));
    REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{18.0, 18.0});
    REQUIRE(config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{210, 230});
}

TEST_CASE("replicate_flow_variant_value leaves indices past modes.size() intact", "[FlowVariantEdit]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});
    // Preset-sized Standard/HF pair plus a trailing composed-layout leftover.
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{12.0, 24.0, 36.0, 48.0});

    REQUIRE(replicate_flow_variant_value(config, "filament_max_volumetric_speed", 0,
                                         {FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW}));
    REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values ==
            std::vector<double>{12.0, 12.0, 36.0, 48.0});
}

TEST_CASE("replicate_flow_variant_value is a no-op for a single mode", "[FlowVariantEdit]")
{
    DynamicPrintConfig config = make_two_mode_filament_config();
    REQUIRE_FALSE(replicate_flow_variant_value(config, "filament_max_volumetric_speed", 0, {FLOW_MODE_STANDARD}));
    REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{12.0, 24.0});
}

TEST_CASE("ensure_flow_support_mode appends High flow and preserves existing modes", "[FlowVariantEdit]")
{
    // The full-config default is standard-only, like a filament preset that
    // never carried the key: the switch to High flow persists both modes.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE(ensure_flow_support_mode(config, ConfigFlowDomain::Filament, FLOW_MODE_HIGH_FLOW));
    REQUIRE(config.option<ConfigOptionStrings>("filament_flow_support")->values ==
            std::vector<std::string>{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});

    // A repeated call must not duplicate the mode.
    REQUIRE_FALSE(ensure_flow_support_mode(config, ConfigFlowDomain::Filament, FLOW_MODE_HIGH_FLOW));
    REQUIRE(config.option<ConfigOptionStrings>("filament_flow_support")->values ==
            std::vector<std::string>{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});

    // Other existing modes are preserved; standard is only appended when missing.
    config.set_key_value("filament_flow_support", new ConfigOptionStrings{"custom_mode"});
    REQUIRE(ensure_flow_support_mode(config, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD));
    REQUIRE(config.option<ConfigOptionStrings>("filament_flow_support")->values ==
            std::vector<std::string>{"custom_mode", FLOW_MODE_STANDARD});
    REQUIRE_FALSE(ensure_flow_support_mode(config, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD));

    // An explicitly emptied key falls back to ["standard"] before appending.
    config.set_key_value("filament_flow_support", new ConfigOptionStrings{});
    REQUIRE(ensure_flow_support_mode(config, ConfigFlowDomain::Filament, FLOW_MODE_HIGH_FLOW));
    REQUIRE(config.option<ConfigOptionStrings>("filament_flow_support")->values ==
            std::vector<std::string>{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});
}

TEST_CASE("ensure_flow_support_mode maps domains to their support keys", "[FlowVariantEdit]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    REQUIRE(ensure_flow_support_mode(config, ConfigFlowDomain::Process, FLOW_MODE_HIGH_FLOW));
    REQUIRE(config.option<ConfigOptionStrings>("process_flow_support")->values ==
            std::vector<std::string>{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});

    REQUIRE(ensure_flow_support_mode(config, ConfigFlowDomain::Printer, FLOW_MODE_HIGH_FLOW));
    REQUIRE(config.option<ConfigOptionStrings>("printer_flow_support")->values ==
            std::vector<std::string>{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});

    // Only the addressed domain's key changes.
    REQUIRE(config.option<ConfigOptionStrings>("filament_flow_support")->values ==
            std::vector<std::string>{FLOW_MODE_STANDARD});
}

TEST_CASE("Both-click path: keyless filament gets High flow injected before entering Both", "[FlowVariantEdit]")
{
    // Simulates on_flow_variant_segment_selected's Both branch on a filament
    // whose filament_flow_support does not carry high_flow yet: ensure first,
    // so the Both-enter differ check then sees a well-defined High-flow slot.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{12.0});

    REQUIRE(ensure_flow_support_mode(config, ConfigFlowDomain::Filament, FLOW_MODE_HIGH_FLOW));
    REQUIRE(config.option<ConfigOptionStrings>("filament_flow_support")->values ==
            std::vector<std::string>{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});

    // The variant vectors still hold a single (Standard) slot; the differ check
    // the Both-enter dialog gates on reads the missing High-flow slot as the
    // Standard slot, so no overwrite prompt appears.
    REQUIRE_FALSE(flow_variant_slots_differ(config, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));

    // Copying Standard onto High flow then materializes the second slot.
    REQUIRE(copy_flow_variant_slot(config, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));
    REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{12.0, 12.0});
}

TEST_CASE("copy_flow_variant_slot visits every filament_flow_variant_options key", "[FlowVariantEdit]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});

    for (const std::string &key : filament_flow_variant_options()) {
        auto *opt = config.option(key);
        if (opt == nullptr)
            continue;
        auto *vec = dynamic_cast<ConfigOptionVectorBase *>(opt);
        REQUIRE(vec != nullptr);
        if (vec->size() < 2)
            vec->resize(2);
    }

    copy_flow_variant_slot(config, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW);
    REQUIRE_FALSE(flow_variant_slots_differ(config, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));

    for (const std::string &key : filament_flow_variant_options()) {
        const auto *opt = config.option(key);
        if (opt == nullptr)
            continue;
        const auto *vec = dynamic_cast<const ConfigOptionVectorBase *>(opt);
        REQUIRE(vec != nullptr);
        const auto values = vec->vserialize();
        REQUIRE_FALSE(values.empty());
        const std::string high_flow = values.size() > 1 ? values[1] : values.front();
        REQUIRE(values.front() == high_flow);
    }
}

static DynamicPrintConfig make_std_hf_filament_tab_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});
    config.set_key_value("enable_pressure_advance", new ConfigOptionBools{true, false});
    config.set_key_value("pressure_advance", new ConfigOptionFloats{0.04, 0.02});
    config.set_key_value("nozzle_temperature", new ConfigOptionInts{210, 250});
    config.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts{215, 180});
    config.set_key_value("nozzle_temperature_range_low", new ConfigOptionInts{200});
    config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts{230});
    config.set_key_value("filament_multitool_ramming", new ConfigOptionBools{false, true});
    config.set_key_value("filament_multitool_ramming_volume", new ConfigOptionFloats{2.0, 20.0});
    config.set_key_value("filament_multitool_ramming_flow", new ConfigOptionFloats{1.0, 10.0});
    config.set_key_value("fan_min_speed", new ConfigOptionInts{30, 80});
    config.set_key_value("fan_max_speed", new ConfigOptionInts{60, 100});
    config.set_key_value("additional_cooling_fan_speed", new ConfigOptionInts{0, 70});
    config.set_key_value("filament_retraction_length", new ConfigOptionFloats{0.8, 3.0});
    config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.98, 0.88});
    return config;
}

TEST_CASE("filament tab option index follows the view for every flow-variant key", "[FlowVariantEdit][FilamentTabIndex]")
{
    for (const std::string &key : filament_flow_variant_options()) {
        REQUIRE(filament_tab_option_index(key, 0) == 0);
        REQUIRE(filament_tab_option_index(key, 1) == 1);
    }
    REQUIRE(filament_tab_option_index("adaptive_pressure_advance", 1) == 0);
    REQUIRE(filament_tab_option_index("nozzle_temperature_range_low", 1) == 0);
}

TEST_CASE("filament tab PA ramming and temp-range checks follow the selected view", "[FlowVariantEdit][FilamentTabIndex]")
{
    const DynamicPrintConfig config = make_std_hf_filament_tab_config();

    const int standard_index = filament_tab_option_index("enable_pressure_advance", 0);
    const int high_flow_index = filament_tab_option_index("enable_pressure_advance", 1);
    REQUIRE(standard_index == 0);
    REQUIRE(high_flow_index == 1);

    REQUIRE(config.opt_bool("enable_pressure_advance", standard_index));
    REQUIRE_FALSE(config.opt_bool("enable_pressure_advance", high_flow_index));

    REQUIRE_FALSE(config.opt_bool("filament_multitool_ramming", filament_tab_option_index("filament_multitool_ramming", 0)));
    REQUIRE(config.opt_bool("filament_multitool_ramming", filament_tab_option_index("filament_multitool_ramming", 1)));

    REQUIRE_FALSE(filament_nozzle_temperature_out_of_range(config, standard_index));
    REQUIRE(filament_nozzle_temperature_out_of_range(config, high_flow_index));
    REQUIRE_FALSE(filament_nozzle_temperature_initial_layer_out_of_range(config, standard_index));
    REQUIRE(filament_nozzle_temperature_initial_layer_out_of_range(config, high_flow_index));

    REQUIRE(config.opt_int("fan_min_speed", filament_tab_option_index("fan_min_speed", 0)) == 30);
    REQUIRE(config.opt_int("fan_min_speed", filament_tab_option_index("fan_min_speed", 1)) == 80);
    REQUIRE(config.opt_float("filament_retraction_length", filament_tab_option_index("filament_retraction_length", 0)) == 0.8);
    REQUIRE(config.opt_float("filament_retraction_length", filament_tab_option_index("filament_retraction_length", 1)) == 3.0);
}

TEST_CASE("filament tab view index 0 on a single-column filament matches the historic index-0 reads", "[FlowVariantEdit][FilamentTabIndex]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD});
    config.set_key_value("enable_pressure_advance", new ConfigOptionBools{true});
    config.set_key_value("nozzle_temperature", new ConfigOptionInts{210});
    config.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts{215});
    config.set_key_value("nozzle_temperature_range_low", new ConfigOptionInts{200});
    config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts{230});
    config.set_key_value("filament_multitool_ramming", new ConfigOptionBools{false});

    const int variant_index = filament_tab_option_index("enable_pressure_advance", 0);
    REQUIRE(variant_index == 0);
    REQUIRE(config.opt_bool("enable_pressure_advance", variant_index) == config.opt_bool("enable_pressure_advance", 0));
    REQUIRE(config.opt_int("nozzle_temperature", variant_index) == config.opt_int("nozzle_temperature", 0));
    REQUIRE_FALSE(filament_nozzle_temperature_out_of_range(config, variant_index));
    REQUIRE_FALSE(filament_nozzle_temperature_initial_layer_out_of_range(config, variant_index));
    REQUIRE_FALSE(config.opt_bool("filament_multitool_ramming", variant_index));
}

TEST_CASE("calib filament_flow_ratio_at follows the packed High-Flow column", "[FlowVariantEdit][N4]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionInts>("filament_flow_step_size", true)->values = {2, 1};
    config.option<ConfigOptionStrings>("filament_flow_support", true)->values =
        {FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW, FLOW_MODE_STANDARD};
    config.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values = {int(fvtHighFlow), int(fvtStandard)};
    config.option<ConfigOptionFloats>("filament_flow_ratio")->values = {0.98, 0.88, 1.05};

    REQUIRE(filament_flow_ratio_at(config, 0) == 0.88);
    REQUIRE(filament_flow_ratio_at(config, 1) == 1.05);
    REQUIRE(config.option<ConfigOptionFloats>("filament_flow_ratio")->get_at(0) == 0.98);
}

TEST_CASE("filament preset flow ratio follows the selected volume type", "[FlowVariantEdit][N4]")
{
    DynamicPrintConfig preset = DynamicPrintConfig::full_print_config();
    preset.set_key_value("filament_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});
    preset.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.98, 0.88});

    REQUIRE(filament_preset_flow_ratio(preset, fvtStandard) == 0.98);
    REQUIRE(filament_preset_flow_ratio(preset, fvtHighFlow) == 0.88);
    REQUIRE(preset.option<ConfigOptionFloats>("filament_flow_ratio")->get_at(0) == 0.98);
}

TEST_CASE("calib volume type follows filament_volume_type not nozzle_volume_type", "[FlowVariantEdit][N2]")
{
    DynamicPrintConfig project = DynamicPrintConfig::full_print_config();
    project.set_key_value("filament_volume_type", new ConfigOptionEnumsGeneric{int(fvtStandard)});
    project.set_key_value("nozzle_volume_type", new ConfigOptionEnumsGeneric{int(fvtHighFlow)});

    DynamicPrintConfig preset = DynamicPrintConfig::full_print_config();
    preset.set_key_value("filament_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});
    preset.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.98, 0.88});
    preset.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{12.0, 24.0});

    REQUIRE(filament_volume_type_at(project, 0) == fvtStandard);
    REQUIRE(get_nozzle_volume_type(project, 0) == fvtHighFlow);
    REQUIRE(filament_preset_flow_ratio(preset, filament_volume_type_at(project, 0)) == 0.98);
    REQUIRE(get_preset_value_at(preset, *preset.option<ConfigOptionFloats>("filament_max_volumetric_speed"),
                                ConfigFlowDomain::Filament, filament_volume_type_at(project, 0)) == 12.0);
    REQUIRE(preset.option<ConfigOptionFloats>("filament_flow_ratio")->get_at(0) == 0.98);
}

TEST_CASE("filament_volume_type_at returns High Flow when that slot is High Flow", "[FlowVariantEdit][N2b]")
{
    DynamicPrintConfig project = DynamicPrintConfig::full_print_config();
    project.set_key_value("filament_volume_type", new ConfigOptionEnumsGeneric{int(fvtHighFlow)});
    REQUIRE(filament_volume_type_at(project, 0) == fvtHighFlow);
}

TEST_CASE("filament_volume_type_at past the vector falls back to Standard", "[FlowVariantEdit][N2c]")
{
    DynamicPrintConfig project = DynamicPrintConfig::full_print_config();
    project.set_key_value("filament_volume_type", new ConfigOptionEnumsGeneric{int(fvtHighFlow)});
    REQUIRE(filament_volume_type_at(project, 1) == fvtStandard);
    REQUIRE(filament_volume_type_at(project, 5) == fvtStandard);
}

TEST_CASE("slice sync target follows uniform nozzle type before the first slice", "[FlowVariantEdit][SliceSync]")
{
    REQUIRE(slice_sync_target_filament_volume_type(FILAMENT_GROUPING_STANDARD, 1, fvtHighFlow, fvtStandard) == fvtHighFlow);
    REQUIRE(slice_sync_target_filament_volume_type(FILAMENT_GROUPING_STANDARD, 1, fvtStandard, fvtHighFlow) == fvtStandard);
    REQUIRE(slice_sync_target_filament_volume_type(FILAMENT_GROUPING_STANDARD, 2, fvtHighFlow, fvtHighFlow) == fvtStandard);
    REQUIRE(slice_sync_target_filament_volume_type(FILAMENT_GROUPING_CUSTOM, 1, fvtHighFlow, fvtStandard) == fvtHighFlow);
    REQUIRE(slice_sync_target_filament_volume_type(FILAMENT_GROUPING_CUSTOM, 2, fvtHighFlow, fvtStandard) == fvtStandard);
    REQUIRE(slice_sync_target_filament_volume_type(FILAMENT_GROUPING_CUSTOM, 2, fvtHighFlow, fvtHighFlow) == fvtHighFlow);
}

// CalibUtils::calib_flowrate / calib_max_vol_speed read the column of the nozzle on
// calib_info.extruder_id, not filament 0's slice-sync target.
TEST_CASE("calibration flow type follows the chosen extruder's nozzle", "[FlowVariantEdit][Calib]")
{
    const std::vector<std::string> single_hf{FLOW_MODE_HIGH_FLOW};
    REQUIRE(nozzle_flow_type_at(single_hf, 0, fvtStandard) == fvtHighFlow);
    const std::vector<std::string> uniform_std{FLOW_MODE_STANDARD, FLOW_MODE_STANDARD};
    REQUIRE(nozzle_flow_type_at(uniform_std, 0, fvtHighFlow) == fvtStandard);
    REQUIRE(nozzle_flow_type_at(uniform_std, 1, fvtHighFlow) == fvtStandard);
    // Mixed H2D/H2C: each extruder gets its own nozzle's column. The slice-sync
    // target for this layout (standard grouping) is Standard for every filament.
    const std::vector<std::string> mixed{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW};
    REQUIRE(nozzle_flow_type_at(mixed, 0, fvtHighFlow) == fvtStandard);
    REQUIRE(nozzle_flow_type_at(mixed, 1, fvtStandard) == fvtHighFlow);
    REQUIRE(slice_sync_target_filament_volume_type(FILAMENT_GROUPING_STANDARD, 2, fvtStandard, fvtStandard) == fvtStandard);
    // Beyond the selected printer's nozzles: the printer preset's value.
    REQUIRE(nozzle_flow_type_at(single_hf, 1, fvtStandard) == fvtStandard);
    REQUIRE(nozzle_flow_type_at(single_hf, 1, fvtHighFlow) == fvtHighFlow);
    REQUIRE(nozzle_flow_type_at({}, 0, fvtHighFlow) == fvtHighFlow);
}

// G1 / G1L: `>` → `>=` flags 230; `<` → `<=` flags 200. 199 and 231 stay out.
TEST_CASE("recommended nozzle temp range bounds are inclusive", "[FlowVariantEdit][G1][G1L]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("nozzle_temperature_range_low", new ConfigOptionInts{200});
    config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts{230});

    config.set_key_value("nozzle_temperature", new ConfigOptionInts{200});
    config.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts{200});
    REQUIRE_FALSE(filament_nozzle_temperature_out_of_range(config, 0));
    REQUIRE_FALSE(filament_nozzle_temperature_initial_layer_out_of_range(config, 0));

    config.option<ConfigOptionInts>("nozzle_temperature")->values[0] = 230;
    config.option<ConfigOptionInts>("nozzle_temperature_initial_layer")->values[0] = 230;
    REQUIRE_FALSE(filament_nozzle_temperature_out_of_range(config, 0));
    REQUIRE_FALSE(filament_nozzle_temperature_initial_layer_out_of_range(config, 0));

    config.option<ConfigOptionInts>("nozzle_temperature")->values[0] = 199;
    config.option<ConfigOptionInts>("nozzle_temperature_initial_layer")->values[0] = 231;
    REQUIRE(filament_nozzle_temperature_out_of_range(config, 0));
    REQUIRE(filament_nozzle_temperature_initial_layer_out_of_range(config, 0));
}

// G4 (reviewer harness): filament_flow_ratio_at missing/empty fallback `1.0` → `0.0`.
TEST_CASE("filament_flow_ratio_at missing option falls back to 1.0", "[FlowVariantEdit][G4]")
{
    DynamicPrintConfig empty;
    REQUIRE(filament_flow_ratio_at(empty) == 1.0);

    DynamicPrintConfig cleared = DynamicPrintConfig::full_print_config();
    cleared.set_key_value("filament_flow_ratio", new ConfigOptionFloats{});
    REQUIRE(filament_flow_ratio_at(cleared) == 1.0);
}

// G5 (reviewer harness): filament_preset_flow_ratio missing/empty fallback `1.0` → `0.0`.
TEST_CASE("filament_preset_flow_ratio missing option falls back to 1.0", "[FlowVariantEdit][G5]")
{
    DynamicPrintConfig empty;
    REQUIRE(filament_preset_flow_ratio(empty, fvtStandard) == 1.0);
    REQUIRE(filament_preset_flow_ratio(empty, fvtHighFlow) == 1.0);

    DynamicPrintConfig cleared = DynamicPrintConfig::full_print_config();
    cleared.set_key_value("filament_flow_ratio", new ConfigOptionFloats{});
    REQUIRE(filament_preset_flow_ratio(cleared, fvtStandard) == 1.0);
    REQUIRE(filament_preset_flow_ratio(cleared, fvtHighFlow) == 1.0);
}

// The filament tab's Setting Overrides page showed "nan" with a ticked box in the
// High-Flow view: a preset that stores one value (["nil"]) was read at index 1, past
// the end. is_nil(idx) read out of bounds (garbage, so "set") while get_at fell back
// to the nil slot 0. is_nil now reads the slot get_at would.
TEST_CASE("is_nil past the end reads the slot get_at reads", "[FlowVariantEdit][Overrides]")
{
    const double nil = ConfigOptionFloatsNullable::nil_value();
    ConfigOptionFloatsNullable one_nil;
    one_nil.values = {nil};
    CHECK(one_nil.is_nil(0));
    CHECK(one_nil.is_nil(1));
    ConfigOptionFloatsNullable one_value;
    one_value.values = {0.8};
    CHECK_FALSE(one_value.is_nil(1));
    CHECK(one_value.get_at(1) == Approx(0.8));
    ConfigOptionIntsNullable ints;
    ints.values = {ConfigOptionIntsNullable::nil_value()};
    CHECK(ints.is_nil(3));
    ConfigOptionBoolsNullable bools;
    bools.values = {ConfigOptionBoolsNullable::nil_value()};
    CHECK(bools.is_nil(1));
}

TEST_CASE("a nil High-Flow override uses the Standard slot, then the printer", "[FlowVariantEdit][Overrides]")
{
    const double nil = ConfigOptionFloatsNullable::nil_value();
    ConfigOptionFloatsNullable std_only;
    std_only.values = {0.7, nil};
    CHECK(filament_override_effective_slot(&std_only, 1) == 0);
    CHECK(filament_override_effective_slot(&std_only, 0) == 0);
    ConfigOptionFloatsNullable own_hf;
    own_hf.values = {nil, 0.6};
    CHECK(filament_override_effective_slot(&own_hf, 1) == 1);
    CHECK(filament_override_effective_slot(&own_hf, 0) == -1);
    ConfigOptionFloatsNullable one_nil;
    one_nil.values = {nil};
    CHECK(filament_override_effective_slot(&one_nil, 1) == -1);
    ConfigOptionFloatsNullable one_value;
    one_value.values = {0.7};
    CHECK(filament_override_effective_slot(&one_value, 1) == 1);   // past the end reads slot 0, which is set
    CHECK(filament_override_effective_slot(nullptr, 1) == -1);

    // Composition into the packed config: the same rule, so the G-code matches the tab.
    ConfigOptionFloatsNullable packed;
    packed.values = {nil};
    compose_filament_flow_variant_segment(packed, std_only, 0, 2);
    compose_filament_flow_variant_segment(packed, one_nil, 2, 2);
    compose_filament_flow_variant_segment(packed, own_hf, 4, 2);
    REQUIRE(packed.values.size() == 6);
    CHECK(packed.values[0] == Approx(0.7));
    CHECK(packed.values[1] == Approx(0.7));   // nil High-Flow -> Standard
    CHECK(packed.is_nil(2));
    CHECK(packed.is_nil(3));                  // both nil -> printer value at apply time
    CHECK(packed.is_nil(4));
    CHECK(packed.values[5] == Approx(0.6));   // own value kept

    // Non-nullable variant keys: a short preset vector repeats its Standard value.
    ConfigOptionFloats flow_dst{1.0};
    compose_filament_flow_variant_segment(flow_dst, ConfigOptionFloats{0.95}, 0, 2);
    REQUIRE(flow_dst.values.size() == 2);
    CHECK(flow_dst.values[1] == Approx(0.95));
}

// Audit of the 19 filament flow-variant keys: only the six retract overrides are
// nullable, so only they can hold a nil High-Flow slot. The other 13 are plain
// vectors whose short presets read the Standard value through get_at.
TEST_CASE("only the retract overrides among the filament flow-variant keys are nullable", "[FlowVariantEdit][Overrides]")
{
    std::vector<std::string> nullable;
    for (const std::string &key : filament_flow_variant_options()) {
        const ConfigOptionDef *def = print_config_def.get(key);
        REQUIRE(def != nullptr);
        if (def->nullable)
            nullable.push_back(key);
    }
    std::sort(nullable.begin(), nullable.end());
    CHECK(nullable == std::vector<std::string>{"filament_deretraction_speed", "filament_retract_length_toolchange",
                                               "filament_retraction_length", "filament_retraction_speed",
                                               "filament_wipe_distance", "filament_z_hop_types"});
}
