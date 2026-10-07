// Project presets ("Save to project" in the save-preset dialog, listed under "Project-inside
// presets") live only inside the project 3MF: Metadata/process_settings_N.config,
// filament_settings_N.config and machine_settings_N.config. These tests run the whole cycle the
// GUI runs - save presets to the project, store the 3MF, start a fresh bundle (a new session),
// read the 3MF back the way Plater::load_files does, and save and re-open again - against the
// real BBL profiles in resources/profiles.
#include <catch2/catch.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>

using namespace Slic3r;

namespace {

const std::string X1C        = "Bambu Lab X1 Carbon 0.4 nozzle";
const std::string P1S        = "Bambu Lab P1S 0.4 nozzle";
const std::string H2D        = "Bambu Lab H2D 0.4 nozzle";
const std::string X1C_PROC   = "0.20mm Standard @BBL X1C";
const std::string X1C_PLA    = "Bambu PLA Basic @BBL X1C";
const std::string PROJ_PROC  = "Project process";
const std::string PROJ_PLA   = "Project PLA";

std::string scratch_dir()
{
    static std::string dir;
    if (dir.empty()) {
        const boost::filesystem::path p = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("snorca_projpresets_%%%%-%%%%");
        boost::filesystem::create_directories(p / "datadir");
        Slic3r::set_temporary_dir(p.string());
        dir = p.string();
    }
    return dir;
}

// A fresh bundle with the BBL system presets: what a new session (or another PC) starts with.
std::unique_ptr<PresetBundle> bbl_bundle()
{
    static std::unique_ptr<PresetBundle> library;
    const std::string saved_data_dir = data_dir();
    set_data_dir((boost::filesystem::path(scratch_dir()) / "datadir").string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    if (!library) {
        library = std::make_unique<PresetBundle>();
        library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                               ForwardCompatibilitySubstitutionRule::EnableSilent);
    }
    auto bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                          library.get());
    set_data_dir(saved_data_dir);
    return bundle;
}

// Printer + process + two filament slots, the way the sidebar holds them.
void select_machine(PresetBundle &bundle, const std::string &printer, const std::string &process, const std::string &filament)
{
    REQUIRE(bundle.printers.select_preset_by_name(printer, true));
    bundle.update_compatible(PresetSelectCompatibleType::Always);
    REQUIRE(bundle.prints.select_preset_by_name(process, true));
    REQUIRE(bundle.filaments.select_preset_by_name(filament, true));
    bundle.set_num_filaments(2, std::vector<std::string>{ "#E01919", "#1943E0" });
    bundle.filament_presets = { filament, filament };
    bundle.update_compatible(PresetSelectCompatibleType::Never);
}

// What Tab::save_preset does after "Save to project": the edited preset becomes a project preset.
// `printers` is the Save Preset dialog's "Use for every printer" (ticked: EveryPrinter, not ticked:
// FollowParent; Keep when the dialog is not involved).
void save_to_project(PresetBundle &bundle, PresetCollection &presets, const std::string &name,
                     ProjectPresetPrinters printers = ProjectPresetPrinters::Keep)
{
    const Preset *printer = presets.type() == Preset::TYPE_FILAMENT ? &bundle.printers.get_selected_preset_base() : nullptr;
    presets.save_current_preset(name, false, /* save_to_project */ true, nullptr, printer, printers);
    bundle.update_compatible(PresetSelectCompatibleType::Never);
}

std::string project_path(const std::string &name) { return (boost::filesystem::path(scratch_dir()) / name).string(); }

