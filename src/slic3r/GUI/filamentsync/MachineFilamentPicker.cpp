#include "MachineFilamentPicker.hpp"

#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/dcgraph.h>

#include "slic3r/GUI/Widgets/Label.hpp"

namespace
{

// --- Popup layout ---
constexpr int g_popupWidth   = 67;  // DIP — matches FilamentColorMapBox card width
constexpr int g_popupHeight  = 106; // DIP

// --- Item row ---
constexpr int g_itemRowH      = 16; // DIP — row height
constexpr int g_itemStepY     = 22; // DIP — vertical step between rows
constexpr int g_firstRowY     = 12; // DIP — top of first row (after shadow)
constexpr int g_itemGap       = 6;  // DIP — gap between rows

// --- Checkmark (draw with path, roughly 8×8 DIP) ---
constexpr int g_checkmarkX = 9;
constexpr int g_checkmarkY = 4;  // offset from row top
constexpr int g_checkmarkW = 8;
constexpr int g_checkmarkH = 8;

// --- Colour circle (filament index) ---
constexpr int g_circleRadius = 7;  // DIP (design: 6.57143)
constexpr int g_circleCx     = 30; // DIP from left edge

// --- Text label ---
constexpr int g_textX = 42; // DIP from left edge

// --- Colours (from design spec) ---
const wxColour g_selectionBg(0xE7, 0xEF, 0xFC);  // #0C63E2 at 10% on white
const wxColour g_checkmarkColor(0x00, 0x96, 0x88);   // #009688
const wxColour g_textColor(0x33, 0x33, 0x33);         // #333333
const wxColour g_circleStroke(0xDB, 0xDB, 0xDA);      // #DBDBDA
// ============================================================
// Helper: determine which row is under a mouse-y coordinate,
//         returns -1 when the click is not on any row.
// ============================================================
int hitTestRow(int yDip, int itemCount)
{
    if (yDip < g_firstRowY) {
        return -1;
    }
    int row = (yDip - g_firstRowY) / g_itemStepY;
    if (row >= itemCount) {
        return -1;
    }
    // Allow a little tolerance into the gap below the last row
    int top = g_firstRowY + row * g_itemStepY;
    if (yDip > top + g_itemRowH + g_itemGap) {
        return -1;
    }
    return row;
}

} // namespace

namespace Slic3r
{
namespace GUI
{

// ============================================================
// Inner panel that does all custom drawing
// ============================================================
class PickerContentPanel : public wxPanel
{
public:
    PickerContentPanel(wxWindow* parent,
                       const std::vector<FilamentData>& dataList,
                       unsigned int selectedIndex)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                  wxFULL_REPAINT_ON_RESIZE)
        , m_dataList(dataList)
        , m_selectedIndex(selectedIndex)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &PickerContentPanel::onPaint, this);
        Bind(wxEVT_LEFT_DOWN, &PickerContentPanel::onLeftDown, this);

        wxSize sz = FromDIP(wxSize(g_popupWidth, g_popupHeight));
        SetSize(sz);
        SetMinSize(sz);
        SetMaxSize(sz);
    }

    void setSelectedIndex(unsigned int idx)
    {
        m_selectedIndex = idx;
        Refresh();
    }

    void bindSelectionCallback(std::function<void(unsigned int)> cb)
    {
        m_selectionCallback = std::move(cb);
    }

