#include <catch2/catch_all.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

// Snapmaker Orca: the state the setup wizard leaves for a Snapmaker U1, without the window: fresh data
// dir, first-start load, GuideFrame::apply_config -> PresetBundle::apply_vendor_config, then the
// extruder count raised to the U1's (PresetBundle::on_extruders_count_changed).

namespace {

const char *const SNAPMAKER = "Snapmaker";
const char *const U1_MODEL  = "Snapmaker U1";

struct TempDir
{
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / fs::unique_path("orca-first-run-%%%%-%%%%")) { fs::create_directories(path); }
    ~TempDir() { boost::system::error_code ec; fs::remove_all(path, ec); }
};

// resources_dir() / data_dir() are process-wide: restored however the case leaves.
struct ScopedDirs
{
    std::string prev_data { data_dir() }, prev_rsrc { resources_dir() };
    ScopedDirs(const fs::path &data, const fs::path &rsrc) { set_data_dir(data.string()); set_resources_dir(rsrc.string()); }
    ~ScopedDirs() { set_data_dir(prev_data); set_resources_dir(prev_rsrc); }
};

std::string joined(const std::vector<std::string> &values)
{
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i)
        out << (i == 0 ? "" : ", ") << values[i];
    return out.str();
}

// The filament presets the wizard's filament page offers, by name: every system filament of the
// filament library and of the Snapmaker vendor. Ticking "All" sends every one of them.
std::map<std::string, std::string> every_filament()
{
    std::map<std::string, std::string> out;
    for (const char *vendor : { PresetBundle::ORCA_FILAMENT_LIBRARY, SNAPMAKER }) {
        PresetBundle catalogue;
        catalogue.load_vendor_configs_from_json(PROFILES_DIR, vendor, PresetBundle::LoadSystem,
                                                ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr, /*allow_cache=*/false);
        for (const Preset &preset : catalogue.filaments)
            if (preset.is_system && !preset.is_default)
                out[preset.name] = "true";
    }
    return out;
}

// The model entry of the U1 in the shipped vendor profile.
VendorProfile::PrinterModel u1_model()
{
    PresetBundle catalogue;
    catalogue.load_vendor_configs_from_json(PROFILES_DIR, SNAPMAKER, PresetBundle::LoadSystem,
                                            ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr, /*allow_cache=*/false);
    const auto vendor = catalogue.vendors.find(SNAPMAKER);
    REQUIRE(vendor != catalogue.vendors.end());
    for (const VendorProfile::PrinterModel &model : vendor->second.models)
        if (model.id == U1_MODEL)
            return model;
    FAIL("the Snapmaker vendor has no model " << U1_MODEL);
    return {};
}

// The default materials of the U1 model, as the wizard marks them when the model is ticked
// (GuideFrame::OnScriptMessage, "save_userguide_models").
std::map<std::string, std::string> default_materials()
{
    std::map<std::string, std::string> out;
    for (const std::string &name : u1_model().default_materials)
        out[name] = "true";
    return out;
}

// The variant the wizard selects among ticked U1 variants (PresetBundle::wizard_printer_variant).
std::string wizard_variant(const std::set<std::string> &ticked)
{
    const VendorProfile::PrinterModel model = u1_model();
    return PresetBundle::wizard_printer_variant(SNAPMAKER, &model, ticked);
}

// A model entry of another vendor whose variants are listed largest first (the Creality K1 SE
// lists 0.8;0.6;0.4). Mainline activates the first ticked variant in that order.
VendorProfile::PrinterModel largest_first_model()
{
    VendorProfile::PrinterModel model;
    model.id = "Other vendor largest first";
    for (const char *name : { "0.8", "0.6", "0.4" })
        model.variants.emplace_back(std::string(name));
    return model;
}

// A dirty option UnsavedChangesDialog::update_tree can list: any key that is not a string or a list
// of strings (the settings index is a GUI object, so this rule stands in for it for every key).
bool listable(const std::string &opt_key)
{
    const std::string pure = opt_key.substr(0, opt_key.find('#'));
    const ConfigOptionDef *def = print_config_def.get(pure);
    return def == nullptr || (def->type != coString && def->type != coStrings);
}

