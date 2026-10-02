// Bambu Studio's names for settings we spell differently, on the way IN: genuine Bambu projects,
// Bambu exports and Bambu presets keep those values instead of dropping them as unknown keys.
// See docs/bambu-config-compat.md ("Renamed keys") and src/libslic3r/BambuKeyAliases.{hpp,cpp}.
#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/Format/BambuKeyAliases.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/miniz_extension.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>

#include <cstdlib>
#include <iostream>
#include <map>
#include <set>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

struct CorpusCount
{
    size_t                        files   = 0;
    size_t                        failed  = 0;
    size_t                        dropped = 0; // sum over files of distinct unknown keys
    std::map<std::string, size_t> by_key;      // unknown key -> number of files dropping it
};

void count_dropped(const std::vector<std::string> &unrecognized, CorpusCount &c)
{
    const std::set<std::string> distinct(unrecognized.begin(), unrecognized.end());
    c.dropped += distinct.size();
    for (const std::string &k : distinct)
        ++c.by_key[k];
}

void count_json(const fs::path &file, CorpusCount &c)
{
    DynamicPrintConfig                 cfg;
    ConfigSubstitutionContext          ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    std::map<std::string, std::string> key_values;
    std::string                        reason;
    ++c.files;
    try {
        if (cfg.load_from_json(file.string(), ctxt, false, key_values, reason) != 0) {
            ++c.failed;
            return;
        }
    } catch (const std::exception &ex) {
        ++c.failed;
        std::cout << "BAMBU_CORPUS failed " << file.string() << ": " << ex.what() << "\n";
        return;
    }
    count_dropped(ctxt.unrecogized_keys, c);
}

// The project settings and the embedded presets (Metadata/*.config JSON), read the way the 3MF
// reader reads them, plus whatever a full project load drops on top (per-object settings). The
// JSON part is counted even when the full load fails, so one bad object does not hide the rest.
void count_3mf(const fs::path &file, CorpusCount &c)
{
    ++c.files;
    std::vector<std::string> unrecognized;
    mz_zip_archive           archive;
    mz_zip_zero_struct(&archive);
    if (open_zip_reader(&archive, file.string())) {
        const mz_uint n = mz_zip_reader_get_num_files(&archive);
        for (mz_uint i = 0; i < n; ++i) {
            mz_zip_archive_file_stat stat;
            if (!mz_zip_reader_file_stat(&archive, i, &stat))
                continue;
            const std::string name = stat.m_filename;
            if (!boost::algorithm::starts_with(name, "Metadata/") || !boost::algorithm::ends_with(name, ".config"))
                continue;
            size_t size = 0;
            void  *data = mz_zip_reader_extract_to_heap(&archive, i, &size, 0);
            if (data == nullptr)
                continue;
            const std::string text(static_cast<const char *>(data), size);
            mz_free(data);
            if (text.empty() || text.front() != '{')
                continue; // model_settings.config / slice_info.config are XML
            const fs::path tmp = fs::temp_directory_path() / ("snorca_bambu_corpus_" + std::to_string(get_current_pid()) + ".config");
            {
                boost::nowide::ofstream f(tmp.string(), std::ios::binary);
                f << text;
            }
            DynamicPrintConfig                 cfg;
            ConfigSubstitutionContext          ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
            std::map<std::string, std::string> key_values;
            std::string                        reason;
            try {
                cfg.load_from_json(tmp.string(), ctxt, false, key_values, reason);
            } catch (const std::exception &ex) {
                std::cout << "BAMBU_CORPUS failed " << file.string() << " " << name << ": " << ex.what() << "\n";
            }
            unrecognized.insert(unrecognized.end(), ctxt.unrecogized_keys.begin(), ctxt.unrecogized_keys.end());
            fs::remove(tmp);
        }
        close_zip_reader(&archive);
    }

    DynamicPrintConfig        cfg;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    Model                     model;
    PlateDataPtrs             plates;
    std::vector<Preset *>     presets;
    bool                      is_bbl = false;
    Semver                    version;
    try {
        if (!load_bbs_3mf(file.string().c_str(), &cfg, &ctxt, &model, &plates, &presets, &is_bbl, &version, nullptr,
                          LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence))
            ++c.failed;
    } catch (const std::exception &ex) {
        ++c.failed;
        std::cout << "BAMBU_CORPUS full load failed " << file.string() << ": " << ex.what() << "\n";
    }
    release_PlateData_list(plates);
    for (Preset *p : presets)
        delete p;
    unrecognized.insert(unrecognized.end(), ctxt.unrecogized_keys.begin(), ctxt.unrecogized_keys.end());
    count_dropped(unrecognized, c);
}

