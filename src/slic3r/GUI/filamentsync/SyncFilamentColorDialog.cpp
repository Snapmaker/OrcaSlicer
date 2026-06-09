#include "SyncFilamentColorDialog.hpp"

#include <wx/stattext.h>
#include <wx/checkbox.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/image.h>
#include <wx/statbox.h>
#include <cmath>
#include <algorithm>
#include <limits>

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
constexpr int g_dialogWidth  = 572; // DIP
constexpr int g_dialogHeight = 677; // DIP

// Block widths (Figma) — centered independently in dialog
constexpr int g_block1W = 555; // Mode toggle
constexpr int g_block1H = 40;
constexpr int g_block2W = 571; // Filament mapping
constexpr int g_block2H = 130;
constexpr int g_block3W = 571; // Plate preview + hints
constexpr int g_block3H = 386; // 677 - 4 - 40 - 12 - 130 - 12 - 81 - 12 = 386
constexpr int g_block4W = 571; // Bottom buttons
constexpr int g_block4H = 81;

// Gaps between blocks
constexpr int g_gap1_2 = 12;
constexpr int g_gap2_3 = 12;
constexpr int g_gap3_4 = 12;

// Top padding: Block4 fills to bottom, no bottom padding
constexpr int g_topPadding = 4; // DIP

// Internal block padding
constexpr int g_blockPaddingH = 20; // DIP — horizontal
constexpr int g_blockPaddingV = 12; // DIP — vertical

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
constexpr const char* g_primaryText     = "#FEFEFE";
constexpr const char* g_disabledBg      = "#DFDFDF";
constexpr const char* g_disabledText    = "#6B6A6A";

} // namespace

namespace Slic3r
{
namespace GUI
{

SyncFilamentColorDialog::SyncFilamentColorDialog(wxWindow* parent,
                                                 const std::vector<FilamentData>& designDataList,
                                                 const std::vector<FilamentData>& machineDataList)
    : wxDialog(parent, wxID_ANY, _L("Device Filament Sync"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE)
    , m_designDataList(designDataList)
    , m_machineDataList(machineDataList)
{
    const wxSize dialogSize = FromDIP(wxSize(g_dialogWidth, g_dialogHeight));
    SetSize(dialogSize);
    SetMinSize(dialogSize);
    SetMaxSize(dialogSize);
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_dialogBg)));

    auto* topSizer = new wxBoxSizer(wxVERTICAL);

    topSizer->AddSpacer(FromDIP(g_topPadding));

    // =====================================================================
    // Block 1: Mode toggle  (555 × 40, centered)
    // =====================================================================
    {
        auto* block = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        block->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_blockBg)));
        block->SetMinSize(FromDIP(wxSize(g_block1W, g_block1H)));

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

        topSizer->Add(block, 0, wxALIGN_CENTER_HORIZONTAL);
    }

    topSizer->AddSpacer(FromDIP(g_gap1_2));

    // =====================================================================
    // Block 2: Filament mapping  (571 × 130)
    // =====================================================================
    {
        auto* block = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        block->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_blockBg)));
        block->SetMinSize(FromDIP(wxSize(g_block2W, g_block2H)));

        auto* blockSizer = new wxBoxSizer(wxVERTICAL);
        auto* mappingRow = new wxBoxSizer(wxHORIZONTAL);

        m_pFilamentColorMapBoxGroup = new FilamentColorMapBoxGroup(block, m_designDataList, m_machineDataList);
        m_pFilamentColorMapBoxGroup->bindMappingChangedCallback([this]() {
            loadCoverPreview();
        });
        mappingRow->Add(m_pFilamentColorMapBoxGroup, 1, wxEXPAND | wxALL, FromDIP(g_blockPaddingV));

        blockSizer->Add(mappingRow, 0, wxEXPAND);
        block->SetSizer(blockSizer);
        topSizer->Add(block, 0, wxALIGN_CENTER_HORIZONTAL);
    }

    topSizer->AddSpacer(FromDIP(g_gap2_3));

    // =====================================================================
    // Block 3: Plate preview + hints  (571 × 386)
    // =====================================================================
    {
        auto* block = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        block->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_blockBg)));
        block->SetMinSize(FromDIP(wxSize(g_block3W, g_block3H)));

        auto* blockSizer = new wxBoxSizer(wxVERTICAL);

        m_pPlaterPreview = new PlaterPreview(block);
        blockSizer->Add(m_pPlaterPreview, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(g_blockPaddingH));

        m_pHintLabel = new wxStaticText(block, wxID_ANY,
            _L("Note: Only filament type and color information are synchronized, nozzle order is not included."));
        m_pHintLabel->SetFont(Label::Body_12);
        m_pHintLabel->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
        m_pHintLabel->Wrap(FromDIP(460));
        blockSizer->Add(m_pHintLabel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(g_blockPaddingH));

        m_pAddUnUsedMachineFilaments = new wxCheckBox(block, wxID_ANY,
            _L("Add remaining printhead filaments to the software filament list"));
        m_pAddUnUsedMachineFilaments->SetFont(Label::Body_12);
        m_pAddUnUsedMachineFilaments->SetForegroundColour(StateColor::darkModeColorFor(wxColour(g_labelColor)));
        blockSizer->Add(m_pAddUnUsedMachineFilaments, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,
                        FromDIP(g_blockPaddingH));

        block->SetSizer(blockSizer);
        topSizer->Add(block, 0, wxALIGN_CENTER_HORIZONTAL);
    }

    topSizer->AddSpacer(FromDIP(g_gap3_4));

    // =====================================================================
    // Block 4: Bottom buttons  (571 × 61, centered)
    // =====================================================================
    {
        auto* block = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        block->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_blockBg)));
        block->SetMinSize(FromDIP(wxSize(g_block4W, g_block4H)));

        auto* blockSizer = new wxBoxSizer(wxVERTICAL);

        auto* btnRow = new wxBoxSizer(wxHORIZONTAL);

        constexpr int g_btnW = 238; // ~237.5 DIP
        constexpr int g_btnH = 36;

        btnRow->AddStretchSpacer();

        m_pResetBtn = new Button(block, _L("Reset"));
        m_pResetBtn->SetMinSize(FromDIP(wxSize(g_btnW, g_btnH)));
        m_pResetBtn->SetCornerRadius(FromDIP(4));
        m_pResetBtn->SetBorderWidth(FromDIP(1));
        m_pResetBtn->SetBorderColorNormal(wxColour(g_secondaryBorder));
        m_pResetBtn->SetBackgroundColorNormal(wxColour(g_blockBg));
        m_pResetBtn->SetTextColorNormal(wxColour(g_secondaryText));
        m_pResetBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onReset(); });
        btnRow->Add(m_pResetBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

        m_pSyncBtn = new Button(block, _L("Sync Now"));
        m_pSyncBtn->SetMinSize(FromDIP(wxSize(g_btnW, g_btnH)));
        m_pSyncBtn->SetCornerRadius(FromDIP(4));
        m_pSyncBtn->SetBorderWidth(0);
        m_pSyncBtn->SetBackgroundColor(StateColor(
            std::pair(wxColour(g_disabledBg), (int)StateColor::Disabled),
            std::pair(wxColour(g_primaryBg), (int)StateColor::Normal)));
        m_pSyncBtn->SetTextColor(StateColor(
            std::pair(wxColour(g_disabledText), (int)StateColor::Disabled),
            std::pair(wxColour(g_primaryText), (int)StateColor::Normal)));
        m_pSyncBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onSync(); });
        btnRow->Add(m_pSyncBtn, 0, wxALIGN_CENTER_VERTICAL);

        btnRow->AddStretchSpacer();

        blockSizer->Add(btnRow, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(g_blockPaddingH));
        block->SetSizer(blockSizer);

        topSizer->Add(block, 0, wxALIGN_CENTER_HORIZONTAL);
    }

    SetSizer(topSizer);
    Layout();

    initPlatePreview();
    onAutoMatch();
}

