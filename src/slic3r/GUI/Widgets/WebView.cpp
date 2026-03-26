#include "WebView.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/OrcaLocalHandler.hpp"
#include "slic3r/Utils/MacDarkMode.hpp"
#include "libslic3r/Utils.hpp"

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
#include <Shlwapi.h>
#include <wrl/event.h>
#include <wrl/implements.h>
#include "WebView2EnvironmentOptions.h"
#pragma comment(lib, "Shlwapi.lib")
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

// ============================================================
// Hook wxCreateCoreWebView2EnvironmentWithOptions 以注册 orca:// 自定义 scheme
// 不需要修改 wxWidgets 源码，在 Environment 创建时自动注入 scheme 注册
// ============================================================

// webview_edge.cpp 中的全局函数指针（链接时可见）
typedef HRESULT (__stdcall *CreateCoreWebView2EnvironmentWithOptions_t)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* environment_created_handler);
extern CreateCoreWebView2EnvironmentWithOptions_t wxCreateCoreWebView2EnvironmentWithOptions;

static CreateCoreWebView2EnvironmentWithOptions_t g_originalCreateEnv = nullptr;

static HRESULT __stdcall CreateEnvWithOrcaScheme(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler)
{
    // wxWidgets 的 CoreWebView2EnvironmentOptions 对象是用旧版 SDK (1.0.1418.22) 编译的，
    // 不实现 ICoreWebView2EnvironmentOptions4，因此 QueryInterface 会失败。
    // 解决方案：用项目自己的新版头文件创建新的 options 对象，它实现了 Options4~Options8。
    auto newOptions = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();

    // 从旧 options 对象复制 AdditionalBrowserArguments（wxWidgets 用它设置 user-agent），并合并本机所需参数
    wxString browser_args;
    if (environmentOptions) {
        LPWSTR args = nullptr;
        if (SUCCEEDED(environmentOptions->get_AdditionalBrowserArguments(&args)) && args) {
            browser_args = args;
            CoTaskMemFree(args);
        }
    }
    // WebView2 中文档源为 orca://app 时，对 https 的 XMLHttpRequest/fetch 会执行严格 CORS；api.snapmaker.cn 等若未返回
    // Access-Control-Allow-Origin（无法列举自定义 scheme 源）则请求被拦截，首页数据失败白屏。macOS WKWebView 对自定义 scheme 往往更宽松。
    // 以下 Chromium 开关用于嵌入式仅加载本地 Flutter + 固定 API 的场景（勿用于加载任意不可信网页）。
    if (!browser_args.empty())
        browser_args += L' ';
    browser_args += L"--disable-web-security --disable-site-isolation-trials";
    newOptions->put_AdditionalBrowserArguments(browser_args.wc_str());

    // 在新 options 上注册 orca:// 自定义 scheme
    ICoreWebView2EnvironmentOptions4* options4 = nullptr;
    if (SUCCEEDED(newOptions.Get()->QueryInterface(IID_PPV_ARGS(&options4)))) {
        auto scheme = Microsoft::WRL::Make<CoreWebView2CustomSchemeRegistration>(L"orca");
        scheme->put_TreatAsSecure(TRUE);
        scheme->put_HasAuthorityComponent(TRUE);
        LPCWSTR origins[] = { L"orca://*" };
        scheme->SetAllowedOrigins(1, origins);
        ICoreWebView2CustomSchemeRegistration* schemes[] = { scheme.Get() };
        options4->SetCustomSchemeRegistrations(1, schemes);
        options4->Release();
        BOOST_LOG_TRIVIAL(info) << "HookWebView2: registered orca:// custom scheme on new options object";
    } else {
        BOOST_LOG_TRIVIAL(error) << "HookWebView2: QueryInterface for ICoreWebView2EnvironmentOptions4 failed even on new options";
    }

    // 传递新的 options 对象（替代 wxWidgets 的旧对象）
    return g_originalCreateEnv(browserExecutableFolder, userDataFolder, newOptions.Get(), handler);
}

