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
 * @brief Filament color data shared by the dialog input and selected result.
 */
struct FilamentColorData
{
    std::vector<std::string> colors;
    int mode { 0 }; // 0 = solid/averaged, 1 = gradient
    std::string sku;
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
    FilamentColorDialog(wxWindow* parent, const FilamentMaterial& material, const FilamentColorData& currentColor);

    /**
     * @brief Gets the selected color result.
     */
    const FilamentColorData& Selection() const
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
    void UpdateRoundedShape();
    void BindDragWindow(wxWindow* window);
    void StartDrag(wxMouseEvent& event);
    void DragDialog(wxMouseEvent& event);
    void EndDrag(wxMouseEvent& event);
    void on_dpi_changed(const wxRect& suggestedRect) override;

private:
    FilamentMaterial _material;
    std::string _languageCode;
    FilamentColorData _selection;
    std::string _highlightSku;
    std::vector<std::pair<wxWindow*, std::string>> _swatchBySku;
    wxStaticBitmap* _previewBitmap { nullptr };
    wxStaticText* _nameLabel { nullptr };
    wxStaticText* _skuLabel { nullptr };
    bool _dragPending { false };
    bool _isDragging { false };
    wxPoint _dragStartMouse;
    wxPoint _dragStartPosition;
};

} // namespace GUI
} // namespace Slic3r
