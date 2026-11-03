#include "CmykPalettePanel.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "Plater.hpp"
#include "PresetBundle.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/Preset.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/StateColor.hpp"

#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/colordlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

#include <algorithm>

namespace Slic3r { namespace GUI {

namespace {

constexpr int kSwatchPx = 22;

wxColour hex_to_colour(const std::string& hex)
{
    wxColour c;
    if (!try_parse_color_match_hex(wxString::FromUTF8(hex), c))
        return wxColour(128, 128, 128);
    return c;
}

std::string colour_to_hex(const wxColour& c)
{
    return normalize_palette_hex(c.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
}

// Names of the filament presets that can be assigned to a palette slot.
std::vector<std::string> selectable_filament_presets(PresetBundle& pb)
{
    std::vector<std::string> names;
    for (const Preset& preset : pb.filaments) {
        if (preset.is_visible && preset.is_compatible)
            names.push_back(preset.name);
    }
    return names;
}

} // namespace

std::vector<ColorConversionPaletteEntry> get_color_conversion_palette(PresetBundle& pb)
{
    const ConfigOptionStrings* opt = pb.project_config.option<ConfigOptionStrings>("color_conversion_palette");
    std::vector<ColorConversionPaletteEntry> palette = parse_color_conversion_palette(opt ? opt->values : std::vector<std::string>{});
    if (palette.size() >= kMinColorConversionPalette)
        return palette;

    // Default: cyan / magenta / yellow / white from the Full Spectrum library palette.
    const std::string default_family = GetFilamentMatchName(full_spectrum_preset_name());
    if (FilamentColorLibrary::Instance().EnsureLoaded()) {
        palette = default_color_conversion_palette(BuildFullSpectrumPalette(FilamentColorLibrary::Instance().GetAllFilamentInfos()),
                                                   default_family);
    } else {
        palette.clear();
    }
    if (palette.size() < 4) {
        palette = {{"#08ABFB", default_family}, {"#D93B90", default_family}, {"#F9ED3D", default_family}, {"#FFFFFF", default_family}};
    }
    // Library entries carry a family name; resolve it to a real, selectable preset.
    for (ColorConversionPaletteEntry& e : palette)
        e.preset_name = find_selectable_full_spectrum_family_preset(e.preset_name);
    return palette;
}

void set_color_conversion_palette(PresetBundle& pb, const std::vector<ColorConversionPaletteEntry>& entries)
{
    if (ConfigOptionStrings* opt = pb.project_config.option<ConfigOptionStrings>("color_conversion_palette", true))
        opt->values = serialize_color_conversion_palette(entries);
}

// ---------------------------------------------------------------------------
// CmykPalettePanel
// ---------------------------------------------------------------------------

CmykPalettePanel::CmykPalettePanel(wxWindow* parent, std::function<void()> on_layout_changed)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
    , m_on_layout_changed(std::move(on_layout_changed))
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    m_rows = new wxBoxSizer(wxVERTICAL);
    SetSizer(m_rows);
    reload();
}

void CmykPalettePanel::reload()
{
    PresetBundle* pb = wxGetApp().preset_bundle;
    if (pb == nullptr)
        return;
    m_entries = get_color_conversion_palette(*pb);
    m_preset_names = selectable_filament_presets(*pb);
    rebuild_rows();
}

void CmykPalettePanel::rebuild_rows()
{
    Freeze();
    m_rows->Clear(true);

    const int pad = FromDIP(6);
    const bool can_remove = m_entries.size() > kMinColorConversionPalette;

    for (size_t i = 0; i < m_entries.size(); ++i) {
        const ColorConversionPaletteEntry& e = m_entries[i];
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* index_label = new wxStaticText(this, wxID_ANY, wxString::Format("%zu", i + 1), wxDefaultPosition, wxSize(FromDIP(18), -1));
        row->Add(index_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, pad);

        auto* swatch = new wxStaticBitmap(this, wxID_ANY, make_color_match_swatch_bitmap(hex_to_colour(e.hex), wxSize(FromDIP(kSwatchPx), FromDIP(kSwatchPx))));
        swatch->SetToolTip(_L("Click to change the colour of this filament"));
        swatch->SetCursor(wxCursor(wxCURSOR_HAND));
        swatch->Bind(wxEVT_LEFT_UP, [this, i](wxMouseEvent&) { on_pick_color(i); });
        row->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, pad);

        auto* hex_label = new wxStaticText(this, wxID_ANY, wxString::FromUTF8(e.hex), wxDefaultPosition, wxSize(FromDIP(62), -1));
        row->Add(hex_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, pad);

        auto* choice = new wxChoice(this, wxID_ANY);
        choice->Append(_L("(keep current preset)"));
        int selected = 0;
        for (size_t p = 0; p < m_preset_names.size(); ++p) {
            choice->Append(wxString::FromUTF8(m_preset_names[p]));
            if (m_preset_names[p] == e.preset_name)
                selected = int(p) + 1;
        }
        choice->SetSelection(selected);
        choice->Bind(wxEVT_CHOICE, [this, i, choice](wxCommandEvent&) { on_preset_selected(i, choice->GetSelection()); });
        row->Add(choice, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, pad);

        auto* remove = new Button(this, _L("Remove"));
        remove->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        remove->Enable(can_remove);
        if (!can_remove)
            remove->SetToolTip(_L("At least two filaments are needed to mix colours."));
        remove->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { on_remove(i); });
        row->Add(remove, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, pad);

        m_rows->Add(row, 0, wxEXPAND | wxTOP, pad);
    }

