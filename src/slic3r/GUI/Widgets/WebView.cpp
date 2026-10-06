#include "WebView.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/MacDarkMode.hpp"
#include "slic3r/Utils/LoginUserAgent.hpp"

#include <algorithm>
#include <cctype>
#include <boost/log/trivial.hpp>

#include <wx/webviewarchivehandler.h>
#include <wx/webviewfshandler.h>
#if wxUSE_WEBVIEW_EDGE
#include <wx/msw/webview_edge.h>
#elif defined(__WXMAC__)
#include <wx/osx/webview_webkit.h>
#endif
#include <wx/uri.h>
#if defined(__WIN32__) || defined(__WXMAC__)
#include "wx/private/jsscriptwrapper.h"
#endif

#include "sentry_wrapper/SentryWrapper.hpp"

#if defined(__linux__)
#include <mutex>
#endif

#ifdef __WIN32__
#include <WebView2.h>
#include <Shellapi.h>
#include <slic3r/Utils/Http.hpp>
#elif defined __linux__
#include <gtk/gtk.h>
#define WEBKIT_API
struct WebKitWebView;
struct WebKitJavascriptResult;
struct WebKitWebContext;
struct WebKitUserContentManager;
extern "C" {
WEBKIT_API void
webkit_web_view_run_javascript                       (WebKitWebView             *web_view,
                                                      const gchar               *script,
                                                      GCancellable              *cancellable,
                                                      GAsyncReadyCallback       callback,
                                                      gpointer                  user_data);
WEBKIT_API WebKitJavascriptResult *
webkit_web_view_run_javascript_finish                (WebKitWebView             *web_view,
                                                      GAsyncResult              *result,
						      GError                    **error);
WEBKIT_API void
webkit_javascript_result_unref              (WebKitJavascriptResult *js_result);
WEBKIT_API WebKitWebContext *
webkit_web_context_get_default              (void);
WEBKIT_API void
webkit_web_context_set_preferred_languages  (WebKitWebContext          *context,
                                             const gchar * const       *languages);
WEBKIT_API WebKitUserContentManager *
webkit_web_view_get_user_content_manager    (WebKitWebView             *web_view);
WEBKIT_API void
webkit_user_content_manager_unregister_script_message_handler(WebKitUserContentManager *manager,
                                                              const gchar              *name);
}
#endif

#ifdef __WIN32__
// Run Download and Install in another thread so we don't block the UI thread
DWORD DownloadAndInstallWV2RT() {

  int returnCode = 2; // Download failed
  // Use fwlink to download WebView2 Bootstrapper at runtime and invoke installation
  // Broken/Invalid Https Certificate will fail to download
  // Use of the download link below is governed by the below terms. You may acquire the link
  // for your use at https://developer.microsoft.com/microsoft-edge/webview2/. Microsoft owns
  // all legal right, title, and interest in and to the WebView2 Runtime Bootstrapper
  // ("Software") and related documentation, including any intellectual property in the
  // Software. You must acquire all code, including any code obtained from a Microsoft URL,
  // under a separate license directly from Microsoft, including a Microsoft download site
  // (e.g., https://developer.microsoft.com/microsoft-edge/webview2/).
  // HRESULT hr = URLDownloadToFileW(NULL, L"https://go.microsoft.com/fwlink/p/?LinkId=2124703",
  //                               L".\\plugin\\MicrosoftEdgeWebview2Setup.exe", 0, 0);
  fs::path target_file_path = (fs::temp_directory_path() / "MicrosoftEdgeWebview2Setup.exe");
  bool downloaded = false;
  Slic3r::Http::get("https://go.microsoft.com/fwlink/p/?LinkId=2124703")
      .on_error([](std::string body, std::string error, unsigned http_status) {

      })
      .on_complete([&downloaded, target_file_path](std::string body, unsigned http_status) {
        fs::fstream file(target_file_path, std::ios::out | std::ios::binary | std::ios::trunc);
        file.write(body.c_str(), body.size());
        file.flush();
        file.close();

        downloaded = true;
      })
      .perform_sync();
  // Sleep for 1 second to wait for the buffer writen into disk
  std::this_thread::sleep_for(1000ms);
  if (downloaded) {
    // Either Package the WebView2 Bootstrapper with your app or download it using fwlink
    // Then invoke install at Runtime.
    SHELLEXECUTEINFOW shExInfo = {0};
    shExInfo.cbSize = sizeof(shExInfo);
    shExInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    shExInfo.hwnd = 0;
    shExInfo.lpVerb = L"runas";
    shExInfo.lpFile = target_file_path.generic_wstring().c_str();
    shExInfo.lpParameters = L" /install";
    shExInfo.lpDirectory = 0;
    shExInfo.nShow = 0;
    shExInfo.hInstApp = 0;

    if (ShellExecuteExW(&shExInfo)) {
      WaitForSingleObject(shExInfo.hProcess, INFINITE);
      returnCode = 0; // Install successfull
    } else {
      returnCode = 1; // Install failed
    }
  }
  return returnCode;
}

