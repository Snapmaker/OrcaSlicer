// Bambu High Flow, phase 1 (tests/research_high_flow_brands.md, owner decisions D1-D9):
// BambuFlowSupport::derive switches Edge's flow variants on from Bambu's own variant names,
// dual-nozzle printers pick each filament's column from filament_map, 'E' nozzles slice High Flow
// and 'U' (TPU High Flow) Standard, and a filament without a High Flow column falls back to Standard.
#include <catch2/catch.hpp>

#include "libslic3r/BambuFlowSupport.hpp"
#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PresetFlowVariant.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem.hpp>

#include <cmath>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::BambuFlowSupport;

namespace {

const std::vector<std::string> STD_HF{ FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW };

// A standalone filament preset config, the shape Preset::normalize sees after include/inherits.
DynamicPrintConfig filament_preset(const std::vector<std::string> &variants, const std::vector<double> &max_volumetric)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("filament_diameter", new ConfigOptionFloats{ 1.75 });
    cfg.set_key_value("filament_flow_support", new ConfigOptionStrings{ FLOW_MODE_STANDARD });
    cfg.set_key_value("filament_extruder_variant", new ConfigOptionStrings(variants));
    cfg.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats(max_volumetric));
    return cfg;
}

DynamicPrintConfig process_preset(const std::vector<std::string> &variants)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    cfg.set_key_value("process_flow_support", new ConfigOptionStrings{ FLOW_MODE_STANDARD });
    cfg.set_key_value("print_extruder_variant", new ConfigOptionStrings(variants));
    return cfg;
}

DynamicPrintConfig printer_preset(const std::vector<std::string> &extruder_variant_list)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("nozzle_diameter", new ConfigOptionFloats(std::vector<double>(extruder_variant_list.size(), 0.4)));
    cfg.set_key_value("printer_flow_support", new ConfigOptionStrings{ FLOW_MODE_STANDARD });
    cfg.set_key_value("extruder_variant_list", new ConfigOptionStrings(extruder_variant_list));
    return cfg;
}

std::vector<std::string> strings(const ConfigBase &cfg, const char *key) { return cfg.option<ConfigOptionStrings>(key)->values; }

ConfigOptionEnumsGeneric *enums(DynamicPrintConfig &cfg, const char *key) { return cfg.option<ConfigOptionEnumsGeneric>(key, true); }

const std::string H2D_LEFT  = "Direct Drive Standard,Direct Drive High Flow,Direct Drive E3D High Flow";
const std::string H2D_RIGHT = "Direct Drive Standard,Direct Drive High Flow,Direct Drive TPU High Flow,Direct Drive E3D High Flow";

} // namespace

TEST_CASE("derive switches a Bambu filament with Standard and High Flow slots to two flow columns", "[BambuFlowSupport]")
{
    DynamicPrintConfig cfg = filament_preset({ "Direct Drive Standard", "Direct Drive High Flow", "Direct Drive E3D High Flow" },
                                             { 25., 40., 25. });
    const DeriveResult r = derive(cfg);
    CHECK(r.filament);
    CHECK_FALSE(r.process);
    CHECK_FALSE(r.printer);
    CHECK(strings(cfg, "filament_flow_support") == STD_HF);

    SECTION("idempotent")
    {
        const DynamicPrintConfig once = cfg;
        derive(cfg);
        CHECK(cfg.equals(once));
    }
    SECTION("Preset::normalize runs it and sizes short flow-variant vectors to both columns")
    {
        DynamicPrintConfig fresh = filament_preset({ "Direct Drive Standard", "Direct Drive High Flow" }, { 25., 40. });
        fresh.set_key_value("filament_flow_ratio", new ConfigOptionFloats{ 0.98 });
        Preset::normalize(fresh);
        CHECK(strings(fresh, "filament_flow_support") == STD_HF);
        // Bambu stores one value where both slots agree; the High Flow slot repeats the Standard one.
        CHECK(fresh.option<ConfigOptionFloats>("filament_flow_ratio")->values == std::vector<double>{ 0.98, 0.98 });
        // Slot 1 is Bambu's own High Flow value.
        CHECK(get_preset_value_at(fresh, *fresh.option<ConfigOptionFloats>("filament_max_volumetric_speed"), ConfigFlowDomain::Filament,
                                  fvtHighFlow) == Approx(40.));
        CHECK(get_preset_value_at(fresh, *fresh.option<ConfigOptionFloats>("filament_max_volumetric_speed"), ConfigFlowDomain::Filament,
                                  fvtStandard) == Approx(25.));
    }
}

