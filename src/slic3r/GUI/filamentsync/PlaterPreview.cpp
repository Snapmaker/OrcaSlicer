#include "PlaterPreview.hpp"

#include <wx/button.h>
#include <wx/combobox.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/sizer.h>

#include "slic3r/GUI/I18N.hpp"

namespace Slic3r
{
namespace GUI
{

PlaterPreview::PlaterPreview(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    auto* mainSizer = new wxBoxSizer(wxHORIZONTAL);

    // ---- Left panel: original thumbnail + navigation ----
    auto* leftSizer = new wxBoxSizer(wxVERTICAL);

    m_pOriginalLabel = new wxStaticText(this, wxID_ANY, _L("Original"));
    m_pOriginalLabel->SetFont(m_pOriginalLabel->GetFont().MakeBold());
    leftSizer->Add(m_pOriginalLabel, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(2));

    m_pOriginalThumbnail = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap,
                                               wxDefaultPosition, wxDefaultSize,
                                               wxBORDER_SIMPLE);
    m_pOriginalThumbnail->SetMinSize(wxSize(FromDIP(200), FromDIP(150)));
    leftSizer->Add(m_pOriginalThumbnail, 1, wxEXPAND | wxALL, FromDIP(6));

    // Navigation bar
    auto* navSizer = new wxBoxSizer(wxHORIZONTAL);

    m_pPrevPageBtn = new wxButton(this, wxID_ANY, wxString::FromUTF8("◀"),
                                   wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    m_pPrevPageBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onPrePage(); });
    navSizer->Add(m_pPrevPageBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

    m_pPlateLabel = new wxStaticText(this, wxID_ANY, _L("Plate"));
    navSizer->Add(m_pPlateLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

    m_pPlateCombo = new wxComboBox(this, wxID_ANY, wxEmptyString,
                                    wxDefaultPosition, wxDefaultSize, 0, nullptr,
                                    wxCB_READONLY);
    m_pPlateCombo->Append("01");
    m_pPlateCombo->SetSelection(0);
    m_pPlateCombo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
        onPlateComboBoxChanged(m_pPlateCombo->GetSelection());
    });
    navSizer->Add(m_pPlateCombo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

    m_pNextPageBtn = new wxButton(this, wxID_ANY, wxString::FromUTF8("▶"),
                                   wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    m_pNextPageBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onNextPage(); });
    navSizer->Add(m_pNextPageBtn, 0, wxALIGN_CENTER_VERTICAL);

    leftSizer->Add(navSizer, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, FromDIP(6));

    mainSizer->Add(leftSizer, 1, wxEXPAND | wxALL, FromDIP(2));

    // ---- Right panel: cover thumbnail ----
    auto* rightSizer = new wxBoxSizer(wxVERTICAL);

    m_pCoverLabel = new wxStaticText(this, wxID_ANY, _L("Matched"));
    m_pCoverLabel->SetFont(m_pCoverLabel->GetFont().MakeBold());
    rightSizer->Add(m_pCoverLabel, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(2));

    m_pCoverThumbnail = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap,
                                            wxDefaultPosition, wxDefaultSize,
                                            wxBORDER_SIMPLE);
    m_pCoverThumbnail->SetMinSize(wxSize(FromDIP(200), FromDIP(150)));
    rightSizer->Add(m_pCoverThumbnail, 1, wxEXPAND | wxALL, FromDIP(6));

    mainSizer->Add(rightSizer, 1, wxEXPAND | wxALL, FromDIP(2));

    SetSizer(mainSizer);
    Layout();

    updateNavButtons();

    // Bind resize event for auto-scaling thumbnails
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        e.Skip();
        CallAfter([this]() { rescaleBitmaps(); });
    });
}

void PlaterPreview::setOriginalPreview(const wxBitmap& thumbnail)
{
    m_originalBitmap = thumbnail;
    rescaleOriginalBitmap();
}

void PlaterPreview::setCoverPreview(const wxBitmap& thumbnail)
{
    m_coverBitmap = thumbnail;
    rescaleCoverBitmap();
}

void PlaterPreview::updateCoverPreview(const wxBitmap& thumbnail)
{
    setCoverPreview(thumbnail);
}

void PlaterPreview::setCurrentPlate(unsigned int plateIndex)
{
    if (plateIndex >= m_totalPlateCount)
        return;

    m_currentPlateIndex = plateIndex;

    if (m_pPlateCombo)
        m_pPlateCombo->SetSelection(static_cast<int>(plateIndex));

    updateNavButtons();
}

unsigned int PlaterPreview::getCurrentPlate() const
{
    return m_currentPlateIndex;
}

void PlaterPreview::setTotalPlateCount(unsigned int count)
{
    m_totalPlateCount = (count > 0) ? count : 1;

    if (m_pPlateCombo) {
        m_pPlateCombo->Clear();
        for (unsigned int i = 0; i < m_totalPlateCount; ++i)
            m_pPlateCombo->Append(wxString::Format("%02u", i + 1));

        if (m_currentPlateIndex >= m_totalPlateCount)
            m_currentPlateIndex = 0;
        m_pPlateCombo->SetSelection(static_cast<int>(m_currentPlateIndex));
    }

    updateNavButtons();
}