class WebViewEdge : public wxWebViewEdge
{
public:
    bool SetUserAgent(const wxString &userAgent)
    {
        bool dark = userAgent.Contains("dark");
        SetColorScheme(dark ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT);

        ICoreWebView2 *webView2 = (ICoreWebView2 *) GetNativeBackend();
        if (webView2) {
            ICoreWebView2Settings *settings;
            HRESULT                hr = webView2->get_Settings(&settings);
            if (hr == S_OK) {
                ICoreWebView2Settings2 *settings2;
                hr = settings->QueryInterface(&settings2);
                if (hr == S_OK) {
                    settings2->put_UserAgent(userAgent.wc_str());
                    settings2->Release();
                    return true;
                }
            }
            settings->Release();
            return false;
        }
        pendingUserAgent = userAgent;
        return true;
    }

    bool SetColorScheme(COREWEBVIEW2_PREFERRED_COLOR_SCHEME colorScheme)
    {
        ICoreWebView2 *webView2 = (ICoreWebView2 *) GetNativeBackend();
        if (webView2) {
            ICoreWebView2_13 * webView2_13;
            HRESULT           hr = webView2->QueryInterface(&webView2_13);
            if (hr == S_OK) {
                ICoreWebView2Profile *profile;
                hr = webView2_13->get_Profile(&profile);
                if (hr == S_OK) {
                    profile->put_PreferredColorScheme(colorScheme);
                    profile->Release();
                    return true;
                }
                webView2_13->Release();
            }
            return false;
        }
        pendingColorScheme = colorScheme;
        return true;
    }

    void DoGetClientSize(int *x, int *y) const override
    {
        if (!pendingUserAgent.empty()) {
            auto thiz = const_cast<WebViewEdge *>(this);
            auto userAgent = std::move(thiz->pendingUserAgent);
            thiz->pendingUserAgent.clear();
            thiz->SetUserAgent(userAgent);
        }
        if (pendingColorScheme) {
            auto thiz      = const_cast<WebViewEdge *>(this);
            auto colorScheme = pendingColorScheme;
            thiz->pendingColorScheme = COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO;
            thiz->SetColorScheme(colorScheme);
        }
        wxWebViewEdge::DoGetClientSize(x, y);
    };
private:
    wxString pendingUserAgent;
    COREWEBVIEW2_PREFERRED_COLOR_SCHEME pendingColorScheme = COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO;
};

#elif defined __WXOSX__

class WebViewWebKit : public wxWebViewWebKit
{
public:
    explicit WebViewWebKit(const wxString &initialUrl) : m_pendingUrl(initialUrl) {}

    ~WebViewWebKit() override
    {
        RemoveScriptMessageHandler("wx");
    }

    void LoadURL(const wxString &url) override
    {
        if (!m_scriptMessageHandlerInstalled && url != wxString("about:blank")) {
            m_pendingUrl = url;
            return;
        }
        RequestLoad(url);
    }

