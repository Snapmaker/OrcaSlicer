#include "SyncFilamentColorDialog.hpp"

#include "../I18N.hpp"

#include <wx/radiobut.h>
#include <wx/stattext.h>
#include <wx/checkbox.h>
#include <wx/button.h>
#include <wx/sizer.h>

#include "FilamentColorMapBoxGroup.hpp"
#include "PlaterPreview.hpp"

SyncFilamentColorDialog::SyncFilamentColorDialog(wxWindow* parent,
                                                 const std::list<FilamentData>& designDataList,
                                                 const std::list<FilamentData>& machineDataList)
    : wxDialog(parent, wxID_ANY, _L("Device Filament Sync"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_designDataList(designDataList)
    , m_machineDataList(machineDataList)
{
    // TODO: implement full layout
}

std::list<FilamentData> SyncFilamentColorDialog::getSyncDataList() const
{
    // TODO: return final sync data list
    std::list<FilamentData> result;
    return result;
}

bool SyncFilamentColorDialog::isAddToSoftwareList() const
{
    // TODO: return add-to-software-list checkbox state
    return false;
}

void SyncFilamentColorDialog::onReset()
{
    // TODO: implement reset all mappings
}

void SyncFilamentColorDialog::onCancel()
{
    // TODO: implement cancel and close
}

void SyncFilamentColorDialog::onSync()
{
    // TODO: implement sync and close
}

void SyncFilamentColorDialog::onModeChanged(bool bIsMappingMode)
{
    // TODO: implement mode switch
}

void SyncFilamentColorDialog::onAutoMatch()
{
    // TODO: implement nearest-color auto-match algorithm
}

void SyncFilamentColorDialog::onCoverMatch()
{
    // TODO: implement sequential one-to-one override
}
