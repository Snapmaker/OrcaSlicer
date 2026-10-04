#ifndef slic3r_GUI_ModelBrowserPanel_hpp_
#define slic3r_GUI_ModelBrowserPanel_hpp_

#include <functional>
#include <memory>

#include <wx/panel.h>

namespace Slic3r {
namespace GUI {

// Whether Home > Models is offered: Windows only in this phase, and Preferences > "Model browser
// (beta)" (app_config "model_browser_beta", on by default).
bool model_browser_enabled();

// Home > Models: a small web browser for the model sites (Printables, MakerWorld, Snapmaker Space).
//
// Security model (see libslic3r/ModelBrowserRules.hpp for the rules, which are unit tested):
//  - Its own WebView2 environment and user data folder (%LOCALAPPDATA%\<app>\ModelBrowser): its
//    cookies, logins and cache never mix with the app's own web views (page server, hub, sign-in
//    dialogs), and those views' cookies are never visible to the sites. Logins persist there.
//  - It is never made with WebView::CreateWebView and carries no script bridge: web messages and
//    host objects are off, and the panel refuses to show a view in which they are not (asserted
//    after the settings are applied).
//  - Nothing on this computer or the LAN is loaded, top-level, in frames or as a sub-resource
//    (our page server and the phone hub live there). No file:, no other programs' URL schemes:
//    Printables' "Open in" buttons are read and handed to the app's downloader (its domain checks
//    apply), MakerWorld's are not followed (a notice points to the download button).
//  - Downloads: model files go through the safe download path (sanitized name, exclusive marker,
//    no-replace placement, size and magic-byte checks) and then the import queue; programs and
//    scripts are refused; anything else may be saved to Downloads after asking.
//  - Permissions (camera, microphone, location, notifications, clipboard, ...) are denied,
//    password saving and autofill are off, DevTools only in developer mode, an honest User-Agent.
//  - Popups only for sign-in (or a popup the user clicked for), in the same isolated browser.
class ModelBrowserPanel : public wxPanel
{
public:
    // on_leave: the "Home" button (back to the Home sections).
    ModelBrowserPanel(wxWindow* parent, std::function<void()> on_leave);
    ~ModelBrowserPanel() override;

    // The panel was shown (true) or hidden (false). The browser is created the first time it is
    // shown, never before.
    void set_active(bool active);
    void sys_color_changed();

    struct Impl;

private:
    std::unique_ptr<Impl> p;
};

} // namespace GUI
} // namespace Slic3r

#endif
