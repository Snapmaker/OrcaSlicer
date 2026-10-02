#ifndef slic3r_GUI_WebView_hpp_
#define slic3r_GUI_WebView_hpp_

#include <wx/webview.h>

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
    // Dark mode for a Flutter page: the app forces its light theme and draws into a canvas, so in
    // dark mode a colour filter (resources/web/include/flutter_dark.js) goes over that canvas, mapped
    // onto the slicer's dark background and text colours; in light mode it comes off again. Runs on
    // every page load of every view and on theme changes; does nothing on other pages.
    static void ApplyFlutterTheme(wxWebView *webView);
};

#endif // !slic3r_GUI_WebView_hpp_
