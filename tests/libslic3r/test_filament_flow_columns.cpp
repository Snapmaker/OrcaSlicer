#include <catch2/catch_all.hpp>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/FilamentFlowColumns.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ProjectSchemaVersion.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/SnapmakerFlowCompat.hpp"

#include "test_utils.hpp"

using namespace Slic3r;

// The Standard / High Flow columns of a filament preset (libslic3r/FilamentFlowColumns.hpp): adding,
// checking and repairing them, and their save and load.

namespace {

const char *const STANDARD  = "Direct Drive Standard";
const char *const HIGH_FLOW = "Direct Drive High Flow";

// A filament preset config with one Standard column: every filament key at its default.
DynamicPrintConfig one_column_filament()
{
    DynamicPrintConfig config;
    config.apply_only(FullPrintConfig::defaults(), Preset::filament_options());
    REQUIRE(config.option<ConfigOptionStrings>("filament_extruder_variant")->values == std::vector<std::string>{STANDARD});
    return config;
}

std::vector<std::string> texts_of(const DynamicPrintConfig &config, const std::string &key)
{
    const auto *option = dynamic_cast<const ConfigOptionVectorBase *>(config.option(key));
    REQUIRE(option != nullptr);
    return option->vserialize();
}

std::unique_ptr<PresetBundle> load_snapmaker_bundle()
{
    auto bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(PROFILES_DIR, "Snapmaker", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr,
                                          /*allow_cache=*/false);
    return bundle;
}

// A value for column 1 of `key` other than `avoid`, as text of a one-value vector.
std::string other_value(const std::string &key, const ConfigOptionVectorBase &option, const std::string &avoid, size_t k)
{
    switch (option.type()) {
    case coFloats: return std::to_string(100.5 + double(k));
    case coInts: return std::to_string(100 + int(k));
    case coPercents: return std::to_string(11 + int(k)) + "%";
    case coFloatsOrPercents: return std::to_string(1 + int(k)) + ".5";
    case coBools: return avoid == "1" ? "0" : "1";
    case coStrings: return "hf" + std::to_string(k);
    case coEnums: {
        const ConfigOptionDef *def = print_config_def.get(key);
        REQUIRE(def != nullptr);
        for (const std::string &value : def->enum_values)
            if (value != avoid)
                return value;
        break;
    }
    default: break;
    }
    FAIL("no other value for " << key);
    return {};
}

// Sets column 1 of every present variant key of `config` to a value its `parent` does not hold there.
void set_distinct_high_flow_values(DynamicPrintConfig &config, const DynamicPrintConfig &parent)
{
    size_t k = 0;
    for (const std::string &key : filament_options_with_variant) {
        if (key == "filament_extruder_variant")
            continue;
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        if (option == nullptr)
            continue;
        INFO(key);
        REQUIRE(option->size() == 2);
        const auto       *theirs = dynamic_cast<const ConfigOptionVectorBase *>(parent.option(key));
        const std::string avoid  = theirs == nullptr ? std::string() : theirs->vserialize()[std::min<size_t>(1, theirs->size() - 1)];
        const std::unique_ptr<ConfigOption> value(option->clone());
        REQUIRE(value->deserialize(other_value(key, *option, avoid, k++)));
        option->set_at(value.get(), 1, 0);
        CHECK(option->vserialize()[1] != avoid);
    }
}

// Saves `child` as user preset `name` of `parent` into `dir`/filament and loads it again through the
// filament loader of `bundle`.
const Preset &save_and_reload(PresetBundle &bundle, const Preset &parent, const DynamicPrintConfig &child_config, const std::string &name,
                              const ScopedTemporaryDir &dir)
{
    {
        Preset child(Preset::TYPE_FILAMENT, name, false);
        child.config     = child_config;
        child.version    = parent.version;
        child.inherits() = parent.name;
        child.file       = (dir.path() / PRESET_FILAMENT_NAME / (name + ".json")).string();
        DynamicPrintConfig parent_config = parent.config;
        child.save(&parent_config);
        REQUIRE(boost::filesystem::exists(child.file));
    }
    PresetsConfigSubstitutions substitutions;
    bundle.filaments.load_presets(dir.path().string(), PRESET_FILAMENT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);
    const Preset *loaded = bundle.filaments.find_preset(name, false);
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->name == name);
    return *loaded;
}