void print(const std::string &name, const CorpusCount &c)
{
    std::cout << "BAMBU_CORPUS " << name << ": files=" << c.files << " failed=" << c.failed << " dropped_keys=" << c.dropped
              << " distinct=" << c.by_key.size() << "\n";
    for (const auto &kv : c.by_key)
        std::cout << "BAMBU_CORPUS " << name << "   " << kv.first << " " << kv.second << "\n";
}

} // namespace

// How many keys our loader throws away as unknown, over a corpus of genuine Bambu Studio files.
//   BAMBU_CORPUS="<dir or file>;<dir or file>..."  libslic3r_tests "[.bambu_corpus]"
// Directories are walked recursively: *.3mf are loaded as projects, *.json as presets (not
// resolving "inherits": the count is per file, what that file itself carries). Hidden: it needs
// files outside the repository (a Bambu Studio checkout) and only reports.
TEST_CASE("Count the unknown keys dropped from a corpus of Bambu Studio files", "[.bambu_corpus]")
{
    const char *env = std::getenv("BAMBU_CORPUS");
    REQUIRE(env != nullptr);
    std::vector<std::string> roots;
    const std::string        list(env);
    boost::algorithm::split(roots, list, boost::is_any_of(";"));
    CorpusCount projects, json_presets;
    for (const std::string &root : roots) {
        if (root.empty())
            continue;
        std::vector<fs::path> files;
        if (fs::is_directory(root)) {
            for (fs::recursive_directory_iterator it(root), end; it != end; ++it)
                if (fs::is_regular_file(it->path()))
                    files.push_back(it->path());
        } else if (fs::is_regular_file(root)) {
            files.push_back(root);
        }
        std::sort(files.begin(), files.end());
        for (const fs::path &f : files) {
            const std::string ext = boost::algorithm::to_lower_copy(f.extension().string());
            if (ext == ".3mf")
                count_3mf(f, projects);
            else if (ext == ".json")
                count_json(f, json_presets);
        }
    }
    print("3mf", projects);
    print("json", json_presets);
    CHECK(projects.files + json_presets.files > 0);
}

// ------------------------------------------------------------------------------------------------
// The alias table (Format/BambuKeyAliases.cpp) and the import direction.
// ------------------------------------------------------------------------------------------------

