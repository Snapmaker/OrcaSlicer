#pragma once
// ScalableBitmap / create_scaled_bitmap: DPI-aware bitmap loading extracted
// from slic3r/GUI/wxExtensions.{hpp,cpp}. All app-state access goes through
// the UiAppearance seam (widgets/UiAppearance.hpp), so this translation unit
// compiles into libslic3r_widgets and can be linked by the main GUI library
// or by an out-of-tree standalone widget lab.
//
// Behaviour differences from the wxExtensions implementation:
// - a missing bitmap no longer throws: a placeholder is drawn and the name is
//   registered in the missing-bitmap registry (the old throw ran from widget
//   construction, where nothing caught it - it was a crash, not a policy);
// - DIP conversion falls back to the raw pixel count when the window is null
//   (the default UiAppearance exposes no mainframe).

#include <string>
#include <vector>

#include <wx/bitmap.h>
#include <wx/gdicmn.h>
#include <wx/window.h>

namespace Slic3r {
namespace GUI {

class BitmapCache;

}} // namespace Slic3r::GUI

// DPI unit helper shared with slic3r/GUI/wxExtensions.hpp (which keeps its own
// declaration of the same global function); implementation lives in
// ScalableBitmap.cpp and is pure UiAppearance-seam code.
int em_unit(wxWindow* win);

// BBS: support resize by fill border
wxBitmap create_scaled_bitmap(const std::string& bmp_name, wxWindow *win = nullptr,
    const int px_cnt = 16, const bool grayscale = false,
    const std::string& new_color = std::string(), // color witch will used instead of orange
    const bool menu_bitmap = false, const bool resize = false,
    const bool bitmap2 = false,// for create_scaled_bitmap2
    const std::vector<std::string>& array_new_color = std::vector<std::string>());
//used for semi transparent material
wxBitmap create_scaled_bitmap2(const std::string& bmp_name_in, Slic3r::GUI::BitmapCache& cache, wxWindow* win = nullptr,
    const int px_cnt = 16, const bool grayscale = false, const bool resize = false,
    const std::vector<std::string>& array_new_color = std::vector<std::string>()); // color witch will used instead of orange

// Missing-resource registry (fix-widget-resource-crash, controlled-degradation
// semantics): when a bitmap resource fails to load,
// create_scaled_bitmap no longer throws - it returns a recognizable placeholder
// bitmap and records the missing resource name
// in this list (once per name) with a wxLogWarning. Used for runtime
// diagnostics and test assertions;
// failures are never silently swallowed.
std::vector<std::string> const& missing_bitmap_resources();

// ----------------------------------------------------------------------------
// ScalableBitmap
// ----------------------------------------------------------------------------

// NOTE: kept in the global namespace exactly as in slic3r/GUI/wxExtensions.hpp
// (the class was never inside Slic3r::GUI; consumers reference it unqualified).
class ScalableBitmap
{
public:
    ScalableBitmap() {};
    ScalableBitmap( wxWindow *parent,
                    const std::string& icon_name = "",
                    const int px_cnt = 16,
                    const bool grayscale = false,
                    const bool resize = false,
                    const bool bitmap2 = false,
                    const std::vector<std::string>& new_color = std::vector<std::string>());// BBS: support resize by fill border
    // Bitmap-constructed instance (e.g. an icon loaded from a local file):
    // carries no resource name, so msw_rescale() keeps the bitmap as-is
    // instead of re-loading it by name.
    ScalableBitmap( wxWindow *parent, const wxBitmap &bmp);

    ~ScalableBitmap() {}

    wxSize  GetBmpSize() const;
    int     GetBmpWidth() const;
    int     GetBmpHeight() const;

    void                msw_rescale();

    const wxBitmap&     bmp() const { return m_bmp; }
    wxBitmap&           bmp()       { return m_bmp; }
    const std::string&  name() const{ return m_icon_name; }

    int                 px_cnt()const           {return m_px_cnt;}

private:
    wxWindow*       m_parent{ nullptr };
    wxBitmap        m_bmp = wxBitmap();
    std::string     m_icon_name = "";
    int             m_px_cnt {16};
    bool            m_grayscale{ false };
    bool            m_resize{ false };
};
