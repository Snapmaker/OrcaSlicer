#pragma once

#include <list>
#include <memory>

#include <wx/panel.h>

#include "FilamentData.hpp"
#include "FilamentColorMapBox.hpp"

class wxSizer;
class MachineFilamentPicker;

class FilamentColorMapBoxGroup : public wxPanel
{
public:
    FilamentColorMapBoxGroup(wxWindow* parent,
                             const std::list<FilamentData>& designDataList,
                             const std::list<FilamentData>& machineDataList);

    std::list<FilamentData> getCurFilamentList() const;

    void setGroupBoxEnable(bool bEnable, FilamentColorMapBox::ButtonType type);

    void showMachineFilamentPicker(int boxIndex);

private:
    void onBoxBelowClicked(const FilamentData& aboveData);
    void updateBoxFilament(int boxIndex, const FilamentData& machineData);

    std::list<std::unique_ptr<FilamentColorMapBox>> m_boxList;
    std::list<FilamentData> m_designDataList;
    std::list<FilamentData> m_machineDataList;

    MachineFilamentPicker* m_pPicker = nullptr;

    wxSizer* m_pWrapSizer = nullptr;
};
