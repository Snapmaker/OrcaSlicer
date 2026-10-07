// Your own filament prices (PR 1b): the family key over the shipped presets, precedence including a
// user preset's deliberate price, the store file and the slice-funnel rewrite of filament_cost,
// and sliced-plate 3MFs that leave the prices out.

#include <catch2/catch.hpp>

#include "libslic3r/FilamentPrices.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>

using namespace Slic3r;
using namespace Slic3r::FilamentPrices;
using Catch::Matchers::WithinAbs;
using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

fs::path scratch_dir(const std::string &name)
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("snorca_prices_" + name + "_%%%%-%%%%");
    fs::create_directories(dir);
    return dir;
}

std::string read_text(const fs::path &path)
{
    boost::nowide::ifstream in(path.string(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_text(const fs::path &path, const std::string &text)
{
    boost::nowide::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
    out << text;
}

// The shipped BBL vendor profiles plus Orca's filament library, loaded the way the application
// loads system presets (never from the user's data directory).
PresetBundle &shipped_bundle()
{
    static std::unique_ptr<PresetBundle> bundle;
    static std::unique_ptr<PresetBundle> library;
    if (bundle)
        return *bundle;
    const std::string saved_data_dir = data_dir();
    set_data_dir(scratch_dir("datadir").string());
    const std::string profiles = (fs::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    library = std::make_unique<PresetBundle>();
    library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                           ForwardCompatibilitySubstitutionRule::EnableSilent);
    bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                          library.get());
    set_data_dir(saved_data_dir);
    return *bundle;
}

double preset_price(const Preset &p)
{
    const auto *opt = p.config.option<ConfigOptionFloats>("filament_cost");
    return opt != nullptr && !opt->values.empty() ? opt->get_at(0) : 0.;
}

// Every system filament preset whose name starts with `prefix` and continues with " @".
std::vector<const Preset *> system_variants(const PresetCollection &filaments, const std::string &prefix)
{
    std::vector<const Preset *> out;
    for (const Preset &p : filaments)
        if (p.is_system && p.name.rfind(prefix + " @", 0) == 0)
            out.push_back(&p);
    return out;
}

// A user preset derived from `parent` the way "Save as" makes one: the parent's config, its name as
// inherits, and the given price.
void add_user_preset(PresetCollection &filaments, const std::string &name, const Preset &parent, double price)
{
    DynamicPrintConfig config = parent.config;
    config.set_key_value("inherits", new ConfigOptionString(parent.name));
    config.option<ConfigOptionFloats>("filament_cost", true)->values = {price};
    filaments.load_preset((scratch_dir("user") / (name + ".json")).string(), name, config, false);
}

// The per-slot keys of PresetBundle::full_fff_config() that apply() reads, plus one unrelated key.
DynamicPrintConfig slots_config(const std::vector<std::string> &names, const std::vector<std::string> &vendors,
                                const std::vector<std::string> &types, const std::vector<double> &costs,
                                const std::vector<std::string> &inherits_filaments = {})
{
    DynamicPrintConfig config;
    config.set_key_value("filament_settings_id", new ConfigOptionStrings(names));
    config.set_key_value("filament_vendor", new ConfigOptionStrings(vendors));
    config.set_key_value("filament_type", new ConfigOptionStrings(types));
    config.set_key_value("filament_cost", new ConfigOptionFloats(costs));
    config.set_key_value("filament_density", new ConfigOptionFloats(std::vector<double>(names.size(), 1.24)));
    if (!inherits_filaments.empty()) {
        std::vector<std::string> group{""};   // print
        group.insert(group.end(), inherits_filaments.begin(), inherits_filaments.end());
        group.push_back("");                  // printer
        config.set_key_value("inherits_group", new ConfigOptionStrings(group));
    }
    return config;
}

std::vector<double> costs_of(const DynamicPrintConfig &config) { return config.option<ConfigOptionFloats>("filament_cost")->values; }

} // namespace

// ------------------------------------------------------------------------------------------- key

TEST_CASE("Family name drops the printer and nozzle part of a preset name", "[FilamentPrices]")
{
    CHECK(family_name("Bambu PLA Basic @BBL X1C") == "Bambu PLA Basic");
    CHECK(family_name("Bambu PLA Basic @BBL A1 0.2 nozzle") == "Bambu PLA Basic");
    CHECK(family_name("Generic PLA @System") == "Generic PLA");
    CHECK(family_name("Afinia ABS+@HS") == "Afinia ABS+");
    CHECK(family_name("Anker Generic ABS 0.2 nozzle") == "Anker Generic ABS");
    CHECK(family_name("Some PETG (0.6 nozzle)") == "Some PETG");
    CHECK(family_name("Some PETG 0.4mm nozzle") == "Some PETG");
    CHECK(family_name("  My   PLA  ") == "My PLA");
    // A name that is only a nozzle is kept rather than emptied.
    CHECK(family_name("0.4 nozzle") == "0.4 nozzle");
    // Case and spacing do not split a family; type is part of the key.
    CHECK(family_key("Bambu Lab", "PLA", "Bambu PLA Basic") == family_key(" bambu  lab", "pla", "BAMBU PLA BASIC"));
    CHECK(family_key("Bambu Lab", "PLA", "Bambu PLA Basic") != family_key("Bambu Lab", "PLA-CF", "Bambu PLA Basic"));
}

TEST_CASE("Shipped presets: every printer variant of a filament shares one key", "[FilamentPrices][profiles]")
{
    const PresetCollection &filaments = shipped_bundle().filaments;

    auto keys_of = [&filaments](const std::string &prefix, size_t &count) {
        std::set<std::string> keys;
        const auto            variants = system_variants(filaments, prefix);
        count                          = variants.size();
        for (const Preset *p : variants) {
            const Identity id = identify(*p, preset_price(*p), &filaments);
            INFO(p->name << " -> " << id.key());
            CHECK_FALSE(id.own_price);
            keys.insert(id.key());
        }
        return keys;
    };

    size_t     n_basic = 0, n_cf = 0, n_polymaker = 0, n_silk = 0;
    const auto basic     = keys_of("Bambu PLA Basic", n_basic);
    const auto cf        = keys_of("Bambu PLA-CF", n_cf);
    const auto polymaker = keys_of("Polymaker PLA", n_polymaker);
    const auto silk      = keys_of("Bambu PLA Silk", n_silk);

    // Many printers / nozzles each, one key each.
    CHECK(n_basic >= 10);
    CHECK(n_cf >= 5);
    CHECK(n_polymaker >= 3);
    CHECK(n_silk >= 3);
    REQUIRE(basic.size() == 1);
    REQUIRE(cf.size() == 1);
    REQUIRE(polymaker.size() == 1);
    REQUIRE(silk.size() == 1);
    CHECK(*basic.begin() == "bambu lab|PLA|bambu pla basic");
    CHECK(*cf.begin() == "bambu lab|PLA-CF|bambu pla-cf");

    // PLA vs PLA-CF, another family of the same vendor and type, another vendor: all different.
    const std::set<std::string> all{*basic.begin(), *cf.begin(), *polymaker.begin(), *silk.begin()};
    CHECK(all.size() == 4);

    // No visible shipped filament preset lacks a family.
    size_t without_family = 0;
    for (const Preset &p : filaments)
        if (p.is_system && identify(p, preset_price(p), &filaments).family.empty())
            ++without_family;
    CHECK(without_family == 0);
}

// ------------------------------------------------------------------------------------ precedence

TEST_CASE("Precedence: this preset only > deliberate user price > family > preset price", "[FilamentPrices]")
{
    Identity id;
    id.preset = "Bambu PLA Basic @BBL X1C";
    id.vendor = "Bambu Lab";
    id.type   = "PLA";
    id.family = "Bambu PLA Basic";

    Store store;
    SECTION("nothing of yours: the preset's price")
    {
        const Resolved r = store.resolve(id, 24.99);
        CHECK(r.source == Source::Preset);
        CHECK_THAT(r.price, WithinAbs(24.99, 1e-9));
        CHECK_FALSE(r.yours());
    }
    SECTION("family price beats the preset's")
    {
        REQUIRE(store.set_family("Bambu Lab", "PLA", "Bambu PLA Basic", 21.5));
        const Resolved r = store.resolve(id, 24.99);
        CHECK(r.source == Source::Family);
        CHECK_THAT(r.price, WithinAbs(21.5, 1e-9));
        CHECK_THAT(r.preset_price, WithinAbs(24.99, 1e-9));
        CHECK(r.yours());
    }
    SECTION("a price of 0 is a price, not 'none'")
    {
        REQUIRE(store.set_family("Bambu Lab", "PLA", "Bambu PLA Basic", 0.));
        const Resolved r = store.resolve(id, 24.99);
        CHECK(r.source == Source::Family);
        CHECK_THAT(r.price, WithinAbs(0., 1e-12));
    }
    SECTION("this preset only beats the family price")
    {
        REQUIRE(store.set_family("Bambu Lab", "PLA", "Bambu PLA Basic", 21.5));
        REQUIRE(store.set_preset(id.preset, id, 19.));
        const Resolved r = store.resolve(id, 24.99);
        CHECK(r.source == Source::PresetOnly);
        CHECK_THAT(r.price, WithinAbs(19., 1e-9));
        // Another variant of the family still gets the family price.
        Identity other = id;
        other.preset   = "Bambu PLA Basic @BBL A1";
        CHECK(store.resolve(other, 24.99).source == Source::Family);
    }
    SECTION("a user preset priced deliberately keeps its price over the family's")
    {
        REQUIRE(store.set_family("Bambu Lab", "PLA", "Bambu PLA Basic", 21.5));
        Identity user  = id;
        user.preset    = "My PLA Basic";
        user.own_price = true;
        const Resolved r = store.resolve(user, 30.);
        CHECK(r.source == Source::OwnPrice);
        CHECK_THAT(r.price, WithinAbs(30., 1e-9));
        CHECK(r.family_shadowed);
        CHECK_THAT(r.family_price, WithinAbs(21.5, 1e-9));
        // ...but a price for that very preset is still yours to set.
        REQUIRE(store.set_preset("My PLA Basic", user, 27.));
        CHECK(store.resolve(user, 30.).source == Source::PresetOnly);
    }
    SECTION("different type or vendor: not the same family")
    {
        REQUIRE(store.set_family("Bambu Lab", "PLA", "Bambu PLA Basic", 21.5));
        Identity cf = id;
        cf.type     = "PLA-CF";
        CHECK(store.resolve(cf, 24.99).source == Source::Preset);
        Identity other_vendor = id;
        other_vendor.vendor   = "Polymaker";
        CHECK(store.resolve(other_vendor, 24.99).source == Source::Preset);
    }
    SECTION("negative and non-finite prices are refused")
    {
        CHECK_FALSE(store.set_family("Bambu Lab", "PLA", "Bambu PLA Basic", -1.));
        CHECK_FALSE(store.set_preset(id.preset, id, std::numeric_limits<double>::quiet_NaN()));
        CHECK(store.empty());
    }
}

TEST_CASE("A user preset priced deliberately = its price differs from its parent's", "[FilamentPrices][profiles]")
{
    PresetCollection &filaments = shipped_bundle().filaments;
    const auto        variants  = system_variants(filaments, "Bambu PLA Basic");
    REQUIRE_FALSE(variants.empty());
    const Preset     &parent       = *variants.front();
    const std::string parent_name  = parent.name;
    const double      parent_price = preset_price(parent);
    const std::string basic_key    = identify(parent, parent_price, &filaments).key();

    // Both added first: inserting into the collection moves its presets, so look them up afterwards.
    add_user_preset(filaments, "Price test PLA same", *filaments.find_preset(parent_name, false), parent_price);
    add_user_preset(filaments, "Price test PLA own", *filaments.find_preset(parent_name, false), parent_price + 7.);
    const Preset &same_price = *filaments.find_preset("Price test PLA same", false);
    const Preset &own        = *filaments.find_preset("Price test PLA own", false);
    REQUIRE_FALSE(same_price.is_system);
    REQUIRE(same_price.inherits() == parent_name);

    const Identity same = identify(same_price, parent_price, &filaments);
    CHECK_FALSE(same.own_price);
    CHECK(same.key() == basic_key);   // a user preset is in its parent's family
    CHECK(same.family == "Bambu PLA Basic");

    const Identity mine = identify(own, parent_price + 7., &filaments);
    CHECK(mine.own_price);
    CHECK(mine.key() == basic_key);
    CHECK_THAT(mine.parent_price, WithinAbs(parent_price, 1e-9));

    // An unsaved edit of the same-priced preset is deliberate too: the slot price is what counts.
    CHECK(identify(same_price, parent_price + 1., &filaments).own_price);

    Store store;
    REQUIRE(store.set_family("Bambu Lab", "PLA", "Bambu PLA Basic", 12.));
    CHECK(store.resolve(same, parent_price).source == Source::Family);
    CHECK(store.resolve(mine, parent_price + 7.).source == Source::OwnPrice);

    // Through the funnel: the parent and the same-priced user preset take the family price, the
    // deliberately priced one keeps its own.
    DynamicPrintConfig config = slots_config({parent_name, "Price test PLA same", "Price test PLA own"},
                                             {"Bambu Lab", "Bambu Lab", "Bambu Lab"}, {"PLA", "PLA", "PLA"},
                                             {parent_price, parent_price, parent_price + 7.});
    apply(config, store, &filaments);
    CHECK(costs_of(config) == std::vector<double>{12., 12., parent_price + 7.});
}

// ----------------------------------------------------------------------------------------- funnel

TEST_CASE("apply() rewrites only filament_cost, per slot, idempotently", "[FilamentPrices]")
{
    Store store;
    REQUIRE(store.set_family("Bambu Lab", "PLA", "Bambu PLA Basic", 20.));
    REQUIRE(store.set_family("Polymaker", "PLA", "Polymaker PLA", 33.));

    // No preset collection: identities come from the config (a project whose presets are not installed).
    DynamicPrintConfig config = slots_config({"Bambu PLA Basic @BBL X1C", "Bambu PLA-CF @BBL X1C", "My Polymaker"},
                                             {"Bambu Lab", "Bambu Lab", "Polymaker"}, {"PLA", "PLA-CF", "PLA"}, {24.99, 34.99, 25.},
                                             {"", "", "Polymaker PLA @BBL X1C"});
    const DynamicPrintConfig before = config;
    const auto slots = apply(config, store, nullptr);
    REQUIRE(slots.size() == 3);
    CHECK(slots[0].source == Source::Family);
    CHECK(slots[1].source == Source::Preset);   // PLA-CF is its own family
    CHECK(slots[2].source == Source::Family);   // named after its parent (inherits_group)
    CHECK(costs_of(config) == std::vector<double>{20., 34.99, 33.});

    // Everything but filament_cost is untouched.
    for (const std::string &key : before.keys())
        if (key != "filament_cost") {
            INFO(key);
            CHECK(config.opt_serialize(key) == before.opt_serialize(key));
        }
    CHECK(config.keys() == before.keys());

    // Applying again (same store) changes nothing.
    DynamicPrintConfig again = config;
    apply(again, store, nullptr);
    CHECK(costs_of(again) == costs_of(config));

    // An empty store leaves the config as it was.
    DynamicPrintConfig untouched = before;
    apply(untouched, Store(), nullptr);
    CHECK(costs_of(untouched) == costs_of(before));
}

TEST_CASE("apply() never writes past a short filament_cost", "[FilamentPrices]")
{
    Store store;
    REQUIRE(store.set_family("Generic", "PLA", "Generic PLA", 15.));
    DynamicPrintConfig config = slots_config({"Generic PLA @System", "Generic PLA @System", "Generic PLA @System"},
                                             {"Generic", "Generic", "Generic"}, {"PLA", "PLA", "PLA"}, {20., 20.});
    const auto slots = apply(config, store, nullptr);
    CHECK(slots.size() == 2);
    CHECK(costs_of(config) == std::vector<double>{15., 15.});
}

// ------------------------------------------------------------------------------------------ store

TEST_CASE("Price store: round trip, unknown fields kept, versioned", "[FilamentPrices][store]")
{
    const fs::path dir  = scratch_dir("store");
    const fs::path path = dir / "cost" / "filament_overrides.json";

    Identity id;
    id.vendor = "Snapmaker";
    id.type   = "PLA";
    id.family = "Snapmaker PLA";
    Store store;
    REQUIRE(store.set_family("Bambu Lab", "PLA", "Bambu PLA Basic", 23.5));
    REQUIRE(store.set_preset("My PLA Silk @U1", id, 31.));
    REQUIRE(store.save(path.string()));
    CHECK(fs::exists(path));
    CHECK_FALSE(fs::exists(path.string() + ".tmp"));

    Store back;
    REQUIRE(back.load(path.string()));
    REQUIRE(back.entries().size() == 2);
    const Entry *family = back.find_family(family_key("Bambu Lab", "PLA", "Bambu PLA Basic"));
    REQUIRE(family != nullptr);
    CHECK_THAT(family->price_per_kg, WithinAbs(23.5, 1e-12));
    CHECK(family->updated > 0);
    const Entry *preset = back.find_preset("My PLA Silk @U1");
    REQUIRE(preset != nullptr);
    CHECK(preset->family == "Snapmaker PLA");
    CHECK_THAT(preset->price_per_kg, WithinAbs(31., 1e-12));

    // A later version's fields, at the top and in an entry, survive a rewrite by this one.
    json j = json::parse(read_text(path));
    CHECK(j["version"] == 1);
    j["version"]                   = 3;
    j["currency_note"]             = "set by a newer EdgeSlicer";
    j["filament"][0]["density"]    = 1.31;
    j["filament"][0]["spool_mass"] = {{"g", 250}};
    write_text(path, j.dump());

    Store newer;
    REQUIRE(newer.load(path.string()));
    REQUIRE(newer.set_family("Polymaker", "PLA", "Polymaker PLA", 29.));
    REQUIRE(newer.save(path.string()));
    const json k = json::parse(read_text(path));
    CHECK(k["version"] == 3);
    CHECK(k["currency_note"] == "set by a newer EdgeSlicer");
    CHECK(k["filament"][0]["density"] == 1.31);
    CHECK(k["filament"][0]["spool_mass"]["g"] == 250);
    CHECK(k["filament"].size() == 3);

    // Clearing works by key / name.
    CHECK(newer.clear_family(family_key("bambu lab", "pla", "bambu pla basic")));
    CHECK(newer.clear_preset("My PLA Silk @U1"));
    CHECK_FALSE(newer.clear_preset("My PLA Silk @U1"));
    CHECK(newer.entries().size() == 1);

    fs::remove_all(dir);
}

TEST_CASE("Price store: missing or unreadable files", "[FilamentPrices][store]")
{
    const fs::path dir = scratch_dir("store_bad");

    Store missing;
    CHECK(missing.load((dir / "nope.json").string()));
    CHECK(missing.empty());

    for (const std::string &garbage : {std::string("{\"version\":1,\"filament\":[{\"scope\""), std::string("[1,2,3]"),
                                       std::string("{\"filament\": 5}")}) {
        DYNAMIC_SECTION("garbage " << garbage.size()) {
            const fs::path bad = dir / "bad.json";
            write_text(bad, garbage);
            Store store;
            CHECK_FALSE(store.load(bad.string()));
            CHECK(store.empty());
            CHECK_FALSE(store.load_error().empty());
        }
    }

    // Entries that make no sense are skipped, the rest is read.
    const fs::path partial = dir / "partial.json";
    write_text(partial, R"({"version":1,"filament":[
        {"scope":"family","vendor":"A","type":"PLA","family":"A PLA","price_per_kg":-3},
        {"scope":"family","vendor":"A","type":"PLA","family":"A PLA","price_per_kg":"cheap"},
        {"scope":"preset","price_per_kg":10},
        {"scope":"spool","key":"x","price_per_kg":10},
        {"scope":"family","vendor":"A","type":"PETG","family":"A PETG","price_per_kg":18.5}]})");
    Store store;
    CHECK(store.load(partial.string()));
    REQUIRE(store.entries().size() == 1);
    CHECK(store.entries().front().family == "A PETG");

    fs::remove_all(dir);
}

