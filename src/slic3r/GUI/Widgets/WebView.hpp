#ifndef slic3r_GUI_WebView_hpp_
#define slic3r_GUI_WebView_hpp_

#include <wx/webview.h>

#include <string>
#include <utility>
#include <vector>

class WebView
{
public:
    // Ultra: brand_tag prefixes the User-Agent ("SM-Slicer" by default). The Bambu account
    // login must use "BBL-Slicer" or bambulab.com's sign-in won't run the slicer login flow
    // (it just redirects to the marketing home and never posts the token back).
    // script_bridge=false skips the app-wide "wx" script channel: the page then has no way to
    // post anything to the app (the Home tab's hub view, which shows a page served by another
    // process, is built that way).
    static wxWebView *CreateWebView(wxWindow *parent, wxString const &url, wxString const &brand_tag = "SM-Slicer",
                                    bool script_bridge = true);
#if wxUSE_WEBVIEW_EDGE
    static bool CheckWebViewRuntime();
    static bool DownloadAndInstallWebViewRuntime();
#endif
    static void LoadUrl(wxWebView * webView, wxString const &url);

    static bool RunScript(wxWebView * webView, wxString const & msg);

    // After a theme change: refresh every view's User-Agent (it carries the theme token) and
    // reload it - except views that opted out with SetReloadOnThemeChange(view, false), which
    // switch theme live instead (a reload would restart the hub view's camera streams).
    // Snapmaker's Flutter pages (/web/flutter_web/: the U1 Device tab, the pre-print and pre-send
    // pages) are skipped too: they take the theme through ApplyFlutterTheme without a reload.
    static void RecreateAll();
    static void SetReloadOnThemeChange(wxWebView *webView, bool reload);

    // True while the view shows one of the bundled Flutter pages (resources/web/flutter_web).
    static bool IsFlutterPage(wxWebView *webView);
    // Dark mode for a Flutter page: tells the page the slicer's dark mode and dark greys
    // (window.edgeSetDarkMode, added by scripts/patch_flutter_web_dark.py), and the app switches to
    // its own dark or light theme live (new greys, after a theme pack change, reload the page).
    // Runs on every page load of every view and on theme changes; does nothing on other pages.
    static void ApplyFlutterTheme(wxWebView *webView);
    // The colours those pages use in dark mode in place of the app's near-black ones, as
    // (role, "#RRGGBB"), taken from the slicer's own (Bambu) Device page: "bg" the page behind the
    // panels, "card" the panels, "strip" their title bars, "title" the titles - a dark theme pack's
    // colours when one is active, else the stock dark ones (also in light mode, so a later switch
    // to dark matches what the page started with).
    static std::vector<std::pair<std::string, std::string>> FlutterDarkColours();
};

#endif // !slic3r_GUI_WebView_hpp_