namespace {

std::string legacy(std::string key, std::string value)
{
    PrintConfigDef::handle_legacy(key, value);
    return key + "=" + value;
}

std::string temp_path(const std::string &name)
{
    const fs::path dir = fs::temp_directory_path() / ("snorca_tests_bambu_aliases_" + std::to_string(get_current_pid()));
    fs::create_directories(dir);
    Slic3r::set_temporary_dir(dir.string());
    return (dir / name).string();
}

std::string data_path(const std::string &name) { return (fs::path(TEST_DATA_DIR) / "bambu_import_renames" / name).string(); }

bool load_json(const std::string &path, DynamicPrintConfig &cfg, ConfigSubstitutionContext &ctxt)
{
    std::map<std::string, std::string> key_values;
    std::string                        reason;
    return cfg.load_from_json(path, ctxt, false, key_values, reason) == 0;
}

// A full project config, as the application's full_config() is, with a value that is NOT the
// default for every aliased key, so a key that silently fails to come back is noticed.
DynamicPrintConfig aliased_project()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    for (const std::string &key : cfg.keys())
        if (const ConfigOption *opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    cfg.set_num_extruders(1);
    cfg.set_num_filaments(2);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.4 };
    cfg.option<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#00FF00" };
    cfg.set_deserialize_strict({
        { "infill_anchor", "3" },
        { "infill_anchor_max", "15" },
        { "chamber_temperature", "45,50" },
        { "bottom_solid_infill_flow_ratio", "0.95" },
        { "ironing_angle", "30" },
        { "only_one_wall_top", "1" },
        { "support_ironing", "1" },
        { "extruder_clearance_radius", "65" },
        { "dont_slow_down_outer_wall", "1,0" },
        { "role_based_wipe_speed", "0" },
        { "reduce_infill_retraction", "1" },
        { "wipe_tower_rib_width", "12" },
        { "wipe_tower_extra_rib_length", "3" },
        { "wipe_tower_fillet_wall", "0" },
        { "wipe_tower_wall_type", "rib" },
        { "wipe_tower_max_purge_speed", "120" },
        { "wipe_tower_wall_gap", "0" },
        { "lateral_lattice_angle_1", "-30" },
        { "lateral_lattice_angle_2", "40" },
        { "notes", "keep me; and me" },
        { "filament_colour_mode", "1,0" },
        { "filament_multi_colors", "\"#FF0000|#0000FF\";\"#00FF00\"" },
        { "sparse_infill_pattern", "lateral-lattice" },
        { "support_style", "organic" },
    });
    return cfg;
}

} // namespace

TEST_CASE("The Bambu alias table names real keys, once each", "[BambuAliases]")
{
    std::set<std::string> ours, bambu;
    for (const BambuKeyAliases::Alias &a : BambuKeyAliases::aliases()) {
        INFO("alias " << a.ours << " <-> " << a.bambu);
        CHECK(print_config_def.has(a.ours));
        // A Bambu name we also declare would make the loader rename one of our own keys.
        CHECK_FALSE(print_config_def.has(a.bambu));
        CHECK(BambuExport::find_key(a.bambu) != nullptr);
        CHECK(BambuExport::find_key(a.ours) == nullptr);
        CHECK(ours.insert(a.ours).second);
        CHECK(bambu.insert(a.bambu).second);
        CHECK(BambuExport::bambu_key_name(a.ours) == a.bambu);
    }
    for (const BambuKeyAliases::EnumAlias &e : BambuKeyAliases::enum_aliases()) {
        INFO("enum alias " << e.key << ": " << e.ours << " <-> " << e.bambu);
        const ConfigOptionDef *def = print_config_def.get(e.key);
        REQUIRE(def != nullptr);
        REQUIRE(def->enum_keys_map != nullptr);
        CHECK(def->enum_keys_map->count(e.ours) == 1);
        // For an export-only row (import=false), e.bambu is allowed to already be one of our own
        // enum keys: that is exactly how a many-of-ours-to-one-of-theirs fallback (e.g. seam
        // position Left/Right export as our own "aligned", which Bambu Studio also spells that
        // way) is written. import_enum_value() guards this on the way in - it returns immediately
        // when the incoming value is already a native key of ours (BambuKeyAliases.cpp), before it
        // ever consults this table - so such a row can never shadow a native import. A reversible
        // row (import=true) still must not collide: it is the one whose Bambu spelling has to
        // round-trip back through this table rather than the "already native" fast path.
        if (e.import)
            CHECK(def->enum_keys_map->count(e.bambu) == 0);
        CHECK(BambuExport::translate_enum_value(e.key, e.ours) == e.bambu);
    }
}