struct FirstRun
{
    TempDir      tmp;
    ScopedDirs   dirs { tmp.path / "data", fs::path(PROFILES_DIR).parent_path() };
    AppConfig    config;
    PresetBundle bundle;
    std::string  default_printer_filament;

    FirstRun()
    {
        fs::create_directories(tmp.path / "data");
        bundle.setup_directories();
        // PresetUpdater installs the filament library and the Snapmaker vendor before the first window.
        REQUIRE(install_vendor_bundles_from_resources({ PresetBundle::ORCA_FILAMENT_LIBRARY, SNAPMAKER }));
        bundle.nozzle_filament_enabled = true; // GUI_App: AppConfig "filament_follows_nozzle", on by default
        bundle.load_presets(config, ForwardCompatibilitySubstitutionRule::EnableSystemSilent);
        REQUIRE(bundle.printers.only_default_printers());
        // GuideFrame::run() exports the selections before the wizard opens.
        bundle.export_selections(config);
        default_printer_filament = bundle.filament_presets.empty() ? std::string() : bundle.filament_presets.front();
    }

    // Finish of the wizard with the U1 ticked (every nozzle size) and the filaments `filaments`.
    void finish(const std::map<std::string, std::string> &filaments, const std::string &variant)
    {
        const std::set<std::string> sizes { "0.2", "0.4", "0.6", "0.8" };
        const std::map<std::string, std::map<std::string, std::set<std::string>>> vendors { { SNAPMAKER, { { U1_MODEL, sizes } } } };
        REQUIRE(bundle.apply_vendor_config(vendors, filaments, &config, true, U1_MODEL, variant));
        // TabPrinter::on_preset_loaded: the tab still counts the one extruder of "Default Printer".
        const auto *nozzles = bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
        REQUIRE(nozzles != nullptr);
        if (nozzles->size() != 1)
            bundle.on_extruders_count_changed(int(nozzles->size()));
    }

    std::string report()
    {
        std::ostringstream out;
        out << "filament of Default Printer before the wizard: \"" << default_printer_filament << "\"\n"
            << "printer: \"" << bundle.printers.get_edited_preset().name << "\"\n"
            << "process: \"" << bundle.prints.get_edited_preset().name << "\"\n"
            << "filament slots (" << bundle.filament_presets.size() << "): " << joined(bundle.filament_presets) << "\n"
            << "printer dirty options: " << joined(bundle.printers.current_dirty_options(true));
        return out.str();
    }

    // What the first run must leave: four slots, one material, the U1's PLA, all installed and
    // compatible; a printer preset without changes, or one whose changes the dialog can show.
    void check_first_run_state()
    {
        INFO(report());
        REQUIRE(bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->size() == 4);

        CHECK(bundle.filament_presets.size() == 4);
        const Preset *first = bundle.filaments.find_preset(bundle.filament_presets.front(), false);
        REQUIRE(first != nullptr);
        CHECK(first->config.opt_string("filament_type", 0u) == "PLA");
        for (size_t slot = 0; slot < bundle.filament_presets.size(); ++slot) {
            INFO("slot " << slot + 1 << ": " << bundle.filament_presets[slot]);
            const Preset *preset = bundle.filaments.find_preset(bundle.filament_presets[slot], false);
            REQUIRE(preset != nullptr);
            CHECK(preset->alias == first->alias);
            CHECK(preset->is_visible);
            CHECK(preset->is_compatible);
        }

        CHECK_FALSE(bundle.printers.current_is_dirty());
        if (bundle.printers.current_is_dirty()) {
            const std::vector<std::string> dirty = bundle.printers.current_dirty_options(true);
            CHECK(std::any_of(dirty.begin(), dirty.end(), listable));
        }
    }
};

} // namespace

