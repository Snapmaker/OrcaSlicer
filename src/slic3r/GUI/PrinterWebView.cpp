#include "PrinterWebView.hpp"

#include "I18N.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/OrcaWebViewLoader.hpp"
#include "common_func/common_func.hpp"

#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>
#include <wx/event.h>

#include <slic3r/GUI/Widgets/WebView.hpp>
#include <wx/webview.h>
#include "slic3r/GUI/SSWCP.hpp"
#include "sentry_wrapper/SentryWrapper.hpp"

namespace pt = boost::property_tree;

namespace Slic3r {
namespace GUI {

PrinterWebView::PrinterWebView(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);

    OrcaWebLoadConfig config = OrcaWebViewLoader::CreateConfigForPage(2);
    m_browser = WebView::CreateWebViewWithLocalRoot(this, "", config.root_path, config.user_assets_dir);
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }

    m_browser->Bind(wxEVT_WEBVIEW_ERROR, &PrinterWebView::OnError, this);
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &PrinterWebView::OnLoaded, this);
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrinterWebView::OnScriptMessage, this, m_browser->GetId());

    SetSizer(topsizer);

    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

#ifdef __WIN32__
    Bind(wxEVT_SIZE, [this](wxSizeEvent& evt) {
        try_load_pending_orca_url();
        evt.Skip();
    });
#endif

    update_mode();

    //Zoom
    m_zoomFactor = 100;

    //Connect the idle events
    Bind(wxEVT_CLOSE_WINDOW, &PrinterWebView::OnClose, this);

 }

void PrinterWebView::try_load_pending_orca_url()
{
#ifdef __WIN32__
    if (!m_browser || m_pending_orca_url.empty())
        return;
    wxSize sz = m_browser->GetClientSize();
    if (sz.GetWidth() <= 0 || sz.GetHeight() <= 0)
        return;
    wxString u = m_pending_orca_url;
    m_pending_orca_url.clear();
    m_browser->LoadURL(u);
#endif
}

PrinterWebView::~PrinterWebView()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Start";
    SetEvtHandlerEnabled(false);
    SSWCP::on_webview_delete(m_browser);

    wxGetApp().fltviews().remove_printer_view(this);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " End";
}


void PrinterWebView::load_url(wxString& url, wxString apikey)
{
    if (m_browser == nullptr)
        return;
    m_apikey = apikey;
    m_apikey_sent = false;

    std::string url_str = url.ToUTF8().data();
    bool is_orca = (url_str.find("path=2") != std::string::npos) ||
                   (url_str.find("path=/deviceControl") != std::string::npos) ||
                   (url_str.find("path=%2FdeviceControl") != std::string::npos) ||
                   (url_str.find("orca://") != std::string::npos);

    if (is_orca) {
        wxGetApp().fltviews().add_printer_view(this, "orca:2", apikey);
        OrcaWebLoadConfig config = OrcaWebViewLoader::CreateConfigForPage(2);
        wxString url_to_load = OrcaWebViewLoader::LoadLocalHtml(m_browser, config);
#ifdef __WIN32__
        if (!url_to_load.empty()) {
            m_pending_orca_url = url_to_load;
            try_load_pending_orca_url();
            wxGetApp().CallAfter([this]() { try_load_pending_orca_url(); });
        }
#endif
        m_browser->Show();
    } else {
        wxGetApp().fltviews().remove_printer_view(this);
        m_browser->LoadURL(url);
        m_browser->Show();
    }

    UpdateState();
}

void PrinterWebView::load_url(const OrcaWebLoadConfig& config, wxString apikey)
{
    if (m_browser == nullptr)
        return;
    m_apikey = apikey;
    m_apikey_sent = false;

    wxGetApp().fltviews().add_printer_view(this, "orca:2", apikey);

    OrcaWebLoadConfig cfg = config;
    cfg.route_params = OrcaWebViewLoader::BuildRouteParamsFromApp();
    wxString url_to_load = OrcaWebViewLoader::LoadLocalHtml(m_browser, cfg);
#ifdef __WIN32__
    if (!url_to_load.empty()) {
        m_pending_orca_url = url_to_load;
        try_load_pending_orca_url();
        wxGetApp().CallAfter([this]() { try_load_pending_orca_url(); });
    }
#endif

    m_browser->Show();
    UpdateState();
}

