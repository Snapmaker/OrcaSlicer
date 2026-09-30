#include <catch2/catch_all.hpp>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/FilamentFlowColumns.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/ProjectSchemaVersion.hpp"
#include "libslic3r/SnapmakerFlowCompat.hpp"

#include "snapmaker_high_flow_fixture.hpp"

#include "test_utils.hpp"

using namespace Slic3r;

// Pressure advance, fan, ramming, purge, bed and chamber values hold one value per filament variant column
// (Standard / High Flow). Project files of mainline OrcaSlicer and of earlier versions store them
// once per filament; these cases cover the rebuild of such files.

namespace {

const char *const STANDARD  = "Direct Drive Standard";
const char *const HIGH_FLOW = "Direct Drive High Flow";

// Project settings of two filaments with a Standard and a High Flow column each.
DynamicPrintConfig two_filaments_with_flow_columns()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.75}));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4}));
    config.set_key_value("extruder_variant_list", new ConfigOptionStrings(std::vector<std::string>(2, std::string(STANDARD) + "," + HIGH_FLOW)));
    config.set_key_value("filament_extruder_variant", new ConfigOptionStrings({STANDARD, HIGH_FLOW, STANDARD, HIGH_FLOW}));
    config.set_key_value("filament_self_index", new ConfigOptionInts({1, 1, 2, 2}));
    config.set_key_value("nozzle_temperature", new ConfigOptionInts({215, 220, 240, 250}));
    return config;
}

} // namespace

TEST_CASE("Per filament pressure advance of an older project is rebuilt per filament variant column", "[HighFlow][FlowCompat][hf_promoted_shim]")
{
    DynamicPrintConfig config = two_filaments_with_flow_columns();
    config.set_key_value("pressure_advance", new ConfigOptionFloats({0.03, 0.05}));
    config.set_key_value("enable_pressure_advance", new ConfigOptionBools({true, false}));
    config.set_key_value("additional_cooling_fan_speed", new ConfigOptionInts({70, 10}));
    // Already one value per column: left alone.
    config.set_key_value("fan_max_speed", new ConfigOptionFloats({40., 60., 30., 25.}));
    // Neither layout: left to the loader's padding.
    config.set_key_value("fan_min_speed", new ConfigOptionFloats({35.}));

    const size_t rebuilt = normalize_promoted_filament_keys(config, 2, {1, 1, 2, 2});

    CHECK(rebuilt == 3);
    CHECK_THAT(config.option<ConfigOptionFloats>("pressure_advance")->values, Catch::Matchers::Approx(std::vector<double>{0.03, 0.03, 0.05, 0.05}));
    CHECK(config.option<ConfigOptionBools>("enable_pressure_advance")->values == std::vector<unsigned char>{1, 1, 0, 0});
    CHECK(config.option<ConfigOptionInts>("additional_cooling_fan_speed")->values == std::vector<int>{70, 70, 10, 10});
    CHECK_THAT(config.option<ConfigOptionFloats>("fan_max_speed")->values, Catch::Matchers::Approx(std::vector<double>{40., 60., 30., 25.}));
    CHECK_THAT(config.option<ConfigOptionFloats>("fan_min_speed")->values, Catch::Matchers::Approx(std::vector<double>{35.}));
}

TEST_CASE("A project with one column per filament is the same in both layouts and stays untouched", "[HighFlow][FlowCompat][hf_promoted_shim]")
{
    DynamicPrintConfig config = two_filaments_with_flow_columns();
    config.set_key_value("pressure_advance", new ConfigOptionFloats({0.03, 0.05}));

    CHECK(normalize_promoted_filament_keys(config, 2, {1, 2}) == 0);
    CHECK_THAT(config.option<ConfigOptionFloats>("pressure_advance")->values, Catch::Matchers::Approx(std::vector<double>{0.03, 0.05}));

    SECTION("a column index that names no filament of the project rebuilds nothing") {
        CHECK(normalize_promoted_filament_keys(config, 2, {1, 1, 3, 3}) == 0);
        CHECK(normalize_promoted_filament_keys(config, 2, {0, 1, 2, 2}) == 0);
        CHECK_THAT(config.option<ConfigOptionFloats>("pressure_advance")->values, Catch::Matchers::Approx(std::vector<double>{0.03, 0.05}));
    }
}

TEST_CASE("Loading an older project gives every filament preset its own pressure advance in both columns", "[HighFlow][FlowCompat][hf_promoted_shim]")
{
    // The loader splits the project settings into one preset per filament and resizes the shared
    // vectors while it does so; the second filament must not end up with the first one's value.
    DynamicPrintConfig config = two_filaments_with_flow_columns();
    config.set_key_value("pressure_advance", new ConfigOptionFloats({0.03, 0.05}));
    config.set_key_value("filament_minimal_purge_on_wipe_tower", new ConfigOptionFloats({15., 50.}));
    // The application normalizes the project settings before it loads them.
    Preset::normalize(config);

    PresetBundle bundle;
    bundle.load_config_model("older_project.3mf", std::move(config), Semver());

    REQUIRE(bundle.filament_presets.size() == 2);
    const std::vector<std::vector<double>> expected_pa    = {{0.03, 0.03}, {0.05, 0.05}};
    const std::vector<std::vector<double>> expected_purge = {{15., 15.}, {50., 50.}};
    const std::vector<std::vector<int>>    expected_temp  = {{215, 220}, {240, 250}};
    for (size_t i = 0; i < 2; ++i) {
        INFO("filament " << i + 1 << ": " << bundle.filament_presets[i]);
        const Preset *preset = bundle.filaments.find_preset(bundle.filament_presets[i], false);
        REQUIRE(preset != nullptr);
        CHECK(preset->config.option<ConfigOptionStrings>("filament_extruder_variant")->values == std::vector<std::string>{STANDARD, HIGH_FLOW});
        CHECK_THAT(preset->config.option<ConfigOptionFloats>("pressure_advance")->values, Catch::Matchers::Approx(expected_pa[i]));
        CHECK_THAT(preset->config.option<ConfigOptionFloats>("filament_minimal_purge_on_wipe_tower")->values, Catch::Matchers::Approx(expected_purge[i]));
        // A key that was stored per column all along.
        CHECK(preset->config.option<ConfigOptionInts>("nozzle_temperature")->values == expected_temp[i]);
    }
}

