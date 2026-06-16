#include "SyncFilamentColorDialog.hpp"

#include <wx/stattext.h>
#include <wx/checkbox.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/image.h>
#include <wx/statbox.h>
#include <wx/dcgraph.h>
#include <cmath>
#include <algorithm>
#include <limits>

#include "SyncConfirmDialog.hpp"
#include "FilamentColorMapBoxGroup.hpp"
#include "FilamentSyncAlgorithm.hpp"
#include "PlaterPreview.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/SegmentedToggle.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/3DScene.hpp"

namespace
{

// --- Dialog layout (Figma specs) ---
constexpr int g_dialogWidth  = 620; // DIP
constexpr int g_dialogHeight = 665; // DIP

// Block widths (Figma) — centered independently in dialog
constexpr int g_block1W = 555; // Mode toggle
constexpr int g_block1H = 40;
constexpr int g_block4W = 571; // Bottom buttons
constexpr int g_block4H = 61;

// Block 3 wrapper panel styling
constexpr int g_block3BorderWidth = 1;
constexpr int g_block3Radius      = 4;
constexpr int g_block3Padding     = 16; // DIP — internal padding (all sides)
constexpr int g_block3HintGap     = 12; // DIP — hint label / separator / checkbox spacing

// Merged Block 2+3 wrapper margins
constexpr int g_block23PaddingH = 20; // DIP — left/right
constexpr int g_block23PaddingV = 12; // DIP — top/bottom

// Scrollbar
constexpr int g_scrollBarWidth = 10; // DIP — track width

// Button sizing
constexpr int g_btnW = 237;
constexpr int g_btnH = 36;

// Block 4 wrapper margins
constexpr int g_block4PaddingV = 12; // DIP — top/bottom
constexpr int g_block4PaddingH = 42; // DIP — left/right

// Top padding
constexpr int g_topPadding = 12; // DIP

// --- Color processing ---
constexpr int            g_colorMax      = 255; // max RGBA component value
constexpr int            g_alphaDecode   = 255; // noLight alpha decode: 255 - alpha
constexpr int            g_alphaMax      = 255; // fully opaque
constexpr int            g_alphaBkg      = 0;   // transparent background
constexpr int            g_noLightMin    = 0;   // minimum noLight brightness to use ratio
constexpr unsigned char  g_pixelTransparent = 0;

// Segmented toggle mode indices
constexpr int g_modeMapping   = 0;
constexpr int g_modeOverwrite = 1;

// --- Colors (matching color-mixing dialog style) ---
constexpr const char* g_dialogBg        = "#F8F7F7";
constexpr const char* g_blockBg         = "#FFFFFF";
constexpr const char* g_labelColor      = "#242424";
constexpr const char* g_secondaryBorder = "#D1D5DC";
constexpr const char* g_secondaryText   = "#242424";
constexpr const char* g_primaryBg       = "#019687";
constexpr const char* g_primaryHoverBg  = "#26A69A";
constexpr const char* g_primaryText     = "#FEFEFE";
constexpr const char* g_disabledBg      = "#DFDFDF";
constexpr const char* g_disabledText    = "#6B6A6A";
constexpr const char* g_block3BorderColor = "#F0F0F0";
constexpr const char* g_block3SeparatorColor = "#F3F4F6";
constexpr const char* g_secondaryHoverBg = "#F3F4F6";

} // namespace

