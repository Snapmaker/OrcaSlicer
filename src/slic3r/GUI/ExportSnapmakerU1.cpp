///|/ Copyright (c) Snapmaker and Contributors
///|/ OrcaSlicer is released under the terms of the AGPLv3 or higher
///|/
// ExportSnapmakerU1.cpp
// Implementation of the "Export for Snapmaker U1" dialog.

#include "ExportSnapmakerU1.hpp"
#include "I18N.hpp"

#include <wx/button.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statline.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace Slic3r {
namespace GUI {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Rows beyond this height scroll instead of growing the dialog off-screen.
constexpr int MAX_ROWS_HEIGHT_DIP = 360;

// Types offered when no U1 filament profiles are installed.
const std::vector<std::string>& fallback_type_choices()
{
    static const std::vector<std::string> choices = {
        "PLA", "PETG", "ABS", "ASA", "TPU", "PLA-CF", "PETG-CF", "PA", "PVA"
    };
    return choices;
}

/// Convert wxColour → "#RRGGBB" string
std::string wx_colour_to_hex(const wxColour& c)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.Red(), c.Green(), c.Blue());
    return buf;
}

int hex_digit_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    return std::toupper(static_cast<unsigned char>(ch)) - 'A' + 10;
}

/// Convert "#RRGGBB" or "#RRGGBBAA" → wxColour; anything malformed becomes white.
wxColour hex_to_wx_colour(const std::string& hex)
{
    const size_t start = (!hex.empty() && hex[0] == '#') ? 1 : 0;
    if (hex.size() < start + 6)
        return *wxWHITE;

    unsigned char rgb[3];
    for (size_t i = 0; i < 3; ++i) {
        const char hi = hex[start + 2 * i];
        const char lo = hex[start + 2 * i + 1];
        if (!std::isxdigit(static_cast<unsigned char>(hi)) || !std::isxdigit(static_cast<unsigned char>(lo)))
            return *wxWHITE;
        rgb[i] = static_cast<unsigned char>(hex_digit_value(hi) * 16 + hex_digit_value(lo));
    }
    return wxColour(rgb[0], rgb[1], rgb[2]);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ExportSnapmakerU1Dialog::ExportSnapmakerU1Dialog(
    wxWindow*                       parent,
    const std::vector<std::string>& filament_types,
    const std::vector<std::string>& filament_colors,
    const U1FilamentPresetMap&      u1_presets)
    : wxDialog(parent, wxID_ANY, _L("Export for Snapmaker U1"),
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_init_types (filament_types)
    , m_init_colors(filament_colors)
    , m_u1_presets (u1_presets)
{
    if (m_u1_presets.empty()) {
        m_type_choices = fallback_type_choices();
    } else {
        for (const auto& [type, preset] : m_u1_presets)
            m_type_choices.push_back(type);
    }

    build_ui();
    CentreOnParent();
}

// ---------------------------------------------------------------------------
// Preset lookup
// ---------------------------------------------------------------------------

std::string ExportSnapmakerU1Dialog::preset_for_type(const std::string& filament_type) const
{
    auto it = m_u1_presets.find(filament_type);
    return it == m_u1_presets.end() ? std::string() : it->second;
}

wxString ExportSnapmakerU1Dialog::profile_label_for_type(const std::string& filament_type) const
{
    const std::string preset = preset_for_type(filament_type);
    return preset.empty() ? _L("(no U1 profile installed)") : wxString::FromUTF8(preset);
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
           "Select which extruders to include and adjust types/colours as needed.\n"
           "Parts assigned to an excluded filament will print with slot 1."));
    top_sizer->Add(info, 0, wxALL | wxEXPAND, 10);
    top_sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // ---- Scrollable grid: headers + one row per filament ----
    auto* scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    scroll->SetScrollRate(0, FromDIP(10));

    auto* grid = new wxFlexGridSizer(6, FromDIP(4), FromDIP(8));
    grid->AddGrowableCol(4, 1);

    grid->Add(new wxStaticText(scroll, wxID_ANY, _L("Include")),    0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(scroll, wxID_ANY, _L("Src #")),      0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(scroll, wxID_ANY, _L("Colour")),     0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(scroll, wxID_ANY, _L("Type")),       0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(scroll, wxID_ANY, _L("U1 Profile")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(scroll, wxID_ANY, _L("Slot")),       0, wxALIGN_CENTER_VERTICAL);

    wxArrayString wx_types;
    for (const auto& t : m_type_choices) wx_types.Add(wxString::FromUTF8(t));

    const auto default_it  = std::find(m_type_choices.begin(), m_type_choices.end(), SNAPMAKER_U1_DEFAULT_TYPE);
    const int  default_sel = default_it == m_type_choices.end() ? 0 : int(default_it - m_type_choices.begin());

    const int n = (int)m_init_types.size();
    m_rows.resize(n);

    for (int i = 0; i < n; ++i) {
        FilamentRow& row = m_rows[i];
        row.original_id  = i + 1;

        // Checkbox: include?
        row.chk_enabled = new wxCheckBox(scroll, wxID_ANY, wxEmptyString);
        row.chk_enabled->SetValue(i < SNAPMAKER_U1_MAX_FILAMENTS); // first 4 checked
        row.chk_enabled->Bind(wxEVT_CHECKBOX, &ExportSnapmakerU1Dialog::on_checkbox_toggle, this);
        grid->Add(row.chk_enabled, 0, wxALIGN_CENTER_VERTICAL);

        // Source index label
        grid->Add(new wxStaticText(scroll, wxID_ANY, wxString::Format(wxT("%d"), row.original_id)),
                  0, wxALIGN_CENTER_VERTICAL);

        // Colour picker
        wxColour init_col = (i < (int)m_init_colors.size()) ? hex_to_wx_colour(m_init_colors[i]) : *wxWHITE;
        row.color_picker = new wxColourPickerCtrl(scroll, wxID_ANY, init_col);
        grid->Add(row.color_picker, 0, wxALIGN_CENTER_VERTICAL);

        // Filament type choice, pre-selecting the source type when it is offered
        row.type_choice = new wxChoice(scroll, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(90), -1), wx_types);
        const auto type_it = std::find(m_type_choices.begin(), m_type_choices.end(), m_init_types[i]);
        const int  sel     = type_it == m_type_choices.end() ? default_sel : int(type_it - m_type_choices.begin());
        row.type_choice->SetSelection(sel);
        row.type_choice->Bind(wxEVT_CHOICE, [this, i](wxCommandEvent&) {
            const std::string t = m_rows[i].type_choice->GetStringSelection().ToStdString();
            m_rows[i].lbl_profile->SetLabel(profile_label_for_type(t));
            Layout();
        });
        grid->Add(row.type_choice, 0, wxALIGN_CENTER_VERTICAL);

        // Profile label (read-only)
        row.lbl_profile = new wxStaticText(scroll, wxID_ANY,
            profile_label_for_type(m_type_choices.empty() ? std::string() : m_type_choices[sel]),
            wxDefaultPosition, wxSize(FromDIP(220), -1), wxST_ELLIPSIZE_END);
        grid->Add(row.lbl_profile, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND);

        // New slot label (updated dynamically)
        row.lbl_new_id = new wxStaticText(scroll, wxID_ANY, wxT("-"),
            wxDefaultPosition, wxSize(FromDIP(40), -1), wxALIGN_CENTER_HORIZONTAL);
        grid->Add(row.lbl_new_id, 0, wxALIGN_CENTER_VERTICAL);
    }

    auto* scroll_sizer = new wxBoxSizer(wxVERTICAL);
    scroll_sizer->Add(grid, 1, wxEXPAND | wxALL, FromDIP(4));
    scroll->SetSizer(scroll_sizer);
    scroll->FitInside();

    const wxSize grid_size = scroll_sizer->GetMinSize();
    scroll->SetMinSize(wxSize(grid_size.x + wxSystemSettings::GetMetric(wxSYS_VSCROLL_X),
                              std::min(grid_size.y, FromDIP(MAX_ROWS_HEIGHT_DIP))));
    top_sizer->Add(scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // ---- OK / Cancel ----
    top_sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
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
    const wxColour normal_colour = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    const wxColour grey_colour   = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);

    int slot = 1;
    for (auto& row : m_rows) {
        if (row.chk_enabled->GetValue() && slot <= SNAPMAKER_U1_MAX_FILAMENTS) {
            row.lbl_new_id->SetLabel(wxString::Format(wxT("%d"), slot));
            row.lbl_new_id->SetForegroundColour(normal_colour);
            ++slot;
        } else {
            row.lbl_new_id->SetLabel(wxT("-"));
            row.lbl_new_id->SetForegroundColour(grey_colour);
        }
        row.lbl_new_id->Refresh();
    }
}

void ExportSnapmakerU1Dialog::on_checkbox_toggle(wxCommandEvent& evt)
{
    int enabled_count = 0;
    for (const auto& row : m_rows)
        if (row.chk_enabled->GetValue()) ++enabled_count;

    // Undo the tick that pushed us over the limit – the box the user just clicked.
    auto* toggled = dynamic_cast<wxCheckBox*>(evt.GetEventObject());
    if (enabled_count > SNAPMAKER_U1_MAX_FILAMENTS && toggled != nullptr && toggled->GetValue()) {
        toggled->SetValue(false);
        wxMessageBox(
            _L("The Snapmaker U1 supports a maximum of 4 filaments.\n"
               "Please deselect a filament before enabling another."),
            _L("Snapmaker U1 Limit"), wxOK | wxICON_WARNING, this);
    }
    refresh_new_ids();
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
        m.color                = wx_colour_to_hex(row.color_picker->GetColour());
        m.filament_type        = row.type_choice->GetStringSelection().ToStdString();
        m.snapmaker_profile    = preset_for_type(m.filament_type);

        result.push_back(std::move(m));
    }

    return result;
}

} // namespace GUI
} // namespace Slic3r
