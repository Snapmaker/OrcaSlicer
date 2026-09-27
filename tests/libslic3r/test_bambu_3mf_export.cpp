// "Export Bambu 3MF" and the honest Application tag on normal saves.
// See docs/bambu-3mf-export.md and src/libslic3r/Format/BambuExport.{hpp,cpp}.
#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/miniz_extension.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <regex>

using namespace Slic3r;
using nlohmann::json;

namespace {

std::string zip_entry(const std::string &zip_path, const std::string &entry_name)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, zip_path))
        return {};
    size_t      size = 0;
    void       *data = mz_zip_reader_extract_file_to_heap(&archive, entry_name.c_str(), &size, 0);
    std::string result;
    if (data != nullptr) {
        result.assign(static_cast<const char *>(data), size);
        mz_free(data);
    }
    close_zip_reader(&archive);
    return result;
}

std::string application_tag_of(const std::string &zip_path)
{
    const std::string model = zip_entry(zip_path, "3D/3dmodel.model");
    std::smatch       m;
    static const std::regex re("<metadata name=\"Application\">([^<]*)</metadata>");
    return std::regex_search(model, m, re) ? m[1].str() : std::string();
}

// Every key="..." of a <metadata> element in model_settings.config / layer_config_ranges.xml.
std::vector<std::string> xml_config_keys(const std::string &xml)
{
    std::vector<std::string> keys;
    static const std::regex  re("(?:key|opt_key)=\"([^\"]+)\"");
    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), re); it != std::sregex_iterator(); ++it)
        keys.push_back((*it)[1].str());
    return keys;
}

// A project config with every option present, the way the application's full_config() is.
DynamicPrintConfig project_config(size_t extruders, size_t filaments)
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    // coEnums of a static config have no keys_map and the project writer serializes them (see the
    // support-group scenario in test_3mf.cpp); rebuild them from print_config_def.
    for (const std::string &key : cfg.keys())
        if (const ConfigOption *opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    cfg.set_num_extruders(unsigned(extruders));
    cfg.set_num_filaments(unsigned(filaments));
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values = std::vector<double>(extruders, 0.4);
    std::vector<std::string> colours;
    for (size_t i = 0; i < filaments; ++i)
        colours.push_back(i % 2 ? "#00FF00" : "#FF0000");
    cfg.option<ConfigOptionStrings>("filament_colour")->values = colours;
    cfg.set_key_value("printer_model", new ConfigOptionString("Bambu Lab P1S"));
    return cfg;
}

// The model every scenario stores: one object with two instances, a painted part and a modifier,
// per-object / per-part / per-height-range settings.
void build_model(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "painted_cube";
    TriangleMesh cube   = make_cube(20., 20., 20.);
    ModelVolume *part   = object->add_volume(cube);
    part->name          = "cube";
    ModelVolume *mod    = object->add_volume(make_cube(5., 5., 5.), ModelVolumeType::PARAMETER_MODIFIER);
    mod->name           = "modifier";
    object->add_instance();
    object->add_instance()->set_offset({ 60., 0., 0. });
    object->ensure_on_bed();

    TriangleSelector selector(cube);
    for (int facet = 0; facet < 4; ++facet)
        selector.set_facet(facet, EnforcerBlockerType(facet % 2 + 1));
    REQUIRE(part->mmu_segmentation_facets.set(selector));

    object->config.set_key_value("wall_loops", new ConfigOptionInt(3));                                // Bambu key
    object->config.set_key_value("make_overhang_printable", new ConfigOptionBool(true));               // ours only
    object->config.set_key_value("infill_anchor", new ConfigOptionFloatOrPercent(2.5, false));         // renamed
    object->config.set_key_value("sparse_infill_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
    mod->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(30));
    mod->config.set_key_value("zaa_enabled", new ConfigOptionBool(true));                             // ours only
    DynamicPrintConfig range;
    range.set_key_value("sparse_infill_pattern", new ConfigOptionEnum<InfillPattern>(ipGyroid));
    range.set_key_value("skirt_speed", new ConfigOptionFloat(40));                                    // ours only
    object->layer_config_ranges[{ 0., 5. }].assign_config(range);
}

