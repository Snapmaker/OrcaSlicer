#pragma once

#include <wx/button.h>
#include <wx/dcclient.h>

#include <vector>

namespace Slic3r {
struct MixedFilament;
struct MixedFilamentDisplayContext;
}

namespace Slic3r { namespace GUI {

class MixedFilamentBadge : public wxButton
{
public:
    MixedFilamentBadge(wxWindow* parent, wxWindowID id, int virtual_id,
                       const MixedFilament& mf,
                       const MixedFilamentDisplayContext& display_context,
                       bool show_number = true, int badge_size = 20);

private:
    wxColour m_solid_color;
    bool m_is_gradient = false;
    bool m_show_number = true;
    std::vector<wxColour> m_gradient_colors;

    void on_paint(wxPaintEvent&);
    wxColour interpolate_color(const std::vector<wxColour>& colors, double pos);
};

// Create a menu/dropdown bitmap for a mixed filament.
// Matches MixedFilamentBadge drawing style (font, border, gradient direction).
wxBitmap create_mixed_filament_menu_bitmap(const MixedFilament&               mf,
                                           const MixedFilamentDisplayContext& ctx,
                                           int  width, int  height,
                                           const wxString& label);

}} // namespace Slic3r::GUI