TEST_CASE("Global store: test path, save, revision, unreadable file kept", "[FilamentPrices][store]")
{
    const fs::path dir  = scratch_dir("global");
    const fs::path path = dir / "filament_overrides.json";
    set_store_path(path.string());
    CHECK(store_path() == path.string());

    write_text(path, "not json at all");
    CHECK(global()->empty());

    Store store = *global();
    REQUIRE(store.set_family("Generic", "PLA", "Generic PLA", 17.));
    const unsigned before = revision();
    CHECK(save_global(store));
    CHECK(revision() == before + 1);
    CHECK(global()->find_family(family_key("Generic", "PLA", "Generic PLA")) != nullptr);
    // The unreadable file was kept next to the new one, not lost.
    CHECK(read_text(path.string() + ".unreadable") == "not json at all");

    // Another reader sees what was saved.
    reset_global();
    CHECK(global()->entries().size() == 1);

    set_store_path("");
    CHECK(store_path() != path.string());
    fs::remove_all(dir);
}

// ------------------------------------------------------------------------- sliced-plate 3MF

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

DynamicPrintConfig priced_project_config()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    for (const std::string &key : cfg.keys())
        if (const ConfigOption *opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    cfg.set_num_extruders(1);
    cfg.set_num_filaments(2);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values  = {0.4};
    cfg.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#00FF00"};
    cfg.option<ConfigOptionFloats>("filament_cost")->values    = {24.99, 31.5};
    cfg.set_key_value("filament_settings_id", new ConfigOptionStrings({"Bambu PLA Basic @BBL X1C", "Bambu PLA-CF @BBL X1C"}));
    cfg.set_key_value("different_settings_to_system",
                      new ConfigOptionStrings({"", "filament_cost;nozzle_temperature", "filament_cost", ""}));
    return cfg;
}