// Plater::export_3mf: the full config plus every project preset (stored as a difference to its parent).
void store_project(PresetBundle &bundle, const std::string &path)
{
    Model model;
    ModelObject *object = model.add_object();
    object->name        = "cube";
    object->add_volume(make_cube(20., 20., 20.))->name = "cube";
    object->add_instance()->set_offset({ 100., 100., 0. });
    object->ensure_on_bed();

    PlateData *plate   = new PlateData();
    plate->plate_index = 0;
    plate->objects_and_instances.emplace_back(0, 0);

    DynamicPrintConfig cfg = bundle.full_config_secure();
    std::vector<Preset *> presets = bundle.get_current_project_embedded_presets();

    StoreParams sp;
    sp.path            = path.c_str();
    sp.model           = &model;
    sp.config          = &cfg;
    sp.plate_data_list = { plate };
    sp.project_presets = presets;
    sp.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    const bool ok = store_bbs_3mf(sp);
    for (Preset *p : presets) delete p;
    release_PlateData_list(sp.plate_data_list);
    REQUIRE(ok);
}

// Plater::priv::load_files for a project: embedded presets first, then the project config.
// Returns how many project presets the 3MF carried.
size_t open_project(PresetBundle &bundle, const std::string &path)
{
    Model                     model;
    DynamicPrintConfig        config_loaded;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plates;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence;
    REQUIRE(load_bbs_3mf(path.c_str(), &config_loaded, &ctxt, &model, &plates, &project_presets, &is_bbl_3mf, &file_version, nullptr, strategy));
    release_PlateData_list(plates);

    const size_t count = project_presets.size();
    bundle.load_project_embedded_presets(project_presets, ForwardCompatibilitySubstitutionRule::Enable);
    for (Preset *p : project_presets) delete p;

    DynamicPrintConfig config;
    config.apply(static_cast<const ConfigBase &>(FullPrintConfig::defaults()));
    config += std::move(config_loaded);
    Preset::normalize(config);
    bundle.load_config_model(path, std::move(config), file_version);
    return count;
}

// Switching printers in the sidebar / printer tab.
void switch_printer(PresetBundle &bundle, const std::string &printer)
{
    REQUIRE(bundle.printers.select_preset_by_name(printer, true));
    bundle.update_compatible(PresetSelectCompatibleType::Always);
}

const Preset *find(PresetCollection &presets, const std::string &name) { return presets.find_preset(name, false); }

// The project presets are in the bundle, marked as such, and the combos would list them.
void check_project_presets_present(PresetBundle &bundle)
{
    const Preset *proc = find(bundle.prints, PROJ_PROC);
    REQUIRE(proc != nullptr);
    CHECK(proc->is_project_embedded);
    CHECK(proc->is_visible);
    CHECK(proc->config.opt_int("wall_loops") == 5);
    CHECK(proc->inherits() == X1C_PROC);

    const Preset *pla = find(bundle.filaments, PROJ_PLA);
    REQUIRE(pla != nullptr);
    CHECK(pla->is_project_embedded);
    CHECK(pla->is_visible);
    CHECK(pla->config.opt_int("nozzle_temperature", 0) == 231);
    CHECK(pla->inherits() == X1C_PLA);
}

// ... and the project re-opened onto them, unmodified.
void check_project_presets_selected(PresetBundle &bundle)
{
    CHECK(bundle.printers.get_selected_preset_name() == X1C);
    CHECK(bundle.prints.get_selected_preset_name() == PROJ_PROC);
    CHECK_FALSE(bundle.prints.get_edited_preset().is_dirty);
    CHECK(bundle.prints.get_edited_preset().config.opt_int("wall_loops") == 5);
    REQUIRE(bundle.filament_presets.size() == 2);
    CHECK(bundle.filament_presets[0] == PROJ_PLA);
    CHECK(bundle.filament_presets[1] == X1C_PLA);
    CHECK(find(bundle.filaments, PROJ_PLA)->is_compatible);
    CHECK(find(bundle.prints, PROJ_PROC)->is_compatible);
}