    void SetScriptMessageHandlerInstalled()
    {
        m_scriptMessageHandlerInstalled = true;
        auto url = std::move(m_pendingUrl);
        m_pendingUrl.clear();
        if (!url.empty() && url != wxString("about:blank"))
            RequestLoad(url);
    }

    void AttachNavigationGate()
    {
        Bind(wxEVT_WEBVIEW_LOADED, &WebViewWebKit::OnNavigationSettled, this);
        Bind(wxEVT_WEBVIEW_ERROR, &WebViewWebKit::OnNavigationSettled, this);
    }

private:
    static bool IsBlankUrl(const wxString &url)
    {
        return url.empty() || url == wxString("about:blank");
    }

    static wxString CanonicalUrl(const wxString &url)
    {
        if (IsBlankUrl(url))
            return url;
        return wxURI(url).BuildURI();
    }

    static bool IsFlutterUrl(const wxString &url)
    {
        return url.find("flutter_web") != std::string::npos;
    }

    static wxString UrlPath(const wxString &url)
    {
        if (IsBlankUrl(url))
            return url;
        return wxURI(url).GetPath();
    }

    void RequestLoad(const wxString &url)
    {
        if (IsBlankUrl(url)) {
            wxWebViewWebKit::LoadURL(url);
            return;
        }

        const wxString target = CanonicalUrl(url);
        if (!m_inFlightUrl.empty()) {
            if (UrlPath(target) == UrlPath(m_inFlightUrl))
                return;
            // Flutter interrupted by another Flutter → -999 white screen. Queue that case only.
            // missing_connection.gif sitting in-flight while the view is hidden never settles,
            // so queuing path=2 behind it leaves Device on the GIF forever.
            if (IsFlutterUrl(m_inFlightUrl) && IsFlutterUrl(target)) {
                m_queuedUrl = target;
                return;
            }
        }

        m_queuedUrl.clear();
        m_inFlightUrl = target;
        wxWebViewWebKit::LoadURL(target);
    }

    void FlushQueued()
    {
        if (m_queuedUrl.empty())
            return;
        auto url = std::move(m_queuedUrl);
        m_queuedUrl.clear();
        m_inFlightUrl = url;
        wxWebViewWebKit::LoadURL(url);
    }

    void OnNavigationSettled(wxWebViewEvent &evt)
    {
        evt.Skip();
        const wxString eventUrl = CanonicalUrl(evt.GetURL());
        if (IsBlankUrl(eventUrl))
            return;
        // Ignore the document we just replaced (GIF -999 after jumping to path=2).
        if (!m_inFlightUrl.empty() && UrlPath(eventUrl) != UrlPath(m_inFlightUrl))
            return;
        m_inFlightUrl.clear();
        FlushQueued();
    }

    bool     m_scriptMessageHandlerInstalled = false;
    wxString m_pendingUrl;
    wxString m_inFlightUrl;
    wxString m_queuedUrl;
};

#endif

