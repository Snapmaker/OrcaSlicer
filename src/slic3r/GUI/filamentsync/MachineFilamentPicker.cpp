#include "MachineFilamentPicker.hpp"

#include <wx/sizer.h>

#include "FilamentColorRadio.hpp"

namespace Slic3r
{
namespace GUI
{

MachineFilamentPicker::MachineFilamentPicker(wxWindow* parent,
                                             const std::list<FilamentData>& dataList,
                                             unsigned int curIndex)
    : wxPopupTransientWindow(parent)
{
    // TODO: implement layout, create FilamentColorRadio list
}

FilamentData MachineFilamentPicker::getSelectedData() const
{
    // TODO: return current selected data
    return m_currentSelectedData;
}

unsigned int MachineFilamentPicker::getSelectedIndex() const
{
    // TODO: return current selected index
    return m_currentSelectedData.m_index;
}

void MachineFilamentPicker::setSelectedIndex(unsigned int index)
{
    // TODO: implement programmatic selection
}

void MachineFilamentPicker::popupAt(const wxPoint& pos)
{
    // TODO: position and show the popup at the given screen coordinates
}

void MachineFilamentPicker::onRadioClicked(const FilamentData& data)
{
    // TODO: implement radio click handler, then close popup and return result
}

void MachineFilamentPicker::deselectAllExcept(const FilamentData& currentData)
{
    // TODO: implement deselect all others
}

} // namespace GUI
} // namespace Slic3r
