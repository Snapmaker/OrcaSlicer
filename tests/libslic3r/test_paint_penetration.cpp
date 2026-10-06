// Top / bottom paint penetration layers (top_color_penetration_layers / bottom_color_penetration_layers,
// Bambu Studio's keys). 0 (the default) keeps the shell-derived paint depth of
// MultiMaterialSegmentation.cpp; a value >= 1 sets the depth of a painted top / bottom in layers,
// surface layer included, whatever the shell settings and the paint-depth mode say.
#include <catch2/catch.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/miniz_extension.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <regex>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// make_cube() facets: {2,3} = top cap (z = height), {0,1} = bottom cap (z = 0), {4,5} = +X side.
const std::vector<int> TOP_CAP    = {2, 3};
const std::vector<int> BOTTOM_CAP = {0, 1};
const std::vector<int> PLUS_X     = {4, 5};

struct SlabSetup
{
    PaintDepthMode mode                   = pdmUnlimited;
    double         layer_height           = 0.1;
    int            top_shell_layers       = 4;
    double         top_shell_thickness    = 0.;
    int            bottom_shell_layers    = 3;
    double         bottom_shell_thickness = 0.;
    int            top_penetration        = 0;
    int            bottom_penetration     = 0;
    // Per-object override of top_color_penetration_layers (-1 = none).
    int            object_top_penetration = -1;
};

// Two filaments, flows pinned the way tests/libslic3r/test_paint_depth_clamp.cpp pins them (a bare
// outer_wall_line_width of 0 makes the segmentation's Flow computation throw).
DynamicPrintConfig slab_config(const SlabSetup &s)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FFFFFF", "#804020"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->percent = false;
    config.option<ConfigOptionEnum<PaintDepthMode>>("paint_depth_mode")->value = s.mode;
    config.option<ConfigOptionInt>("paint_depth_walls")->value                  = 3;
    config.option<ConfigOptionFloat>("layer_height")->value                     = s.layer_height;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value       = s.layer_height;
    config.option<ConfigOptionInt>("top_shell_layers")->value                   = s.top_shell_layers;
    config.option<ConfigOptionFloat>("top_shell_thickness")->value              = s.top_shell_thickness;
    config.option<ConfigOptionInt>("bottom_shell_layers")->value                = s.bottom_shell_layers;
    config.option<ConfigOptionFloat>("bottom_shell_thickness")->value           = s.bottom_shell_thickness;
    config.option<ConfigOptionInt>("top_color_penetration_layers")->value       = s.top_penetration;
    config.option<ConfigOptionInt>("bottom_color_penetration_layers")->value    = s.bottom_penetration;
    return config;
}

// A 40 x 40 x 4 mm slab with the given facets painted Extruder2, applied to `print` (not sliced).
void apply_painted_slab(const std::vector<int> &painted, const SlabSetup &s, Model &model, Print &print)
{
    ModelObject *object = model.add_object();
    object->name        = "paint-penetration.stl";
    ModelVolume *volume = object->add_volume(make_cube(40., 40., 4.));
    object->add_instance();
    object->ensure_on_bed();
    if (s.object_top_penetration >= 0)
        object->config.set_key_value("top_color_penetration_layers", new ConfigOptionInt(s.object_top_penetration));

    TriangleSelector selector(volume->mesh());
    for (int facet_idx : painted)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    print.set_status_silent();
    print.apply(model, slab_config(s));
    REQUIRE(print.objects().size() == 1);
}

PrintObject *slice_painted_slab(const std::vector<int> &painted, const SlabSetup &s, Model &model, Print &print)
{
    apply_painted_slab(painted, s, model, print);
    PrintObject *object = print.objects_mutable().front();
    object->slice();
    REQUIRE(object->layer_count() > 0);
    return object;
}

// The area apply_mm_segmentation gave the Extruder2 paint on one layer.
ExPolygons extruder2_claim(const PrintObject &object, size_t layer_idx)
{
    ExPolygons   result;
    const Layer *layer = object.get_layer(int(layer_idx));
    for (size_t region_idx = 0; region_idx < object.num_printing_regions(); ++region_idx) {
        const PrintRegion &region = object.printing_region(region_idx);
        if (region.config().wall_filament.value != 2)
            continue;
        const int local_id = region.print_object_region_id();
        if (local_id < 0 || local_id >= layer->region_count())
            continue;
        for (const Surface &s : layer->get_region(local_id)->slices.surfaces)
            result.emplace_back(s.expolygon);
    }
    return result;
}

