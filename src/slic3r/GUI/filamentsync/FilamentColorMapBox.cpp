#include "FilamentColorMapBox.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/panel.h>


namespace
{

// --- Layout ---
constexpr int g_colorMapBoxWidth  = 100;
constexpr int g_colorMapBoxHeight = 120;
constexpr int g_aboveProportion   = 2; // 2/5 of total height
constexpr int g_belowProportion   = 3; // 3/5 of total height
constexpr int g_separatorHeight   = 1; // DIP
constexpr int g_labelWrapWidth    = 90; // DIP

// --- Colors ---
constexpr int g_luminanceThreshold = 140; // perceived brightness threshold for white/black text

// --- Luminance coefficients (ITU-R BT.601) ---
constexpr double g_lumR = 0.299;
constexpr double g_lumG = 0.587;
constexpr double g_lumB = 0.114;

// --- Defaults ---
constexpr const char* g_separatorColor("#999999");


bool isDarkColour(const wxColour& c)
{
    return (c.Red() * g_lumR + c.Green() * g_lumG + c.Blue() * g_lumB) < g_luminanceThreshold;
}

wxColour getTextColour(const wxColour& bg)
{
    return isDarkColour(bg) ? *wxWHITE : *wxBLACK;
}

wxString formatLabel(const Slic3r::GUI::FilamentData& data)
{
    // Display as 1-based index (1, 2, …) regardless of internal storage.
    unsigned int displayIndex = data.m_index + 1;
    if (data.m_name.empty())
        return wxString::Format("%u", displayIndex);
    return wxString::Format("%u  %s", displayIndex, data.m_name);
}

} // namespace

namespace Slic3r
{
namespace GUI
{

FilamentColorMapBox::FilamentColorMapBox(wxWindow* parent,
                                         const FilamentData& aboveData,
                                         const FilamentData& belowData)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE)
    , m_aboveFilament(aboveData)
    , m_belowFilament(belowData)
{
    SetMinSize(FromDIP(wxSize(g_colorMapBoxWidth, g_colorMapBoxHeight)));

    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Above panel (2/5 logical filament) ----
    m_pAbovePanel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                wxBORDER_NONE);

    auto* aboveSizer = new wxBoxSizer(wxVERTICAL);
    m_pAboveLabel = new wxStaticText(m_pAbovePanel, wxID_ANY, formatLabel(m_aboveFilament),
                                      wxDefaultPosition, wxDefaultSize,
                                      wxALIGN_CENTRE_HORIZONTAL);
    m_pAboveLabel->Wrap(FromDIP(g_labelWrapWidth));
    aboveSizer->AddStretchSpacer();
    aboveSizer->Add(m_pAboveLabel, 0, wxALIGN_CENTER_HORIZONTAL);
    aboveSizer->AddStretchSpacer();
    m_pAbovePanel->SetSizer(aboveSizer);

    m_pAbovePanel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { onAboveButtonClicked(); });
    m_pAboveLabel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { onAboveButtonClicked(); });

    mainSizer->Add(m_pAbovePanel, g_aboveProportion, wxEXPAND);

    // ---- Separator line ----
    auto* sepPanel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                 FromDIP(wxSize(-1, g_separatorHeight)));
    sepPanel->SetBackgroundColour(wxColour(g_separatorColor));
    mainSizer->Add(sepPanel, 0, wxEXPAND);

    // ---- Below panel (3/5 machine filament) ----
    m_pBelowPanel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                wxBORDER_NONE);

    auto* belowSizer = new wxBoxSizer(wxVERTICAL);
    m_pBelowLabel = new wxStaticText(m_pBelowPanel, wxID_ANY,
                                      m_belowFilament.m_name.empty()
                                          ? wxString("--")
                                          : wxString(m_belowFilament.m_name),
                                      wxDefaultPosition, wxDefaultSize,
                                      wxALIGN_CENTRE_HORIZONTAL);
    m_pBelowLabel->Wrap(FromDIP(g_labelWrapWidth));
    belowSizer->AddStretchSpacer();
    belowSizer->Add(m_pBelowLabel, 0, wxALIGN_CENTER_HORIZONTAL);
    belowSizer->AddStretchSpacer();
    m_pBelowPanel->SetSizer(belowSizer);

    m_pBelowPanel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { onBelowButtonClicked(); });
    m_pBelowLabel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { onBelowButtonClicked(); });

    mainSizer->Add(m_pBelowPanel, g_belowProportion, wxEXPAND);

    SetSizer(mainSizer);
    Layout();

    applyColors();
}

void FilamentColorMapBox::bindButton(FilamentInfoCallback cb, ButtonType type)
{
    if (type == ButtonType::Above)
        m_aboveCallback = std::move(cb);
    else
        m_belowCallback = std::move(cb);
}

void FilamentColorMapBox::setEnable(bool bEnable, ButtonType type)
{
    if (type == ButtonType::Above) {
        m_bAboveEnabled = bEnable;
        if (m_pAbovePanel)
            m_pAbovePanel->Enable(bEnable);
    } else {
        m_bBelowEnabled = bEnable;
        if (m_pBelowPanel)
            m_pBelowPanel->Enable(bEnable);
    }
}

void FilamentColorMapBox::updateAboveData(const FilamentData& data)
{
    m_aboveFilament = data;
    if (m_pAboveLabel)
        m_pAboveLabel->SetLabel(formatLabel(m_aboveFilament));
    applyColors();
}

void FilamentColorMapBox::updateBelowData(const FilamentData& data)
{
    m_belowFilament = data;
    if (m_pBelowLabel) {
        m_pBelowLabel->SetLabel(m_belowFilament.m_name.empty()
                                    ? wxString("--")
                                    : wxString(m_belowFilament.m_name));
    }
    applyColors();
}

const FilamentData& FilamentColorMapBox::getAboveData() const
{
    return m_aboveFilament;
}

const FilamentData& FilamentColorMapBox::getBelowData() const
{
    return m_belowFilament;
}

void FilamentColorMapBox::applyColors()
{
    wxColour aboveBg(m_aboveFilament.m_color_r, m_aboveFilament.m_color_g, m_aboveFilament.m_color_b);
    wxColour belowBg(m_belowFilament.m_color_r, m_belowFilament.m_color_g, m_belowFilament.m_color_b);

    if (m_pAbovePanel) {
        m_pAbovePanel->SetBackgroundColour(aboveBg);
        m_pAboveLabel->SetForegroundColour(getTextColour(aboveBg));
        m_pAbovePanel->Refresh();
    }
    if (m_pBelowPanel) {
        m_pBelowPanel->SetBackgroundColour(belowBg);
        m_pBelowLabel->SetForegroundColour(getTextColour(belowBg));
        m_pBelowPanel->Refresh();
    }
}

void FilamentColorMapBox::onAboveButtonClicked()
{
    if (m_bAboveEnabled && m_aboveCallback)
        m_aboveCallback(m_aboveFilament);
}

void FilamentColorMapBox::onBelowButtonClicked()
{
    if (m_bBelowEnabled && m_belowCallback)
        m_belowCallback(m_belowFilament);
}

} // namespace GUI
} // namespace Slic3r