TEST_CASE("derive leaves filaments without a Standard / High Flow pair alone", "[BambuFlowSupport]")
{
    struct Case
    {
        const char              *what;
        std::vector<std::string> variants;
        std::vector<double>      mvs;
    };
    const std::vector<Case> cases = {
        { "Standard only (an X1 / P1 / A1 preset or the default)", { "Direct Drive Standard" }, { 20. } },
        { "TPU High Flow only", { "Direct Drive TPU High Flow" }, { 16. } },
        { "Standard then TPU High Flow", { "Direct Drive Standard", "Direct Drive TPU High Flow" }, { 12., 16. } },
        { "Standard then E3D High Flow", { "Direct Drive Standard", "Direct Drive E3D High Flow" }, { 25., 25. } },
        { "High Flow before Standard", { "Direct Drive High Flow", "Direct Drive Standard" }, { 40., 25. } },
        { "mixed extruder types", { "Direct Drive Standard", "Bowden High Flow" }, { 25., 40. } },
        { "two names but a single value (no High Flow data)", { "Direct Drive Standard", "Direct Drive High Flow" }, { 18. } },
    };
    for (const Case &c : cases) {
        INFO(c.what);
        DynamicPrintConfig cfg = filament_preset(c.variants, c.mvs);
        CHECK_FALSE(derive(cfg).any());
        CHECK(strings(cfg, "filament_flow_support") == std::vector<std::string>{ FLOW_MODE_STANDARD });
    }
}

TEST_CASE("an authored flow support always wins over derive", "[BambuFlowSupport]")
{
    DynamicPrintConfig filament = filament_preset({ "Direct Drive Standard", "Direct Drive High Flow" }, { 25., 40. });
    filament.option<ConfigOptionStrings>("filament_flow_support")->values = { FLOW_MODE_HIGH_FLOW };
    CHECK_FALSE(derive(filament).any());
    CHECK(strings(filament, "filament_flow_support") == std::vector<std::string>{ FLOW_MODE_HIGH_FLOW });

    // A Snapmaker U1 filament authors both columns and has no Bambu variant names: untouched.
    DynamicPrintConfig u1 = filament_preset({ "Direct Drive Standard" }, { 22., 40. });
    u1.option<ConfigOptionStrings>("filament_flow_support")->values = STD_HF;
    const DynamicPrintConfig u1_before = u1;
    CHECK_FALSE(derive(u1).any());
    CHECK(u1.equals(u1_before));

    DynamicPrintConfig printer = printer_preset({ H2D_LEFT, H2D_RIGHT });
    printer.option<ConfigOptionStrings>("printer_flow_support")->values = { "standard", "something_else" };
    CHECK_FALSE(derive(printer).any());
    CHECK(strings(printer, "printer_flow_support") == std::vector<std::string>{ "standard", "something_else" });
}

TEST_CASE("derive switches Bambu process presets with Standard / High Flow slots", "[BambuFlowSupport]")
{
    DynamicPrintConfig on = process_preset({ "Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Standard", "Direct Drive High Flow" });
    CHECK(derive(on).process);
    CHECK(strings(on, "process_flow_support") == STD_HF);

    DynamicPrintConfig off = process_preset({ "Direct Drive Standard" });
    CHECK_FALSE(derive(off).any());
    CHECK(strings(off, "process_flow_support") == std::vector<std::string>{ FLOW_MODE_STANDARD });
}

