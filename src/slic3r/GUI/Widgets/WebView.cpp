#include "WebView.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/MacDarkMode.hpp"

#include <algorithm>
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
    // Keep the path string alive for the duration of the ShellExecuteExW call;
    // assigning .c_str() of a temporary directly would leave lpFile dangling.
    const std::wstring installer_path = target_file_path.generic_wstring();
    SHELLEXECUTEINFOW shExInfo = {0};
    shExInfo.cbSize = sizeof(shExInfo);
    shExInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    shExInfo.hwnd = 0;
    shExInfo.lpVerb = L"runas";
    shExInfo.lpFile = installer_path.c_str();
    shExInfo.lpParameters = L" /silent /install";
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
    bool SetUserAgent(const wxString &userAgent) override
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
    explicit WebViewWebKit(const wxString &initialUrl)
        : wxWebViewWebKit(MakeConfiguration())
        , m_pendingUrl(initialUrl)
    {
    }

    ~WebViewWebKit() override
    {
        RemoveScriptMessageHandler("wx");
    }

    // WKWebView starts loading inside Create(), but the "wx" handler is installed later from
    // a CallAfter; the navigation is held back until then so page JS finds messageHandlers.wx.
    void LoadURL(const wxString &url) override
    {
        if (!m_scriptMessageHandlerInstalled && url != wxString("about:blank")) {
            m_pendingUrl = url;
            m_hasPendingPage = false;
            return;
        }
        RequestLoad(url);
    }

    void SetScriptMessageHandlerInstalled()
    {
        m_scriptMessageHandlerInstalled = true;
        auto url = std::move(m_pendingUrl);
        m_pendingUrl.clear();
        if (m_hasPendingPage) {
            m_hasPendingPage = false;
            auto html    = std::move(m_pendingHtml);
            auto baseUrl = std::move(m_pendingBaseUrl);
            m_pendingHtml.clear();
            m_pendingBaseUrl.clear();
            wxWebViewWebKit::DoSetPage(html, baseUrl);
        } else if (!url.empty() && url != wxString("about:blank"))
            RequestLoad(url);
    }

    void AttachNavigationGate()
    {
        Bind(wxEVT_WEBVIEW_LOADED, &WebViewWebKit::OnNavigationSettled, this);
        Bind(wxEVT_WEBVIEW_ERROR, &WebViewWebKit::OnNavigationSettled, this);
    }