// Session 1: X1C, process and slot-1 filament changed and saved to the project, project stored.
std::string make_project(const std::string &file_name, ProjectPresetPrinters printers = ProjectPresetPrinters::Keep)
{
    auto bundle = bbl_bundle();
    select_machine(*bundle, X1C, X1C_PROC, X1C_PLA);

    bundle->prints.get_edited_preset().config.set_key_value("wall_loops", new ConfigOptionInt(5));
    bundle->prints.update_dirty();
    save_to_project(*bundle, bundle->prints, PROJ_PROC, printers);

    bundle->filaments.get_edited_preset().config.option<ConfigOptionInts>("nozzle_temperature")->values[0] = 231;
    bundle->filaments.update_dirty();
    save_to_project(*bundle, bundle->filaments, PROJ_PLA, printers);
    bundle->set_filament_preset(0, PROJ_PLA);

    REQUIRE(bundle->prints.get_selected_preset_name() == PROJ_PROC);
    REQUIRE(bundle->filament_presets[0] == PROJ_PLA);
    check_project_presets_present(*bundle);

    const std::string path = project_path(file_name);
    store_project(*bundle, path);
    return path;
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

bool fits_every_printer(const Preset *preset) { return preset != nullptr && Preset::fits_every_printer(preset->config); }

} // namespace

TEST_CASE("Project presets come back when the project is re-opened, again and again", "[Preset][Bundle][ProjectPreset]")
{
    const std::string first = make_project("roundtrip.3mf");

    // Session 2: a fresh start on the same printer.
    auto second = bbl_bundle();
    select_machine(*second, X1C, X1C_PROC, X1C_PLA);
    CHECK(open_project(*second, first) == 2);
    check_project_presets_present(*second);
    check_project_presets_selected(*second);

    // Saved again unchanged, and re-opened in session 3.
    const std::string again = project_path("roundtrip_again.3mf");
    store_project(*second, again);
    auto third = bbl_bundle();
    select_machine(*third, X1C, X1C_PROC, X1C_PLA);
    CHECK(open_project(*third, again) == 2);
    check_project_presets_present(*third);
    check_project_presets_selected(*third);
}

TEST_CASE("Project presets come back when the app last used another printer", "[Preset][Bundle][ProjectPreset]")
{
    const std::string path = make_project("other_printer.3mf");

    auto bundle = bbl_bundle();
    select_machine(*bundle, H2D, "0.20mm Standard @BBL H2D", "Bambu PLA Basic @BBL H2D");
    CHECK(open_project(*bundle, path) == 2);
    check_project_presets_present(*bundle);
    check_project_presets_selected(*bundle);
}

TEST_CASE("Project presets survive a printer switch and a save on that printer", "[Preset][Bundle][ProjectPreset]")
{
    const std::string path = make_project("switch.3mf");

    auto bundle = bbl_bundle();
    select_machine(*bundle, X1C, X1C_PROC, X1C_PLA);
    REQUIRE(open_project(*bundle, path) == 2);

    SECTION("A printer their parent profiles cover keeps them selected") {
        switch_printer(*bundle, P1S);
        CHECK(find(bundle->prints, PROJ_PROC)->is_compatible);
        CHECK(find(bundle->filaments, PROJ_PLA)->is_compatible);
        CHECK(bundle->prints.get_selected_preset_name() == PROJ_PROC);
        CHECK(bundle->filament_presets[0] == PROJ_PLA);
    }

    SECTION("Saved while another printer is active, they are still in the project") {
        switch_printer(*bundle, H2D);
        const std::string saved = project_path("switch_saved_on_h2d.3mf");
        store_project(*bundle, saved);

        auto next = bbl_bundle();
        select_machine(*next, X1C, X1C_PROC, X1C_PLA);
        CHECK(open_project(*next, saved) == 2);
        check_project_presets_present(*next);
        // Back on the printer they were made for, they can be picked again.
        switch_printer(*next, X1C);
        CHECK(find(next->prints, PROJ_PROC)->is_compatible);
        CHECK(find(next->filaments, PROJ_PLA)->is_compatible);
    }
}

