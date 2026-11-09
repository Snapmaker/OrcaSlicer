#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PresetFlowVariant.hpp"

#include <boost/filesystem.hpp>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace Slic3r;
using json = nlohmann::json;

namespace {
const std::string standard  = "Direct Drive Standard";
const std::string high_flow = "Direct Drive High Flow";

struct ProfileFile
{
    boost::filesystem::path path = boost::filesystem::temp_directory_path() /
                                   boost::filesystem::unique_path("orca-variants-%%%%-%%%%.json");
    ~ProfileFile()
    {
        boost::system::error_code ec;
        boost::filesystem::remove(path, ec);
        auto info = path;
        boost::filesystem::remove(info.replace_extension(".info"), ec);
    }
    void write(const json& value) const { std::ofstream(path.string()) << value.dump(2); }
    json read() const
    {
        std::ifstream stream(path.string());
        return json::parse(stream);
    }
};

DynamicPrintConfig load(const ProfileFile& file)
{
    DynamicPrintConfig                 config;
    std::map<std::string, std::string> metadata;
    std::string                        reason;
    config.load_from_json(file.path.string(), ForwardCompatibilitySubstitutionRule::Enable, metadata, reason);
    REQUIRE(reason.empty());
    return config;
}

json save(const DynamicPrintConfig& config, const ProfileFile& file)
{
    config.save_to_json(file.path.string(), "Variant test", "User", "2.4.0.0");
    const auto result = file.read();
    REQUIRE_FALSE(result.contains("filament_flow_support"));
    REQUIRE_FALSE(result.contains("filament_flow_step_size"));
    return result;
}

DynamicPrintConfig make_variant_config(const std::vector<std::string>& variants, const std::vector<double>& speeds)
{
    DynamicPrintConfig config;
    config.set_key_value("filament_extruder_variant", new ConfigOptionStrings(variants));
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats(speeds));
    return config;
}

void routing(DynamicPrintConfig& config, std::initializer_list<FilamentVolumeType> types)
{
    config.set_deserialize_strict("filament_volume_type", "standard");
    auto& values = config.option<ConfigOptionEnumsGeneric>("filament_volume_type")->values;
    values.clear();
    for (auto type : types)
        values.push_back(int(type));
}

double speed(const DynamicPrintConfig& config, unsigned material)
{
    return config.option<ConfigOptionFloats>("filament_max_volumetric_speed")
        ->get_at(get_config_idx(config, ConfigFlowDomain::Filament, material));
}
} // namespace

TEST_CASE("Legacy material saves the common variant structure and reloads", "[FlowVariant]")
{
    ProfileFile source, saved;
    source.write({{"name", "Old material"},
                  {"from", "User"},
                  {"version", "2.4.0.0"},
                  {"filament_flow_support", {"standard", "high_flow"}},
                  {"filament_flow_step_size", {"2"}},
                  {"filament_max_volumetric_speed", {"12", "30"}},
                  {"filament_flow_ratio", {"0.98", "0.97"}},
                  {"filament_vendor", {"Generic"}},
                  {"compatible_printers", {"Snapmaker U1"}}});
    auto       config = load(source);
    const auto result = save(config, saved);
    REQUIRE(result["filament_extruder_variant"] == json({standard, high_flow}));
    REQUIRE_FALSE(result.contains("filament_self_index"));
    auto restored = load(saved);
    REQUIRE(restored.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{12, 30});
    REQUIRE(restored.option<ConfigOptionFloats>("filament_flow_ratio")->values == std::vector<double>{0.98, 0.97});
    REQUIRE(restored.option<ConfigOptionStrings>("filament_vendor")->values == std::vector<std::string>{"Generic"});
    REQUIRE(result["compatible_printers"] == json({"Snapmaker U1"}));
    REQUIRE(source.read().contains("filament_flow_support"));
}

TEST_CASE("Variant names determine routing even when High Flow is first", "[FlowVariant]")
{
    ProfileFile file;
    auto        config = make_variant_config({high_flow, standard}, {40, 12});
    REQUIRE(get_preset_flow_variant_idx(config, ConfigFlowDomain::Filament, fvtStandard) == 1);
    REQUIRE(get_preset_flow_variant_idx(config, ConfigFlowDomain::Filament, fvtHighFlow) == 0);
    normalize_filament_variant_metadata(config);
    routing(config, {fvtStandard});
    config.set_key_value("filament_flow_step_size", new ConfigOptionInts{2});
    normalize_filament_variant_metadata(config);
    REQUIRE(speed(config, 0) == 12);
    save(config, file);
    auto restored = load(file);
    REQUIRE(filament_extruder_variants(restored) == std::vector<std::string>{high_flow, standard});
    REQUIRE(restored.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{40, 12});
}

