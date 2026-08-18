#include "SliceModePopup.hpp"

#include "FlowTypeHelper.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"

#include "libslic3r/PrintConfig.hpp"

#include <wx/dcbuffer.h>

namespace Slic3r { namespace GUI {

static const wxColour BRAND_GREEN("#009688");

SliceModePopup::SliceModePopup(wxWindow *parent)
    : PopupWindow(parent, wxBORDER_NONE)
    , m_timer(this)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &SliceModePopup::on_paint, this);
    Bind(wxEVT_MOTION, &SliceModePopup::on_mouse_move, this);
    Bind(wxEVT_LEFT_UP, &SliceModePopup::on_left_up, this);
    Bind(wxEVT_TIMER, &SliceModePopup::on_timer, this);
    update_metrics();
}

void SliceModePopup::update_metrics()
{
    wxClientDC dc(this);
    dc.SetFont(Label::Body_14);
    // The widest line decides the popup width: descriptions are indented by 20px.
    const int desc_w = std::max(dc.GetTextExtent(_L("Prioritize print quality")).x,
                                dc.GetTextExtent(_L("Manually assign filaments to nozzles")).x);
    dc.SetFont(Label::Head_14);
    const int title_w = std::max(dc.GetTextExtent(_L("Standard Mode")).x,
                                 dc.GetTextExtent(_L("Custom Mode")).x) + FromDIP(22);

    const int pad     = FromDIP(16);
    const int row_h   = FromDIP(46);   // 24px title + 2px gap + 20px description
    const int row_gap = FromDIP(16);
    const int width   = pad * 2 + std::max(title_w, FromDIP(20) + desc_w);

    m_std_rect    = wxRect(pad, pad, width - pad * 2, row_h);
    m_custom_rect = wxRect(pad, pad + row_h + row_gap, width - pad * 2, row_h);
    SetSize(wxSize(width, pad * 2 + row_h * 2 + row_gap));
}

void SliceModePopup::ShowFor(const std::vector<wxWindow*> &anchors, wxWindow *align_to)
{
    if (IsShown())
        return;
    m_anchors = anchors;
    update_metrics();
    const wxPoint bottom_right = align_to->ClientToScreen(wxPoint(align_to->GetSize().x, align_to->GetSize().y));
    SetPosition(wxPoint(bottom_right.x - GetSize().x, bottom_right.y + FromDIP(4)));
    Show();
    m_timer.Start(200);
    Refresh();
}

void SliceModePopup::HidePopup()
{
    m_timer.Stop();
    m_hovered_row = -1;
    if (IsShown())
        Hide();
}

void SliceModePopup::on_timer(wxTimerEvent &)
{
    const wxPoint mouse = wxGetMousePosition();
    bool keep = wxRect(GetScreenPosition(), GetSize()).Contains(mouse);
    for (wxWindow *anchor : m_anchors)
        if (!keep && anchor != nullptr && anchor->IsShownOnScreen())
            keep = wxRect(anchor->GetScreenPosition(), anchor->GetSize()).Contains(mouse);
    if (!keep)
        HidePopup();
}

void SliceModePopup::on_mouse_move(wxMouseEvent &evt)
{
    int row = -1;
    if (m_std_rect.Contains(evt.GetPosition()))
        row = 0;
    else if (m_custom_rect.Contains(evt.GetPosition()))
        row = 1;
    if (row != m_hovered_row) {
        m_hovered_row = row;
        SetCursor(wxCursor(row >= 0 ? wxCURSOR_HAND : wxCURSOR_ARROW));
        Refresh();
    }
}

void SliceModePopup::on_left_up(wxMouseEvent &evt)
{
    if (m_std_rect.Contains(evt.GetPosition()))
        FlowType::set_grouping_mode(FILAMENT_GROUPING_STANDARD);
    else if (m_custom_rect.Contains(evt.GetPosition()))
        FlowType::set_grouping_mode(FILAMENT_GROUPING_CUSTOM);
    else
        return;
    Refresh();
}

void SliceModePopup::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC dc(this);
    const bool dark = wxGetApp().dark_mode();

    const wxColour bg     = dark ? wxColour("#2D2D30") : *wxWHITE;
    const wxColour border = dark ? wxColour("#4B4B4D") : wxColour("#DBDBDA");
    const wxColour title  = dark ? wxColour("#E5E5E4") : wxColour("#242424");
    const wxColour desc   = dark ? wxColour("#ACACAC") : wxColour("#4A4A4A");
    const wxColour grey   = wxColour("#ACACAC");

    dc.SetBackground(wxBrush(bg));
    dc.Clear();
    dc.SetPen(wxPen(border));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(wxRect(wxPoint(0, 0), GetSize()), FromDIP(8));

    const std::string mode = FlowType::grouping_mode();

    auto draw_row = [&](const wxRect &rect, const wxString &title_text, const wxString &desc_text,
                        bool selected, bool enabled, bool hovered) {
        if (hovered && enabled) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(dark ? wxColour("#3A3A3D") : wxColour("#F5F5F5")));
            wxRect hover_rect = rect;
            hover_rect.Inflate(FromDIP(4), FromDIP(2));
            dc.DrawRoundedRectangle(hover_rect, FromDIP(4));
        }
        // Radio: 16px ring, selected adds a 10px brand-green dot.
        const int     ring = FromDIP(16);
        const wxPoint center(rect.x + ring / 2, rect.y + FromDIP(12));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(!enabled ? grey : selected ? BRAND_GREEN : border, FromDIP(1)));
        dc.DrawCircle(center, ring / 2);
        if (selected) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(enabled ? BRAND_GREEN : grey));
            dc.DrawCircle(center, FromDIP(5));
        }
        dc.SetFont(Label::Head_14);
        dc.SetTextForeground(enabled ? title : grey);
        dc.DrawText(title_text, rect.x + FromDIP(22), rect.y + FromDIP(2));
        dc.SetFont(Label::Body_14);
        dc.SetTextForeground(enabled ? desc : grey);
        dc.DrawText(desc_text, rect.x + FromDIP(20), rect.y + FromDIP(26));
    };

    draw_row(m_std_rect, _L("Standard Mode"), _L("Prioritize print quality"),
             mode == FILAMENT_GROUPING_STANDARD, true, m_hovered_row == 0);
    draw_row(m_custom_rect, _L("Custom Mode"), _L("Manually assign filaments to nozzles"),
             mode == FILAMENT_GROUPING_CUSTOM, true, m_hovered_row == 1);
}

}} // namespace Slic3r::GUI