TEST_CASE("Bambu Studio's names load as our keys, with the value converted", "[BambuAliases][Config]")
{
    // Pure renames.
    CHECK(legacy("prime_tower_max_speed", "90") == "wipe_tower_max_purge_speed=90");
    CHECK(legacy("prime_tower_rib_width", "8") == "wipe_tower_rib_width=8");
    CHECK(legacy("prime_tower_extra_rib_length", "2") == "wipe_tower_extra_rib_length=2");
    CHECK(legacy("prime_tower_fillet_wall", "0") == "wipe_tower_fillet_wall=0");
    CHECK(legacy("prime_tower_skip_points", "0") == "wipe_tower_wall_gap=0");
    // Not a rename any more: extruder_clearance_max_radius is a setting of ours too (the timelapse
    // position picker reads it, as Bambu Studio's does). Loading a whole file still feeds
    // extruder_clearance_radius from it when the file does not set ours (BambuKeyAliases::load_fallbacks),
    // which the 3MF and machine-preset cases below pin.
    CHECK(legacy("extruder_clearance_max_radius", "73") == "extruder_clearance_max_radius=73");
    CHECK(legacy("enable_support_ironing", "1") == "support_ironing=1");
    CHECK(legacy("role_base_wipe_speed", "0") == "role_based_wipe_speed=0");
    CHECK(legacy("no_slow_down_for_cooling_on_outwalls", "1,0") == "dont_slow_down_outer_wall=1,0");
    CHECK(legacy("sparse_infill_lattice_angle_1", "-45") == "lateral_lattice_angle_1=-45");
    CHECK(legacy("process_notes", "hi") == "notes=hi");
    CHECK(legacy("sparse_infill_anchor", "400%") == "infill_anchor=400%");
    CHECK(legacy("chamber_temperatures", "0,0") == "chamber_temperature=0,0");
    CHECK(legacy("ironing_direction", "45") == "ironing_angle=45");
    // Value conversions.
    CHECK(legacy("prime_tower_rib_wall", "1") == "wipe_tower_wall_type=rib");
    CHECK(legacy("prime_tower_rib_wall", "0") == "wipe_tower_wall_type=rectangle");
    CHECK(legacy("reduce_infill_retraction_mode", "Enabled") == "reduce_infill_retraction=1");
    CHECK(legacy("reduce_infill_retraction_mode", "Disabled") == "reduce_infill_retraction=0");
    CHECK(legacy("reduce_infill_retraction_mode", "Auto") == "=Auto"); // no single bool: ours keeps its value
    CHECK(legacy("top_one_wall_type", "all top") == "only_one_wall_top=1");
    CHECK(legacy("top_one_wall_type", "not apply") == "only_one_wall_top=0");
    CHECK(legacy("filament_colour_type", "0,1") == "filament_colour_mode=1,0");
    CHECK(legacy("filament_colour_type", "\"0\";\"1\"") == "filament_colour_mode=1,0"); // model_settings.config shape
    CHECK(legacy("filament_multi_colour", "\"#FF0000 #0000FF\";\"#00FF00\"") == "filament_multi_colors=#FF0000|#0000FF;#00FF00");
    // A key-only pass (different_settings_to_system, the first look at a JSON array) only renames.
    CHECK(legacy("prime_tower_rib_wall", "") == "wipe_tower_wall_type=");
    CHECK(legacy("reduce_infill_retraction_mode", "") == "reduce_infill_retraction=");
    // Enum values Bambu spells differently for a key we share.
    CHECK(legacy("sparse_infill_pattern", "2dlattice") == "sparse_infill_pattern=lateral-lattice");
    CHECK(legacy("top_surface_pattern", "2dlattice") == "top_surface_pattern=lateral-lattice");
    CHECK(legacy("sparse_infill_pattern", "zig-zag") == "sparse_infill_pattern=rectilinear");
    CHECK(legacy("support_style", "tree_organic") == "support_style=organic");
    CHECK(legacy("ensure_vertical_shell_thickness", "partial") == "ensure_vertical_shell_thickness=ensure_moderate");
    // Our own keys and values pass through untouched.
    CHECK(legacy("wipe_tower_wall_type", "cone") == "wipe_tower_wall_type=cone");
    CHECK(legacy("reduce_infill_retraction", "1") == "reduce_infill_retraction=1");
    CHECK(legacy("sparse_infill_pattern", "gyroid") == "sparse_infill_pattern=gyroid");
    // The different_settings_to_system list is rewritten into our names.
    CHECK(legacy("different_settings_to_system", "prime_tower_rib_wall;sparse_infill_density") ==
          "different_settings_to_system=wipe_tower_wall_type;sparse_infill_density");
}

