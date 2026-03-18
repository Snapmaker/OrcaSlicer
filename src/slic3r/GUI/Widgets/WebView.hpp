#ifndef slic3r_GUI_WebView_hpp_
#define slic3r_GUI_WebView_hpp_

#include <wx/webview.h>

class WebView
{
public:
    static wxWebView *CreateWebView(wxWindow *parent, wxString const &url);
    /// Create WebView with orca:// scheme handler for local directory (on-demand disk read).
    static wxWebView *CreateWebViewWithLocalRoot(wxWindow *parent, wxString const &url, wxString const &localRootPath,
                                                  wxString const &userAssetsRoot = wxEmptyString);
#if wxUSE_WEBVIEW_EDGE
    static bool CheckWebViewRuntime();
    static bool DownloadAndInstallWebViewRuntime();
    /// SetVirtualHostNameToFolderMapping：https://orca.local/* → localFolderPath/*
    static bool SetupOrcaScheme(wxWebView* webView, wxString const& localFolderPath);
#endif
    static void LoadUrl(wxWebView * webView, wxString const &url);

    static bool RunScript(wxWebView * webView, wxString const & msg);

    static void RecreateAll();
};

#endif // !slic3r_GUI_WebView_hpp_