std::vector<FilamentData> SyncFilamentColorDialog::getSyncDataList() const
{
    std::vector<FilamentData> dataList;
    if (!m_pFilamentColorMapBoxGroup)
        return dataList;

    dataList = m_pFilamentColorMapBoxGroup->getCurFilamentList();

    // In overwrite mode, trim to machine filament count
    if (!m_bMappingMode && dataList.size() > m_machineDataList.size())
        dataList.resize(m_machineDataList.size());

    if (isAddUnUsedMachineFilaments()) {
        std::set<unsigned int> usedMachineIndices;
        for (const auto& data : dataList) {
            usedMachineIndices.insert(data.m_index);
        }
        for (const auto& machineData : m_machineDataList) {
            if (usedMachineIndices.find(machineData.m_index) == usedMachineIndices.end()) {
                dataList.push_back(machineData);
            }
        }
    }

    return dataList;
}

bool SyncFilamentColorDialog::isAddUnUsedMachineFilaments() const
{
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
        MessageDialog dlg(this,
            _L("Mixed filament configuration is detected in the current project. "
               "Syncing may affect the mixed filament settings. "
               "Do you want to continue?"),
            _L("Mixed Filament Warning"),
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

    if (m_bMappingMode)
        onAutoMatch();
    else
        onCoverMatch();
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
    Layout();
}

void SyncFilamentColorDialog::onCoverMatch()
{
    if (!m_pFilamentColorMapBoxGroup)
        return;

    size_t designCount  = m_designDataList.size();
    size_t machineCount = m_machineDataList.size();

    std::vector<int> mapping = compute_direct_override(designCount, machineCount);

    int idx = 0;
    for (int m_idx : mapping) {
        if (m_idx >= 0 && m_idx < static_cast<int>(machineCount)) {
            auto it = m_machineDataList.begin();
            std::advance(it, m_idx);
            m_pFilamentColorMapBoxGroup->updateBoxBelowData(idx, *it);
        }
        ++idx;
    }
    m_pFilamentColorMapBoxGroup->setGroupBoxEnable(false, FilamentColorMapBox::ButtonType::Below);

    // Build 1-based filament ID remap for overwrite mode:
    // old_id -> ((old_id - 1) % machineCount) + 1
    m_filamentIdRemap.assign(designCount + 1, 0);
    if (machineCount > 0) {
        for (size_t old_id = 1; old_id <= designCount; ++old_id)
            m_filamentIdRemap[old_id] = static_cast<unsigned int>(((old_id - 1) % machineCount) + 1);
    }
    Layout();
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

} // namespace GUI
} // namespace Slic3r