// Settings chosen to exercise every kind of conversion.
void set_conversion_values(DynamicPrintConfig &cfg)
{
    cfg.set_key_value("sparse_infill_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));   // -> "zig-zag"
    cfg.set_key_value("line_width", new ConfigOptionFloatOrPercent(110, true));                       // 110% of 0.4 -> 0.44
    cfg.set_key_value("infill_anchor", new ConfigOptionFloatOrPercent(400, true));                    // -> sparse_infill_anchor
    cfg.set_key_value("only_one_wall_top", new ConfigOptionBool(true));                               // -> top_one_wall_type
    cfg.set_key_value("ensure_vertical_shell_thickness", new ConfigOptionEnum<EnsureVerticalShellThickness>(evstAll));
    cfg.set_key_value("seam_position", new ConfigOptionEnum<SeamPosition>(spAlignedBack));            // no Bambu value
    cfg.set_key_value("seam_gap", new ConfigOptionFloatOrPercent(0.1, false));                        // mm -> 25%
    cfg.set_key_value("travel_speed_z", new ConfigOptionFloat(12));                                   // scalar -> vector
    cfg.set_key_value("ironing_speed", new ConfigOptionFloats({ 30. }));                              // vector -> scalar
    cfg.set_key_value("make_overhang_printable", new ConfigOptionBool(true));                         // ours only
    cfg.set_key_value("ironing_angle", new ConfigOptionFloat(-1));                                    // "default": left out
    cfg.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
    cfg.set_key_value("ooze_prevention", new ConfigOptionBool(true));                                 // Bambu: not with a tower
    auto *fan = cfg.option<ConfigOptionFloats>("fan_max_speed", true);                                // float -> int
    fan->values = std::vector<double>(cfg.option<ConfigOptionStrings>("filament_colour")->size(), 95.6);
}

struct Loaded
{
    bool                  ok = false;
    Model                 model;
    DynamicPrintConfig    config;
    PlateDataPtrs         plates;
    std::vector<Preset *> presets;
    bool                  is_bbl_3mf = false;
    Semver                version;
    ~Loaded()
    {
        release_PlateData_list(plates);
        for (Preset *p : presets) delete p;
    }
};

void load(const std::string &path, Loaded &out, bool with_config = true)
{
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence;
    if (with_config)
        strategy = strategy | LoadStrategy::LoadConfig;
    out.ok = load_bbs_3mf(path.c_str(), &out.config, &ctxt, &out.model, &out.plates, &out.presets, &out.is_bbl_3mf, &out.version,
                          nullptr, strategy);
}

std::string temp_file(const std::string &name)
{
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() /
        ("snorca_tests_bambu3mf_" + std::to_string(get_current_pid()));
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    return (tmp_root / name).string();
}

bool store(const std::string &path, Model &model, DynamicPrintConfig &cfg, PlateDataPtrs plates, std::vector<Preset *> presets,
           BambuExport::Report *bambu_report)
{
    StoreParams sp;
    sp.path            = path.c_str();
    sp.model           = &model;
    sp.config          = &cfg;
    sp.plate_data_list = plates;
    sp.project_presets = presets;
    sp.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    sp.bambu_compat    = bambu_report != nullptr;
    sp.bambu_report    = bambu_report;
    return store_bbs_3mf(sp);
}

PlateDataPtrs two_plates()
{
    PlateData *a = new PlateData();
    a->plate_index = 0;
    a->objects_and_instances.emplace_back(0, 0);
    a->config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(btPEI));
    PlateData *b = new PlateData();
    b->plate_index = 1;
    b->objects_and_instances.emplace_back(0, 1);
    b->config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(btPCT)); // Textured Cool Plate: not in Bambu
    return { a, b };
}

// True if our loader drops or renames the key (handle_legacy), so it cannot come back unchanged.
bool retired_on_load(const std::string &key)
{
    t_config_option_key legacy_key = key;
    std::string         value;
    PrintConfigDef::handle_legacy(legacy_key, value);
    return legacy_key != key;
}