namespace Slic3r
{
namespace GUI
{

// =====================================================================
// SyncFilamentColorDialog
// =====================================================================

SyncFilamentColorDialog::SyncFilamentColorDialog(wxWindow* parent,
                                                 const std::vector<FilamentData>& designDataList,
                                                 const std::vector<FilamentData>& machineDataList)
    : wxDialog(parent, wxID_ANY, _L("Device Filament Sync"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE)
    , m_designDataList(designDataList)
    , m_machineDataList(machineDataList)
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_dialogBg)));

    auto* topSizer = new wxBoxSizer(wxVERTICAL);

    // =====================================================================
    // Block 1: Mode toggle  (555 × 40, centered)
    // =====================================================================
    {
        auto* block = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        block->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_blockBg)));
        block->SetMinSize(wxSize(-1, FromDIP(g_block1H)));

        auto* blockSizer = new wxBoxSizer(wxVERTICAL);

        std::vector<wxString> modeOptions = { _L("Match Mapping"), _L("Direct Overwrite") };
        m_pModeToggle = new SegmentedToggle(block, modeOptions, g_modeMapping);
        m_pModeToggle->bindSelectionCallback([this](int index) {
            onModeChanged(index);
        });

        blockSizer->AddStretchSpacer();
        blockSizer->Add(m_pModeToggle, 0, wxALIGN_CENTER_HORIZONTAL);
        blockSizer->AddStretchSpacer();
        block->SetSizer(blockSizer);

        topSizer->Add(block, 0, wxEXPAND);
    }

    // =====================================================================
    // Block 2+3: Filament mapping + Preview/hint wrapper
    //   No scroll : L/R 20px, T/B 12px
    //   Scroll    : L 20px, R 10px (scrollbar in remaining 10px), T/B 12px
    // =====================================================================
    {
        auto* block = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        block->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_dialogBg)));

        // --- Filament mapping (was Block 2) ---
        m_pFilamentColorMapBoxGroup = new FilamentColorMapBoxGroup(block, m_designDataList, m_machineDataList);
        m_pFilamentColorMapBoxGroup->bindMappingChangedCallback([this]() {
            loadCoverPreview();
        });

        m_bNeedScroll = m_pFilamentColorMapBoxGroup->exceedsRowCount(2);

        // --- Preview wrapper (was Block 3) ---
        auto* previewWrapper = new wxPanel(block, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        previewWrapper->SetBackgroundStyle(wxBG_STYLE_PAINT);
        previewWrapper->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_blockBg)));
        previewWrapper->Bind(wxEVT_PAINT, [previewWrapper](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(previewWrapper);
            wxSize sz = previewWrapper->GetClientSize();
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour(g_dialogBg))));
            dc.DrawRectangle(0, 0, sz.x, sz.y);
            wxGCDC gdc(dc);
            gdc.SetPen(wxPen(wxColour(g_block3BorderColor), previewWrapper->FromDIP(g_block3BorderWidth)));
            gdc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour(g_blockBg))));
            gdc.DrawRoundedRectangle(0, 0, sz.x, sz.y, previewWrapper->FromDIP(g_block3Radius));
        });

        auto* contentSizer = new wxBoxSizer(wxVERTICAL);

        m_pPlaterPreview = new PlaterPreview(previewWrapper);
        contentSizer->Add(m_pPlaterPreview, 0, wxEXPAND);

        m_pHintCheckBoxPanel = new wxPanel(previewWrapper, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_pHintCheckBoxPanel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_blockBg)));

        auto* hintSizer = new wxBoxSizer(wxVERTICAL);

        m_pHintLabel = new wxStaticText(m_pHintCheckBoxPanel, wxID_ANY,
            _L("Note: Only filament type and color information are synchronized, nozzle order is not included."));
        m_pHintLabel->SetFont(Label::Body_12);
        m_pHintLabel->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
        m_pHintLabel->Wrap(FromDIP(460));
        hintSizer->Add(m_pHintLabel, 0, wxEXPAND | wxTOP, FromDIP(g_block3HintGap));

        // Separator: 1px horizontal line, color #F3F4F6, 12px gap on both sides
        hintSizer->AddSpacer(FromDIP(g_block3HintGap));
        auto* separator = new wxPanel(m_pHintCheckBoxPanel, wxID_ANY);
        separator->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_block3SeparatorColor)));
        separator->SetMinSize(wxSize(-1, FromDIP(1)));
        separator->SetMaxSize(wxSize(-1, FromDIP(1)));
        hintSizer->Add(separator, 0, wxEXPAND);
        hintSizer->AddSpacer(FromDIP(g_block3HintGap));

        m_pAddUnUsedMachineFilaments = new wxCheckBox(m_pHintCheckBoxPanel, wxID_ANY,
            _L("Add remaining printhead filaments to the software filament list"));
        m_pAddUnUsedMachineFilaments->SetFont(Label::Body_14);
        m_pAddUnUsedMachineFilaments->SetForegroundColour(StateColor::darkModeColorFor(wxColour(g_labelColor)));
        hintSizer->Add(m_pAddUnUsedMachineFilaments, 0, wxEXPAND | wxBOTTOM, FromDIP(g_block3HintGap));

        m_pHintCheckBoxPanel->SetSizer(hintSizer);
        contentSizer->Add(m_pHintCheckBoxPanel, 0, wxEXPAND);

        // Wrap content with 16 DIP padding on all four sides
        auto* innerPad = new wxBoxSizer(wxVERTICAL);
        innerPad->Add(contentSizer, 1, wxEXPAND | wxALL, FromDIP(g_block3Padding));
        previewWrapper->SetSizer(innerPad);

        // ============================================================
        // Assemble: scroll widgets are always created so the scrollbar
        //           can be shown / hidden when the mode changes.
        // ============================================================
        m_maxViewportHeight  = m_pFilamentColorMapBoxGroup->getHeightForRowCount(2);
        {
            int boxCount   = m_pFilamentColorMapBoxGroup->getVisibleBoxCount();
            int actualRows = (boxCount + 4) / 5; // 5 columns (g_gridCols), ceil division
            m_scrollContentHeight = m_pFilamentColorMapBoxGroup->getHeightForRowCount(actualRows);
        }

        // --- Scroll viewport (always created) ---
        m_pScrollViewport = new wxPanel(block, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                        wxBORDER_NONE | wxCLIP_CHILDREN);
        m_pScrollViewport->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_dialogBg)));
        m_pScrollViewport->SetDoubleBuffered(true);

        // Place the group inside the viewport unconditionally
        m_pFilamentColorMapBoxGroup->Reparent(m_pScrollViewport);
        m_pFilamentColorMapBoxGroup->SetPosition(wxPoint(0, 0));
        m_pFilamentColorMapBoxGroup->SetSize(-1, m_scrollContentHeight);

        m_pScrollViewport->Bind(wxEVT_SIZE, [this](wxSizeEvent& evt) {
            wxSize vpSz = evt.GetSize();
            if (m_pFilamentColorMapBoxGroup && vpSz.x > 0)
                m_pFilamentColorMapBoxGroup->SetSize(vpSz.x, m_scrollContentHeight);
            evt.Skip();
        });

        m_pScrollViewport->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& evt) {
            if (m_pFilamentColorMapBoxGroup && m_pFilamentColorMapBoxGroup->hasOpenPicker()) {
                m_pFilamentColorMapBoxGroup->dismissOpenPicker();
                m_pScrollViewport->SetFocus();
            }
            int lines  = evt.GetWheelRotation() / evt.GetWheelDelta();
            int delta  = -lines * FilamentScrollBar::s_scrollStepLines;
            int curOff = m_pScrollBar ? m_pScrollBar->getScrollOffset() : 0;
            applyScrollOffset(curOff + delta);
        });

        // --- Scrollbar (always created) ---
        m_pScrollBar = new FilamentScrollBar(block, StateColor::darkModeColorFor(wxColour(g_dialogBg)));
        m_pScrollBar->SetMinSize(wxSize(FromDIP(g_scrollBarWidth), -1));
        m_pScrollBar->SetMaxSize(wxSize(FromDIP(g_scrollBarWidth), -1));
        m_pScrollBar->setScrollRange(m_scrollContentHeight, m_maxViewportHeight);
        m_pScrollBar->setOnScroll([this](int offset) {
            applyScrollOffset(offset);
        });

        // Dynamic gap: 10 px when scrollbar is shown, 20 px when hidden,
        // so the total right-side spacing is always 20 px.
        m_pScrollGap = new wxPanel(block, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_pScrollGap->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_dialogBg)));

        auto* viewportRow = new wxBoxSizer(wxHORIZONTAL);
        viewportRow->Add(m_pScrollViewport, 1, wxEXPAND);
        viewportRow->Add(m_pScrollGap, 0, wxEXPAND);
        viewportRow->Add(m_pScrollBar, 0, wxEXPAND);

        auto* bodyVSizer = new wxBoxSizer(wxVERTICAL);
        bodyVSizer->Add(viewportRow, 0, wxEXPAND);

        // Preview row with fixed 20 px right margin
        auto* previewRow = new wxBoxSizer(wxHORIZONTAL);
        previewRow->Add(previewWrapper, 1, wxEXPAND);
        previewRow->AddSpacer(FromDIP(g_block23PaddingH));
        bodyVSizer->Add(previewRow, 0, wxEXPAND | wxTOP, FromDIP(g_block23PaddingV));

        // T/B padding = 12px
        auto* vPad = new wxBoxSizer(wxVERTICAL);
        vPad->Add(bodyVSizer, 1, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(g_block23PaddingV));

        // L/R: left = 20, no right spacer (handled per-row above)
        auto* hPad = new wxBoxSizer(wxHORIZONTAL);
        hPad->AddSpacer(FromDIP(g_block23PaddingH));
        hPad->Add(vPad, 1, wxEXPAND);
        block->SetSizer(hPad);
        topSizer->Add(block, 0, wxEXPAND);

        // Apply initial state
        updateScrollState();
    }

    // =====================================================================
    // Block 4: Bottom buttons  (571 × 61, T/B 12, L/R 42)
    // =====================================================================
    {
        auto* block = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        block->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_blockBg)));
        block->SetMinSize(wxSize(-1, FromDIP(g_block4H)));

        auto* btnRow = new wxBoxSizer(wxHORIZONTAL);

        btnRow->AddStretchSpacer();

        m_pResetBtn = new Button(block, _L("Reset"));
        m_pResetBtn->SetMinSize(FromDIP(wxSize(g_btnW, g_btnH)));
        m_pResetBtn->SetCornerRadius(FromDIP(4));
        m_pResetBtn->SetBorderWidth(FromDIP(1));

        auto resetBg = StateColor(
            std::make_pair(wxColour(g_secondaryHoverBg), (int)StateColor::Hovered),
            std::make_pair(wxColour(g_blockBg), (int)StateColor::Normal));
        resetBg.setTakeFocusedAsHovered(false);
        m_pResetBtn->SetBackgroundColor(resetBg);
        m_pResetBtn->SetBorderColor(StateColor(wxColour(g_secondaryBorder)));
        m_pResetBtn->SetTextColor(StateColor(wxColour(g_secondaryText)));
        m_pResetBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onReset(); });
        btnRow->Add(m_pResetBtn, 0, wxALIGN_CENTER_VERTICAL);

        // Spring between buttons
        btnRow->AddStretchSpacer();

        m_pSyncBtn = new Button(block, _L("Sync Now"));
        m_pSyncBtn->SetMinSize(FromDIP(wxSize(g_btnW, g_btnH)));
        m_pSyncBtn->SetCornerRadius(FromDIP(4));
        m_pSyncBtn->SetBorderWidth(0);
        m_pSyncBtn->SetBackgroundColor(StateColor(
            std::pair(wxColour(g_disabledBg), (int)StateColor::Disabled),
            std::pair(wxColour(g_primaryHoverBg), (int)StateColor::Hovered),
            std::pair(wxColour(g_primaryBg), (int)StateColor::Normal)));
        m_pSyncBtn->SetTextColor(StateColor(
            std::pair(wxColour(g_disabledText), (int)StateColor::Disabled),
            std::pair(wxColour(g_primaryText), (int)StateColor::Normal)));
        m_pSyncBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onSync(); });
        btnRow->Add(m_pSyncBtn, 0, wxALIGN_CENTER_VERTICAL);

        // T/B = 12px margins
        auto* vPad = new wxBoxSizer(wxVERTICAL);
        vPad->Add(btnRow, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(g_block4PaddingV));
        // L/R = 42px margins
        auto* hPad = new wxBoxSizer(wxHORIZONTAL);
        hPad->Add(vPad, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(g_block4PaddingH));
        block->SetSizer(hPad);

        topSizer->Add(block, 0, wxEXPAND);
    }

    SetSizer(topSizer);
    initPlatePreview();
    onAutoMatch();
    Layout();
}

