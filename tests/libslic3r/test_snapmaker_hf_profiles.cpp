#include <catch2/catch_all.hpp>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

// The shipped Snapmaker vendor, loaded as the application does: pins the Standard / High Flow
// columns of the U1 0.4 mm presets, invariants M1 / M2 (resources/profiles/Snapmaker/README.md)
// and that all-Standard tool heads compose the same values as without High Flow columns.

namespace {

namespace fs = boost::filesystem;

const char *const U1_MACHINE      = "Snapmaker U1 (0.4 nozzle)";
const char *const U1_PROCESS      = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)";
const char *const U1_PROCESS_0_16 = "0.16mm Standard @Snapmaker U1 (0.4 nozzle)";
const char *const STANDARD        = "Direct Drive Standard";
const char *const HIGH_FLOW       = "Direct Drive High Flow";
constexpr size_t  HEADS           = 4;

// Every case loads its own bundle, so no case depends on what another one selected.
std::unique_ptr<PresetBundle> load_snapmaker_bundle()
{
    auto bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(PROFILES_DIR, "Snapmaker", PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr,
                                          /*allow_cache=*/false);
    return bundle;
}

const DynamicPrintConfig &system_config(PresetCollection &presets, const std::string &name)
{
    const Preset *preset = presets.find_preset(name, false, true);
    REQUIRE(preset != nullptr);
    return preset->config;
}

size_t vector_size(const DynamicPrintConfig &config, const std::string &key)
{
    const ConfigOption *opt = config.option(key);
    REQUIRE(opt != nullptr);
    REQUIRE(!opt->is_scalar());
    return static_cast<const ConfigOptionVectorBase *>(opt)->size();
}

// One serialized value per element, so columns of any option type can be compared.
std::vector<std::string> column_values(const DynamicPrintConfig &config, const std::string &key)
{
    const ConfigOption *opt = config.option(key);
    REQUIRE(opt != nullptr);
    REQUIRE(!opt->is_scalar());
    return static_cast<const ConfigOptionVectorBase *>(opt)->vserialize();
}

// The value a key has in the machine preset's parents, read from the shipped JSON files.
// Returns an empty vector when no parent sets the key.
std::vector<std::string> parent_json_values(const std::string &first_parent, const std::string &key)
{
    const fs::path machine_dir = fs::path(PROFILES_DIR) / "Snapmaker" / "machine";
    std::string    name        = first_parent;
    while (!name.empty()) {
        std::ifstream in((machine_dir / (name + ".json")).string());
        REQUIRE(in.good());
        nlohmann::json j = nlohmann::json::parse(in);
        if (j.contains(key)) {
            std::vector<std::string> out;
            if (j[key].is_array())
                for (const auto &v : j[key])
                    out.emplace_back(v.get<std::string>());
            else
                out.emplace_back(j[key].get<std::string>());
            return out;
        }
        name = j.value("inherits", std::string());
    }
    return {};
}

// Selects the all-Standard reference setup: the converted machine and process, four converted
// filaments, filament i on tool head i.
void select_reference_setup(PresetBundle &bundle)
{
    REQUIRE(bundle.printers.select_preset_by_name(U1_MACHINE, true));
    REQUIRE(bundle.prints.select_preset_by_name(U1_PROCESS, true));
    bundle.filament_presets = {"Snapmaker PLA SnapSpeed @U1", "Snapmaker PETG HF", "Snapmaker PLA Matte @U1",
                               "Snapmaker ABS @U1 0.4 nozzle"};
    REQUIRE(bundle.filaments.select_preset_by_name(bundle.filament_presets.front(), true));
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2, 3, 4};
    bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values =
        std::vector<int>(HEADS, int(nvtStandard));
}

} // namespace

