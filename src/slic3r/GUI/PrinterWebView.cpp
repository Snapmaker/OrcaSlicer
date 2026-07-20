#include "PrinterWebView.hpp"

#include "I18N.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "common_func/common_func.hpp"

#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>

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

    wxString url      = wxString::FromUTF8(LOCALHOST_URL + std::to_string(wxGetApp().get_page_http_port()) + "/web/flutter_web/index.html?path=2");
    auto     real_url = wxGetApp().get_international_url(url);
    m_last_url        = real_url;
      // Create the webview
    m_browser = WebView::CreateWebView(this, real_url);
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }

    m_browser->Bind(wxEVT_WEBVIEW_ERROR, &PrinterWebView::OnError, this);
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &PrinterWebView::OnLoaded, this);
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrinterWebView::OnScriptMessage, this, m_browser->GetId());

    // CreateWebView() already started loading m_last_url; watch it and retry
    // automatically until the page actually loads (see OnLoadRetryTimer).
    m_load_retry_timer.Bind(wxEVT_TIMER, &PrinterWebView::OnLoadRetryTimer, this);
    m_load_in_flight = true;
    m_load_retry_timer.Start(RETRY_INTERVAL_MS);

    SetSizer(topsizer);

    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    update_mode();

    //Zoom
    m_zoomFactor = 100;

    //Connect the idle events
    Bind(wxEVT_CLOSE_WINDOW, &PrinterWebView::OnClose, this);

 }

PrinterWebView::~PrinterWebView()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Start";
    m_load_retry_timer.Stop();
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
    m_last_url = url;
    m_retry_count = 0;
    m_load_succeeded = false;
    m_load_in_flight = true;
    m_in_flight_ticks = 0;
    if (!m_load_retry_timer.IsRunning())
        m_load_retry_timer.Start(RETRY_INTERVAL_MS);
    
    if (url.find("path=2") != std::string::npos) {
        wxGetApp().fltviews().add_printer_view(this, url, apikey);
    } else {
        wxGetApp().fltviews().remove_printer_view(this);
    }

    m_browser->Show();
    m_browser->LoadURL(url);

    UpdateState();
}

void PrinterWebView::reload()
{
    m_browser->Reload();
}

void PrinterWebView::reload_if_failed()
{
    if (m_load_succeeded || m_browser == nullptr || m_last_url.empty())
        return;
    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": device page was not loaded, reloading " << m_last_url;
    m_retry_count = 0;
    m_load_in_flight = true;
    m_in_flight_ticks = 0;
    if (!m_load_retry_timer.IsRunning())
        m_load_retry_timer.Start(RETRY_INTERVAL_MS);
    m_browser->LoadURL(m_last_url);
}

bool PrinterWebView::isSnapmakerPage()
{
    if (m_browser == nullptr)
        return false;
    auto url = m_browser->GetCurrentURL();
    return (url.find("flutter_web") != std::string::npos);
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

    // USER_CANCELLED (-999) means a load we issued ourselves cancelled a load
    // that was still in flight — not a real failure. Keep the in-flight state
    // of the new load and let it run.
    if (evt.GetInt() == wxWEBVIEW_NAV_ERR_USER_CANCELLED)
        return;
    m_load_succeeded = false;
    m_load_in_flight = false; // the retry timer will re-issue the load
}

void PrinterWebView::OnLoadRetryTimer(wxTimerEvent&)
{
    if (m_load_succeeded || m_browser == nullptr) {
        m_load_retry_timer.Stop();
        return;
    }
    if (m_last_url.empty())
        return;
    if (m_load_in_flight && m_in_flight_ticks < MAX_IN_FLIGHT_TICKS) {
        // A load is in flight; give it up to ~15s before treating it as hung.
        // (The startup-hang failure mode never completes nor errors quickly.)
        ++m_in_flight_ticks;
        return;
    }
    if (m_retry_count >= MAX_LOAD_RETRIES) {
        m_load_retry_timer.Stop();
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": giving up after " << MAX_LOAD_RETRIES << " attempts for " << m_last_url;
        return;
    }
    ++m_retry_count;
    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": reloading device page (attempt %1%/%2%) %3%") % m_retry_count % MAX_LOAD_RETRIES % m_last_url;
    m_load_in_flight  = true;
    m_in_flight_ticks = 0;
    m_browser->LoadURL(m_last_url);
}

void PrinterWebView::OnLoaded(wxWebViewEvent &evt)
{
    if (evt.GetURL().IsEmpty())
        return;
    m_retry_count = 0;
    m_load_succeeded = true;
    m_load_in_flight = false;
    m_load_retry_timer.Stop();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": PrinterWebView loaded ok: " << evt.GetURL();
    SendAPIKey();
}

void PrinterWebView::OnScriptMessage(wxWebViewEvent& evt) {
    // BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << evt.GetString().ToUTF8().data();

    // if (wxGetApp().get_mode() == comDevelop)
    //     wxLogMessage("Script message received; value = %s, handler = %s", evt.GetString(), evt.GetMessageHandler());

    // test
    SSWCP::handle_web_message(evt.GetString().ToUTF8().data(), m_browser);
}


} // GUI
} // Slic3r
