#include "FilamentColorDialog.hpp"

#include "FilamentColorUtils.hpp"
#include "GUI_App.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "libslic3r/AppConfig.hpp"

#include <ColorSpaceConvert.hpp>

#include <wx/colordlg.h>
#include <wx/dcbuffer.h>
#include <wx/display.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/wrapsizer.h>

#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace Slic3r
{
namespace GUI
{
namespace
{

/**
 * @brief HSV value cached for color sorting.
 */
struct SortHsv
{
    float h { 0.0f };
    float s { 0.0f };
    float v { 0.0f };
};

/**
 * @brief Lightweight color item with cached sort data.
 */
struct ColorSortItem
{
    const FilamentColor* color { nullptr };
    std::vector<std::string> colors;
    std::vector<SortHsv> hsvColors;
    int typeOrder { 0 };
    std::string enName;
    std::string zhName;
};

/**
 * @brief Gets the current language code used by the filament color data.
 */
std::string GetLanguageCode()
{
    if (wxGetApp().app_config == nullptr)
        return "en";

    const std::string language = wxGetApp().app_config->get("language");
    if (language.empty())
        return "en";
    if (language == "zh-cn")
        return "zh_CN";
    return language;
}

/**
 * @brief Converts an UTF-8 string to wxString.
 */
wxString FromStdString(const std::string& value)
{
    return wxString::FromUTF8(value.c_str());
}

/**
 * @brief Gets a localized color name with English fallback.
 */
std::string GetLocalizedColorName(const FilamentColor& color, const std::string& language_code)
{
    const std::string primary_color = FilamentColorUtils::GetPrimaryColor(color.colors, color.primaryColor);

    std::unordered_map<std::string, std::string>::const_iterator localized_name = color.colorNames.find(language_code);
    if (localized_name != color.colorNames.end() && !localized_name->second.empty())
        return localized_name->second;

    std::unordered_map<std::string, std::string>::const_iterator english_name = color.colorNames.find("en");
    if (english_name != color.colorNames.end() && !english_name->second.empty())
        return english_name->second;

    return primary_color;
}

/**
 * @brief Normalizes all valid colors from an official color record.
 */
std::vector<std::string> NormalizeColorList(const FilamentColor& color)
{
    std::vector<std::string> colors;
    colors.reserve(color.colors.size());
    for (const std::string& item : color.colors)
    {
        const std::string normalized = FilamentColorUtils::NormalizeHexColor(item);
        if (!normalized.empty())
            colors.emplace_back(normalized);
    }

    if (colors.empty())
    {
        const std::string normalized = FilamentColorUtils::NormalizeHexColor(color.primaryColor);
        if (!normalized.empty())
            colors.emplace_back(normalized);
    }

    return colors;
}

/**
 * @brief Builds a selectable result from a built-in color.
 */
FilamentColorSelection MakeFilamentColorSelection(const FilamentColor& color, const std::string& language_code)
{
    FilamentColorSelection selection;
    selection.name = GetLocalizedColorName(color, language_code);
    selection.sku = color.sku;
    selection.mode = FilamentColorUtils::NormalizeColourMode(color.mode);
    selection.isCustom = false;
    selection.colors = NormalizeColorList(color);

    if (selection.colors.empty())
    {
        const std::string normalized = FilamentColorUtils::NormalizeHexColor(color.primaryColor);
        if (!normalized.empty())
            selection.colors.emplace_back(normalized);
    }

    selection.primaryColor = FilamentColorUtils::GetPrimaryColor(selection.colors, color.primaryColor);
    if (selection.colors.size() <= 1)
        selection.mode = 0;
    return selection;
}

/**
 * @brief Builds a selectable result from a custom color.
 */
FilamentColorSelection MakeCustomColorSelection(const std::string& multi_colors,
    int mode,
    const std::string& fallback_color)
{
    FilamentColorSelection selection;
    selection.colors = FilamentColorUtils::SplitMultiColors(multi_colors);
    if (selection.colors.empty())
    {
        const std::string fallback = FilamentColorUtils::NormalizeHexColor(fallback_color, "#FFFFFF");
        selection.colors.emplace_back(fallback.empty() ? "#FFFFFF" : fallback);
    }

    selection.primaryColor = FilamentColorUtils::GetPrimaryColor(selection.colors, fallback_color);
    selection.name = selection.primaryColor;
    selection.mode = selection.colors.size() > 1 ? FilamentColorUtils::NormalizeColourMode(mode) : 0;
    selection.isCustom = true;
    return selection;
}

/**
 * @brief Gets the currently saved single custom color for official color fallback matching.
 */
std::string GetSingleFallbackColor(const FilamentColorDialogContext& context)
{
    std::vector<std::string> colors = FilamentColorUtils::SplitMultiColors(context.currentMultiColors);
    if (colors.size() == 1)
        return colors.front();

    if (colors.empty())
        return FilamentColorUtils::NormalizeHexColor(context.fallbackColor);

    return {};
}

/**
 * @brief Finds an official single-color item matching the current saved custom color.
 */
std::vector<FilamentColor>::const_iterator FindColorBySingleColor(const FilamentMaterial& material,
                                                                  const std::string& color)
{
    if (color.empty())
        return material.colors.end();

    return std::find_if(material.colors.begin(),
        material.colors.end(),
        [&color](const FilamentColor& item)
        {
            const std::vector<std::string> colors = NormalizeColorList(item);
            return colors.size() == 1 && colors.front() == color;
        });
}

/**
 * @brief Converts a color to HSV for sorting.
 */
SortHsv MakeSortHsvFromHex(const std::string& color)
{
    wxColour wx_color(color);
    if (!wx_color.IsOk())
        return SortHsv();

    SortHsv hsv;
    RGB2HSV(static_cast<float>(wx_color.Red()) / 255.0f,
        static_cast<float>(wx_color.Green()) / 255.0f,
        static_cast<float>(wx_color.Blue()) / 255.0f,
        &hsv.h,
        &hsv.s,
        &hsv.v);
    if (hsv.h < 0.0f)
        hsv.h += 360.0f;
    return hsv;
}

/**
 * @brief Gets the display type order used by the color grid.
 */
int GetColorTypeOrder(const std::vector<std::string>& colors, int mode)
{
    if (colors.size() <= 1)
        return 0;
    return FilamentColorUtils::NormalizeColourMode(mode) == 1 ? 2 : 1;
}

/**
 * @brief Compares two HSV values.
 */
int CompareHsv(const SortHsv& left, const SortHsv& right)
{
    if (left.h != right.h)
        return left.h < right.h ? -1 : 1;
    if (left.s != right.s)
        return left.s < right.s ? -1 : 1;
    if (left.v != right.v)
        return left.v < right.v ? -1 : 1;
    return 0;
}

/**
 * @brief Gets a language-specific name used as the final sort tie breaker.
 */
std::string GetColorNameForSort(const FilamentColor& color, const std::string& language_code)
{
    std::unordered_map<std::string, std::string>::const_iterator name = color.colorNames.find(language_code);
    return name != color.colorNames.end() ? name->second : std::string();
}

/**
 * @brief Builds a lightweight, precomputed sort key.
 */
ColorSortItem MakeColorSortItem(const FilamentColor& color)
{
    ColorSortItem item;
    item.color = &color;
    item.colors = NormalizeColorList(color);
    item.typeOrder = GetColorTypeOrder(item.colors, color.mode);
    item.hsvColors.reserve(item.colors.size());
    for (const std::string& hex_color : item.colors)
        item.hsvColors.emplace_back(MakeSortHsvFromHex(hex_color));
    item.enName = GetColorNameForSort(color, "en");
    item.zhName = GetColorNameForSort(color, "zh_CN");
    return item;
}

/**
 * @brief Compares two precomputed sort keys.
 */
bool ColorSortItemLess(const ColorSortItem& left, const ColorSortItem& right)
{
    if (left.color == nullptr)
        return false;
    if (right.color == nullptr)
        return true;

    if (left.typeOrder != right.typeOrder)
        return left.typeOrder < right.typeOrder;

    const size_t color_count = std::min(left.hsvColors.size(), right.hsvColors.size());
    for (size_t index = 0; index < color_count; ++index)
    {
        const int hsv_compare = CompareHsv(left.hsvColors[index], right.hsvColors[index]);
        if (hsv_compare != 0)
            return hsv_compare < 0;
    }

    if (left.colors.size() != right.colors.size())
        return left.colors.size() < right.colors.size();
    if (left.color->sku != right.color->sku)
        return left.color->sku < right.color->sku;
    if (left.enName != right.enName)
        return left.enName < right.enName;
    return left.zhName < right.zhName;
}

/**
 * @brief Builds sorted lightweight items without copying full color records.
 */
std::vector<ColorSortItem> BuildSortedColorItems(const std::vector<FilamentColor>& colors)
{
    std::vector<ColorSortItem> items;
    items.reserve(colors.size());
    for (const FilamentColor& color : colors)
        items.emplace_back(MakeColorSortItem(color));

    std::stable_sort(items.begin(), items.end(), ColorSortItemLess);
    return items;
}

/**
 * @brief Small swatch used by the filament color dialog.
 */
class FilamentColorSwatch : public wxPanel
{
public:
    FilamentColorSwatch(wxWindow* parent,
        const FilamentColor& color,
        const std::string& language_code,
        const wxSize& size)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
        , _color(&color)
        , _languageCode(language_code)
    {
        SetMinSize(size);
        SetMaxSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        SetToolTip(FromStdString(GetLocalizedColorName(color, language_code)));
        Bind(wxEVT_PAINT, &FilamentColorSwatch::OnPaint, this);
    }

    /**
     * @brief Updates the selected visual state.
     */
    void SetSelected(bool selected)
    {
        if (_selected == selected)
            return;
        _selected = selected;
        Refresh();
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF"))));
        dc.Clear();

        if (_color == nullptr)
            return;

        const int pad = FromDIP(3);
        const int bitmap_width = std::max(1, size.GetWidth() - pad * 2);
        const int bitmap_height = std::max(1, size.GetHeight() - pad * 2);
        const FilamentColorSelection selection = MakeFilamentColorSelection(*_color, _languageCode);
        wxBitmap* bitmap = FilamentColorUtils::GetFilamentColorIcon(
            selection.colors, selection.mode, "", bitmap_width, bitmap_height);
        if (bitmap != nullptr)
            dc.DrawBitmap(*bitmap, pad, pad, true);

        const wxColour border = _selected ? wxColour("#00AE42") : wxColour("#D0D0D0");
        dc.SetPen(wxPen(border, _selected ? FromDIP(2) : FromDIP(1)));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
    }

private:
    const FilamentColor* _color { nullptr };
    std::string _languageCode;
    bool _selected { false };
};

/**
 * @brief Dashed button used for the custom color entry.
 */
class MoreColorPanel : public wxPanel
{
public:
    MoreColorPanel(wxWindow* parent, const wxSize& size)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
    {
        SetMinSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &MoreColorPanel::OnPaint, this);
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF"))));
        dc.Clear();

        dc.SetPen(wxPen(wxColour("#D8D8D8"), FromDIP(1), wxPENSTYLE_SHORT_DASH));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());

        dc.SetFont(Label::Body_14);
        dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#262E30")));
        const wxString label = _L("+ More colors");
        wxCoord text_width = 0;
        wxCoord text_height = 0;
        dc.GetTextExtent(label, &text_width, &text_height);
        const int text_x = std::max(0, (size.GetWidth() - static_cast<int>(text_width)) / 2);
        const int text_y = std::max(0, (size.GetHeight() - static_cast<int>(text_height)) / 2);
        dc.DrawText(label, text_x, text_y);
    }
};

} // namespace

