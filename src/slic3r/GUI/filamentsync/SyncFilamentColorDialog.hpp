#pragma once

#include <list>

#include <wx/dialog.h>

#include "FilamentData.hpp"

class wxRadioButton;
class wxStaticText;
class wxCheckBox;
class wxButton;

namespace Slic3r
{

struct ThumbnailData;

namespace GUI
{

class FilamentColorMapBoxGroup;
class PlaterPreview;

class SyncFilamentColorDialog : public wxDialog
{
public:
    SyncFilamentColorDialog(wxWindow* parent,
                            const std::vector<FilamentData>& designDataList,
                            const std::vector<FilamentData>& machineDataList);

    std::vector<FilamentData> getSyncDataList() const;
    bool isAddUnUsedMachineFilaments() const;

private:
    void onReset();
    void onCancel();
    void onSync();
    void onModeChanged(bool bIsMappingMode);

    void onAutoMatch();
    void onCoverMatch();

    void initPlatePreview();
    void loadPlateThumbnail(unsigned int plateIndex);
    void loadCoverPreview();

    wxBitmap generateCoverPreview(const ThumbnailData& thumb,
                                   const ThumbnailData& noLightThumb,
                                   const std::vector<FilamentData>& filamentMapping);

    static wxBitmap thumbnailToBitmap(const ThumbnailData& thumb);

    // Mode selection
    wxRadioButton* m_pMappingModeRadio   = nullptr;
    wxRadioButton* m_pOverwriteModeRadio = nullptr;

    FilamentColorMapBoxGroup* m_pFilamentColorMapBoxGroup = nullptr;
    PlaterPreview* m_pPlaterPreview = nullptr;
    wxStaticText* m_pHintLabel = nullptr;
    wxCheckBox* m_pAddUnUsedMachineFilaments = nullptr;

    wxButton* m_pResetBtn = nullptr;
    wxButton* m_pCancelBtn = nullptr;
    wxButton* m_pSyncBtn  = nullptr;

    // Data
    std::vector<FilamentData> m_designDataList;
    std::vector<FilamentData> m_machineDataList;
    bool m_bMappingMode = true;
};

} // namespace GUI
} // namespace Slic3r