protected:
    // SetPage() is held back the same way and replaces the pending URL, so whichever of
    // LoadURL() / SetPage() came last is released once the handler is installed.
    void DoSetPage(const wxString &html, const wxString &baseUrl) override
    {
        if (!m_scriptMessageHandlerInstalled) {
            m_pendingUrl.clear();
            m_pendingHtml    = html;
            m_pendingBaseUrl = baseUrl;
            m_hasPendingPage = true;
            return;
        }
        // The page replaces whatever navigation is in flight or queued.
        m_inFlightUrl.clear();
        m_queuedUrl.clear();
        wxWebViewWebKit::DoSetPage(html, baseUrl);
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

    // A loadRequest during a provisional navigation cancels it (-999) and can blank the view.
    // Serialized: the in-flight canonical URL (query included: Flutter pages differ only in
    // ?path=) is dropped, Flutter over Flutter is queued, anything else loads at once.
    void RequestLoad(const wxString &url)
    {
        if (IsBlankUrl(url)) {
            wxWebViewWebKit::LoadURL(url);
            return;
        }

        const wxString target = CanonicalUrl(url);
        if (!m_inFlightUrl.empty()) {
            if (target == m_inFlightUrl) {
                // The most recent request is the one already loading, so an older queued
                // switch to another page is obsolete and must not replace it afterwards.
                if (!m_queuedUrl.empty())
                    BOOST_LOG_TRIVIAL(info) << "WebViewWebKit: unqueued " << m_queuedUrl.ToUTF8().data() << ", superseded";
                m_queuedUrl.clear();
                BOOST_LOG_TRIVIAL(info) << "WebViewWebKit: dropped " << target.ToUTF8().data() << ", same URL in flight";
                return;
            }
            // Flutter interrupted by another Flutter → -999 white screen. Queue that case only.
            // missing_connection.gif sitting in-flight while the view is hidden never settles,
            // so queuing path=2 behind it leaves Device on the GIF forever.
            if (IsFlutterUrl(m_inFlightUrl) && IsFlutterUrl(target)) {
                BOOST_LOG_TRIVIAL(info) << "WebViewWebKit: queued " << target.ToUTF8().data() << " behind " << m_inFlightUrl.ToUTF8().data();
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
        // Ignore the document that was just replaced (GIF -999 after jumping to path=2).
        if (!m_inFlightUrl.empty() && UrlPath(eventUrl) != UrlPath(m_inFlightUrl))
            return;
        m_inFlightUrl.clear();
        FlushQueued();
    }

    static wxWebViewConfiguration MakeConfiguration()
    {
        wxWebViewConfiguration config = wxWebView::NewConfiguration(wxWebViewBackendWebKit);
        // Must be applied to the native WKWebViewConfiguration before the
        // WKWebView is created (post-creation the configuration is a copy).
        Slic3r::GUI::WKWebViewConfiguration_keepActiveWhenHidden(config.GetNativeConfiguration());
        return config;
    }

    bool     m_scriptMessageHandlerInstalled = false;
    bool     m_hasPendingPage                = false;
    wxString m_pendingUrl;
    wxString m_pendingHtml;
    wxString m_pendingBaseUrl;
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
    }
    wxWebView *m_webView;
    // Guards against registering the "wx" handler twice (a duplicate throws on WKWebView).
    bool m_script_handler_added = false;
};

static WebViewRef *webview_ref(wxWebView *webView)
{
    return webView ? static_cast<WebViewRef *>(webView->GetRefData()) : nullptr;
}

wxWebView* WebView::CreateWebView(wxWindow * parent, wxString const & url)
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
    wxWebView* webView = new WebViewWebKit(url2);
#else
    auto webView = wxWebView::New();
#endif
    if (webView) {
        webView->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

        wxString language_code = Slic3r::GUI::wxGetApp().current_language_code().BeforeFirst('_');
        language_code          = language_code.ToStdString();
#ifdef __WIN32__
        // Snapmaker: keep the "SM-Slicer" product token (fork branding) while adopting Orca's
        // trailing token layout and the BBL-Language segment expected by the embedded web pages.
        webView->SetUserAgent(wxString::Format("SM-Slicer/v%s (%s) Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                               "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/107.0.0.0 Safari/537.36 Edg/107.0.1418.52 BBL-Language/%s",
                                               SLIC3R_VERSION, Slic3r::GUI::wxGetApp().dark_mode() ? "dark" : "light", language_code.mb_str()));
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
        const wxString user_agent = wxString::Format("SM-Slicer/v%s (%s) Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) BBL-Language/%s",
                                                     SLIC3R_VERSION, Slic3r::GUI::wxGetApp().dark_mode() ? "dark" : "light", language_code.mb_str());
#ifdef __WXMAC__
        // WKWebView starts loading during Create(), before the delayed handler callback
        // runs. The initial document is kept blank; the subclass flushes the pending URL
        // once the "wx" handler is installed.
        webView->Create(parent, wxID_ANY, wxString("about:blank"), wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        static_cast<WebViewWebKit *>(webView)->AttachNavigationGate();
        // SetUserAgent() only after Create(): before it the native view pointer is
        // uninitialised. Only about:blank is loading yet, so the real navigation has the agent.
        webView->SetUserAgent(user_agent);
#else
        // Set the user agent BEFORE Create(): wx buffers it and applies it ahead of the
        // first navigation, so the live view is not mutated mid-provisional-load.
        // Mirrors the Windows branch order.
        webView->SetUserAgent(user_agent);
        webView->Create(parent, wxID_ANY, url2, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
#endif
#endif
#ifdef __WXMAC__
        WKWebView * wkWebView = (WKWebView *) webView->GetNativeBackend();
        Slic3r::GUI::WKWebView_setTransparentBackground(wkWebView);
        Slic3r::GUI::WKWebView_keepActiveWhenHidden(wkWebView);
#endif
        auto addScriptMessageHandler = [] (wxWebView *webView) {
#ifdef __WXMAC__
            // Releases the deferred initial URL on every exit path, after
            // set_adding_script_handler(false). AddScriptMessageHandler() pumps the event
            // loop and can destroy the view, so the guard checks g_webviews first.
            struct ReleasePendingUrl {
                wxWebView *view;
                ~ReleasePendingUrl()
                {
                    if (std::find(g_webviews.begin(), g_webviews.end(), view) != g_webviews.end())
                        static_cast<WebViewWebKit *>(view)->SetScriptMessageHandlerInstalled();
                }
            } release{webView};
#endif
            // Skip if SendAPIKey() already registered "wx"; a duplicate add throws an
            // uncatchable NSException on WKWebView, killing the app at startup.
            WebViewRef *ref = webview_ref(webView);
            if (ref && ref->m_script_handler_added)
                return;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": begin to add script message handler for wx.";
            Slic3r::GUI::wxGetApp().set_adding_script_handler(true);
            if (!webView->AddScriptMessageHandler("wx"))
                wxLogError("Could not add script message handler");
            // The call above pumps the event loop. A view destroyed meanwhile has taken its
            // WebViewRef along, so the ref is only written while the view is still registered.
            else if (ref && std::find(g_webviews.begin(), g_webviews.end(), webView) != g_webviews.end())
                ref->m_script_handler_added = true;
            Slic3r::GUI::wxGetApp().set_adding_script_handler(false);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": finished add script message handler for wx.";
        };
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
        webView->EnableContextMenu(true);
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

void WebView::MarkScriptMessageHandlerAdded(wxWebView * webView)
{
    if (WebViewRef *ref = webview_ref(webView))
        ref->m_script_handler_added = true;
}
#if wxUSE_WEBVIEW_EDGE
bool WebView::CheckWebViewRuntime()
{
    wxWebViewFactoryEdge factory;
    auto wxVersion = factory.GetVersionInfo(wxVersionContext::RunTime);
    bool present = wxVersion.GetMajor() != 0;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": WebView2 runtime "
                            << (present ? "found, version " : "not found (")
                            << wxVersion.ToString().ToUTF8().data() << (present ? "" : ")");
    return present;
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
    wxString language_code = Slic3r::GUI::wxGetApp().current_language_code().BeforeFirst('_');
    language_code          = language_code.ToStdString();
    for (auto webView : g_webviews) {
        webView->SetUserAgent(wxString::Format("SM-Slicer/v%s (%s) Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) BBL-Language/%s",
                                               SLIC3R_VERSION, dark ? "dark" : "light", language_code.mb_str()));
        // A host-themed WebViewHostDialog re-themes in place (no reload). If it handles
        // the event, skip the reload; legacy pages fall through and reload as before
        // (their own dark.css swap re-themes them on reload).
        wxCommandEvent evt(EVT_WEBVIEW_RECREATED);
        evt.SetEventObject(webView);
        if (!webView->GetEventHandler()->ProcessEvent(evt))
            webView->Reload();
    }
}
