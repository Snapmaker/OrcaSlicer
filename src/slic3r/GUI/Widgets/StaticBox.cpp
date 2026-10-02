#include "StaticBox.hpp"

#include <algorithm>
#include "../GUI.hpp"
#include <wx/dcclient.h>
#include <wx/dcgraph.h>

BEGIN_EVENT_TABLE(StaticBox, wxWindow)

// catch paint events
//EVT_ERASE_BACKGROUND(StaticBox::eraseEvent)
EVT_PAINT(StaticBox::paintEvent)

END_EVENT_TABLE()

/*
 * Called by the system of by wxWidgets when the panel needs
 * to be redrawn. You can also trigger this call by
 * calling Refresh()/Update().
 */

StaticBox::StaticBox()
    : state_handler(this)
    , radius(8)
{
    border_color = StateColor(
        std::make_pair(0xF0F0F1, (int) StateColor::Disabled), 
        std::make_pair(0x303A3C, (int) StateColor::Normal));
}

StaticBox::StaticBox(wxWindow* parent,
                   wxWindowID      id,
                   const wxPoint & pos,
                   const wxSize &  size, long style)
    : StaticBox()
{
    Create(parent, id, pos, size, style);
}

bool StaticBox::Create(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
{
    if (style & wxBORDER_NONE)
        border_width = 0;
    wxWindow::Create(parent, id, pos, size, style);
    state_handler.attach({&border_color, &background_color, &background_color2});
    state_handler.update_binds();
    SetBackgroundColour(GetParentBackgroundColor(parent));
    return true;
}

static int g_theme_button_radius = -1;
static int g_theme_box_radius    = -1;

void StaticBox::SetThemeRadius(int button_dip, int box_dip)
{
    g_theme_button_radius = button_dip;
    g_theme_box_radius    = box_dip;
}

double StaticBox::drawn_radius() const
{
    if (radius <= 0)
        return radius;
    const int theme = IsThemeButton() ? g_theme_button_radius : g_theme_box_radius;
    if (theme < 0)
        return radius;
    // A round control (a dot, a circular button) keeps its shape.
    const wxSize size = GetSize();
    if (radius * 2 >= std::min(size.x, size.y) - 1 && size.x * 4 <= size.y * 5)
        return radius;
    return FromDIP(theme);
}

void StaticBox::SetCornerRadius(double radius)
{
    this->radius = radius;
    Refresh();
}

void StaticBox::SetBorderWidth(int width)
{
    border_width = width;
    Refresh();
}

void StaticBox::SetBorderColor(StateColor const &color)
{
    border_color = color;
    state_handler.update_binds();
    Refresh();
}

void StaticBox::SetBorderColorNormal(wxColor const &color)
{
    border_color.setColorForStates(color, 0);
    Refresh();
}

void StaticBox::SetBackgroundColor(StateColor const &color)
{
    background_color = color;
    state_handler.update_binds();
    Refresh();
}

void StaticBox::SetBackgroundColorNormal(wxColor const &color)
{
    background_color.setColorForStates(color, 0);
    Refresh();
}

void StaticBox::SetBackgroundColor2(StateColor const &color)
{
    background_color2 = color;
    state_handler.update_binds();
    Refresh();
}

wxColor StaticBox::GetParentBackgroundColor(wxWindow* parent)
{
#ifdef __WXMSW__
    // A plain child panel that never had a colour of its own reports the system button face
    // (#F0F0F0), but MSW paints it with its nearest coloured ancestor's brush. Copy what is
    // actually on screen, or a Label on such a panel shows a #F0F0F0 band in light mode and a
    // #3F3F46 one (the dark twin of #F0F0F0) in dark mode.
    while (parent != nullptr && !parent->UseBgCol() && !parent->IsTopLevel() && parent->GetParent() != nullptr &&
           parent->HasTransparentBackground())
        parent = parent->GetParent();
#endif
    if (auto box = dynamic_cast<StaticBox*>(parent)) {
        if (box->background_color.count() > 0) {
            if (box->background_color2.count() == 0)
                return box->background_color.defaultColor();
            auto s = box->background_color.defaultColor();
            auto e = box->background_color2.defaultColor();
            int r = (s.Red() + e.Red()) / 2;
            int g = (s.Green() + e.Green()) / 2;
            int b = (s.Blue() + e.Blue()) / 2;
            return wxColor(r, g, b);
        }
    }
    if (parent)
        return parent->GetBackgroundColour();
    return *wxWHITE;
}

void StaticBox::eraseEvent(wxEraseEvent& evt)
{
    // for transparent background, but not work
#ifdef __WXMSW__
    wxDC *dc = evt.GetDC();
    wxSize size = GetSize();
    wxClientDC dc2(GetParent());
    dc->Blit({0, 0}, size, &dc2, GetPosition());
#endif
}

void StaticBox::paintEvent(wxPaintEvent& evt)
{
    // depending on your system you may need to look at double-buffered dcs
    wxPaintDC dc(this);
    render(dc);
}

/*
 * Here we do the actual rendering. I put it in a separate
 * method so that it can work no matter what type of DC
 * (e.g. wxPaintDC or wxClientDC) is used.
 */
void StaticBox::render(wxDC& dc)
{
#ifdef __WXMSW__
    if (drawn_radius() == 0) {
        doRender(dc);
        return;
    }

	wxSize size = GetSize();
    if (size.x <= 0 || size.y <= 0)
        return;
    wxMemoryDC memdc(&dc);
    if (!memdc.IsOk()) {
        doRender(dc);
        return;
    }
    wxBitmap bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    //memdc.Blit({0, 0}, size, &dc, {0, 0});
    memdc.SetBackground(wxBrush(GetBackgroundColour()));
    memdc.Clear();
    {
        wxGCDC dc2(memdc);
        doRender(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
	dc.DrawBitmap(bmp, 0, 0);
#else
    doRender(dc);
#endif
}

void StaticBox::doRender(wxDC& dc)
{
    wxSize size = GetSize();
    int states = state_handler.states();
    if (background_color2.count() == 0) {
        if ((border_width && border_color.count() > 0) || background_color.count() > 0) {
            wxRect rc(0, 0, size.x, size.y);
            if (border_width && border_color.count() > 0) {
                if (dc.GetContentScaleFactor() == 1.0) {
                    int d  = floor(border_width / 2.0);
                    int d2 = floor(border_width - 1);
                    rc.x += d;
                    rc.width -= d2;
                    rc.y += d;
                    rc.height -= d2;
                } else {
                    int d  = 1;
                    rc.x += d;
                    rc.width -= d;
                    rc.y += d;
                    rc.height -= d;
                }
                dc.SetPen(wxPen(border_color.colorForStates(states), border_width));
            } else {
                dc.SetPen(wxPen(background_color.colorForStates(states)));
            }
            if (background_color.count() > 0)
                dc.SetBrush(wxBrush(background_color.colorForStates(states)));
            else
                dc.SetBrush(wxBrush(GetBackgroundColour()));
            const double r = drawn_radius();
            if (r == 0) {
                dc.DrawRectangle(rc);
            }
            else {
                dc.DrawRoundedRectangle(rc, r - border_width);
            }
        }
    }
    else {
        wxColor start = background_color.colorForStates(states);
        wxColor stop = background_color2.colorForStates(states);
        int r = start.Red(), g = start.Green(), b = start.Blue();
        int dr = (int) stop.Red() - r, dg = (int) stop.Green() - g, db = (int) stop.Blue() - b;
        int lr = 0, lg = 0, lb = 0;
        for (int y = 0; y < size.y; ++y) {
            dc.SetPen(wxPen(wxColor(r, g, b)));
            dc.DrawLine(0, y, size.x, y);
            lr += dr; while (lr >= size.y) { ++r, lr -= size.y; } while (lr <= -size.y) { --r, lr += size.y; }
            lg += dg; while (lg >= size.y) { ++g, lg -= size.y; } while (lg <= -size.y) { --g, lg += size.y; }
            lb += db; while (lb >= size.y) { ++b, lb -= size.y; } while (lb <= -size.y) { --b, lb += size.y; }
        }
    }
}