class FakeWebView : public wxWebView
{
    virtual bool Create(wxWindow* parent, wxWindowID id, const wxString& url, const wxPoint& pos, const wxSize& size, long style, const wxString& name) override { return false; }
    virtual wxString GetCurrentTitle() const override { return wxString(); }
    virtual wxString GetCurrentURL() const override { return wxString(); }
    virtual bool IsBusy() const override { return false; }
    virtual bool IsEditable() const override { return false; }
    virtual void LoadURL(const wxString& url) override { }
    virtual void Print() override { }
    virtual void RegisterHandler(wxSharedPtr<wxWebViewHandler> handler) override { }
    virtual void Reload(wxWebViewReloadFlags flags = wxWEBVIEW_RELOAD_DEFAULT) override { }
    virtual bool RunScript(const wxString& javascript, wxString* output = NULL) const override { return false; }
    virtual void SetEditable(bool enable = true) override { }
    virtual void Stop() override { }
    virtual bool CanGoBack() const override { return false; }
    virtual bool CanGoForward() const override { return false; }
    virtual void GoBack() override { }
    virtual void GoForward() override { }
    virtual void ClearHistory() override { }
    virtual void EnableHistory(bool enable = true) override { }
    virtual wxVector<wxSharedPtr<wxWebViewHistoryItem>> GetBackwardHistory() override { return {}; }
    virtual wxVector<wxSharedPtr<wxWebViewHistoryItem>> GetForwardHistory() override { return {}; }
    virtual void LoadHistoryItem(wxSharedPtr<wxWebViewHistoryItem> item) override { }
    virtual bool CanSetZoomType(wxWebViewZoomType type) const override { return false; }
    virtual float GetZoomFactor() const override { return 0.0f; }
    virtual wxWebViewZoomType GetZoomType() const override { return wxWebViewZoomType(); }
    virtual void SetZoomFactor(float zoom) override { }
    virtual void SetZoomType(wxWebViewZoomType zoomType) override { }
    virtual bool CanUndo() const override { return false; }
    virtual bool CanRedo() const override { return false; }
    virtual void Undo() override { }
    virtual void Redo() override { }
    virtual void* GetNativeBackend() const override { return nullptr; }
    virtual void DoSetPage(const wxString& html, const wxString& baseUrl) override { }
};

wxDEFINE_EVENT(EVT_WEBVIEW_RECREATED, wxCommandEvent);

static std::vector<wxWebView*> g_webviews;
static std::vector<wxWebView*> g_delay_webviews;
// Views RecreateAll() must not reload (WebView::SetReloadOnThemeChange).
static std::vector<wxWebView*> g_no_theme_reload;

class WebViewRef : public wxObjectRefData
{
public:
    WebViewRef(wxWebView *webView) : m_webView(webView) {}
    ~WebViewRef() {
        auto iter = std::find(g_webviews.begin(), g_webviews.end(), m_webView);
        assert(iter != g_webviews.end());
        if (iter != g_webviews.end())
            g_webviews.erase(iter);
        // Drop pending handler installs so a later g_delay_webviews flush never
        // calls AddScriptMessageHandler() on a destroyed view.
        // See bambulab/BambuStudio #11004 and #10968.
        auto diter = std::find(g_delay_webviews.begin(), g_delay_webviews.end(), m_webView);
        if (diter != g_delay_webviews.end())
            g_delay_webviews.erase(diter);
        auto niter = std::find(g_no_theme_reload.begin(), g_no_theme_reload.end(), m_webView);
        if (niter != g_no_theme_reload.end())
            g_no_theme_reload.erase(niter);
    }
    wxWebView *m_webView;
};

#if defined(__linux__)
// The flatpak launcher exports LC_ALL=C.UTF-8 (BambuStudio #3440). WebKitGTK derives
// navigator.languages from LC_CTYPE and maps only a bare "C" to en-US, so "C.UTF-8" becomes
// the tag "C". Intl.Locale rejects that, the Flutter web engine shipped since v2.4.0 fails to
// start, and every web view stays blank. Hand WebKit the UI language as a valid BCP 47 tag.
static std::string to_bcp47_language_tag(std::string name)
{
    name = name.substr(0, name.find_first_of(".@")); // drop codeset / modifier: "ja_JP.UTF-8" -> "ja_JP"
    std::replace(name.begin(), name.end(), '_', '-');
    const size_t primary = std::min(name.find('-'), name.size());
    const bool valid = (primary == 2 || primary == 3) &&
        std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '-'; });
    return valid ? name : "en-US"; // "C", "POSIX", "" or anything odd would reintroduce the bug
}

