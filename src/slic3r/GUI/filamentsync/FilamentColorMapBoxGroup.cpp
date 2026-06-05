#include "FilamentColorMapBoxGroup.hpp"

#include <wx/wrapsizer.h>
#include <algorithm>

#include "MachineFilamentPicker.hpp"

namespace
{

// --- Layout ---
constexpr int g_boxMargin = 4; // DIP — margin around each FilamentColorMapBox

// --- Default below (placeholder) ---
constexpr unsigned char g_placeholderGrey = 0xCC;

Slic3r::GUI::FilamentData makeDefaultBelow(unsigned int index)
{
    Slic3r::GUI::FilamentData d;
    d.m_index   = index;
    d.m_name    = "--";
    d.m_color_r = g_placeholderGrey;
    d.m_color_g = g_placeholderGrey;
    d.m_color_b = g_placeholderGrey;
    return d;
}

} // namespace

namespace Slic3r
{
namespace GUI
{

FilamentColorMapBoxGroup::FilamentColorMapBoxGroup(wxWindow* parent,
                                                   const std::vector<FilamentData>& designDataList,
                                                   const std::vector<FilamentData>& machineDataList)
    : wxPanel(parent, wxID_ANY)
    , m_designDataList(designDataList)
    , m_machineDataList(machineDataList)
{
    m_pWrapSizer = new wxWrapSizer(wxHORIZONTAL);

    int boxIndex = 0;
    for (const auto& designData : m_designDataList) {
        FilamentData initialBelow = makeDefaultBelow(designData.m_index);

        auto box = std::make_unique<FilamentColorMapBox>(this, designData, initialBelow);
        box->bindButton([this, boxIndex](const FilamentData&) {
            showMachineFilamentPicker(boxIndex);
        }, FilamentColorMapBox::ButtonType::Below);

        m_pWrapSizer->Add(box.get(), 0, wxALL, FromDIP(g_boxMargin));
        m_boxList.push_back(std::move(box));
        ++boxIndex;
    }

    SetSizer(m_pWrapSizer);
    Layout();
}

std::vector<FilamentData> FilamentColorMapBoxGroup::getCurFilamentList() const
{
    std::vector<FilamentData> result;
    for (const auto& box : m_boxList) {
        FilamentData data = box->getBelowData();
        result.push_back(data);
    }
    return result;
}

void FilamentColorMapBoxGroup::setGroupBoxEnable(bool bEnable, FilamentColorMapBox::ButtonType type)
{
    for (auto& box : m_boxList)
        box->setEnable(bEnable, type);
}

void FilamentColorMapBoxGroup::showMachineFilamentPicker(int boxIndex)
{
    if (boxIndex < 0 || boxIndex >= m_boxList.size())
        return;

    // Destroy any existing picker before opening a new one.
    if (m_pPicker) {
        m_pPicker->Destroy();
        m_pPicker = nullptr;
    }

    // Determine the currently-mapped machine filament index for pre-selection.
    auto it = m_boxList.begin();
    std::advance(it, boxIndex);
    unsigned int curMachineIndex = (*it)->getBelowData().m_index;

    auto* picker = new MachineFilamentPicker(this, m_machineDataList, curMachineIndex);

    // When user clicks a radio in the picker, update the target box.
    picker->bindSelectionCallback([this, boxIndex](const FilamentData& data) {
        updateBoxFilament(boxIndex, data);
        m_pPicker = nullptr;
    });

    picker->bindOnDismissCallback([this]() {
        m_pPicker = nullptr;
    });

    // Position the popup near the target box.
    wxPoint screenPos = m_boxList[boxIndex]->GetScreenPosition();
    screenPos.y += m_boxList[boxIndex]->GetSize().y;

    m_pPicker = picker;
    picker->popupAt(screenPos);
}

void FilamentColorMapBoxGroup::updateBoxBelowData(int boxIndex, const FilamentData& machineData)
{
    updateBoxFilament(boxIndex, machineData);
}

int FilamentColorMapBoxGroup::getBoxCount() const
{
    return m_boxList.size();
}

void FilamentColorMapBoxGroup::bindMappingChangedCallback(std::function<void()> cb)
{
    m_mappingChangedCallback = std::move(cb);
}

void FilamentColorMapBoxGroup::updateBoxFilament(int boxIndex, const FilamentData& machineData)
{
    if (boxIndex < 0 || boxIndex >= m_boxList.size())
        return;

    m_boxList[boxIndex]->updateBelowData(machineData);

    if (m_mappingChangedCallback)
        m_mappingChangedCallback();
}

} // namespace GUI
} // namespace Slic3r
