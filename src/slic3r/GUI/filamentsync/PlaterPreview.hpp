#pragma once

#include <wx/panel.h>
#include <wx/bitmap.h>

class wxButton;
class wxComboBox;
class wxStaticBitmap;
class wxStaticText;

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

private:
    void onCoverPreview();
    void onPrePage();
    void onNextPage();
    void onPlateComboBoxChanged(int index);

    wxStaticBitmap* m_pOriginalThumbnail = nullptr;
    wxStaticBitmap* m_pCoverThumbnail    = nullptr;
    wxStaticText*   m_pOriginalLabel     = nullptr;
    wxStaticText*   m_pCoverLabel        = nullptr;

    wxButton*     m_pPrevPageBtn = nullptr;
    wxButton*     m_pNextPageBtn = nullptr;
    wxComboBox*   m_pPlateCombo  = nullptr;
    wxStaticText* m_pPlateLabel  = nullptr;

    unsigned int m_currentPlateIndex = 0;
    unsigned int m_totalPlateCount   = 1;
};