TEST_CASE("derive offers High Flow on Bambu printers whose extruders take it, not on the X2D", "[BambuFlowSupport]")
{
    DynamicPrintConfig h2d = printer_preset({ H2D_LEFT, H2D_RIGHT });
    CHECK(derive(h2d).printer);
    CHECK(strings(h2d, "printer_flow_support") == STD_HF);

    DynamicPrintConfig h2s = printer_preset({ H2D_LEFT });
    CHECK(derive(h2s).printer);

    // D9: the X2D's second extruder is Bowden; phase 1 leaves the whole printer Standard-only.
    DynamicPrintConfig x2d = printer_preset({ H2D_LEFT, "Bowden Standard,Bowden High Flow,Bowden E3D High Flow" });
    CHECK_FALSE(derive(x2d).any());

    // X1 / P1 / A1: no variant list (the default), no High Flow (D5).
    DynamicPrintConfig x1c = printer_preset({ "Direct Drive Standard" });
    CHECK_FALSE(derive(x1c).any());

    // Only TPU High Flow / E3D High Flow on offer is not the Bambu High Flow column.
    CHECK_FALSE(printer_variants_offer_high_flow({ "Direct Drive Standard,Direct Drive TPU High Flow" }));
    CHECK_FALSE(printer_variants_offer_high_flow({ "Direct Drive Standard,Direct Drive E3D High Flow" }));
    CHECK(printer_variants_offer_high_flow({ "Direct Drive Standard,Direct Drive High Flow" }));

    // A High Flow-only extruder (Prusa CORE One INDX: every tool is a High Flow nozzle) has no
    // Standard to switch to, so it offers no Standard/High Flow choice.
    CHECK_FALSE(printer_variants_offer_high_flow({ "Direct Drive High Flow" }));
    CHECK_FALSE(printer_variants_offer_high_flow({ "Direct Drive High Flow", "Direct Drive High Flow", "Direct Drive High Flow", "Direct Drive High Flow" }));
    DynamicPrintConfig indx = printer_preset({ "Direct Drive High Flow", "Direct Drive High Flow", "Direct Drive High Flow", "Direct Drive High Flow" });
    CHECK_FALSE(derive(indx).any());
}

TEST_CASE("derive never rewrites a composed full config", "[BambuFlowSupport]")
{
    DynamicPrintConfig full = filament_preset({ "Direct Drive Standard", "Direct Drive High Flow" }, { 25., 40. });
    full.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    full.set_key_value("nozzle_diameter", new ConfigOptionFloats{ 0.4, 0.4 });
    full.set_key_value("extruder_variant_list", new ConfigOptionStrings{ H2D_LEFT, H2D_RIGHT });
    CHECK_FALSE(derive(full).any());
    CHECK(strings(full, "filament_flow_support") == std::vector<std::string>{ FLOW_MODE_STANDARD });
}

TEST_CASE("D6: E3D nozzles slice High Flow, TPU High Flow nozzles slice Standard", "[BambuFlowSupport]")
{
    CHECK(nozzle_flow_from_device_code('H') == nvtHighFlow);
    CHECK(nozzle_flow_from_device_code('E') == nvtHighFlow);
    CHECK(nozzle_flow_from_device_code('h') == nvtHighFlow);
    CHECK(nozzle_flow_from_device_code('e') == nvtHighFlow);
    CHECK(nozzle_flow_from_device_code('U') == nvtStandard);
    CHECK(nozzle_flow_from_device_code('S') == nvtStandard);
    CHECK(nozzle_flow_from_device_code('?') == nvtStandard);

    CHECK(slicing_flow_of_nozzle(nvtStandard) == fvtStandard);
    CHECK(slicing_flow_of_nozzle(nvtHighFlow) == fvtHighFlow);
    CHECK(slicing_flow_of_nozzle(nvtE3DHighFlow) == fvtHighFlow);
    CHECK(slicing_flow_of_nozzle(nvtTPUHighFlow) == fvtStandard);
    CHECK(slicing_flow_of_nozzle(nvtHybrid) == fvtStandard);
    CHECK(slicing_flow_of_nozzle(42) == fvtStandard);
}

TEST_CASE("D2: each filament slices the column of the nozzle its extruder carries", "[BambuFlowSupport]")
{
    using V = std::vector<FilamentVolumeType>;
    // Left Standard, right High Flow; filaments 1 and 4 on the left, 2 and 3 on the right.
    CHECK(filament_volume_types_from_map({ 1, 2, 2, 1 }, { nvtStandard, nvtHighFlow }, 4) == V{ fvtStandard, fvtHighFlow, fvtHighFlow, fvtStandard });
    // E3D on the right counts as High Flow; TPU High Flow on the right as Standard.
    CHECK(filament_volume_types_from_map({ 1, 2 }, { nvtHighFlow, nvtE3DHighFlow }, 2) == V{ fvtHighFlow, fvtHighFlow });
    CHECK(filament_volume_types_from_map({ 1, 2 }, { nvtHighFlow, nvtTPUHighFlow }, 2) == V{ fvtHighFlow, fvtStandard });
    // A short map, a 0 entry or an extruder the nozzle list does not reach slice Standard.
    CHECK(filament_volume_types_from_map({ 2 }, { nvtStandard, nvtHighFlow }, 3) == V{ fvtHighFlow, fvtStandard, fvtStandard });
    CHECK(filament_volume_types_from_map({ 0, 3 }, { nvtHighFlow, nvtHighFlow }, 2) == V{ fvtStandard, fvtStandard });
    CHECK(filament_volume_types_from_map({}, {}, 1) == V{ fvtStandard });
}

