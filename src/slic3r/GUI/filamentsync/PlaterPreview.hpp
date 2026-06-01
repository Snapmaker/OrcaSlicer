#pragma once

#include <functional>

#include <wx/panel.h>
#include <wx/bitmap.h>

class wxButton;
class wxComboBox;
class wxStaticBitmap;
class wxStaticText;

namespace Slic3r
{
namespace GUI
{

using PlateSwitchCallback  = std::function<void(unsigned int newPlateIndex)>;
using CoverPreviewCallback = std::function<void()>;

class PlaterPreview : public wxPanel
{
public:
    PlaterPreview(wxWindow* parent);

    void setOriginalPreview(const wxBitmap& thumbnail);
    void setCoverPreview(const wxBitmap& thumbnail);
    void updateCoverPreview(const wxBitmap& thumbnail);

    void setCurrentPlate(unsigned int plateIndex);
    unsigned int getCurrentPlate() const;

    void setTotalPlateCount(unsigned int count);

    void bindPlateSwitchCallback(PlateSwitchCallback cb);
    void bindCoverPreviewCallback(CoverPreviewCallback cb);

    void onCoverPreview();

private:
    void onPrePage();
    void onNextPage();
    void onPlateComboBoxChanged(int index);
    void updateNavButtons();

    void rescaleBitmaps();
    void rescaleOriginalBitmap();
    void rescaleCoverBitmap();

    wxStaticBitmap* m_pOriginalThumbnail = nullptr;
    wxStaticBitmap* m_pCoverThumbnail    = nullptr;
    wxStaticText*   m_pOriginalLabel     = nullptr;
    wxStaticText*   m_pCoverLabel        = nullptr;

    wxButton*     m_pPrevPageBtn = nullptr;
    wxButton*     m_pNextPageBtn = nullptr;
    wxComboBox*   m_pPlateCombo  = nullptr;
    wxStaticText* m_pPlateLabel  = nullptr;

    wxBitmap m_originalBitmap;
    wxBitmap m_coverBitmap;
    bool     m_isRescaling = false;

    unsigned int m_currentPlateIndex = 0;
    unsigned int m_totalPlateCount   = 1;

    PlateSwitchCallback  m_plateSwitchCallback  = nullptr;
    CoverPreviewCallback m_coverPreviewCallback = nullptr;
};

} // namespace GUI
} // namespace Slic3r