TEST_CASE("The U1 0.4 machine preset carries a Standard and a High Flow column per tool head", "[Profiles][HighFlow][hf_profiles_machine]")
{
    const auto                loaded  = load_snapmaker_bundle();
    PresetBundle             &bundle  = *loaded;
    const DynamicPrintConfig &machine = system_config(bundle.printers, U1_MACHINE);

    const std::vector<std::string> list = machine.option<ConfigOptionStrings>("extruder_variant_list")->values;
    REQUIRE(list == std::vector<std::string>(HEADS, std::string(STANDARD) + "," + HIGH_FLOW));
    CHECK(machine.option<ConfigOptionInts>("printer_extruder_id")->values == std::vector<int>{1, 1, 2, 2, 3, 3, 4, 4});
    CHECK(machine.option<ConfigOptionStrings>("printer_extruder_variant")->values ==
          std::vector<std::string>{STANDARD, HIGH_FLOW, STANDARD, HIGH_FLOW, STANDARD, HIGH_FLOW, STANDARD, HIGH_FLOW});

    SECTION("the feature is off until a tool head is switched: every head defaults to Standard") {
        CHECK(machine.option<ConfigOptionEnumsGeneric>("default_nozzle_volume_type")->values ==
              std::vector<int>(HEADS, int(nvtStandard)));
    }

    SECTION("keys that are not variant keys stay one value per tool head") {
        CHECK(vector_size(machine, "nozzle_diameter") == HEADS);
        CHECK(vector_size(machine, "extruder_layer_height") == HEADS);
        CHECK(vector_size(machine, "min_layer_height") == HEADS);
        CHECK(vector_size(machine, "max_layer_height") == HEADS);
    }

    SECTION("stride-1 keys are 8 wide and do not differ by flow (M1)") {
        for (const std::string &key : printer_options_with_variant_1) {
            // The key set also names the two column keys, which differ by flow by definition.
            if (key == "printer_extruder_id" || key == "printer_extruder_variant")
                continue;
            if (machine.option(key) == nullptr || machine.option(key)->is_scalar())
                continue;
            INFO(key);
            const std::vector<std::string> values = column_values(machine, key);
            REQUIRE(values.size() == 2 * HEADS);
            for (size_t head = 0; head < HEADS; ++head)
                CHECK(values[2 * head] == values[2 * head + 1]);
        }
    }

    SECTION("stride-2 keys are 16 wide, do not differ by flow (M1) and are the same on every head (M2)") {
        for (const std::string &key : printer_options_with_variant_2) {
            if (machine.option(key) == nullptr)
                continue;
            INFO(key);
            const std::vector<std::string> values = column_values(machine, key);
            REQUIRE(values.size() == 4 * HEADS);
            for (size_t column = 1; column < 2 * HEADS; ++column) {
                CHECK(values[2 * column] == values[0]);
                CHECK(values[2 * column + 1] == values[1]);
            }
        }
    }

    SECTION("the (normal, silent) pair the file states reaches every column") {
        CHECK(column_values(machine, "machine_max_speed_z") ==
              std::vector<std::string>{"20", "12", "20", "12", "20", "12", "20", "12", "20", "12", "20", "12", "20", "12", "20", "12"});
    }

    SECTION("values written out of the shared parents still equal the parents' values") {
        // The 0.2 / 0.6 / 0.8 presets inherit the same parents without declaring the variant list,
        // so the wide vectors cannot live there. A later edit of a parent has to be repeated in
        // the 0.4 preset; this flags the one that was forgotten.
        const std::set<std::string> own_values = {"machine_max_speed_z", "machine_max_jerk_z", "machine_max_speed_e"};
        size_t                      compared   = 0;
        for (const std::string &key : printer_options_with_variant_2) {
            if (own_values.count(key) != 0)
                continue;
            const std::vector<std::string> parent = parent_json_values("fdm_U1", key);
            if (parent.size() < 2)
                continue;
            INFO(key);
            const ConfigOption *parsed = machine.option(key);
            REQUIRE(parsed != nullptr);
            // Compare parsed values, not spellings ("9000" and "9000.0" are the same limit).
            std::unique_ptr<ConfigOption> expected(parsed->clone());
            std::string                   joined;
            for (size_t column = 0; column < 2 * HEADS; ++column)
                joined += (joined.empty() ? "" : ",") + parent[0] + "," + parent[1];
            REQUIRE(expected->deserialize(joined));
            CHECK(*expected == *parsed);
            ++compared;
        }
        CHECK(compared >= 10);
    }
}

TEST_CASE("The other U1 machine presets declare no flow variants", "[Profiles][HighFlow][hf_profiles_machine]")
{
    const auto    loaded = load_snapmaker_bundle();
    PresetBundle &bundle = *loaded;
    for (const char *name : {"Snapmaker U1 (0.2 nozzle)", "Snapmaker U1 (0.6 nozzle)", "Snapmaker U1 (0.8 nozzle)"}) {
        INFO(name);
        const DynamicPrintConfig &machine = system_config(bundle.printers, name);
        for (const std::string &entry : machine.option<ConfigOptionStrings>("extruder_variant_list")->values)
            CHECK(entry.find("High Flow") == std::string::npos);
        CHECK(vector_size(machine, "retraction_length") == HEADS);
    }
}