TEST_CASE("D2: the slice rewrites filament_volume_type from filament_map only on dual-nozzle High Flow printers", "[BambuFlowSupport]")
{
    auto composed = [](size_t nozzles, bool high_flow) {
        DynamicPrintConfig cfg;
        cfg.set_key_value("nozzle_diameter", new ConfigOptionFloats(std::vector<double>(nozzles, 0.4)));
        cfg.set_key_value("filament_diameter", new ConfigOptionFloats{ 1.75, 1.75, 1.75 });
        cfg.set_key_value("printer_flow_support", new ConfigOptionStrings(high_flow ? STD_HF : std::vector<std::string>{ FLOW_MODE_STANDARD }));
        cfg.set_key_value("filament_map", new ConfigOptionInts{ 2, 1, 2 });
        enums(cfg, "nozzle_volume_type")->values   = std::vector<int>(nozzles, int(nvtStandard));
        enums(cfg, "nozzle_volume_type")->values.back() = int(nvtHighFlow);
        enums(cfg, "filament_volume_type")->values = { fvtStandard, fvtHighFlow, fvtStandard };
        return cfg;
    };
    DynamicPrintConfig h2d = composed(2, true);
    CHECK(apply_filament_volume_types_from_map(h2d));
    CHECK(h2d.option<ConfigOptionEnumsGeneric>("filament_volume_type")->values == std::vector<int>{ fvtHighFlow, fvtStandard, fvtHighFlow });
    CHECK_FALSE(apply_filament_volume_types_from_map(h2d)); // already derived

    // A printer without High Flow support, and a single-nozzle printer, keep the GUI's mapping
    // (the Snapmaker U1 grouping, the uniform single-nozzle sync).
    DynamicPrintConfig no_support = composed(2, false);
    CHECK_FALSE(apply_filament_volume_types_from_map(no_support));
    CHECK(no_support.option<ConfigOptionEnumsGeneric>("filament_volume_type")->values == std::vector<int>{ fvtStandard, fvtHighFlow, fvtStandard });
    DynamicPrintConfig single = composed(1, true);
    CHECK_FALSE(apply_filament_volume_types_from_map(single));
}

TEST_CASE("D4: a filament mapped to High Flow without a High Flow column is reported and slices Standard", "[BambuFlowSupport]")
{
    // Composed config: filament 0 has [standard, high_flow], filament 1 only [standard].
    DynamicPrintConfig cfg;
    cfg.set_key_value("filament_diameter", new ConfigOptionFloats{ 1.75, 1.75 });
    cfg.set_key_value("filament_flow_support", new ConfigOptionStrings{ FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW, FLOW_MODE_STANDARD });
    cfg.set_key_value("filament_flow_step_size", new ConfigOptionInts{ 2, 1 });
    cfg.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{ 25., 40., 12. });
    enums(cfg, "filament_volume_type")->values = { fvtHighFlow, fvtHighFlow };

    CHECK(filaments_without_high_flow_column(cfg, { 0, 1 }) == std::vector<unsigned int>{ 1 });
    const auto &mvs = *cfg.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    CHECK(get_value_at(cfg, mvs, ConfigFlowDomain::Filament, 0) == Approx(40.));
    CHECK(get_value_at(cfg, mvs, ConfigFlowDomain::Filament, 1) == Approx(12.)); // its Standard value

    // Only filaments the plate uses count, and Standard-mapped ones never do.
    CHECK(filaments_without_high_flow_column(cfg, { 0 }).empty());
    enums(cfg, "filament_volume_type")->values = { fvtHighFlow, fvtStandard };
    CHECK(filaments_without_high_flow_column(cfg, { 0, 1 }).empty());
}