TEST_CASE("Per filament bed and chamber temperatures of an older project are rebuilt per filament variant column", "[HighFlow][FlowCompat][hf_promoted_shim][BedChamber]")
{
    // Mainline projects and projects of earlier versions store them once per filament.
    DynamicPrintConfig config = two_filaments_with_flow_columns();
    config.set_key_value("textured_plate_temp", new ConfigOptionInts({60, 80}));
    config.set_key_value("textured_plate_temp_initial_layer", new ConfigOptionInts({65, 85}));
    config.set_key_value("activate_chamber_temp_control", new ConfigOptionBools({false, true}));
    config.set_key_value("chamber_temperature", new ConfigOptionInts({0, 40}));

    SECTION("the values are spread over the columns of each filament") {
        CHECK(normalize_promoted_filament_keys(config, 2, {1, 1, 2, 2}) == 4);
        CHECK(config.option<ConfigOptionInts>("textured_plate_temp")->values == std::vector<int>{60, 60, 80, 80});
        CHECK(config.option<ConfigOptionInts>("textured_plate_temp_initial_layer")->values == std::vector<int>{65, 65, 85, 85});
        CHECK(config.option<ConfigOptionBools>("activate_chamber_temp_control")->values == std::vector<unsigned char>{0, 0, 1, 1});
        CHECK(config.option<ConfigOptionInts>("chamber_temperature")->values == std::vector<int>{0, 0, 40, 40});
    }
    SECTION("every filament preset of the loaded project has its own values in both columns") {
        // The application normalizes the project settings before it loads them.
        Preset::normalize(config);
        PresetBundle bundle;
        bundle.load_config_model("older_project.3mf", std::move(config), Semver());
        REQUIRE(bundle.filament_presets.size() == 2);
        const std::vector<std::vector<int>>           expected_bed     = {{60, 60}, {80, 80}};
        const std::vector<std::vector<int>>           expected_chamber = {{0, 0}, {40, 40}};
        const std::vector<std::vector<unsigned char>> expected_control = {{0, 0}, {1, 1}};
        for (size_t i = 0; i < 2; ++i) {
            INFO("filament " << i + 1 << ": " << bundle.filament_presets[i]);
            const Preset *preset = bundle.filaments.find_preset(bundle.filament_presets[i], false);
            REQUIRE(preset != nullptr);
            CHECK(preset->config.option<ConfigOptionStrings>("filament_extruder_variant")->values == std::vector<std::string>{STANDARD, HIGH_FLOW});
            CHECK(preset->config.option<ConfigOptionInts>("textured_plate_temp")->values == expected_bed[i]);
            CHECK(preset->config.option<ConfigOptionInts>("chamber_temperature")->values == expected_chamber[i]);
            CHECK(preset->config.option<ConfigOptionBools>("activate_chamber_temp_control")->values == expected_control[i]);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Snapmaker Orca 2.4 files (tests/data/snapmaker_2_4, from upstream b1831e5dcb presets): a U1 0.4
// project with four filaments, tool head 2 on High Flow, and a user filament and process preset.
// ---------------------------------------------------------------------------------------------

namespace {

namespace fs = boost::filesystem;

const char *const U1_MACHINE = "Snapmaker U1 (0.4 nozzle)";
const char *const U1_PROCESS = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)";

std::string fixture(const std::string &name) { return std::string(TEST_DATA_DIR) + "/snapmaker_2_4/" + name; }

DynamicPrintConfig load_json(const std::string &path, ConfigSubstitutionContext &context)
{
    DynamicPrintConfig                 config;
    std::map<std::string, std::string> key_values;
    std::string                        reason;
    config.load_from_json(path, context, true, key_values, reason);
    REQUIRE(reason.empty());
    return config;
}

DynamicPrintConfig load_project_2_4()
{
    ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Enable};
    DynamicPrintConfig        config = load_json(fixture("project_settings_u1_head2_high_flow.config"), context);
    for (const ConfigSubstitution &substitution : context.substitutions)
        UNSCOPED_INFO("substituted: " << substitution.opt_def->opt_key << " = " << substitution.old_value);
    CHECK(context.substitutions.empty());
    // The application normalizes the project settings before it loads them.
    Preset::normalize(config);
    return config;
}

std::unique_ptr<PresetBundle> load_snapmaker_bundle()
{
    auto bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(PROFILES_DIR, "Snapmaker", PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr,
                                          /*allow_cache=*/false);
    return bundle;
}

std::vector<int> head_flow_types(const DynamicPrintConfig &config)
{
    const auto *types = config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    REQUIRE(types != nullptr);
    return types->values;
}

std::vector<std::string> strings_of(const DynamicPrintConfig &config, const std::string &key)
{
    const auto *values = config.option<ConfigOptionStrings>(key);
    REQUIRE(values != nullptr);
    return values->values;
}

// What the loaded bundle prints filament i with, once filament i is printed by tool head i.
DynamicPrintConfig composed_per_head(PresetBundle &bundle)
{
    std::vector<int> map(bundle.filament_presets.size());
    for (size_t i = 0; i < map.size(); ++i)
        map[i] = int(i) + 1;
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = map;
    return bundle.full_config(true);
}

void check_head_2_prints_high_flow(PresetBundle &bundle)
{
    CHECK(head_flow_types(bundle.project_config) == std::vector<int>{int(nvtStandard), int(nvtHighFlow), int(nvtStandard), int(nvtStandard)});

    // The printer still declares a Standard and a High Flow nozzle for every tool head.
    const DynamicPrintConfig &printer = bundle.printers.get_edited_preset().config;
    CHECK(strings_of(printer, "extruder_variant_list") == std::vector<std::string>(4, std::string(STANDARD) + "," + HIGH_FLOW));
    CHECK(strings_of(printer, "printer_extruder_variant").size() == 8);

    REQUIRE(bundle.filament_presets.size() == 4);
    const std::vector<size_t> expected_columns = {2, 2, 1, 2};
    for (size_t i = 0; i < 4; ++i) {
        INFO(bundle.filament_presets[i]);
        const Preset *preset = bundle.filaments.find_preset(bundle.filament_presets[i], false);
        REQUIRE(preset != nullptr);
        CHECK(strings_of(preset->config, "filament_extruder_variant").size() == expected_columns[i]);
    }

    const DynamicPrintConfig composed = composed_per_head(bundle);
    // The shipped presets give the expected numbers: this application keeps its own values for a
    // system preset, the file only says which presets and which flow types.
    const Preset *snapspeed = bundle.filaments.find_preset("Snapmaker PLA SnapSpeed @U1", false, true);
    const Preset *petg_hf   = bundle.filaments.find_preset("Snapmaker PETG HF", false, true);
    const Preset *process   = bundle.prints.find_preset(U1_PROCESS, false, true);
    REQUIRE(snapspeed != nullptr);
    REQUIRE(petg_hf != nullptr);
    REQUIRE(process != nullptr);
    const auto *petg_speed = petg_hf->config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    REQUIRE(petg_speed->size() == 2);
    REQUIRE(petg_speed->get_at(0) != petg_speed->get_at(1));

    const auto *speed = composed.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    REQUIRE(speed->size() == 4);
    CHECK_THAT(speed->get_at(0), Catch::Matchers::WithinAbs(snapspeed->config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->get_at(0), 1e-9));
    CHECK_THAT(speed->get_at(1), Catch::Matchers::WithinAbs(petg_speed->get_at(1), 1e-9));
    const auto *pressure_advance = composed.option<ConfigOptionFloats>("pressure_advance");
    CHECK_THAT(pressure_advance->get_at(1), Catch::Matchers::WithinAbs(petg_hf->config.option<ConfigOptionFloats>("pressure_advance")->get_at(1), 1e-9));
    // The High Flow column of "Snapmaker PETG HF" sets a retraction, its Standard column takes the printer's.
    const auto *retraction = composed.option<ConfigOptionFloatsNullable>("filament_retraction_length");
    REQUIRE(retraction->size() == 4);
    CHECK_FALSE(retraction->is_nil(1));
    CHECK_THAT(retraction->get_at(1), Catch::Matchers::WithinAbs(0.8, 1e-9));
    // The process: High Flow speeds for tool head 2 only.
    const auto *wall_speed = composed.option<ConfigOptionFloats>("outer_wall_speed");
    const auto *preset_wall_speed = process->config.option<ConfigOptionFloats>("outer_wall_speed");
    REQUIRE(preset_wall_speed->size() == 2);
    CHECK_THAT(wall_speed->values, Catch::Matchers::Approx(std::vector<double>{preset_wall_speed->get_at(0), preset_wall_speed->get_at(1),
                                                                                  preset_wall_speed->get_at(0), preset_wall_speed->get_at(0)}));
}

} // namespace

TEST_CASE("The flow types of Snapmaker Orca 2.4 are read in its spelling and written in the spelling of this application", "[HighFlow][FlowCompat][Config]")
{
    DynamicPrintConfig config;
    config.set_deserialize_strict("nozzle_volume_type", "standard,high_flow,standard,High Flow");
    CHECK(head_flow_types(config) == std::vector<int>{int(nvtStandard), int(nvtHighFlow), int(nvtStandard), int(nvtHighFlow)});
    CHECK(config.opt_serialize("nozzle_volume_type") == "Standard,High Flow,Standard,High Flow");

    config.set_deserialize_strict("default_nozzle_volume_type", "high_flow");
    CHECK(config.opt_serialize("default_nozzle_volume_type") == "High Flow");
}

TEST_CASE("Project settings of Snapmaker Orca 2.4 become variant columns", "[HighFlow][FlowCompat][hf_2_4_project]")
{
    DynamicPrintConfig config = load_project_2_4();
    REQUIRE(has_snapmaker_flow_keys(config));

    FlowImportReport report;
    REQUIRE(normalize_snapmaker_flow_config(config, report));

    // Filaments: strides {2, 2, 1, 2}.
    CHECK(strings_of(config, "filament_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW, STANDARD, HIGH_FLOW, STANDARD, STANDARD, HIGH_FLOW});
    CHECK(config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>{1, 1, 2, 2, 3, 4, 4});
    // One of the keys 2.4 stores per flow type: kept as it was.
    CHECK_THAT(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values, Catch::Matchers::Approx(std::vector<double>{20., 40., 20., 30., 15., 22., 40.}));
    // A key that has columns here and one value per filament in 2.4: repeated over the columns of its filament.
    const auto *z_hop = config.option<ConfigOptionFloatsNullable>("filament_z_hop");
    REQUIRE(z_hop != nullptr);
    REQUIRE(z_hop->size() == 7);
    CHECK_THAT(z_hop->get_at(0), Catch::Matchers::WithinAbs(0.4, 1e-9));
    CHECK_THAT(z_hop->get_at(1), Catch::Matchers::WithinAbs(0.4, 1e-9));
    for (size_t column : {2, 3, 4, 5, 6})
        CHECK(z_hop->is_nil(column));
    // A key without columns keeps one value per filament.
    CHECK(strings_of(config, "filament_type") == std::vector<std::string>{"PLA", "PETG", "PLA", "PLA"});

    // Process: a Standard and a High Flow column that serve every tool head.
    CHECK(config.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{1, 1});
    CHECK(strings_of(config, "print_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW});
    CHECK_THAT(config.option<ConfigOptionFloats>("outer_wall_speed")->values, Catch::Matchers::Approx(std::vector<double>{200., 500.}));

    // Printer: both nozzle types declared for every tool head, both columns with the tool head's value.
    CHECK(strings_of(config, "extruder_variant_list") == std::vector<std::string>(4, std::string(STANDARD) + "," + HIGH_FLOW));
    CHECK(config.option<ConfigOptionInts>("printer_extruder_id")->values == std::vector<int>{1, 1, 2, 2, 3, 3, 4, 4});
    CHECK(strings_of(config, "printer_extruder_variant") ==
          std::vector<std::string>{STANDARD, HIGH_FLOW, STANDARD, HIGH_FLOW, STANDARD, HIGH_FLOW, STANDARD, HIGH_FLOW});

    // Tool heads and filaments agree in this file: nothing to report.
    CHECK(head_flow_types(config) == std::vector<int>{int(nvtStandard), int(nvtHighFlow), int(nvtStandard), int(nvtStandard)});
    CHECK(report.empty());
    CHECK_FALSE(report.custom_grouping);

    CHECK_FALSE(has_snapmaker_flow_keys(config));
    // Nothing left to do for a second pass.
    CHECK_FALSE(normalize_snapmaker_flow_config(config, report));
}

TEST_CASE("A Snapmaker Orca 2.4 project loads with its High Flow tool head and the High Flow values of its presets", "[HighFlow][FlowCompat][hf_2_4_project]")
{
    const auto    loaded = load_snapmaker_bundle();
    PresetBundle &bundle = *loaded;

    bundle.load_config_model("upstream_2_4.3mf", load_project_2_4(), Semver(2, 4, 0));

    CHECK(bundle.last_flow_import_report.empty());
    CHECK(bundle.printers.get_edited_preset().name == U1_MACHINE);
    CHECK(bundle.prints.get_edited_preset().name == U1_PROCESS);
    CHECK(bundle.filament_presets == std::vector<std::string>{"Snapmaker PLA SnapSpeed @U1", "Snapmaker PETG HF", "Snapmaker PLA Basic @U1", "Snapmaker PLA Matte @U1"});
    check_head_2_prints_high_flow(bundle);

    SECTION("saved as a project of this application and loaded again, it is the same plate") {
        Model model;
        REQUIRE(load_stl((std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl").c_str(), &model));
        model.add_default_instances();
        ScopedTemporaryDir backup_dir("orca_hf_24");
        model.set_backup_path(backup_dir.string());

        DynamicPrintConfig project = bundle.full_config_secure();
        CHECK_FALSE(has_snapmaker_flow_keys(project));
        CHECK(project_schema_version_for(project) == 2);

        ScopedTemporaryFile temp(".3mf");
        StoreParams         store_params;
        const std::string path = temp.string();
        store_params.path     = path.c_str();
        store_params.model    = &model;
        store_params.config   = &project;
        store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
        PlateData *plate = new PlateData();
        plate->plate_index = 0;
        store_params.plate_data_list.push_back(plate);
        REQUIRE(store_bbs_3mf(store_params));

        Model                     dst_model;
        ScopedTemporaryDir        dst_backup_dir("orca_hf_24_dst");
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
        // Schema 2: Snapmaker Orca 2.4 says that it cannot fully load the file; this application knows the schema.
        CHECK(ProjectSchemaRegistry::version_from(dst_config) == 2);
        CHECK_FALSE(ProjectSchemaRegistry::is_newer(dst_config));

        const auto    reloaded = load_snapmaker_bundle();
        PresetBundle &second   = *reloaded;
        Preset::normalize(dst_config);
        second.load_config_model("saved_here.3mf", std::move(dst_config), file_version);
        CHECK(second.last_flow_import_report.empty());
        CHECK(second.printers.get_edited_preset().name == U1_MACHINE);
        CHECK(second.filament_presets == bundle.filament_presets);
        check_head_2_prints_high_flow(second);

        release_PlateData_list(dst_plates);
        delete plate;
    }
}

TEST_CASE("Values changed in a Snapmaker Orca 2.4 project land in the column they were changed in", "[HighFlow][FlowCompat][hf_2_4_project]")
{
    // For an unchanged system preset this application takes its own values, so the columns of the
    // file only matter for what the user changed. Here the High Flow speed limit of filament 2 and
    // the High Flow wall speed of the process were edited in 2.4 before the project was saved.
    DynamicPrintConfig config = load_project_2_4();
    auto *speeds = config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    REQUIRE(speeds != nullptr);
    REQUIRE(speeds->values.size() == 7);
    speeds->values[3] = 33.; // filament 2, High Flow
    config.option<ConfigOptionFloats>("outer_wall_speed")->values = {200., 450.};
    auto *different = config.option<ConfigOptionStrings>("different_settings_to_system", true);
    different->values.resize(6);
    different->values[0] = "outer_wall_speed";
    different->values[2] = "filament_max_volumetric_speed";

    const auto    loaded = load_snapmaker_bundle();
    PresetBundle &bundle = *loaded;
    bundle.load_config_model("upstream_2_4_edited.3mf", std::move(config), Semver(2, 4, 0));

    REQUIRE(bundle.filament_presets.size() == 4);
    const Preset *petg = bundle.filaments.find_preset(bundle.filament_presets[1], false);
    REQUIRE(petg != nullptr);
    CHECK_THAT(petg->config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values, Catch::Matchers::Approx(std::vector<double>{20., 33.}));
    // The neighbours keep their own values.
    const Preset *snapspeed = bundle.filaments.find_preset(bundle.filament_presets[0], false);
    const Preset *shipped   = bundle.filaments.find_preset("Snapmaker PLA SnapSpeed @U1", false, true);
    REQUIRE(snapspeed != nullptr);
    REQUIRE(shipped != nullptr);
    CHECK_THAT(snapspeed->config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values,
               Catch::Matchers::Approx(shipped->config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values));
    CHECK_THAT(bundle.prints.get_edited_preset().config.option<ConfigOptionFloats>("outer_wall_speed")->values, Catch::Matchers::Approx(std::vector<double>{200., 450.}));

    const DynamicPrintConfig composed = composed_per_head(bundle);
    CHECK_THAT(composed.option<ConfigOptionFloats>("filament_max_volumetric_speed")->get_at(1), Catch::Matchers::WithinAbs(33., 1e-9));
    CHECK_THAT(composed.option<ConfigOptionFloats>("outer_wall_speed")->values, Catch::Matchers::Approx(std::vector<double>{200., 450., 200., 200.}));
}

TEST_CASE("A filament flow type of Snapmaker Orca 2.4 that differs from its tool head moves the tool head", "[HighFlow][FlowCompat][hf_2_4_reconcile]")
{
    // 2.4 printed a filament with the values of the filament's own flow type, whatever the nozzle.
    DynamicPrintConfig config;
    config.set_deserialize_strict("filament_colour", "#111111;#222222;#333333;#444444;#555555");
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4, 0.4, 0.4}));
    config.set_key_value("filament_flow_step_size", new ConfigOptionInts({1, 1, 1, 1, 1}));
    config.set_deserialize_strict("nozzle_volume_type", "standard,high_flow,standard,high_flow");
    FlowImportReport report;

    SECTION("filament i is printed by tool head i, the filaments beyond the tool heads by tool head 1") {
        // Head 1: filaments 1 and 5 agree on High Flow. Head 2: filament 2 asks for Standard.
        // Head 3: agrees. Head 4: agrees.
        config.set_deserialize_strict("filament_volume_type", "high_flow;standard;standard;high_flow;high_flow");
        REQUIRE(normalize_snapmaker_flow_config(config, report));
        CHECK(head_flow_types(config) == std::vector<int>{int(nvtHighFlow), int(nvtStandard), int(nvtStandard), int(nvtHighFlow)});
        REQUIRE(report.changed_heads.size() == 2);
        CHECK(report.changed_heads[0].head == 1);
        CHECK(report.changed_heads[0].to_high_flow);
        CHECK(report.changed_heads[1].head == 2);
        CHECK_FALSE(report.changed_heads[1].to_high_flow);
        CHECK(report.dropped_filaments.empty());
    }
    SECTION("filaments of one tool head that disagree leave the tool head alone and are listed") {
        config.set_deserialize_strict("filament_volume_type", "high_flow;high_flow;standard;high_flow;standard");
        REQUIRE(normalize_snapmaker_flow_config(config, report));
        CHECK(head_flow_types(config) == std::vector<int>{int(nvtStandard), int(nvtHighFlow), int(nvtStandard), int(nvtHighFlow)});
        CHECK(report.changed_heads.empty());
        REQUIRE(report.dropped_filaments.size() == 1);
        CHECK(report.dropped_filaments[0].filament == 1);
        CHECK(report.dropped_filaments[0].head == 1);
        CHECK(report.dropped_filaments[0].wanted_high_flow);
    }
    SECTION("a manual filament map is binding") {
        config.set_deserialize_strict("filament_map_mode", "Manual");
        config.set_key_value("filament_map", new ConfigOptionInts({3, 3, 3, 3, 3}));
        config.set_deserialize_strict("filament_volume_type", "high_flow;high_flow;high_flow;high_flow;high_flow");
        config.set_key_value("filament_grouping_mode", new ConfigOptionString("custom"));
        REQUIRE(normalize_snapmaker_flow_config(config, report));
        CHECK(head_flow_types(config) == std::vector<int>{int(nvtStandard), int(nvtHighFlow), int(nvtHighFlow), int(nvtHighFlow)});
        REQUIRE(report.changed_heads.size() == 1);
        CHECK(report.changed_heads[0].head == 3);
        CHECK(report.custom_grouping);
    }
    SECTION("the stored map of an automatic mode is not") {
        config.set_deserialize_strict("filament_map_mode", "Auto For Flush");
        config.set_key_value("filament_map", new ConfigOptionInts({1, 1, 1, 1, 1}));
        config.set_deserialize_strict("filament_volume_type", "standard;high_flow;standard;high_flow;standard");
        REQUIRE(normalize_snapmaker_flow_config(config, report));
        CHECK(report.empty());
    }
}

TEST_CASE("A user filament preset of Snapmaker Orca 2.4 keeps its High Flow values", "[HighFlow][FlowCompat][hf_2_4_user_preset]")
{
    const auto    loaded = load_snapmaker_bundle();
    PresetBundle &bundle = *loaded;

    ScopedTemporaryDir user_dir("orca_hf_24_user");
    fs::create_directories(fs::path(user_dir.string()) / "filament");
    fs::create_directories(fs::path(user_dir.string()) / "process");
    fs::copy_file(fixture("user_filament.json"), fs::path(user_dir.string()) / "filament" / "My SnapSpeed 2.4.json");
    fs::copy_file(fixture("user_process.json"), fs::path(user_dir.string()) / "process" / "My 0.20 2.4.json");

    PresetsConfigSubstitutions substitutions;
    bundle.filaments.load_presets(user_dir.string(), "filament", substitutions, ForwardCompatibilitySubstitutionRule::Enable, nullptr, PresetOrigin(), true);
    bundle.prints.load_presets(user_dir.string(), "process", substitutions, ForwardCompatibilitySubstitutionRule::Enable, nullptr, PresetOrigin(), true);

    const Preset *filament = bundle.filaments.find_preset("My SnapSpeed 2.4", false);
    const Preset *parent   = bundle.filaments.find_preset("Snapmaker PLA SnapSpeed @U1", false, true);
    REQUIRE(filament != nullptr);
    REQUIRE(parent != nullptr);
    CHECK(strings_of(filament->config, "filament_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW});
    CHECK_THAT(filament->config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values, Catch::Matchers::Approx(std::vector<double>{19., 33.}));
    CHECK(filament->config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{222, 233});
    // What the file does not state comes from the parent, column by column.
    CHECK_THAT(filament->config.option<ConfigOptionFloats>("pressure_advance")->values,
               Catch::Matchers::Approx(parent->config.option<ConfigOptionFloats>("pressure_advance")->values));
    CHECK(filament->config.option("filament_flow_support") == nullptr);

    const Preset *process = bundle.prints.find_preset("My 0.20 2.4", false);
    REQUIRE(process != nullptr);
    CHECK(process->config.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{1, 1});
    CHECK(strings_of(process->config, "print_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW});
    CHECK_THAT(process->config.option<ConfigOptionFloats>("outer_wall_speed")->values, Catch::Matchers::Approx(std::vector<double>{150., 420.}));
    CHECK(process->config.option("process_flow_support") == nullptr);
    // ironing_speed is a single value here: the Standard value of the file, the other one is reported.
    CHECK_THAT(process->config.opt_float("ironing_speed"), Catch::Matchers::WithinAbs(30., 1e-9));
    bool ironing_reported = false;
    for (const PresetConfigSubstitutions &preset_substitutions : substitutions)
        for (const ConfigSubstitution &substitution : preset_substitutions.substitutions)
            if (substitution.opt_def != nullptr && substitution.opt_def->opt_key == "ironing_speed") {
                ironing_reported = true;
                CHECK(substitution.old_value == "30,45");
            }
    CHECK(ironing_reported);
}

TEST_CASE("Process values that Snapmaker Orca 2.4 stores per flow type and this application once keep the Standard value", "[HighFlow][FlowCompat][hf_2_4_scalars]")
{
    for (const char *key : {"ironing_speed", "slow_down_layers", "accel_to_decel_enable", "accel_to_decel_factor", "max_volumetric_extrusion_rate_slope",
                            "max_volumetric_extrusion_rate_slope_segment_length", "extrusion_rate_smoothing_external_perimeter_only"}) {
        INFO(key);
        CHECK(is_snapmaker_flow_scalar_key(key));
        const ConfigOptionDef *def = print_config_def.get(key);
        REQUIRE(def != nullptr);
        CHECK(def->is_scalar());
    }
    CHECK_FALSE(is_snapmaker_flow_scalar_key("outer_wall_speed"));
}

TEST_CASE("The project schema number tells Snapmaker Orca 2.4 whether it can read the columns of a project", "[HighFlow][FlowCompat][hf_schema]")
{
    const ProjectSchemaDefinition &schema = ProjectSchemaRegistry::definition();
    CHECK(schema.config_key == std::string("project_schema_version"));
    CHECK(schema.current_version == 2);
    CHECK(schema.legacy_version == 1);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    // No part of the slicing configuration.
    CHECK(config.option(schema.config_key) == nullptr);
    CHECK(ProjectSchemaRegistry::version_from(config) == 1);
    config.set_key_value(schema.config_key, new ConfigOptionInt(2));
    CHECK_FALSE(ProjectSchemaRegistry::is_newer(config));
    config.set_key_value(schema.config_key, new ConfigOptionInt(3));
    CHECK(ProjectSchemaRegistry::is_newer(config));

    config.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("filament_extruder_variant", new ConfigOptionStrings({STANDARD, STANDARD}));
    CHECK(project_schema_version_for(config) == 1);

    SECTION("a filament with two columns") {
        config.set_key_value("filament_extruder_variant", new ConfigOptionStrings({STANDARD, HIGH_FLOW, STANDARD}));
        CHECK(project_schema_version_for(config) == 2);
    }
    SECTION("a process with two columns") {
        config.set_key_value("outer_wall_speed", new ConfigOptionFloats({200., 500.}));
        CHECK(project_schema_version_for(config) == 2);
    }
    SECTION("a single column process whose id list was widened to the printer's columns") {
        config.set_key_value("print_extruder_id", new ConfigOptionInts({1, 1, 2, 2}));
        config.set_key_value("print_extruder_variant", new ConfigOptionStrings({STANDARD, HIGH_FLOW, STANDARD, HIGH_FLOW}));
        CHECK(project_schema_version_for(config) == 1);
    }
}

TEST_CASE("A U1 plate whose presets carry one column each stays a schema 1 project", "[HighFlow][FlowCompat][hf_schema]")
{
    // The 0.2 mm presets of the U1 declare no High Flow column (there are no High Flow values
    // for that size), so a project saved with them can be read by Snapmaker Orca 2.4 in full.
    const auto    loaded = load_snapmaker_bundle();
    PresetBundle &bundle = *loaded;
    REQUIRE(bundle.printers.select_preset_by_name("Snapmaker U1 (0.2 nozzle)", true));
    REQUIRE(bundle.prints.select_preset_by_name("0.12mm Standard @Snapmaker U1 (0.2 nozzle)", true));
    bundle.filament_presets = {"Snapmaker PLA SnapSpeed @U1 0.2 nozzle", "Generic PLA @U1 0.2 nozzle",
                               "Snapmaker ABS @U1 0.2 nozzle", "Snapmaker ASA @U1 0.2 nozzle"};
    REQUIRE(bundle.filaments.select_preset_by_name(bundle.filament_presets.front(), true));
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2, 3, 4};
    bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = std::vector<int>(4, int(nvtStandard));

    const DynamicPrintConfig project = bundle.full_config_secure();
    CHECK(strings_of(project, "filament_extruder_variant") == std::vector<std::string>(4, STANDARD));
    CHECK(project.option<ConfigOptionFloats>("outer_wall_speed")->size() == 1);
    CHECK(project_schema_version_for(project) == 1);
}

TEST_CASE("The shipped U1 0.4 mm plate is a schema 2 project even with every tool head on Standard", "[HighFlow][FlowCompat][hf_schema]")
{
    // The 0.4 mm presets carry a Standard and a High Flow column, so a project saved with them
    // holds two columns per preset, which Snapmaker Orca 2.4 cannot read in full: schema 2,
    // whatever the tool heads are set to.
    const auto    loaded = load_snapmaker_bundle();
    PresetBundle &bundle = *loaded;
    REQUIRE(bundle.printers.select_preset_by_name(U1_MACHINE, true));
    REQUIRE(bundle.prints.select_preset_by_name(U1_PROCESS, true));
    bundle.filament_presets = {"Snapmaker PLA SnapSpeed @U1", "Snapmaker PETG HF", "Snapmaker PLA Matte @U1", "Snapmaker ABS @U1 0.4 nozzle"};
    REQUIRE(bundle.filaments.select_preset_by_name(bundle.filament_presets.front(), true));
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2, 3, 4};
    bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = std::vector<int>(4, int(nvtStandard));

    const DynamicPrintConfig project = bundle.full_config_secure();
    CHECK(strings_of(project, "filament_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW, STANDARD, HIGH_FLOW, STANDARD, HIGH_FLOW, STANDARD, HIGH_FLOW});
    CHECK(project.option<ConfigOptionFloats>("outer_wall_speed")->size() == 2);
    CHECK(strings_of(project, "printer_extruder_variant").size() == 8);
    CHECK(project_schema_version_for(project) == 2);
}

namespace {

// The all-Standard 0.4 plate as a project config: the printer preset with its eight columns.
DynamicPrintConfig all_standard_0_4_project(PresetBundle &bundle)
{
    REQUIRE(bundle.printers.select_preset_by_name(U1_MACHINE, true));
    REQUIRE(bundle.prints.select_preset_by_name(U1_PROCESS, true));
    bundle.filament_presets = {"Snapmaker PLA SnapSpeed @U1", "Snapmaker PETG HF", "Snapmaker PLA Matte @U1", "Snapmaker ABS @U1 0.4 nozzle"};
    REQUIRE(bundle.filaments.select_preset_by_name(bundle.filament_presets.front(), true));
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2, 3, 4};
    bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = std::vector<int>(4, int(nvtStandard));
    DynamicPrintConfig config = bundle.full_config_secure();
    // The loader counts the filaments of a project by its colours (a fresh bundle holds one).
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}));
    // print, four filaments, printer: the printer's entry is the last one.
    config.option<ConfigOptionStrings>("different_settings_to_system", true)->values = std::vector<std::string>(6, std::string());
    return config;
}

// Keeps the values at `picks` of a vector option, in that order. Value by value rather than
// through the text form: strings serialize with ';', numbers with ','.
void keep_columns(ConfigOptionVectorBase &option, const std::vector<size_t> &picks)
{
    const std::unique_ptr<ConfigOption> wide(option.clone());
    REQUIRE(picks.size() <= option.size());
    option.resize(picks.size(), wide.get());
    for (size_t i = 0; i < picks.size(); ++i)
        option.set_at(wide.get(), i, picks[i]);
    REQUIRE(option.size() == picks.size());
}

// The printer part of a project saved before its printer preset declared a High Flow column:
// one Standard column per tool head, holding the Standard value of each head.
void narrow_printer_to_one_column_per_head(DynamicPrintConfig &config)
{
    const size_t heads = config.option<ConfigOptionFloats>("nozzle_diameter")->size();
    for (const std::set<std::string> *keys : {&printer_options_with_variant_1, &printer_options_with_variant_2}) {
        const size_t stride = keys == &printer_options_with_variant_2 ? 2 : 1;
        for (const std::string &key : *keys) {
            if (key == "printer_extruder_id" || key == "printer_extruder_variant")
                continue;
            auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
            if (option == nullptr || option->size() != 2 * stride * heads)
                continue;
            std::vector<size_t> picks;
            for (size_t head = 0; head < heads; ++head)
                for (size_t k = 0; k < stride; ++k)
                    picks.emplace_back(head * 2 * stride + k);
            keep_columns(*option, picks);
        }
    }
    std::vector<int> ids;
    for (size_t head = 0; head < heads; ++head)
        ids.emplace_back(int(head) + 1);
    config.set_key_value("printer_extruder_id",      new ConfigOptionInts(ids));
    config.set_key_value("printer_extruder_variant", new ConfigOptionStrings(std::vector<std::string>(heads, STANDARD)));
    config.set_key_value("extruder_variant_list",    new ConfigOptionStrings(std::vector<std::string>(heads, STANDARD)));
}

} // namespace

TEST_CASE("A project saved with one column per tool head fills both columns of its printer preset", "[HighFlow][FlowCompat][hf_project_fill_in]")
{
    // Snapmaker U1 (0.4 nozzle) declares a Standard and a High Flow column per tool head; a
    // project saved before that declaration holds one column per head. The retraction of tool
    // head 2 was changed in that project (2.0 against the preset's 1.5).
    const auto    source = load_snapmaker_bundle();
    const Preset *system = source->printers.find_preset(U1_MACHINE, false, true);
    REQUIRE(system != nullptr);
    REQUIRE(strings_of(system->config, "printer_extruder_variant").size() == 8);
    CHECK_THAT(system->config.option<ConfigOptionFloats>("retraction_length")->values, Catch::Matchers::Approx(std::vector<double>(8, 1.5)));

    DynamicPrintConfig config = all_standard_0_4_project(*source);
    config.option<ConfigOptionStrings>("different_settings_to_system")->values[5] = "retraction_length";

    SECTION("one column per tool head: the changed value reaches the Standard and the High Flow column of its head") {
        narrow_printer_to_one_column_per_head(config);
        REQUIRE(config.option<ConfigOptionFloats>("retraction_length")->size() == 4);
        config.option<ConfigOptionFloats>("retraction_length")->values[1] = 2.0;

        const auto    loaded = load_snapmaker_bundle();
        PresetBundle &bundle = *loaded;
        bundle.load_config_model("saved_before_the_columns.3mf", std::move(config), Semver());
        CHECK(bundle.last_flow_import_report.empty());

        const DynamicPrintConfig &edited = bundle.printers.get_edited_preset().config;
        CHECK(strings_of(edited, "printer_extruder_variant") == strings_of(system->config, "printer_extruder_variant"));
        CHECK_THAT(edited.option<ConfigOptionFloats>("retraction_length")->values,
                   Catch::Matchers::Approx(std::vector<double>{1.5, 1.5, 2.0, 2.0, 1.5, 1.5, 1.5, 1.5}));
        // Every other machine value is the system preset's, in every column.
        for (const std::set<std::string> *keys : {&printer_options_with_variant_1, &printer_options_with_variant_2})
            for (const std::string &key : *keys) {
                if (key == "retraction_length" || system->config.option(key) == nullptr)
                    continue;
                INFO(key);
                REQUIRE(edited.option(key) != nullptr);
                CHECK(*edited.option(key) == *system->config.option(key));
            }
        // Switching tool head 2 to High Flow keeps the changed retraction (invariant M1).
        bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {int(nvtStandard), int(nvtHighFlow), int(nvtStandard), int(nvtStandard)};
        const DynamicPrintConfig composed = composed_per_head(bundle);
        CHECK_THAT(composed.option<ConfigOptionFloats>("retraction_length")->values, Catch::Matchers::Approx(std::vector<double>{1.5, 2.0, 1.5, 1.5}));
    }

    SECTION("a project saved with every column keeps each column's own value") {
        REQUIRE(config.option<ConfigOptionFloats>("retraction_length")->size() == 8);
        config.option<ConfigOptionFloats>("retraction_length")->values[2] = 2.0; // head 2, Standard
        config.option<ConfigOptionFloats>("retraction_length")->values[3] = 1.7; // head 2, High Flow

        const auto    loaded = load_snapmaker_bundle();
        PresetBundle &bundle = *loaded;
        bundle.load_config_model("saved_with_the_columns.3mf", std::move(config), Semver());
        CHECK_THAT(bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("retraction_length")->values,
                   Catch::Matchers::Approx(std::vector<double>{1.5, 1.5, 2.0, 1.7, 1.5, 1.5, 1.5, 1.5}));
    }
}

// ---------------------------------------------------------------------------------------------
// 0.6 mm plate whose machine preset declares High Flow, via tests/snapmaker_high_flow_fixture.hpp
// (Snapmaker ships High Flow for 0.4 mm only). Projects saved before the columns, in 2.4 and own shape.
// ---------------------------------------------------------------------------------------------

namespace {

const char *const U1_MACHINE_0_6 = "Snapmaker U1 (0.6 nozzle)";
const char *const U1_PROCESS_0_6 = "0.30mm Standard @Snapmaker U1 (0.6 nozzle)";
const std::vector<std::string> U1_FILAMENTS_0_6{"Snapmaker PLA SnapSpeed @U1 0.6 nozzle", "Snapmaker PETG HF @U1 0.6 nozzle",
                                                "Snapmaker PLA Matte @U1 0.6 nozzle", "Snapmaker ABS @U1 0.6 nozzle"};

// The shipped vendor, its 0.6 mm machine preset declaring High Flow for every tool head.
std::unique_ptr<PresetBundle> load_bundle_with_0_6_high_flow()
{
    auto bundle = load_snapmaker_bundle();
    Slic3r::Test::u1_0_6_declares_high_flow(*bundle);
    return bundle;
}

// The all-Standard 0.6 plate as a project config: the printer preset with its eight columns,
// the process and the filaments with their one shipped column.
DynamicPrintConfig all_standard_0_6_project(PresetBundle &bundle)
{
    REQUIRE(bundle.printers.select_preset_by_name(U1_MACHINE_0_6, true));
    REQUIRE(bundle.prints.select_preset_by_name(U1_PROCESS_0_6, true));
    bundle.filament_presets = U1_FILAMENTS_0_6;
    REQUIRE(bundle.filaments.select_preset_by_name(bundle.filament_presets.front(), true));
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2, 3, 4};
    bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = std::vector<int>(4, int(nvtStandard));
    DynamicPrintConfig config = bundle.full_config_secure();
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}));
    config.option<ConfigOptionStrings>("different_settings_to_system", true)->values = std::vector<std::string>(6, std::string());
    return config;
}

// Keeps the first of every `columns` values of the keys in `keys` whose width is `columns * count`
// (the Standard column of each of `count` presets), as a file without the columns holds them.
void keep_standard_columns(DynamicPrintConfig &config, const std::set<std::string> &keys, size_t columns, size_t count)
{
    for (const std::string &key : keys) {
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        if (option == nullptr || option->size() != columns * count)
            continue;
        std::vector<size_t> picks;
        for (size_t i = 0; i < count; ++i)
            picks.emplace_back(i * columns);
        keep_columns(*option, picks);
    }
}

// A Snapmaker Orca 2.4 project of a plate without High Flow values: no variant keys, one value per
// filament / process / tool head, all flow types "standard", filament_flow_step_size 1, no *_flow_support.
void shape_as_2_4_without_high_flow(DynamicPrintConfig &config)
{
    const size_t filaments = config.option<ConfigOptionStrings>("filament_settings_id")->size();
    keep_standard_columns(config, filament_options_with_variant, 2, filaments);
    keep_standard_columns(config, print_options_with_variant, 2, 1);
    narrow_printer_to_one_column_per_head(config);
    for (const char *key : {"filament_extruder_variant", "filament_self_index", "print_extruder_id", "print_extruder_variant",
                            "printer_extruder_id", "printer_extruder_variant", "extruder_variant_list"})
        config.erase(key);
    config.set_key_value("filament_flow_step_size", new ConfigOptionInts(std::vector<int>(filaments, 1)));
    config.set_deserialize_strict("filament_volume_type", "standard;standard;standard;standard");
    config.set_deserialize_strict("nozzle_volume_type", "standard,standard,standard,standard");
}

} // namespace

TEST_CASE("A Snapmaker Orca 2.4 project of the 0.6 mm plate loads with every tool head on Standard", "[HighFlow][FlowCompat][hf_2_4_project]")
{
    const auto         source = load_bundle_with_0_6_high_flow();
    DynamicPrintConfig config = all_standard_0_6_project(*source);
    shape_as_2_4_without_high_flow(config);
    REQUIRE(has_snapmaker_flow_keys(config));
    REQUIRE(config.option<ConfigOptionFloats>("outer_wall_speed")->size() == 1);
    REQUIRE(config.option<ConfigOptionFloats>("retraction_length")->size() == 4);

    const auto    loaded = load_bundle_with_0_6_high_flow();
    PresetBundle &bundle = *loaded;
    bundle.load_config_model("u1_0_6_from_2_4.3mf", std::move(config), Semver(2, 4, 0));

    CHECK(bundle.last_flow_import_report.empty());
    CHECK(bundle.printers.get_edited_preset().name == U1_MACHINE_0_6);
    CHECK(bundle.prints.get_edited_preset().name == U1_PROCESS_0_6);
    CHECK(bundle.filament_presets == U1_FILAMENTS_0_6);
    CHECK(head_flow_types(bundle.project_config) == std::vector<int>(4, int(nvtStandard)));
    // The plate is the 0.6 mm preset with its columns; the file's one value per tool head landed in both.
    const DynamicPrintConfig &edited = bundle.printers.get_edited_preset().config;
    CHECK(strings_of(edited, "printer_extruder_variant").size() == 8);
    CHECK_THAT(edited.option<ConfigOptionFloats>("retraction_length")->values, Catch::Matchers::Approx(std::vector<double>(8, 1.4)));
    // The process has one column, used for every tool head.
    const DynamicPrintConfig composed = composed_per_head(bundle);
    CHECK(composed.option<ConfigOptionFloats>("outer_wall_speed")->size() == 4);
    CHECK_THAT(composed.option<ConfigOptionFloats>("outer_wall_speed")->values, Catch::Matchers::Approx(std::vector<double>(4, 120.)));
}

TEST_CASE("A 0.6 mm project saved with one printer column per tool head fills both columns of its printer preset", "[HighFlow][FlowCompat][hf_project_fill_in]")
{
    // The same situation as for the 0.4 mm plate above, on a 0.6 mm preset that declares High
    // Flow: a project saved before the declaration holds one column per tool head, with the
    // retraction of tool head 2 changed (2.0 against the preset's 1.4).
    const auto    source = load_bundle_with_0_6_high_flow();
    const Preset *system = source->printers.find_preset(U1_MACHINE_0_6, false, true);
    REQUIRE(system != nullptr);
    REQUIRE(strings_of(system->config, "printer_extruder_variant").size() == 8);
    CHECK_THAT(system->config.option<ConfigOptionFloats>("retraction_length")->values, Catch::Matchers::Approx(std::vector<double>(8, 1.4)));

    DynamicPrintConfig config = all_standard_0_6_project(*source);
    config.option<ConfigOptionStrings>("different_settings_to_system")->values[5] = "retraction_length";
    narrow_printer_to_one_column_per_head(config);
    REQUIRE(config.option<ConfigOptionFloats>("retraction_length")->size() == 4);
    config.option<ConfigOptionFloats>("retraction_length")->values[1] = 2.0;

    const auto    loaded = load_bundle_with_0_6_high_flow();
    PresetBundle &bundle = *loaded;
    bundle.load_config_model("saved_before_the_0_6_columns.3mf", std::move(config), Semver());
    CHECK(bundle.last_flow_import_report.empty());

    const DynamicPrintConfig &edited = bundle.printers.get_edited_preset().config;
    CHECK(strings_of(edited, "printer_extruder_variant") == strings_of(system->config, "printer_extruder_variant"));
    CHECK_THAT(edited.option<ConfigOptionFloats>("retraction_length")->values,
               Catch::Matchers::Approx(std::vector<double>{1.4, 1.4, 2.0, 2.0, 1.4, 1.4, 1.4, 1.4}));
    // Tool head 2 declares both flow types on this 0.6 mm plate, so its Flow row is a choice
    // (HighFlowNotices::flow_row_state on this config; the rule itself is a GUI library function).
    REQUIRE(strings_of(edited, "extruder_variant_list").size() == 4);
    CHECK(strings_of(edited, "extruder_variant_list")[1] == std::string(STANDARD) + "," + HIGH_FLOW);
    // Switching tool head 2 to High Flow keeps the changed retraction (invariant M1).
    bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {int(nvtStandard), int(nvtHighFlow), int(nvtStandard), int(nvtStandard)};
    const DynamicPrintConfig composed = composed_per_head(bundle);
    CHECK_THAT(composed.option<ConfigOptionFloats>("retraction_length")->values, Catch::Matchers::Approx(std::vector<double>{1.4, 2.0, 1.4, 1.4}));
}

TEST_CASE("A user filament preset of Snapmaker Orca 2.4 with High Flow values keeps both columns over a parent without them", "[HighFlow][FlowCompat][FilamentFlow]")
{
    const auto    loaded = load_snapmaker_bundle();
    PresetBundle &bundle = *loaded;
    const Preset *parent = bundle.filaments.find_preset("Generic PETG", false, true);
    REQUIRE(parent != nullptr);
    REQUIRE(strings_of(parent->config, "filament_extruder_variant") == std::vector<std::string>{STANDARD});

    ScopedTemporaryDir user_dir("orca_hf_24_one_column");
    fs::create_directories(fs::path(user_dir.string()) / "filament");
    {
        boost::nowide::ofstream out((fs::path(user_dir.string()) / "filament" / "My PETG 2.4.json").string());
        out << R"({
    "type": "filament",
    "name": "My PETG 2.4",
    "from": "User",
    "inherits": "Generic PETG",
    "version": "2.4.0",
    "filament_settings_id": ["My PETG 2.4"],
    "filament_flow_support": ["standard", "high_flow"],
    "nozzle_temperature": ["250", "265"]
})";
    }
    PresetsConfigSubstitutions substitutions;
    bundle.filaments.load_presets(user_dir.string(), "filament", substitutions, ForwardCompatibilitySubstitutionRule::Enable, nullptr, PresetOrigin(), true);

    const Preset *filament = bundle.filaments.find_preset("My PETG 2.4", false);
    REQUIRE(filament != nullptr);
    REQUIRE(filament->name == "My PETG 2.4");
    CHECK(strings_of(filament->config, "filament_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW});
    CHECK(filament->config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{250, 265});
    CHECK(filament_columns_consistent(filament->config));
    // A key the file does not state holds the parent's value in both columns.
    const double speed = parent->config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->get_at(0);
    const auto  *speeds = filament->config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    REQUIRE(speeds->values.size() == 2);
    CHECK_THAT(speeds->values[0], Catch::Matchers::WithinAbs(speed, 1e-9));
    CHECK_THAT(speeds->values[1], Catch::Matchers::WithinAbs(speed, 1e-9));
}

TEST_CASE("The project config lists a filament's High Flow column right after its Standard column", "[HighFlow][FlowCompat][FilamentFlow]")
{
    // Snapmaker Orca 2.4 reads filament i at column i of every per-column key and has no segment
    // table here (no filament_flow_step_size): in such a project it reads the second filament from
    // the first one's High Flow column. This pins the layout that it misreads.
    const auto    loaded = load_snapmaker_bundle();
    PresetBundle &bundle = *loaded;
    REQUIRE(bundle.printers.select_preset_by_name(U1_MACHINE, true));
    REQUIRE(bundle.prints.select_preset_by_name(U1_PROCESS, true));
    bundle.filament_presets = {"Generic PETG", "Generic PLA", "Generic PLA", "Generic PLA"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour", true)->values.assign(4, "#FFFFFF");
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2, 3, 4};
    bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {int(nvtStandard), int(nvtHighFlow), int(nvtStandard), int(nvtStandard)};
    REQUIRE(bundle.filaments.select_preset_by_name("Generic PETG", true));
    DynamicPrintConfig &petg = bundle.filaments.get_edited_preset().config;
    REQUIRE(filament_add_flow_column(petg, nvtHighFlow));
    petg.option<ConfigOptionInts>("nozzle_temperature")->values[1] = 265;

    const DynamicPrintConfig project = bundle.full_config_secure();
    CHECK(strings_of(project, "filament_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW, STANDARD, STANDARD, STANDARD});
    CHECK(project.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>{1, 1, 2, 3, 4});
    const std::vector<int> temperatures = project.option<ConfigOptionInts>("nozzle_temperature")->values;
    REQUIRE(temperatures.size() == 5);
    CHECK(temperatures[1] == 265);
    CHECK(project.option("filament_flow_step_size") == nullptr);
    CHECK(project_schema_version_for(project) == 2);
}
