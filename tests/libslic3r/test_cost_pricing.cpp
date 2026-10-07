// Costs > Project: fees and markup over a plate's cost (src/libslic3r/CostPricing.{hpp,cpp}), your
// defaults in the cost store (file version 3) and the project's own values in the 3MF.

#include <catch2/catch.hpp>

#include "libslic3r/CostEstimate.hpp"
#include "libslic3r/CostOverrides.hpp"
#include "libslic3r/CostPricing.hpp"
#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <set>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

fs::path scratch_dir(const std::string &name)
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("snorca_pricing_" + name + "_%%%%-%%%%");
    fs::create_directories(dir);
    return dir;
}

void write_text(const fs::path &path, const std::string &text)
{
    boost::nowide::ofstream out(path.string(), std::ios::binary);
    out << text;
}

std::string read_text(const fs::path &path)
{
    boost::nowide::ifstream in(path.string(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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

// A plate that cost 10.00 of material and 5.00 of machine time.
CostBreakdown plate_cost(double material = 10., double machine = 5.)
{
    CostBreakdown c;
    c.material = c.display_material = material;
    c.machine = c.display_machine = machine;
    c.total = c.display_total = material + machine;
    c.print_time_s       = 3600.;
    c.machine_rate_per_h = machine;
    return c;
}

PricingSettings fees()
{
    PricingSettings s;
    s.assembly_hours      = 0.5;
    s.assembly_rate_per_h = 20.;     // 10.00
    s.fee_per_object      = 0.25;
    s.fee_per_part        = 0.10;
    s.fee_per_plate       = 2.;
    s.packaging_per_plate = 1.5;
    return s;
}

ProjectPricing some_project_pricing()
{
    ProjectPricing p;
    PricingSettings v;
    v.markup_type    = MarkupType::Percent;
    v.markup_basis   = MarkupBasis::Material;
    v.markup_value   = 35.;
    v.fee_per_object = 0.75;
    p.set_field(PricingField::Markup, v);
    p.set_field(PricingField::ObjectFee, v);
    return p;
}

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
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values  = {0.4};
    cfg.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000"};
    cfg.set_key_value("printer_model", new ConfigOptionString("Bambu Lab P1S"));
    return cfg;
}

void one_cube(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "cube";
    object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();
}

bool store(const fs::path &path, Model &model, DynamicPrintConfig &cfg, SaveStrategy strategy, BambuExport::Report *bambu = nullptr)
{
    const std::string p = path.string();
    StoreParams       sp;
    sp.path         = p.c_str();
    sp.model        = &model;
    sp.config       = &cfg;
    sp.strategy     = strategy;
    sp.bambu_compat = bambu != nullptr;
    sp.bambu_report = bambu;
    return store_bbs_3mf(sp);
}

bool load(const fs::path &path, Model &model)
{
    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::EnableSilent};
    PlateDataPtrs             plates;
    std::vector<Preset *>     presets;
    bool                      is_bbl = false;
    Semver                    version;
    const bool ok = load_bbs_3mf(path.string().c_str(), &config, &ctxt, &model, &plates, &presets, &is_bbl, &version, nullptr,
                                 LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances |
                                     LoadStrategy::Silence);
    release_PlateData_list(plates);
    for (Preset *preset : presets)
        delete preset;
    return ok;
}

const SaveStrategy PROJECT = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;

} // namespace

// ------------------------------------------------------------------ math ----

TEST_CASE("Pricing: no fees and no markup leave the cost alone", "[CostPricing]")
{
    const PricedCost p = price_plate(plate_cost(), {3, 7}, PricingSettings());
    CHECK(p.fees == 0.);
    CHECK(p.assembly == 0.);
    CHECK(p.object_fees == 0.);
    CHECK(p.part_fees == 0.);
    CHECK(p.setup_fees == 0.);
    CHECK(p.packaging == 0.);
    CHECK_FALSE(p.has_markup());
    CHECK(p.markup == 0.);
    CHECK_THAT(p.total_cost, WithinAbs(15., 1e-9));
    CHECK_THAT(p.selling_price, WithinAbs(15., 1e-9));
}

