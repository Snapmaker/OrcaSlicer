#include "WebTextPanel.hpp"
#include "OrcaWebViewLoader.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/OrcaBridge.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"

#include <wx/sizer.h>
#include <wx/event.h>
#include <wx/filename.h>

namespace Slic3r {
namespace GUI {

// ============================================================
// 在这里注册所有 Bridge 命令
// ============================================================
static void registerBridgeCommands()
{
    static bool registered = false;
    if (registered) return;
    registered = true;

    // ---- ping ----
    OrcaBridge::bind("ping", [](const json& data, auto reply, auto fail) {
        json res;
        res["msg"] = "pong";
        res["version"] = SLIC3R_VERSION;
        reply(res);
    });

    // ---- getAppVersion ----
    OrcaBridge::bind("getAppVersion", [](const json& data, auto reply, auto fail) {
        json res;
        res["version"] = SLIC3R_VERSION;
        res["build"] = __DATE__ " " __TIME__;
        reply(res);
    });

    // ---- listImages: 列出 Flutter 构建目录下的图片 ----
    // Flutter web 构建输出为 assets/assets/images/（嵌套），非 assets/images/
    OrcaBridge::bind("listImages", [](const json& data, auto reply, auto fail) {
        json res;
        json images = json::array();
        images.push_back("orca://app/assets/assets/images/logo.png");
        images.push_back("orca://app/assets/assets/images/banner.png");
        images.push_back("orca://app/assets/assets/images/avatar.png");
        images.push_back("orca://app/assets/assets/images/empty.png");
        res["images"] = images;
        reply(res);
    });

    // ---- openFileDialog: 打开原生文件选择对话框 ----
    // 所有平台统一：复制到 temp/orca_user_assets/，返回 orca://app/user_assets/xxx
    OrcaBridge::bind("openFileDialog", [](const json& data, auto reply, auto fail) {
        wxGetApp().CallAfter([reply, fail]() {
            wxFileDialog dlg(nullptr,
                "Select Image", "", "",
                "Image files (*.png;*.jpg;*.jpeg;*.gif)|*.png;*.jpg;*.jpeg;*.gif|All files (*.*)|*.*",
                wxFD_OPEN | wxFD_FILE_MUST_EXIST);

            if (dlg.ShowModal() != wxID_OK) {
                fail("cancelled");
                return;
            }

            wxString srcPath = dlg.GetPath();
            wxString filename = dlg.GetFilename();
            wxString ext = wxFileName(filename).GetExt();
            if (ext.empty()) ext = "bin";

            wxString userAssetsDir;
            std::string orcaUrl;

            wxString tempBase = wxStandardPaths::Get().GetTempDir();
            userAssetsDir = tempBase + wxFileName::GetPathSeparator() + "orca_user_assets";
            if (!wxFileName::DirExists(userAssetsDir))
                wxFileName::Mkdir(userAssetsDir, 0755, wxPATH_MKDIR_FULL);
            orcaUrl = "orca://app/user_assets/";

            // 用 path+mtime+size 生成唯一 id，相同文件得到相同 id，实现去重
            wxStructStat st;
            if (wxStat(srcPath, &st) != 0) {
                fail("stat failed");
                return;
            }
            std::string fingerprint = srcPath.ToUTF8().data();
            fingerprint += "|" + std::to_string((long long)st.st_mtime);
            fingerprint += "|" + std::to_string((long long)st.st_size);
            size_t h = std::hash<std::string>{}(fingerprint);
            wxString destName = wxString::Format("%llx.%s", (unsigned long long)h, ext.ToUTF8().data());
            wxString destPath = userAssetsDir + wxFileName::GetPathSeparator() + destName;

            if (!wxFileExists(destPath)) {
                if (!wxCopyFile(srcPath, destPath, true)) {
                    fail("copy failed");
                    return;
                }
            }

            json res;
            res["path"] = srcPath.ToUTF8().data();
            res["name"] = filename.ToUTF8().data();
            res["orcaUrl"] = orcaUrl + destName.ToUTF8().data();
            reply(res);
        });
    });

    // ---- uiReady (通知, 无需回复) ----
    OrcaBridge::bind("uiReady", [](const json& data, auto reply, auto fail) {
        std::string page = data.value("page", "unknown");
        BOOST_LOG_TRIVIAL(info) << "OrcaBridge: UI ready, page=" << page;
        // 不调用 reply/fail，因为这是 emit (无 id)
    });

    BOOST_LOG_TRIVIAL(info) << "OrcaBridge: commands registered";
}

// ============================================================
// WebTextPanel
// ============================================================

WebTextPanel::WebTextPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
    registerBridgeCommands();

    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);

//    OrcaWebLoadConfig config = OrcaWebViewLoader::ResolveConfig("/Users/jgfan/code/lava_app/orca/build/flutter_web");
//
    OrcaWebLoadConfig config = OrcaWebViewLoader::ResolveConfig("/Users/jgfan/code/flutter_web_app/build/web");

    // OrcaWebLoadConfig config = OrcaWebViewLoader::ResolveConfig("D:\\snapmaker\\web");
    config.use_debug_server   = false;
    config.debug_server_url   = "http://localhost:7357";

    if (config.use_debug_server) {
        m_browser = WebView::CreateWebView(this, "");
        if (!m_browser) {
            wxLogError("WebTextPanel: Could not init m_browser");
            return;
        }
        topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
        SetSizer(topsizer);

        wxString full_url = config.debug_server_url + "/?path=" + config.route_path;
        if (!config.route_params.empty())
            full_url += "&" + config.route_params;
        m_browser->LoadURL(full_url);
    } else {
        m_browser = WebView::CreateWebViewWithLocalRoot(this, "", config.root_path, config.user_assets_dir);
        if (!m_browser) {
            wxLogError("WebTextPanel: Could not init m_browser");
            return;
        }
        topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
        SetSizer(topsizer);

#ifdef __WIN32__
        // Windows: orca:// 需在首次 EVT_SIZE 时 LoadURL，确保 scheme handler 已就绪
        wxString url_to_load = OrcaWebViewLoader::LoadLocalHtml(m_browser, config);
        if (!url_to_load.empty()) {
            m_browser->Bind(wxEVT_SIZE, [this, url_to_load](wxSizeEvent& evt) {
                if (m_browser && !m_orca_url_loaded) {
                    m_orca_url_loaded = true;
                    m_browser->LoadURL(url_to_load);
                }
                evt.Skip();
            });
        }
#else
        // macOS/Linux: 直接 SetPage(html, baseUrl)，orca:// handler 立即可用
        OrcaWebViewLoader::LoadLocalHtml(m_browser, config);
#endif
    }

    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &WebTextPanel::OnScriptMessage, this);
}

WebTextPanel::~WebTextPanel()
{
    SetEvtHandlerEnabled(false);
}

void WebTextPanel::load_url(const wxString& url)
{
    if (m_browser == nullptr)
        return;
    m_browser->LoadURL(url);
}

void WebTextPanel::reload()
{
    if (m_browser)
        m_browser->Reload();
}

void WebTextPanel::OnScriptMessage(wxWebViewEvent& evt)
{
    // 所有消息都交给 OrcaBridge 分发
    OrcaBridge::dispatch(evt.GetString().ToUTF8().data(), m_browser);
}

void WebTextPanel::RunScript(const wxString& javascript)
{
    if (m_browser)
        WebView::RunScript(m_browser, javascript);
}

} // namespace GUI
} // namespace Slic3r
