#pragma once

#include <functional>
#include <list>

#include <wx/dialog.h>
#include <wx/panel.h>

#include "FilamentData.hpp"
#include "FilamentScrollBar.hpp"

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

    bool Layout() override;

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

    void setScrollViewport(int contentHeight, int viewportHeight);
    void applyScrollOffset(int offset);
    void updateScrollState();

    // Mode selection (segmented toggle, same style as color-mixing)
    SegmentedToggle* m_pModeToggle = nullptr;

    FilamentColorMapBoxGroup* m_pFilamentColorMapBoxGroup = nullptr;
    PlaterPreview*            m_pPlaterPreview            = nullptr;

    FilamentScrollBar*  m_pScrollBar  = nullptr;
    wxPanel*            m_pScrollViewport = nullptr;
    wxPanel*            m_pScrollGap     = nullptr; // dynamic spacer (10 or 20 px)

    wxPanel*      m_pHintCheckBoxPanel         = nullptr;
    wxStaticText* m_pHintLabel                 = nullptr;
    wxCheckBox*   m_pAddUnUsedMachineFilaments = nullptr;

    Button* m_pResetBtn = nullptr;
    Button* m_pSyncBtn  = nullptr;

    int                 m_scrollContentHeight = 0;
    int                 m_maxViewportHeight   = 0;
    bool                m_bNeedScroll         = false;

    // Data
    std::vector<FilamentData> m_designDataList;
    std::vector<FilamentData> m_machineDataList;
    std::vector<unsigned int> m_filamentIdRemap;
    bool m_bMappingMode        = true;
    bool m_hasMixedFilaments   = false;
};

} // namespace GUI
} // namespace Slic3r