TEST_CASE("Pricing: each fee counts what it says", "[CostPricing]")
{
    const PricedCost p = price_plate(plate_cost(), {3, 7}, fees());
    CHECK(p.objects == 3);
    CHECK(p.parts == 7);
    CHECK_THAT(p.assembly, WithinAbs(10., 1e-9));      // 0.5 h x 20.00
    CHECK_THAT(p.object_fees, WithinAbs(0.75, 1e-9));  // 3 x 0.25
    CHECK_THAT(p.part_fees, WithinAbs(0.70, 1e-9));    // 7 x 0.10
    CHECK_THAT(p.setup_fees, WithinAbs(2., 1e-9));
    CHECK_THAT(p.packaging, WithinAbs(1.5, 1e-9));
    CHECK_THAT(p.fees, WithinAbs(14.95, 1e-9));
    CHECK_THAT(p.total_cost, WithinAbs(29.95, 1e-9));  // 10 + 5 + 14.95
    CHECK_THAT(p.selling_price, WithinAbs(29.95, 1e-9));

    // Only some fees set: the others stay 0 (and the breakdown leaves their lines out).
    PricingSettings only_setup;
    only_setup.fee_per_plate = 3.;
    const PricedCost q = price_plate(plate_cost(), {3, 7}, only_setup);
    CHECK(q.object_fees == 0.);
    CHECK(q.part_fees == 0.);
    CHECK(q.assembly == 0.);
    CHECK_THAT(q.fees, WithinAbs(3., 1e-9));
}

TEST_CASE("Pricing: markup percent / flat on material / overall", "[CostPricing]")
{
    PricingSettings s = fees();   // total cost 29.95, material 10.00
    struct Case { MarkupType type; MarkupBasis basis; double value; double markup; };
    const Case cases[] = {
        {MarkupType::Percent, MarkupBasis::Overall, 100., 29.95},   // 100% doubles the cost
        {MarkupType::Percent, MarkupBasis::Overall, 35., 10.48},    // 10.4825 -> 10.48
        {MarkupType::Percent, MarkupBasis::Material, 35., 3.50},    // machine time and fees not marked up
        {MarkupType::Flat, MarkupBasis::Overall, 5., 5.},
        {MarkupType::Flat, MarkupBasis::Material, 5., 5.},          // the basis does not apply to flat
        {MarkupType::Percent, MarkupBasis::Overall, 0., 0.},        // 0 = no markup
    };
    for (const Case &c : cases) {
        s.markup_type  = c.type;
        s.markup_basis = c.basis;
        s.markup_value = c.value;
        const PricedCost p = price_plate(plate_cost(), {3, 7}, s);
        INFO("type " << int(c.type) << " basis " << int(c.basis) << " value " << c.value);
        CHECK(p.has_markup() == (c.value > 0.));
        CHECK_THAT(p.markup, WithinAbs(c.markup, 1e-9));
        CHECK_THAT(p.selling_price, WithinAbs(29.95 + c.markup, 1e-9));
    }

    // A flat markup with nothing to cost still sells for the amount.
    PricingSettings flat;
    flat.markup_type  = MarkupType::Flat;
    flat.markup_value = 4.;
    const PricedCost free_plate = price_plate(plate_cost(0., 0.), {}, flat);
    CHECK_THAT(free_plate.selling_price, WithinAbs(4., 1e-9));
}

TEST_CASE("Pricing: a percent markup is taken of the rounded lines", "[CostPricing]")
{
    PricingSettings s;
    s.markup_value  = 10.;
    s.fee_per_part  = 0.333;   // 3 parts: 0.999 -> 1.00
    const PricedCost p = price_plate(plate_cost(1.005, 0.), {1, 3}, s);   // display_material is already cents in real use
    CHECK_THAT(p.part_fees, WithinAbs(1., 1e-9));
    CHECK_THAT(p.markup, WithinAbs(round_money((1.005 + 1.) * 0.1), 1e-9));
    CHECK_THAT(p.selling_price, WithinAbs(p.total_cost + p.markup, 1e-9));
}

