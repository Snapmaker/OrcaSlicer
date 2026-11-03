///|/ Copyright (c) Snapmaker and Contributors
///|/ OrcaSlicer is released under the terms of the AGPLv3 or higher
///|/
// ExportSnapmakerU1.hpp
// Dialog and helpers for "Export for Snapmaker U1" feature.
// Mirrors the bl2u1 web-app conversion logic natively inside OrcaSlicer.

#pragma once

#include <wx/dialog.h>
#include <wx/clrpicker.h>
#include <wx/choice.h>
#include <wx/checkbox.h>
#include <wx/stattext.h>

#include <map>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Maximum number of extruders the Snapmaker U1 supports
static constexpr int SNAPMAKER_U1_MAX_FILAMENTS = 4;

// Canonical printer model name as it appears in Snapmaker profiles
static constexpr const char* SNAPMAKER_U1_PRINTER_MODEL = "Snapmaker U1";

// Filament type used to pad unused slots (matches bl2u1 behaviour)
static constexpr const char* SNAPMAKER_U1_DEFAULT_TYPE = "PLA";

// Colour used to pad unused slots, as "#RRGGBB"
static constexpr const char* SNAPMAKER_U1_DEFAULT_COLOR = "#FFFFFF";

// Filament type -> name of the U1-compatible filament preset used for it.
// Built at runtime from the installed profile library, so every entry refers
// to a preset that actually exists.
using U1FilamentPresetMap = std::map<std::string, std::string>;

// ---------------------------------------------------------------------------
// U1FilamentMapping  – describes one filament slot after remapping
// ---------------------------------------------------------------------------

struct U1FilamentMapping {
    int         original_extruder_id { 1 };  ///< 1-based index in the source file
    int         new_extruder_id      { 1 };  ///< 1-based index in the output (1-4)
    std::string color;                       ///< "#RRGGBB" format
    std::string filament_type;               ///< e.g. "PLA"
    std::string snapmaker_profile;           ///< U1 preset name, empty if none is installed
};

// ---------------------------------------------------------------------------
// ExportSnapmakerU1Dialog
// ---------------------------------------------------------------------------

/// Dialog shown before exporting.  Lets the user:
///   • choose which extruders to include (max 4)
///   • adjust per-filament colour and type
///   • see which Snapmaker U1 profile will be used
class ExportSnapmakerU1Dialog : public wxDialog
{
public:
    /// @param parent          Owner window
    /// @param filament_types  Current filament type strings (index = extruder-1)
    /// @param filament_colors Current filament colours as "#RRGGBB" (index = extruder-1)
    /// @param u1_presets      Available U1 filament presets keyed by filament type.
    ///                        Empty when the U1 profiles are not installed.
    ExportSnapmakerU1Dialog(wxWindow*                        parent,
                            const std::vector<std::string>&  filament_types,
                            const std::vector<std::string>&  filament_colors,
                            const U1FilamentPresetMap&       u1_presets);

    /// Returns the final mapping chosen by the user.
    /// Only contains enabled rows, re-numbered 1..N (N ≤ 4).
    std::vector<U1FilamentMapping> get_mappings() const;

private:
    // ---- helpers -----------------------------------------------------------
    void        build_ui();
    void        refresh_new_ids();
    void        on_checkbox_toggle(wxCommandEvent& evt);
    std::string preset_for_type(const std::string& filament_type) const;
    wxString    profile_label_for_type(const std::string& filament_type) const;

    // ---- per-row UI --------------------------------------------------------
    struct FilamentRow {
        int                  original_id { 1 };
        wxCheckBox*          chk_enabled  { nullptr };
        wxColourPickerCtrl*  color_picker { nullptr };
        wxChoice*            type_choice  { nullptr };
        wxStaticText*        lbl_profile  { nullptr };
        wxStaticText*        lbl_new_id   { nullptr };
    };

    std::vector<FilamentRow>    m_rows;
    std::vector<std::string>    m_init_types;
    std::vector<std::string>    m_init_colors;
    U1FilamentPresetMap         m_u1_presets;
    std::vector<std::string>    m_type_choices;
};

} // namespace GUI
} // namespace Slic3r