TEST_CASE("Upstream drive variants and future names survive a round trip", "[FlowVariant]")
{
    const std::vector<std::string> names{standard,
                                         high_flow,
                                         "Direct Drive Extra High Flow",
                                         "Bowden Standard",
                                         "Bowden High Flow",
                                         "Bowden Extra High Flow",
                                         "Future Drive Variant"};
    const std::vector<double>      speeds{12, 21, 40, 12, 21, 30, 55};
    auto                           config = make_variant_config(names, speeds);
    normalize_filament_variant_metadata(config);
    Preset::normalize(config);
    ProfileFile file;
    save(config, file);
    auto restored = load(file);
    REQUIRE(filament_extruder_variants(restored) == names);
    REQUIRE(restored.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == speeds);
    REQUIRE(get_preset_flow_variant_idx(restored, ConfigFlowDomain::Filament, fvtHighFlow) == 1);
}

TEST_CASE("Mixed project owner indices preserve each material's selected values", "[FlowVariant]")
{
    auto config = make_variant_config({standard, high_flow, standard, standard, high_flow}, {12, 40, 22, 14, 88});
    config.set_key_value("filament_diameter", new ConfigOptionFloats{1.75, 1.75, 1.75});
    config.set_key_value("filament_self_index", new ConfigOptionInts{1, 1, 2, 3, 3});
    routing(config, {fvtHighFlow, fvtStandard, fvtStandard});
    normalize_filament_variant_metadata(config);
    REQUIRE(config.option<ConfigOptionInts>("filament_flow_step_size")->values == std::vector<int>{2, 1, 2});
    REQUIRE(speed(config, 0) == 40);
    REQUIRE(speed(config, 1) == 22);
    REQUIRE(speed(config, 2) == 14);
    ProfileFile file;
    REQUIRE(save(config, file)["filament_self_index"] == json({"1", "1", "2", "3", "3"}));
    auto restored = load(file);
    REQUIRE(speed(restored, 0) == 40);
    REQUIRE(speed(restored, 1) == 22);
    REQUIRE(speed(restored, 2) == 14);
}

TEST_CASE("Interleaved owners regroup complete rows without swapping values", "[FlowVariant]")
{
    auto config = make_variant_config({standard, high_flow, high_flow, standard}, {22, 40, 44, 12});
    config.set_key_value("filament_self_index", new ConfigOptionInts{2, 1, 2, 1});
    config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.92, 0.94, 0.96, 0.98});
    routing(config, {fvtStandard, fvtHighFlow});
    normalize_filament_variant_metadata(config);
    REQUIRE(config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>{1, 1, 2, 2});
    REQUIRE(config.option<ConfigOptionFloats>("filament_flow_ratio")->values == std::vector<double>{0.94, 0.98, 0.92, 0.96});
    REQUIRE(speed(config, 0) == 12);
    REQUIRE(speed(config, 1) == 44);
    const auto names = filament_extruder_variants(config);
    normalize_filament_variant_metadata(config);
    REQUIRE(filament_extruder_variants(config) == names);
    REQUIRE(speed(config, 0) == 12);
    REQUIRE(speed(config, 1) == 44);
}

TEST_CASE("Per-material and global values broadcast into their own variant rows", "[FlowVariant]")
{
    auto config = make_variant_config({standard, high_flow, standard, high_flow}, {12, 22});
    config.set_key_value("filament_self_index", new ConfigOptionInts{1, 1, 2, 2});
    config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.97});
    normalize_filament_variant_metadata(config);
    REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{12, 12, 22, 22});
    REQUIRE(config.option<ConfigOptionFloats>("filament_flow_ratio")->values == std::vector<double>{0.97, 0.97, 0.97, 0.97});
}

