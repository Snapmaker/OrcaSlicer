#pragma once

#include <list>
#include <memory>

#include <wx/popupwin.h>

#include "FilamentData.hpp"


class wxSizer;

namespace Slic3r
{
namespace GUI
{

class FilamentColorRadio;

class MachineFilamentPicker : public wxPopupTransientWindow
{
public:
    MachineFilamentPicker(wxWindow* parent,
                          const std::list<FilamentData>& dataList,
                          unsigned int curIndex);

    FilamentData getSelectedData() const;
    unsigned int getSelectedIndex() const;

    void setSelectedIndex(unsigned int index);

    void popupAt(const wxPoint& pos);

    void bindSelectionCallback(FilamentInfoCallback cb);
    void bindOnDismissCallback(std::function<void()> cb);

    void OnDismiss() override;

private:
    void onRadioClicked(const FilamentData& data);
    void deselectAllExcept(const FilamentData& currentData);

    std::list<std::shared_ptr<FilamentColorRadio>> m_radioList;
    FilamentData m_currentSelectedData;

    wxSizer* m_pWrapSizer = nullptr;

    FilamentInfoCallback m_selectionCallback = nullptr;
    std::function<void()> m_onDismissCallback = nullptr;
};

} // namespace GUI
} // namespace Slic3r