// "Keep my printer" (keep_printer_on_open, on by default) switched a re-opened project to the
// last printer the user picked, and every project preset that printer could not use was
// deselected, hidden and replaced: the project's own settings were gone on each re-open.
// Plater now keeps the project's printer when this list is not empty.
TEST_CASE("Project presets a printer cannot use are reported before a Keep-my-printer switch", "[Preset][Bundle][ProjectPreset]")
{
    const std::string path = make_project("keep_printer.3mf");
    auto bundle = bbl_bundle();
    select_machine(*bundle, H2D, "0.20mm Standard @BBL H2D", "Bambu PLA Basic @BBL H2D");
    REQUIRE(open_project(*bundle, path) == 2);
    REQUIRE(bundle->printers.get_selected_preset_name() == X1C);

    // The parents cover the P1S: switching loses nothing, so Keep my printer may switch.
    CHECK(bundle->project_presets_lost_on_printer(P1S).empty());
    // The H2D can use neither: switching would drop both.
    const std::vector<std::string> lost = bundle->project_presets_lost_on_printer(H2D);
    CHECK(lost.size() == 2);
    CHECK(std::find(lost.begin(), lost.end(), PROJ_PROC) != lost.end());
    CHECK(std::find(lost.begin(), lost.end(), PROJ_PLA) != lost.end());
    // The project's own printer, or one that does not exist, loses nothing.
    CHECK(bundle->project_presets_lost_on_printer(X1C).empty());
    CHECK(bundle->project_presets_lost_on_printer("No such printer").empty());

    // Without the check this is what the switch did: both deselected and out of the combos.
    switch_printer(*bundle, H2D);
    CHECK(bundle->prints.get_selected_preset_name() != PROJ_PROC);
    CHECK_FALSE(find(bundle->prints, PROJ_PROC)->is_compatible);
    CHECK(bundle->filament_presets[0] != PROJ_PLA);
}

TEST_CASE("Presets the loader made up for a project do not hold Keep-my-printer back", "[Preset][Bundle][ProjectPreset]")
{
    auto bundle = bbl_bundle();
    select_machine(*bundle, X1C, X1C_PROC, X1C_PLA);
    // What load_external_preset() names a slot whose values matched no preset.
    bundle->filaments.get_edited_preset().config.option<ConfigOptionInts>("nozzle_temperature")->values[0] = 233;
    save_to_project(*bundle, bundle->filaments, X1C_PLA + "(downloaded.3mf)");
    bundle->set_filament_preset(0, X1C_PLA + "(downloaded.3mf)");
    REQUIRE(find(bundle->filaments, X1C_PLA + "(downloaded.3mf)")->is_project_embedded);

    CHECK(bundle->project_presets_lost_on_printer(H2D).empty());
}

// The Orca rule pins a filament saved from a system profile with an empty printer list to the
// current printer. A project preset is not pinned: it keeps its parent's list.
TEST_CASE("A filament saved to the project is not pinned to the printer it was saved on", "[Preset][Bundle][ProjectPreset]")
{
    auto bundle = bbl_bundle();
    select_machine(*bundle, X1C, X1C_PROC, X1C_PLA);
    // A parent that fits every printer, like the Orca filament library's.
    for (Preset *p : { &bundle->filaments.get_selected_preset(), &bundle->filaments.get_edited_preset() }) {
        p->config.option<ConfigOptionStrings>("compatible_printers", true)->values.clear();
        p->config.option<ConfigOptionString>("compatible_printers_condition", true)->value.clear();
    }
    bundle->filaments.get_edited_preset().config.option<ConfigOptionInts>("nozzle_temperature")->values[0] = 231;
    save_to_project(*bundle, bundle->filaments, PROJ_PLA);
    bundle->set_filament_preset(0, PROJ_PLA);

    const Preset *pla = find(bundle->filaments, PROJ_PLA);
    REQUIRE(pla != nullptr);
    CHECK(pla->config.option<ConfigOptionStrings>("compatible_printers")->values.empty());

    switch_printer(*bundle, H2D);
    CHECK(find(bundle->filaments, PROJ_PLA)->is_compatible);
    CHECK(bundle->filament_presets[0] == PROJ_PLA);
}

