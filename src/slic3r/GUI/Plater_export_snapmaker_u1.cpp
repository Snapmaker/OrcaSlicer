///|/ Copyright (c) Snapmaker and Contributors
///|/ OrcaSlicer is released under the terms of the AGPLv3 or higher
///|/
// Plater_export_snapmaker_u1.cpp
//
// Drop this file into src/slic3r/GUI/ and add it to CMakeLists.txt.
// It provides Plater::export_snapmaker_u1() which is declared in Plater.hpp.
//
// The function:
//   1. Reads the currently-loaded model's filament info from the active config.
//   2. Shows ExportSnapmakerU1Dialog for user confirmation / remapping.
//   3. Builds a modified DynamicPrintConfig that targets the Snapmaker U1.
//   4. Calls store_bbs_3mf() to write the output .3mf.

#include "Plater.hpp"
#include "ExportSnapmakerU1.hpp"
#include "GUI_App.hpp"
#include "format.hpp"
#include "I18N.hpp"
#include "NotificationManager.hpp"

// libslic3r headers
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Model.hpp"

#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/app.h>
#include <wx/progdlg.h>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <string>
#include <vector>
#include <algorithm>
#include <sstream>

namespace Slic3r {
namespace GUI {

// ---------------------------------------------------------------------------
// Internal helpers (file scope)
// ---------------------------------------------------------------------------

namespace {

/// Extract per-filament colour strings ("#RRGGBB") from the active config.
std::vector<std::string> get_active_filament_colors(const DynamicPrintConfig& cfg)
{
    std::vector<std::string> result;
    const auto* opt = dynamic_cast<const ConfigOptionStrings*>(cfg.option("filament_colour"));
    if (!opt) return result;

    for (const std::string& raw : opt->values) {
        // OrcaSlicer stores colours as "#RRGGBBAA"; trim to "#RRGGBB"
        std::string c = raw;
        if (!c.empty() && c[0] == '#') {
            if (c.size() == 9) c = c.substr(0, 7); // strip alpha
        } else if (c.size() >= 6) {
            // No '#' prefix – add one
            c = "#" + c.substr(0, 6);
        }
        // Uppercase hex for consistency
        std::transform(c.begin(), c.end(), c.begin(), ::toupper);
        if (c.size() > 1) c[0] = '#'; // keep '#' lowercase
        result.push_back(c);
    }
    return result;
}

/// Extract per-filament type strings ("PLA", "PETG", …) from the active config.
std::vector<std::string> get_active_filament_types(const DynamicPrintConfig& cfg)
{
    std::vector<std::string> result;
    const auto* opt = dynamic_cast<const ConfigOptionStrings*>(cfg.option("filament_type"));
    if (!opt) return result;
    for (const std::string& t : opt->values)
        result.push_back(t);
    return result;
}

/// Build a "#RRGGBBAA" colour string expected by OrcaSlicer/bbs_3mf from "#RRGGBB".
static std::string to_bbs_color(const std::string& hex6)
{
    // Accepts "#RRGGBB" and appends "FF" alpha
    std::string h = hex6;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() == 6) h += "FF";
    // Ensure uppercase
    std::transform(h.begin(), h.end(), h.begin(), ::toupper);
    return "#" + h;
}

/// Pad a vector<string> to exactly `target_size` by repeating `default_val`.
static void pad_string_vector(std::vector<std::string>& v, size_t target_size,
                              const std::string& default_val)
{
    while (v.size() < target_size)
        v.push_back(default_val);
    if (v.size() > target_size)
        v.resize(target_size);
}

/// Progress callback compatible with Export3mfProgressFn (bool fn(int, const char*))
static bool progress_callback(int percent, const char* msg)
{
    // Yield to the wxApp event loop so the UI stays alive during export.
    // A progress dialog is shown by the caller below.
    wxYield();
    (void)percent; (void)msg;
    return true; // returning false would abort
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Plater::export_snapmaker_u1()
// ---------------------------------------------------------------------------

void Plater::export_snapmaker_u1()
{
    // ------------------------------------------------------------------
    // 0. Basic guards
    // ------------------------------------------------------------------
    if (!p || p->model.objects.empty()) {
        wxMessageBox(_L("Nothing to export – the build plate is empty."),
                     _L("Export for Snapmaker U1"), wxOK | wxICON_INFORMATION, q);
        return;
    }

    // ------------------------------------------------------------------
    // 1. Read current filament info from the active preset bundle
    // ------------------------------------------------------------------
    DynamicPrintConfig active_cfg = wxGetApp().preset_bundle->full_config_secure();

    std::vector<std::string> filament_types  = get_active_filament_types(active_cfg);
    std::vector<std::string> filament_colors = get_active_filament_colors(active_cfg);

    if (filament_types.empty()) {
        // Fallback: at least one PLA slot
        filament_types.push_back("PLA");
        filament_colors.push_back("#FFFFFF");
    }

    // ------------------------------------------------------------------
    // 2. Show the filament-mapping dialog
    // ------------------------------------------------------------------
    ExportSnapmakerU1Dialog dlg(q, filament_types, filament_colors);
    if (dlg.ShowModal() != wxID_OK)
        return;

    std::vector<U1FilamentMapping> mappings = dlg.get_mappings();
    if (mappings.empty()) {
        wxMessageBox(_L("No filaments were selected for export."),
                     _L("Export for Snapmaker U1"), wxOK | wxICON_WARNING, q);
        return;
    }

    // ------------------------------------------------------------------
    // 3. Ask for output file path
    // ------------------------------------------------------------------
    wxString default_name = wxT("Snapmaker_U1_Ready.3mf");
    wxFileDialog save_dlg(q,
        _L("Export for Snapmaker U1"),
        wxStandardPaths::Get().GetDocumentsDir(),
        default_name,
        wxT("3MF files (*.3mf)|*.3mf"),
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    if (save_dlg.ShowModal() == wxID_CANCEL)
        return;

    std::string output_path = save_dlg.GetPath().ToStdString();

    // ------------------------------------------------------------------
    // 4. Build modified DynamicPrintConfig targeting Snapmaker U1
    // ------------------------------------------------------------------
    DynamicPrintConfig u1_cfg = active_cfg; // start from active settings

    // 4a. Override the printer preset with the Snapmaker U1 settings.
    //     Look for it by printer_model first, then by preset name.
    PresetCollection& printer_presets = wxGetApp().preset_bundle->printers;
    const Preset* u1_printer_preset   = nullptr;

    for (const Preset& p : printer_presets) {
        if (p.config.has("printer_model")) {
            const auto* pm = dynamic_cast<const ConfigOptionString*>(
                p.config.option("printer_model"));
            if (pm && pm->value == SNAPMAKER_U1_PRINTER_MODEL) {
                u1_printer_preset = &p;
                break;
            }
        }
    }

    if (u1_printer_preset == nullptr) {
        // Preset not installed – warn but still export with current settings.
        wxMessageBox(
            wxString::Format(
                _L("The Snapmaker U1 printer profile (\"%s\") was not found in your "
                   "profile library.\n\nThe project will be exported using your current "
                   "printer settings.\nInstall the Snapmaker profiles via the "
                   "Printer/Filament wizard for full compatibility."),
                wxString::FromUTF8(SNAPMAKER_U1_PRINTER_MODEL)),
            _L("Profile Not Found"), wxOK | wxICON_WARNING, q);
    } else {
        u1_cfg.apply(u1_printer_preset->config);
    }

    // 4b. Build the remapped filament arrays (always 4 slots for U1).
    constexpr size_t TARGET_FILAMENTS = SNAPMAKER_U1_MAX_FILAMENTS;

    std::vector<std::string> new_colors;
    std::vector<std::string> new_types;
    std::vector<std::string> new_profiles;

    for (const U1FilamentMapping& m : mappings) {
        new_colors.push_back(to_bbs_color(m.color));
        new_types.push_back(m.filament_type);
        new_profiles.push_back(m.snapmaker_profile);
    }

    // Pad to exactly 4 with white PLA (matches bl2u1 behaviour)
    const std::string default_color   = "#FFFFFFFF";
    const std::string default_type    = "PLA";
    const std::string default_profile = get_u1_filament_preset("PLA");

    pad_string_vector(new_colors,   TARGET_FILAMENTS, default_color);
    pad_string_vector(new_types,    TARGET_FILAMENTS, default_type);
    pad_string_vector(new_profiles, TARGET_FILAMENTS, default_profile);

    // Apply new filament_colour
    {
        ConfigOptionStrings* opt = u1_cfg.option<ConfigOptionStrings>("filament_colour", true);
        opt->values = new_colors;
    }

    // Apply new filament_type
    {
        ConfigOptionStrings* opt = u1_cfg.option<ConfigOptionStrings>("filament_type", true);
        opt->values = new_types;
    }

    // Apply new filament_settings_id (Snapmaker U1 profiles)
    {
        ConfigOptionStrings* opt = u1_cfg.option<ConfigOptionStrings>("filament_settings_id", true);
        opt->values = new_profiles;
    }

    // Keep all other filament_* array options consistent in length
    auto extend_filament_array = [&](const std::string& key) {
        auto* opt = dynamic_cast<ConfigOptionStrings*>(u1_cfg.option(key));
        if (!opt || opt->values.empty()) return;
        while (opt->values.size() < TARGET_FILAMENTS)
            opt->values.push_back(opt->values.back());
        opt->values.resize(TARGET_FILAMENTS);
    };
    extend_filament_array("filament_id");
    extend_filament_array("filament_vendor");
    extend_filament_array("filament_is_support");

    // 4c. Map extruder assignments in the model objects to the new slot IDs.
    //     Build old_id -> new_id map.
    std::map<int, int> extruder_remap; // 1-based
    for (const U1FilamentMapping& m : mappings)
        extruder_remap[m.original_extruder_id] = m.new_extruder_id;

    // Clone the model so we don't mutate the live scene.
    Model export_model = p->model;

    for (ModelObject* obj : export_model.objects) {
        for (ModelVolume* vol : obj->volumes) {
            int old_ext = vol->config.option<ConfigOptionInt>("extruder")
                          ? vol->config.opt_int("extruder") : 1;
            auto it = extruder_remap.find(old_ext);
            if (it != extruder_remap.end()) {
                vol->config.set_key_value("extruder",
                    new ConfigOptionInt(it->second));
            }
        }
        // Also remap the object-level extruder override if set
        if (obj->config.has("extruder")) {
            int old_ext = obj->config.opt_int("extruder");
            auto it = extruder_remap.find(old_ext);
            if (it != extruder_remap.end())
                obj->config.set_key_value("extruder",
                    new ConfigOptionInt(it->second));
        }
    }

    // ------------------------------------------------------------------
    // 5. Collect plate data from the live plater (geometry unchanged)
    // ------------------------------------------------------------------
    PlateDataPtrs plate_data_list;
    p->partplate_list.store_plate_to_model_objects(export_model);
    for (int i = 0; i < p->partplate_list.get_plate_count(); ++i) {
        PartPlate* plate = p->partplate_list.get_plate(i);
        if (!plate) continue;
        PlateData* pd = new PlateData();
        plate->get_plate_data(*pd);
        plate_data_list.emplace_back(pd);
    }

    // ------------------------------------------------------------------
    // 6. Gather project presets (process + filament) to embed in the file
    // ------------------------------------------------------------------
    std::vector<Preset*> project_presets;
    {
        PresetBundle& pb = *wxGetApp().preset_bundle;
        // Embed current process preset
        if (Preset* pr = pb.prints.find_preset(pb.prints.get_selected_preset_name()))
            project_presets.push_back(pr);
        // Embed all filament presets that will be used
        for (const std::string& prof : new_profiles) {
            if (Preset* fp = pb.filaments.find_preset(prof))
                project_presets.push_back(fp);
        }
    }

    // ------------------------------------------------------------------
    // 7. Build StoreParams and write the 3MF
    // ------------------------------------------------------------------
    StoreParams store_params;
    store_params.path            = output_path.c_str();
    store_params.model           = &export_model;
    store_params.plate_data_list = plate_data_list;
    store_params.config          = &u1_cfg;
    store_params.project_presets = project_presets;
    store_params.strategy        = SaveStrategy::Zip64; // same as normal export
    store_params.proFn           = progress_callback;
    store_params.export_plate_idx = -1; // export all plates

    // Thumbnails: omit for now (export_3mf generates them from the GL canvas;
    // here we keep it simple and let the importer regenerate them).
    // Users can re-open the file in OrcaSlicer to regenerate a thumbnail.

    bool ok = false;
    try {
        ok = Slic3r::store_bbs_3mf(store_params);
    }
    catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << "export_snapmaker_u1: " << ex.what();
    }

    // ------------------------------------------------------------------
    // 8. Cleanup and notify user
    // ------------------------------------------------------------------
    for (PlateData* pd : plate_data_list)
        delete pd;

    if (ok) {
        wxString msg = wxString::Format(
            _L("Exported successfully:\n%s\n\n"
               "Open this file in OrcaSlicer or Snapmaker Luban to slice for the U1."),
            wxString::FromUTF8(output_path));
        wxMessageBox(msg, _L("Export for Snapmaker U1"), wxOK | wxICON_INFORMATION, q);

        // Show a persistent notification in the status bar as well
        if (p->notification_manager)
            p->notification_manager->push_notification(
                NotificationType::CustomNotification,
                NotificationManager::NotificationLevel::RegularNotificationLevel,
                _u8L("Snapmaker U1 export complete: ") + output_path);
    } else {
        wxMessageBox(
            _L("Export failed. Check the application log for details."),
            _L("Export for Snapmaker U1"), wxOK | wxICON_ERROR, q);
    }
}

} // namespace GUI
} // namespace Slic3r
