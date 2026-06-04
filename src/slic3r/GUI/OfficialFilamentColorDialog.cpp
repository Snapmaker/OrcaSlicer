#include "OfficialFilamentColorDialog.hpp"

#include "FilamentColorUtils.hpp"
#include "GUI_App.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "libslic3r/AppConfig.hpp"

#include <ColorSpaceConvert.hpp>

#include <wx/button.h>
#include <wx/colordlg.h>
#include <wx/dcbuffer.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/wrapsizer.h>

#include <algorithm>

namespace Slic3r { namespace GUI {
namespace {

OfficialFilamentColorSelection make_official_selection(const OfficialFilamentColor& color)
{
    OfficialFilamentColorSelection selection;
    selection.name = color.name;
    selection.sku = color.sku;
    selection.mode = FilamentColorUtils::normalize_colour_mode(color.mode);
    selection.is_custom = false;

    for (const std::string& item : color.colors) {
        std::string normalized = FilamentColorUtils::normalize_hex_color(item);
        if (!normalized.empty())
            selection.colors.emplace_back(normalized);
    }

    if (selection.colors.empty()) {
        std::string normalized = FilamentColorUtils::normalize_hex_color(color.primary_color);
        if (!normalized.empty())
            selection.colors.emplace_back(normalized);
    }

    selection.primary_color = FilamentColorUtils::get_primary_color(selection.colors, color.primary_color);
    if (selection.colors.size() <= 1)
        selection.mode = 0;
    return selection;
}

OfficialFilamentColorSelection make_custom_selection(const std::string& multi_colors, int mode, const std::string& fallback_color)
{
    OfficialFilamentColorSelection selection;
    selection.colors = FilamentColorUtils::split_multi_colors(multi_colors);
    if (selection.colors.empty()) {
        std::string fallback = FilamentColorUtils::normalize_hex_color(fallback_color, "#FFFFFF");
        selection.colors.emplace_back(fallback.empty() ? "#FFFFFF" : fallback);
    }

    selection.primary_color = FilamentColorUtils::get_primary_color(selection.colors, fallback_color);
    selection.name = selection.primary_color;
    selection.mode = selection.colors.size() > 1 ? FilamentColorUtils::normalize_colour_mode(mode) : 0;
    selection.is_custom = true;
    return selection;
}

wxString from_std(const std::string& value)
{
    return wxString::FromUTF8(value.c_str());
}

class OfficialColorSwatch : public wxPanel
{
public:
    OfficialColorSwatch(wxWindow* parent, const OfficialFilamentColor& color, const wxSize& size)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
        , m_color(color)
    {
        SetMinSize(size);
        SetMaxSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        SetToolTip(from_std(color.name + "\nSKU: " + color.sku));
        Bind(wxEVT_PAINT, &OfficialColorSwatch::on_paint, this);
    }

    void set_selected(bool selected)
    {
        if (m_selected == selected)
            return;
        m_selected = selected;
        Refresh();
    }

private:
    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF"))));
        dc.Clear();

        int pad = FromDIP(3);
        int bitmap_width = std::max(1, size.GetWidth() - pad * 2);
        int bitmap_height = std::max(1, size.GetHeight() - pad * 2);
        OfficialFilamentColorSelection selection = make_official_selection(m_color);
        wxBitmap* bitmap = FilamentColorUtils::get_filament_color_icon(selection.colors,
            selection.mode,
            "",
            bitmap_width,
            bitmap_height);
        if (bitmap != nullptr)
            dc.DrawBitmap(*bitmap, pad, pad, true);

        wxColour border = m_selected ? wxColour("#0B57FF") : wxColour("#D0D0D0");
        dc.SetPen(wxPen(border, m_selected ? FromDIP(2) : FromDIP(1)));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
    }

private:
    OfficialFilamentColor m_color;
    bool m_selected { false };
};

} // namespace

OfficialFilamentColorDialog::OfficialFilamentColorDialog(wxWindow* parent,
    const OfficialFilamentMaterial& material,
    const std::string& current_sku,
    const std::string& current_multi_colors,
    int current_mode,
    const std::string& fallback_color)
    : DPIDialog(parent, wxID_ANY, _L("Filament Color"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_material(material)
{
    auto current_color = std::find_if(m_material.colors.begin(),
        m_material.colors.end(),
        [&current_sku](const OfficialFilamentColor& color) { return !current_sku.empty() && color.sku == current_sku; });

    if (current_color != m_material.colors.end())
        m_selection = make_official_selection(*current_color);
    else
        m_selection = make_custom_selection(current_multi_colors, current_mode, fallback_color);

    build_ui();
    update_preview();
    update_swatch_selection();
    wxGetApp().UpdateDarkUIWin(this);
}

void OfficialFilamentColorDialog::build_ui()
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F4F4F4")));

    auto* root = new wxBoxSizer(wxVERTICAL);
    int margin = FromDIP(24);

    auto* current_row = new wxBoxSizer(wxHORIZONTAL);
    m_preview_bitmap = new wxStaticBitmap(this, wxID_ANY, wxBitmap(FromDIP(82), FromDIP(82)));
    current_row->Add(m_preview_bitmap, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(18));

    auto* info_sizer = new wxBoxSizer(wxVERTICAL);
    m_name_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_name_label->SetFont(Label::Head_16);
    m_sku_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_sku_label->SetFont(Label::Body_14);
    info_sizer->Add(m_name_label, 0, wxBOTTOM, FromDIP(8));
    info_sizer->Add(m_sku_label, 0);
    current_row->Add(info_sizer, 1, wxALIGN_CENTER_VERTICAL);
    root->Add(current_row, 0, wxEXPAND | wxALL, margin);

    root->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, margin);

    auto* official_label = new wxStaticText(this, wxID_ANY, _L("Official material"));
    official_label->SetFont(Label::Head_14);
    root->Add(official_label, 0, wxLEFT | wxRIGHT | wxTOP, margin);

    auto* swatch_panel = new wxPanel(this, wxID_ANY);
    auto* swatch_sizer = new wxWrapSizer(wxHORIZONTAL);
    wxSize swatch_size(FromDIP(38), FromDIP(38));
    for (const OfficialFilamentColor& color : m_material.colors) {
        auto* swatch = new OfficialColorSwatch(swatch_panel, color, swatch_size);
        swatch->Bind(wxEVT_LEFT_UP, [this, color](wxMouseEvent&) { select_official_color(color); });
        swatch_sizer->Add(swatch, 0, wxRIGHT | wxBOTTOM, FromDIP(12));
        m_swatch_by_sku.emplace_back(swatch, color.sku);
    }
    swatch_panel->SetSizer(swatch_sizer);
    root->Add(swatch_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);

    root->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);