bool SyncFilamentColorDialog::Layout()
{
    bool ret = wxDialog::Layout();

    wxSize bestSize = GetBestSize();
    wxSize minSize  = GetMinSize();
    wxSize newSize(wxMax(bestSize.GetWidth(), minSize.GetWidth()),
                   wxMax(bestSize.GetHeight(), minSize.GetHeight()));
    if (newSize != GetSize()) {
        SetSize(newSize);
    }
    return ret;
}

std::vector<FilamentData> SyncFilamentColorDialog::getSyncDataList() const
{
    std::vector<FilamentData> dataList;
    if (!m_pFilamentColorMapBoxGroup)
        return dataList;

    dataList = m_pFilamentColorMapBoxGroup->getCurFilamentList();

    // In overwrite mode, first trim to machine filament count,
    // then remove NONE (empty) entries from the result.
    if (!m_bMappingMode) {
        if (dataList.size() > m_machineDataList.size())
            dataList.resize(m_machineDataList.size());
        dataList.erase(std::remove_if(dataList.begin(), dataList.end(),
            [](const FilamentData& d) { return is_none_filament(d); }),
            dataList.end());
    }

    if (isAddUnUsedMachineFilaments()) {
        std::set<unsigned int> usedMachineIndices;
        for (const auto& data : dataList) {
            usedMachineIndices.insert(data.m_index);
        }
        for (const auto& machineData : m_machineDataList) {
            if (usedMachineIndices.find(machineData.m_index) == usedMachineIndices.end()
                && !is_none_filament(machineData)) {
                dataList.push_back(machineData);
            }
        }
    }

    return dataList;
}

