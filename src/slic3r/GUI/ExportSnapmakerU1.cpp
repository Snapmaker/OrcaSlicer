///|/ Copyright (c) Snapmaker and Contributors
///|/ OrcaSlicer is released under the terms of the AGPLv3 or higher
///|/
// ExportSnapmakerU1.cpp
// Implementation of the "Export for Snapmaker U1" dialog.

#include "ExportSnapmakerU1.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "format.hpp"

#include <wx/button.h>
#include <wx/panel.h>
#include <wx/statline.h>
#include <wx/clrpicker.h>

#include <algorithm>
#include <sstream>

namespace Slic3r {
namespace GUI {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Convert wxColour → "#RRGGBB" string
static std::string wx_colour_to_hex(const wxColour& c)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.Red(), c.Green(), c.Blue());
    return buf;
}

/// Convert "#RRGGBB" or "#RRGGBBAA" → wxColour
static wxColour hex_to_wx_colour(const std::string& hex)
{
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() >= 6) {
        unsigned long r = std::stoul(h.substr(0, 2), nullptr, 16);
        unsigned long g = std::stoul(h.substr(2, 2), nullptr, 16);
        unsigned long b = std::stoul(h.substr(4, 2), nullptr, 16);
        return wxColour((unsigned char)r, (unsigned char)g, (unsigned char)b);
    }
    return *wxWHITE;
}

// ---------------------------------------------------------------------------
// Static data
// ---------------------------------------------------------------------------

/*static*/
const std::vector<std::string>& ExportSnapmakerU1Dialog::filament_type_choices()
{
    static const std::vector<std::string> choices = {
        "PLA", "PETG", "ABS", "TPU",
        "PLA-CF", "PETG-CF", "ASA", "PA",
        "PVA", "HIPS"
    };
    return choices;
}

// ---------------------------------------------------------------------------
// Event table
// ---------------------------------------------------------------------------

BEGIN_EVENT_TABLE(ExportSnapmakerU1Dialog, wxDialog)
    EVT_BUTTON(wxID_OK,     ExportSnapmakerU1Dialog::on_ok)