TEST_CASE("Pricing: material prices not in the G-code price as zero material", "[CostPricing]")
{
    CostBreakdown c = plate_cost(12., 3.);
    c.prices_known  = false;
    PricingSettings s;
    s.markup_basis = MarkupBasis::Material;
    s.markup_value = 50.;
    const PricedCost p = price_plate(c, {}, s);
    CHECK(p.material == 0.);
    CHECK(p.markup == 0.);
    CHECK_THAT(p.total_cost, WithinAbs(3., 1e-9));
}

TEST_CASE("Pricing: all plates is the sum of the plates", "[CostPricing]")
{
    PricingSettings s = fees();
    s.markup_type     = MarkupType::Flat;
    s.markup_value    = 5.;
    const PricedCost a = price_plate(plate_cost(10., 5.), {3, 7}, s);
    const PricedCost b = price_plate(plate_cost(4., 1.), {1, 1}, s);
    const PricedCost c = price_plate(plate_cost(0., 2.), {2, 2}, s);
    const PricedCost all = sum_priced({a, b, c});
    CHECK(all.plates == 3);
    CHECK(all.objects == 6);
    CHECK(all.parts == 10);
    CHECK_THAT(all.assembly_hours, WithinAbs(1.5, 1e-9));
    CHECK_THAT(all.setup_fees, WithinAbs(6., 1e-9));     // per plate, 3 plates
    CHECK_THAT(all.packaging, WithinAbs(4.5, 1e-9));
    CHECK_THAT(all.object_fees, WithinAbs(1.5, 1e-9));   // 6 x 0.25
    CHECK_THAT(all.part_fees, WithinAbs(1., 1e-9));      // 10 x 0.10
    CHECK_THAT(all.markup, WithinAbs(15., 1e-9));        // flat per plate
    CHECK_THAT(all.material, WithinAbs(14., 1e-9));
    CHECK_THAT(all.machine, WithinAbs(8., 1e-9));
    CHECK_THAT(all.total_cost, WithinAbs(a.total_cost + b.total_cost + c.total_cost, 1e-9));
    CHECK_THAT(all.selling_price, WithinAbs(a.selling_price + b.selling_price + c.selling_price, 1e-9));
    CHECK_THAT(all.total_cost, WithinAbs(all.material + all.machine + all.fees, 1e-9));
    CHECK_THAT(all.selling_price, WithinAbs(all.total_cost + all.markup, 1e-9));

    // A percent markup on overall: the sum of the plates' markups (each taken of its own plate).
    s.markup_type  = MarkupType::Percent;
    s.markup_value = 20.;
    const PricedCost pa = price_plate(plate_cost(10., 5.), {3, 7}, s);
    const PricedCost pb = price_plate(plate_cost(4., 1.), {1, 1}, s);
    const PricedCost two = sum_priced({pa, pb});
    CHECK_THAT(two.markup, WithinAbs(pa.markup + pb.markup, 1e-9));
    CHECK_THAT(two.markup, WithinAbs(round_money(pa.total_cost * 0.2) + round_money(pb.total_cost * 0.2), 1e-9));

    CHECK(sum_priced({}).plates == 0);
}

