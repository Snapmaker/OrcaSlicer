///|/ Copyright (c) Snapmaker and Contributors
///|/ OrcaSlicer is released under the terms of the AGPLv3 or higher
///|/
// ExportSnapmakerU1.hpp
// Dialog and helpers for "Export for Snapmaker U1" feature.
// Mirrors the bl2u1 web-app conversion logic natively inside OrcaSlicer.

#pragma once

#include <wx/wx.h>
#include <wx/dialog.h>
#include <wx/clrpicker.h>
#include <wx/choice.h>
#include <wx/checkbox.h>
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <wx/scrolwin.h>

#include <string>
#include <vector>
#include <map>

namespace Slic3r {
namespace GUI {

// ---------------------------------------------------------------------------
// Constants / mapping tables
// ---------------------------------------------------------------------------

// Maximum number of extruders the Snapmaker U1 supports
static constexpr int SNAPMAKER_U1_MAX_FILAMENTS = 4;

// Canonical printer model name as it appears in Snapmaker profiles
static constexpr const char* SNAPMAKER_U1_PRINTER_MODEL = "Snapmaker U1";

// Default white PLA colour used when padding to 4 filaments
static constexpr const char* SNAPMAKER_U1_DEFAULT_COLOR = "#FFFFFFFF";

// Maps generic filament type strings → Snapmaker U1 preset names.
// These should match entries in resources/profiles/Snapmaker.json.
inline const std::map<std::string, std::string>& snapmaker_u1_filament_map()
{
    static const std::map<std::string, std::string> m = {
        { "PLA",     "Snapmaker PLA SnapSpeed @U1" },
        { "PETG",    "Snapmaker PETG HF @U1"       },
        { "ABS",     "Generic ABS @U1"              },
        { "TPU",     "Generic TPU @U1"              },
        { "PLA-CF",  "Generic PLA-CF @U1"           },
        { "PETG-CF", "Generic PETG-CF @U1"          },
        { "ASA",     "Generic ASA @U1"              },
        { "PA",      "Generic PA @U1"               },
    };
    return m;
}

// Returns the best-matching Snapmaker U1 filament preset name for a given type.
inline std::string get_u1_filament_preset(const std::string& filament_type)
{
    const auto& m = snapmaker_u1_filament_map();
    auto it = m.find(filament_type);
    if (it != m.end())
        return it->second;
    // Fall back to PLA profile for unknown types
    return "Snapmaker PLA SnapSpeed @U1";
}

// ---------------------------------------------------------------------------
// U1FilamentMapping  – describes one filament slot after remapping
// ---------------------------------------------------------------------------

struct U1FilamentMapping {
    int         original_extruder_id { 1 };  ///< 1-based index in the source file
    int         new_extruder_id      { 1 };  ///< 1-based index in the output (1-4)
    bool        enabled              { true };
    std::string color;                       ///< "#RRGGBB" format
    std::string filament_type;               ///< e.g. "PLA"
    std::string snapmaker_profile;           ///< Snapmaker preset name
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
    ExportSnapmakerU1Dialog(wxWindow*                        parent,
                            const std::vector<std::string>&  filament_types,
                            const std::vector<std::string>&  filament_colors);

    /// Returns the final mapping chosen by the user.
    /// Only contains enabled rows, re-numbered 1..N (N ≤ 4).
    std::vector<U1FilamentMapping> get_mappings() const;

private:
    // ---- helpers -----------------------------------------------------------
    void build_ui();
    void refresh_new_ids();
    void on_checkbox_toggle(wxCommandEvent& evt);
    void on_ok            (wxCommandEvent& evt);

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

    // Sorted list of filament type strings for the wxChoice drop-down
    static const std::vector<std::string>& filament_type_choices();

    DECLARE_EVENT_TABLE()
};

} // namespace GUI
} // namespace Slic3r