static void HookWebView2EnvironmentCreation()
{
    static bool hooked = false;
    if (hooked) return;
    hooked = true;

    // 触发 WebView2Loader.dll 加载（如果尚未加载）
    // MSWSetBrowserExecutableDir 应已由调用方设置
    wxWebViewFactoryEdge factory;
    factory.IsAvailable();

    if (wxCreateCoreWebView2EnvironmentWithOptions) {
        g_originalCreateEnv = wxCreateCoreWebView2EnvironmentWithOptions;
        wxCreateCoreWebView2EnvironmentWithOptions = CreateEnvWithOrcaScheme;
        BOOST_LOG_TRIVIAL(info) << "HookWebView2: hooked environment creation for orca:// scheme";
    } else {
        BOOST_LOG_TRIVIAL(warning) << "HookWebView2: wxCreateCoreWebView2EnvironmentWithOptions not yet loaded";
    }
}

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
    /// 注册 orca:// 自定义 scheme 拦截器，通过 WebResourceRequested 事件从本地目录读取文件
    /// 与 macOS 的 OrcaLocalHandler 行为一致
    bool SetupOrcaScheme(const wxString& localFolderPath, const wxString& userAssetsRoot = wxEmptyString)
    {
        m_orcaRootPath = localFolderPath;
        if (!m_orcaRootPath.empty() && m_orcaRootPath.Last() != wxFileName::GetPathSeparator())
            m_orcaRootPath += wxFileName::GetPathSeparator();
        m_orcaUserAssetsRoot = userAssetsRoot;
        if (!m_orcaUserAssetsRoot.empty() && m_orcaUserAssetsRoot.Last() != wxFileName::GetPathSeparator())
            m_orcaUserAssetsRoot += wxFileName::GetPathSeparator();

        ICoreWebView2* webView2 = (ICoreWebView2*)GetNativeBackend();
        if (webView2) {
            return InstallOrcaSchemeHandler(webView2);
        }
        m_pendingOrcaScheme = true;
        return true;
    }

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
        if (m_pendingOrcaScheme) {
            auto thiz = const_cast<WebViewEdge*>(this);
            ICoreWebView2* webView2 = (ICoreWebView2*)GetNativeBackend();
            if (webView2) {
                thiz->m_pendingOrcaScheme = false;
                thiz->InstallOrcaSchemeHandler(webView2);
            }
        }
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
    bool InstallOrcaSchemeHandler(ICoreWebView2* webView2)
    {
        // 添加 orca:// 请求过滤器
        HRESULT hr = webView2->AddWebResourceRequestedFilter(L"orca://*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        if (FAILED(hr)) {
            BOOST_LOG_TRIVIAL(error) << "WebViewEdge: AddWebResourceRequestedFilter failed: 0x" << std::hex << hr;
            return false;
        }

        // 获取 Environment 用于创建响应
        ICoreWebView2_2* webView2_2 = nullptr;
        hr = webView2->QueryInterface(IID_PPV_ARGS(&webView2_2));
        if (FAILED(hr) || !webView2_2) {
            BOOST_LOG_TRIVIAL(error) << "WebViewEdge: QueryInterface ICoreWebView2_2 failed";
            return false;
        }

        ICoreWebView2Environment* env = nullptr;
        hr = webView2_2->get_Environment(&env);
        webView2_2->Release();
        if (FAILED(hr) || !env) {
            BOOST_LOG_TRIVIAL(error) << "WebViewEdge: get_Environment failed";
            return false;
        }

        wxString rootPath = m_orcaRootPath;
        wxString userAssetsRoot = m_orcaUserAssetsRoot;

        EventRegistrationToken token;
        hr = webView2->add_WebResourceRequested(
            Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [env, rootPath, userAssetsRoot](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                    return HandleOrcaRequest(env, args, rootPath, userAssetsRoot);
                }
            ).Get(),
            &token
        );

        if (SUCCEEDED(hr)) {
            BOOST_LOG_TRIVIAL(info) << "WebViewEdge: orca:// scheme handler installed, root=" << m_orcaRootPath.ToUTF8();
        } else {
            BOOST_LOG_TRIVIAL(error) << "WebViewEdge: add_WebResourceRequested failed: 0x" << std::hex << hr;
            env->Release();
        }
        // env 的生命周期由 lambda 捕获管理，不在这里 Release
        return SUCCEEDED(hr);
    }

    static HRESULT HandleOrcaRequest(ICoreWebView2Environment* env,
                                     ICoreWebView2WebResourceRequestedEventArgs* args,
                                     const wxString& rootPath,
                                     const wxString& userAssetsRoot)
    {
        ICoreWebView2WebResourceRequest* request = nullptr;
        args->get_Request(&request);
        if (!request)
            return S_OK;

        LPWSTR uri = nullptr;
        request->get_Uri(&uri);
        request->Release();
        if (!uri)
            return S_OK;

        wxString uriStr(uri);
        CoTaskMemFree(uri);

        // 解析 orca://app/path
        wxURI u(uriStr);
        wxString path = u.GetPath();

        // 去掉开头的斜杠
        while (!path.empty() && (path[0] == '/' || path[0] == '\\'))
            path = path.Mid(1);

        // 安全检查：防止路径遍历
        if (path.Contains(".."))
            return CreateErrorResponse(env, args, 403, L"Forbidden");

        wxString fullPath;
        wxString pathForMime = path;

        // orca://app/user_assets/xxx -> userAssetsRoot/xxx
        if (!userAssetsRoot.empty() && path.Lower().StartsWith("user_assets")) {
            wxString sub = path.Mid(11);  // skip "user_assets"
            while (!sub.empty() && (sub[0] == '/' || sub[0] == '\\'))
                sub = sub.Mid(1);
            if (sub.Contains(".."))
                return CreateErrorResponse(env, args, 403, L"Forbidden");
            sub.Replace("/", wxString(wxFileName::GetPathSeparator()));
            fullPath = userAssetsRoot + sub;
        } else {
            path.Replace("/", wxString(wxFileName::GetPathSeparator()));
            fullPath = rootPath + path;
        }

        BOOST_LOG_TRIVIAL(trace) << "WebViewEdge::HandleOrcaRequest uri=" << uriStr.ToUTF8() << " fullPath=" << fullPath.ToUTF8();

        // 打开文件流
        IStream* stream = nullptr;
        HRESULT hr = SHCreateStreamOnFileEx(fullPath.wc_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &stream);
        if (FAILED(hr) || !stream) {
            // SPA fallback：如果路径没有文件扩展名（说明是客户端路由如 /bridge），
            // 回退到同级目录或上级目录的 index.html，让 Flutter 路由器处理
            if (!pathForMime.Contains(".")) {
                wxString fallbackPath;
                // 尝试 dirname(path)/index.html，例如 web/flutter_web/bridge → web/flutter_web/index.html
                wxFileName fn(fullPath);
                wxString dir = fn.GetPath();
                fallbackPath = dir + wxFileName::GetPathSeparator() + "index.html";

                IStream* fallbackStream = nullptr;
                hr = SHCreateStreamOnFileEx(fallbackPath.wc_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &fallbackStream);
                if (FAILED(hr) || !fallbackStream) {
                    // 再尝试 rootPath/index.html
                    fallbackPath = rootPath + "index.html";
                    hr = SHCreateStreamOnFileEx(fallbackPath.wc_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &fallbackStream);
                }
                if (SUCCEEDED(hr) && fallbackStream) {
                    BOOST_LOG_TRIVIAL(info) << "WebViewEdge: SPA fallback -> " << fallbackPath.ToUTF8();
                    pathForMime = "index.html";
                    stream = fallbackStream;
                    // fall through to serve the file below
                } else {
                    BOOST_LOG_TRIVIAL(trace) << "WebViewEdge: file not found (no SPA fallback): " << fullPath.ToUTF8();
                    return CreateErrorResponse(env, args, 404, L"Not Found");
                }
            } else {
                BOOST_LOG_TRIVIAL(trace) << "WebViewEdge: file not found: " << fullPath.ToUTF8();
                return CreateErrorResponse(env, args, 404, L"Not Found");
            }
        }

        // 确定 MIME 类型
        wxString mime = GetMimeType(pathForMime);
        wxString headers = wxString::Format("Content-Type: %s\r\nAccess-Control-Allow-Origin: *", mime);

        // 创建响应
        ICoreWebView2WebResourceResponse* response = nullptr;
        hr = env->CreateWebResourceResponse(stream, 200, L"OK", headers.wc_str(), &response);
        stream->Release();
        if (SUCCEEDED(hr) && response) {
            args->put_Response(response);
            response->Release();
        }
        return S_OK;
    }

    static HRESULT CreateErrorResponse(ICoreWebView2Environment* env,
                                       ICoreWebView2WebResourceRequestedEventArgs* args,
                                       int statusCode, LPCWSTR reason)
    {
        ICoreWebView2WebResourceResponse* response = nullptr;
        env->CreateWebResourceResponse(nullptr, statusCode, reason, L"", &response);
        if (response) {
            args->put_Response(response);
            response->Release();
        }
        return S_OK;
    }

    static wxString GetMimeType(const wxString& path)
    {
        wxString lower = path.Lower();
        if (lower.EndsWith(".html") || lower.EndsWith(".htm")) return "text/html; charset=utf-8";
        if (lower.EndsWith(".css")) return "text/css";
        if (lower.EndsWith(".js")) return "application/javascript";
        if (lower.EndsWith(".json")) return "application/json";
        if (lower.EndsWith(".wasm")) return "application/wasm";
        if (lower.EndsWith(".png")) return "image/png";
        if (lower.EndsWith(".jpg") || lower.EndsWith(".jpeg")) return "image/jpeg";
        if (lower.EndsWith(".svg")) return "image/svg+xml";
        if (lower.EndsWith(".gif")) return "image/gif";
        if (lower.EndsWith(".ttf")) return "font/ttf";
        if (lower.EndsWith(".woff")) return "font/woff";
        if (lower.EndsWith(".woff2")) return "font/woff2";
        if (lower.EndsWith(".ico")) return "image/x-icon";
        return "application/octet-stream";
    }

    wxString m_orcaRootPath;
    wxString m_orcaUserAssetsRoot;
    bool m_pendingOrcaScheme = false;
    wxString pendingUserAgent;
    COREWEBVIEW2_PREFERRED_COLOR_SCHEME pendingColorScheme = COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO;
};

