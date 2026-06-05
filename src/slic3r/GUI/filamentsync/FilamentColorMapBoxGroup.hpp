#pragma once

#include <functional>
#include <list>
#include <memory>

#include <wx/panel.h>

#include "FilamentData.hpp"
#include "FilamentColorMapBox.hpp"


class wxSizer;

namespace Slic3r
{
namespace GUI
{

class MachineFilamentPicker;

class FilamentColorMapBoxGroup : public wxPanel
{
public:
    FilamentColorMapBoxGroup(wxWindow* parent,
                             const std::vector<FilamentData>& designDataList,
                             const std::vector<FilamentData>& machineDataList);

    std::vector<FilamentData> getCurFilamentList() const;

    void setGroupBoxEnable(bool bEnable, FilamentColorMapBox::ButtonType type);
    void showMachineFilamentPicker(int boxIndex);
    void updateBoxBelowData(int boxIndex, const FilamentData& machineData);
    int getBoxCount() const;

    void bindMappingChangedCallback(std::function<void()> cb);

private:
    void updateBoxFilament(int boxIndex, const FilamentData& machineData);

    std::vector<std::unique_ptr<FilamentColorMapBox>> m_boxList;
    std::vector<FilamentData> m_designDataList;
    std::vector<FilamentData> m_machineDataList;

    MachineFilamentPicker* m_pPicker = nullptr;

    wxSizer* m_pWrapSizer = nullptr;

    std::function<void()> m_mappingChangedCallback = nullptr;
};

} // namespace GUI
} // namespace Slic3r
