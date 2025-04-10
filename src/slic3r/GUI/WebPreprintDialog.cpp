#include "WebPreprintDialog.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "SSWCP.hpp"
#include <wx/sizer.h>
#include <slic3r/GUI/Widgets/WebView.hpp>
#include "NotificationManager.hpp"

namespace Slic3r { namespace GUI {

BEGIN_EVENT_TABLE(WebPreprintDialog, wxDialog)
    EVT_CLOSE(WebPreprintDialog::OnClose)
END_EVENT_TABLE()

WebPreprintDialog::WebPreprintDialog()
    : wxDialog((wxWindow*)(wxGetApp().mainframe), wxID_ANY, _L("Print preset"))
{
    m_prePrint_url = "http://localhost:" + std::to_string(wxGetApp().m_page_http_server.get_port()) +
                     "/web/flutter_web/index.html?path=filament_extruder_mapping";
    SetBackgroundColour(*wxWHITE);

    // Create the webview

    // 语言判断
    wxString strlang = wxGetApp().current_language_code_safe();
    wxString target_url = m_prePrint_url;
    if (strlang != "") {
        if (target_url.find("?") != std::string::npos) {
            target_url += "&lang=" + strlang;
        } else {
            target_url += "?lang=" + strlang;
        }
    }
        

    m_browser = WebView::CreateWebView(this, target_url);
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }
    m_browser->Hide();

    // Connect the webview events
    Bind(wxEVT_WEBVIEW_NAVIGATING, &WebPreprintDialog::OnNavigationRequest, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NAVIGATED, &WebPreprintDialog::OnNavigationComplete, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_LOADED, &WebPreprintDialog::OnDocumentLoaded, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_ERROR, &WebPreprintDialog::OnError, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &WebPreprintDialog::OnScriptMessage, this, m_browser->GetId());

    // Set dialog size
    SetSize(FromDIP(wxSize(800, 600)));

    // Create sizer and add webview
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    SetSizer(sizer);

    // Center dialog
    CenterOnParent();

    wxGetApp().UpdateDlgDarkUI(this);
    wxGetApp().set_web_preprint_dialog(this);
}

WebPreprintDialog::~WebPreprintDialog()
{
    SSWCP::on_webview_delete(m_browser);

    wxGetApp().set_web_preprint_dialog(nullptr);
}

void WebPreprintDialog::set_display_file_name(const std::string& filename) {
    m_display_file_name = filename;
}

void WebPreprintDialog::set_gcode_file_name(const std::string& filename)
{ m_gcode_file_name = filename; }

void WebPreprintDialog::reload()
{
    load_url(m_prePrint_url);
}

void WebPreprintDialog::load_url(wxString &url)
{
    m_browser->LoadURL(url);
    m_browser->Show();
    Layout();
}

bool WebPreprintDialog::run()
{
    SSWCP::update_active_filename(m_gcode_file_name);
    SSWCP::update_display_filename(m_display_file_name);

    this->load_url(m_prePrint_url);
    if (this->ShowModal() == wxID_OK) {
        return true;
    }
    return false;
}

void WebPreprintDialog::RunScript(const wxString &javascript)
{
    m_javascript = javascript;
    if (!m_browser) return;
    WebView::RunScript(m_browser, javascript);
}

void WebPreprintDialog::OnNavigationRequest(wxWebViewEvent &evt)
{
    evt.Skip();
}

void WebPreprintDialog::OnNavigationComplete(wxWebViewEvent &evt)
{
    m_browser->Show();
    Layout();
}

void WebPreprintDialog::OnDocumentLoaded(wxWebViewEvent &evt)
{
    evt.Skip();
}

void WebPreprintDialog::OnError(wxWebViewEvent &evt)
{
    wxLogError("Web View Error: %s", evt.GetString());
}

void WebPreprintDialog::OnScriptMessage(wxWebViewEvent &evt)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << evt.GetString().ToUTF8().data();

    if (wxGetApp().get_mode() == comDevelop)
        wxLogMessage("Script message received; value = %s, handler = %s", evt.GetString(), evt.GetMessageHandler());

    // test
    SSWCP::handle_web_message(evt.GetString().ToUTF8().data(), m_browser);

}

void WebPreprintDialog::OnClose(wxCloseEvent& evt)
{
    auto noti_manager = wxGetApp().mainframe->plater()->get_notification_manager();
    noti_manager->close_notification_of_type(NotificationType::PrintHostUpload);
    evt.Skip();
}

}} // namespace Slic3r::GUI 