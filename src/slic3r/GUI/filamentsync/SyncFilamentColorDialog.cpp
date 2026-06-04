#include "SyncFilamentColorDialog.hpp"

#include <wx/radiobut.h>
#include <wx/stattext.h>
#include <wx/checkbox.h>
#include <wx/button.h>
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
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Widgets/StaticLine.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/3DScene.hpp"

namespace
{

// --- Dialog layout ---
constexpr int g_dialogWidth        = 700; // DIP
constexpr int g_dialogHeight       = 600; // DIP
constexpr int g_sectionMargin      = 12;  // DIP — outer margin for major sections
constexpr int g_separatorMargin    = 8;   // DIP — left/right of separators
constexpr int g_radioButtonSpacing = 20;  // DIP — between mode radio buttons
constexpr int g_buttonSpacing      = 8;   // DIP — between bottom buttons
constexpr int g_hintLabelWrapWidth = 500; // DIP

// Sizer proportions (vertical)
constexpr int g_mappingGroupProportion = 1;
constexpr int g_platePreviewProportion = 2;

// --- Color processing ---
constexpr int            g_colorMax      = 255; // max RGBA component value
constexpr int            g_alphaDecode   = 255; // noLight alpha decode: 255 - alpha
constexpr int            g_alphaMax      = 255; // fully opaque
constexpr int            g_alphaBkg      = 0;   // transparent background
constexpr int            g_noLightMin    = 0;   // minimum noLight brightness to use ratio
constexpr unsigned char  g_pixelTransparent = 0;

} // namespace

namespace Slic3r
{
namespace GUI
{

SyncFilamentColorDialog::SyncFilamentColorDialog(wxWindow* parent,
                                                 const std::list<FilamentData>& designDataList,
                                                 const std::list<FilamentData>& machineDataList)
    : wxDialog(parent, wxID_ANY, _L("Device Filament Sync"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_designDataList(designDataList)
    , m_machineDataList(machineDataList)
{
    SetSize(FromDIP(wxSize(g_dialogWidth, g_dialogHeight)));

    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Mode selection ----
    auto* modeSizer = new wxBoxSizer(wxHORIZONTAL);
    m_pMappingModeRadio = new wxRadioButton(this, wxID_ANY, _L("Match Mapping"),
                                             wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    m_pMappingModeRadio->SetValue(true);
    m_pMappingModeRadio->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) {
        onModeChanged(true);
    });
    modeSizer->Add(m_pMappingModeRadio, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT,
                   FromDIP(g_radioButtonSpacing));

    m_pOverwriteModeRadio = new wxRadioButton(this, wxID_ANY, _L("Direct Overwrite"));
    m_pOverwriteModeRadio->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) {
        onModeChanged(false);
    });
    modeSizer->Add(m_pOverwriteModeRadio, 0, wxALIGN_CENTER_VERTICAL);

    mainSizer->Add(modeSizer, 0, wxEXPAND | wxALL, FromDIP(g_sectionMargin));

    // ---- Separator ----
    auto* sep1 = new StaticLine(this);
    sep1->SetLineColour("#CECECE");
    mainSizer->Add(sep1, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(g_separatorMargin));

    // ---- Filament mapping group ----
    m_pFilamentColorMapBoxGroup = new FilamentColorMapBoxGroup(this, m_designDataList, m_machineDataList);
    m_pFilamentColorMapBoxGroup->bindMappingChangedCallback([this]() {
        if (m_pPlaterPreview)
            m_pPlaterPreview->onCoverPreview();
    });
    mainSizer->Add(m_pFilamentColorMapBoxGroup, g_mappingGroupProportion, wxEXPAND | wxALL,
                   FromDIP(g_separatorMargin));

    // ---- Separator ----
    auto* sep2 = new StaticLine(this);
    sep2->SetLineColour("#CECECE");
    mainSizer->Add(sep2, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(g_separatorMargin));

    // ---- Plate preview ----
    m_pPlaterPreview = new PlaterPreview(this);
    mainSizer->Add(m_pPlaterPreview, g_platePreviewProportion, wxEXPAND | wxALL,
                   FromDIP(g_separatorMargin));

    // ---- Separator ----
    auto* sep3 = new StaticLine(this);
    sep3->SetLineColour("#CECECE");
    mainSizer->Add(sep3, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(g_separatorMargin));

    // ---- Hint label ----
    m_pHintLabel = new wxStaticText(this, wxID_ANY,
        _L("Note: Only filament type and color information are synchronized, nozzle order is not included."));
    m_pHintLabel->Wrap(FromDIP(g_hintLabelWrapWidth));
    mainSizer->Add(m_pHintLabel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(g_sectionMargin));