void PrinterWebView::reload()
{
    m_browser->Reload();
}

bool PrinterWebView::isSnapmakerPage()
{
    wxString url = m_browser->GetCurrentURL();
    return url.Contains("flutter_web") || url.Contains("orca://");
}

void PrinterWebView::sendMessage(const std::string& msg) {
    WebView::RunScript(m_browser, msg);
}

void PrinterWebView::update_mode()
{
    // m_browser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
    m_browser->EnableAccessToDevTools(true);
}

/**
 * Method that retrieves the current state from the web control and updates the
 * GUI the reflect this current state.
 */
void PrinterWebView::UpdateState() {
  // SetTitle(m_browser->GetCurrentTitle());

}

void PrinterWebView::OnClose(wxCloseEvent& evt)
{
    this->Hide();
}

void PrinterWebView::SendAPIKey()
{
    if (m_apikey_sent || m_apikey.IsEmpty())
        return;
    m_apikey_sent   = true;
    wxString script = wxString::Format(R"(
    // Check if window.fetch exists before overriding
    if (window.fetch) {
        const originalFetch = window.fetch;
        window.fetch = function(input, init = {}) {
            init.headers = init.headers || {};
            init.headers['X-API-Key'] = '%s';
            return originalFetch(input, init);
        };
    }
)",
                                       m_apikey);
    m_browser->RemoveAllUserScripts();

    m_browser->AddUserScript(script);
    m_browser->Reload();
}

void PrinterWebView::OnError(wxWebViewEvent &evt)
{
    auto e = "unknown error";
    switch (evt.GetInt()) {
      case wxWEBVIEW_NAV_ERR_CONNECTION:
        e = "wxWEBVIEW_NAV_ERR_CONNECTION";
        break;
      case wxWEBVIEW_NAV_ERR_CERTIFICATE:
        e = "wxWEBVIEW_NAV_ERR_CERTIFICATE";
        break;
      case wxWEBVIEW_NAV_ERR_AUTH:
        e = "wxWEBVIEW_NAV_ERR_AUTH";
        break;
      case wxWEBVIEW_NAV_ERR_SECURITY:
        e = "wxWEBVIEW_NAV_ERR_SECURITY";
        break;
      case wxWEBVIEW_NAV_ERR_NOT_FOUND:
        e = "wxWEBVIEW_NAV_ERR_NOT_FOUND";
        break;
      case wxWEBVIEW_NAV_ERR_REQUEST:
        e = "wxWEBVIEW_NAV_ERR_REQUEST";
        break;
      case wxWEBVIEW_NAV_ERR_USER_CANCELLED:
        e = "wxWEBVIEW_NAV_ERR_USER_CANCELLED";
        break;
      case wxWEBVIEW_NAV_ERR_OTHER:
        e = "wxWEBVIEW_NAV_ERR_OTHER";
        break;
      }
    BOOST_LOG_TRIVIAL(fatal) << __FUNCTION__<< boost::format(":PrinterWebView error loading page %1% %2% %3% %4%") %evt.GetURL() %evt.GetTarget() %e %evt.GetString();
}

void PrinterWebView::OnLoaded(wxWebViewEvent &evt)
{
    if (evt.GetURL().IsEmpty())
        return;
    SendAPIKey();
}

void PrinterWebView::OnScriptMessage(wxWebViewEvent& evt) {
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << evt.GetString().ToUTF8().data();

    if (wxGetApp().get_mode() == comDevelop)
        wxLogMessage("Script message received; value = %s, handler = %s", evt.GetString(), evt.GetMessageHandler());

    // test
    SSWCP::handle_web_message(evt.GetString().ToUTF8().data(), m_browser);
}


} // GUI
} // Slic3r
