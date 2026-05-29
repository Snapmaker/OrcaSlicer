#include "FilamentColorMapBox.hpp"

#include <wx/stattext.h>
#include <wx/sizer.h>

FilamentColorMapBox::FilamentColorMapBox(wxWindow* parent, const FilamentData& aboveData, const FilamentData& belowData)
    : wxPanel(parent, wxID_ANY)
    , m_aboveFilament(aboveData)
    , m_belowFilament(belowData)
{
    // TODO: implement layout
}

void FilamentColorMapBox::bindButton(FilamentInfoCallback cb, ButtonType type)
{
    // TODO: implement callback binding
}

void FilamentColorMapBox::setEnable(bool bEnable, ButtonType type)
{
    // TODO: implement enable/disable
}

void FilamentColorMapBox::updateAboveData(const FilamentData& data)
{
    // TODO: implement update above data
}

void FilamentColorMapBox::updateBelowData(const FilamentData& data)
{
    // TODO: implement update below data
}

const FilamentData& FilamentColorMapBox::getAboveData() const
{
    return m_aboveFilament;
}

const FilamentData& FilamentColorMapBox::getBelowData() const
{
    return m_belowFilament;
}

void FilamentColorMapBox::onAboveButtonClicked()
{
    // TODO: implement above button click handler
}

void FilamentColorMapBox::onBelowButtonClicked()
{
    // TODO: implement below button click handler
}