END_EVENT_TABLE()

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ExportSnapmakerU1Dialog::ExportSnapmakerU1Dialog(
    wxWindow*                       parent,
    const std::vector<std::string>& filament_types,
    const std::vector<std::string>& filament_colors)
    : wxDialog(parent, wxID_ANY, _L("Export for Snapmaker U1"),
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_init_types (filament_types)
    , m_init_colors(filament_colors)
{
    build_ui();
    Fit();
    CentreOnParent();
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void ExportSnapmakerU1Dialog::build_ui()
{
    auto* top_sizer = new wxBoxSizer(wxVERTICAL);

    // ---- Banner / info ----
    auto* info = new wxStaticText(this, wxID_ANY,
        _L("The Snapmaker U1 supports up to 4 filaments.\n"
           "Select which extruders to include and adjust types/colours as needed."));
    top_sizer->Add(info, 0, wxALL | wxEXPAND, 10);
    top_sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // ---- Column headers ----
    auto* hdr_sizer = new wxBoxSizer(wxHORIZONTAL);
    hdr_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Include")),  0, wxALL, 4);
    hdr_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Src #")),    0, wxALL, 4);
    hdr_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Colour")),   0, wxALL, 4);
    hdr_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Type")),     0, wxALL, 4);
    hdr_sizer->Add(new wxStaticText(this, wxID_ANY, _L("U1 Profile")), 1, wxALL | wxEXPAND, 4);
    hdr_sizer->Add(new wxStaticText(this, wxID_ANY, _L("→ Slot")),   0, wxALL, 4);
    top_sizer->Add(hdr_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
    top_sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // ---- One row per filament in the source file ----
    const auto& type_list = filament_type_choices();
    wxArrayString wx_types;
    for (const auto& t : type_list) wx_types.Add(wxString::FromUTF8(t));

    const int n = (int)m_init_types.size();
    m_rows.resize(n);

    for (int i = 0; i < n; ++i) {
        FilamentRow& row = m_rows[i];
        row.original_id  = i + 1;

        auto* row_sizer = new wxBoxSizer(wxHORIZONTAL);

        // Checkbox: include?
        row.chk_enabled = new wxCheckBox(this, wxID_ANY, wxEmptyString);
        row.chk_enabled->SetValue(i < SNAPMAKER_U1_MAX_FILAMENTS); // first 4 checked
        row.chk_enabled->Bind(wxEVT_CHECKBOX,
            &ExportSnapmakerU1Dialog::on_checkbox_toggle, this);
        row_sizer->Add(row.chk_enabled, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);

        // Source index label
        row_sizer->Add(new wxStaticText(this, wxID_ANY,
            wxString::Format(wxT("%d"), row.original_id)),
            0, wxALIGN_CENTER_VERTICAL | wxALL, 4);

        // Colour picker
        wxColour init_col = (i < (int)m_init_colors.size())
                          ? hex_to_wx_colour(m_init_colors[i])
                          : *wxWHITE;
        row.color_picker = new wxColourPickerCtrl(this, wxID_ANY, init_col);
        row_sizer->Add(row.color_picker, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);

        // Filament type choice
        row.type_choice = new wxChoice(this, wxID_ANY, wxDefaultPosition,
                                       wxSize(90, -1), wx_types);
        // Pre-select matching type
        std::string init_type = (i < (int)m_init_types.size()) ? m_init_types[i] : "PLA";
        int sel = 0;
        for (int j = 0; j < (int)type_list.size(); ++j) {
            if (type_list[j] == init_type) { sel = j; break; }
        }
        row.type_choice->SetSelection(sel);
        row.type_choice->Bind(wxEVT_CHOICE, [this, i](wxCommandEvent&) {
            // Update profile label when type changes
            std::string t = m_rows[i].type_choice->GetStringSelection().ToStdString();
            m_rows[i].lbl_profile->SetLabel(wxString::FromUTF8(get_u1_filament_preset(t)));
            Layout();
        });
        row_sizer->Add(row.type_choice, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);

        // Profile label (read-only)
        std::string profile = get_u1_filament_preset(init_type);
        row.lbl_profile = new wxStaticText(this, wxID_ANY,
            wxString::FromUTF8(profile), wxDefaultPosition,
            wxSize(220, -1), wxST_ELLIPSIZE_END);
        row_sizer->Add(row.lbl_profile, 1, wxALIGN_CENTER_VERTICAL | wxALL, 4);

        // New slot label (updated dynamically)
        row.lbl_new_id = new wxStaticText(this, wxID_ANY, wxT("-"),
            wxDefaultPosition, wxSize(30, -1), wxALIGN_CENTER);
        row_sizer->Add(row.lbl_new_id, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);

        top_sizer->Add(row_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
    }

    // ---- Status line ----
    top_sizer->AddSpacer(6);
    top_sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
    top_sizer->AddSpacer(6);

    // ---- OK / Cancel ----
    auto* btn_sizer = CreateButtonSizer(wxOK | wxCANCEL);
    top_sizer->Add(btn_sizer, 0, wxALL | wxALIGN_RIGHT, 10);

    SetSizerAndFit(top_sizer);
    refresh_new_ids();
}

// ---------------------------------------------------------------------------
// Helpers – slot numbering
// ---------------------------------------------------------------------------

void ExportSnapmakerU1Dialog::refresh_new_ids()
{
    int slot = 1;
    for (auto& row : m_rows) {
        if (row.chk_enabled->GetValue()) {
            if (slot <= SNAPMAKER_U1_MAX_FILAMENTS) {
                row.lbl_new_id->SetLabel(wxString::Format(wxT("→ %d"), slot));
                ++slot;
            } else {
                // Over the limit – show warning colour
                row.lbl_new_id->SetLabel(wxT("OVER"));
                row.lbl_new_id->SetForegroundColour(*wxRED);
            }
        } else {
            row.lbl_new_id->SetLabel(wxT("-"));
            row.lbl_new_id->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        }
    }
}

void ExportSnapmakerU1Dialog::on_checkbox_toggle(wxCommandEvent& /*evt*/)
{
    // Count enabled rows; prevent enabling a 5th
    int enabled_count = 0;
    for (const auto& row : m_rows)
        if (row.chk_enabled->GetValue()) ++enabled_count;

    if (enabled_count > SNAPMAKER_U1_MAX_FILAMENTS) {
        // Find the last newly-checked one and uncheck it
        for (auto it = m_rows.rbegin(); it != m_rows.rend(); ++it) {
            if (it->chk_enabled->GetValue()) {
                it->chk_enabled->SetValue(false);
                wxMessageBox(
                    _L("The Snapmaker U1 supports a maximum of 4 filaments.\n"
                       "Please deselect a filament before enabling another."),
                    _L("Snapmaker U1 Limit"), wxOK | wxICON_WARNING, this);
                break;
            }
        }
    }
    refresh_new_ids();
}

void ExportSnapmakerU1Dialog::on_ok(wxCommandEvent& evt)
{
    evt.Skip(); // let the dialog close
}

// ---------------------------------------------------------------------------
// Result extraction
// ---------------------------------------------------------------------------

std::vector<U1FilamentMapping> ExportSnapmakerU1Dialog::get_mappings() const
{
    std::vector<U1FilamentMapping> result;
    int slot = 1;

    for (const auto& row : m_rows) {
        if (!row.chk_enabled->GetValue()) continue;
        if (slot > SNAPMAKER_U1_MAX_FILAMENTS)   break;

        U1FilamentMapping m;
        m.original_extruder_id = row.original_id;
        m.new_extruder_id      = slot++;
        m.enabled              = true;
        m.color                = wx_colour_to_hex(row.color_picker->GetColour());
        m.filament_type        = row.type_choice->GetStringSelection().ToStdString();
        m.snapmaker_profile    = get_u1_filament_preset(m.filament_type);

        result.push_back(std::move(m));
    }

    return result;
}

} // namespace GUI
} // namespace Slic3r