void SyncFilamentColorDialog::setOverwriteMode()
{
    if (m_pModeToggle)
        m_pModeToggle->setSelected(g_modeOverwrite);
    onModeChanged(g_modeOverwrite);
}

bool SyncFilamentColorDialog::isAddUnUsedMachineFilaments() const
{
    if (!m_bMappingMode)
        return false;
    return m_pAddUnUsedMachineFilaments && m_pAddUnUsedMachineFilaments->GetValue();
}

void SyncFilamentColorDialog::setHasMixedFilaments(bool has)
{
    m_hasMixedFilaments = has;
}

void SyncFilamentColorDialog::onReset()
{
    if (m_bMappingMode)
        onAutoMatch();
    else
        onCoverMatch();
}

void SyncFilamentColorDialog::onSync()
{
    if (m_hasMixedFilaments) {
        SyncConfirmDialog dlg(this,
            _L("Detected that consumables with color-mixing dependency will be replaced / removed. "
                "Performing this operation will update the color-mixing results. Proceed?"),
            wxYES_NO | wxICON_WARNING);
        dlg.CentreOnScreen();
        if (dlg.ShowModal() != wxID_YES)
            return;
    }
    EndModal(wxID_OK);
}

void SyncFilamentColorDialog::onModeChanged(int index)
{
    m_bMappingMode = (index == g_modeMapping);

    if (m_pFilamentColorMapBoxGroup) {
        if (m_bMappingMode)
            m_pFilamentColorMapBoxGroup->setVisibleCount(m_pFilamentColorMapBoxGroup->getBoxCount());
        else
            m_pFilamentColorMapBoxGroup->setVisibleCount(
                std::min(m_pFilamentColorMapBoxGroup->getBoxCount(),
                         static_cast<int>(m_machineDataList.size())));
    }

    // Hide hint label/checkbox and reset button in overwrite mode
    if (m_pHintCheckBoxPanel)
        m_pHintCheckBoxPanel->Show(m_bMappingMode);
    if (m_pResetBtn)
        m_pResetBtn->Show(m_bMappingMode);

    if (m_bMappingMode)
        onAutoMatch();
    else
        onCoverMatch();

    updateScrollState();
    Layout();
}