FilamentColorDialog::FilamentColorDialog(wxWindow* parent,
    const FilamentMaterial& material,
    const FilamentColorDialogContext& context)
    : DPIDialog(parent,
        wxID_ANY,
        wxEmptyString,
        wxDefaultPosition,
        wxDefaultSize,
        wxBORDER_NONE)
    , _material(material)
    , _languageCode(GetLanguageCode())
{
    std::vector<FilamentColor>::const_iterator current_color = std::find_if(_material.colors.begin(),
        _material.colors.end(),
        [&context](const FilamentColor& color)
        {
            return !context.currentSku.empty() && color.sku == context.currentSku;
        });
    if (current_color == _material.colors.end() && context.currentSku.empty())
    {
        current_color = FindColorBySingleColor(_material, GetSingleFallbackColor(context));
    }

    _highlightSku = current_color != _material.colors.end() ? current_color->sku : std::string();

    if (current_color != _material.colors.end())
        _selection = MakeFilamentColorSelection(*current_color, _languageCode);
    else
        _selection = MakeCustomColorSelection(context.currentMultiColors, context.currentMode, context.fallbackColor);

    BuildUi();
    UpdatePreview();
    UpdateSwatchSelection();
}

void FilamentColorDialog::BuildUi()
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));

    wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);
    const int outer_margin = 0;
    const int card_margin = FromDIP(20);
    const int dialog_width = FromDIP(448);
    const int content_width = dialog_width - card_margin * 2;

    wxPanel* card = new wxPanel(this, wxID_ANY);
    card->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    wxBoxSizer* card_sizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* current_row = new wxBoxSizer(wxHORIZONTAL);
    _previewBitmap = new wxStaticBitmap(card, wxID_ANY, wxBitmap(FromDIP(64), FromDIP(64)));
    current_row->Add(_previewBitmap, 0, wxALIGN_TOP | wxRIGHT, FromDIP(16));

    wxBoxSizer* info_sizer = new wxBoxSizer(wxVERTICAL);
    _nameLabel = new wxStaticText(card, wxID_ANY, wxEmptyString);
    _nameLabel->SetFont(Label::Head_16);
    _skuLabel = new wxStaticText(card, wxID_ANY, wxEmptyString);
    _skuLabel->SetFont(Label::Body_14);
    info_sizer->Add(_nameLabel, 0, wxBOTTOM, FromDIP(10));
    info_sizer->Add(_skuLabel, 0);
    current_row->Add(info_sizer, 1, wxALIGN_CENTER_VERTICAL);
    card_sizer->Add(current_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, card_margin);

    wxStaticText* official_label = new wxStaticText(card, wxID_ANY, _L("Official material"));
    official_label->SetFont(Label::Head_14);
    card_sizer->Add(official_label, 0, wxLEFT | wxRIGHT | wxTOP, card_margin);

    wxPanel* swatch_panel = new wxPanel(card, wxID_ANY);
    swatch_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    wxWrapSizer* swatch_sizer = new wxWrapSizer(wxHORIZONTAL);
    const wxSize swatch_size(FromDIP(32), FromDIP(32));
    const int swatch_gap = FromDIP(8);
    const std::vector<ColorSortItem> sorted_items = BuildSortedColorItems(_material.colors);
    for (const ColorSortItem& item : sorted_items)
    {
        if (item.color == nullptr)
            continue;

        const FilamentColor* color = item.color;
        FilamentColorSwatch* swatch = new FilamentColorSwatch(swatch_panel, *color, _languageCode, swatch_size);
        swatch->Bind(wxEVT_LEFT_UP, [this, color](wxMouseEvent&)
        {
            if (color != nullptr)
                SelectFilamentColor(*color);
        });
        swatch_sizer->Add(swatch, 0, wxRIGHT | wxBOTTOM, swatch_gap);
        _swatchBySku.emplace_back(swatch, color->sku);
    }
    const int swatch_columns = std::max(1, (content_width + swatch_gap) / (swatch_size.GetWidth() + swatch_gap));
    const int swatch_rows = _swatchBySku.empty()
        ? 0
        : static_cast<int>((_swatchBySku.size() + static_cast<size_t>(swatch_columns) - 1) /
            static_cast<size_t>(swatch_columns));
    const int swatch_panel_height = swatch_rows > 0
        ? swatch_rows * (swatch_size.GetHeight() + swatch_gap)
        : 0;
    swatch_panel->SetMinSize(wxSize(content_width, swatch_panel_height));
    swatch_panel->SetSizer(swatch_sizer);
    card_sizer->Add(swatch_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, card_margin);

    MoreColorPanel* more_button = new MoreColorPanel(card, wxSize(FromDIP(384), FromDIP(44)));
    more_button->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&)
    {
        OpenMoreColorDialog();
    });
    card_sizer->Add(more_button, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, card_margin);

    wxPanel* button_panel = new wxPanel(card, wxID_ANY);
    button_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    button_panel->SetMinSize(wxSize(-1, FromDIP(30)));
    wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
    Button* cancel = new Button(button_panel, _L("Cancel"), wxEmptyString, wxBORDER_NONE, 0, wxID_CANCEL);
    cancel->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    const wxSize button_size(FromDIP(70), FromDIP(30));
    cancel->SetMinSize(button_size);
    cancel->SetSize(button_size);
    cancel->SetCornerRadius(FromDIP(15));
    Button* ok = new Button(button_panel, _L("OK"), wxEmptyString, wxBORDER_NONE, 0, wxID_OK);
    ok->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    ok->SetMinSize(button_size);
    ok->SetSize(button_size);
    ok->SetCornerRadius(FromDIP(15));

    buttons->AddStretchSpacer();
    buttons->Add(cancel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));
    buttons->Add(ok, 0, wxALIGN_CENTER_VERTICAL);
    button_panel->SetSizer(buttons);
    card_sizer->Add(button_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, card_margin);

    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        EndModal(wxID_CANCEL);
    });

    ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        EndModal(wxID_OK);
    });

    card->SetSizer(card_sizer);
    root->Add(card, 0, wxEXPAND | wxALL, outer_margin);
    SetSizer(root);
    root->SetSizeHints(this);
    SetClientSize(wxSize(dialog_width, root->CalcMin().GetHeight()));
    SetMinClientSize(wxSize(dialog_width, root->CalcMin().GetHeight()));
    Layout();
    PlaceNearFilamentPanel();
}

