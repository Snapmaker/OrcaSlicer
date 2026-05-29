#include "PlaterPreview.hpp"

#include <wx/button.h>
#include <wx/combobox.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/sizer.h>

PlaterPreview::PlaterPreview(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    // TODO: implement layout
}

void PlaterPreview::setOriginalPreview(const wxBitmap& thumbnail)
{
    // TODO: implement set original thumbnail
}

void PlaterPreview::setCoverPreview(const wxBitmap& thumbnail)
{
    // TODO: implement set cover thumbnail
}

void PlaterPreview::updateCoverPreview(const wxBitmap& thumbnail)
{
    // TODO: implement update cover preview
}

void PlaterPreview::setCurrentPlate(unsigned int plateIndex)
{
    // TODO: implement jump to plate
}

unsigned int PlaterPreview::getCurrentPlate() const
{
    return m_currentPlateIndex;
}

void PlaterPreview::setTotalPlateCount(unsigned int count)
{
    // TODO: implement set total plate count
}

void PlaterPreview::onCoverPreview()
{
    // TODO: implement re-render on filament change
}

void PlaterPreview::onPrePage()
{
    // TODO: implement previous page
}

void PlaterPreview::onNextPage()
{
    // TODO: implement next page
}

void PlaterPreview::onPlateComboBoxChanged(int index)
{
    // TODO: implement combo box plate switch
}