void SyncFilamentColorDialog::onAutoMatch()
{
    if (!m_pFilamentColorMapBoxGroup)
        return;

    // Mapping mode keeps filament count unchanged — no remap needed
    m_filamentIdRemap.clear();

    std::vector<FilamentData> design_vec(m_designDataList.begin(), m_designDataList.end());
    std::vector<FilamentData> machine_vec(m_machineDataList.begin(), m_machineDataList.end());

    std::vector<int> mapping = compute_color_match(design_vec, machine_vec);

    int idx = 0;
    for (int m_idx : mapping) {
        if (m_idx >= 0 && m_idx < static_cast<int>(m_machineDataList.size())) {
            auto it = m_machineDataList.begin();
            std::advance(it, m_idx);
            m_pFilamentColorMapBoxGroup->updateBoxBelowData(idx, *it);
        }
        ++idx;
    }
    m_pFilamentColorMapBoxGroup->setGroupBoxEnable(true, FilamentColorMapBox::ButtonType::Below);
}

void SyncFilamentColorDialog::onCoverMatch()
{
    if (!m_pFilamentColorMapBoxGroup)
        return;

    size_t designCount  = m_designDataList.size();
    size_t machineCount = m_machineDataList.size();

    // 1:1 positional mapping for UI display (includes NONE slots)
    for (size_t i = 0; i < designCount; ++i) {
        size_t m_idx = i % machineCount;
        auto it = m_machineDataList.begin();
        std::advance(it, m_idx);
        m_pFilamentColorMapBoxGroup->updateBoxBelowData(static_cast<int>(i), *it);
    }
    m_pFilamentColorMapBoxGroup->setGroupBoxEnable(false, FilamentColorMapBox::ButtonType::Below);

    // Build 1-based filament ID remap for overwrite mode.
    // Cycle through non-NONE machine filaments only.
    {
        std::vector<size_t> validPos;
        for (size_t j = 0; j < machineCount; ++j) {
            if (!is_none_filament(m_machineDataList[j]))
                validPos.push_back(j);
        }
        size_t validCount = validPos.size();

        std::vector<unsigned int> machinePosToNewId(machineCount, 0);
        unsigned int runningId = 0;
        for (size_t j = 0; j < machineCount; ++j) {
            if (!is_none_filament(m_machineDataList[j])) {
                ++runningId;
                machinePosToNewId[j] = runningId;
            }
        }

        m_filamentIdRemap.assign(designCount + 1, 0);
        for (size_t old_id = 1; old_id <= designCount; ++old_id) {
            size_t machine_pos = validPos[(old_id - 1) % validCount];
            m_filamentIdRemap[old_id] = machinePosToNewId[machine_pos];
        }
    }
}