TEST_CASE("Pricing: objects are printable instances, parts are model parts", "[CostPricing]")
{
    Model model;
    ModelObject *bracket = model.add_object();   // 2 parts + a modifier, a negative part and a support blocker
    bracket->add_volume(make_cube(10., 10., 10.));
    bracket->add_volume(make_cube(5., 5., 5.));
    bracket->add_volume(make_cube(2., 2., 2.), ModelVolumeType::PARAMETER_MODIFIER);
    bracket->add_volume(make_cube(2., 2., 2.), ModelVolumeType::NEGATIVE_VOLUME);
    bracket->add_volume(make_cube(2., 2., 2.), ModelVolumeType::SUPPORT_BLOCKER);
    bracket->add_volume(make_cube(2., 2., 2.), ModelVolumeType::SUPPORT_ENFORCER);
    bracket->add_instance();
    bracket->add_instance();
    bracket->add_instance()->printable = false;   // not printed: not charged

    ModelObject *pin = model.add_object();       // 1 part
    pin->add_volume(make_cube(3., 3., 3.));
    pin->add_instance();
    pin->add_instance();

    ModelObject *off = model.add_object();       // not printable at all
    off->add_volume(make_cube(3., 3., 3.));
    off->add_instance();
    off->printable = false;

    // Everything on one plate.
    PlateCounts all = count_plate_items(model, [](int, int) { return true; });
    CHECK(all.objects == 4);   // 2 brackets + 2 pins
    CHECK(all.parts == 6);     // 2 x 2 + 2 x 1

    // Plate holding bracket copy 1 and pin copy 0 only.
    const std::set<std::pair<int, int>> on = {{0, 1}, {1, 0}, {2, 0}};
    PlateCounts some = count_plate_items(model, [&on](int o, int i) { return on.count({o, i}) > 0; });
    CHECK(some.objects == 2);
    CHECK(some.parts == 3);

    CHECK(count_plate_items(model, [](int, int) { return false; }).objects == 0);
}

// ------------------------------------------------------- precedence, JSON ----

TEST_CASE("Pricing: the project's own fields over your defaults", "[CostPricing]")
{
    PricingSettings defaults = fees();
    defaults.markup_type     = MarkupType::Flat;
    defaults.markup_value    = 5.;

    ProjectPricing none;
    CHECK(none.empty());
    CHECK(none.resolve(defaults) == defaults);

    const ProjectPricing project = some_project_pricing();
    CHECK_FALSE(project.empty());
    const PricingSettings r = project.resolve(defaults);
    CHECK(r.markup_type == MarkupType::Percent);    // the project's markup, all three parts
    CHECK(r.markup_basis == MarkupBasis::Material);
    CHECK(r.markup_value == 35.);
    CHECK(r.fee_per_object == 0.75);                // the project's
    CHECK(r.fee_per_part == defaults.fee_per_part); // yours
    CHECK(r.fee_per_plate == defaults.fee_per_plate);
    CHECK(r.assembly_hours == defaults.assembly_hours);
    CHECK(r.packaging_per_plate == defaults.packaging_per_plate);

    // A project value of 0 is a value (no fee for this project), not "use the default".
    ProjectPricing zero;
    zero.set_field(PricingField::PlateFee, PricingSettings());
    CHECK(zero.resolve(defaults).fee_per_plate == 0.);

    // Clearing a field goes back to the default.
    ProjectPricing back = project;
    back.clear_field(PricingField::ObjectFee);
    CHECK(back.resolve(defaults).fee_per_object == defaults.fee_per_object);
}

TEST_CASE("Pricing: project JSON keeps only the project's fields", "[CostPricing]")
{
    CHECK(pricing_to_json(ProjectPricing()).empty());

    ProjectPricing p = some_project_pricing();
    const json     j = json::parse(pricing_to_json(p));
    CHECK(j["v"] == 1);
    CHECK(j["markup"]["type"] == "percent");
    CHECK(j["markup"]["basis"] == "material");
    CHECK(j["markup"]["value"] == 35.);
    CHECK(j["fee_per_object"] == 0.75);
    CHECK_FALSE(j.contains("fee_per_part"));
    CHECK_FALSE(j.contains("assembly_hours"));

    ProjectPricing back;
    REQUIRE(pricing_from_json(j.dump(), back));
    CHECK(back == p);

    // Unknown fields survive; bad values are dropped field by field.
    json k              = j;
    k["rush_fee"]       = 9.5;
    k["fee_per_part"]   = -1.;
    k["fee_per_plate"]  = "two";
    k["packaging_per_plate"] = 0.8;
    ProjectPricing odd;
    REQUIRE(pricing_from_json(k.dump(), odd));
    CHECK_FALSE(odd.is_set(PricingField::PartFee));
    CHECK_FALSE(odd.is_set(PricingField::PlateFee));
    CHECK(odd.is_set(PricingField::Packaging));
    CHECK(json::parse(pricing_to_json(odd))["rush_fee"] == 9.5);

    CHECK_FALSE(pricing_from_json("not json", back));
    CHECK(back.empty());
    CHECK_FALSE(pricing_from_json("[1,2]", back));
}