void PlaterPreview::bindPlateSwitchCallback(PlateSwitchCallback cb)
{
    m_plateSwitchCallback = std::move(cb);
}

void PlaterPreview::bindCoverPreviewCallback(CoverPreviewCallback cb)
{
    m_coverPreviewCallback = std::move(cb);
}

void PlaterPreview::onCoverPreview()
{
    if (m_coverPreviewCallback)
        m_coverPreviewCallback();
}

void PlaterPreview::onPrePage()
{
    if (m_currentPlateIndex == 0)
        return;

    --m_currentPlateIndex;

    if (m_pPlateCombo)
        m_pPlateCombo->SetSelection(static_cast<int>(m_currentPlateIndex));

    updateNavButtons();

    if (m_plateSwitchCallback)
        m_plateSwitchCallback(m_currentPlateIndex);
}

void PlaterPreview::onNextPage()
{
    if (m_currentPlateIndex + 1 >= m_totalPlateCount)
        return;

    ++m_currentPlateIndex;

    if (m_pPlateCombo)
        m_pPlateCombo->SetSelection(static_cast<int>(m_currentPlateIndex));

    updateNavButtons();

    if (m_plateSwitchCallback)
        m_plateSwitchCallback(m_currentPlateIndex);
}

void PlaterPreview::onPlateComboBoxChanged(int index)
{
    if (index < 0 || static_cast<unsigned int>(index) >= m_totalPlateCount)
        return;

    m_currentPlateIndex = static_cast<unsigned int>(index);

    updateNavButtons();

    if (m_plateSwitchCallback)
        m_plateSwitchCallback(m_currentPlateIndex);
}

void PlaterPreview::updateNavButtons()
{
    if (m_pPrevPageBtn)
        m_pPrevPageBtn->Enable(m_currentPlateIndex > 0);

    if (m_pNextPageBtn)
        m_pNextPageBtn->Enable(m_currentPlateIndex + 1 < m_totalPlateCount);
}

void PlaterPreview::rescaleBitmaps()
{
    rescaleOriginalBitmap();
    rescaleCoverBitmap();
}

void PlaterPreview::rescaleOriginalBitmap()
{
    if (!m_pOriginalThumbnail || !m_originalBitmap.IsOk())
        return;
    if (m_isRescaling)
        return;

    m_isRescaling = true;

    wxSize availSize = m_pOriginalThumbnail->GetClientSize();
    if (availSize.GetWidth() < 10 || availSize.GetHeight() < 10) {
        m_isRescaling = false;
        return;
    }

    wxImage img = m_originalBitmap.ConvertToImage();
    wxSize  imgSize = img.GetSize();
    if (imgSize.GetWidth() <= 0 || imgSize.GetHeight() <= 0) {
        m_isRescaling = false;
        return;
    }

    // Maintain aspect ratio, fit within available area (with 4px inset margin)
    double scaleX = static_cast<double>(availSize.GetWidth() - 8) / imgSize.GetWidth();
    double scaleY = static_cast<double>(availSize.GetHeight() - 8) / imgSize.GetHeight();
    double scale  = std::min(scaleX, scaleY);

    int newW = std::max(1, static_cast<int>(imgSize.GetWidth() * scale));
    int newH = std::max(1, static_cast<int>(imgSize.GetHeight() * scale));

    img.Rescale(newW, newH, wxIMAGE_QUALITY_HIGH);
    m_pOriginalThumbnail->SetBitmap(wxBitmap(img));

    m_isRescaling = false;
}

void PlaterPreview::rescaleCoverBitmap()
{
    if (!m_pCoverThumbnail || !m_coverBitmap.IsOk())
        return;
    if (m_isRescaling)
        return;

    m_isRescaling = true;

    wxSize availSize = m_pCoverThumbnail->GetClientSize();
    if (availSize.GetWidth() < 10 || availSize.GetHeight() < 10) {
        m_isRescaling = false;
        return;
    }

    wxImage img = m_coverBitmap.ConvertToImage();
    wxSize  imgSize = img.GetSize();
    if (imgSize.GetWidth() <= 0 || imgSize.GetHeight() <= 0) {
        m_isRescaling = false;
        return;
    }

    // Maintain aspect ratio, fit within available area (with 4px inset margin)
    double scaleX = static_cast<double>(availSize.GetWidth() - 8) / imgSize.GetWidth();
    double scaleY = static_cast<double>(availSize.GetHeight() - 8) / imgSize.GetHeight();
    double scale  = std::min(scaleX, scaleY);

    int newW = std::max(1, static_cast<int>(imgSize.GetWidth() * scale));
    int newH = std::max(1, static_cast<int>(imgSize.GetHeight() * scale));

    img.Rescale(newW, newH, wxIMAGE_QUALITY_HIGH);
    m_pCoverThumbnail->SetBitmap(wxBitmap(img));

    m_isRescaling = false;
}

} // namespace GUI
} // namespace Slic3r