void FilamentColorDialog::SelectFilamentColor(const FilamentColor& color)
{
    _selection = MakeFilamentColorSelection(color, _languageCode);
    _highlightSku = color.sku;
    UpdatePreview();
    UpdateSwatchSelection();
}

void FilamentColorDialog::SelectCustomColor(const std::string& color)
{
    const std::string normalized = FilamentColorUtils::NormalizeHexColor(color, "#FFFFFF");
    _selection = MakeCustomColorSelection(normalized, 0, normalized);
    _highlightSku.clear();
    UpdatePreview();
    UpdateSwatchSelection();
}

void FilamentColorDialog::UpdatePreview()
{
    if (_previewBitmap == nullptr || _nameLabel == nullptr || _skuLabel == nullptr)
        return;

    wxBitmap* bitmap = FilamentColorUtils::GetFilamentColorIcon(
        _selection.colors, _selection.mode, "", FromDIP(64), FromDIP(64));
    if (bitmap != nullptr)
        _previewBitmap->SetBitmap(*bitmap);

    const wxString name = _selection.name.empty() ? FromStdString(_selection.primaryColor) : FromStdString(_selection.name);
    _nameLabel->SetLabel(name);
    _skuLabel->SetLabel(FromStdString(_selection.sku));
    _skuLabel->Show(!_selection.sku.empty());
    Layout();
}

