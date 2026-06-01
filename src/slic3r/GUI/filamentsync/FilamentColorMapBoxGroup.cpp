#include "FilamentColorMapBoxGroup.hpp"

#include <wx/sizer.h>

#include "MachineFilamentPicker.hpp"

namespace Slic3r
{
namespace GUI
{

FilamentColorMapBoxGroup::FilamentColorMapBoxGroup(wxWindow* parent,
                                                   const std::list<FilamentData>& designDataList,
                                                   const std::list<FilamentData>& machineDataList)
    : wxPanel(parent, wxID_ANY)
    , m_designDataList(designDataList)
    , m_machineDataList(machineDataList)
{
    // TODO: implement layout, create FilamentColorMapBox list
}

std::list<FilamentData> FilamentColorMapBoxGroup::getCurFilamentList() const
{
    // TODO: return current mapped filament list
    std::list<FilamentData> result;
    return result;
}

void FilamentColorMapBoxGroup::setGroupBoxEnable(bool bEnable, FilamentColorMapBox::ButtonType type)
{
    // TODO: implement group enable/disable
}

void FilamentColorMapBoxGroup::showMachineFilamentPicker(int boxIndex)
{
    // TODO: implement show picker
}

void FilamentColorMapBoxGroup::onBoxBelowClicked(const FilamentData& aboveData)
{
    // TODO: implement box below click handler
}

void FilamentColorMapBoxGroup::updateBoxFilament(int boxIndex, const FilamentData& machineData)
{
    // TODO: implement update box filament data
}

} // namespace GUI
} // namespace Slic3r
