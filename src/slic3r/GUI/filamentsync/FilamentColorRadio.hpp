#pragma once

#include <wx/panel.h>

#include "FilamentData.hpp"

class wxStaticText;

class FilamentColorRadio : public wxPanel
{
public:
    explicit FilamentColorRadio(wxWindow* parent, const FilamentData& data);

    void setSelected(bool bSelected);
    bool isSelected() const;

    void bindSelectedClicked(FilamentInfoCallback cb);

    void updateData(const FilamentData& data);

    const FilamentData& getData() const;

private:
    void onRadioClicked();

    FilamentData m_machineData;
    bool         m_bSelected = false;

    wxPanel*      m_pIndexCircle = nullptr;
    wxStaticText* m_pNameLabel   = nullptr;
    wxPanel*      m_pRadioCircle = nullptr;

    FilamentInfoCallback m_selectedCallback = nullptr;
};
