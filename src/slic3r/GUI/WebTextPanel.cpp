#include "WebTextPanel.hpp"

#include "libslic3r/Utils.hpp"
#include "nlohmann/json.hpp"
#include "common_func/common_func.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"

#include <wx/sizer.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/event.h>

namespace Slic3r {
namespace GUI {

namespace {

wxString setup_temp_demo()
{
    wxString tempBase = wxStandardPaths::Get().GetTempDir();
    wxString demoDir = tempBase + wxFileName::GetPathSeparator() + "orca_webtext_temp";
    if (!wxFileName::DirExists(demoDir))
        wxFileName::Mkdir(demoDir, 0755, wxPATH_MKDIR_FULL);

    std::string res = Slic3r::resources_dir() + "/web/login/";
    wxString srcImg = from_u8(res + "disconnect.png");
    if (wxFileExists(srcImg)) {
        wxString sep = wxFileName::GetPathSeparator();
        wxCopyFile(srcImg, demoDir + sep + "img1.png", true);
        wxCopyFile(srcImg, demoDir + sep + "img2.png", true);
        wxCopyFile(srcImg, demoDir + sep + "img3.png", true);
    }

    wxFileName fn = wxFileName::DirName(demoDir);
    fn.MakeAbsolute();
    return fn.GetFullPath();
}

} // namespace

WebTextPanel::WebTextPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);

    wxString rootPath = setup_temp_demo();

    m_browser = WebView::CreateWebViewWithLocalRoot(this, "", rootPath);
    if (m_browser == nullptr) {
        wxLogError("WebTextPanel: Could not init m_browser");
        return;
    }

    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    SetSizer(topsizer);

    // Load HTML from resources/web/webtext/demo.html
    // Use SetPage with base URL orca://app/ so HTML renders correctly.
    wxString htmlPath = from_u8(Slic3r::resources_dir() + "/web/webtext/demo.html");
    wxString html;
    if (wxFileExists(htmlPath)) {
        wxFile f(htmlPath, wxFile::read);
        if (f.IsOpened()) {
            f.ReadAll(&html);
        }
    }
    if (html.empty())
        html = "<html><body><p>demo.html not found</p></body></html>";
    m_browser->SetPage(html, "orca://app/");

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
