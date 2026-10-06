#ifndef slic3r_MacDarkMode_hpp_
#define slic3r_MacDarkMode_hpp_

#include <wx/event.h>

namespace Slic3r {
namespace GUI {

#if __APPLE__
extern bool mac_dark_mode();
extern double mac_max_scaling_factor();
extern void set_miniaturizable(void * window);
void WKWebView_evaluateJavaScript(void * web, wxString const & script, void (*callback)(wxString const &));
void WKWebView_setTransparentBackground(void * web);
void set_tag_when_enter_full_screen(bool isfullscreen);
void set_title_colour_after_set_title(void * window);
void initGestures(void * view,  wxEvtHandler * handler);
void openFolderForFile(wxString const & file);
// The `--hub` helper runs from the same bundle as the slicer. Without this it is a second
// regular app with its own Dock tile (showing the bundle icon); clicking that tile activated
// the window-less hub and sent the slicer window behind other apps.
void mac_make_accessory_app();
// Bring this process to the front (an accessory app's dialogs open behind the active app).
void mac_activate_app();
#endif


} // namespace GUI
} // namespace Slic3r

#endif // MacDarkMode_h
