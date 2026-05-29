#pragma once

#include <list>

#include <wx/dialog.h>

#include "FilamentData.hpp"

class wxRadioButton;
class wxStaticText;
class wxCheckBox;
class wxButton;
class FilamentColorMapBoxGroup;
class PlaterPreview;

class SyncFilamentColorDialog : public wxDialog
{
public:
    SyncFilamentColorDialog(wxWindow* parent,
                            const std::list<FilamentData>& designDataList,
                            const std::list<FilamentData>& machineDataList);

    std::list<FilamentData> getSyncDataList() const;
    bool isAddToSoftwareList() const;

private:
    void onReset();
    void onCancel();
    void onSync();
    void onModeChanged(bool bIsMappingMode);

    void onAutoMatch();
    void onCoverMatch();

    wxRadioButton* m_pMappingModeRadio   = nullptr;
    wxRadioButton* m_pOverwriteModeRadio = nullptr;

    FilamentColorMapBoxGroup* m_pFilamentColorMapBoxGroup = nullptr;

    PlaterPreview* m_pPlaterPreview = nullptr;

    wxStaticText* m_pHintLabel = nullptr;
    wxCheckBox* m_pAddToSoftwareListCheck = nullptr;
    
    wxButton* m_pResetBtn = nullptr;
    wxButton* m_pCancelBtn = nullptr;
    wxButton* m_pSyncBtn  = nullptr;

    std::list<FilamentData> m_designDataList;
    std::list<FilamentData> m_machineDataList;
    bool m_bMappingMode = true;
};
