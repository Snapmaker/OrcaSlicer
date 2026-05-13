#pragma once

#include <wx/button.h>
#include <wx/dcclient.h>
#include <wx/bitmap.h>

#include <vector>
#include <string>

namespace Slic3r {
struct MixedFilament;
struct MixedFilamentDisplayContext;
}

namespace Slic3r { namespace GUI {

// Unified colour-block parameters used by both solid and gradient swatches.
struct ColorBlockParams
{
    enum Mode { Solid, Gradient };
    Mode mode = Solid;
    wxColour solid_color;
    std::vector<wxColour> gradient_colors; // 2-stop gradient, already sorted (bottom→top)
    wxString label;
    int width  = 20;
    int height = 20;
};

// Linear interpolation across an ordered list of colours (0.0 → colors[0], 1.0 → colors.back()).
wxColour interpolate_color(const std::vector<wxColour>& colors, double pos);

// Cached colour-block bitmap. The static BitmapCache lives inside the implementation.
// Key format:  "solid:#RRGGBB:hH:wW:label"  or  "grad:#RRGGBB:#RRGGBBBT:hH:wW:label"
wxBitmap* get_color_block_bitmap_cached(const ColorBlockParams& params);

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
};

// Create a menu/dropdown bitmap for a mixed filament.
// Matches MixedFilamentBadge drawing style (font, border, gradient direction).
// Returns a pointer into a static BitmapCache — caller must NOT delete it.
wxBitmap* create_mixed_filament_menu_bitmap(const MixedFilament&               mf,
                                            const MixedFilamentDisplayContext& ctx,
                                            int  width, int  height,
                                            const wxString& label);

}} // namespace Slic3r::GUI