void SyncFilamentColorDialog::initPlatePreview()
{
    Plater* plater = wxGetApp().plater();
    if (!plater || !m_pPlaterPreview)
        return;

    PartPlateList& plateList   = plater->get_partplate_list();
    unsigned int plateCount    = plateList.get_plate_count();
    unsigned int currentPlate  = plateList.get_curr_plate_index();

    m_pPlaterPreview->setTotalPlateCount(plateCount);
    m_pPlaterPreview->setCurrentPlate(currentPlate);

    m_pPlaterPreview->bindPlateSwitchCallback([this](unsigned int newPlateIndex) {
        loadPlateThumbnail(newPlateIndex);
    });

    loadPlateThumbnail(currentPlate);
}

void SyncFilamentColorDialog::loadPlateThumbnail(unsigned int plateIndex)
{
    Plater* plater = wxGetApp().plater();
    if (!plater || !m_pPlaterPreview)
        return;

    PartPlateList& plateList = plater->get_partplate_list();
    PartPlate* plate = plateList.get_plate(plateIndex);
    if (!plate || !plate->thumbnail_data.is_valid())
        return;

    wxBitmap originalBmp = thumbnailToBitmap(plate->thumbnail_data);
    m_pPlaterPreview->setOriginalPreview(originalBmp);

    loadCoverPreview();
}

void SyncFilamentColorDialog::loadCoverPreview()
{
    Plater* plater = wxGetApp().plater();
    if (!plater || !m_pPlaterPreview)
        return;

    unsigned int plateIndex = m_pPlaterPreview->getCurrentPlate();

    PartPlateList& plateList = plater->get_partplate_list();
    PartPlate* plate = plateList.get_plate(plateIndex);
    if (!plate || !plate->thumbnail_data.is_valid())
        return;

    std::vector<FilamentData> filamentMapping;
    if (m_pFilamentColorMapBoxGroup)
        filamentMapping = m_pFilamentColorMapBoxGroup->getCurFilamentList();

    // In overwrite mode, replace NONE colors with valid machine filament colors
    // so the preview doesn't show grey for empty machine slots.
    if (!m_bMappingMode && !filamentMapping.empty()) {
        std::vector<const FilamentData*> validMachine;
        for (const auto& d : m_machineDataList) {
            if (!is_none_filament(d))
                validMachine.push_back(&d);
        }
        if (!validMachine.empty()) {
            for (size_t i = 0; i < filamentMapping.size(); ++i) {
                FilamentData& fd = filamentMapping[i];
                if (is_none_filament(fd)) {
                    const FilamentData& vm = *validMachine[i % validMachine.size()];
                    fd.m_color_r = vm.m_color_r;
                    fd.m_color_g = vm.m_color_g;
                    fd.m_color_b = vm.m_color_b;
                }
            }
        }
    }

    if (plate->no_light_thumbnail_data.is_valid() && !filamentMapping.empty()) {
        wxBitmap coverBmp = generateCoverPreview(plate->thumbnail_data,
                                                  plate->no_light_thumbnail_data,
                                                  filamentMapping);
        m_pPlaterPreview->setCoverPreview(coverBmp);
    } else {
        m_pPlaterPreview->setCoverPreview(thumbnailToBitmap(plate->thumbnail_data));
    }
}