TEST_CASE("Converted Snapmaker filaments carry a Standard and a High Flow column", "[Profiles][HighFlow][hf_profiles_filament]")
{
    const auto    loaded = load_snapmaker_bundle();
    PresetBundle &bundle = *loaded;

    SECTION("Snapmaker PETG HF") {
        const DynamicPrintConfig &petg = system_config(bundle.filaments, "Snapmaker PETG HF");
        CHECK(petg.option<ConfigOptionStrings>("filament_extruder_variant")->values == std::vector<std::string>{STANDARD, HIGH_FLOW});
        CHECK(vector_size(petg, "filament_max_volumetric_speed") == 2);
        CHECK(vector_size(petg, "nozzle_temperature") == 2);

        // A nullable pair: Standard inherits the printer's retraction, High Flow overrides it.
        const auto *retraction = petg.option<ConfigOptionFloatsNullable>("filament_retraction_length");
        REQUIRE(retraction != nullptr);
        REQUIRE(retraction->size() == 2);
        CHECK(retraction->is_nil(0));
        CHECK_FALSE(retraction->is_nil(1));
        CHECK_THAT(retraction->get_at(1), Catch::Matchers::WithinAbs(0.8, 1e-9));
    }

    SECTION("every variant key of a converted filament is as wide as its column list") {
        for (const char *name : {"Snapmaker PETG HF", "Snapmaker PLA SnapSpeed @U1", "Snapmaker PLA Matte @U1", "Snapmaker ABS @U1 0.4 nozzle"}) {
            const DynamicPrintConfig &filament = system_config(bundle.filaments, name);
            for (const std::string &key : filament_options_with_variant) {
                if (filament.option(key) == nullptr || filament.option(key)->is_scalar())
                    continue;
                INFO(name << ": " << key);
                CHECK(vector_size(filament, key) == 2);
            }
        }
    }

    SECTION("a filament that was not converted keeps its single column") {
        const DynamicPrintConfig &generic = system_config(bundle.filaments, "Snapmaker PLA SnapSpeed @U1 0.2 nozzle");
        CHECK(vector_size(generic, "filament_extruder_variant") == 1);
        CHECK(vector_size(generic, "filament_max_volumetric_speed") == 1);
        for (const std::string &key : promoted_filament_variant_keys()) {
            INFO(key);
            CHECK(vector_size(generic, key) == 1);
        }
    }

    SECTION("pressure advance, fan, ramming and purge values are tuned per flow type") {
        const DynamicPrintConfig &matte = system_config(bundle.filaments, "Snapmaker PLA Matte @U1");
        CHECK_THAT(matte.option<ConfigOptionFloats>("pressure_advance")->values, Catch::Matchers::Approx(std::vector<double>{0.02, 0.024}));
        CHECK(matte.option<ConfigOptionBools>("enable_pressure_advance")->values == std::vector<unsigned char>{0, 1});
        CHECK(matte.option<ConfigOptionInts>("additional_cooling_fan_speed")->values == std::vector<int>{80, 100});

        const DynamicPrintConfig &abs = system_config(bundle.filaments, "Snapmaker ABS @U1 0.4 nozzle");
        CHECK_THAT(abs.option<ConfigOptionFloats>("fan_max_speed")->values, Catch::Matchers::Approx(std::vector<double>{50., 60.}));
        CHECK_THAT(abs.option<ConfigOptionFloats>("filament_multitool_ramming_flow")->values, Catch::Matchers::Approx(std::vector<double>{20., 30.}));
        CHECK_THAT(abs.option<ConfigOptionFloats>("filament_multitool_ramming_volume")->values, Catch::Matchers::Approx(std::vector<double>{10., 5.}));
        CHECK_THAT(abs.option<ConfigOptionFloats>("filament_minimal_purge_on_wipe_tower")->values, Catch::Matchers::Approx(std::vector<double>{30., 15.}));

        for (const char *name : {"Snapmaker PLA Matte @U1", "Snapmaker ABS @U1 0.4 nozzle", "Snapmaker PVA @U1"}) {
            const DynamicPrintConfig &filament = system_config(bundle.filaments, name);
            for (const std::string &key : promoted_filament_variant_keys()) {
                INFO(name << ": " << key);
                CHECK(vector_size(filament, key) == 2);
            }
        }
    }
}

TEST_CASE("A multi column filament of another vendor gets its per filament values in every column", "[Profiles][HighFlow][hf_profiles_filament]")
{
    // Mainline presets state pressure advance, fan, ramming and purge once per filament; loading pads
    // them to every column. The filament library is loaded first as the vendor's base bundle.
    PresetBundle library;
    library.load_vendor_configs_from_json(PROFILES_DIR, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr,
                                          /*allow_cache=*/false);
    PresetBundle bundle;
    bundle.load_vendor_configs_from_json(PROFILES_DIR, "BBL", PresetBundle::LoadSystem,
                                         ForwardCompatibilitySubstitutionRule::EnableSilent, &library,
                                         /*allow_cache=*/false);
    const DynamicPrintConfig &filament = system_config(bundle.filaments, "Bambu ABS @BBL H2D");
    const size_t              columns  = vector_size(filament, "filament_extruder_variant");
    REQUIRE(columns > 1);
    for (const std::string &key : promoted_filament_variant_keys()) {
        INFO(key);
        const std::vector<std::string> values = column_values(filament, key);
        REQUIRE(values.size() == columns);
        for (const std::string &value : values)
            CHECK(value == values.front());
    }
}