std::string store_plate(const fs::path &path, DynamicPrintConfig &cfg, bool strip, SaveStrategy strategy,
                        std::vector<Preset *> presets = {})
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "cube";
    object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    StoreParams sp;
    const std::string p    = path.string();
    sp.path                 = p.c_str();
    sp.model                = &model;
    sp.config               = &cfg;
    sp.project_presets      = presets;
    sp.strategy             = strategy;
    sp.strip_filament_prices = strip;
    REQUIRE(store_bbs_3mf(sp));
    REQUIRE(sp.config == &cfg);   // the caller's pointers are given back
    REQUIRE(sp.project_presets == presets);
    return zip_entry(p, "Metadata/project_settings.config");
}

} // namespace

TEST_CASE("Sliced-plate 3MF without filament prices: none in its settings", "[FilamentPrices][3MF]")
{
    const fs::path dir = scratch_dir("3mf");
    set_temporary_dir(dir.string());
    const SaveStrategy sliced = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary | SaveStrategy::WithGcode |
                                SaveStrategy::SkipModel;

    // An embedded user filament preset with a price of its own.
    Preset user(Preset::TYPE_FILAMENT, "My PLA");
    user.config = DynamicPrintConfig::full_print_config();
    user.config.option<ConfigOptionFloats>("filament_cost")->values = {42.};
    user.config.set_key_value("inherits", new ConfigOptionString("Bambu PLA Basic @BBL X1C"));
    std::vector<Preset *> presets{&user};

    SECTION("preference on: prices written, as before")
    {
        DynamicPrintConfig cfg  = priced_project_config();
        const json         proj = json::parse(store_plate(dir / "on.gcode.3mf", cfg, false, sliced, presets));
        REQUIRE(proj.contains("filament_cost"));
        CHECK(proj["filament_cost"] == json::array({"24.99", "31.5"}));
        CHECK(zip_entry((dir / "on.gcode.3mf").string(), "Metadata/filament_settings_1.config").find("filament_cost") != std::string::npos);
    }
    SECTION("preference off: no filament_cost anywhere in the archive's settings")
    {
        DynamicPrintConfig cfg   = priced_project_config();
        const std::string  text  = store_plate(dir / "off.gcode.3mf", cfg, true, sliced, presets);
        const json         proj  = json::parse(text);
        CHECK_FALSE(proj.contains("filament_cost"));
        CHECK(text.find("filament_cost") == std::string::npos);
        // Its mention in different_settings_to_system goes too, the rest stays.
        CHECK(proj["different_settings_to_system"] == json::array({"", "nozzle_temperature", "", ""}));
        CHECK(proj["filament_settings_id"] == json::array({"Bambu PLA Basic @BBL X1C", "Bambu PLA-CF @BBL X1C"}));
        CHECK(proj.contains("filament_density"));
        const std::string embedded = zip_entry((dir / "off.gcode.3mf").string(), "Metadata/filament_settings_1.config");
        CHECK_FALSE(embedded.empty());
        CHECK(embedded.find("filament_cost") == std::string::npos);
        // The caller's config and preset are not changed.
        CHECK(cfg.has("filament_cost"));
        CHECK(user.config.has("filament_cost"));

        // Reopening reads the file; filament_cost is simply absent (presets fill it in).
        DynamicPrintConfig        back;
        ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::EnableSilent};
        Model                     model;
        PlateDataPtrs             plates;
        std::vector<Preset *>     loaded_presets;
        bool                      is_bbl = false;
        Semver                    version;
        CHECK(load_bbs_3mf((dir / "off.gcode.3mf").string().c_str(), &back, &ctxt, &model, &plates, &loaded_presets, &is_bbl, &version,
                           nullptr, LoadStrategy::LoadConfig | LoadStrategy::Silence));
        // Absent, or at most a default: never the stripped prices.
        if (back.has("filament_cost"))
            CHECK(back.opt_serialize("filament_cost") != "24.99,31.5");
        CHECK(back.option<ConfigOptionStrings>("filament_settings_id")->values.size() == 2);
        release_PlateData_list(plates);
        for (Preset *p : loaded_presets)
            delete p;
    }
    SECTION("a project save keeps the preset prices")
    {
        DynamicPrintConfig cfg  = priced_project_config();
        const json         proj = json::parse(
            store_plate(dir / "project.3mf", cfg, false, SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary));
        CHECK(proj["filament_cost"] == json::array({"24.99", "31.5"}));
    }
    fs::remove_all(dir);
}