    auto* other_label = new wxStaticText(this, wxID_ANY, _L("Other"));
    other_label->SetFont(Label::Head_14);
    root->Add(other_label, 0, wxLEFT | wxRIGHT | wxTOP, margin);

    auto* more_button = new wxButton(this, wxID_ANY, _L("+ More colors"));
    more_button->SetMinSize(wxSize(FromDIP(360), FromDIP(42)));
    more_button->Bind(wxEVT_BUTTON, &OfficialFilamentColorDialog::on_more_color, this);
    root->Add(more_button, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    auto* ok = new wxButton(this, wxID_OK, _L("OK"));
    buttons->AddStretchSpacer();
    buttons->Add(cancel, 0, wxRIGHT, FromDIP(12));
    buttons->Add(ok, 0);
    root->Add(buttons, 0, wxEXPAND | wxALL, margin);

    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); }, wxID_OK);
    SetSizerAndFit(root);
    SetMinSize(wxSize(FromDIP(430), GetSize().GetHeight()));
    CenterOnParent();
}

void OfficialFilamentColorDialog::select_official_color(const OfficialFilamentColor& color)
{
    m_selection = make_official_selection(color);
    update_preview();
    update_swatch_selection();
}

void OfficialFilamentColorDialog::select_custom_color(const std::string& color)
{
    std::string normalized = FilamentColorUtils::normalize_hex_color(color, "#FFFFFF");
    m_selection = make_custom_selection(normalized, 0, normalized);
    update_preview();
    update_swatch_selection();
}

void OfficialFilamentColorDialog::update_preview()
{
    wxBitmap* bitmap = FilamentColorUtils::get_filament_color_icon(m_selection.colors,
        m_selection.mode,
        "",
        FromDIP(82),
        FromDIP(82));
    if (bitmap != nullptr)
        m_preview_bitmap->SetBitmap(*bitmap);

    wxString name = m_selection.name.empty() ? from_std(m_selection.primary_color) : from_std(m_selection.name);
    m_name_label->SetLabel(name);
    m_sku_label->SetLabel(m_selection.sku.empty() ? _L("SKU: -") : _L("SKU: ") + from_std(m_selection.sku));
    Layout();
}

void OfficialFilamentColorDialog::update_swatch_selection()
{
    for (const auto& item : m_swatch_by_sku) {
        auto* swatch = dynamic_cast<OfficialColorSwatch*>(item.first);
        if (swatch != nullptr)
            swatch->set_selected(!m_selection.sku.empty() && item.second == m_selection.sku);
    }
}

void OfficialFilamentColorDialog::on_more_color(wxCommandEvent&)
{
    wxColourData data;
    data.SetChooseFull(true);
    data.SetChooseAlpha(false);
    data.SetColour(wxColour(m_selection.primary_color));

    std::vector<std::string> custom_colors;
    if (wxGetApp().app_config != nullptr)
        custom_colors = wxGetApp().app_config->get_custom_color_from_config();

    int custom_count = std::min(static_cast<int>(custom_colors.size()), CUSTOM_COLOR_COUNT);
    for (int i = 0; i < custom_count; ++i)
        data.SetCustomColour(i, string_to_wxColor(custom_colors[i]));

    wxColourDialog dialog(this, &data);
    dialog.SetTitle(_L("Please choose the filament color"));
    if (dialog.ShowModal() != wxID_OK)
        return;

    wxColourData result = dialog.GetColourData();
    if (custom_colors.size() != CUSTOM_COLOR_COUNT)
        custom_colors.resize(CUSTOM_COLOR_COUNT);
    for (int i = 0; i < CUSTOM_COLOR_COUNT; ++i)
        custom_colors[i] = color_to_string(result.GetCustomColour(i));
    if (wxGetApp().app_config != nullptr)
        wxGetApp().app_config->save_custom_color_to_config(custom_colors);

    wxColour color = result.GetColour();
    if (color.IsOk())
        select_custom_color(color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
}

}} // namespace Slic3r::GUI