    auto* add = new Button(this, _L("+ Add filament"));
    add->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    add->Enable(m_entries.size() < kMaxColorConversionPalette);
    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_add(); });
    m_rows->Add(add, 0, wxALL, pad);

    Layout();
    Thaw();
    if (m_on_layout_changed)
        m_on_layout_changed();
}

void CmykPalettePanel::commit()
{
    if (PresetBundle* pb = wxGetApp().preset_bundle)
        set_color_conversion_palette(*pb, m_entries);
    if (wxGetApp().plater() != nullptr)
        wxGetApp().plater()->update_project_dirty_from_presets();
}

void CmykPalettePanel::on_add()
{
    if (m_entries.size() >= kMaxColorConversionPalette)
        return;
    wxColourData data;
    data.SetColour(wxColour("#000000")); // black is the most common 5th filament
    wxColourDialog dlg(this, &data);
    if (dlg.ShowModal() != wxID_OK)
        return;
    ColorConversionPaletteEntry e;
    e.hex = colour_to_hex(dlg.GetColourData().GetColour());
    if (e.hex.empty())
        return;
    e.preset_name = m_entries.empty() ? std::string() : m_entries.back().preset_name;
    m_entries.push_back(std::move(e));
    commit();
    CallAfter([this]() { rebuild_rows(); }); // the Add button that fired this event is destroyed by the rebuild
}

void CmykPalettePanel::on_remove(size_t index)
{
    if (index >= m_entries.size() || m_entries.size() <= kMinColorConversionPalette)
        return;
    m_entries.erase(m_entries.begin() + index);
    commit();
    CallAfter([this]() { rebuild_rows(); }); // the button that fired this event is destroyed by the rebuild
}

void CmykPalettePanel::on_pick_color(size_t index)
{
    if (index >= m_entries.size())
        return;
    wxColourData data;
    data.SetColour(hex_to_colour(m_entries[index].hex));
    wxColourDialog dlg(this, &data);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string hex = colour_to_hex(dlg.GetColourData().GetColour());
    if (hex.empty() || hex == m_entries[index].hex)
        return;
    m_entries[index].hex = hex;
    commit();
    CallAfter([this]() { rebuild_rows(); });
}

void CmykPalettePanel::on_preset_selected(size_t index, int selection)
{
    if (index >= m_entries.size())
        return;
    m_entries[index].preset_name = (selection <= 0 || size_t(selection) > m_preset_names.size()) ? std::string() : m_preset_names[size_t(selection) - 1];
    commit();
}

// ---------------------------------------------------------------------------
// CmykConversionPreviewDialog
// ---------------------------------------------------------------------------

CmykConversionPreviewDialog::CmykConversionPreviewDialog(wxWindow*                                       parent,
                                                         const std::vector<ColorConversionPaletteEntry>& palette,
                                                         const std::vector<CmykPreviewRow>&              rows,
                                                         size_t                                          current_filament_count,
                                                         size_t                                          mixed_to_create)
    : wxDialog(parent, wxID_ANY, _L("Convert to CMYK"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    auto* root = new wxBoxSizer(wxVERTICAL);
    const int pad = FromDIP(10);

    wxString summary = wxString::Format(
        _L("Every model colour will be replaced by the closest mix of the %zu palette filaments (%zu mixed colours will be created). "
           "The %zu current filaments are removed from the project. This cannot be undone with Ctrl+Z."),
        palette.size(), mixed_to_create, current_filament_count);
    auto* text = new wxStaticText(this, wxID_ANY, summary);
    text->Wrap(FromDIP(460));
    root->Add(text, 0, wxALL, pad);

    // Palette legend: position -> colour, so recipe texts ("1 40% + 3 60%") can be read.
    auto* legend = new wxBoxSizer(wxHORIZONTAL);
    for (size_t i = 0; i < palette.size(); ++i) {
        legend->Add(new wxStaticText(this, wxID_ANY, wxString::Format("%zu", i + 1)), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        legend->Add(new wxStaticBitmap(this, wxID_ANY, make_color_match_swatch_bitmap(hex_to_colour(palette[i].hex), wxSize(FromDIP(16), FromDIP(16)))),
                    0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(3));
    }
    root->Add(legend, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);

    auto* scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(480), FromDIP(260)), wxVSCROLL | wxBORDER_SIMPLE);
    scroll->SetScrollRate(0, FromDIP(8));
    scroll->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    auto* grid = new wxFlexGridSizer(5, FromDIP(6), FromDIP(8));
    grid->AddGrowableCol(3, 1);
    const wxSize sw(FromDIP(22), FromDIP(22));
    for (const CmykPreviewRow& r : rows) {
        grid->Add(new wxStaticBitmap(scroll, wxID_ANY, make_color_match_swatch_bitmap(hex_to_colour(r.source_hex), sw)), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(new wxStaticText(scroll, wxID_ANY, wxString::FromUTF8("\xE2\x86\x92")), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(new wxStaticBitmap(scroll, wxID_ANY, make_color_match_swatch_bitmap(hex_to_colour(r.matched_hex), sw)), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(new wxStaticText(scroll, wxID_ANY, wxString::FromUTF8(r.recipe_text)), 1, wxALIGN_CENTER_VERTICAL | wxEXPAND);
        grid->Add(new wxStaticText(scroll, wxID_ANY, wxString(L"\u0394E ") + wxString::Format("%.1f", r.delta_e)), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    }
    scroll->SetSizer(grid);
    grid->FitInside(scroll);
    root->Add(scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, pad);

    auto* buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    if (auto* ok = FindWindow(wxID_OK))
        static_cast<wxButton*>(ok)->SetLabel(_L("Convert"));
    root->Add(buttons, 0, wxEXPAND | wxALL, pad);

    SetSizerAndFit(root);
    CentreOnParent();
}

}} // namespace Slic3r::GUI