TEST_CASE("Older multi-material projects retain all values through repeated normalization", "[FlowVariant]")
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_diameter", new ConfigOptionFloats{1.75, 1.75, 1.75, 1.75});
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{12, 22, 30, 44});
    normalize_filament_variant_metadata(config);
    normalize_filament_variant_metadata(config);
    REQUIRE(filament_extruder_variants(config) == std::vector<std::string>(4, standard));
    REQUIRE(config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>{1, 2, 3, 4});
    REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{12, 22, 30, 44});
}

TEST_CASE("Default variant metadata is independent of material and physical nozzle counts", "[FlowVariant]")
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(3);
    config.set_num_extruders(4);
    REQUIRE(filament_extruder_variants(config) == std::vector<std::string>{standard});
    REQUIRE(config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>{1});
}

TEST_CASE("Explicit common metadata wins and legacy children override their parent", "[FlowVariant]")
{
    auto parent = make_variant_config({standard}, {12});
    parent.set_key_value("filament_flow_support", new ConfigOptionStrings{"high_flow"});
    normalize_filament_variant_metadata(parent);
    REQUIRE(parent.option<ConfigOptionStrings>("filament_flow_support")->values == std::vector<std::string>{"standard"});
    ProfileFile file;
    file.write({{"name", "Child"},
                {"from", "User"},
                {"version", "2.4.0.0"},
                {"inherits", "Parent"},
                {"filament_flow_support", {"standard", "high_flow"}},
                {"filament_max_volumetric_speed", {"12", "35"}}});
    const auto child = load(file);
    parent.apply(child);
    normalize_filament_variant_metadata(parent);
    REQUIRE(filament_extruder_variants(parent) == std::vector<std::string>{standard, high_flow});
    REQUIRE(get_preset_flow_variant_idx(parent, ConfigFlowDomain::Filament, fvtHighFlow) == 1);
}

TEST_CASE("Invalid owner tables are rejected without changing the original file", "[FlowVariant]")
{
    ProfileFile file;
    const json  original{{"name", "Unsupported layout"},
                         {"from", "User"},
                         {"version", "2.4.0.0"},
                         {"filament_extruder_variant", {standard, high_flow}},
                         {"filament_self_index", {"1", "3"}}};
    file.write(original);
    DynamicPrintConfig                 config;
    std::map<std::string, std::string> metadata;
    std::string                        reason;
    REQUIRE_THROWS(config.load_from_json(file.path.string(), ForwardCompatibilitySubstitutionRule::Enable, metadata, reason));
    REQUIRE(reason.empty());
    REQUIRE(file.read() == original);
}

TEST_CASE("Collection loading preserves unsupported material files", "[FlowVariant]")
{
    const auto directory = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca-collection-%%%%-%%%%");
    boost::filesystem::create_directories(directory / "filament");
    ProfileFile file;
    file.path = directory / "filament" / "unsupported.json";
    const json original{{"name", "Unsupported layout"},
                        {"from", "User"},
                        {"version", "2.4.0.0"},
                        {"filament_extruder_variant", {standard, high_flow}},
                        {"filament_self_index", {"1", "3"}}};
    file.write(original);
    PresetCollection           collection(Preset::TYPE_FILAMENT, Preset::filament_options(),
                                          static_cast<const PrintRegionConfig&>(FullPrintConfig::defaults()));
    PresetsConfigSubstitutions substitutions;
    REQUIRE_NOTHROW(collection.load_presets(directory.string(), "filament", substitutions, ForwardCompatibilitySubstitutionRule::Enable));
    REQUIRE(file.read() == original);
    REQUIRE(collection.find_preset("Unsupported layout", false) == nullptr);
    boost::filesystem::remove(file.path);
    boost::filesystem::remove(directory / "filament");
    boost::filesystem::remove(directory);
}

TEST_CASE("Differential user saves keep the common declaration even when inherited", "[FlowVariant]")
{
    auto parent = make_variant_config({standard, high_flow}, {12, 30});
    normalize_filament_variant_metadata(parent);
    Preset child(Preset::TYPE_FILAMENT, "Calibrated HF");
    child.config                                                                        = parent;
    child.config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values[1] = 35;
    child.inherits()                                                                    = "Generic PLA";
    child.version                                                                       = *Semver::parse("2.4.0.0");
    // A legacy editor may have updated its declaration before the common view.
    child.config.option<ConfigOptionStrings>("filament_extruder_variant")->values = {standard};
    ProfileFile file;
    child.file = file.path.string();
    child.save(&parent);
    const auto saved = file.read();
    REQUIRE(saved["filament_extruder_variant"] == json({standard, high_flow}));
    REQUIRE(saved["inherits"] == "Generic PLA");
    REQUIRE_FALSE(saved.contains("filament_flow_support"));
    REQUIRE_FALSE(saved.contains("filament_flow_step_size"));
    REQUIRE_FALSE(saved.contains("filament_self_index"));
    REQUIRE(load(file).option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{12, 35});
}