static void apply_webkit_preferred_language()
{
    const wxLocale *locale = wxGetLocale();
    const std::string tag = to_bcp47_language_tag(locale ? locale->GetCanonicalName().ToStdString() : std::string());
    static std::string s_applied; // re-applied only when the UI language changes
    if (tag == s_applied)
        return;
    WebKitWebContext *ctx = webkit_web_context_get_default();
    if (ctx == nullptr)
        return;
    const gchar *const languages[] = {tag.c_str(), nullptr};
    webkit_web_context_set_preferred_languages(ctx, languages);
    s_applied = tag;
    BOOST_LOG_TRIVIAL(info) << "WebKit preferred language: " << tag;
}

// wx connects "script-message-received" on the view's WebKitUserContentManager with the
// wxWebViewWebKit as user data, but ~wxWebViewWebKit only disconnects handlers from the
// WebKitWebView. A message the page still has in flight during teardown (e.g. a language
// switch, where AddScriptMessageHandler() on a new view spins a nested main loop) then reaches
// a freed object. Unregister the handler and drop wx's connections before the view goes away.
// The native view is captured here: on the plain delete path wxEVT_DESTROY is only sent from
// ~wxWindow, when calling back into the wxWebView is no longer safe, while the GTK widget itself
// is disposed only after that event.
static void disconnect_script_messages_on_destroy(wxWebView *webView)
{
    WebKitWebView *view = static_cast<WebKitWebView *>(webView->GetNativeBackend());
    if (view == nullptr)
        return;
    webView->Bind(wxEVT_DESTROY, [webView, view](wxWindowDestroyEvent &evt) {
        evt.Skip();
        if (evt.GetEventObject() != webView)
            return; // destroy events of child windows propagate up to us
        WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(view);
        if (ucm == nullptr)
            return;
        webkit_user_content_manager_unregister_script_message_handler(ucm, "wx");
        g_signal_handlers_disconnect_by_data(ucm, webView);
    });
}
#endif

wxWebView* WebView::CreateWebView(wxWindow * parent, wxString const & url, wxString const & brand_tag, bool script_bridge)
{
#if wxUSE_WEBVIEW_EDGE
    // Check if a fixed version of edge is present in
    // $executable_path/edge_fixed and use it
    wxFileName edgeFixedDir(wxStandardPaths::Get().GetExecutablePath());
    edgeFixedDir.SetFullName("");
    edgeFixedDir.AppendDir("edge_fixed");
    if (edgeFixedDir.DirExists()) {
        wxWebViewEdge::MSWSetBrowserExecutableDir(edgeFixedDir.GetFullPath());
        wxLogMessage("Using fixed edge version");
    }
#endif
    auto url2  = url;
#ifdef __WIN32__
    url2.Replace("\\", "/");
#endif
    if (!url2.empty()) { url2 = wxURI(url2).BuildURI(); }
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << url2.ToUTF8();

#ifdef __WIN32__
    wxWebView* webView = new WebViewEdge;
#elif defined(__WXOSX__)
    wxWebView *webView = new WebViewWebKit(url2);
#else
#if defined(__linux__)
    apply_webkit_preferred_language();
#endif
    auto webView = wxWebView::New();
#endif
    if (webView) {
        webView->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
#ifdef __WIN32__
        // Ultra: bambulab.com/sign-in version-gates its login flavour on the BBL-Slicer UA
        // token. Build the UA with Bambu Studio's exact layout (browser prefix first, then
        // BBL-Slicer and BBL-Language) via the shared, wx-free builder - the fork's old
        // "BBL-Slicer/v.. Mozilla/5.0 .." order with no BBL-Language token is not what the
        // site parses, and its spoofed 02.03.00.01 is below the current gate.
        webView->SetUserAgent(wxString::FromUTF8(Slic3r::bbl_login_user_agent(
            Slic3r::LoginUAPlatform::Windows, Slic3r::GUI::wxGetApp().dark_mode(),
            Slic3r::GUI::wxGetApp().current_language_code().ToStdString(),
            brand_tag.ToStdString(), SLIC3R_VERSION)));
        webView->Create(parent, wxID_ANY, url2, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        // We register the wxfs:// protocol for testing purposes
        webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewArchiveHandler("bbl")));
        // And the memory: file system
        webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewFSHandler("memory")));