TEST_CASE("Project files keep preset prices while your prices apply to slicing", "[FilamentPrices][3MF]")
{
    // What the GUI does: the slice gets FilamentPrices::apply() on its own copy of the config; the
    // project writer gets full_config_secure(), the presets' values.
    const fs::path dir = scratch_dir("3mf_project");
    set_temporary_dir(dir.string());
    Store store;
    REQUIRE(store.set_family("Bambu Lab", "PLA", "Bambu PLA Basic", 11.));

    DynamicPrintConfig project = priced_project_config();
    project.option<ConfigOptionStrings>("filament_vendor", true)->values = {"Bambu Lab", "Bambu Lab"};
    project.option<ConfigOptionStrings>("filament_type", true)->values   = {"PLA", "PLA-CF"};
    DynamicPrintConfig for_slicing = project;
    apply(for_slicing, store, nullptr);
    CHECK(costs_of(for_slicing) == std::vector<double>{11., 31.5});

    const json proj = json::parse(
        store_plate(dir / "project.3mf", project, false, SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary));
    CHECK(proj["filament_cost"] == json::array({"24.99", "31.5"}));
    fs::remove_all(dir);
}

TEST_CASE("strip_prices() removes filament_cost and its 'different from system' mention", "[FilamentPrices]")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("filament_cost", new ConfigOptionFloats({1., 2.}));
    cfg.set_key_value("time_cost", new ConfigOptionFloat(3.));
    cfg.set_key_value("different_settings_to_system", new ConfigOptionStrings({"layer_height", "filament_cost", "a;filament_cost;b"}));
    strip_prices(cfg);
    CHECK_FALSE(cfg.has("filament_cost"));
    CHECK(cfg.has("time_cost"));   // the machine rate is not a filament price
    CHECK(cfg.option<ConfigOptionStrings>("different_settings_to_system")->values == std::vector<std::string>{"layer_height", "", "a;b"});
}