TEST_CASE("Missing HF falls back to the named Standard row", "[FlowVariant]")
{
    auto config = make_variant_config({"Future Drive Variant", standard}, {88, 12});
    config.set_key_value("filament_self_index", new ConfigOptionInts{1, 1});
    routing(config, {fvtHighFlow});
    normalize_filament_variant_metadata(config);
    REQUIRE(get_preset_flow_variant_idx(config, ConfigFlowDomain::Filament, fvtHighFlow) == 1);
    REQUIRE(speed(config, 0) == 12);
}

TEST_CASE("Legacy project overrides retain the common declaration through inheritance", "[FlowVariant]")
{
    PresetCollection collection(Preset::TYPE_FILAMENT, Preset::filament_options(),
                                static_cast<const PrintRegionConfig&>(FullPrintConfig::defaults()));
    auto             parent                                                    = collection.default_preset().config;
    parent.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {12};
    normalize_filament_variant_metadata(parent);
    collection.load_preset("unused-parent.json", "Generic PLA", parent, false);
    auto child = make_variant_config({standard, high_flow}, {12, 30});
    child.set_key_value("inherits", new ConfigOptionString("Generic PLA"));
    normalize_filament_variant_metadata(child);
    const auto loaded = collection.load_external_preset("legacy-project.3mf", "legacy-project.3mf", "Legacy HF", child,
                                                        {"inherits", "filament_flow_support", "filament_max_volumetric_speed"},
                                                        PresetCollection::LoadAndSelect::Always, *Semver::parse("2.4.0.0"), "");
    REQUIRE(filament_extruder_variants(loaded.first->config) == std::vector<std::string>{standard});
    auto edited = collection.get_edited_preset();
    REQUIRE(filament_extruder_variants(edited.config) == std::vector<std::string>{standard, high_flow});
    REQUIRE(edited.config.option<ConfigOptionStrings>("filament_flow_support")->values ==
            std::vector<std::string>{"standard", "high_flow"});
    REQUIRE(edited.config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{12, 30});
    ProfileFile file;
    edited.file = file.path.string();
    edited.save(&parent);
    REQUIRE(file.read()["filament_extruder_variant"] == json({standard, high_flow}));
    REQUIRE(get_preset_flow_variant_idx(load(file), ConfigFlowDomain::Filament, fvtHighFlow) == 1);
}

TEST_CASE("Project material splitting and recomposition preserve the common rows", "[FlowVariant]")
{
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    config.set_key_value("filament_diameter", new ConfigOptionFloats{1.75, 1.75, 1.75});
    config.set_key_value("filament_extruder_variant", new ConfigOptionStrings{high_flow, standard, standard, standard, high_flow});
    config.set_key_value("filament_self_index", new ConfigOptionInts{1, 1, 2, 3, 3});
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{40, 12, 22, 14, 88});
    config.set_key_value("filament_settings_id", new ConfigOptionStrings{"First material", "Second material", "Third material"});
    config.set_key_value("filament_colour", new ConfigOptionStrings{"#FF0000", "#00FF00", "#0000FF"});
    routing(config, {fvtHighFlow, fvtStandard, fvtStandard});
    normalize_filament_variant_metadata(config);
    INFO("Constructing the preset bundle");
    PresetBundle bundle;
    INFO("Splitting the project into material presets");
    bundle.load_config_model("common-variants.3mf", std::move(config), *Semver::parse("2.4.0.0"));
    INFO("Recomposing the material presets");
    const auto restored = bundle.full_config();
    REQUIRE(filament_extruder_variants(restored) == std::vector<std::string>{high_flow, standard, standard, standard, high_flow});
    REQUIRE(restored.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>{1, 1, 2, 3, 3});
    REQUIRE(restored.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{40, 12, 22, 14, 88});
    REQUIRE(speed(restored, 0) == 40);
    REQUIRE(speed(restored, 1) == 22);
    REQUIRE(speed(restored, 2) == 14);
}
