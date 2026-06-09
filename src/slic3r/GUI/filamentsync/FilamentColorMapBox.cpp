#include "FilamentColorMapBox.hpp"

#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/dcmemory.h>
#include <wx/settings.h>

#include "slic3r/GUI/BitmapCache.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"

namespace
{

// ============================================================
// Layout constants
// ============================================================

constexpr int g_cardWidth = 67;

constexpr int g_topBarHeight   = 26;
constexpr int g_bodyOverlap    = 1;
constexpr int g_bodyY          = g_topBarHeight - g_bodyOverlap; // 25

constexpr int g_bodyHeightEnabled  = 60; // 25 + 60 = 85
constexpr int g_bodyHeightDisabled = 46; // 25 + 46 = 71

constexpr int g_cardHeightEnabled  = g_bodyY + g_bodyHeightEnabled;  // 85
constexpr int g_cardHeightDisabled = g_bodyY + g_bodyHeightDisabled; // 71

constexpr int g_cornerRadius = 6;

constexpr int g_circleDiameter = 20;
constexpr int g_circleYOffset  = 5;

constexpr int g_arrowSize      = 14;
constexpr int g_arrowBotMargin = 4;

constexpr int g_topTextY = 7;

constexpr int g_nameTextY        = 29;
constexpr int g_bodyBorderWidth  = 1;

// ============================================================
// Colours
// ============================================================
const wxColour g_bodyBorderColor(0xDB, 0xDB, 0xDA);
const wxColour g_bodyTextColor(0x33, 0x33, 0x33);
const wxColour g_disabledBodyBg(0xE8, 0xE8, 0xE8);

// ============================================================
// Luminance helpers
// ============================================================
constexpr double g_lumR = 0.299;
constexpr double g_lumG = 0.587;
constexpr double g_lumB = 0.114;
constexpr int    g_luminanceThreshold = 140;

bool isDarkColour(const wxColour& c)
{
    return (c.Red() * g_lumR + c.Green() * g_lumG + c.Blue() * g_lumB) < g_luminanceThreshold;
}

wxColour getTextColour(const wxColour& bg)
{
    return isDarkColour(bg) ? *wxWHITE : *wxBLACK;
}

wxString formatTopLabel(const Slic3r::GUI::FilamentData& data)
{
    unsigned int displayIndex = data.m_index + 1;
    if (data.m_type.empty())
        return wxString::Format("%u NONE", displayIndex);
    return wxString::Format("%u %s", displayIndex, data.m_type);
}

Slic3r::GUI::BitmapCache& getIconCache()
{
    static Slic3r::GUI::BitmapCache s_cache;
    return s_cache;
}

const wxBitmap& getDropdownArrowBitmap(int sizePxHasDpi)
{
    static wxBitmap s_bmp;
    static int      s_cachedPx = -1;
    if (s_cachedPx != sizePxHasDpi || !s_bmp.IsOk()) {
        wxBitmap* loaded = getIconCache().load_svg("filament_dropdown_arrow", sizePxHasDpi, sizePxHasDpi);
        if (loaded && loaded->IsOk()) {
            s_bmp       = *loaded;
            s_cachedPx = sizePxHasDpi;
        }
    }
    return s_bmp;
}

} // namespace

namespace Slic3r
{
namespace GUI
{

FilamentColorMapBox::FilamentColorMapBox(wxWindow* parent,
                                         const FilamentData& aboveData,
                                         const FilamentData& belowData)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
              wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE)
    , m_aboveFilament(aboveData)
    , m_belowFilament(belowData)
{
    updateSizing();
    Bind(wxEVT_PAINT, &FilamentColorMapBox::onPaint, this);
    Bind(wxEVT_LEFT_DOWN, &FilamentColorMapBox::onLeftDown, this);
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
    } else {
        m_bBelowEnabled = bEnable;
    }
    updateSizing();
    Refresh();
    if (auto* p = GetParent())
        p->Layout();
}

void FilamentColorMapBox::updateAboveData(const FilamentData& data)
{
    m_aboveFilament = data;
    Refresh();
}

void FilamentColorMapBox::updateBelowData(const FilamentData& data)
{
    m_belowFilament = data;
    Refresh();
}

const FilamentData& FilamentColorMapBox::getAboveData() const
{
    return m_aboveFilament;
}

const FilamentData& FilamentColorMapBox::getBelowData() const
{
    return m_belowFilament;
}

void FilamentColorMapBox::updateSizing()
{
    int totalH = m_bBelowEnabled ? g_cardHeightEnabled : g_cardHeightDisabled;
    wxSize sz = FromDIP(wxSize(g_cardWidth, totalH));
    SetMinSize(sz);
    SetMaxSize(sz);
}

