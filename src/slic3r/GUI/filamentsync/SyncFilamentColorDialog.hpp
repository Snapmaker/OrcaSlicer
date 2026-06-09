#pragma once

#include <list>

#include <wx/dialog.h>

#include "FilamentData.hpp"

#include "slic3r/GUI/Widgets/Button.hpp"

class wxStaticText;
class wxCheckBox;

namespace Slic3r
{

struct ThumbnailData;

namespace GUI
{

class FilamentColorMapBoxGroup;
class PlaterPreview;
class SegmentedToggle;

class SyncFilamentColorDialog : public wxDialog
{
public:
    SyncFilamentColorDialog(wxWindow* parent,
                            const std::vector<FilamentData>& designDataList,
                            const std::vector<FilamentData>& machineDataList);

    std::vector<FilamentData> getSyncDataList() const;
    bool isAddUnUsedMachineFilaments() const;
    const std::vector<unsigned int>& getFilamentIdRemap() const { return m_filamentIdRemap; }

    void setHasMixedFilaments(bool has);

private:
    void onReset();
    void onSync();
    void onModeChanged(int index);

    void onAutoMatch();
    void onCoverMatch();

    void initPlatePreview();
    void loadPlateThumbnail(unsigned int plateIndex);
    void loadCoverPreview();

    wxBitmap generateCoverPreview(const ThumbnailData& thumb,
                                   const ThumbnailData& noLightThumb,
                                   const std::vector<FilamentData>& filamentMapping);

    static wxBitmap thumbnailToBitmap(const ThumbnailData& thumb);

    // Mode selection (segmented toggle, same style as color-mixing)
    SegmentedToggle* m_pModeToggle = nullptr;

    FilamentColorMapBoxGroup* m_pFilamentColorMapBoxGroup = nullptr;
    PlaterPreview*            m_pPlaterPreview            = nullptr;

    wxStaticText* m_pHintLabel = nullptr;
    wxCheckBox*   m_pAddUnUsedMachineFilaments = nullptr;

    Button* m_pResetBtn = nullptr;
    Button* m_pSyncBtn  = nullptr;

    // Data
    std::vector<FilamentData> m_designDataList;
    std::vector<FilamentData> m_machineDataList;
    std::vector<unsigned int> m_filamentIdRemap;
    bool m_bMappingMode        = true;
    bool m_hasMixedFilaments   = false;
};

} // namespace GUI
} // namespace Slic3r