wxBitmap SyncFilamentColorDialog::generateCoverPreview(const ThumbnailData& thumb,
                                                        const ThumbnailData& noLightThumb,
                                                        const std::vector<FilamentData>& filamentMapping)
{
    if (thumb.width != noLightThumb.width || thumb.height != noLightThumb.height)
        return thumbnailToBitmap(thumb);

    wxImage image(thumb.width, thumb.height);
    image.InitAlpha();

    // Build 0-based color lookup: filament_id = 255 - noLight.px[3].
    // Use sequential list position as the key — it matches the
    // extruder-to-alpha encoding: alpha = 256 - extruder_id, 255 - alpha = extruder_id - 1.
    std::map<int, wxColour> colorMap;
    for (size_t idx = 0, size = filamentMapping.size(); idx < size; ++idx) {
        const FilamentData& fd = filamentMapping[idx];
        colorMap[idx] = wxColour(fd.m_color_r, fd.m_color_g, fd.m_color_b);
    }

    for (unsigned int r = 0; r < thumb.height; ++r) {
        unsigned int rr = (thumb.height - 1 - r) * thumb.width;
        for (unsigned int c = 0; c < thumb.width; ++c) {
            const unsigned char* originPx  = thumb.pixels.data() + 4 * (rr + c);
            const unsigned char* noLightPx = noLightThumb.pixels.data() + 4 * (rr + c);

            // Background / transparent pixel — copy original as-is
            if (originPx[3] == g_pixelTransparent) {
                image.SetRGB(c, r, originPx[0], originPx[1], originPx[2]);
                image.SetAlpha(c, r, originPx[3]);
                continue;
            }

            // TODO Directly cover the cases where the quantity of consumables exceeds the mapping limit for the model.

            // noLight alpha = 255 - (extruder_id - 1)  →  filament_id = 255 - alpha
            int filament_id = g_alphaDecode - noLightPx[3];

            auto it = colorMap.find(filament_id);
            if (it != colorMap.end()) {
                const wxColour& amsColor = it->second;

                int originRgb  = originPx[0]  + originPx[1]  + originPx[2];
                int noLightRgb = noLightPx[0] + noLightPx[1] + noLightPx[2];

                unsigned char newR, newG, newB;
                if (noLightRgb > g_noLightMin && originRgb >= noLightRgb) {
                    // Bright area: add lighting delta to the target color
                    newR = std::clamp(amsColor.Red()   + (originPx[0] - noLightPx[0]), 0, g_colorMax);
                    newG = std::clamp(amsColor.Green() + (originPx[1] - noLightPx[1]), 0, g_colorMax);
                    newB = std::clamp(amsColor.Blue()  + (originPx[2] - noLightPx[2]), 0, g_colorMax);
                } else if (noLightRgb > g_noLightMin) {
                    // Shadow area: scale target color by original/noLight ratio
                    float ratio = static_cast<float>(originRgb) / static_cast<float>(noLightRgb);
                    newR = std::clamp(static_cast<int>(amsColor.Red()   * ratio), 0, g_colorMax);
                    newG = std::clamp(static_cast<int>(amsColor.Green() * ratio), 0, g_colorMax);
                    newB = std::clamp(static_cast<int>(amsColor.Blue()  * ratio), 0, g_colorMax);
                } else {
                    newR = amsColor.Red();
                    newG = amsColor.Green();
                    newB = amsColor.Blue();
                }

                image.SetRGB(c, r, newR, newG, newB);
                image.SetAlpha(c, r, originPx[3]);
            } else {
                // TODO: Handle the case where filament_id is not found in colorMap
                image.SetRGB(c, r, originPx[0], originPx[1], originPx[2]);
                image.SetAlpha(c, r, originPx[3]);
            }
        }
    }

    return wxBitmap(image);
}

