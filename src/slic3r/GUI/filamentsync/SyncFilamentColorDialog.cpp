#include "SyncFilamentColorDialog.hpp"

#include <wx/radiobut.h>
#include <wx/stattext.h>
#include <wx/checkbox.h>
#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/image.h>
#include <wx/statbox.h>

#include "FilamentColorMapBoxGroup.hpp"
#include "PlaterPreview.hpp"

#include "libslic3r/GCode/ThumbnailData.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Widgets/StaticLine.hpp"

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
    SetSize(FromDIP(wxSize(700, 600)));

    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Mode selection ----
    auto* modeSizer = new wxBoxSizer(wxHORIZONTAL);
    m_pMappingModeRadio = new wxRadioButton(this, wxID_ANY, _L("Match Mapping"),
                                             wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    m_pMappingModeRadio->SetValue(true);
    m_pMappingModeRadio->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) {
        onModeChanged(true);
    });
    modeSizer->Add(m_pMappingModeRadio, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(20));

    m_pOverwriteModeRadio = new wxRadioButton(this, wxID_ANY, _L("Direct Overwrite"));
    m_pOverwriteModeRadio->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) {
        onModeChanged(false);
    });
    modeSizer->Add(m_pOverwriteModeRadio, 0, wxALIGN_CENTER_VERTICAL);

    mainSizer->Add(modeSizer, 0, wxEXPAND | wxALL, FromDIP(12));

    // ---- Separator ----
    auto* sep1 = new StaticLine(this);
    sep1->SetLineColour("#CECECE");
    mainSizer->Add(sep1, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

    // ---- Filament mapping group ----
    m_pFilamentColorMapBoxGroup = new FilamentColorMapBoxGroup(this, m_designDataList, m_machineDataList);
    mainSizer->Add(m_pFilamentColorMapBoxGroup, 1, wxEXPAND | wxALL, FromDIP(8));

    // ---- Separator ----
    auto* sep2 = new StaticLine(this);
    sep2->SetLineColour("#CECECE");
    mainSizer->Add(sep2, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

    // ---- Plate preview ----
    m_pPlaterPreview = new PlaterPreview(this);
    mainSizer->Add(m_pPlaterPreview, 2, wxEXPAND | wxALL, FromDIP(8));

    // ---- Separator ----
    auto* sep3 = new StaticLine(this);
    sep3->SetLineColour("#CECECE");
    mainSizer->Add(sep3, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

    // ---- Hint label ----
    m_pHintLabel = new wxStaticText(this, wxID_ANY,
        _L("Note: Only filament type and color information are synchronized, nozzle order is not included."));
    m_pHintLabel->Wrap(FromDIP(500));
    mainSizer->Add(m_pHintLabel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    // ---- Separator ----
    auto* sep4 = new StaticLine(this);
    sep4->SetLineColour("#CECECE");
    mainSizer->Add(sep4, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

    // ---- Add to software list checkbox ----
    m_pAddToSoftwareListCheck = new wxCheckBox(this, wxID_ANY,
        _L("Add remaining printhead filaments to the software filament list"));
    mainSizer->Add(m_pAddToSoftwareListCheck, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    // ---- Separator ----
    auto* sep5 = new StaticLine(this);
    sep5->SetLineColour("#CECECE");
    mainSizer->Add(sep5, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

    // ---- Bottom buttons ----
    auto* buttonSizer = new wxBoxSizer(wxHORIZONTAL);
    buttonSizer->AddStretchSpacer();

    m_pResetBtn = new wxButton(this, wxID_ANY, _L("Reset"));
    m_pResetBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onReset(); });
    buttonSizer->Add(m_pResetBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    m_pCancelBtn = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    m_pCancelBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onCancel(); });
    buttonSizer->Add(m_pCancelBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    m_pSyncBtn = new wxButton(this, wxID_OK, _L("Sync Now"));
    m_pSyncBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onSync(); });
    buttonSizer->Add(m_pSyncBtn, 0, wxALIGN_CENTER_VERTICAL);

    mainSizer->Add(buttonSizer, 0, wxEXPAND | wxALL, FromDIP(12));

    SetSizer(mainSizer);
    Layout();
    CentreOnParent();

    // Initialize plate preview with thumbnails and plate count
    initPlatePreview();
}

std::list<FilamentData> SyncFilamentColorDialog::getSyncDataList() const
{
    // TODO: return final sync data list
    std::list<FilamentData> result;
    return result;
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
    // TODO: write synced data back to project config
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
    // TODO: implement nearest-color auto-match algorithm
}

void SyncFilamentColorDialog::onCoverMatch()
{
    // TODO: implement sequential one-to-one override
}

void SyncFilamentColorDialog::initPlatePreview()
{
    Plater* plater = wxGetApp().plater();
    if (!plater || !m_pPlaterPreview)
        return;

    PartPlateList& plateList = plater->get_partplate_list();
    int plateCount = plateList.get_plate_count();
    int currentPlate = plateList.get_curr_plate_index();

    m_pPlaterPreview->setTotalPlateCount(static_cast<unsigned int>(plateCount));
    m_pPlaterPreview->setCurrentPlate(static_cast<unsigned int>(currentPlate));

    // Register plate switch callback: reload both thumbnails when user flips pages
    m_pPlaterPreview->bindPlateSwitchCallback([this](unsigned int newPlateIndex) {
        loadPlateThumbnail(newPlateIndex);
    });

    // Register cover preview callback: regenerate cover when filament mapping changes
    m_pPlaterPreview->bindCoverPreviewCallback([this]() {
        loadCoverPreview();
    });

    // Load thumbnails for the initial plate
    loadPlateThumbnail(static_cast<unsigned int>(currentPlate));
}

void SyncFilamentColorDialog::loadPlateThumbnail(unsigned int plateIndex)
{
    Plater* plater = wxGetApp().plater();
    if (!plater || !m_pPlaterPreview)
        return;

    PartPlateList& plateList = plater->get_partplate_list();
    PartPlate* plate = plateList.get_plate(static_cast<int>(plateIndex));
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

    // Get the plate index that PlaterPreview is currently showing (not the plater's current plate)
    unsigned int plateIndex = m_pPlaterPreview->getCurrentPlate();

    PartPlateList& plateList = plater->get_partplate_list();
    PartPlate* plate = plateList.get_plate(static_cast<int>(plateIndex));
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
    wxImage image(thumb.width, thumb.height);
    image.InitAlpha();

    std::map<unsigned int, std::array<float, 3>> colorMap;
    for (const auto& fd : filamentMapping) {
        colorMap[fd.m_index] = {fd.m_color_r / 255.0f, fd.m_color_g / 255.0f, fd.m_color_b / 255.0f};
    }

    for (unsigned int r = 0; r < thumb.height; ++r) {
        unsigned int rr = (thumb.height - 1 - r) * thumb.width;
        for (unsigned int c = 0; c < thumb.width; ++c) {
            unsigned char* origPx = const_cast<unsigned char*>(thumb.pixels.data()) + 4 * (rr + c);
            unsigned char* maskPx = const_cast<unsigned char*>(noLightThumb.pixels.data()) + 4 * (rr + c);

            unsigned int extruderId = static_cast<unsigned int>(maskPx[3]);

            auto it = colorMap.find(extruderId);
            if (it != colorMap.end()) {
                const auto& tc = it->second;
                float origLum = 0.299f * origPx[0] + 0.587f * origPx[1] + 0.114f * origPx[2];
                float targetLum = 0.299f * (tc[0] * 255.0f) + 0.587f * (tc[1] * 255.0f) + 0.114f * (tc[2] * 255.0f);

                float lumRatio = (targetLum > 0.0f) ? (origLum / targetLum) : 1.0f;
                unsigned char newR = static_cast<unsigned char>(std::min(255.0f, tc[0] * 255.0f * lumRatio));
                unsigned char newG = static_cast<unsigned char>(std::min(255.0f, tc[1] * 255.0f * lumRatio));
                unsigned char newB = static_cast<unsigned char>(std::min(255.0f, tc[2] * 255.0f * lumRatio));

                image.SetRGB(static_cast<int>(c), static_cast<int>(r), newR, newG, newB);
                image.SetAlpha(static_cast<int>(c), static_cast<int>(r), origPx[3]);
            } else {
                image.SetRGB(static_cast<int>(c), static_cast<int>(r), origPx[0], origPx[1], origPx[2]);
                image.SetAlpha(static_cast<int>(c), static_cast<int>(r), origPx[3]);
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
            unsigned char* px = const_cast<unsigned char*>(thumb.pixels.data()) + 4 * (rr + c);
            image.SetRGB(static_cast<int>(c), static_cast<int>(r), px[0], px[1], px[2]);
            image.SetAlpha(static_cast<int>(c), static_cast<int>(r), px[3]);
        }
    }
    return wxBitmap(image);
}

} // namespace GUI
} // namespace Slic3r