// Every variant key of `written` comes back with both columns.
void check_round_trip(const DynamicPrintConfig &written, const DynamicPrintConfig &loaded)
{
    CHECK(texts_of(loaded, "filament_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW});
    for (const std::string &key : filament_options_with_variant) {
        if (key == "filament_extruder_variant" || written.option(key) == nullptr)
            continue;
        INFO(key);
        REQUIRE(loaded.option(key) != nullptr);
        CHECK(texts_of(loaded, key) == texts_of(written, key));
    }
    CHECK(filament_columns_consistent(loaded));
}

nlohmann::json json_of(const std::string &file)
{
    boost::nowide::ifstream in(file);
    return nlohmann::json::parse(in);
}

} // namespace

TEST_CASE("A filament key of another width than the column list is found and repaired", "[FilamentFlow]")
{
    DynamicPrintConfig config = one_column_filament();
    REQUIRE(filament_columns_consistent(config));

    SECTION("a value written past the last column is dropped") {
        // set_at grows a vector with its first value; nothing names that column.
        auto *speed = config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
        const ConfigOptionFloat value(40.);
        speed->set_at(&value, 1, 0);
        REQUIRE(speed->values.size() == 2);
        CHECK_FALSE(filament_columns_consistent(config));
        CHECK(filament_repair_columns(config));
        CHECK(speed->values.size() == 1);
        CHECK(filament_columns_consistent(config));
        CHECK_FALSE(filament_repair_columns(config));
    }
    SECTION("a short key of a two-column preset is padded with its first value") {
        config.option<ConfigOptionStrings>("filament_extruder_variant")->values = {STANDARD, HIGH_FLOW};
        CHECK_FALSE(filament_columns_consistent(config));
        CHECK(filament_repair_columns(config));
        CHECK(filament_columns_consistent(config));
        const auto *temperature = config.option<ConfigOptionInts>("nozzle_temperature");
        REQUIRE(temperature->values.size() == 2);
        CHECK(temperature->values[1] == temperature->values[0]);
    }
}

TEST_CASE("The volumetric speed check names only the columns below the limit", "[FilamentFlow]")
{
    DynamicPrintConfig config = one_column_filament();
    config.option<ConfigOptionStrings>("filament_extruder_variant")->values = {STANDARD, HIGH_FLOW};
    auto *speed = config.option<ConfigOptionFloats>("filament_max_volumetric_speed");

    speed->values = {12., 22.};
    CHECK(filament_columns_below(config, "filament_max_volumetric_speed", 0.5).empty());
    speed->values = {0.3, 22.};
    CHECK(filament_columns_below(config, "filament_max_volumetric_speed", 0.5) == std::vector<size_t>{0});
    speed->values = {12., 0.3};
    CHECK(filament_columns_below(config, "filament_max_volumetric_speed", 0.5) == std::vector<size_t>{1});
    CHECK(filament_columns_below(config, "no_such_key", 0.5).empty());
}

TEST_CASE("Every per-column filament key keeps both columns through save and load", "[FilamentFlow]")
{
    auto          bundle = load_snapmaker_bundle();
    ScopedTemporaryDir dir("orca_filament_columns");

    SECTION("a user preset of a one-column preset, given High Flow values") {
        const Preset *parent = bundle->filaments.find_preset("Generic PETG", false, true);
        REQUIRE(parent != nullptr);
        REQUIRE(texts_of(parent->config, "filament_extruder_variant") == std::vector<std::string>{STANDARD});
        DynamicPrintConfig child = parent->config;
        REQUIRE(filament_add_flow_column(child, nvtHighFlow));
        set_distinct_high_flow_values(child, parent->config);
        const Preset &loaded = save_and_reload(*bundle, *parent, child, "My PETG columns", dir);
        check_round_trip(child, loaded.config);
    }
    SECTION("a user preset of a two-column Snapmaker preset") {
        const Preset *parent = bundle->filaments.find_preset("Snapmaker PLA SnapSpeed @U1", false, true);
        REQUIRE(parent != nullptr);
        REQUIRE(texts_of(parent->config, "filament_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW});
        DynamicPrintConfig child = parent->config;
        set_distinct_high_flow_values(child, parent->config);
        const Preset &loaded = save_and_reload(*bundle, *parent, child, "My SnapSpeed columns", dir);
        check_round_trip(child, loaded.config);
    }
}

TEST_CASE("A saved filament preset writes its columns in full and without nil", "[FilamentFlow]")
{
    auto bundle = load_snapmaker_bundle();
    const Preset *parent = bundle->filaments.find_preset("Generic PETG", false, true);
    REQUIRE(parent != nullptr);
    REQUIRE(texts_of(parent->config, "filament_extruder_variant") == std::vector<std::string>{STANDARD});
    REQUIRE(texts_of(parent->config, "filament_flow_ratio") == std::vector<std::string>{"0.95"});
    REQUIRE(texts_of(parent->config, "filament_retraction_length") == std::vector<std::string>{"nil"});

    // The High Flow column as the Filament tab adds it, with a High Flow flow ratio of 0.90.
    DynamicPrintConfig child = parent->config;
    child.option<ConfigOptionStrings>("filament_extruder_variant")->values = {STANDARD, HIGH_FLOW};
    REQUIRE(filament_repair_columns(child));
    child.option<ConfigOptionFloatsNullable>("filament_flow_ratio")->values[1] = 0.90;

    ScopedTemporaryDir dir("orca_filament_json");
    Preset preset(Preset::TYPE_FILAMENT, "My PETG", false);
    preset.config     = child;
    preset.version    = parent->version;
    preset.inherits() = parent->name;
    preset.file       = (dir.path() / "My PETG.json").string();
    DynamicPrintConfig parent_config = parent->config;
    preset.save(&parent_config);

    const nlohmann::json j = json_of(preset.file);
    REQUIRE(j.contains("filament_extruder_variant"));
    CHECK(j["filament_extruder_variant"].size() == 2);
    REQUIRE(j.contains("filament_flow_ratio"));
    REQUIRE(j["filament_flow_ratio"].size() == 2);
    // Snapmaker Orca 2.4 declares filament_flow_ratio without nil and deletes a file it cannot parse.
    CHECK(j["filament_flow_ratio"][0].get<std::string>() != "nil");
    CHECK_THAT(std::stod(j["filament_flow_ratio"][0].get<std::string>()), Catch::Matchers::WithinAbs(0.95, 1e-9));
    CHECK_THAT(std::stod(j["filament_flow_ratio"][1].get<std::string>()), Catch::Matchers::WithinAbs(0.90, 1e-9));
    // An override switched off in both columns stays off.
    if (j.contains("filament_retraction_length"))
        for (const auto &entry : j["filament_retraction_length"])
            CHECK(entry.get<std::string>() == "nil");
}

TEST_CASE("A transferred filament column lands in the column of its variant name", "[FilamentFlow]")
{
    DynamicPrintConfig source = one_column_filament();
    REQUIRE(filament_add_variant_column(source, HIGH_FLOW));
    source.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {20., 40.};

    DynamicPrintConfig target = one_column_filament();
    target.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {12.};

    filament_transfer_columns(target, source, {"filament_max_volumetric_speed#1"});
    CHECK(texts_of(target, "filament_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW});
    CHECK_THAT(target.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values, Catch::Matchers::Approx(std::vector<double>{12., 40.}));
    CHECK(filament_columns_consistent(target));
    // The other keys of the new column start as a copy of the target's Standard column.
    const auto *temperature = target.option<ConfigOptionInts>("nozzle_temperature");
    REQUIRE(temperature->values.size() == 2);
    CHECK(temperature->values[1] == temperature->values[0]);

    SECTION("a column the target has already takes the value") {
        filament_transfer_columns(target, source, {"filament_max_volumetric_speed#0"});
        CHECK_THAT(target.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values, Catch::Matchers::Approx(std::vector<double>{20., 40.}));
    }
}

TEST_CASE("A High Flow column starts as a copy of the Standard column", "[FilamentFlow]")
{
    DynamicPrintConfig config = one_column_filament();
    config.option<ConfigOptionInts>("nozzle_temperature")->values = {255};
    // An override switched off in the Standard column.
    auto *retraction = config.option<ConfigOptionFloatsNullable>("filament_retraction_length");
    REQUIRE(retraction != nullptr);
    retraction->values = {ConfigOptionFloatsNullable::nil_value()};

    REQUIRE(filament_add_flow_column(config, nvtHighFlow));
    CHECK(texts_of(config, "filament_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW});
    CHECK(config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{255, 255});
    REQUIRE(retraction->values.size() == 2);
    CHECK(retraction->is_nil(0));
    CHECK(retraction->is_nil(1));
    for (const std::string &key : filament_options_with_variant) {
        if (key == "filament_extruder_variant" || config.option(key) == nullptr)
            continue;
        INFO(key);
        const std::vector<std::string> values = texts_of(config, key);
        REQUIRE(values.size() == 2);
        CHECK(values[1] == values[0]);
    }
    CHECK(filament_columns_consistent(config));
    // Once there, nothing more is added; Standard is no column to add.
    CHECK_FALSE(filament_add_flow_column(config, nvtHighFlow));
    CHECK_FALSE(filament_add_flow_column(config, nvtStandard));
    CHECK(texts_of(config, "filament_extruder_variant").size() == 2);
}

TEST_CASE("The High Flow column of a Bowden filament is a Bowden column", "[FilamentFlow]")
{
    DynamicPrintConfig config = one_column_filament();
    config.option<ConfigOptionStrings>("filament_extruder_variant")->values = {"Bowden Standard"};
    REQUIRE(filament_add_flow_column(config, nvtHighFlow));
    CHECK(texts_of(config, "filament_extruder_variant") == std::vector<std::string>{"Bowden Standard", "Bowden High Flow"});
    CHECK(filament_flow_column(config, nvtHighFlow) == 1);
    CHECK(filament_flow_column(config, nvtStandard) == 0);
}

TEST_CASE("A parent is compared in the column layout of its child", "[FilamentFlow]")
{
    DynamicPrintConfig parent = one_column_filament();
    parent.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {12.};
    DynamicPrintConfig storage;

    SECTION("the same columns: the parent itself") {
        const DynamicPrintConfig child = parent;
        CHECK(&filament_reference_in_layout_of(child, parent, storage) == &parent);
    }
    SECTION("a column the parent lacks: the parent widened by a copy of its Standard column") {
        DynamicPrintConfig child = parent;
        REQUIRE(filament_add_flow_column(child, nvtHighFlow));
        const DynamicPrintConfig &reference = filament_reference_in_layout_of(child, parent, storage);
        CHECK(&reference == &storage);
        CHECK(texts_of(reference, "filament_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW});
        CHECK_THAT(reference.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values, Catch::Matchers::Approx(std::vector<double>{12., 12.}));
        // The parent is left as it is.
        CHECK(texts_of(parent, "filament_extruder_variant") == std::vector<std::string>{STANDARD});
    }
    SECTION("a child that names its columns in another order") {
        DynamicPrintConfig child = parent;
        REQUIRE(filament_add_flow_column(child, nvtHighFlow));
        child.option<ConfigOptionStrings>("filament_extruder_variant")->values = {HIGH_FLOW, STANDARD};
        child.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {22., 12.};
        const DynamicPrintConfig &reference = filament_reference_in_layout_of(child, parent, storage);
        CHECK(texts_of(reference, "filament_extruder_variant") == std::vector<std::string>{HIGH_FLOW, STANDARD});
        CHECK_THAT(reference.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values, Catch::Matchers::Approx(std::vector<double>{12., 12.}));
    }
}

TEST_CASE("A Standard change is offered to the High Flow column only while that column was a copy", "[FilamentFlow]")
{
    DynamicPrintConfig saved = one_column_filament();
    saved.option<ConfigOptionInts>("nozzle_temperature")->values = {255};
    saved.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {12.};
    REQUIRE(filament_add_flow_column(saved, nvtHighFlow));
    saved.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values[1] = 22.;

    DynamicPrintConfig edited = saved;
    edited.option<ConfigOptionInts>("nozzle_temperature")->values[0] = 250;
    edited.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values[0] = 10.;
    // The temperature was a copy and is offered; the speed has its own High Flow value and is not.
    CHECK(filament_standard_edits_not_followed(edited, saved) == std::vector<std::string>{"nozzle_temperature"});

    SECTION("a High Flow value changed as well is not offered") {
        edited.option<ConfigOptionInts>("nozzle_temperature")->values[1] = 260;
        CHECK(filament_standard_edits_not_followed(edited, saved).empty());
    }
    SECTION("the copy carries the Standard value over") {
        CHECK(filament_copy_standard_to_high_flow(edited, {"nozzle_temperature"}));
        CHECK(edited.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{250, 250});
        CHECK(filament_standard_edits_not_followed(edited, saved).empty());
        CHECK_FALSE(filament_copy_standard_to_high_flow(edited, {"nozzle_temperature"}));
    }
    SECTION("a preset without a High Flow column offers nothing") {
        DynamicPrintConfig one = one_column_filament();
        DynamicPrintConfig changed = one;
        changed.option<ConfigOptionInts>("nozzle_temperature")->values[0] += 5;
        CHECK(filament_standard_edits_not_followed(changed, one).empty());
    }
}

// ---------------------------------------------------------------------------------------------
// A U1 0.4 plate with extruder 2 on High Flow, Generic PETG given High Flow values in the Filament
// tab and not saved: the edited copy of the system preset holds both columns.
// ---------------------------------------------------------------------------------------------

namespace {

const char *const U1_MACHINE = "Snapmaker U1 (0.4 nozzle)";
const char *const U1_PROCESS = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)";
const char *const PETG       = "Generic PETG";

void select_plate_with_high_flow_petg(PresetBundle &bundle, const std::vector<std::string> &slots)
{
    REQUIRE(bundle.printers.select_preset_by_name(U1_MACHINE, true));
    REQUIRE(bundle.prints.select_preset_by_name(U1_PROCESS, true));
    bundle.filament_presets = slots;
    // The project counts its filaments by filament_colour.
    bundle.project_config.option<ConfigOptionStrings>("filament_colour", true)->values.assign(slots.size(), "#FFFFFF");
    REQUIRE(bundle.filaments.select_preset_by_name(PETG, true));
    std::vector<int> map;
    for (size_t slot = 0; slot < slots.size(); ++slot)
        map.emplace_back(int(slot) + 1);
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = map;
    bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {int(nvtStandard), int(nvtHighFlow), int(nvtStandard), int(nvtStandard)};
    DynamicPrintConfig &edited = bundle.filaments.get_edited_preset().config;
    REQUIRE(filament_add_flow_column(edited, nvtHighFlow));
    edited.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values[1] = 22.;
    bundle.filaments.update_dirty();
    REQUIRE(bundle.filaments.get_edited_preset().is_dirty);
}

// Stores the project of `bundle` as a 3MF, loads it into a fresh bundle and returns that bundle.
std::unique_ptr<PresetBundle> project_round_trip(PresetBundle &bundle)
{
    Model model;
    REQUIRE(load_stl((std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl").c_str(), &model));
    model.add_default_instances();
    ScopedTemporaryDir backup_dir("orca_filament_flow_project");
    model.set_backup_path(backup_dir.string());

    DynamicPrintConfig project = bundle.full_config_secure();
    ScopedTemporaryFile temp(".3mf");
    StoreParams         store_params;
    const std::string   path = temp.string();
    store_params.path     = path.c_str();
    store_params.model    = &model;
    store_params.config   = &project;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    PlateData *plate   = new PlateData();
    plate->plate_index = 0;
    store_params.plate_data_list.push_back(plate);
    REQUIRE(store_bbs_3mf(store_params));

    Model              dst_model;
    ScopedTemporaryDir dst_backup_dir("orca_filament_flow_project_dst");
    dst_model.set_backup_path(dst_backup_dir.string());
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs             dst_plates;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false, is_orca_3mf = false;
    Semver                    file_version;
    REQUIRE(load_bbs_3mf(path.c_str(), &dst_config, &context, &dst_model, &dst_plates, &project_presets, &is_bbl_3mf, &is_orca_3mf, &file_version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    auto second = load_snapmaker_bundle();
    Preset::normalize(dst_config);
    second->load_config_model("filament_flow.3mf", std::move(dst_config), file_version);
    release_PlateData_list(dst_plates);
    delete plate;
    return second;
}

// The different settings of filament `slot` as the project records them.
std::string different_settings_of(const DynamicPrintConfig &project, size_t slot)
{
    const auto *different = project.option<ConfigOptionStrings>("different_settings_to_system");
    REQUIRE(different != nullptr);
    // Process first, then one entry per filament, then the printer.
    REQUIRE(different->values.size() > slot + 1);
    return different->values[slot + 1];
}

} // namespace

TEST_CASE("An unsaved High Flow column prints on the High Flow extruder and Standard elsewhere", "[FilamentFlow][HighFlow]")
{
    auto bundle = load_snapmaker_bundle();

    SECTION("four filaments, one per extruder") {
        select_plate_with_high_flow_petg(*bundle, {PETG, PETG, PETG, PETG});
        const DynamicPrintConfig composed = bundle->full_config(true);
        const auto *speed = composed.option<ConfigOptionFloats>("filament_max_volumetric_speed");
        REQUIRE(speed != nullptr);
        CHECK_THAT(speed->values, Catch::Matchers::Approx(std::vector<double>{12., 22., 12., 12.}));
        const auto *temperature = composed.option<ConfigOptionInts>("nozzle_temperature");
        REQUIRE(temperature != nullptr);
        CHECK(temperature->values == std::vector<int>{255, 255, 255, 255});
    }
    SECTION("one filament, printed by extruder 2") {
        select_plate_with_high_flow_petg(*bundle, {PETG});
        const DynamicPrintConfig composed = bundle->full_config(true, std::vector<int>{2});
        CHECK_THAT(composed.option<ConfigOptionFloats>("filament_max_volumetric_speed")->get_at(0), Catch::Matchers::WithinAbs(22., 1e-9));
        CHECK(composed.option<ConfigOptionInts>("nozzle_temperature")->get_at(0) == 255);
    }
}

TEST_CASE("A project keeps the High Flow column of an unsaved filament", "[FilamentFlow][HighFlow]")
{
    auto bundle = load_snapmaker_bundle();
    const std::vector<std::string> slots = GENERATE(std::vector<std::string>{PETG}, std::vector<std::string>{PETG, PETG, PETG, PETG});
    CAPTURE(slots.size());
    select_plate_with_high_flow_petg(*bundle, slots);
    const size_t slot = slots.size() > 1 ? 1 : 0;

    const DynamicPrintConfig project = bundle->full_config_secure();
    CHECK(project_schema_version_for(project) > 1);
    CHECK(different_settings_of(project, slot).find("filament_max_volumetric_speed") != std::string::npos);

    auto second = project_round_trip(*bundle);
    // The U1 gets at least one filament slot per extruder.
    REQUIRE(second->filament_presets.size() == std::max<size_t>(slots.size(), 4));
    const Preset *loaded = second->filaments.find_preset(second->filament_presets[slot], false);
    REQUIRE(loaded != nullptr);
    CHECK(texts_of(loaded->config, "filament_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW});
    CHECK_THAT(loaded->config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values, Catch::Matchers::Approx(std::vector<double>{12., 22.}));
    // A key left unchanged has both columns from the system preset.
    CHECK(loaded->config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{255, 255});
    CHECK(filament_columns_consistent(loaded->config));
}

// ---------------------------------------------------------------------------------------------
// Preset::reload: saving a detached user base in place reloads its children against it.
// ---------------------------------------------------------------------------------------------

TEST_CASE("A filament child reloaded after its user base gained a High Flow column keeps one layout and its own values", "[FilamentFlow][FilamentReload]")
{
    ScopedTemporaryDir dir("orca_filament_reload");
    Preset base(Preset::TYPE_FILAMENT, "My Base PETG", false);
    base.config = one_column_filament();
    base.config.option<ConfigOptionInts>("nozzle_temperature")->values             = {255};
    base.config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {12.};

    // The child, saved against the one-column base, writes only its Standard temperature.
    Preset child(Preset::TYPE_FILAMENT, "My Child PETG", false);
    child.config = base.config;
    child.config.option<ConfigOptionInts>("nozzle_temperature")->values = {250};
    child.inherits() = base.name;
    child.file       = (dir.path() / "My Child PETG.json").string();
    child.save(&base.config);
    REQUIRE(boost::filesystem::exists(child.file));

    // The base gains High Flow values and is saved in place.
    REQUIRE(filament_add_flow_column(base.config, nvtHighFlow));
    base.config.option<ConfigOptionInts>("nozzle_temperature")->values[1]             = 265;
    base.config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values[1] = 22.;
    child.reload(base);

    CHECK(texts_of(child.config, "filament_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW});
    CHECK(filament_columns_consistent(child.config));
    // The child's own Standard value stays; the High Flow column it never wrote follows the base.
    CHECK(child.config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{250, 265});
    CHECK_THAT(child.config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values, Catch::Matchers::Approx(std::vector<double>{12., 22.}));
    CHECK(child.inherits() == base.name);
}

TEST_CASE("A filament child with High Flow values reloaded under a one-column base keeps both columns", "[FilamentFlow][FilamentReload]")
{
    ScopedTemporaryDir dir("orca_filament_reload_wide");
    Preset base(Preset::TYPE_FILAMENT, "My Base PETG", false);
    base.config = one_column_filament();
    base.config.option<ConfigOptionInts>("nozzle_temperature")->values             = {255};
    base.config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {12.};

    Preset child(Preset::TYPE_FILAMENT, "My Child PETG", false);
    child.config = base.config;
    REQUIRE(filament_add_flow_column(child.config, nvtHighFlow));
    child.config.option<ConfigOptionInts>("nozzle_temperature")->values = {250, 265};
    child.inherits() = base.name;
    child.file       = (dir.path() / "My Child PETG.json").string();
    child.save(&base.config);
    REQUIRE(boost::filesystem::exists(child.file));

    // The base is saved in place with another speed and still one column.
    base.config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {14.};
    child.reload(base);

    CHECK(texts_of(child.config, "filament_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW});
    CHECK(filament_columns_consistent(child.config));
    CHECK(child.config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{250, 265});
    // A key the child does not write follows the base in both columns.
    CHECK_THAT(child.config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values, Catch::Matchers::Approx(std::vector<double>{14., 14.}));
    // The base itself is left as it is.
    CHECK(texts_of(base.config, "filament_extruder_variant") == std::vector<std::string>{STANDARD});
}

TEST_CASE("A project filament with a narrow per-column key is padded before its columns are matched", "[FilamentFlow]")
{
    auto bundle = load_snapmaker_bundle();
    const Preset *parent = bundle->filaments.find_preset(PETG, false, true);
    REQUIRE(parent != nullptr);
    REQUIRE(texts_of(parent->config, "filament_extruder_variant") == std::vector<std::string>{STANDARD});

    // The preset as a project stores it: the keys that differ from Generic PETG, one of them with a
    // single value for two columns.
    Preset embedded(Preset::TYPE_FILAMENT, "My PETG in the project", false);
    embedded.is_project_embedded = true;
    embedded.config.set_key_value("inherits", new ConfigOptionString(PETG));
    embedded.config.set_key_value("filament_extruder_variant", new ConfigOptionStrings({STANDARD, HIGH_FLOW}));
    embedded.config.set_key_value("nozzle_temperature", new ConfigOptionInts({250, 265}));
    embedded.config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats({22.}));
    std::vector<Preset *>      project_presets{&embedded};
    PresetsConfigSubstitutions substitutions;
    bundle->filaments.load_project_embedded_presets(project_presets, PRESET_FILAMENT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);

    const Preset *loaded = bundle->filaments.find_preset("My PETG in the project", false);
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->name == "My PETG in the project");
    CHECK(texts_of(loaded->config, "filament_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW});
    CHECK(filament_columns_consistent(loaded->config));
    CHECK(loaded->config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{250, 265});
    // The single value stands for both columns.
    const auto *speed = loaded->config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    REQUIRE(speed->values.size() == 2);
    CHECK_THAT(speed->values[0], Catch::Matchers::WithinAbs(22., 1e-9));
    CHECK_THAT(speed->values[1], Catch::Matchers::WithinAbs(22., 1e-9));
    // The system preset is left as it is.
    CHECK(texts_of(parent->config, "filament_extruder_variant") == std::vector<std::string>{STANDARD});
}

// ---------------------------------------------------------------------------------------------
// One writer: the user preset file (Preset::save) and the copy a project stores
// (PresetCollection::get_preset_differed_for_save) hold the same keys and values.
// ---------------------------------------------------------------------------------------------

namespace {

std::pair<nlohmann::json, nlohmann::json> saved_and_project_copy(PresetCollection &collection, const Preset &parent, const DynamicPrintConfig &child_config,
                                                                 const ScopedTemporaryDir &dir)
{
    Preset child(parent.type, "My copy", false);
    child.config     = child_config;
    child.version    = parent.version;
    child.inherits() = parent.name;
    child.file       = (dir.path() / "saved.json").string();
    DynamicPrintConfig parent_config = parent.config;
    child.save(&parent_config);
    const std::unique_ptr<Preset> project(collection.get_preset_differed_for_save(child));
    REQUIRE(project != nullptr);
    const std::string project_file = (dir.path() / "project.json").string();
    project->config.save_to_json(project_file, "My copy", "User", parent.version.to_string());
    return {json_of(child.file), json_of(project_file)};
}

bool has_nil(const nlohmann::json &value)
{
    if (value.is_array()) {
        for (const auto &entry : value)
            if (entry.is_string() && entry.get<std::string>() == "nil")
                return true;
        return false;
    }
    return value.is_string() && value.get<std::string>() == "nil";
}

} // namespace

TEST_CASE("A saved preset file and its project copy are written alike", "[FilamentFlow][PresetWriter]")
{
    auto bundle = load_snapmaker_bundle();
    ScopedTemporaryDir dir("orca_preset_writer");

    SECTION("a filament with High Flow values") {
        const Preset *parent = bundle->filaments.find_preset(PETG, false, true);
        REQUIRE(parent != nullptr);
        DynamicPrintConfig child = parent->config;
        REQUIRE(filament_add_flow_column(child, nvtHighFlow));
        child.option<ConfigOptionFloatsNullable>("filament_flow_ratio")->values[1] = 0.90;
        const auto copies = saved_and_project_copy(bundle->filaments, *parent, child, dir);
        CHECK(copies.first == copies.second);
        REQUIRE(copies.first.contains("filament_flow_ratio"));
        REQUIRE(copies.second.contains("filament_flow_ratio"));
        CHECK_FALSE(has_nil(copies.first["filament_flow_ratio"]));
        CHECK_FALSE(has_nil(copies.second["filament_flow_ratio"]));
    }
    SECTION("a process whose per-extruder line width changed in one column") {
        const Preset *parent = bundle->prints.find_preset(U1_PROCESS, false, true);
        REQUIRE(parent != nullptr);
        DynamicPrintConfig child = parent->config;
        auto *width = child.option<ConfigOptionFloatsOrPercentsNullable>("line_width");
        REQUIRE(width != nullptr);
        REQUIRE(width->values.size() > 1);
        width->values[0] = FloatOrPercent(110., true);
        const auto copies = saved_and_project_copy(bundle->prints, *parent, child, dir);
        CHECK(copies.first == copies.second);
        REQUIRE(copies.second.contains("line_width"));
        // A key read as one value by older readers is written without "nil" (scalar_when_uniform).
        CHECK_FALSE(has_nil(copies.second["line_width"]));
    }
}

TEST_CASE("Opening a project with High Flow values leaves the system filament unchanged", "[FilamentFlow]")
{
    auto bundle = load_snapmaker_bundle();
    const Preset *system = bundle->filaments.find_preset(PETG, false, true);
    REQUIRE(system != nullptr);
    const DynamicPrintConfig before   = system->config;
    const double             standard = before.option<ConfigOptionFloats>("filament_max_volumetric_speed")->get_at(0);

    // The filament as the project config holds it: Generic PETG with a High Flow column and one
    // High Flow value of its own.
    DynamicPrintConfig project = before;
    REQUIRE(filament_add_flow_column(project, nvtHighFlow));
    project.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values[1] = 22.;
    const std::set<std::string> different{"filament_max_volumetric_speed"};

    std::string original_name;
    SECTION("a user preset made from Generic PETG") {
        project.option<ConfigOptionString>("inherits", true)->value = PETG;
        original_name                                                = "My PETG from the project";
    }
    SECTION("Generic PETG itself with changes") {
        project.option<ConfigOptionString>("inherits", true)->value.clear();
        original_name = PETG;
    }
    // Called as PresetBundle::load_config_file_config does for a 3MF: the file name as the name, the
    // stored preset name as the original name. The preset is then embedded in the project, not saved.
    const auto loaded = bundle->filaments.load_external_preset("project.3mf", "project.3mf", original_name, project, different,
                                                               PresetCollection::LoadAndSelect::Never);
    REQUIRE(loaded.first != nullptr);
    CHECK(loaded.first->is_project_embedded);
    CHECK(texts_of(loaded.first->config, "filament_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW});
    const auto *speed = loaded.first->config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    REQUIRE(speed->values.size() == 2);
    CHECK_THAT(speed->values[0], Catch::Matchers::WithinAbs(standard, 1e-9));
    CHECK_THAT(speed->values[1], Catch::Matchers::WithinAbs(22., 1e-9));

    // Found again: loading may have moved the presets of the collection.
    system = bundle->filaments.find_preset(PETG, false, true);
    REQUIRE(system != nullptr);
    CHECK(texts_of(system->config, "filament_extruder_variant") == std::vector<std::string>{STANDARD});
    CHECK(system->config.diff(before).empty());
}

TEST_CASE("The filament index of a project is rebuilt only from column counts that fill the column list", "[FilamentFlow]")
{
    std::vector<int> index{7, 7};
    // Filament 1 with Standard and High Flow columns, filament 2 with one.
    CHECK(filament_self_index_from_counts({2, 1}, 3, index));
    CHECK(index == std::vector<int>{1, 1, 2});

    SECTION("counts that do not fill the list leave the index as it is") {
        std::vector<int> untouched{7, 7};
        CHECK_FALSE(filament_self_index_from_counts({2, 1}, 2, untouched));
        CHECK(untouched == std::vector<int>{7, 7});
        CHECK_FALSE(filament_self_index_from_counts({2, 1}, 4, untouched));
        CHECK(untouched == std::vector<int>{7, 7});
    }
    SECTION("a negative count is refused") {
        std::vector<int> untouched{7};
        CHECK_FALSE(filament_self_index_from_counts({-1, 2}, 1, untouched));
        CHECK(untouched == std::vector<int>{7});
    }
}

TEST_CASE("Filament fields outside the per-column keys are shared by Standard and High Flow", "[FilamentFlow]")
{
    // The time fields of the two fan speed threshold lines, the overhang fan.
    for (const char *key : {"fan_cooling_layer_time", "slow_down_layer_time", "overhang_fan_speed"}) {
        INFO(key);
        CHECK(filament_field_shared_under_high_flow(key));
    }
    // The fan speeds of those lines and the values tuned per nozzle flow type, the bed and chamber
    // temperatures and adaptive pressure advance included.
    for (const char *key : {"fan_min_speed", "fan_max_speed", "nozzle_temperature", "filament_max_volumetric_speed", "hot_plate_temp", "adaptive_pressure_advance",
                            "textured_plate_temp_initial_layer", "graphic_effect_plate_temp", "activate_chamber_temp_control",
                            "chamber_temperature", "chamber_minimal_temperature"}) {
        INFO(key);
        CHECK_FALSE(filament_field_shared_under_high_flow(key));
    }
}

TEST_CASE("Standard changes the High Flow column does not follow are found against the saved preset", "[FilamentFlow]")
{
    DynamicPrintConfig saved = one_column_filament();
    saved.option<ConfigOptionInts>("nozzle_temperature")->values             = {255};
    saved.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {12.};

    SECTION("a saved preset with High Flow values") {
        REQUIRE(filament_add_flow_column(saved, nvtHighFlow));
        DynamicPrintConfig edited = saved;
        // 255 -> 250 -> 245 in one session: compared with the saved 255 / 255, still listed.
        edited.option<ConfigOptionInts>("nozzle_temperature")->values = {250, 255};
        edited.option<ConfigOptionInts>("nozzle_temperature")->values = {245, 255};
        CHECK(filament_standard_edits_not_followed(edited, saved) == std::vector<std::string>{"nozzle_temperature"});
        edited.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {10., 12.};
        CHECK(filament_standard_edits_not_followed(edited, saved) == std::vector<std::string>{"filament_max_volumetric_speed", "nozzle_temperature"});
    }
    SECTION("a High Flow value that differed when saved is not listed") {
        REQUIRE(filament_add_flow_column(saved, nvtHighFlow));
        saved.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {12., 22.};
        DynamicPrintConfig edited = saved;
        edited.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {10., 22.};
        CHECK(filament_standard_edits_not_followed(edited, saved).empty());
    }
    SECTION("an unsaved High Flow column is compared with the saved preset widened by it") {
        DynamicPrintConfig edited = saved;
        REQUIRE(filament_add_flow_column(edited, nvtHighFlow));
        edited.option<ConfigOptionInts>("nozzle_temperature")->values = {245, 255};
        DynamicPrintConfig storage;
        CHECK(filament_standard_edits_not_followed(edited, filament_reference_in_layout_of(edited, saved, storage)) ==
              std::vector<std::string>{"nozzle_temperature"});
        // The saved preset as it is has no High Flow column to compare with.
        CHECK(filament_standard_edits_not_followed(edited, saved).empty());
    }
}

// ---------------------------------------------------------------------------------------------
// Limits of the High Flow column (doc/speeds-per-tool-head.md, "High Flow values for any filament").
// ---------------------------------------------------------------------------------------------

TEST_CASE("A retraction override switched off under High Flow only follows the parent again after loading", "[FilamentFlow]")
{
    ScopedTemporaryDir dir("orca_filament_override_off");
    Preset base(Preset::TYPE_FILAMENT, "My Base PETG", false);
    base.config = one_column_filament();
    base.config.option<ConfigOptionFloatsNullable>("filament_retraction_length")->values = {0.8};

    Preset child(Preset::TYPE_FILAMENT, "My Child PETG", false);
    child.config = base.config;
    REQUIRE(filament_add_flow_column(child.config, nvtHighFlow));
    // The override switched off in the High Flow column: nil, the only "off" the key has.
    child.config.option<ConfigOptionFloatsNullable>("filament_retraction_length")->values[1] = ConfigOptionFloatsNullable::nil_value();
    child.inherits() = base.name;
    child.file       = (dir.path() / "My Child PETG.json").string();
    child.save(&base.config);
    const nlohmann::json j = json_of(child.file);
    REQUIRE(j.contains("filament_retraction_length"));
    REQUIRE(j["filament_retraction_length"].size() == 2);
    CHECK(j["filament_retraction_length"][1].get<std::string>() == "nil");

    // Loaded again, nil takes the parent's value: the override is on again.
    child.reload(base);
    const auto *retraction = child.config.option<ConfigOptionFloatsNullable>("filament_retraction_length");
    REQUIRE(retraction->values.size() == 2);
    CHECK_FALSE(retraction->is_nil(1));
    CHECK_THAT(retraction->values[1], Catch::Matchers::WithinAbs(0.8, 1e-9));
}

TEST_CASE("A user filament preset re-saved without its column list loads with its Standard values only", "[FilamentFlow]")
{
    // The file Snapmaker Orca 2.4 writes when it saves a preset of this application again: the
    // per-column keys keep two values, the column list is gone.
    auto bundle = load_snapmaker_bundle();
    const Preset *parent = bundle->filaments.find_preset(PETG, false, true);
    REQUIRE(parent != nullptr);
    ScopedTemporaryDir dir("orca_filament_no_list");
    boost::filesystem::create_directories(dir.path() / PRESET_FILAMENT_NAME);
    {
        nlohmann::json j;
        j["type"]                 = "filament";
        j["name"]                 = "My PETG re-saved";
        j["from"]                 = "User";
        j["inherits"]             = PETG;
        j["version"]              = parent->version.to_string();
        j["filament_settings_id"] = {"My PETG re-saved"};
        j["nozzle_temperature"]   = {"250", "265"};
        boost::nowide::ofstream out((dir.path() / PRESET_FILAMENT_NAME / "My PETG re-saved.json").string());
        out << j.dump(4);
    }
    PresetsConfigSubstitutions substitutions;
    bundle->filaments.load_presets(dir.path().string(), PRESET_FILAMENT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);
    const Preset *loaded = bundle->filaments.find_preset("My PETG re-saved", false);
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->name == "My PETG re-saved");
    CHECK(texts_of(loaded->config, "filament_extruder_variant") == std::vector<std::string>{STANDARD});
    CHECK(loaded->config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{250});
    CHECK(filament_columns_consistent(loaded->config));
}

// ---------------------------------------------------------------------------------------------
// Bed and chamber temperatures per column: every plate's bed temperature (first and other layers)
// and the chamber keys hold one value per filament variant column, like the nozzle temperature.
// ---------------------------------------------------------------------------------------------

namespace {

// The bed temperature keys of every plate and the chamber keys.
const std::vector<std::string> &bed_and_chamber_keys()
{
    static const std::vector<std::string> keys = {
        "supertack_plate_temp", "supertack_plate_temp_initial_layer", "cool_plate_temp", "cool_plate_temp_initial_layer",
        "textured_cool_plate_temp", "textured_cool_plate_temp_initial_layer", "eng_plate_temp", "eng_plate_temp_initial_layer",
        "hot_plate_temp", "hot_plate_temp_initial_layer", "textured_plate_temp", "textured_plate_temp_initial_layer",
        "graphic_effect_plate_temp", "graphic_effect_plate_temp_initial_layer",
        "activate_chamber_temp_control", "chamber_temperature", "chamber_minimal_temperature"};
    return keys;
}

// Writes the user preset file `name` (child of Generic PETG unless `inherits` is empty) with the
// given keys into `dir`/filament and loads it through the filament loader of `bundle`.
const Preset &load_user_filament_file(PresetBundle &bundle, const ScopedTemporaryDir &dir, const std::string &name, const std::string &inherits,
                                      const std::vector<std::pair<std::string, std::vector<std::string>>> &values)
{
    const Preset *parent = bundle.filaments.find_preset(PETG, false, true);
    REQUIRE(parent != nullptr);
    boost::filesystem::create_directories(dir.path() / PRESET_FILAMENT_NAME);
    {
        nlohmann::json j;
        j["type"]                 = "filament";
        j["name"]                 = name;
        j["from"]                 = "User";
        j["version"]              = parent->version.to_string();
        j["filament_settings_id"] = {name};
        if (!inherits.empty())
            j["inherits"] = inherits;
        for (const auto &[key, texts] : values)
            j[key] = texts;
        boost::nowide::ofstream out((dir.path() / PRESET_FILAMENT_NAME / (name + ".json")).string());
        out << j.dump(4);
    }
    PresetsConfigSubstitutions substitutions;
    bundle.filaments.load_presets(dir.path().string(), PRESET_FILAMENT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);
    const Preset *loaded = bundle.filaments.find_preset(name, false);
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->name == name);
    return *loaded;
}

} // namespace

TEST_CASE("Bed and chamber temperatures are per-column filament keys", "[FilamentFlow][BedChamber]")
{
    for (const std::string &key : bed_and_chamber_keys()) {
        INFO(key);
        CHECK(filament_options_with_variant.count(key) == 1);
        // Stored once per filament in mainline projects and in projects of earlier versions.
        CHECK(std::count(promoted_filament_variant_keys().begin(), promoted_filament_variant_keys().end(), key) == 1);
    }
}

TEST_CASE("A new High Flow column holds its own bed and chamber temperatures, starting as a copy of Standard", "[FilamentFlow][BedChamber]")
{
    DynamicPrintConfig saved = one_column_filament();
    saved.option<ConfigOptionInts>("textured_plate_temp")->values = {70};
    saved.option<ConfigOptionInts>("chamber_temperature")->values = {40};
    saved.option<ConfigOptionBools>("activate_chamber_temp_control")->values = {1};
    REQUIRE(filament_add_flow_column(saved, nvtHighFlow));
    CHECK(filament_columns_consistent(saved));
    CHECK(saved.option<ConfigOptionInts>("textured_plate_temp")->values == std::vector<int>{70, 70});
    CHECK(saved.option<ConfigOptionInts>("chamber_temperature")->values == std::vector<int>{40, 40});
    CHECK(saved.option<ConfigOptionBools>("activate_chamber_temp_control")->values == std::vector<unsigned char>{1, 1});
    for (const std::string &key : bed_and_chamber_keys()) {
        INFO(key);
        const std::vector<std::string> texts = texts_of(saved, key);
        REQUIRE(texts.size() == 2);
        CHECK(texts[0] == texts[1]);
    }

    // A Standard change is offered to the High Flow column like any other per-column value.
    DynamicPrintConfig edited = saved;
    edited.option<ConfigOptionInts>("textured_plate_temp")->values = {65, 70};
    CHECK(filament_standard_edits_not_followed(edited, saved) == std::vector<std::string>{"textured_plate_temp"});
    CHECK(filament_copy_standard_to_high_flow(edited, {"textured_plate_temp"}));
    CHECK(edited.option<ConfigOptionInts>("textured_plate_temp")->values == std::vector<int>{65, 65});
}

TEST_CASE("A High Flow column saved before the bed and chamber temperatures had columns loads them as a copy of Standard", "[FilamentFlow][BedChamber]")
{
    // The file an earlier version wrote: the per-column keys of that time with two values, a changed
    // bed and chamber temperature with one value.
    auto bundle = load_snapmaker_bundle();
    ScopedTemporaryDir dir("orca_filament_bed_chamber_old");
    const std::vector<std::pair<std::string, std::vector<std::string>>> old_file = {
        {"filament_extruder_variant", {STANDARD, HIGH_FLOW}},
        {"filament_max_volumetric_speed", {"12", "22"}},
        {"nozzle_temperature", {"255", "265"}},
        {"textured_plate_temp", {"70"}},
        {"activate_chamber_temp_control", {"1"}},
        {"chamber_temperature", {"40"}}};

    SECTION("a user preset of a one-column system preset") {
        const Preset &loaded = load_user_filament_file(*bundle, dir, "My old PETG", PETG, old_file);
        const DynamicPrintConfig &config = loaded.config;
        CHECK(texts_of(config, "filament_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW});
        CHECK(filament_columns_consistent(config));
        CHECK(config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{255, 265});
        // The values the file names, in both columns.
        CHECK(config.option<ConfigOptionInts>("textured_plate_temp")->values == std::vector<int>{70, 70});
        CHECK(config.option<ConfigOptionInts>("chamber_temperature")->values == std::vector<int>{40, 40});
        CHECK(config.option<ConfigOptionBools>("activate_chamber_temp_control")->values == std::vector<unsigned char>{1, 1});
        // A key the file does not name follows the parent in both columns.
        CHECK(config.option<ConfigOptionInts>("textured_plate_temp_initial_layer")->values == std::vector<int>{80, 80});
        for (const std::string &key : bed_and_chamber_keys()) {
            INFO(key);
            const std::vector<std::string> texts = texts_of(config, key);
            REQUIRE(texts.size() == 2);
            CHECK(texts[0] == texts[1]);
        }
    }
    SECTION("a user preset without a parent") {
        const Preset &loaded = load_user_filament_file(*bundle, dir, "My old root PETG", "", old_file);
        const DynamicPrintConfig &config = loaded.config;
        CHECK(texts_of(config, "filament_extruder_variant") == std::vector<std::string>{STANDARD, HIGH_FLOW});
        CHECK(filament_columns_consistent(config));
        CHECK(config.option<ConfigOptionInts>("textured_plate_temp")->values == std::vector<int>{70, 70});
        CHECK(config.option<ConfigOptionInts>("chamber_temperature")->values == std::vector<int>{40, 40});
        for (const std::string &key : bed_and_chamber_keys()) {
            INFO(key);
            const std::vector<std::string> texts = texts_of(config, key);
            REQUIRE(texts.size() == 2);
            CHECK(texts[0] == texts[1]);
        }
    }
}

TEST_CASE("A filament preset without a High Flow column writes its bed and chamber temperatures with one value", "[FilamentFlow][BedChamber]")
{
    auto bundle = load_snapmaker_bundle();
    const Preset *parent = bundle->filaments.find_preset(PETG, false, true);
    REQUIRE(parent != nullptr);
    REQUIRE(texts_of(parent->config, "filament_extruder_variant") == std::vector<std::string>{STANDARD});

    DynamicPrintConfig child = parent->config;
    child.option<ConfigOptionInts>("textured_plate_temp")->values           = {70};
    child.option<ConfigOptionInts>("chamber_temperature")->values           = {40};
    child.option<ConfigOptionBools>("activate_chamber_temp_control")->values = {1};

    ScopedTemporaryDir dir("orca_filament_bed_chamber_json");
    Preset preset(Preset::TYPE_FILAMENT, "My PETG bed", false);
    preset.config     = child;
    preset.version    = parent->version;
    preset.inherits() = parent->name;
    preset.file       = (dir.path() / "My PETG bed.json").string();
    DynamicPrintConfig parent_config = parent->config;
    preset.save(&parent_config);

    // The file of the one-column layout: the changed keys with their one value, nothing else of them.
    const nlohmann::json j = json_of(preset.file);
    for (const char *key : {"filament_extruder_variant", "textured_plate_temp", "chamber_temperature", "activate_chamber_temp_control"})
        REQUIRE(j.contains(key));
    CHECK(j.at("filament_extruder_variant") == nlohmann::json::array({STANDARD}));
    CHECK(j.at("textured_plate_temp") == nlohmann::json::array({"70"}));
    CHECK(j.at("chamber_temperature") == nlohmann::json::array({"40"}));
    CHECK(j.at("activate_chamber_temp_control") == nlohmann::json::array({"1"}));
    for (const std::string &key : bed_and_chamber_keys())
        if (key != "textured_plate_temp" && key != "chamber_temperature" && key != "activate_chamber_temp_control") {
            INFO(key);
            CHECK_FALSE(j.contains(key));
        }
    for (const std::string &key : filament_options_with_variant)
        if (j.contains(key)) {
            INFO(key);
            CHECK(j.at(key).size() == 1);
        }
}