TEST_CASE("Bambu 3MF export takes each filament's High Flow column for Bambu's High Flow variants", "[BambuFlowSupport][BambuExport]")
{
    // Two filaments with mixed step sizes: filament 1 [standard, high_flow], filament 2 [standard].
    // Before, the packed vector [25, 40, 12] was split "n / filament_count" and filament 1's
    // High Flow slot was read for filament 2.
    DynamicPrintConfig project = DynamicPrintConfig::full_print_config();
    project.set_num_extruders(2);
    project.set_num_filaments(2);
    project.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.4, 0.4 };
    project.option<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#00FF00" };
    project.option<ConfigOptionStrings>("printer_extruder_variant", true)->values = { "Direct Drive Standard", "Direct Drive High Flow",
                                                                                      "Direct Drive Standard", "Direct Drive High Flow" };
    project.option<ConfigOptionInts>("printer_extruder_id", true)->values = { 1, 1, 2, 2 };
    project.option<ConfigOptionStrings>("filament_flow_support", true)->values = { FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW, FLOW_MODE_STANDARD };
    project.option<ConfigOptionInts>("filament_flow_step_size", true)->values = { 2, 1 };
    project.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = { 25., 40., 12. };

    BambuExport::Context ctx = BambuExport::Context::from_project(project);
    BambuExport::Report  report;
    const BambuExport::Config out = BambuExport::convert_project(project, ctx, report);

    REQUIRE(ctx.filament.size() == 4);
    CHECK(ctx.filament.names == std::vector<std::string>{ "Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Standard", "Direct Drive High Flow" });
    auto it = out.find("filament_max_volumetric_speed");
    REQUIRE(it != out.end());
    REQUIRE(it->second.values.size() == 4);
    CHECK(std::stod(it->second.values[0]) == Approx(25.));
    CHECK(std::stod(it->second.values[1]) == Approx(40.));
    CHECK(std::stod(it->second.values[2]) == Approx(12.));
    CHECK(std::stod(it->second.values[3]) == Approx(12.));
}

// ---------------------------------------------------------------------------------------------
// With the shipped BBL profiles.
// ---------------------------------------------------------------------------------------------

namespace {

PresetBundle &bbl_bundle()
{
    static std::unique_ptr<PresetBundle> bundle;
    static std::unique_ptr<PresetBundle> library;
    if (bundle)
        return *bundle;
    const std::string saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("bambu_hf_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    library = std::make_unique<PresetBundle>();
    library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                           ForwardCompatibilitySubstitutionRule::EnableSilent);
    bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                          library.get());
    set_data_dir(saved_data_dir);
    return *bundle;
}

bool has_high_flow(const Preset *preset, const char *key)
{
    REQUIRE(preset != nullptr);
    const auto *opt = preset->config.option<ConfigOptionStrings>(key);
    return opt != nullptr && opt->values == STD_HF;
}

// Two H2D filaments, filament i on extruder filament_map[i], one 30 x 30 x 4 mm block each.
DynamicPrintConfig h2d_config(const std::vector<std::string> &filaments, const std::string &nozzles, const std::string &map,
                              const std::string &map_mode = "Manual")
{
    PresetBundle &b = bbl_bundle();
    REQUIRE(b.printers.select_preset_by_name("Bambu Lab H2D 0.4 nozzle", true));
    REQUIRE(b.prints.select_preset_by_name("0.20mm Standard @BBL H2D", true));
    REQUIRE(b.filaments.select_preset_by_name(filaments.front(), true));
    b.filament_presets = { filaments.front() };
    b.set_num_filaments(unsigned(filaments.size()), std::vector<std::string>{ "#1943E0", "#FFFFFF" });
    b.filament_presets = filaments;
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "nozzle_volume_type", nozzles },
        { "filament_map_mode", map_mode },
        { "filament_map", map },
        // No tower and no arcs: every extrusion move is a straight G1 the rate check below can read.
        { "enable_prime_tower", "0" },
        { "enable_arc_fitting", "0" },
        { "gcode_comments", 0 },
        // Fast enough that every filament's max volumetric speed caps its infill, and no layer
        // slow-down for cooling, so the cap is what the G-code F values show.
        { "sparse_infill_speed", "600,600,600,600" },
        { "internal_solid_infill_speed", "600,600,600,600" },
        { "inner_wall_speed", "600,600,600,600" },
        { "slow_down_for_layer_cooling", "0,0" },
    });
    return cfg;
}

