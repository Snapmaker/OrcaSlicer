#include "FilamentColorRadio.hpp"

#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace
{

// --- Radio layout ---
constexpr int g_radioMinWidth  = 80;  // DIP
constexpr int g_radioMinHeight = 100; // DIP

// --- Circle diameters ---
constexpr int g_indexCircleDiameter = 28; // DIP — filled index circle
constexpr int g_radioCircleDiameter = 18; // DIP — selection indicator

// --- Label ---
constexpr int g_nameLabelWrapWidth = 70; // DIP

// --- Spacing ---
constexpr int g_circleTopMargin    = 6; // DIP
constexpr int g_nameLabelTopMargin = 4; // DIP
constexpr int g_radioBottomMargin  = 6; // DIP

// --- Circle painting ---
constexpr int g_circleStrokeWidth = 2; // DIP — outline width for unfilled circle
constexpr int g_circleInset       = 2; // DIP — inset from panel edge

} // namespace

namespace Slic3r
{
namespace GUI
{

class CirclePanel : public wxPanel
{
public:
    CirclePanel(wxWindow* parent, const wxColour& color, bool filled, int diameterDip)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_color(color)
        , m_filled(filled)
    {
        SetSize(FromDIP(wxSize(diameterDip, diameterDip)));
        SetMinSize(FromDIP(wxSize(diameterDip, diameterDip)));
        SetMaxSize(FromDIP(wxSize(diameterDip, diameterDip)));
        Bind(wxEVT_PAINT, &CirclePanel::onPaint, this);
    }

    void setColor(const wxColour& c)
    {
        m_color = c;
        Refresh();
    }

    void setFilled(bool f)
    {
        m_filled = f;
        Refresh();
    }

private:
    void onPaint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        wxSize    sz = GetClientSize();
        int       cx = sz.x / 2;
        int       cy = sz.y / 2;
        int       r  = std::min(cx, cy) - FromDIP(g_circleInset);

        if (m_filled) {
            dc.SetBrush(wxBrush(m_color));
            dc.SetPen(wxPen(m_color));
        } else {
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(m_color, FromDIP(g_circleStrokeWidth)));
        }
        dc.DrawCircle(cx, cy, r);
    }

    wxColour m_color;
    bool     m_filled;
};

FilamentColorRadio::FilamentColorRadio(wxWindow* parent, const FilamentData& data)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE)
    , m_machineData(data)
{
    SetMinSize(FromDIP(wxSize(g_radioMinWidth, g_radioMinHeight)));

    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Index circle ----
    wxColour c(m_machineData.m_color_r, m_machineData.m_color_g, m_machineData.m_color_b);
    m_pIndexCircle = new CirclePanel(this, c, true, g_indexCircleDiameter);
    mainSizer->Add(m_pIndexCircle, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP,
                   FromDIP(g_circleTopMargin));

    // ---- Name label ----
    m_pNameLabel = new wxStaticText(this, wxID_ANY, m_machineData.m_name,
                                     wxDefaultPosition, wxDefaultSize,
                                     wxALIGN_CENTRE_HORIZONTAL);
    m_pNameLabel->Wrap(FromDIP(g_nameLabelWrapWidth));
    mainSizer->Add(m_pNameLabel, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP,
                   FromDIP(g_nameLabelTopMargin));

    // ---- Radio circle ----
    m_pRadioCircle = new CirclePanel(this, c, false, g_radioCircleDiameter);
    mainSizer->Add(m_pRadioCircle, 0,
                   wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM,
                   FromDIP(g_radioBottomMargin));

    SetSizer(mainSizer);
    Layout();

    // ---- Click-to-select ----
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { onRadioClicked(); });
    m_pIndexCircle->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { onRadioClicked(); });
    m_pNameLabel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { onRadioClicked(); });
    m_pRadioCircle->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { onRadioClicked(); });
}

void FilamentColorRadio::setSelected(bool bSelected)
{
    m_bSelected = bSelected;

    if (m_pRadioCircle) {
        auto* cp = static_cast<CirclePanel*>(m_pRadioCircle);
        cp->setFilled(bSelected);
    }
}

bool FilamentColorRadio::isSelected() const
{
    return m_bSelected;
}

void FilamentColorRadio::bindSelectedClicked(FilamentInfoCallback cb)
{
    m_selectedCallback = std::move(cb);
}

void FilamentColorRadio::updateData(const FilamentData& data)
{
    m_machineData = data;

    wxColour c(m_machineData.m_color_r, m_machineData.m_color_g, m_machineData.m_color_b);

    if (m_pIndexCircle) {
        auto* cp = static_cast<CirclePanel*>(m_pIndexCircle);
        cp->setColor(c);
    }
    if (m_pNameLabel)
        m_pNameLabel->SetLabel(m_machineData.m_name);
    if (m_pRadioCircle) {
        auto* cp = static_cast<CirclePanel*>(m_pRadioCircle);
        cp->setColor(c);
    }

    Refresh();
}

const FilamentData& FilamentColorRadio::getData() const
{
    return m_machineData;
}

void FilamentColorRadio::onRadioClicked()
{
    if (!m_bSelected) {
        setSelected(true);
        if (m_selectedCallback)
            m_selectedCallback(m_machineData);
    }
}

} // namespace GUI
} // namespace Slic3r