#else
        // Handlers must be registered before Create(). Linux (WebKit2): scheme is process-global, register once to avoid "Cannot register URI scheme ... more than once".
        // macOS (WKWebView): scheme is per-view, each WebView needs its own handlers.
#if defined(__linux__)
        static std::once_flag s_wxfs_memory_handlers_once;
        std::call_once(s_wxfs_memory_handlers_once, [webView]() {
            webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewArchiveHandler("wxfs")));
            webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewFSHandler("memory")));
        });
#else
        webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewArchiveHandler("wxfs")));
        webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewFSHandler("memory")));
#endif
#ifdef __WXMAC__
        // WKWebView starts loading during Create(), before the delayed handler callback runs.
        // Keep its initial document blank; the subclass flushes the pending URL after installation.
        webView->Create(parent, wxID_ANY, wxString("about:blank"), wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        static_cast<WebViewWebKit *>(webView)->AttachNavigationGate();
#else
        webView->Create(parent, wxID_ANY, url2, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
#endif
#if defined(__linux__)
        disconnect_script_messages_on_destroy(webView);
#endif
        // Same UA layout as the Windows branch above; macOS/Linux WebKit prefix.
        webView->SetUserAgent(wxString::FromUTF8(Slic3r::bbl_login_user_agent(
#if defined(__linux__)
            Slic3r::LoginUAPlatform::Linux,
#else
            Slic3r::LoginUAPlatform::MacOS,
#endif
            Slic3r::GUI::wxGetApp().dark_mode(),
            Slic3r::GUI::wxGetApp().current_language_code().ToStdString(),
            brand_tag.ToStdString(), SLIC3R_VERSION)));
#endif
#ifdef __WXMAC__
        WKWebView * wkWebView = (WKWebView *) webView->GetNativeBackend();
        Slic3r::GUI::WKWebView_setTransparentBackground(wkWebView);
#endif
        auto addScriptMessageHandler = [] (wxWebView *webView) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": begin to add script message handler for wx.";
            Slic3r::GUI::wxGetApp().set_adding_script_handler(true);
            if (!webView->AddScriptMessageHandler("wx"))
                wxLogError("Could not add script message handler");
            Slic3r::GUI::wxGetApp().set_adding_script_handler(false);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": finished add script message handler for wx.";
#ifdef __WXMAC__
            static_cast<WebViewWebKit *>(webView)->SetScriptMessageHandlerInstalled();
#endif
        };
        if (script_bridge) {
#ifndef __WIN32__
        webView->CallAfter([webView, addScriptMessageHandler] {
            // CallAfter can run after this webView has been destroyed (macOS 26.5+).
            // g_webviews holds only live views; skip dangling pointers.
            // See bambulab/BambuStudio #11004 and #10968.
            if (std::find(g_webviews.begin(), g_webviews.end(), webView) == g_webviews.end())
                return;
#endif
            if (Slic3r::GUI::wxGetApp().is_adding_script_handler()) {
                g_delay_webviews.push_back(webView);
            } else {
                addScriptMessageHandler(webView);
                while (!g_delay_webviews.empty()) {
                    auto views = std::move(g_delay_webviews);
                    for (auto wv : views) {
                        if (std::find(g_webviews.begin(), g_webviews.end(), wv) == g_webviews.end())
                            continue;
                        addScriptMessageHandler(wv);
                    }
                }
            }
#ifndef __WIN32__
        });
#endif
        } else {
#ifdef __WXMAC__
            // Loads wait for the handler install on macOS; with no handler to install, open
            // the gate now or the view would stay on about:blank.
            static_cast<WebViewWebKit *>(webView)->SetScriptMessageHandlerInstalled();
#endif
        } // script_bridge
        webView->EnableContextMenu(true);
        // Snapmaker's Flutter pages follow the slicer's dark mode (ApplyFlutterTheme). Bound on the
        // view itself, so it runs ahead of the hosts' handlers, which sit on their windows.
        webView->Bind(wxEVT_WEBVIEW_LOADED, [webView](wxWebViewEvent &evt) {
            evt.Skip();
            WebView::ApplyFlutterTheme(webView);
        });
    } else {
        BOOST_LOG_TRIVIAL(fatal) << __FUNCTION__ << ": failed. Use fake web view.";
        Slic3r::sentryReportLog(Slic3r::SENTRY_LOG_FATAL, "bury_point_create webview fail and use fakewebview", BP_WEB_VIEW);
        webView = new FakeWebView;
    }
    webView->SetRefData(new WebViewRef(webView));
    g_webviews.push_back(webView);
    webView->EnableAccessToDevTools();
    return webView;
}
#if wxUSE_WEBVIEW_EDGE
bool WebView::CheckWebViewRuntime()
{
    wxWebViewFactoryEdge factory;
    auto wxVersion = factory.GetVersionInfo();
    return wxVersion.GetMajor() != 0;
}