void add_blocks(Model &model, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        ModelObject *object = model.add_object();
        object->name        = "block" + std::to_string(i + 1);
        TriangleMesh mesh   = Test::mesh(Test::TestMesh::cube_20x20x20);
        mesh.scale(Vec3f(1.5f, 1.5f, 0.2f));
        object->add_volume(mesh);
        object->config.set("extruder", int(i + 1));
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(120. + 60. * double(i), 140., 0.));
        object->ensure_on_bed();
    }
}

std::string slice(Print &print, Model &model, const DynamicPrintConfig &cfg)
{
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

// Highest commanded volumetric rate (mm3/s) of each tool's extrusion moves (relative E, as
// Bambu's start G-code sets M83).
std::map<int, double> max_volumetric_per_tool(const std::string &gcode, double filament_diameter = 1.75)
{
    static const std::regex re_tool(R"(^T(\d+)\b)");
    static const std::regex re_word(R"(([XYEF])(-?\d*\.?\d+))");
    const double            area = 3.14159265358979 * filament_diameter * filament_diameter / 4.;
    std::map<int, double>   out;
    std::istringstream      ss(gcode);
    std::string             line;
    int                     tool = -1;
    double                  x = 0., y = 0., f = 0.;
    while (std::getline(ss, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re_tool)) {
            const int t = std::stoi(m[1].str());
            if (t < 100) // T255 / T1000 style codes are firmware commands, not tool changes
                tool = t;
            continue;
        }
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0)
            continue;
        double nx = x, ny = y, e = 0.;
        bool   has_e = false;
        const std::string body = line.substr(0, line.find(';'));
        for (auto it = std::sregex_iterator(body.begin(), body.end(), re_word); it != std::sregex_iterator(); ++it) {
            const char   c = (*it)[1].str()[0];
            const double v = std::stod((*it)[2].str());
            if (c == 'X') nx = v;
            else if (c == 'Y') ny = v;
            else if (c == 'E') { e = v; has_e = true; }
            else if (c == 'F') f = v;
        }
        const double dist = std::hypot(nx - x, ny - y);
        x = nx;
        y = ny;
        if (tool < 0 || !has_e || e <= 0. || dist < 2. || f <= 0.)
            continue;
        const double rate = e * area * (f / 60.) / dist;
        out[tool]         = std::max(out[tool], rate);
    }
    return out;
}

} // namespace

TEST_CASE("Shipped BBL profiles: High Flow on H2D / H2D Pro / H2C / H2S / P2S only", "[BambuFlowSupport][BBLProfiles]")
{
    PresetBundle &b = bbl_bundle();
    for (const char *name : { "Bambu Lab H2D 0.4 nozzle", "Bambu Lab H2D Pro 0.4 nozzle", "Bambu Lab H2C 0.4 nozzle", "Bambu Lab H2S 0.4 nozzle",
                              "Bambu Lab P2S 0.4 nozzle", "Bambu Lab H2D 0.6 nozzle" }) {
        INFO(name);
        CHECK(has_high_flow(b.printers.find_preset(name, false), "printer_flow_support"));
    }
    for (const char *name : { "Bambu Lab X2D 0.4 nozzle", "Bambu Lab X1 Carbon 0.4 nozzle", "Bambu Lab P1S 0.4 nozzle", "Bambu Lab A1 0.4 nozzle",
                              "Bambu Lab A1 mini 0.4 nozzle", "Bambu Lab A2L 0.4 nozzle" }) {
        INFO(name);
        CHECK_FALSE(has_high_flow(b.printers.find_preset(name, false), "printer_flow_support"));
    }
    CHECK(has_high_flow(b.prints.find_preset("0.20mm Standard @BBL H2D", false), "process_flow_support"));
    CHECK_FALSE(has_high_flow(b.prints.find_preset("0.20mm Standard @BBL X1C", false), "process_flow_support"));

    const Preset *pla = b.filaments.find_preset("Bambu PLA Basic @BBL H2D", false);
    CHECK(has_high_flow(pla, "filament_flow_support"));
    const auto &mvs = *pla->config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    CHECK(get_preset_value_at(pla->config, mvs, ConfigFlowDomain::Filament, fvtStandard) == Approx(25.));
    CHECK(get_preset_value_at(pla->config, mvs, ConfigFlowDomain::Filament, fvtHighFlow) == Approx(40.));
    // The X1 Carbon copy has no High Flow column (D5: no X1 / P1 refresh).
    CHECK_FALSE(has_high_flow(b.filaments.find_preset("Bambu PLA Basic @BBL X1C", false), "filament_flow_support"));
}