bool claimed_at_centre(const PrintObject &object, size_t layer_idx)
{
    const BoundingBox bb = get_extents(object.get_layer(0)->lslices);
    const Point       centre((bb.min.x() + bb.max.x()) / 2, (bb.min.y() + bb.max.y()) / 2);
    for (const ExPolygon &p : extruder2_claim(object, layer_idx))
        if (p.contains(centre))
            return true;
    return false;
}

// How many layers deep, counted from the painted surface layer (included), the centre of a painted
// top (or bottom) is claimed without a gap.
int top_depth(const PrintObject &object)
{
    int depth = 0;
    for (int idx = int(object.layer_count()) - 1; idx >= 0 && claimed_at_centre(object, size_t(idx)); --idx)
        ++depth;
    return depth;
}

int bottom_depth(const PrintObject &object)
{
    int depth = 0;
    for (size_t idx = 0; idx < object.layer_count() && claimed_at_centre(object, idx); ++idx)
        ++depth;
    return depth;
}

// Every region's surfaces on every layer, for exact comparisons between two slices.
std::vector<std::vector<ExPolygons>> segmentation_of(const PrintObject &object)
{
    std::vector<std::vector<ExPolygons>> out(object.layer_count());
    for (size_t l = 0; l < object.layer_count(); ++l)
        for (const LayerRegion *layerm : object.get_layer(int(l))->regions()) {
            ExPolygons polys;
            for (const Surface &s : layerm->slices.surfaces)
                polys.emplace_back(s.expolygon);
            out[l].emplace_back(std::move(polys));
        }
    return out;
}

// The G-code without its config block (which names the settings) and the generated-at timestamp.
std::string without_config_block(const std::string &gcode)
{
    std::string  out   = gcode;
    const size_t begin = out.find("; CONFIG_BLOCK_START");
    const size_t end   = out.find("; CONFIG_BLOCK_END");
    if (begin != std::string::npos && end != std::string::npos)
        out = out.substr(0, begin) + out.substr(end);
    static const std::regex generated("; generated by [^\\n]*\\n");
    return std::regex_replace(out, generated, "");
}

// A project config with every option present, the way the application's full_config() is (enum
// vectors rebuilt from print_config_def so the project writer can serialize them - see
// tests/libslic3r/test_bambu_3mf_export.cpp's project_config()).
DynamicPrintConfig project_config()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    for (const std::string &key : cfg.keys())
        if (const ConfigOption *opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    cfg.set_num_extruders(1);
    cfg.set_num_filaments(1);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.4 };
    cfg.set_key_value("printer_model", new ConfigOptionString("Bambu Lab P1S"));
    return cfg;
}

std::string bambu_data(const std::string &name) { return (fs::path(TEST_DATA_DIR) / "bambu_import_renames" / name).string(); }

std::string temp_path(const std::string &name)
{
    const fs::path dir = fs::temp_directory_path() / ("snorca_tests_paintpen_" + std::to_string(get_current_pid()));
    fs::create_directories(dir);
    Slic3r::set_temporary_dir(dir.string());
    return (dir / name).string();
}

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

std::string exported(const BambuExport::Config &cfg, const std::string &key)
{
    const auto it = cfg.find(key);
    return it == cfg.end() ? std::string("<absent>") : BambuExport::serialize(it->second);
}

} // namespace

// --- the settings ---------------------------------------------------------------------------------

TEST_CASE("Paint penetration settings: Bambu's names, default 0 (follow the shell), print preset options", "[paintpen][Config]")
{
    for (const char *key : { "top_color_penetration_layers", "bottom_color_penetration_layers" }) {
        INFO(key);
        const ConfigOptionDef *def = print_config_def.get(key);
        REQUIRE(def != nullptr);
        CHECK(def->type == coInt);
        CHECK(def->min == 0);
        CHECK(def->get_default_value<ConfigOptionInt>()->value == 0);
        // A per-region setting (the segmentation maxes it over the regions of a layer), and part of the
        // print preset, so it is saved, compared and shown with the process settings.
        CHECK(PrintRegionConfig().has(key));
        const auto &options = Preset::print_options();
        CHECK(std::find(options.begin(), options.end(), key) != options.end());
        // Bambu Studio has the same key with the same type, so import and export are 1:1.
        const BambuExport::BambuKeyDef *bdef = BambuExport::find_key(key);
        REQUIRE(bdef != nullptr);
        CHECK(std::string(bdef->type) == "coInt");
    }
}

