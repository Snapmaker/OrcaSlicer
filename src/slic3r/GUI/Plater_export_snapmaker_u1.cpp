///|/ Copyright (c) Snapmaker and Contributors
///|/ OrcaSlicer is released under the terms of the AGPLv3 or higher
///|/
// Plater_export_snapmaker_u1.cpp
//
// Provides Plater::export_snapmaker_u1() which is declared in Plater.hpp.
//
// The function:
//   1. Reads the currently-loaded model's filament info from the active config.
//   2. Shows ExportSnapmakerU1Dialog for user confirmation / remapping.
//   3. Builds a modified DynamicPrintConfig that targets the Snapmaker U1,
//      taking every per-filament setting from the chosen U1 filament presets.
//   4. Remaps filament ids on a copy of the model and writes it with store_bbs_3mf().

#include "Plater.hpp"
#include "ExportSnapmakerU1.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace Slic3r {
namespace GUI {

namespace {

constexpr size_t TARGET_FILAMENTS = SNAPMAKER_U1_MAX_FILAMENTS;

// Preferred preset per type when several U1-compatible presets exist.
constexpr const char* PREFERRED_PLA_PRESET = "Snapmaker PLA SnapSpeed @U1";

// Filament-id options that can appear on objects, volumes and layer ranges.
const std::array<const char*, 6> FILAMENT_ID_KEYS = {
    "extruder", "wall_filament", "sparse_infill_filament",
    "solid_infill_filament", "support_filament", "support_interface_filament"
};

std::vector<std::string> get_config_strings(const DynamicPrintConfig& cfg, const std::string& key)
{
    const auto* opt = cfg.option<ConfigOptionStrings>(key);
    return opt ? opt->values : std::vector<std::string>();
}

/// Normalise "#RRGGBB" / "#RRGGBBAA" / "RRGGBB" to upper-case "#RRGGBB".
std::string normalize_colour(const std::string& raw)
{
    std::string hex = (!raw.empty() && raw[0] == '#') ? raw.substr(1) : raw;
    if (hex.size() > 6) hex.resize(6);
    std::transform(hex.begin(), hex.end(), hex.begin(),
                   [](unsigned char ch) { return char(std::toupper(ch)); });
    return "#" + hex;
}

/// Pick the U1 printer preset, preferring the visible 0.4 mm nozzle variant.
const Preset* find_u1_printer_preset(const PresetCollection& printers)
{
    const Preset* best = nullptr;
    for (const Preset& preset : printers) {
        const auto* model = preset.config.option<ConfigOptionString>("printer_model");
        if (model == nullptr || model->value != SNAPMAKER_U1_PRINTER_MODEL)
            continue;
        if (preset.is_visible && boost::contains(preset.name, "0.4 nozzle"))
            return &preset;
        if (best == nullptr || (preset.is_visible && !best->is_visible))
            best = &preset;
    }
    return best;
}

int filament_preset_rank(const Preset& preset)
{
    if (preset.name == PREFERRED_PLA_PRESET)            return 0;
    if (boost::starts_with(preset.name, "Snapmaker "))  return 1;
    if (boost::starts_with(preset.name, "Generic "))    return 2;
    return 3;
}

/// For every filament type, the best installed filament preset compatible with the U1.
std::map<std::string, const Preset*> find_u1_filament_presets(const PresetBundle& bundle, const Preset& u1_printer)
{
    const PresetWithVendorProfile printer_with_vendor = bundle.printers.get_preset_with_vendor_profile(u1_printer);

    std::map<std::string, const Preset*> best;
    for (const Preset& preset : bundle.filaments) {
        const auto* types = preset.config.option<ConfigOptionStrings>("filament_type");
        if (types == nullptr || types->values.empty() || types->values.front().empty())
            continue;
        if (!is_compatible_with_printer(bundle.filaments.get_preset_with_vendor_profile(preset), printer_with_vendor))
            continue;

        const Preset*& slot = best[types->values.front()];
        const auto key = [](const Preset& p) { return std::make_tuple(filament_preset_rank(p), !p.is_visible, p.name); };
        if (slot == nullptr || key(preset) < key(*slot))
            slot = &preset;
    }
    return best;
}

/// Rebuild every per-filament vector option so it has exactly TARGET_FILAMENTS entries,
/// taking values from the chosen U1 preset of each slot and falling back to the source
/// filament the slot was mapped from.
void remap_filament_options(DynamicPrintConfig&               cfg,
                            size_t                            src_count,
                            const std::vector<size_t>&        src_index_per_slot,
                            const std::vector<const Preset*>& preset_per_slot)
{
    for (const std::string& key : Preset::filament_options()) {
        auto* dst = dynamic_cast<ConfigOptionVectorBase*>(cfg.option(key));
        if (dst == nullptr || dst->size() != src_count)
            continue;

        const std::unique_ptr<ConfigOption> src(dst->clone());
        dst->resize(TARGET_FILAMENTS);
        for (size_t slot = 0; slot < TARGET_FILAMENTS; ++slot) {
            const ConfigOption* from_preset = preset_per_slot[slot] ? preset_per_slot[slot]->config.option(key) : nullptr;
            const auto*         preset_vec  = dynamic_cast<const ConfigOptionVectorBase*>(from_preset);
            if (preset_vec != nullptr && !preset_vec->empty() && from_preset->type() == dst->type())
                dst->set_at(from_preset, slot, 0);
            else
                dst->set_at(src.get(), slot, src_index_per_slot[slot]);
        }
    }
}

/// Reorder the (possibly per-nozzle, block-stacked) flush volume matrix to the new slots.
void remap_flush_matrix(DynamicPrintConfig& cfg, size_t src_count, const std::vector<size_t>& src_index_per_slot)
{
    auto*        opt   = cfg.option<ConfigOptionFloats>("flush_volumes_matrix");
    const size_t block = src_count * src_count;
    if (opt == nullptr || block == 0 || opt->values.empty() || opt->values.size() % block != 0)
        return;

    const std::vector<double>& old_values = opt->values;
    std::vector<double>        new_values;
    new_values.reserve(old_values.size() / block * TARGET_FILAMENTS * TARGET_FILAMENTS);
    for (size_t b = 0; b < old_values.size() / block; ++b)
        for (size_t i = 0; i < TARGET_FILAMENTS; ++i)
            for (size_t j = 0; j < TARGET_FILAMENTS; ++j)
                new_values.push_back(i == j ? 0. : old_values[b * block + src_index_per_slot[i] * src_count + src_index_per_slot[j]]);
    opt->values = std::move(new_values);
}

/// Map a 1-based source filament id to its U1 slot. Excluded filaments go to slot 1;
/// 0 ("inherit") is preserved.
int remap_filament_id(const std::map<int, int>& remap, int old_id)
{
    if (old_id <= 0)
        return old_id;
    auto it = remap.find(old_id);
    return it == remap.end() ? 1 : it->second;
}

void remap_config_filament_ids(ModelConfig& cfg, const std::map<int, int>& remap)
{
    for (const char* key : FILAMENT_ID_KEYS) {
        if (!cfg.has(key))
            continue;
        const int old_id = cfg.opt_int(key);
        const int new_id = remap_filament_id(remap, old_id);
        if (new_id != old_id)
            cfg.set(key, new_id);
    }
}

/// Remap filament assignments of objects, volumes (incl. modifiers), layer ranges and
/// painted facets. Operates on the export copy only.
void remap_model_filament_ids(Model& model, const std::map<int, int>& remap)
{
    EnforcerBlockerStateMap state_map;
    state_map[0] = EnforcerBlockerType::NONE;
    for (size_t i = 1; i < state_map.size(); ++i)
        state_map[i] = EnforcerBlockerType(remap_filament_id(remap, int(i)));

    for (ModelObject* obj : model.objects) {
        remap_config_filament_ids(obj->config, remap);
        for (auto& [range, range_cfg] : obj->layer_config_ranges)
            remap_config_filament_ids(range_cfg, remap);
        for (ModelVolume* vol : obj->volumes) {
            remap_config_filament_ids(vol->config, remap);
            vol->remap_extruder_ids(TARGET_FILAMENTS, state_map);
        }
    }
}

void add_embedded_preset(std::vector<Preset*>& presets, const Preset* preset)
{
    // System presets are resolved from the profile library on load; only user presets are embedded.
    if (preset == nullptr || preset->is_system || preset->is_default)
        return;
    Preset* mutable_preset = const_cast<Preset*>(preset);
    if (std::find(presets.begin(), presets.end(), mutable_preset) == presets.end())
        presets.push_back(mutable_preset);
}

/// Owns the PlateData allocated by PartPlateList::store_to_3mf_structure().
struct PlateDataGuard {
    PlateDataPtrs list;
    ~PlateDataGuard() { release_PlateData_list(list); }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Plater::export_snapmaker_u1()
// ---------------------------------------------------------------------------

void Plater::export_snapmaker_u1()
{
    Model& live_model = model();
    if (live_model.objects.empty()) {
        wxMessageBox(_L("Nothing to export – the build plate is empty."),
                     _L("Export for Snapmaker U1"), wxOK | wxICON_INFORMATION, this);
        return;
    }

    // ------------------------------------------------------------------
    // 1. Read current filament info and resolve the U1 profiles
    // ------------------------------------------------------------------
    PresetBundle&            bundle     = *wxGetApp().preset_bundle;
    const DynamicPrintConfig active_cfg = bundle.full_config_secure();

    std::vector<std::string> filament_types = get_config_strings(active_cfg, "filament_type");
    std::vector<std::string> filament_colors;
    for (const std::string& c : get_config_strings(active_cfg, "filament_colour"))
        filament_colors.push_back(normalize_colour(c));
    if (filament_types.empty()) {
        filament_types.push_back(SNAPMAKER_U1_DEFAULT_TYPE);
        filament_colors.push_back(SNAPMAKER_U1_DEFAULT_COLOR);
    }
    const size_t src_count = filament_types.size();

    const Preset* u1_printer = find_u1_printer_preset(bundle.printers);
    std::map<std::string, const Preset*> u1_filaments;
    U1FilamentPresetMap                  u1_filament_names;
    if (u1_printer != nullptr) {
        u1_filaments = find_u1_filament_presets(bundle, *u1_printer);
        for (const auto& [type, preset] : u1_filaments)
            u1_filament_names[type] = preset->name;
    } else {
        wxMessageBox(
            wxString::Format(
                _L("The Snapmaker U1 printer profile (\"%s\") was not found in your "
                   "profile library.\n\nThe project will be exported using your current "
                   "printer and filament settings.\nInstall the Snapmaker profiles via the "
                   "Printer/Filament wizard for full compatibility."),
                wxString::FromUTF8(SNAPMAKER_U1_PRINTER_MODEL)),
            _L("Profile Not Found"), wxOK | wxICON_WARNING, this);
    }

    // ------------------------------------------------------------------
    // 2. Show the filament-mapping dialog
    // ------------------------------------------------------------------
    ExportSnapmakerU1Dialog dlg(this, filament_types, filament_colors, u1_filament_names);
    if (dlg.ShowModal() != wxID_OK)
        return;

    const std::vector<U1FilamentMapping> mappings = dlg.get_mappings();
    if (mappings.empty()) {
        wxMessageBox(_L("No filaments were selected for export."),
                     _L("Export for Snapmaker U1"), wxOK | wxICON_WARNING, this);
        return;
    }

    // ------------------------------------------------------------------
    // 3. Ask for output file path
    // ------------------------------------------------------------------
    wxFileDialog save_dlg(this,
        _L("Export for Snapmaker U1"),
        wxStandardPaths::Get().GetDocumentsDir(),
        wxT("Snapmaker_U1_Ready.3mf"),
        wxT("3MF files (*.3mf)|*.3mf"),
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (save_dlg.ShowModal() == wxID_CANCEL)
        return;

    const std::string output_path = save_dlg.GetPath().utf8_str().data();

    // ------------------------------------------------------------------
    // 4. Build the U1 config: printer preset + per-slot filament settings
    // ------------------------------------------------------------------
    DynamicPrintConfig u1_cfg = active_cfg;
    if (u1_printer != nullptr) {
        u1_cfg.apply(u1_printer->config);
        u1_cfg.set_key_value("printer_settings_id", new ConfigOptionString(u1_printer->name));
    }

    // Slot i takes its fallback values from source filament src_index_per_slot[i];
    // padding slots are white PLA (matches bl2u1 behaviour).
    const auto    pla_it     = u1_filaments.find(SNAPMAKER_U1_DEFAULT_TYPE);
    const Preset* pla_preset = pla_it == u1_filaments.end() ? nullptr : pla_it->second;

    std::vector<size_t>        src_index_per_slot(TARGET_FILAMENTS, 0);
    std::vector<const Preset*> preset_per_slot(TARGET_FILAMENTS, pla_preset);
    std::vector<std::string>   new_colors(TARGET_FILAMENTS, SNAPMAKER_U1_DEFAULT_COLOR);
    std::vector<std::string>   new_types(TARGET_FILAMENTS, SNAPMAKER_U1_DEFAULT_TYPE);

    for (const U1FilamentMapping& m : mappings) {
        const size_t slot        = size_t(m.new_extruder_id - 1);
        const auto   preset_it   = u1_filaments.find(m.filament_type);
        src_index_per_slot[slot] = size_t(m.original_extruder_id - 1);
        preset_per_slot[slot]    = preset_it == u1_filaments.end() ? nullptr : preset_it->second;
        new_colors[slot]         = m.color;
        new_types[slot]          = m.filament_type;
    }

    std::vector<std::string>       new_settings_ids(TARGET_FILAMENTS);
    const std::vector<std::string> src_settings_ids = get_config_strings(active_cfg, "filament_settings_id");
    for (size_t slot = 0; slot < TARGET_FILAMENTS; ++slot) {
        if (preset_per_slot[slot] != nullptr)
            new_settings_ids[slot] = preset_per_slot[slot]->name;
        else if (src_index_per_slot[slot] < src_settings_ids.size())
            new_settings_ids[slot] = src_settings_ids[src_index_per_slot[slot]];
    }

    remap_filament_options(u1_cfg, src_count, src_index_per_slot, preset_per_slot);
    remap_flush_matrix(u1_cfg, src_count, src_index_per_slot);
    u1_cfg.option<ConfigOptionStrings>("filament_colour", true)->values      = new_colors;
    u1_cfg.option<ConfigOptionStrings>("filament_type", true)->values        = new_types;
    u1_cfg.option<ConfigOptionStrings>("filament_settings_id", true)->values = new_settings_ids;

    // ------------------------------------------------------------------
    // 5. Remap filament ids on a deep copy so the live scene is untouched
    // ------------------------------------------------------------------
    std::map<int, int> extruder_remap; // 1-based old id -> 1-based U1 slot
    for (const U1FilamentMapping& m : mappings)
        extruder_remap[m.original_extruder_id] = m.new_extruder_id;

    Model export_model = live_model;
    remap_model_filament_ids(export_model, extruder_remap);

    PlateDataGuard plate_data;
    get_partplate_list().store_to_3mf_structure(plate_data.list, false, -1);

    std::vector<Preset*> project_presets;
    add_embedded_preset(project_presets, &bundle.prints.get_selected_preset());
    add_embedded_preset(project_presets, u1_printer);
    for (const Preset* preset : preset_per_slot)
        add_embedded_preset(project_presets, preset);

    // ------------------------------------------------------------------
    // 6. Write the 3MF. Thumbnails are omitted; OrcaSlicer regenerates them on load.
    //    No progress callback: yielding to the event loop here would allow a second
    //    export to start while this one is still writing.
    // ------------------------------------------------------------------
    StoreParams store_params;
    store_params.path            = output_path.c_str();
    store_params.model           = &export_model;
    store_params.plate_data_list = plate_data.list;
    store_params.config          = &u1_cfg;
    store_params.project_presets = project_presets;
    store_params.strategy        = SaveStrategy::Zip64;
    store_params.export_plate_idx = -1; // export all plates

    bool ok = false;
    {
        wxBusyCursor wait;
        try {
            ok = Slic3r::store_bbs_3mf(store_params);
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(error) << "export_snapmaker_u1: " << ex.what();
        }
    }

    if (ok) {
        wxMessageBox(
            wxString::Format(_L("Exported successfully:\n%s\n\n"
                                "Open this file in OrcaSlicer or Snapmaker Luban to slice for the U1."),
                             wxString::FromUTF8(output_path)),
            _L("Export for Snapmaker U1"), wxOK | wxICON_INFORMATION, this);
    } else {
        wxMessageBox(_L("Export failed. Check the application log for details."),
                     _L("Export for Snapmaker U1"), wxOK | wxICON_ERROR, this);
    }
}

} // namespace GUI
} // namespace Slic3r