// Every enum value Bambu Studio can write for an enum key we share either deserializes here or is
// translated on load. The exceptions have no equivalent and fall back through the normal
// substitution (the user is told): they are listed so a new one is noticed.
TEST_CASE("Every Bambu enum value of a shared key loads", "[BambuAliases][Config]")
{
    const std::set<std::pair<std::string, std::string>> no_equivalent = {
        { "fuzzy_skin", "disabled_fuzzy" },    // "off, ignore paint": ours paints with fuzzy_skin=none
        { "nozzle_type", "tungsten_carbide" }, // no such nozzle type here
    };
    // Keys handle_legacy's ignore set retires whatever their value (see hubtest notes on extruder_type).
    const std::set<std::string> retired = { "extruder_type" };
    for (const BambuExport::BambuKeyDef &bdef : BambuExport::bambu_key_defs()) {
        if (bdef.enum_values == nullptr)
            continue;
        const ConfigOptionDef *ours = print_config_def.get(bdef.key);
        if (ours == nullptr || ours->enum_keys_map == nullptr || (ours->type != coEnum && ours->type != coEnums) || retired.count(bdef.key))
            continue;
        std::vector<std::string> values;
        const std::string        joined(bdef.enum_values);
        boost::algorithm::split(values, joined, boost::is_any_of("|"));
        for (const std::string &v : values) {
            if (no_equivalent.count({ bdef.key, v }))
                continue;
            std::string key = bdef.key, value = v;
            PrintConfigDef::handle_legacy(key, value);
            INFO("Bambu " << bdef.key << " = " << v << " loads as " << key << " = " << value);
            CHECK(key == bdef.key);
            CHECK(ours->enum_keys_map->count(value) == 1);
        }
    }
}

TEST_CASE("Our config survives Export Bambu 3MF and our own import, key by key", "[BambuAliases][3mf]")
{
    const DynamicPrintConfig cfg = aliased_project();
    BambuExport::Context     ctx = BambuExport::Context::from_project(cfg);
    BambuExport::Report      report;
    const BambuExport::Config exported = BambuExport::convert_project(cfg, ctx, report);
    const std::string         path     = temp_path("aliased_project_settings.config");
    {
        boost::nowide::ofstream f(path, std::ios::binary);
        f << BambuExport::to_json(exported, "project_settings", "project", BambuExport::export_version());
    }
    DynamicPrintConfig        back;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    REQUIRE(load_json(path, back, ctxt));

    for (const BambuKeyAliases::Alias &a : BambuKeyAliases::aliases()) {
        INFO("our " << a.ours << " went out as Bambu's " << a.bambu);
        CHECK(exported.count(a.bambu) == 1);
        CHECK(exported.count(a.ours) == 0);
        REQUIRE(back.has(a.ours));
        CHECK(back.opt_serialize(a.ours) == cfg.opt_serialize(a.ours));
        CHECK(std::find(ctxt.unrecogized_keys.begin(), ctxt.unrecogized_keys.end(), a.bambu) == ctxt.unrecogized_keys.end());
    }
    CHECK(exported.at("sparse_infill_pattern").values.front() == "2dlattice");
    CHECK(back.opt_serialize("sparse_infill_pattern") == "lateral-lattice");
    CHECK(exported.at("support_style").values.front() == "tree_organic");
    CHECK(back.opt_serialize("support_style") == "organic");
    fs::remove(path);
}

