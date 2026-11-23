#ifndef slic3r_GUI_Widgets_UiAppearance_hpp_
#define slic3r_GUI_Widgets_UiAppearance_hpp_

#include <wx/font.h>
#include <wx/settings.h>
#include <wx/window.h>

#include <algorithm>

namespace Slic3r {
namespace GUI {

// Appearance-state seam between the widgets static library and the running
// application. Widgets read global look-and-feel state (DPI unit, dark mode,
// app fonts, dark restyle) exclusively through this interface; the library
// never links GUI_App. The application installs its implementation at
// startup (GUI_App::OnInit -> set_ui_appearance) before any widget exists;
// the standalone widget lab installs its own.
class UiAppearance
{
public:
    virtual ~UiAppearance() = default;

    // Global DPI unit (GUI_App::em_unit()).
    virtual int em_unit() const = 0;
    // DPI-aware unit for a window: walks the top-level parent and prefers
    // DPIDialog/DPIFrame values. Returns <= 0 when the implementation does
    // not handle the window.
    virtual int em_unit_of(wxWindow *win) const = 0;
    // OS-level DPI of a window. Default: the window's own wx GetDPI.
    virtual int dpi_for_window(wxWindow *win) const
    {
        return win ? win->GetDPI().x : 96;
    }
    // Default UI font scaled for a DPI. Default: the window's font with the
    // point size scaled by dpi/96.
    virtual wxFont default_font_for_dpi(wxWindow *win, int dpi) const
    {
        wxFont font = win ? win->GetFont() : wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
        font.SetPointSize(std::max(1, int(font.GetPointSize() * (float(dpi) / 96.0f) + 0.5f)));
        return font;
    }
    // OS dark mode (GUI_App::dark_mode()).
    virtual bool dark_mode() const = 0;
    // Windows menu-bar dark tracking (check_dark_mode()).
    virtual bool menu_dark_mode() const = 0;
    // Applies the dark background restyle to a window (GUI_App::UpdateDarkUI).
    virtual void update_dark_ui(wxWindow *win, bool highlited = false) const = 0;
    virtual const wxFont &bold_font() const = 0;
    virtual const wxFont &normal_font() const = 0;
    // Main frame fallback for window-less bitmap/DC creation.
    virtual wxWindow *mainframe() const = 0;
    // Re-reads the dark-mode configuration from the OS/config store.
    // Only reachable on the non-Windows system-colour-changed path; a no-op
    // outside the main application.
    virtual void update_dark_config() const {}
};

// Process-global provider. Should be installed before the first widget is
// constructed. Reading without an installed implementation degrades to a
// built-in default appearance (one-time wxLogWarning) instead of crashing —
// use ui_appearance_installed() to detect the degraded mode programmatically.
UiAppearance &ui();
bool ui_appearance_installed();
void set_ui_appearance(UiAppearance *impl);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_Widgets_UiAppearance_hpp_