// ------------------------------------------------------------------ store ----

TEST_CASE("Cost store: a version 2 file is migrated to version 3 with no fees and no markup", "[CostPricing][CostOverrides][store]")
{
    const fs::path dir  = scratch_dir("migrate_v3");
    const fs::path path = dir / "filament_overrides.json";
    write_text(path, R"({"version":2,"note":"keep me","filament":[
        {"scope":"family","vendor":"Bambu Lab","type":"PLA","family":"Bambu PLA Basic","price_per_kg":23.5}],
        "machine":[{"scope":"model","vendor":"Bambulab","model":"Bambu Lab X1 Carbon","rate_per_h":1.5}],
        "machine_default":{"rate_per_h":0.75}})");

    CostOverrides::Store store;
    REQUIRE(store.load(path.string()));
    CHECK(store.version() == 3);
    CHECK(store.pricing() == PricingSettings());   // nothing charged, no markup
    CHECK(store.entries().size() == 1);
    CHECK(store.machines().size() == 1);
    CHECK(store.has_default_rate());

    // Unchanged it is rewritten as version 3, everything kept, with an explicit "pricing".
    REQUIRE(store.save(path.string()));
    json j = json::parse(read_text(path));
    CHECK(j["version"] == 3);
    CHECK(j["note"] == "keep me");
    CHECK(j["filament"].size() == 1);
    CHECK(j["machine"].size() == 1);
    CHECK(j["machine_default"]["rate_per_h"] == 0.75);
    REQUIRE(j.contains("pricing"));
    CHECK(j["pricing"]["markup"]["value"] == 0.);
    CHECK(j["pricing"]["fee_per_plate"] == 0.);

    // Defaults set, saved, read back.
    PricingSettings mine = fees();
    mine.markup_value    = 35.;
    REQUIRE(store.set_pricing(mine));
    PricingSettings bad = mine;
    bad.fee_per_part    = -0.1;
    CHECK_FALSE(store.set_pricing(bad));
    CHECK(store.pricing() == mine);
    REQUIRE(store.save(path.string()));

    // A later version's pricing fields survive this one.
    j = json::parse(read_text(path));
    j["pricing"]["vat_percent"] = 20;
    write_text(path, j.dump());
    CostOverrides::Store back;
    REQUIRE(back.load(path.string()));
    CHECK(back.pricing() == mine);
    REQUIRE(back.save(path.string()));
    const json k = json::parse(read_text(path));
    CHECK(k["pricing"]["vat_percent"] == 20);
    CHECK(k["pricing"]["assembly_rate_per_h"] == 20.);

    // Pricing is not slicing: a store with only defaults of yours changes no config.
    CostOverrides::Store only_pricing;
    REQUIRE(only_pricing.set_pricing(mine));
    CHECK(only_pricing.empty());

    fs::remove_all(dir);
}

TEST_CASE("Pricing is not a print setting", "[CostPricing]")
{
    for (const char *key : {"markup", "markup_value", "markup_type", "assembly_hours", "assembly_rate_per_h", "fee_per_object",
                            "fee_per_part", "fee_per_plate", "packaging_per_plate", "edgeslicer_pricing"}) {
        INFO(key);
        CHECK(print_config_def.get(key) == nullptr);
    }
}

// -------------------------------------------------------------------- 3MF ----