TEST_CASE("What Export Bambu 3MF cannot say comes back as a documented approximation", "[BambuAliases][3mf]")
{
    auto round_trip = [](const std::string &key, const std::string &value) {
        DynamicPrintConfig cfg = aliased_project();
        cfg.set_deserialize_strict(key, value);
        BambuExport::Context      ctx = BambuExport::Context::from_project(cfg);
        BambuExport::Report       report;
        const BambuExport::Config exported = BambuExport::convert_project(cfg, ctx, report);
        const std::string         bkey     = BambuExport::bambu_key_name(key);
        const auto                it       = exported.find(bkey);
        if (it == exported.end())
            return std::string("<left out>");
        std::string k = bkey, v = BambuExport::serialize(it->second);
        PrintConfigDef::handle_legacy(k, v);
        return k + "=" + v;
    };
    CHECK(round_trip("wipe_tower_wall_type", "cone") == "wipe_tower_wall_type=rectangle"); // Bambu has no cone tower
    CHECK(round_trip("ironing_angle", "-1") == "<left out>");                               // ours: -1 = default method
    CHECK(round_trip("ensure_vertical_shell_thickness", "ensure_critical_only") == "ensure_vertical_shell_thickness=ensure_moderate");
}

TEST_CASE("A file that sets both spellings keeps ours", "[BambuAliases][Config]")
{
    // std::map-ordered JSON: "top_one_wall_type" and "process_notes" sort AFTER our spelling, so
    // before the shadow rule the Bambu value was applied last and won.
    const std::string path = temp_path("both_spellings.json");
    {
        boost::nowide::ofstream f(path, std::ios::binary);
        f << "{ \"name\": \"both\", \"only_one_wall_top\": \"0\", \"top_one_wall_type\": \"all top\","
             " \"notes\": \"ours\", \"process_notes\": \"bambu\","
             " \"wipe_tower_max_purge_speed\": \"120\", \"prime_tower_max_speed\": \"50\","
             " \"prime_tower_rib_wall\": \"1\" }";
    }
    DynamicPrintConfig        cfg;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    REQUIRE(load_json(path, cfg, ctxt));
    CHECK(cfg.opt_serialize("only_one_wall_top") == "0");
    CHECK(cfg.opt_serialize("notes") == "ours");
    CHECK(cfg.opt_serialize("wipe_tower_max_purge_speed") == "120");
    // Alone, the Bambu spelling still maps.
    CHECK(cfg.opt_serialize("wipe_tower_wall_type") == "rib");
    fs::remove(path);

    // Per-object / per-part settings: whichever order model_settings.config lists them in.
    for (const bool ours_first : { true, false }) {
        ModelConfig mc;
        ConfigSubstitutionContext mctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
        if (ours_first) {
            mc.set_deserialize("only_one_wall_top", "0", mctxt);
            mc.set_deserialize("top_one_wall_type", "all top", mctxt);
        } else {
            mc.set_deserialize("top_one_wall_type", "all top", mctxt);
            mc.set_deserialize("only_one_wall_top", "0", mctxt);
        }
        INFO("ours first: " << ours_first);
        CHECK(mc.get().opt_serialize("only_one_wall_top") == "0");
        ModelConfig alone;
        alone.set_deserialize("top_one_wall_type", "all top", mctxt);
        CHECK(alone.get().opt_serialize("only_one_wall_top") == "1");
    }
}