// Bambu shape check for one exported JSON config: every key known to Bambu, scalar/vector as Bambu
// declares it, enum values from Bambu's list.
void check_bambu_shape(const json &j, const std::string &where)
{
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string &key = it.key();
        if (key == "version" || key == "name" || key == "from" || key == "is_custom_defined")
            continue;
        INFO(where << ": key " << key);
        const BambuExport::BambuKeyDef *def = BambuExport::find_key(key);
        REQUIRE(def != nullptr);
        const std::string type(def->type);
        const bool        vector = type != "coPoint3" && boost::algorithm::ends_with(type, "s");
        CHECK(it.value().is_array() == vector);
        if (def->enum_values) {
            std::vector<std::string> accepted;
            std::string              joined(def->enum_values);
            boost::algorithm::split(accepted, joined, boost::is_any_of("|"));
            auto check_value = [&](const std::string &v) {
                INFO("value " << v);
                CHECK((v == "nil" || std::find(accepted.begin(), accepted.end(), v) != accepted.end()));
            };
            if (it.value().is_array())
                for (const auto &v : it.value()) check_value(v.get<std::string>());
            else
                check_value(it.value().get<std::string>());
        }
    }
}

} // namespace

TEST_CASE("The Bambu key table is generated from Bambu Studio and is sane", "[3mf][BambuExport]")
{
    CHECK(BambuExport::bambu_key_defs().size() > 500);
    CHECK(BambuExport::find_key("wall_loops") != nullptr);
    CHECK(BambuExport::find_key("sparse_infill_anchor") != nullptr);
    // Declared by Bambu but thrown away by its own handle_legacy(): never worth writing.
    CHECK(BambuExport::find_key("prime_volume") == nullptr);
    CHECK(BambuExport::find_key("only_one_wall_top") == nullptr);
    // Ours only.
    CHECK(BambuExport::find_key("make_overhang_printable") == nullptr);
    CHECK(BambuExport::export_version() == "02.08.00.00");
    CHECK(BambuExport::application_tag() == "BambuStudio-02.08.00.00");
}

// If one of our enum options gains a value, the generated translation table must be regenerated
// (scripts/gen_bambu_known_keys.py) so the export knows whether Bambu has it.
TEST_CASE("Every value of our enum options has a known Bambu fate", "[3mf][BambuExport]")
{
    for (const BambuExport::BambuKeyDef &bdef : BambuExport::bambu_key_defs()) {
        if (!bdef.enum_values)
            continue;
        const ConfigOptionDef *ours = print_config_def.get(bdef.key);
        if (ours == nullptr || ours->enum_keys_map == nullptr || (ours->type != coEnum && ours->type != coEnums))
            continue;
        std::vector<std::string> accepted;
        std::string              joined(bdef.enum_values);
        boost::algorithm::split(accepted, joined, boost::is_any_of("|"));
        for (const auto &kv : *ours->enum_keys_map) {
            if (std::find(accepted.begin(), accepted.end(), kv.first) != accepted.end())
                continue;
            bool listed = false;
            for (const BambuExport::EnumTranslation &t : BambuExport::generated_enum_translations())
                listed |= bdef.key == std::string(t.key) && kv.first == t.ours;
            listed |= !BambuExport::translate_enum_value(bdef.key, kv.first).empty();
            INFO("option " << bdef.key << " value " << kv.first << ": regenerate BambuKnownKeys.cpp");
            CHECK(listed);
        }
    }
}

SCENARIO("A normal project save names this application and still loads as a full project", "[3mf][BambuExport]")
{
    Model model;
    build_model(model);
    DynamicPrintConfig cfg = project_config(1, 2);
    set_conversion_values(cfg);
    const std::string path = temp_file("honest_tag.3mf");
    REQUIRE(store(path, model, cfg, two_plates(), {}, nullptr));

    THEN("the Application tag is EdgeSlicer-<version>, not BambuStudio-") {
        CHECK(application_tag_of(path) == std::string(SLIC3R_APP_NAME) + "-" + Snapmaker_VERSION);
    }
    THEN("the project settings are written unconverted") {
        const json j = json::parse(zip_entry(path, "Metadata/project_settings.config"));
        CHECK(j.contains("make_overhang_printable"));
        CHECK(j["sparse_infill_pattern"] == "rectilinear");
        CHECK(j["version"] == Snapmaker_VERSION);
    }
    THEN("we load it back as one of our projects with the whole config") {
        Loaded back;
        load(path, back);
        REQUIRE(back.ok);
        CHECK(back.is_bbl_3mf);
        CHECK(back.version == *Semver::parse(Snapmaker_VERSION));
        for (const std::string &key : cfg.keys()) {
            // handle_legacy_composite() rewrites "thumbnails" into its canonical spelling on load.
            if (retired_on_load(key) || key == "thumbnails")
                continue;
            INFO("option " << key);
            REQUIRE(back.config.has(key));
            CHECK(back.config.opt_serialize(key) == cfg.opt_serialize(key));
        }
        REQUIRE(back.model.objects.size() == 1);
        CHECK(back.model.objects[0]->config.get().has("make_overhang_printable"));
    }
    boost::filesystem::remove(path);
}