void FilamentColorDialog::UpdateSwatchSelection()
{
    for (const std::pair<wxWindow*, std::string>& item : _swatchBySku)
    {
        FilamentColorSwatch* swatch = dynamic_cast<FilamentColorSwatch*>(item.first);
        if (swatch != nullptr)
            swatch->SetSelected(!_highlightSku.empty() && item.second == _highlightSku);
    }
}

void FilamentColorDialog::on_dpi_changed(const wxRect&)
{
    Fit();
    PlaceNearFilamentPanel();
}

void FilamentColorDialog::PlaceNearFilamentPanel()
{
    wxWindow* parent = GetParent();
    wxWindow* top_window = parent != nullptr ? wxGetTopLevelParent(parent) : nullptr;
    wxWindow* anchor = nullptr;
    wxWindow* current = parent;
    const wxPoint top_position = top_window != nullptr ? top_window->GetScreenPosition() : wxPoint(0, 0);
    const int min_panel_width = FromDIP(300);
    const int max_panel_width = FromDIP(560);
    const int max_left_offset = FromDIP(120);
    int best_width = 0;

    while (current != nullptr && current != top_window)
    {
        const wxPoint current_position = current->GetScreenPosition();
        const int current_width = current->GetSize().GetWidth();
        const bool is_left_panel = current_position.x <= top_position.x + max_left_offset;
        const bool is_panel_width = current_width >= min_panel_width && current_width <= max_panel_width;
        if (is_left_panel && is_panel_width && current_width > best_width)
        {
            anchor = current;
            best_width = current_width;
        }
        current = current->GetParent();
    }

    const wxPoint anchor_position = anchor != nullptr ? anchor->GetScreenPosition() : top_position;
    const wxSize anchor_size = anchor != nullptr ? anchor->GetSize() : wxSize(FromDIP(420), 0);
    const wxSize dialog_size = GetSize();

    int x = anchor_position.x + anchor_size.GetWidth() + FromDIP(8);
    int y = top_position.y + FromDIP(160);

    const int display_index = wxDisplay::GetFromWindow(this);
    const wxDisplay display(display_index == wxNOT_FOUND ? 0 : static_cast<unsigned int>(display_index));
    const wxRect area = display.IsOk() ? display.GetClientArea() : wxRect(top_position, wxSize(1920, 1080));
    x = std::max(area.GetLeft() + FromDIP(8), std::min(x, area.GetRight() - dialog_size.GetWidth() - FromDIP(8)));
    y = std::max(area.GetTop() + FromDIP(8), std::min(y, area.GetBottom() - dialog_size.GetHeight() - FromDIP(8)));
    Move(wxPoint(x, y));
}