TEST_CASE("Only the 0.20mm Standard process of the U1 carries High Flow speeds", "[Profiles][HighFlow][hf_profiles_process]")
{
    const auto    loaded = load_snapmaker_bundle();
    PresetBundle &bundle = *loaded;

    const DynamicPrintConfig &process = system_config(bundle.prints, U1_PROCESS);
    // The flow-only layout: one column per flow type, shared by all tool heads.
    CHECK(process.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{1, 1});
    CHECK(process.option<ConfigOptionStrings>("print_extruder_variant")->values == std::vector<std::string>{STANDARD, HIGH_FLOW});
    CHECK_THAT(process.option<ConfigOptionFloats>("outer_wall_speed")->values, Catch::Matchers::Approx(std::vector<double>{200., 500.}));
    for (const std::string &key : print_options_with_variant) {
        if (process.option(key) == nullptr || process.option(key)->is_scalar())
            continue;
        INFO(key);
        CHECK(vector_size(process, key) == 2);
    }

    const DynamicPrintConfig &slower = system_config(bundle.prints, U1_PROCESS_0_16);
    CHECK(vector_size(slower, "print_extruder_variant") == 1);
    CHECK(vector_size(slower, "outer_wall_speed") == 1);
}

TEST_CASE("With every tool head on Standard the composed config holds the Standard columns", "[Profiles][HighFlow][hf_profiles_all_standard]")
{
    const auto    loaded = load_snapmaker_bundle();
    PresetBundle &bundle = *loaded;
    select_reference_setup(bundle);

    const DynamicPrintConfig full = bundle.full_config(true);

    // Process: one value per tool head, all from the Standard column.
    CHECK_THAT(full.option<ConfigOptionFloats>("outer_wall_speed")->values, Catch::Matchers::Approx(std::vector<double>(HEADS, 200.)));
    CHECK_THAT(full.option<ConfigOptionFloats>("sparse_infill_speed")->values,
               Catch::Matchers::Approx(std::vector<double>(HEADS, system_config(bundle.prints, U1_PROCESS).option<ConfigOptionFloats>("sparse_infill_speed")->get_at(0))));

    // Filaments: one value per filament, the Standard column of each preset.
    const auto *speed = full.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    const auto *temp  = full.option<ConfigOptionInts>("nozzle_temperature");
    REQUIRE(speed->size() == bundle.filament_presets.size());
    REQUIRE(temp->size() == bundle.filament_presets.size());
    for (size_t i = 0; i < bundle.filament_presets.size(); ++i) {
        INFO(bundle.filament_presets[i]);
        const DynamicPrintConfig &preset = system_config(bundle.filaments, bundle.filament_presets[i]);
        CHECK_THAT(speed->get_at(i), Catch::Matchers::WithinAbs(preset.option<ConfigOptionFloats>("filament_max_volumetric_speed")->get_at(0), 1e-9));
        CHECK(temp->get_at(i) == preset.option<ConfigOptionInts>("nozzle_temperature")->get_at(0));
    }
    // The per flow type pressure advance, fan, ramming and purge values: one per filament, Standard column.
    for (const std::string &key : promoted_filament_variant_keys()) {
        const ConfigOption *composed = full.option(key);
        REQUIRE(composed != nullptr);
        const std::vector<std::string> values = static_cast<const ConfigOptionVectorBase *>(composed)->vserialize();
        REQUIRE(values.size() == bundle.filament_presets.size());
        for (size_t i = 0; i < bundle.filament_presets.size(); ++i) {
            INFO(bundle.filament_presets[i] << ": " << key);
            CHECK(values[i] == column_values(system_config(bundle.filaments, bundle.filament_presets[i]), key).front());
        }
    }
    // The nullable Standard column stays nil, so the printer's retraction applies.
    CHECK(full.option<ConfigOptionFloatsNullable>("filament_retraction_length")->is_nil(1));

    // Machine: one column per tool head again.
    CHECK_THAT(full.option<ConfigOptionFloats>("retraction_length")->values, Catch::Matchers::Approx(std::vector<double>(HEADS, 1.5)));
    CHECK_THAT(full.option<ConfigOptionFloats>("nozzle_diameter")->values, Catch::Matchers::Approx(std::vector<double>(HEADS, 0.4)));

    // Config gate of the profile conversion: SNAPMAKER_HF_CONFIG_DUMP=<file> writes the composed
    // config, one "key = value" line per option, to be diffed against a dump of another revision.
    if (const char *dump = std::getenv("SNAPMAKER_HF_CONFIG_DUMP"); dump != nullptr && *dump != 0) {
        std::ofstream out(dump);
        for (const std::string &key : full.keys())
            out << key << " = " << full.opt_serialize(key) << "\n";
    }
}