wxBitmap SyncFilamentColorDialog::thumbnailToBitmap(const ThumbnailData& thumb)
{
    wxImage image(thumb.width, thumb.height);
    image.InitAlpha();
    for (unsigned int r = 0; r < thumb.height; ++r) {
        unsigned int rr = (thumb.height - 1 - r) * thumb.width;
        for (unsigned int c = 0; c < thumb.width; ++c) {
            const unsigned char* px = thumb.pixels.data() + 4 * (rr + c);
            image.SetRGB(c, r, px[0], px[1], px[2]);
            image.SetAlpha(c, r, px[3]);
        }
    }
    return wxBitmap(image);
}

void SyncFilamentColorDialog::updateScrollState()
{
    if (!m_pFilamentColorMapBoxGroup || !m_pScrollBar || !m_pScrollGap || !m_pScrollViewport)
        return;

    bool needScroll = m_pFilamentColorMapBoxGroup->exceedsRowCount(2);
    m_bNeedScroll = needScroll;

    // Recalculate heights (visible count may have changed)
    m_maxViewportHeight  = m_pFilamentColorMapBoxGroup->getHeightForRowCount(2);
    {
        int boxCount   = m_pFilamentColorMapBoxGroup->getVisibleBoxCount();
        int actualRows = (boxCount + 4) / 5;
        m_scrollContentHeight = m_pFilamentColorMapBoxGroup->getHeightForRowCount(actualRows);
    }
    m_pFilamentColorMapBoxGroup->SetSize(-1, m_scrollContentHeight);

    if (needScroll) {
        // Constrain viewport to 2-row height
        m_pScrollViewport->SetMinSize(wxSize(-1, m_maxViewportHeight));
        m_pScrollViewport->SetMaxSize(wxSize(-1, m_maxViewportHeight));

        // Gap = 10 px + scrollbar 10 px = 20 px total right side
        m_pScrollGap->SetMinSize(wxSize(FromDIP(g_scrollBarWidth), 1));
        m_pScrollGap->SetMaxSize(wxSize(FromDIP(g_scrollBarWidth), 1));
        m_pScrollGap->Show();
        m_pScrollBar->Show();

        m_pScrollBar->setScrollRange(m_scrollContentHeight, m_maxViewportHeight);
        applyScrollOffset(0);
    } else {
        // Let viewport auto-size to the group
        m_pScrollViewport->SetMinSize(wxDefaultSize);
        m_pScrollViewport->SetMaxSize(wxDefaultSize);

        // Gap = full right margin (20 px), scrollbar hidden
        m_pScrollGap->SetMinSize(wxSize(FromDIP(g_block23PaddingH), 1));
        m_pScrollGap->SetMaxSize(wxSize(FromDIP(g_block23PaddingH), 1));
        m_pScrollGap->Show();
        m_pScrollBar->Hide();

        m_pFilamentColorMapBoxGroup->SetPosition(wxPoint(0, 0));
    }
}

void SyncFilamentColorDialog::setScrollViewport(int contentHeight, int viewportHeight)
{
    m_scrollContentHeight = contentHeight;
    m_maxViewportHeight   = viewportHeight;

    if (m_pScrollBar)
        m_pScrollBar->setScrollRange(contentHeight, viewportHeight);
}

void SyncFilamentColorDialog::applyScrollOffset(int offset)
{
    if (!m_pScrollViewport || !m_pFilamentColorMapBoxGroup || !m_pScrollBar)
        return;

    int maxOffset = std::max(0, m_scrollContentHeight - m_maxViewportHeight);
    int clamped   = std::max(0, std::min(offset, maxOffset));
    if (clamped == m_pScrollBar->getScrollOffset())
        return;

    // Move the group up within the viewport (negative Y scrolls content up)
    m_pFilamentColorMapBoxGroup->SetPosition(wxPoint(0, -clamped));

    m_pScrollViewport->Refresh();

    m_pScrollBar->setScrollOffset(clamped);
}

} // namespace GUI
} // namespace Slic3r
