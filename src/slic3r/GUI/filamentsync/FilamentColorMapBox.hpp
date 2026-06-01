#pragma once

#include <wx/panel.h>

#include "FilamentData.hpp"

class wxStaticText;
namespace Slic3r
{
namespace GUI
{

class FilamentColorMapBox : public wxPanel
{
public:
    enum ButtonType
    {
        Above,
        Below
    };

    FilamentColorMapBox(wxWindow* parent, const FilamentData& aboveData, const FilamentData& belowData);

    void bindButton(FilamentInfoCallback cb, ButtonType type = ButtonType::Below);
    void setEnable(bool bEnable, ButtonType type = ButtonType::Below);

    void updateAboveData(const FilamentData& data);
    void updateBelowData(const FilamentData& data);

    const FilamentData& getAboveData() const;
    const FilamentData& getBelowData() const;

private:
    void onAboveButtonClicked();
    void onBelowButtonClicked();

    void applyColors();

    FilamentData m_aboveFilament;
    FilamentData m_belowFilament;

    bool m_bAboveEnabled = true;
    bool m_bBelowEnabled = true;

    wxPanel*      m_pAbovePanel = nullptr;
    wxStaticText* m_pAboveLabel = nullptr;
    wxPanel*      m_pBelowPanel = nullptr;
    wxStaticText* m_pBelowLabel = nullptr;

    FilamentInfoCallback m_aboveCallback = nullptr;
    FilamentInfoCallback m_belowCallback = nullptr;
};

} // namespace GUI
} // namespace Slic3r
