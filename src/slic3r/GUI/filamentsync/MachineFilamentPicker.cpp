#include "MachineFilamentPicker.hpp"

#include <wx/wrapsizer.h>
#include <wx/panel.h>

#include "FilamentColorRadio.hpp"

namespace
{

// --- Popup layout ---
constexpr int g_popupItemMargin     = 4;   // DIP — around each radio item
constexpr int g_maxItemsPerRow      = 6;   // max radios per row
constexpr int g_itemWidthEstimate   = 84;  // DIP — estimated width of one radio + margin
constexpr int g_popupPaddingWidth   = 20;  // DIP — extra popup border

} // namespace

namespace Slic3r
{
namespace GUI
{

MachineFilamentPicker::MachineFilamentPicker(wxWindow* parent,
                                             const std::list<FilamentData>& dataList,
                                             unsigned int curIndex)
    : wxPopupTransientWindow(parent)
{
    auto* panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    panel->SetBackgroundColour(*wxWHITE);

    m_pWrapSizer = new wxWrapSizer(wxHORIZONTAL);

    for (const auto& data : dataList) {
        auto radio = std::make_shared<FilamentColorRadio>(panel, data);
        radio->bindSelectedClicked([this](const FilamentData& d) { onRadioClicked(d); });

        m_pWrapSizer->Add(radio.get(), 0, wxALL, FromDIP(g_popupItemMargin));
        m_radioList.push_back(radio);
    }

    // Pre-select the current index
    if (curIndex < dataList.size()) {
        setSelectedIndex(curIndex);
    }

    panel->SetSizer(m_pWrapSizer);
    m_pWrapSizer->Fit(panel);

    // Size the popup to fit the panel, clamped to a reasonable max width
    wxSize panelSize = panel->GetBestSize();
    int    maxWidth  = FromDIP(g_maxItemsPerRow * g_itemWidthEstimate + g_popupPaddingWidth);
    if (panelSize.x > maxWidth)
        panelSize.x = maxWidth;
    panel->SetSize(panelSize);
    SetClientSize(panelSize);
}

FilamentData MachineFilamentPicker::getSelectedData() const
{
    return m_currentSelectedData;
}

unsigned int MachineFilamentPicker::getSelectedIndex() const
{
    return m_currentSelectedData.m_index;
}

void MachineFilamentPicker::setSelectedIndex(unsigned int index)
{
    for (auto& radio : m_radioList) {
        if (radio->getData().m_index == index) {
            radio->setSelected(true);
            m_currentSelectedData = radio->getData();
        } else {
            radio->setSelected(false);
        }
    }
}

void MachineFilamentPicker::popupAt(const wxPoint& pos)
{
    SetPosition(pos);
    Popup();
}

void MachineFilamentPicker::bindSelectionCallback(FilamentInfoCallback cb)
{
    m_selectionCallback = std::move(cb);
}

void MachineFilamentPicker::bindOnDismissCallback(std::function<void()> cb)
{
    m_onDismissCallback = std::move(cb);
}

void MachineFilamentPicker::OnDismiss()
{
    if (m_onDismissCallback)
        m_onDismissCallback();

    CallAfter([this]() { Destroy(); });
}

void MachineFilamentPicker::onRadioClicked(const FilamentData& data)
{
    m_currentSelectedData = data;
    deselectAllExcept(data);

    if (m_selectionCallback)
        m_selectionCallback(data);

    Dismiss();
}

void MachineFilamentPicker::deselectAllExcept(const FilamentData& currentData)
{
    for (auto& radio : m_radioList) {
        if (radio->getData().m_index != currentData.m_index)
            radio->setSelected(false);
    }
}

} // namespace GUI
} // namespace Slic3r
