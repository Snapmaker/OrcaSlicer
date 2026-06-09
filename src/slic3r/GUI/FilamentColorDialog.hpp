#pragma once

#include "GUI_Utils.hpp"
#include "FilamentColorLibrary.hpp"

#include <string>
#include <utility>
#include <vector>

class wxStaticBitmap;
class wxStaticText;

namespace Slic3r
{
namespace GUI
{

/**
 * @brief Result selected from the filament color dialog.
 */
struct FilamentColorSelection
{
    std::string name;
    std::string sku;
    int mode { 0 };
    std::string primaryColor;
    std::vector<std::string> colors;
    bool isCustom { false };
};

struct FilamentColorDialogContext
{
    std::string currentSku;
    std::string currentMultiColors;
    int currentMode { 0 };
    std::string currentPrimaryColor;
};

/**
 * @brief Dialog for choosing a built-in filament color or a custom color.
 */
class FilamentColorDialog : public DPIDialog
{
public:
    /**
     * @brief Creates the filament color dialog.
     */
    FilamentColorDialog(wxWindow* parent, const FilamentMaterial& material, const FilamentColorDialogContext& context);

    /**
     * @brief Gets the selected color result.
     */
    const FilamentColorSelection& Selection() const
    {
        return _selection;
    }

private:
    void BuildUi();
    void SelectFilamentColor(const FilamentColor& color);
    void SelectCustomColor(const std::string& color);
    void UpdatePreview();
    void UpdateSwatchSelection();
    void OpenMoreColorDialog();
    void PlaceNearFilamentPanel();
    void on_dpi_changed(const wxRect& suggestedRect) override;

private:
    FilamentMaterial _material;
    std::string _languageCode;
    FilamentColorSelection _selection;
    std::string _highlightSku;
    std::vector<std::pair<wxWindow*, std::string>> _swatchBySku;
    wxStaticBitmap* _previewBitmap { nullptr };
    wxStaticText* _nameLabel { nullptr };
    wxStaticText* _skuLabel { nullptr };
};

} // namespace GUI
} // namespace Slic3r