TEST_CASE("Project pricing round-trips through the project 3MF and stays out of other files", "[CostPricing][3MF]")
{
    const fs::path dir = scratch_dir("3mf");
    set_temporary_dir(dir.string());
    DynamicPrintConfig cfg = project_config();

    Model model;
    one_cube(model);
    model.pricing = some_project_pricing();

    SECTION("project save and reopen") {
        const fs::path path = dir / "project.3mf";
        REQUIRE(store(path, model, cfg, PROJECT));
        CHECK(zip_entry(path.string(), "3D/3dmodel.model").find("edgeslicer_pricing") != std::string::npos);
        Model back;
        REQUIRE(load(path, back));
        CHECK(back.pricing == model.pricing);
        // Read into Model::pricing only, so it is written from there and nowhere else.
        REQUIRE(back.model_info);
        CHECK(back.model_info->metadata_items.count(PRICING_METADATA_KEY) == 0);

        // The reopened project, saved again, keeps it; with the project's values removed it is gone.
        const fs::path again = dir / "again.3mf";
        REQUIRE(store(again, back, cfg, PROJECT));
        Model twice;
        REQUIRE(load(again, twice));
        CHECK(twice.pricing == model.pricing);
        back.pricing.clear();
        REQUIRE(store(again, back, cfg, PROJECT));
        CHECK(zip_entry(again.string(), "3D/3dmodel.model").find("edgeslicer_pricing") == std::string::npos);
        Model cleared;
        REQUIRE(load(again, cleared));
        CHECK(cleared.pricing.empty());
    }

    SECTION("a project without its own pricing writes none") {
        Model plain;
        one_cube(plain);
        const fs::path path = dir / "plain.3mf";
        REQUIRE(store(path, plain, cfg, PROJECT));
        CHECK(zip_entry(path.string(), "3D/3dmodel.model").find("edgeslicer_pricing") == std::string::npos);
        Model back;
        REQUIRE(load(path, back));
        CHECK(back.pricing.empty());
    }

    SECTION("Export Bambu 3MF leaves it out, also after a reopen") {
        const fs::path      project = dir / "project.3mf";
        REQUIRE(store(project, model, cfg, PROJECT));
        Model reopened;
        REQUIRE(load(project, reopened));
        REQUIRE_FALSE(reopened.pricing.empty());

        for (Model *m : {&model, &reopened}) {
            BambuExport::Report report;
            const fs::path      path = dir / "bambu.3mf";
            REQUIRE(store(path, *m, cfg, PROJECT, &report));
            const std::string xml = zip_entry(path.string(), "3D/3dmodel.model");
            REQUIRE_FALSE(xml.empty());
            CHECK(xml.find("edgeslicer_pricing") == std::string::npos);
            Model back;
            REQUIRE(load(path, back));
            CHECK(back.pricing.empty());
        }
    }

    SECTION("a sliced-plate file leaves it out") {
        const fs::path path = dir / "plate.gcode.3mf";
        REQUIRE(store(path, model, cfg, PROJECT | SaveStrategy::WithGcode | SaveStrategy::SkipModel));
        const std::string xml = zip_entry(path.string(), "3D/3dmodel.model");
        REQUIRE_FALSE(xml.empty());
        CHECK(xml.find("edgeslicer_pricing") == std::string::npos);
    }

    SECTION("a backup keeps it (crash recovery reopens the project as it was)") {
        // A backup is restored through its own path, not load_bbs_3mf: check what it wrote.
        const fs::path path = dir / "backup.3mf";
        REQUIRE(store(path, model, cfg, PROJECT | SaveStrategy::Backup));
        const std::string xml = zip_entry(path.string(), "3D/3dmodel.model");
        REQUIRE_FALSE(xml.empty());
        CHECK(xml.find("edgeslicer_pricing") != std::string::npos);
    }

    fs::remove_all(dir);
}

TEST_CASE("Model copies carry the project pricing", "[CostPricing]")
{
    Model model;
    one_cube(model);
    model.pricing = some_project_pricing();
    Model copy(model);
    CHECK(copy.pricing == model.pricing);
    Model target;
    target.load_from(copy);
    CHECK(target.pricing == model.pricing);
}