    // ---- Separator ----
    auto* sep4 = new StaticLine(this);
    sep4->SetLineColour("#CECECE");
    mainSizer->Add(sep4, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(g_separatorMargin));

    // ---- Add to software list checkbox ----
    m_pAddToSoftwareListCheck = new wxCheckBox(this, wxID_ANY,
        _L("Add remaining printhead filaments to the software filament list"));
    mainSizer->Add(m_pAddToSoftwareListCheck, 0, wxEXPAND | wxLEFT | wxRIGHT,
                   FromDIP(g_sectionMargin));

    // ---- Separator ----
    auto* sep5 = new StaticLine(this);
    sep5->SetLineColour("#CECECE");
    mainSizer->Add(sep5, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(g_separatorMargin));

    // ---- Bottom buttons ----
    auto* buttonSizer = new wxBoxSizer(wxHORIZONTAL);
    buttonSizer->AddStretchSpacer();

    m_pResetBtn = new wxButton(this, wxID_ANY, _L("Reset"));
    m_pResetBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onReset(); });
    buttonSizer->Add(m_pResetBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT,
                     FromDIP(g_buttonSpacing));

    m_pCancelBtn = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    m_pCancelBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onCancel(); });
    buttonSizer->Add(m_pCancelBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT,
                     FromDIP(g_buttonSpacing));

    m_pSyncBtn = new wxButton(this, wxID_OK, _L("Sync Now"));
    m_pSyncBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onSync(); });
    buttonSizer->Add(m_pSyncBtn, 0, wxALIGN_CENTER_VERTICAL);

    mainSizer->Add(buttonSizer, 0, wxEXPAND | wxALL, FromDIP(g_sectionMargin));

    SetSizer(mainSizer);
    Layout();
    CentreOnParent();

    initPlatePreview();
    onAutoMatch();
}

std::list<FilamentData> SyncFilamentColorDialog::getSyncDataList() const
{
    if (m_pFilamentColorMapBoxGroup)
        return m_pFilamentColorMapBoxGroup->getCurFilamentList();
    return {};
}

bool SyncFilamentColorDialog::isAddToSoftwareList() const
{
    return m_pAddToSoftwareListCheck && m_pAddToSoftwareListCheck->GetValue();
}

void SyncFilamentColorDialog::onReset()
{
    if (m_bMappingMode)
        onAutoMatch();
    else
        onCoverMatch();
}

void SyncFilamentColorDialog::onCancel()
{
    EndModal(wxID_CANCEL);
}

void SyncFilamentColorDialog::onSync()
{
    EndModal(wxID_OK);
}

void SyncFilamentColorDialog::onModeChanged(bool bIsMappingMode)
{
    m_bMappingMode = bIsMappingMode;
    if (bIsMappingMode)
        onAutoMatch();
    else
        onCoverMatch();
}

void SyncFilamentColorDialog::onAutoMatch()
{
    if (!m_pFilamentColorMapBoxGroup)
        return;

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
    Layout();
}

void SyncFilamentColorDialog::onCoverMatch()
{
    if (!m_pFilamentColorMapBoxGroup)
        return;

    std::vector<int> mapping = compute_direct_override(
        m_designDataList.size(),
        m_machineDataList.size());

    int idx = 0;
    for (int m_idx : mapping) {
        if (m_idx >= 0 && m_idx < static_cast<int>(m_machineDataList.size())) {
            auto it = m_machineDataList.begin();
            std::advance(it, m_idx);
            m_pFilamentColorMapBoxGroup->updateBoxBelowData(idx, *it);
        }
        ++idx;
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

    m_pPlaterPreview->bindCoverPreviewCallback([this]() {
        loadCoverPreview();
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

    std::list<FilamentData> filamentMapping;
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
                                                        const std::list<FilamentData>& filamentMapping)
{
    if (thumb.width != noLightThumb.width || thumb.height != noLightThumb.height)
        return thumbnailToBitmap(thumb);

    wxImage image(thumb.width, thumb.height);
    image.InitAlpha();

    // Build 0-based color lookup: filament_id = 255 - noLight.px[3].
    // Use sequential list position as the key — it matches the
    // extruder-to-alpha encoding: alpha = 256 - extruder_id, 255 - alpha = extruder_id - 1.
    std::map<int, wxColour> colorMap;
    int idx = 0;
    for (const auto& fd : filamentMapping) {
        colorMap[idx] = wxColour(fd.m_color_r, fd.m_color_g, fd.m_color_b);
        ++idx;
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