TEST_CASE("Paint penetration settings round-trip through a config, a preset file and a project 3MF", "[paintpen][Config]")
{
    DynamicPrintConfig cfg = project_config();
    cfg.set_deserialize_strict("top_color_penetration_layers", "7");
    cfg.set_deserialize_strict("bottom_color_penetration_layers", "2");
    CHECK(cfg.opt_serialize("top_color_penetration_layers") == "7");
    CHECK(cfg.opt_int("bottom_color_penetration_layers") == 2);

    SECTION("preset JSON") {
        const std::string path = temp_path("paintpen_preset.json");
        cfg.save_to_json(path, "paintpen", "User", "02.00.00.00");
        DynamicPrintConfig                 back;
        ConfigSubstitutionContext          ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
        std::map<std::string, std::string> key_values;
        std::string                        reason;
        REQUIRE(back.load_from_json(path, ctxt, true, key_values, reason) == 0);
        CHECK(back.opt_int("top_color_penetration_layers") == 7);
        CHECK(back.opt_int("bottom_color_penetration_layers") == 2);
        fs::remove(path);
    }

    SECTION("project 3MF, with a per-object override") {
        Model        model;
        ModelObject *object = model.add_object();
        object->name        = "cube";
        object->add_volume(make_cube(10., 10., 10.));
        object->add_instance();
        object->config.set_key_value("top_color_penetration_layers", new ConfigOptionInt(5));
        const std::string path = temp_path("paintpen_project.3mf");
        StoreParams       sp;
        sp.path     = path.c_str();
        sp.model    = &model;
        sp.config   = &cfg;
        sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
        REQUIRE(store_bbs_3mf(sp));

        DynamicPrintConfig        back;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
        Model                     back_model;
        PlateDataPtrs             plates;
        std::vector<Preset *>     presets;
        bool                      is_bbl = false;
        Semver                    version;
        REQUIRE(load_bbs_3mf(path.c_str(), &back, &ctxt, &back_model, &plates, &presets, &is_bbl, &version, nullptr,
                             LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
        release_PlateData_list(plates);
        for (Preset *p : presets)
            delete p;
        CHECK(back.opt_int("top_color_penetration_layers") == 7);
        CHECK(back.opt_int("bottom_color_penetration_layers") == 2);
        REQUIRE(back_model.objects.size() == 1);
        CHECK(back_model.objects[0]->config.get().opt_int("top_color_penetration_layers") == 5);
        fs::remove(path);
    }
}

// --- the segmentation -----------------------------------------------------------------------------

TEST_CASE("Paint penetration 0 follows the shell exactly: same segmentation and G-code as the equivalent explicit depth", "[paintpen]")
{
    // Unlimited paint depth, no shell thickness: the shell-derived depth is exactly the layer count,
    // so penetration 0 must reproduce penetration = shell layers to the last point - segmentation and
    // G-code (the config block, which names the two settings, aside).
    SlabSetup follow;
    follow.layer_height        = 0.2;
    follow.top_shell_layers    = 4;
    follow.bottom_shell_layers = 3;
    SlabSetup explicit_depth   = follow;
    explicit_depth.top_penetration    = 4;
    explicit_depth.bottom_penetration = 3;

    // Top, bottom and one side. (Unlimited mode lets the painted side claim the whole interior of
    // every layer, so the depths are pinned by the tests below, not here.)
    const std::vector<int> painted = { 0, 1, 2, 3, 4, 5 };
    Model a_model, b_model;
    Print a, b;
    apply_painted_slab(painted, follow, a_model, a);
    apply_painted_slab(painted, explicit_depth, b_model, b);
    const std::string a_gcode = Slic3r::Test::gcode(a);
    const std::string b_gcode = Slic3r::Test::gcode(b);
    CHECK(segmentation_of(*a.objects().front()) == segmentation_of(*b.objects().front()));
    REQUIRE(! a_gcode.empty());
    CHECK(a_gcode.find("; top_color_penetration_layers = 0") != std::string::npos);
    CHECK(without_config_block(a_gcode) == without_config_block(b_gcode));
}

TEST_CASE("Top paint penetration sets the painted depth of a top in layers, whatever the shell", "[paintpen]")
{
    for (const PaintDepthMode mode : { pdmUnlimited, pdmWalls }) {
        DYNAMIC_SECTION("paint depth mode " << int(mode)) {
            SECTION("penetration 2 with 5 top shell layers: exactly 2 layers") {
                SlabSetup s;
                s.mode             = mode;
                s.top_shell_layers = 5;
                s.top_penetration  = 2;
                Model model; Print print;
                const PrintObject *object = slice_painted_slab(TOP_CAP, s, model, print);
                CHECK(top_depth(*object) == 2);
            }
            SECTION("penetration 6 with 3 top shell layers: exactly 6 layers") {
                SlabSetup s;
                s.mode             = mode;
                s.top_shell_layers = 3;
                s.top_penetration  = 6;
                Model model; Print print;
                const PrintObject *object = slice_painted_slab(TOP_CAP, s, model, print);
                CHECK(top_depth(*object) == 6);
            }
            SECTION("penetration 0: the shell depth, unchanged (5 layers, the thickness bound 0.6 mm = 6 layers deeper)") {
                SlabSetup s;
                s.mode                = mode;
                s.top_shell_layers    = 5;
                s.top_shell_thickness = 0.6;
                Model model; Print print;
                const PrintObject *object = slice_painted_slab(TOP_CAP, s, model, print);
                CHECK(top_depth(*object) == 6);
            }
            SECTION("penetration ignores the shell thickness too (2 layers although 0.6 mm needs 6)") {
                SlabSetup s;
                s.mode                = mode;
                s.top_shell_layers    = 5;
                s.top_shell_thickness = 0.6;
                s.top_penetration     = 2;
                Model model; Print print;
                const PrintObject *object = slice_painted_slab(TOP_CAP, s, model, print);
                CHECK(top_depth(*object) == 2);
            }
        }
    }
}

TEST_CASE("Bottom paint penetration sets the painted depth of a bottom in layers, whatever the shell", "[paintpen]")
{
    for (const PaintDepthMode mode : { pdmUnlimited, pdmWalls }) {
        DYNAMIC_SECTION("paint depth mode " << int(mode)) {
            SECTION("penetration 2 with 5 bottom shell layers: exactly 2 layers") {
                SlabSetup s;
                s.mode                = mode;
                s.bottom_shell_layers = 5;
                s.bottom_penetration  = 2;
                Model model; Print print;
                const PrintObject *object = slice_painted_slab(BOTTOM_CAP, s, model, print);
                CHECK(bottom_depth(*object) == 2);
            }
            SECTION("penetration 6 with 3 bottom shell layers: exactly 6 layers") {
                SlabSetup s;
                s.mode                = mode;
                s.bottom_shell_layers = 3;
                s.bottom_penetration  = 6;
                Model model; Print print;
                const PrintObject *object = slice_painted_slab(BOTTOM_CAP, s, model, print);
                CHECK(bottom_depth(*object) == 6);
            }
            SECTION("penetration 0: the shell depth, unchanged") {
                SlabSetup s;
                s.mode                = mode;
                s.bottom_shell_layers = 3;
                Model model; Print print;
                const PrintObject *object = slice_painted_slab(BOTTOM_CAP, s, model, print);
                CHECK(bottom_depth(*object) == 3);
            }
        }
    }
}

TEST_CASE("Top and bottom paint penetration are independent", "[paintpen]")
{
    SlabSetup s;
    s.top_shell_layers    = 4;
    s.bottom_shell_layers = 4;
    s.top_penetration     = 7;
    Model model; Print print;
    std::vector<int> painted = TOP_CAP;
    painted.insert(painted.end(), BOTTOM_CAP.begin(), BOTTOM_CAP.end());
    const PrintObject *object = slice_painted_slab(painted, s, model, print);
    CHECK(top_depth(*object) == 7);
    CHECK(bottom_depth(*object) == 4);
}

TEST_CASE("A per-object paint penetration overrides the print setting", "[paintpen]")
{
    SlabSetup s;
    s.top_shell_layers       = 5;
    s.object_top_penetration = 2;
    Model model; Print print;
    const PrintObject *object = slice_painted_slab(TOP_CAP, s, model, print);
    CHECK(top_depth(*object) == 2);
}

TEST_CASE("Paint penetration claims a painted top even with zero top shell layers (as Bambu Studio does)", "[paintpen]")
{
    SlabSetup s;
    s.top_shell_layers = 0;
    s.top_penetration  = 3;
    Model model; Print print;
    const PrintObject *object = slice_painted_slab(TOP_CAP, s, model, print);
    CHECK(top_depth(*object) == 3);

    // ... while 0 keeps the "no shell, no claim" rule.
    SlabSetup follow;
    follow.top_shell_layers = 0;
    Model model0; Print print0;
    const PrintObject *object0 = slice_painted_slab(TOP_CAP, follow, model0, print0);
    CHECK(top_depth(*object0) == 0);
}

// --- Bambu Studio import --------------------------------------------------------------------------

TEST_CASE("A genuine Bambu Studio project and preset import their paint penetration", "[paintpen][3mf]")
{
    SECTION("project (auto_pa_line_single.3mf: top 5, bottom 3)") {
        DynamicPrintConfig        cfg;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
        Model                     model;
        PlateDataPtrs             plates;
        std::vector<Preset *>     presets;
        bool                      is_bbl = false;
        Semver                    version;
        REQUIRE(load_bbs_3mf(bambu_data("auto_pa_line_single.3mf").c_str(), &cfg, &ctxt, &model, &plates, &presets, &is_bbl, &version, nullptr,
                             LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
        release_PlateData_list(plates);
        for (Preset *p : presets)
            delete p;
        CHECK(is_bbl);
        CHECK(cfg.opt_int("top_color_penetration_layers") == 5);
        CHECK(cfg.opt_int("bottom_color_penetration_layers") == 3);
        for (const char *key : { "top_color_penetration_layers", "bottom_color_penetration_layers" }) {
            INFO("dropped as unknown: " << key);
            CHECK(std::find(ctxt.unrecogized_keys.begin(), ctxt.unrecogized_keys.end(), key) == ctxt.unrecogized_keys.end());
        }
    }
    SECTION("Bambu's system process base preset (fdm_process_common.json: top 3, bottom 3)") {
        DynamicPrintConfig                 cfg;
        ConfigSubstitutionContext          ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
        std::map<std::string, std::string> key_values;
        std::string                        reason;
        REQUIRE(cfg.load_from_json(bambu_data("fdm_process_common.json"), ctxt, false, key_values, reason) == 0);
        CHECK(cfg.opt_int("top_color_penetration_layers") == 3);
        CHECK(cfg.opt_int("bottom_color_penetration_layers") == 3);
    }
}

// --- Export Bambu 3MF -----------------------------------------------------------------------------

TEST_CASE("Export Bambu 3MF writes the effective paint penetration where ours follows the shell", "[paintpen][BambuExport]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_filaments(2);
    cfg.set_deserialize_strict("layer_height", "0.1");
    cfg.set_deserialize_strict("top_shell_layers", "4");
    cfg.set_deserialize_strict("top_shell_thickness", "0.6");   // 6 layers of 0.1 mm: deeper than 4
    cfg.set_deserialize_strict("bottom_shell_layers", "3");
    cfg.set_deserialize_strict("bottom_shell_thickness", "0");

    auto convert = [](const DynamicPrintConfig &project, BambuExport::Report &report) {
        BambuExport::Context ctx = BambuExport::Context::from_project(project);
        return BambuExport::convert_project(project, ctx, report);
    };

    SECTION("0 goes out as the depth the shell settings give") {
        BambuExport::Report report;
        const BambuExport::Config out = convert(cfg, report);
        CHECK(exported(out, "top_color_penetration_layers") == "6");
        CHECK(exported(out, "bottom_color_penetration_layers") == "3");
        CHECK(report.converted.count("top_color_penetration_layers"));
    }
    SECTION("the layer count wins when it is deeper than the thickness") {
        cfg.set_deserialize_strict("layer_height", "0.2");
        BambuExport::Report report;
        CHECK(exported(convert(cfg, report), "top_color_penetration_layers") == "4");
    }
    SECTION("a zero shell goes out as Bambu's minimum, 1") {
        cfg.set_deserialize_strict("top_shell_layers", "0");
        BambuExport::Report report;
        CHECK(exported(convert(cfg, report), "top_color_penetration_layers") == "1");
    }
    SECTION("an explicit depth goes out as it is") {
        cfg.set_deserialize_strict("top_color_penetration_layers", "2");
        cfg.set_deserialize_strict("bottom_color_penetration_layers", "7");
        BambuExport::Report report;
        const BambuExport::Config out = convert(cfg, report);
        CHECK(exported(out, "top_color_penetration_layers") == "2");
        CHECK(exported(out, "bottom_color_penetration_layers") == "7");
        CHECK(report.converted.count("top_color_penetration_layers") == 0);
    }
    SECTION("per-object: an object with its own shells gets its own depth, an explicit one stays") {
        BambuExport::Report  report;
        BambuExport::Context ctx = BambuExport::Context::from_project(cfg);
        BambuExport::convert_project(cfg, ctx, report);

        DynamicPrintConfig shells;
        shells.set_deserialize_strict("top_shell_layers", "9");
        const BambuExport::Config o1 = BambuExport::convert(shells, ctx, BambuExport::Scope::Object, report, "object 1");
        CHECK(exported(o1, "top_color_penetration_layers") == "9");
        CHECK(exported(o1, "bottom_color_penetration_layers") == "<absent>"); // bottom shell not overridden

        DynamicPrintConfig pen;
        pen.set_deserialize_strict("top_color_penetration_layers", "3");
        pen.set_deserialize_strict("top_shell_layers", "9");
        const BambuExport::Config o2 = BambuExport::convert(pen, ctx, BambuExport::Scope::Object, report, "object 2");
        CHECK(exported(o2, "top_color_penetration_layers") == "3");

        DynamicPrintConfig unrelated;
        unrelated.set_deserialize_strict("wall_loops", "3");
        const BambuExport::Config o3 = BambuExport::convert(unrelated, ctx, BambuExport::Scope::Object, report, "object 3");
        CHECK(exported(o3, "top_color_penetration_layers") == "<absent>");
    }
    SECTION("per-object shells under an explicit project depth inherit it, as in Bambu Studio") {
        cfg.set_deserialize_strict("top_color_penetration_layers", "5");
        BambuExport::Report  report;
        BambuExport::Context ctx = BambuExport::Context::from_project(cfg);
        BambuExport::convert_project(cfg, ctx, report);
        DynamicPrintConfig shells;
        shells.set_deserialize_strict("top_shell_layers", "9");
        CHECK(exported(BambuExport::convert(shells, ctx, BambuExport::Scope::Object, report, "object"), "top_color_penetration_layers") == "<absent>");
    }
}

TEST_CASE("An Export Bambu 3MF project carries the paint penetration into Bambu's files and back", "[paintpen][BambuExport][3mf]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "cube";
    object->add_volume(make_cube(10., 10., 10.));
    object->add_instance();
    object->config.set_key_value("top_shell_layers", new ConfigOptionInt(8));

    DynamicPrintConfig cfg = project_config();
    cfg.set_deserialize_strict("layer_height", "0.2");
    cfg.set_deserialize_strict("top_shell_layers", "5");
    cfg.set_deserialize_strict("top_shell_thickness", "0");
    cfg.set_deserialize_strict("bottom_color_penetration_layers", "2");

    const std::string   path = temp_path("paintpen_bambu_export.3mf");
    BambuExport::Report report;
    StoreParams         sp;
    sp.path         = path.c_str();
    sp.model        = &model;
    sp.config       = &cfg;
    sp.strategy     = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    sp.bambu_compat = true;
    sp.bambu_report = &report;
    REQUIRE(store_bbs_3mf(sp));

    const nlohmann::json project = nlohmann::json::parse(zip_entry(path, "Metadata/project_settings.config"));
    CHECK(project["top_color_penetration_layers"] == "5");
    CHECK(project["bottom_color_penetration_layers"] == "2");
    const std::string model_settings = zip_entry(path, "Metadata/model_settings.config");
    CHECK(model_settings.find("key=\"top_color_penetration_layers\" value=\"8\"") != std::string::npos);

    // Loaded back (as a Bambu Studio project), the depths are explicit now - what Bambu Studio sees.
    DynamicPrintConfig        back;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    Model                     back_model;
    PlateDataPtrs             plates;
    std::vector<Preset *>     presets;
    bool                      is_bbl = false;
    Semver                    version;
    REQUIRE(load_bbs_3mf(path.c_str(), &back, &ctxt, &back_model, &plates, &presets, &is_bbl, &version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
    release_PlateData_list(plates);
    for (Preset *p : presets)
        delete p;
    CHECK(back.opt_int("top_color_penetration_layers") == 5);
    CHECK(back.opt_int("bottom_color_penetration_layers") == 2);
    REQUIRE(back_model.objects.size() == 1);
    CHECK(back_model.objects[0]->config.get().opt_int("top_color_penetration_layers") == 8);
    fs::remove(path);
}

// Writes painted-slab projects on another project's printer/filament settings, for a CLI A/B of the
// G-code between a build without this feature and one with it (default 0 must be byte-identical
// outside the config block). Hidden: it needs a project file from outside the repository.
//   PAINTPEN_SRC=<project.3mf> PAINTPEN_OUT=<prefix>  libslic3r_tests "[.paintpen_fixture]"
// writes <prefix>_walls.3mf and <prefix>_unlimited.3mf (top Extruder2, bottom Extruder3, +X side Extruder2).
TEST_CASE("Write painted-slab fixtures for a CLI A/B", "[.paintpen_fixture]")
{
    const char *src = std::getenv("PAINTPEN_SRC");
    const char *out = std::getenv("PAINTPEN_OUT");
    REQUIRE(src != nullptr);
    REQUIRE(out != nullptr);
    for (const PaintDepthMode mode : { pdmWalls, pdmUnlimited }) {
        DynamicPrintConfig        cfg;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
        Model                     loaded;
        PlateDataPtrs             plates;
        std::vector<Preset *>     presets;
        bool                      is_bbl = false;
        Semver                    version;
        REQUIRE(load_bbs_3mf(src, &cfg, &ctxt, &loaded, &plates, &presets, &is_bbl, &version, nullptr,
                             LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
        REQUIRE(! loaded.objects.empty());
        const Vec3d offset = loaded.objects.front()->instances.front()->get_offset();
        release_PlateData_list(plates);

        Model        model;
        ModelObject *object = model.add_object();
        object->name        = "painted_slab";
        ModelVolume *volume = object->add_volume(make_cube(30., 30., 6.));
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(offset.x(), offset.y(), 0.));
        object->ensure_on_bed();
        TriangleSelector selector(volume->mesh());
        for (int f : TOP_CAP)    selector.set_facet(f, EnforcerBlockerType::Extruder2);
        for (int f : BOTTOM_CAP) selector.set_facet(f, EnforcerBlockerType::Extruder3);
        for (int f : PLUS_X)     selector.set_facet(f, EnforcerBlockerType::Extruder2);
        REQUIRE(volume->mmu_segmentation_facets.set(selector));

        cfg.set_key_value("paint_depth_mode", new ConfigOptionEnum<PaintDepthMode>(mode));
        cfg.erase("top_color_penetration_layers");
        cfg.erase("bottom_color_penetration_layers");

        PlateData *plate = new PlateData();
        plate->plate_index = 0;
        plate->objects_and_instances.emplace_back(0, 0);
        const std::string path = std::string(out) + (mode == pdmWalls ? "_walls.3mf" : "_unlimited.3mf");
        StoreParams       sp;
        sp.path            = path.c_str();
        sp.model           = &model;
        sp.config          = &cfg;
        sp.plate_data_list = { plate };
        sp.project_presets = presets;
        sp.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
        const bool stored  = store_bbs_3mf(sp);
        release_PlateData_list(sp.plate_data_list);
        for (Preset *p : presets)
            delete p;
        REQUIRE(stored);
    }
}