TEST_CASE("A printer saved to the project comes back and keeps its project on it", "[Preset][Bundle][ProjectPreset]")
{
    const std::string PROJ_X1C = "Project X1C";
    std::string path;
    {
        auto bundle = bbl_bundle();
        select_machine(*bundle, X1C, X1C_PROC, X1C_PLA);
        bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("retraction_length")->values[0] = 1.5;
        bundle->printers.update_dirty();
        save_to_project(*bundle, bundle->printers, PROJ_X1C);
        REQUIRE(bundle->printers.get_selected_preset_name() == PROJ_X1C);
        // The process and filament still fit: the project printer stands in for its parent.
        CHECK(bundle->prints.get_selected_preset().is_compatible);
        CHECK(bundle->filaments.find_preset(X1C_PLA, false)->is_compatible);
        path = project_path("project_printer.3mf");
        store_project(*bundle, path);
    }

    auto bundle = bbl_bundle();
    select_machine(*bundle, H2D, "0.20mm Standard @BBL H2D", "Bambu PLA Basic @BBL H2D");
    CHECK(open_project(*bundle, path) == 1);
    const Preset *printer = find(bundle->printers, PROJ_X1C);
    REQUIRE(printer != nullptr);
    CHECK(printer->is_project_embedded);
    CHECK(bundle->printers.get_selected_preset_name() == PROJ_X1C);
    CHECK(bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("retraction_length")->values[0] == Approx(1.5));
    CHECK(bundle->prints.get_selected_preset_name() == X1C_PROC);
    CHECK(bundle->filament_presets[0] == X1C_PLA);

    const std::vector<std::string> lost = bundle->project_presets_lost_on_printer(H2D);
    CHECK(std::find(lost.begin(), lost.end(), PROJ_X1C) != lost.end());
}

// The owner's choice on PR #357 (option B): "Use for every printer" next to "Preset Inside Project"
// in the Save Preset dialog. The project preset then has no printer list and no condition of its
// own, so it is listed for every printer and a printer switch keeps it selected. Not ticked, nothing
// changes.
TEST_CASE("A project preset saved for every printer stays selected across printer switches and re-opens", "[Preset][Bundle][ProjectPreset]")
{
    const std::string path = make_project("every_printer.3mf", ProjectPresetPrinters::EveryPrinter);

    // Stored as the preset's own empty list and condition: the shape a user preset set to "All"
    // printers has, which older builds and Bambu Studio read as "compatible with everything".
    const nlohmann::json proc = nlohmann::json::parse(zip_entry(path, "Metadata/process_settings_1.config"));
    CHECK(proc["name"] == PROJ_PROC);
    CHECK(proc["inherits"] == X1C_PROC);
    REQUIRE(proc.contains("compatible_printers"));
    CHECK(proc["compatible_printers"].empty());
    const nlohmann::json pla = nlohmann::json::parse(zip_entry(path, "Metadata/filament_settings_1.config"));
    CHECK(pla["name"] == PROJ_PLA);
    REQUIRE(pla.contains("compatible_printers"));
    CHECK(pla["compatible_printers"].empty());

    auto bundle = bbl_bundle();
    select_machine(*bundle, X1C, X1C_PROC, X1C_PLA);
    REQUIRE(open_project(*bundle, path) == 2);
    check_project_presets_present(*bundle);
    check_project_presets_selected(*bundle);
    CHECK(fits_every_printer(find(bundle->prints, PROJ_PROC)));
    CHECK(fits_every_printer(find(bundle->filaments, PROJ_PLA)));
    // Nothing for Keep my printer to protect either.
    CHECK(bundle->project_presets_lost_on_printer(H2D).empty());

    // A manual switch to a printer neither parent covers keeps both selected and listed.
    switch_printer(*bundle, H2D);
    CHECK(bundle->prints.get_selected_preset_name() == PROJ_PROC);
    CHECK(find(bundle->prints, PROJ_PROC)->is_compatible);
    CHECK(find(bundle->prints, PROJ_PROC)->is_visible);
    CHECK(bundle->prints.get_edited_preset().config.opt_int("wall_loops") == 5);
    CHECK(bundle->filament_presets[0] == PROJ_PLA);
    CHECK(find(bundle->filaments, PROJ_PLA)->is_compatible);
    // The second slot held an X1C system filament: that one is replaced as before.
    CHECK(bundle->filament_presets[1] != X1C_PLA);

    // Saved on the H2D and opened again: still for every printer, still selected.
    const std::string again = project_path("every_printer_on_h2d.3mf");
    store_project(*bundle, again);
    auto next = bbl_bundle();
    select_machine(*next, X1C, X1C_PROC, X1C_PLA);
    REQUIRE(open_project(*next, again) == 2);
    CHECK(next->printers.get_selected_preset_name() == H2D);
    CHECK(next->prints.get_selected_preset_name() == PROJ_PROC);
    CHECK(next->filament_presets[0] == PROJ_PLA);
    CHECK(fits_every_printer(find(next->prints, PROJ_PROC)));
    CHECK(fits_every_printer(find(next->filaments, PROJ_PLA)));
    switch_printer(*next, P1S);
    CHECK(next->prints.get_selected_preset_name() == PROJ_PROC);
    CHECK(next->filament_presets[0] == PROJ_PLA);
}