void FilamentColorDialog::OpenMoreColorDialog()
{
    wxColourData data;
    data.SetChooseFull(true);
    data.SetChooseAlpha(false);
    data.SetColour(wxColour(_selection.primaryColor));

    std::vector<std::string> custom_colors;
    if (wxGetApp().app_config != nullptr)
        custom_colors = wxGetApp().app_config->get_custom_color_from_config();

    const int custom_count = std::min(static_cast<int>(custom_colors.size()), CUSTOM_COLOR_COUNT);
    for (int index = 0; index < custom_count; ++index)
        data.SetCustomColour(index, string_to_wxColor(custom_colors[index]));

    wxColourDialog dialog(this, &data);
    dialog.SetTitle(_L("Please choose the filament color"));
    if (dialog.ShowModal() != wxID_OK)
        return;

    const wxColourData result = dialog.GetColourData();
    if (custom_colors.size() != CUSTOM_COLOR_COUNT)
        custom_colors.resize(CUSTOM_COLOR_COUNT);
    for (int index = 0; index < CUSTOM_COLOR_COUNT; ++index)
        custom_colors[index] = color_to_string(result.GetCustomColour(index));
    if (wxGetApp().app_config != nullptr)
        wxGetApp().app_config->save_custom_color_to_config(custom_colors);

    const wxColour color = result.GetColour();
    if (color.IsOk())
        SelectCustomColor(color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
}

} // namespace GUI
} // namespace Slic3r