#elif defined __WXOSX__

class WebViewWebKit : public wxWebViewWebKit
{
    ~WebViewWebKit() override
    {
        RemoveScriptMessageHandler("wx");
    }
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
    }
    wxWebView *m_webView;
};

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
    // Hook environment creation to register orca:// custom scheme
    HookWebView2EnvironmentCreation();
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
    wxWebView *webView = new WebViewWebKit;
#else
    auto webView = wxWebView::New();
#endif
    if (webView) {
        webView->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
#ifdef __WIN32__
        webView->SetUserAgent(wxString::Format("SM-Slicer/v%s (%s) Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
            "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/107.0.0.0 Safari/537.36 Edg/107.0.1418.52", SLIC3R_VERSION, 
            Slic3r::GUI::wxGetApp().dark_mode() ? "dark" : "light"));
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
        webView->Create(parent, wxID_ANY, url2, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        webView->SetUserAgent(wxString::Format("SM-Slicer/v%s (%s) Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko)", SLIC3R_VERSION,
                                               Slic3r::GUI::wxGetApp().dark_mode() ? "dark" : "light"));
#endif
#ifdef __WXMAC__
        WKWebView * wkWebView = (WKWebView *) webView->GetNativeBackend();
        Slic3r::GUI::WKWebView_setTransparentBackground(wkWebView);
#endif
        auto addScriptMessageHandler = [] (wxWebView *webView) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": begin to add script message handler for wx.";
            Slic3r::GUI::wxGetApp( ).set_adding_script_handler(true);
            if (!webView->AddScriptMessageHandler("wx"))
                wxLogError("Could not add script message handler");
            Slic3r::GUI::wxGetApp().set_adding_script_handler(false);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": finished add script message handler for wx.";
        };
#ifndef __WIN32__
        webView->CallAfter([webView, addScriptMessageHandler] {
#endif
            if (Slic3r::GUI::wxGetApp().is_adding_script_handler()) {
                g_delay_webviews.push_back(webView);
            } else {
                addScriptMessageHandler(webView);
                while (!g_delay_webviews.empty()) {
                    auto views = std::move(g_delay_webviews);
                    for (auto wv : views)
                        addScriptMessageHandler(wv);
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

wxWebView* WebView::CreateWebViewWithLocalRoot(wxWindow *parent, wxString const &url, wxString const &localRootPath,
                                               wxString const &userAssetsRoot)
{
    auto url2 = url;
#ifdef __WIN32__
    url2.Replace("\\", "/");
#endif
    if (!url2.empty()) { url2 = wxURI(url2).BuildURI(); }
    BOOST_LOG_TRIVIAL(trace) << "CreateWebViewWithLocalRoot: " << url2.ToUTF8() << " root=" << localRootPath.ToUTF8();

#ifdef __WIN32__
    // Hook environment creation to register orca:// custom scheme (no-op if already hooked)
    HookWebView2EnvironmentCreation();
    wxWebView* webView = new WebViewEdge;
#elif defined(__WXOSX__)
    wxWebView* webView = new WebViewWebKit;
#else
    auto webView = wxWebView::New();
#endif
    if (!webView)
        return nullptr;

    webView->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

#ifdef __WIN32__
    webView->SetUserAgent(wxString::Format("SM-Slicer/v%s (%s) Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/107.0.0.0 Safari/537.36 Edg/107.0.1418.52", SLIC3R_VERSION,
        Slic3r::GUI::wxGetApp().dark_mode() ? "dark" : "light"));
    webView->Create(parent, wxID_ANY, url2, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    // 注册 orca:// 自定义 scheme 处理器，拦截请求并从 localRootPath 读取文件
    WebView::SetupOrcaScheme(webView, localRootPath, userAssetsRoot);
    webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewArchiveHandler("bbl")));
    webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewFSHandler("memory")));
    webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new Slic3r::GUI::OrcaLocalHandler("orca", localRootPath, userAssetsRoot)));
#else
    webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewArchiveHandler("wxfs")));
    webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewFSHandler("memory")));
    webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new Slic3r::GUI::OrcaLocalHandler("orca", localRootPath, userAssetsRoot)));
    webView->Create(parent, wxID_ANY, url2, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    webView->SetUserAgent(wxString::Format("SM-Slicer/v%s (%s) Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko)", SLIC3R_VERSION,
        Slic3r::GUI::wxGetApp().dark_mode() ? "dark" : "light"));
#endif

#ifdef __WXMAC__
    WKWebView* wkWebView = (WKWebView*)webView->GetNativeBackend();
    if (wkWebView)
        Slic3r::GUI::WKWebView_setTransparentBackground(wkWebView);
#endif

    auto addScriptMessageHandler = [](wxWebView* wv) {
        Slic3r::GUI::wxGetApp().set_adding_script_handler(true);
        if (!wv->AddScriptMessageHandler("wx"))
            wxLogError("Could not add script message handler");
        Slic3r::GUI::wxGetApp().set_adding_script_handler(false);
    };
#ifndef __WIN32__
    webView->CallAfter([webView, addScriptMessageHandler] {
#endif
        if (Slic3r::GUI::wxGetApp().is_adding_script_handler())
            g_delay_webviews.push_back(webView);
        else {
            addScriptMessageHandler(webView);
            while (!g_delay_webviews.empty()) {
                auto views = std::move(g_delay_webviews);
                for (auto wv : views)
                    addScriptMessageHandler(wv);
            }
        }
#ifndef __WIN32__
    });
#endif
    webView->EnableContextMenu(true);
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

bool WebView::SetupOrcaScheme(wxWebView* webView, wxString const& localFolderPath, wxString const& userAssetsRoot)
{
    auto* edge = dynamic_cast<WebViewEdge*>(webView);
    if (!edge) return false;
    return edge->SetupOrcaScheme(localFolderPath, userAssetsRoot);
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
        webView->SetUserAgent(wxString::Format("SM-Slicer/v%s (%s) Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko)", SLIC3R_VERSION,
                                               dark ? "dark" : "light"));
        webView->Reload();
    }
}
