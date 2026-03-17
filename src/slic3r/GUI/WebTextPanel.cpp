#include "WebTextPanel.hpp"

#include "libslic3r/Utils.hpp"
#include "nlohmann/json.hpp"
#include "common_func/common_func.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"

#include <wx/sizer.h>
#include <wx/event.h>
#include <wx/file.h>
#include <wx/filename.h>

namespace Slic3r {
namespace GUI {

WebTextPanel::WebTextPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);

    // Point rootPath to the Flutter web build output directory.
    // The orca:// scheme handler maps orca://app/<path> → rootPath/<path>.
    // wxString rootPath = "/Users/jgfan/code/flutter_web_app/build/web";


    wxString rootPath = "/Users/jgfan/code/lava_app/orca/build/flutter_web";
//    /Users/jgfan/code/flutter_web_app/build/web
    // Use SetPage instead of LoadURL: wxWebView has a bug where HTML loaded via
    // custom scheme is displayed as plain text. SetPage injects HTML correctly.
    m_browser = WebView::CreateWebViewWithLocalRoot(this, "", rootPath);
    if (m_browser == nullptr) {
        wxLogError("WebTextPanel: Could not init m_browser");
        return;
    }

    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    SetSizer(topsizer);

    // Read index.html and inject with base URL orca://app/ so relative paths resolve correctly.
    wxString htmlPath = rootPath + wxFileName::GetPathSeparator() + "index.html";
    wxString html;
    wxFile f(htmlPath, wxFile::read);
    if (f.IsOpened())
        f.ReadAll(&html);

    if (html.empty())
        html = "<html><body><p>index.html not found</p></body></html>";
    m_browser->SetPage(html, "orca://app?path=1&locale=zh-cn");

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
    std::string msg = evt.GetString().ToUTF8().data();
    try {
        auto j = nlohmann::json::parse(msg);
        std::string type = j.value("type", "");
        if (type == "ping" || type == "get_app_version") {
            nlohmann::json reply;
            reply["type"] = "pong";
            reply["seq"] = j.value("seq", 0);
            reply["payload"]["msg"] = "Hello from C++";
            reply["payload"]["version"] = SLIC3R_VERSION;
            std::string js = "window.postMessage(JSON.stringify(" + reply.dump() + "), '*');";
            RunScript(from_u8(js));
        }
    } catch (...) {
        // ignore parse error
    }
}

void WebTextPanel::RunScript(const wxString& javascript)
{
    if (m_browser)
        WebView::RunScript(m_browser, javascript);
}

} // namespace GUI
} // namespace Slic3r