TEST_CASE("A genuine Bambu Studio project keeps its prime tower and other renamed settings", "[BambuAliases][3mf]")
{
    // Bambu Studio's own pressure-advance calibration project (resources/calib, BambuStudio-02.00.02.01).
    DynamicPrintConfig        cfg;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    Model                     model;
    PlateDataPtrs             plates;
    std::vector<Preset *>     presets;
    bool                      is_bbl = false;
    Semver                    version;
    REQUIRE(load_bbs_3mf(data_path("auto_pa_line_single.3mf").c_str(), &cfg, &ctxt, &model, &plates, &presets, &is_bbl, &version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
    release_PlateData_list(plates);
    for (Preset *p : presets)
        delete p;
    CHECK(is_bbl);
    // project_settings.config: prime_tower_rib_wall 1, prime_tower_rib_width 8, prime_tower_fillet_wall 1,
    // prime_tower_max_speed 90, extruder_clearance_max_radius 73, role_base_wipe_speed 1,
    // top_one_wall_type "all top", ironing_direction 45, sparse_infill_anchor 400%.
    CHECK(cfg.opt_serialize("wipe_tower_wall_type") == "rib");
    CHECK(cfg.opt_float("wipe_tower_rib_width") == Approx(8.));
    CHECK(cfg.opt_bool("wipe_tower_fillet_wall"));
    CHECK(cfg.opt_float("wipe_tower_max_purge_speed") == Approx(90.));
    CHECK(cfg.opt_float("extruder_clearance_radius") == Approx(73.)); // our default is 40
    CHECK(cfg.opt_float("extruder_clearance_max_radius") == Approx(73.)); // Bambu's own key, loaded as itself
    CHECK(cfg.opt_bool("role_based_wipe_speed"));
    CHECK(cfg.opt_bool("only_one_wall_top"));
    CHECK(cfg.opt_float("ironing_angle") == Approx(45.));             // our default is -1
    CHECK(cfg.opt_serialize("infill_anchor") == "400%");
    for (const char *bambu : { "prime_tower_rib_wall", "prime_tower_rib_width", "prime_tower_fillet_wall", "prime_tower_max_speed",
                               "prime_tower_extra_rib_length", "extruder_clearance_max_radius", "role_base_wipe_speed",
                               "top_one_wall_type", "ironing_direction", "process_notes" }) {
        INFO("dropped as unknown: " << bambu);
        CHECK(std::find(ctxt.unrecogized_keys.begin(), ctxt.unrecogized_keys.end(), bambu) == ctxt.unrecogized_keys.end());
    }
}

TEST_CASE("Bambu Studio presets map their renamed keys", "[BambuAliases][Config]")
{
    SECTION("a Bambu user process preset (the owner's, from tests/data/bambu_compat)") {
        DynamicPrintConfig        cfg;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
        REQUIRE(load_json((fs::path(TEST_DATA_DIR) / "bambu_compat" / "0.12mm_@H2C_HiQ_-_Pokeballs.json").string(), cfg, ctxt));
        // enable_support_ironing 1, reduce_infill_retraction_mode Disabled, prime_tower_fillet_wall 0,
        // prime_tower_rib_wall 0, prime_tower_max_speed 200, top_one_wall_type "not apply".
        CHECK(cfg.opt_bool("support_ironing"));
        CHECK(cfg.opt_serialize("reduce_infill_retraction") == "0");
        CHECK(cfg.opt_serialize("wipe_tower_fillet_wall") == "0");
        CHECK(cfg.opt_serialize("wipe_tower_wall_type") == "rectangle");
        CHECK(cfg.opt_float("wipe_tower_max_purge_speed") == Approx(200.));
        CHECK(cfg.opt_serialize("only_one_wall_top") == "0");
    }
    SECTION("Bambu's system process base preset") {
        DynamicPrintConfig        cfg;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
        REQUIRE(load_json(data_path("fdm_process_common.json"), cfg, ctxt));
        // enable_support_ironing 0, prime_tower_max_speed 90, sparse_infill_lattice_angle_1/2 -45/45,
        // reduce_infill_retraction_mode "Auto" (no single bool: not loaded).
        CHECK(cfg.has("support_ironing"));
        CHECK(cfg.opt_float("wipe_tower_max_purge_speed") == Approx(90.));
        CHECK(cfg.opt_float("lateral_lattice_angle_1") == Approx(-45.));
        CHECK(cfg.opt_float("lateral_lattice_angle_2") == Approx(45.));
        CHECK_FALSE(cfg.has("reduce_infill_retraction"));
    }
    SECTION("a Bambu machine preset that still carries the legacy extruder_clearance_radius keeps it") {
        // Bambu Lab H2D 0.4 nozzle.json: extruder_clearance_radius 49 (a leftover Bambu Studio itself
        // ignores) and extruder_clearance_max_radius 96. Ours wins when both are present, which is what
        // our own BBL profiles (the same two keys) have always loaded as.
        DynamicPrintConfig        cfg;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
        REQUIRE(load_json(data_path("Bambu Lab H2D 0.4 nozzle.json"), cfg, ctxt));
        CHECK(cfg.opt_float("extruder_clearance_radius") == Approx(49.));
        CHECK(cfg.opt_float("extruder_clearance_max_radius") == Approx(96.));
    }
}