bool WebView::DownloadAndInstallWebViewRuntime()
{
    return DownloadAndInstallWV2RT() == 0;
}
#endif
void WebView::LoadUrl(wxWebView * webView, wxString const &url)
{
    auto url2  = url;
#ifdef __WIN32__
    url2.Replace("\\", "/");
#endif
    if (!url2.empty()) { url2 = wxURI(url2).BuildURI(); }
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << url2.ToUTF8();
    webView->LoadURL(url2);
}

bool WebView::RunScript(wxWebView *webView, wxString const &javascript)
{
    if (Slic3r::GUI::wxGetApp().app_config->get("internal_developer_mode") == "true"
            && javascript.find("studio_userlogin") == wxString::npos)
        wxLogMessage("Running JavaScript:\n%s\n", javascript);

    try {
#ifdef __WIN32__
        ICoreWebView2 *   webView2 = (ICoreWebView2 *) webView->GetNativeBackend();
        if (webView2 == nullptr)
            return false;
        return webView2->ExecuteScript(javascript, NULL) == 0;
#elif defined __WXMAC__
        WKWebView * wkWebView = (WKWebView *) webView->GetNativeBackend();
        Slic3r::GUI::WKWebView_evaluateJavaScript(wkWebView, javascript, nullptr);
        return true;
#else
        WebKitWebView *wkWebView = (WebKitWebView *) webView->GetNativeBackend();
        webkit_web_view_run_javascript(
            wkWebView, javascript.utf8_str(), NULL,
            [](GObject *wkWebView, GAsyncResult *res, void *) {
                GError * error = NULL;
                auto result = webkit_web_view_run_javascript_finish((WebKitWebView*)wkWebView, res, &error);
                if (!result)
                    g_error_free (error);
                else
                    webkit_javascript_result_unref (result);
        }, NULL);
        return true;
#endif
    } catch (std::exception &) {
        return false;
    }
}

void WebView::RecreateAll()
{
    auto dark = Slic3r::GUI::wxGetApp().dark_mode();
    for (auto webView : g_webviews) {
        // RecreateAll only refreshes the theme token; it cannot know each view's brand tag,
        // so keep the fork's default SM-Slicer here (the login dialog is modal and is not
        // alive across a theme switch).
        webView->SetUserAgent(wxString::FromUTF8(Slic3r::bbl_login_user_agent(
            Slic3r::current_login_ua_platform(), dark,
            Slic3r::GUI::wxGetApp().current_language_code().ToStdString(),
            "SM-Slicer", SLIC3R_VERSION)));
        // A Flutter page switches its theme live (ApplyFlutterTheme); a reload would lose its
        // state (a pre-print page's filament mapping, the Device tab's connection).
        if (IsFlutterPage(webView))
            WebView::ApplyFlutterTheme(webView);
        else if (std::find(g_no_theme_reload.begin(), g_no_theme_reload.end(), webView) == g_no_theme_reload.end())
            webView->Reload();
    }
}

