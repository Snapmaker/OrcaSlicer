#pragma once

#include <vector>

#include <wx/panel.h>
#include <wx/colour.h>
#include <wx/dcbuffer.h>

#include "../libslic3r/MixedFilament.hpp"
#include "../GUI_App.hpp"

namespace Slic3r {
namespace GUI {

class MixedFilamentDragHandle : public wxPanel
{
public:
    MixedFilamentDragHandle(wxWindow *parent, const wxColour &dot_color, const wxColour &bg_color)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_dot_color(dot_color)
    {
        const wxSize handle_size = parent ? parent->FromDIP(wxSize(14, 18)) : wxSize(14, 18);
        SetMinSize(handle_size);
        SetMaxSize(handle_size);
        SetInitialSize(handle_size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(bg_color);
        SetCursor(wxCursor(wxCURSOR_SIZING));
        Bind(wxEVT_PAINT, &MixedFilamentDragHandle::on_paint, this);
    }

    void set_colors(const wxColour &dot_color, const wxColour &bg_color)
    {
        m_dot_color = dot_color;
        SetBackgroundColour(bg_color);
        Refresh();
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_dot_color));

        const wxSize size = GetClientSize();
        const int    radius = std::max(1, FromDIP(1));
        const int    left_x = std::max(radius, size.x / 2 - FromDIP(2));
        const int    right_x = std::min(size.x - radius - 1, size.x / 2 + FromDIP(2));
        const int    top_y = std::max(radius + 1, size.y / 2 - FromDIP(5));
        const int    gap_y = FromDIP(4);

        for (int row = 0; row < 3; ++row) {
            const int y = top_y + row * gap_y;
            dc.DrawCircle(wxPoint(left_x, y), radius);
            dc.DrawCircle(wxPoint(right_x, y), radius);
        }
    }

    wxColour m_dot_color;
};

std::vector<size_t> build_mixed_filament_ui_indices(const std::vector<MixedFilament> &mixed,
    const std::vector<uint64_t>      &preferred_order);


} // namespace GUI
} // namespace Slic3r