TEST_CASE("A project preset saved without Use-for-every-printer behaves as before", "[Preset][Bundle][ProjectPreset]")
{
    const std::string path = make_project("follow_parent.3mf", ProjectPresetPrinters::FollowParent);

    auto bundle = bbl_bundle();
    select_machine(*bundle, X1C, X1C_PROC, X1C_PLA);
    REQUIRE(open_project(*bundle, path) == 2);
    check_project_presets_selected(*bundle);
    // The parents' printer lists, as with the dialog's default.
    CHECK_FALSE(fits_every_printer(find(bundle->prints, PROJ_PROC)));
    CHECK_FALSE(fits_every_printer(find(bundle->filaments, PROJ_PLA)));

    switch_printer(*bundle, H2D);
    CHECK(bundle->prints.get_selected_preset_name() != PROJ_PROC);
    CHECK_FALSE(find(bundle->prints, PROJ_PROC)->is_compatible);
    CHECK(bundle->filament_presets[0] != PROJ_PLA);
    CHECK_FALSE(find(bundle->filaments, PROJ_PLA)->is_compatible);
}

TEST_CASE("Unticking Use-for-every-printer gives a project preset its parent's printers back", "[Preset][Bundle][ProjectPreset]")
{
    auto bundle = bbl_bundle();
    select_machine(*bundle, X1C, X1C_PROC, X1C_PLA);
    bundle->prints.get_edited_preset().config.set_key_value("wall_loops", new ConfigOptionInt(5));
    save_to_project(*bundle, bundle->prints, PROJ_PROC, ProjectPresetPrinters::EveryPrinter);
    REQUIRE(fits_every_printer(find(bundle->prints, PROJ_PROC)));

    // Saved again under the same name, box not ticked.
    save_to_project(*bundle, bundle->prints, PROJ_PROC, ProjectPresetPrinters::FollowParent);
    const Preset *proc = find(bundle->prints, PROJ_PROC);
    REQUIRE(proc != nullptr);
    CHECK(proc->config.option<ConfigOptionStrings>("compatible_printers")->values ==
          find(bundle->prints, X1C_PROC)->config.option<ConfigOptionStrings>("compatible_printers")->values);
    // Saved without the dialog (Keep), the preset's list is left alone.
    save_to_project(*bundle, bundle->prints, PROJ_PROC, ProjectPresetPrinters::Keep);
    CHECK_FALSE(fits_every_printer(find(bundle->prints, PROJ_PROC)));

    // Only project presets take the choice.
    Preset user = *find(bundle->prints, PROJ_PROC);
    user.is_project_embedded = false;
    bundle->prints.set_project_preset_printers(user, ProjectPresetPrinters::EveryPrinter);
    CHECK_FALSE(Preset::fits_every_printer(user.config));
}