TEST_CASE("The setup wizard selects the 0.4 mm variant of the U1", "[FirstRun][Preset][fr1_wizard_variant]")
{
    // The wizard ticks every nozzle size of a ticked model. The U1 ships with a 0.4 mm nozzle,
    // the mixed nozzle sizes of the sidebar start from the 0.4 mm preset, and only that size has
    // High Flow data.
    CHECK(wizard_variant({ "0.2", "0.4", "0.6", "0.8" }) == "0.4");
    // Without the default size: the first ticked variant in the order of the model entry.
    CHECK(wizard_variant({ "0.8", "0.2" }) == "0.2");
    CHECK(wizard_variant({ "0.6" }) == "0.6");
    CHECK(wizard_variant({}).empty());
    // Another vendor keeps mainline's choice, the first ticked variant in model order, even when
    // 0.4 is ticked: 81 of mainline's model entries list another size first while offering 0.4.
    const VendorProfile::PrinterModel other = largest_first_model();
    CHECK(PresetBundle::wizard_printer_variant("Creality", &other, { "0.4", "0.6", "0.8" }) == "0.8");
    CHECK(PresetBundle::wizard_printer_variant("Creality", &other, { "0.4", "0.6" }) == "0.6");
    CHECK(PresetBundle::wizard_printer_variant("Creality", &other, { "0.4" }) == "0.4");
    // The same entry under the Snapmaker bundle: the default size wins.
    CHECK(PresetBundle::wizard_printer_variant(SNAPMAKER, &other, { "0.4", "0.6", "0.8" }) == "0.4");
    // A vendor without model entries (mainline's fallback): the default size, else the first ticked name.
    CHECK(PresetBundle::wizard_printer_variant("Creality", nullptr, { "0.6", "0.4" }) == "0.4");
    CHECK(PresetBundle::wizard_printer_variant("Creality", nullptr, { "0.6", "0.8" }) == "0.6");
    CHECK(PresetBundle::wizard_printer_variant(SNAPMAKER, nullptr, { "0.6", "0.8" }) == "0.6");
}

TEST_CASE("After the setup wizard the U1 has four filament slots of one PLA and a clean printer preset", "[FirstRun][Preset][fr1_first_run]")
{
    FirstRun run;
    const std::string variant = wizard_variant({ "0.2", "0.4", "0.6", "0.8" });

    SECTION("every filament ticked, the variant the wizard selects") {
        run.finish(every_filament(), variant);
        run.check_first_run_state();
    }
    SECTION("the default materials of the model ticked, the variant the wizard selects") {
        run.finish(default_materials(), variant);
        run.check_first_run_state();
    }
    SECTION("every filament ticked, the 0.4 mm variant") {
        run.finish(every_filament(), "0.4");
        CHECK(run.bundle.printers.get_edited_preset().name == "Snapmaker U1 (0.4 nozzle)");
        run.check_first_run_state();
    }
}

TEST_CASE("Selecting a U1 printer preset without an edit leaves it clean", "[FirstRun][Preset][fr1_clean_selection]")
{
    auto bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(PROFILES_DIR, SNAPMAKER, PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr, /*allow_cache=*/false);
    const std::string name = GENERATE(std::string("Snapmaker U1 (0.2 nozzle)"), std::string("Snapmaker U1 (0.4 nozzle)"),
                                      std::string("Snapmaker U1 (0.6 nozzle)"), std::string("Snapmaker U1 (0.8 nozzle)"));
    REQUIRE(bundle->printers.select_preset_by_name(name, true));
    bundle->update_compatible(PresetSelectCompatibleType::Always);
    INFO(name);

    SECTION("the Printer tab resizes its extruder pages from the one extruder of the previous printer") {
        bundle->on_extruders_count_changed(4);
        INFO("dirty options: " << joined(bundle->printers.current_dirty_options(true)));
        CHECK_FALSE(bundle->printers.current_is_dirty());
    }
    SECTION("a sidebar edit of tool head 1 shows in the unsaved changes dialog") {
        bundle->on_extruders_count_changed(4);
        DynamicPrintConfig &edited = bundle->printers.get_edited_preset().config;
        auto *nozzles = edited.option<ConfigOptionFloats>("nozzle_diameter");
        REQUIRE(nozzles != nullptr);
        nozzles->values[0] = nozzles->values[0] > 0.3 ? 0.2 : 0.4;
        bundle->printers.update_dirty();
        const std::vector<std::string> dirty = bundle->printers.current_dirty_options(true);
        INFO("dirty options: " << joined(dirty));
        CHECK(bundle->printers.current_is_dirty());
        CHECK(std::find(dirty.begin(), dirty.end(), "nozzle_diameter#0") != dirty.end());
        CHECK(std::any_of(dirty.begin(), dirty.end(), listable));
    }
}