bool WebView::IsFlutterPage(wxWebView *webView)
{
    if (webView == nullptr)
        return false;
    const wxString url = webView->GetCurrentURL();
    return url.Contains("/web/flutter_web/") && Slic3r::GUI::wxGetApp().is_own_page_url(url.ToStdString(wxConvUTF8));
}

void WebView::ApplyFlutterTheme(wxWebView *webView)
{
    if (!IsFlutterPage(webView))
        return;
    // index.html defines edgeSetDarkMode (scripts/patch_flutter_web_dark.py); the app follows it
    // with its own dark theme, live. The page also starts from its dark_mode= and dark_<role>= URL
    // parameters, which can be out of date after a theme change, hence on every load too.
    std::string colours;
    for (const auto &[role, hex] : FlutterDarkColours())
        colours += (colours.empty() ? "" : ",") + role + ":'" + hex + "'";
    RunScript(webView, wxString::Format("window.edgeSetDarkMode && window.edgeSetDarkMode(%s, {%s});",
                                        Slic3r::GUI::wxGetApp().dark_mode() ? "true" : "false", wxString::FromUTF8(colours)));
}

std::vector<std::pair<std::string, std::string>> WebView::FlutterDarkColours()
{
    // The pages look like the slicer's own (Bambu) Device page, StatusPanel.cpp, whose light colours
    // UpdateDarkUI turns dark through StateColor's table (or a theme pack's map):
    //   bg    - the page behind the panels, STATUS_PANEL_BG #EEEEEE -> #4C4C55 (theme: separator)
    //   card  - the panels, white #FFFFFF                           -> #2D2D31 (theme: window_bg)
    //   strip - the panel title bars, STATUS_TITLE_BG #F8F8F8       -> #36363C (theme: panel_bg)
    //   title - the panel titles, PAGE_TITLE_FONT_COL #6B6B6B        -> #818183 (theme: text_disabled)
    // and the slicer's accent for the Control panel's buttons:
    //   accent      - the Orca accent #009688                        -> #00675B (theme: accent)
    //   accent_text - text and icons on it #FEFEFE                   -> #FEFEFE (theme: accent_text)
    static const std::pair<const char *, const char *> roles[] = {
        {"bg", "#EEEEEE"}, {"card", "#FFFFFF"}, {"strip", "#F8F8F8"}, {"title", "#6B6B6B"},
        {"accent", "#009688"}, {"accent_text", "#FEFEFE"}};
    const auto &stock = StateColor::GetDarkMap();
    auto stock_dark = [&stock](const wxColour &light) {
        auto it = stock.find(light);
        return it != stock.end() ? it->second : light;
    };
    const bool dark = Slic3r::GUI::wxGetApp().dark_mode();
    std::vector<std::pair<std::string, wxColour>> picked;
    for (const auto &[role, light] : roles)
        picked.emplace_back(role, dark ? StateColor::darkModeColorFor(wxColour(light)) : stock_dark(wxColour(light)));
    // A dark look whose window background is not dark after all: the stock greys.
    const wxColour &bg = picked.front().second;
    if (!bg.IsOk() || 0.2126 * bg.Red() + 0.7152 * bg.Green() + 0.0722 * bg.Blue() > 110)
        for (size_t i = 0; i < picked.size(); ++i)
            picked[i].second = stock_dark(wxColour(roles[i].second));
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto &[role, colour] : picked)
        out.emplace_back(role, colour.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
    return out;
}

void WebView::SetReloadOnThemeChange(wxWebView *webView, bool reload)
{
    if (webView == nullptr)
        return;
    auto iter = std::find(g_no_theme_reload.begin(), g_no_theme_reload.end(), webView);
    if (reload && iter != g_no_theme_reload.end())
        g_no_theme_reload.erase(iter);
    else if (!reload && iter == g_no_theme_reload.end())
        g_no_theme_reload.push_back(webView);
}