// A sliced-plate file (SkipModel) goes to Bambu printers and Bambu Studio's preview, not back into a
// slicer as a project; it keeps the tag it always had (see _add_model_file_to_archive).
TEST_CASE("A sliced-plate 3MF keeps the BambuStudio- tag", "[3mf][BambuExport]")
{
    Model model;
    build_model(model);
    DynamicPrintConfig cfg  = project_config(1, 2);
    const std::string  path = temp_file("sliced_plate.3mf");
    StoreParams        sp;
    sp.path     = path.c_str();
    sp.model    = &model;
    sp.config   = &cfg;
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary | SaveStrategy::SkipModel;
    REQUIRE(store_bbs_3mf(sp));
    CHECK(application_tag_of(path) == std::string("BambuStudio-") + Snapmaker_VERSION);
    boost::filesystem::remove(path);
}

SCENARIO("Export Bambu 3MF writes a project Bambu Studio loads with its settings", "[3mf][BambuExport]")
{
    Model model;
    build_model(model);
    DynamicPrintConfig cfg = project_config(1, 2);
    set_conversion_values(cfg);

    // An embedded print preset, as "save preset to project" produces.
    Preset *print_preset = new Preset(Preset::TYPE_PRINT, "My print", false);
    print_preset->config.set_key_value("print_settings_id", new ConfigOptionString("My print"));
    print_preset->config.set_key_value("layer_height", new ConfigOptionFloat(0.16));
    print_preset->config.set_key_value("make_overhang_printable", new ConfigOptionBool(true));
    print_preset->config.set_key_value("sparse_infill_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
    print_preset->config.set_key_value("outer_wall_speed", new ConfigOptionFloats({ 150. }));
    print_preset->config.set_key_value("print_extruder_variant", new ConfigOptionStrings({ "Direct Drive Standard" }));
    Preset *filament_preset = new Preset(Preset::TYPE_FILAMENT, "My PLA", false);
    filament_preset->config.set_key_value("filament_settings_id", new ConfigOptionStrings({ "My PLA" }));
    filament_preset->config.set_key_value("nozzle_temperature", new ConfigOptionInts({ 215 }));
    filament_preset->config.set_key_value("fan_max_speed", new ConfigOptionFloats({ 90.4 }));
    filament_preset->config.set_key_value("filament_shrinkage_compensation_z", new ConfigOptionPercent(100)); // ours only

    const std::string   path = temp_file("bambu_export.3mf");
    BambuExport::Report report;
    const bool          stored = store(path, model, cfg, two_plates(), { print_preset, filament_preset }, &report);
    delete print_preset;
    delete filament_preset;
    REQUIRE(stored);

    const json project = json::parse(zip_entry(path, "Metadata/project_settings.config"));

    THEN("the file says BambuStudio-<the Bambu Studio line the settings were converted for>") {
        CHECK(application_tag_of(path) == "BambuStudio-02.08.00.00");
        CHECK(project["version"] == "02.08.00.00");
        const std::string slice_info = zip_entry(path, "Metadata/slice_info.config");
        if (!slice_info.empty())
            CHECK(slice_info.find("\"02.08.00.00\"") != std::string::npos);
    }

    THEN("the project settings hold only keys Bambu Studio knows, in Bambu's shapes") {
        check_bambu_shape(project, "project_settings.config");
        CHECK(!project.contains("make_overhang_printable"));
        CHECK(!project.contains("infill_anchor"));
        CHECK(!project.contains("seam_position"));   // aligned_back has no Bambu equivalent
        CHECK(!project.contains("ironing_direction")); // our -1 means "default"
        CHECK(!project.contains("ooze_prevention"));   // Bambu refuses it together with a prime tower
        CHECK(project["enable_prime_tower"] == "1");
    }

    THEN("renamed, reshaped and translated values are right") {
        CHECK(project["sparse_infill_pattern"] == "zig-zag");
        CHECK(project["sparse_infill_anchor"] == "400%");
        CHECK(project["top_one_wall_type"] == "all top");
        CHECK(project["ensure_vertical_shell_thickness"] == "enabled");
        CHECK(project["line_width"] == "0.44");
        CHECK(project["seam_gap"] == "25%");
        REQUIRE(project["travel_speed_z"].is_array());
        CHECK(project["travel_speed_z"][0] == "12");
        CHECK(project["ironing_speed"] == "30");
        REQUIRE(project["fan_max_speed"].is_array());
        CHECK(project["fan_max_speed"][0] == "96");
    }

    THEN("per-extruder-variant vectors follow the variant layout") {
        const size_t print_variants    = project["print_extruder_variant"].size();
        const size_t filament_variants = project["filament_extruder_variant"].size();
        CHECK(print_variants == 1);
        CHECK(filament_variants == 2);
        CHECK(project["filament_self_index"] == json::array({ "1", "2" }));
        for (const BambuExport::BambuKeyDef &def : BambuExport::bambu_key_defs()) {
            if (!project.contains(def.key) || !project[def.key].is_array())
                continue;
            INFO("option " << def.key);
            if (def.variant == BambuExport::VariantClass::Print)
                CHECK(project[def.key].size() == print_variants);
            else if (def.variant == BambuExport::VariantClass::Filament)
                CHECK(project[def.key].size() == filament_variants);
        }
    }

    THEN("the embedded presets are converted the same way") {
        const json print = json::parse(zip_entry(path, "Metadata/process_settings_1.config"));
        check_bambu_shape(print, "process_settings_1.config");
        CHECK(print["name"] == "My print");
        CHECK(print["version"] == "02.08.00.00");
        CHECK(print["sparse_infill_pattern"] == "zig-zag");
        CHECK(!print.contains("make_overhang_printable"));
        const json filament = json::parse(zip_entry(path, "Metadata/filament_settings_1.config"));
        check_bambu_shape(filament, "filament_settings_1.config");
        CHECK(filament["fan_max_speed"] == json::array({ "90" }));
        CHECK(!filament.contains("filament_shrinkage_compensation_z"));
    }

    THEN("per-object, per-part and per-height-range settings are filtered and translated") {
        const std::string model_settings = zip_entry(path, "Metadata/model_settings.config");
        for (const std::string &key : xml_config_keys(model_settings)) {
            if (print_config_def.get(key) == nullptr)
                continue; // structural metadata (name, matrix, source_*, plate attributes ...)
            INFO("model_settings.config key " << key);
            CHECK(BambuExport::find_key(key) != nullptr);
        }
        CHECK(model_settings.find("key=\"wall_loops\" value=\"3\"") != std::string::npos);
        CHECK(model_settings.find("key=\"sparse_infill_anchor\" value=\"2.5\"") != std::string::npos);
        CHECK(model_settings.find("key=\"sparse_infill_pattern\" value=\"zig-zag\"") != std::string::npos);
        CHECK(model_settings.find("make_overhang_printable") == std::string::npos);
        CHECK(model_settings.find("zaa_enabled") == std::string::npos);
        // Plate 2's Textured Cool Plate has no Bambu equivalent: the plate follows the project.
        CHECK(model_settings.find("High Temp Plate") != std::string::npos);
        CHECK(model_settings.find("Textured Cool Plate") == std::string::npos);

        const std::string ranges = zip_entry(path, "Metadata/layer_config_ranges.xml");
        CHECK(ranges.find("gyroid") != std::string::npos);
        CHECK(ranges.find("skirt_speed") == std::string::npos);
    }

    THEN("the report names what was left out") {
        CHECK(report.dropped.count("make_overhang_printable"));
        CHECK(report.dropped.count("seam_position"));
        CHECK(report.dropped.count("zaa_enabled"));
        CHECK(report.dropped.count("filament_shrinkage_compensation_z"));
        CHECK(report.renamed.at("infill_anchor") == "sparse_infill_anchor");
        CHECK(report.converted.count("line_width"));
    }

    THEN("we still load the Bambu export as a full project, with geometry, paint, plates and settings") {
        Loaded back;
        load(path, back);
        REQUIRE(back.ok);
        CHECK(back.is_bbl_3mf);
        CHECK(back.version.maj() == 2);
        CHECK(back.version.min() == 8);
        CHECK(back.plates.size() == 2);
        REQUIRE(back.model.objects.size() == 1);
        const ModelObject *obj = back.model.objects[0];
        CHECK(obj->instances.size() == 2);
        REQUIRE(obj->volumes.size() == 2);
        CHECK(obj->volumes[1]->is_modifier());
        CHECK(obj->volumes[0]->mmu_segmentation_facets.get_data() == model.objects[0]->volumes[0]->mmu_segmentation_facets.get_data());
        CHECK(obj->config.get().opt_int("wall_loops") == 3);
        CHECK(obj->config.get().opt_serialize("infill_anchor") == "2.5");
        CHECK(obj->config.get().opt_serialize("sparse_infill_pattern") == "rectilinear");
        CHECK(obj->layer_config_ranges.size() == 1);
        CHECK(back.config.opt_serialize("sparse_infill_pattern") == "rectilinear");
        CHECK(back.config.opt_serialize("infill_anchor") == "400%");
        CHECK(back.config.opt_bool("only_one_wall_top"));
        CHECK(back.config.opt_serialize("ensure_vertical_shell_thickness") == "ensure_all");
        CHECK(back.config.option<ConfigOptionFloatOrPercent>("line_width")->get_abs_value(0.4) == Approx(0.44));
        CHECK(back.presets.size() == 2);
    }
    boost::filesystem::remove(path);
}

SCENARIO("Export Bambu 3MF lays a two-extruder project out the way Bambu Studio indexes it", "[3mf][BambuExport]")
{
    Model model;
    build_model(model);
    DynamicPrintConfig cfg = project_config(2, 2);
    // Two direct-drive extruders, the second with a high-flow nozzle; our own variant lists only
    // name extruder 1, which is the shape a non-Bambu dual-extruder profile has.
    cfg.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values      = { int(etDirectDrive) };
    cfg.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = { int(nvtStandard), int(nvtHighFlow) };
    cfg.set_key_value("printer_extruder_variant", new ConfigOptionStrings({ "Direct Drive Standard" }));
    cfg.set_key_value("printer_extruder_id", new ConfigOptionInts({ 1 }));
    cfg.set_key_value("print_extruder_variant", new ConfigOptionStrings({ "Direct Drive Standard" }));
    cfg.set_key_value("print_extruder_id", new ConfigOptionInts({ 1 }));
    cfg.set_key_value("retraction_length", new ConfigOptionFloats({ 0.8, 1.2 }));
    cfg.set_key_value("outer_wall_speed", new ConfigOptionFloats({ 200. }));
    cfg.set_key_value("nozzle_temperature", new ConfigOptionInts({ 220, 240 }));
    cfg.set_key_value("machine_max_speed_x", new ConfigOptionFloats({ 500., 200. }));
    cfg.set_key_value("flush_volumes_matrix", new ConfigOptionFloats({ 0., 100., 120., 0. }));
    cfg.set_key_value("extruder_nozzle_stats", new ConfigOptionStrings());

    const std::string   path = temp_file("bambu_export_dual.3mf");
    BambuExport::Report report;
    REQUIRE(store(path, model, cfg, {}, {}, &report));
    const json j = json::parse(zip_entry(path, "Metadata/project_settings.config"));
    check_bambu_shape(j, "project_settings.config");

    CHECK(j["printer_extruder_variant"] == json::array({ "Direct Drive Standard", "Direct Drive High Flow" }));
    CHECK(j["printer_extruder_id"] == json::array({ "1", "2" }));
    CHECK(j["print_extruder_variant"] == j["printer_extruder_variant"]);
    CHECK(j["print_extruder_id"] == j["printer_extruder_id"]);
    // One filament slot per (filament, printer variant).
    CHECK(j["filament_extruder_variant"] == json::array({ "Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Standard", "Direct Drive High Flow" }));
    CHECK(j["filament_self_index"] == json::array({ "1", "1", "2", "2" }));
    // Bambu's check_project_config: extruder_type must have one entry per nozzle.
    CHECK(j["extruder_type"].size() == 2);
    CHECK(j["nozzle_volume_type"] == json::array({ "Standard", "High Flow" }));
    CHECK(j["retraction_length"] == json::array({ "0.8", "1.2" }));
    CHECK(j["outer_wall_speed"] == json::array({ "200", "200" }));
    CHECK(j["nozzle_temperature"] == json::array({ "220", "220", "240", "240" }));
    CHECK(j["machine_max_speed_x"] == json::array({ "500", "200", "500", "200" }));
    // Flushing is per extruder in Bambu: one multiplier and one filament x filament matrix each.
    CHECK(j["flush_multiplier"].size() == 2);
    CHECK(j["flush_volumes_matrix"].size() == 2 * 2 * 2);
    // Present-but-empty makes Bambu's CLI fail every multi-extruder slice; absent, it is derived.
    CHECK(!j.contains("extruder_nozzle_stats"));
    boost::filesystem::remove(path);
}

// Write a real-printer project twice - a normal save and a Bambu export - for a live check in
// Bambu Studio's CLI. The config is what the GUI stores: PresetBundle::full_config_secure() of
// system presets loaded from this tree's resources/profiles. The geometry (with paint) comes from
// another 3MF; every object goes on one plate.
//   BAMBU_LIVE_VENDOR=BBL BAMBU_LIVE_PRINTER="Bambu Lab H2C 0.4 nozzle"
//   BAMBU_LIVE_PROCESS="0.20mm Standard @BBL H2C" BAMBU_LIVE_FILAMENTS="Bambu PLA Basic @BBL H2C;..."
//   BAMBU_LIVE_MODEL=<geometry.3mf> BAMBU_LIVE_OUT=<prefix>  libslic3r_tests "[.bambu_live]"
// writes <prefix>_normal.3mf and <prefix>_bambu.3mf. Hidden: it needs files outside the repository.
TEST_CASE("Write a preset-based project, normal and Bambu flavours, for a live Bambu Studio check", "[.bambu_live]")
{
    auto env = [](const char *name) { const char *v = std::getenv(name); return v ? std::string(v) : std::string(); };
    const std::string vendor = env("BAMBU_LIVE_VENDOR").empty() ? "BBL" : env("BAMBU_LIVE_VENDOR");
    const std::string printer = env("BAMBU_LIVE_PRINTER"), process = env("BAMBU_LIVE_PROCESS");
    const std::string model_path = env("BAMBU_LIVE_MODEL"), out = env("BAMBU_LIVE_OUT");
    std::vector<std::string> filaments;
    const std::string        filament_list = env("BAMBU_LIVE_FILAMENTS");
    boost::algorithm::split(filaments, filament_list, boost::is_any_of(";"));
    REQUIRE(!printer.empty());
    REQUIRE(!process.empty());
    REQUIRE(!model_path.empty());
    REQUIRE(!out.empty());
    REQUIRE(!filament_list.empty());

    // Keep anything the preset loader writes (caches) out of the user's data directory.
    const std::string saved_data_dir = data_dir();
    set_data_dir((boost::filesystem::path(temp_file("live")).parent_path() / "datadir").string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    PresetBundle library;
    library.load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    PresetBundle bundle;
    bundle.load_vendor_configs_from_json(profiles, vendor, PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent, &library);
    REQUIRE(bundle.printers.select_preset_by_name(printer, true));
    REQUIRE(bundle.prints.select_preset_by_name(process, true));
    REQUIRE(bundle.filaments.select_preset_by_name(filaments.front(), true));
    bundle.filament_presets = { filaments.front() };
    std::vector<std::string> colours;
    for (size_t i = 0; i < filaments.size(); ++i)
        colours.push_back(i % 3 == 0 ? "#E01919" : i % 3 == 1 ? "#19B23F" : "#1943E0");
    bundle.set_num_filaments(unsigned(filaments.size()), colours);
    bundle.filament_presets = filaments;
    DynamicPrintConfig cfg = bundle.full_config_secure();
    // BAMBU_LIVE_SET="key=value;key=value": distinctive values to look for in Bambu's G-code.
    std::vector<std::string> sets;
    const std::string        set_list = env("BAMBU_LIVE_SET");
    if (!set_list.empty())
        boost::algorithm::split(sets, set_list, boost::is_any_of(";"));
    ConfigSubstitutionContext set_ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
    for (const std::string &kv : sets) {
        const size_t eq = kv.find('=');
        REQUIRE(eq != std::string::npos);
        cfg.set_deserialize(kv.substr(0, eq), kv.substr(eq + 1), set_ctxt);
    }

    Loaded src;
    load(model_path, src, false);
    REQUIRE(src.ok);
    for (ModelObject *o : src.model.objects)
        for (ModelVolume *v : o->volumes)
            if (v->config.has("extruder") && v->config.opt_int("extruder") > int(filaments.size()))
                v->config.set("extruder", int(filaments.size()));
    const BoundingBoxf bed(cfg.option<ConfigOptionPoints>("printable_area")->values);
    src.model.center_instances_around_point(bed.center());
    auto plates = [&src]() {
        PlateData *plate   = new PlateData();
        plate->plate_index = 0;
        for (size_t o = 0; o < src.model.objects.size(); ++o)
            for (size_t i = 0; i < src.model.objects[o]->instances.size(); ++i)
                plate->objects_and_instances.emplace_back(int(o), int(i));
        return PlateDataPtrs{ plate };
    };
    PlateDataPtrs normal_plates = plates(), bambu_plates = plates();
    REQUIRE(store(out + "_normal.3mf", src.model, cfg, normal_plates, {}, nullptr));
    BambuExport::Report report;
    REQUIRE(store(out + "_bambu.3mf", src.model, cfg, bambu_plates, {}, &report));
    release_PlateData_list(normal_plates);
    release_PlateData_list(bambu_plates);
    set_data_dir(saved_data_dir);
    WARN(out << "_bambu.3mf: " << report.summary());
    for (const std::string &note : report.notes)
        WARN(note);
}

TEST_CASE("Bambu Studio's spellings of two settings load as what they mean", "[3mf][BambuExport][Config]")
{
    auto legacy = [](std::string key, std::string value) {
        PrintConfigDef::handle_legacy(key, value);
        return key + "=" + value;
    };
    CHECK(legacy("ensure_vertical_shell_thickness", "enabled") == "ensure_vertical_shell_thickness=ensure_all");
    CHECK(legacy("ensure_vertical_shell_thickness", "partial") == "ensure_vertical_shell_thickness=ensure_moderate");
    CHECK(legacy("ensure_vertical_shell_thickness", "disabled") == "ensure_vertical_shell_thickness=none");
    CHECK(legacy("top_one_wall_type", "all top") == "only_one_wall_top=1");
    CHECK(legacy("top_one_wall_type", "topmost") == "only_one_wall_top=1");
    CHECK(legacy("top_one_wall_type", "not apply") == "only_one_wall_top=0");
}

// Aligned front (Auto-paint seam, 2026-09-25): a seam position of ours that Bambu Studio does not have. It must come
// back unchanged from our own project files, and export to Bambu as its closest equivalent, plain "aligned".
TEST_CASE("Aligned front round-trips through a project and exports to Bambu as aligned", "[3mf][BambuExport][Seam]")
{
    CHECK(BambuExport::translate_enum_value("seam_position", "aligned_front") == "aligned");

    Model model;
    build_model(model);
    model.objects.front()->config.set_key_value("seam_position", new ConfigOptionEnum<SeamPosition>(spAlignedFront));
    DynamicPrintConfig cfg = project_config(1, 2);
    cfg.set_key_value("seam_position", new ConfigOptionEnum<SeamPosition>(spAlignedFront));

    SECTION("our own project")
    {
        const std::string path = temp_file("aligned_front.3mf");
        REQUIRE(store(path, model, cfg, two_plates(), {}, nullptr));
        const json project = json::parse(zip_entry(path, "Metadata/project_settings.config"));
        CHECK(project["seam_position"] == "aligned_front");
        Loaded back;
        load(path, back);
        REQUIRE(back.ok);
        CHECK(back.config.opt_enum<SeamPosition>("seam_position") == spAlignedFront);
        REQUIRE(back.model.objects.size() == 1);
        const ConfigOption *object_seam = back.model.objects[0]->config.option("seam_position");
        REQUIRE(object_seam != nullptr);
        CHECK(object_seam->serialize() == "aligned_front");
        boost::filesystem::remove(path);
    }
    SECTION("exported for Bambu Studio")
    {
        const std::string   path = temp_file("aligned_front_bambu.3mf");
        BambuExport::Report report;
        REQUIRE(store(path, model, cfg, two_plates(), {}, &report));
        const json project = json::parse(zip_entry(path, "Metadata/project_settings.config"));
        CHECK(project["seam_position"] == "aligned");
        const std::string model_settings = zip_entry(path, "Metadata/model_settings.config");
        CHECK(model_settings.find("key=\"seam_position\" value=\"aligned\"") != std::string::npos);
        CHECK(model_settings.find("aligned_front") == std::string::npos);
        boost::filesystem::remove(path);
    }
}