TEST_CASE("H2D, left Standard and right High Flow: each filament slices its nozzle's column", "[BambuFlowSupport][BBLProfiles]")
{
    const std::string pla   = "Bambu PLA Basic @BBL H2D";
    const std::string petcf = "Bambu PET-CF @BBL H2D";

    SECTION("PLA Basic on both nozzles: max volumetric speed 25 on the left, 40 on the right")
    {
        Model              model;
        Print              print;
        add_blocks(model, 2);
        const std::string gcode = slice(print, model, h2d_config({ pla, pla }, "Standard,High Flow", "1,2"));
        CHECK(print.config().filament_volume_type.values == std::vector<int>{ fvtStandard, fvtHighFlow });
        const ResolvedFilamentFlow flow = ResolvedFilamentFlow::resolve(print.config());
        REQUIRE(flow.max_volumetric_speed.size() == 2);
        CHECK(flow.max_volumetric_speed[0] == Approx(25.));
        CHECK(flow.max_volumetric_speed[1] == Approx(40.));
        const std::map<int, double> rate = max_volumetric_per_tool(gcode);
        REQUIRE(rate.count(0) == 1);
        REQUIRE(rate.count(1) == 1);
        CHECK(rate.at(0) <= 25. * 1.03);
        CHECK(rate.at(1) > 25. * 1.1);
        CHECK(rate.at(1) <= 40. * 1.03);
    }
    SECTION("PET-CF on the High Flow nozzle uses its High Flow flow ratio and speed")
    {
        Model model;
        Print print;
        add_blocks(model, 2);
        slice(print, model, h2d_config({ pla, petcf }, "Standard,High Flow", "1,2"));
        const ResolvedFilamentFlow flow = ResolvedFilamentFlow::resolve(print.config());
        REQUIRE(flow.flow_ratio.size() == 2);
        CHECK(flow.flow_ratio[0] == Approx(0.98));
        CHECK(flow.max_volumetric_speed[0] == Approx(25.));
        CHECK(flow.flow_ratio[1] == Approx(1.0));
        CHECK(flow.max_volumetric_speed[1] == Approx(8.));
    }
    SECTION("the same filaments swapped between the nozzles swap columns")
    {
        Model model;
        Print print;
        add_blocks(model, 2);
        slice(print, model, h2d_config({ pla, petcf }, "Standard,High Flow", "2,1"));
        CHECK(print.config().filament_volume_type.values == std::vector<int>{ fvtHighFlow, fvtStandard });
        const ResolvedFilamentFlow flow = ResolvedFilamentFlow::resolve(print.config());
        CHECK(flow.max_volumetric_speed[0] == Approx(40.));
        CHECK(flow.flow_ratio[1] == Approx(0.9555));
        CHECK(flow.max_volumetric_speed[1] == Approx(5.));
    }
    SECTION("both nozzles Standard: Standard columns everywhere")
    {
        Model model;
        Print print;
        add_blocks(model, 2);
        slice(print, model, h2d_config({ pla, petcf }, "Standard,Standard", "1,2"));
        CHECK(print.config().filament_volume_type.values == std::vector<int>{ fvtStandard, fvtStandard });
        const ResolvedFilamentFlow flow = ResolvedFilamentFlow::resolve(print.config());
        CHECK(flow.max_volumetric_speed[0] == Approx(25.));
        CHECK(flow.max_volumetric_speed[1] == Approx(5.));
        CHECK(flow.flow_ratio[1] == Approx(0.9555));
    }
    SECTION("auto grouping: every filament follows the nozzle the grouping put it on")
    {
        Model model;
        Print print;
        add_blocks(model, 2);
        slice(print, model, h2d_config({ pla, petcf }, "Standard,High Flow", "1,1", "Auto For Flush"));
        const std::vector<int> &map   = print.config().filament_map.values;
        const std::vector<int> &types = print.config().filament_volume_type.values;
        REQUIRE(map.size() == 2);
        REQUIRE(types.size() == 2);
        for (size_t i = 0; i < 2; ++i) {
            INFO("filament " << i << " on extruder " << map[i]);
            CHECK(types[i] == (map[i] == 2 ? int(fvtHighFlow) : int(fvtStandard)));
        }
    }
}
