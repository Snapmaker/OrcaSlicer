#include "WebTextPanel.hpp"

#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/OrcaBridge.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"

#include <wx/sizer.h>
#include <wx/event.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/dir.h>
#include <wx/stdpaths.h>
#include <wx/filefn.h>

#include <functional>
#include <string>

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
    // 将选中文件复制到 temp/orca_user_assets/，返回 orcaUrl 供 <img src="orca://app/user_assets/xxx"> 加载
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

            // 复制到 temp/orca_user_assets/，按源路径去重：同一文件多次选择只保留一份
            wxString tempBase = wxStandardPaths::Get().GetTempDir();
            wxString userAssetsDir = tempBase + wxFileName::GetPathSeparator() + "orca_user_assets";
            if (!wxFileName::DirExists(userAssetsDir))
                wxFileName::Mkdir(userAssetsDir, 0755, wxPATH_MKDIR_FULL);

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
            res["orcaUrl"] = std::string("orca://app/user_assets/") + destName.ToUTF8().data();
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

    // ============================================================
    // Debug/Release 双模式配置
    // ============================================================
    const bool USE_DEBUG_SERVER = false;

    const wxString DEBUG_SERVER_URL = "http://localhost:7357";
    const wxString RELEASE_LOCAL_ROOT = "/Users/jgfan/code/flutter_web_app/build/web";

    const wxString ROUTE_PATH = "/bridge";
    const wxString ROUTE_PARAMS = "locale=zh-cn&dark_mode=1";
    // ============================================================

    if (USE_DEBUG_SERVER) {
        BOOST_LOG_TRIVIAL(info) << "WebTextPanel: Debug mode - connecting to " << DEBUG_SERVER_URL.ToUTF8();

        m_browser = WebView::CreateWebView(this, "");
        if (m_browser == nullptr) {
            wxLogError("WebTextPanel: Could not init m_browser");
            return;
        }

        topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
        SetSizer(topsizer);

        wxString fullUrl = DEBUG_SERVER_URL + "/?path=" + ROUTE_PATH;
        if (!ROUTE_PARAMS.empty()) {
            fullUrl += "&" + ROUTE_PARAMS;
        }
        m_browser->LoadURL(fullUrl);

    } else {
        BOOST_LOG_TRIVIAL(info) << "WebTextPanel: Release mode - loading from " << RELEASE_LOCAL_ROOT.ToUTF8();

        // 创建 user_assets 目录，用于 openFileDialog 选中的图片复制到 temp 后通过 orca:// 加载
        wxString userAssetsDir = wxStandardPaths::Get().GetTempDir() + wxFileName::GetPathSeparator() + "orca_user_assets";
        if (!wxFileName::DirExists(userAssetsDir))
            wxFileName::Mkdir(userAssetsDir, 0755, wxPATH_MKDIR_FULL);

        m_browser = WebView::CreateWebViewWithLocalRoot(this, "", RELEASE_LOCAL_ROOT, userAssetsDir);
        if (m_browser == nullptr) {
            wxLogError("WebTextPanel: Could not init m_browser");
            return;
        }

        topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
        SetSizer(topsizer);

        wxString htmlPath = RELEASE_LOCAL_ROOT + wxFileName::GetPathSeparator() + "index.html";
        wxString html;
        wxFile f(htmlPath, wxFile::read);
        if (f.IsOpened())
            f.ReadAll(&html);

        if (html.empty())
            html = "<html><body><p>index.html not found</p></body></html>";

        // baseUrl 必须为 orca://app/，不能包含 /bridge
        // Flutter 构建的 main.dart.js、flutter.js 等在 build/web/ 根目录，
        // 若 baseUrl=orca://app/bridge/ 会解析为 orca://app/bridge/main.dart.js，文件不存在
        wxString baseUrl = "orca://app/?";
        if (!ROUTE_PARAMS.empty()) {
            baseUrl += ROUTE_PARAMS + "&";
        }
        baseUrl += "path=" + ROUTE_PATH;

        BOOST_LOG_TRIVIAL(info) << "WebTextPanel: baseUrl=" << baseUrl.ToUTF8();
        m_browser->SetPage(html, baseUrl);
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