private:
    void onPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC bufDC(this);
        wxGCDC dc(bufDC);
        render(dc);
    }

    void render(wxDC& dc)
    {
        int contentW = FromDIP(g_popupWidth);
        int contentH = FromDIP(g_popupHeight);

        // ---- Background fill (clear) ----
        dc.SetBackground(*wxWHITE);
        dc.Clear();

        // ---- White content area ----
        dc.SetPen(wxPen(wxColour(0xE0, 0xE0, 0xE0), 1));
        dc.SetBrush(wxBrush(*wxWHITE));
        dc.DrawRectangle(0, 0, contentW, contentH);

        // ---- Draw each row ----
        int itemCount = static_cast<int>(m_dataList.size());

        wxFont indexFont = GetFont();
        indexFont.SetPointSize(7);
        indexFont.SetWeight(wxFONTWEIGHT_BOLD);

        wxFont labelFont = Label::Body_10;

        for (int i = 0; i < itemCount; ++i) {
            int rowY = g_firstRowY + i * g_itemStepY;
            drawRow(dc, i, rowY, indexFont, labelFont);
        }
    }

    void drawRow(wxDC& dc, int index, int rowYDip,
                 const wxFont& indexFont, const wxFont& labelFont)
    {
        const FilamentData& data = m_dataList[index];
        bool isSelected = (static_cast<unsigned int>(index) == m_selectedIndex);

        int x = 0; // all coordinates in DIP
        int y = rowYDip;

        // ---- Selection background ----
        if (isSelected) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(g_selectionBg));
            dc.DrawRectangle(FromDIP(x), FromDIP(y),
                             FromDIP(g_popupWidth),
                             FromDIP(g_itemRowH));
        }

        // ---- Checkmark (selected only) ----
        if (isSelected) {
            int cx = FromDIP(g_checkmarkX);
            int cy = FromDIP(y + g_checkmarkY);
            int cw = FromDIP(g_checkmarkW);
            int ch = FromDIP(g_checkmarkH);

            drawCheckmark(dc, cx, cy, cw, ch);
        }

        // ---- Colour circle ----
        wxColour fillColor(data.m_color_r, data.m_color_g, data.m_color_b);
        int circleCxPx = FromDIP(g_circleCx);
        int circleCyPx = FromDIP(y + g_itemRowH / 2);
        int circleR    = FromDIP(g_circleRadius);

        dc.SetBrush(wxBrush(fillColor));
        dc.SetPen(wxPen(g_circleStroke, FromDIP(1)));
        dc.DrawCircle(circleCxPx, circleCyPx, circleR);

        // ---- Index number inside circle ----
        dc.SetFont(indexFont);
        dc.SetTextForeground(getTextColour(fillColor));
        wxString idxStr = wxString::Format("%d", data.m_index + 1);
        wxSize   idxExtent = dc.GetTextExtent(idxStr);
        dc.DrawText(idxStr, circleCxPx - idxExtent.x / 2,
                    circleCyPx - idxExtent.y / 2);

        // ---- Filament type text ----
        dc.SetFont(labelFont);
        dc.SetTextForeground(g_textColor);
        wxString typeStr = wxString::FromUTF8(data.m_type);
        int textX = FromDIP(g_textX);
        int textY = FromDIP(y) + (FromDIP(g_itemRowH) - dc.GetTextExtent(typeStr).y) / 2;
        dc.DrawText(typeStr, textX, textY);
    }

    void drawCheckmark(wxDC& dc, int x, int y, int w, int h)
    {
        // Simple checkmark: two-line polyline ✓
        wxPoint pts[3];
        pts[0] = wxPoint(x + w * 0.2, y + h * 0.5);
        pts[1] = wxPoint(x + w * 0.45, y + h * 0.75);
        pts[2] = wxPoint(x + w * 0.9, y + h * 0.2);

        dc.SetPen(wxPen(g_checkmarkColor, FromDIP(1)));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawLines(3, pts);
    }

    void onLeftDown(wxMouseEvent& evt)
    {
        int row = hitTestRow(evt.GetY() / GetDPIScaleFactor(),
                             static_cast<int>(m_dataList.size()));
        if (row < 0) {
            return;
        }

        m_selectedIndex = static_cast<unsigned int>(row);
        Refresh();

        if (m_selectionCallback) {
            m_selectionCallback(m_selectedIndex);
        }
    }

    std::vector<FilamentData> m_dataList;
    unsigned int              m_selectedIndex = 0;
    std::function<void(unsigned int)> m_selectionCallback = nullptr;
};

// ============================================================
// MachineFilamentPicker public API
// ============================================================

MachineFilamentPicker::MachineFilamentPicker(wxWindow* parent,
                                             const std::vector<FilamentData>& dataList,
                                             unsigned int curIndex)
    : wxPopupTransientWindow(parent)
    , m_dataList(dataList)
    , m_selectedIndex(curIndex)
{
    auto* panel = new PickerContentPanel(this, m_dataList, curIndex);

    panel->bindSelectionCallback([this](unsigned int idx) {
        m_selectedIndex = idx;

        if (m_selectionCallback && idx < m_dataList.size()) {
            m_selectionCallback(m_dataList[idx]);
        }

        Dismiss();
    });

    wxSize panelSize = panel->GetSize();
    SetClientSize(panelSize);
}

FilamentData MachineFilamentPicker::getSelectedData() const
{
    if (m_selectedIndex < m_dataList.size()) {
        return m_dataList[m_selectedIndex];
    }
    return FilamentData();
}

unsigned int MachineFilamentPicker::getSelectedIndex() const
{
    return m_selectedIndex;
}

void MachineFilamentPicker::setSelectedIndex(unsigned int index)
{
    m_selectedIndex = index;
    // Panel is a child — find and notify it
    const auto& children = GetChildren();
    if (!children.empty()) {
        auto* panel = static_cast<PickerContentPanel*>(children[0]);
        panel->setSelectedIndex(index);
    }
}

void MachineFilamentPicker::popupAt(const wxPoint& pos)
{
    SetPosition(pos);
    Popup();
}

void MachineFilamentPicker::bindSelectionCallback(FilamentInfoCallback cb)
{
    m_selectionCallback = std::move(cb);
}

void MachineFilamentPicker::bindOnDismissCallback(std::function<void()> cb)
{
    m_onDismissCallback = std::move(cb);
}

void MachineFilamentPicker::OnDismiss()
{
    if (m_onDismissCallback) {
        m_onDismissCallback();
    }

    CallAfter([this]() { Destroy(); });
}

} // namespace GUI
} // namespace Slic3r