void FilamentColorMapBox::onPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.Clear();

    wxGCDC gdc(dc);

    const wxSize sz    = GetClientSize();
    const int    w     = sz.x;
    const int    totalH = sz.y;
    const int    splitY = FromDIP(g_topBarHeight);
    const int    bodyH  = totalH - splitY;
    const int    radius = FromDIP(g_cornerRadius);

    const wxColour aboveColor(m_aboveFilament.m_color_r,
                              m_aboveFilament.m_color_g,
                              m_aboveFilament.m_color_b);
    const wxColour belowColor(m_belowFilament.m_color_r,
                              m_belowFilament.m_color_g,
                              m_belowFilament.m_color_b);

    const wxColour aboveBg = m_bAboveEnabled ? aboveColor : g_disabledBodyBg;
    const int      borderW = FromDIP(g_bodyBorderWidth);

    // ---- 1. Full card body (white bg, #DBDBDA border) ----
    gdc.SetPen(wxPen(g_bodyBorderColor, borderW));
    gdc.SetBrush(*wxWHITE_BRUSH);
    gdc.DrawRoundedRectangle(0, 0, w, totalH, radius);

    // ---- 2. Top bar (above colour, clipped to splitY) ----
    {
        wxDCClipper clip(gdc, wxRect(0, 0, w, splitY));
        gdc.SetPen(wxPen(g_bodyBorderColor, borderW));
        gdc.SetBrush(wxBrush(aboveBg));
        gdc.DrawRoundedRectangle(0, 0, w, totalH, radius);
    }

    // ---- 3. Separator line ----
    {
        gdc.SetPen(wxPen(g_bodyBorderColor, borderW));
        gdc.DrawLine(0, splitY, w, splitY);
    }

    // ---- 4. Top bar label ----
    {
        const wxColour textColour = m_bAboveEnabled
                                        ? getTextColour(aboveColor)
                                        : g_bodyTextColor;
        gdc.SetTextForeground(textColour);
        gdc.SetFont(Label::Body_10);
        const wxString label = formatTopLabel(m_aboveFilament);
        const wxSize   te    = gdc.GetTextExtent(label);
        gdc.DrawText(label, (w - te.x) / 2, FromDIP(g_topTextY));
    }

    // ---- 5. Index circle (filled with machine filament colour, #DBDBDA border) ----
    {
        const int circleD = FromDIP(g_circleDiameter);
        const int cx      = w / 2;
        const int cy      = splitY + FromDIP(g_circleYOffset) + circleD / 2;
        const int cr      = circleD / 2;

        gdc.SetPen(wxPen(g_bodyBorderColor, borderW));
        gdc.SetBrush(wxBrush(belowColor));
        gdc.DrawCircle(cx, cy, cr);

        gdc.SetTextForeground(getTextColour(belowColor));
        gdc.SetFont(Label::Head_12);
        const wxString num    = wxString::Format("%u", m_belowFilament.m_index + 1);
        const wxSize   numExt = gdc.GetTextExtent(num);
        gdc.DrawText(num, cx - numExt.x / 2, cy - numExt.y / 2);
    }

    // ---- 6. Below filament type / name ----
    {
        gdc.SetTextForeground(g_bodyTextColor);
        gdc.SetFont(Label::Body_10);
        const wxString type = m_belowFilament.m_type.empty()
                                  ? wxString("NONE")
                                  : wxString(m_belowFilament.m_type);
        const wxSize te = gdc.GetTextExtent(type);
        gdc.DrawText(type, (w - te.x) / 2,
                     splitY + FromDIP(g_nameTextY));
    }

    // ---- 7. Dropdown arrow (SVG, only when Below enabled) ----
    if (m_bBelowEnabled) {
        const wxBitmap& arrowBmp = getDropdownArrowBitmap(FromDIP(g_arrowSize));
        if (arrowBmp.IsOk()) {
            const int arrowW = arrowBmp.GetWidth();
            const int arrowH = arrowBmp.GetHeight();
            const int ax     = (w - arrowW) / 2;
            const int ay     = splitY + bodyH - arrowH - FromDIP(g_arrowBotMargin);
            gdc.DrawBitmap(arrowBmp, ax, ay);
        }
    }
}

void FilamentColorMapBox::onLeftDown(wxMouseEvent& event)
{
    int posY  = event.GetPosition().y;
    int splitY = FromDIP(g_topBarHeight);

    if (posY < splitY) {
        if (m_bAboveEnabled && m_aboveCallback)
            m_aboveCallback(m_aboveFilament);
    } else {
        if (m_bBelowEnabled && m_belowCallback)
            m_belowCallback(m_belowFilament);
    }
}

} // namespace GUI
} // namespace Slic3r
