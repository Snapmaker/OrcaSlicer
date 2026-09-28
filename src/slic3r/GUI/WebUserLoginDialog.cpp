#include "WebUserLoginDialog.hpp"

#include <string.h>
#include <thread>
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/DeviceManager.hpp"   // Ultra P4: post-login cloud device discovery
#include "slic3r/GUI/HttpServer.hpp"     // LOCALHOST_PORT / BBL_LOGIN_LOCALHOST_URL
#include "sentry_wrapper/SentryWrapper.hpp"

#include <wx/sizer.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>

#include <wx/wx.h>
#include <wx/fileconf.h>
#include <wx/file.h>
#include <wx/wfstream.h>

#include <boost/cast.hpp>
#include <boost/lexical_cast.hpp>

#include <nlohmann/json.hpp>
#include "MainFrame.hpp"
#include <boost/dll.hpp>

#include <sstream>
#include <slic3r/GUI/Widgets/WebView.hpp>
using namespace std;

using namespace nlohmann;

namespace Slic3r { namespace GUI {

#define NETWORK_OFFLINE_TIMER_ID 10001

BEGIN_EVENT_TABLE(ZUserLogin, wxDialog)
EVT_TIMER(NETWORK_OFFLINE_TIMER_ID, ZUserLogin::OnTimer)
END_EVENT_TABLE()

int ZUserLogin::web_sequence_id = 20000;

ZUserLogin::ZUserLogin() : wxDialog((wxWindow *) (wxGetApp().mainframe), wxID_ANY, "EdgeSlicer")
{
    SetBackgroundColour(*wxWHITE);
    // Url
    NetworkAgent* agent = wxGetApp().getAgent();
    if (!agent) {
        std::string icon_path = (boost::format("%1%/images/EdgeSlicerTitle.ico") % resources_dir()).str();
        SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));

        SetBackgroundColour(*wxWHITE);

        wxBoxSizer* m_sizer_main = new wxBoxSizer(wxVERTICAL);
        auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
        m_sizer_main->Add(m_line_top, 0, wxEXPAND, 0);

        // Ultra (plug-in guards): with our own plug-in installed but not loaded, the download link
        // would fetch Bambu's package over it. A restart is what is actually needed.
        const bool ultranet = wxGetApp().is_ultranet_plugin_installed();
        auto* m_message = new wxStaticText(this, wxID_ANY,
            ultranet ? _L("The network plug-in is installed but not loaded yet. Please restart EdgeSlicer and sign in again.")
                     : _L("Bambu Network plug-in not detected."),
            wxDefaultPosition, wxDefaultSize, 0);
        m_message->SetForegroundColour(*wxBLACK);
        m_message->Wrap(FromDIP(360));

        m_sizer_main->Add(m_message, 0, wxALIGN_CENTER | wxALL, FromDIP(15));
        if (! ultranet) {
            auto m_download_hyperlink = new wxHyperlinkCtrl(this, wxID_ANY, _L("Click here to download it."), wxEmptyString, wxDefaultPosition, wxDefaultSize, wxHL_DEFAULT_STYLE);
            m_download_hyperlink->Bind(wxEVT_HYPERLINK, [this](wxCommandEvent& event) {
                this->Close();
                wxGetApp().ShowDownNetPluginDlg();
                });
            m_sizer_main->Add(m_download_hyperlink, 0, wxALIGN_CENTER | wxALL, FromDIP(10));
        }
        m_sizer_main->Add(0, 0, 1, wxBOTTOM, 10);

        SetSizer(m_sizer_main);
        m_sizer_main->SetSizeHints(this);
        Layout();
        Fit();
        CentreOnParent();
    }
    else {
        std::string host_url = agent->get_bambulab_host();
        // Ultra P4: load the NON-localed /sign-in. The fork appended the UI language
        // (e.g. /en-US/sign-in), which made bambulab carry locale through the OAuth flow
        // and redirect Google's result to the localed callback /en-us/sign-in/callback,
        // which 404s. Stock Bambu Studio uses the bare /sign-in, whose /sign-in/callback
        // resolves and posts the token back to our loopback server.
        TargetUrl = host_url + "/sign-in";
        m_networkOk = false;

        BOOST_LOG_TRIVIAL(info) << "login url = " << TargetUrl.ToStdString();

        m_sm_user_agent = wxString::Format("SM-Slicer/v%s", SLIC3R_VERSION);

        // set the frame icon

        // Create the webview. Ultra: use the BBL-Slicer UA so bambulab.com/sign-in runs the
        // slicer login flow and posts the token back via script message (see OnScriptMessage);
        // the default SM-Slicer UA gets a normal web login that just redirects to the home page.
        m_browser = WebView::CreateWebView(this, TargetUrl, "BBL-Slicer");
        if (m_browser == nullptr) {
            wxLogError("Could not init m_browser");
            return;
        }
        m_browser->Hide();
        m_browser->SetSize(0, 0);

        // Log backend information
        // wxLogMessage(wxWebView::GetBackendVersionInfo().ToString());
        // wxLogMessage("Backend: %s Version: %s",
        // m_browser->GetClassInfo()->GetClassName(),wxWebView::GetBackendVersionInfo().ToString());
        // wxLogMessage("User Agent: %s", m_browser->GetUserAgent());

        // Connect the webview events
        Bind(wxEVT_WEBVIEW_NAVIGATING, &ZUserLogin::OnNavigationRequest, this, m_browser->GetId());
        Bind(wxEVT_WEBVIEW_NAVIGATED, &ZUserLogin::OnNavigationComplete, this, m_browser->GetId());
        Bind(wxEVT_WEBVIEW_LOADED, &ZUserLogin::OnDocumentLoaded, this, m_browser->GetId());
        Bind(wxEVT_WEBVIEW_ERROR, &ZUserLogin::OnError, this, m_browser->GetId());
        Bind(wxEVT_WEBVIEW_NEWWINDOW, &ZUserLogin::OnNewWindow, this, m_browser->GetId());
        Bind(wxEVT_WEBVIEW_TITLE_CHANGED, &ZUserLogin::OnTitleChanged, this, m_browser->GetId());
        Bind(wxEVT_WEBVIEW_FULLSCREEN_CHANGED, &ZUserLogin::OnFullScreenChanged, this, m_browser->GetId());
        Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &ZUserLogin::OnScriptMessage, this, m_browser->GetId());

        // Connect the idle events
        // Bind(wxEVT_IDLE, &ZUserLogin::OnIdle, this);
        // Bind(wxEVT_CLOSE_WINDOW, &ZUserLogin::OnClose, this);

        // UI
        SetTitle(_L("Login"));
        // Set a more sensible size for web browsing
        wxSize pSize = FromDIP(wxSize(650, 840));
        SetSize(pSize);

        int screenheight = wxSystemSettings::GetMetric(wxSYS_SCREEN_Y, NULL);
        int screenwidth = wxSystemSettings::GetMetric(wxSYS_SCREEN_X, NULL);
        int MaxY = (screenheight - pSize.y) > 0 ? (screenheight - pSize.y) / 2 : 0;
        wxPoint tmpPT((screenwidth - pSize.x) / 2, MaxY);
        Move(tmpPT);
    }
    wxGetApp().UpdateDlgDarkUI(this);
}

ZUserLogin::~ZUserLogin() {
    if (m_timer != NULL) {
        m_timer->Stop();
        delete m_timer;
        m_timer = NULL;
    }
}

void ZUserLogin::OnTimer(wxTimerEvent &event) {
    m_timer->Stop();

    if (m_networkOk == false)
    {
        ShowErrorPage();
    }
}

bool ZUserLogin::run() {
    m_timer = new wxTimer(this, NETWORK_OFFLINE_TIMER_ID);
    m_timer->Start(8000);

    if (this->ShowModal() == wxID_OK) {
        return true;
    } else {
        return false;
    }
}


void ZUserLogin::load_url(wxString &url)
{
    m_browser->LoadURL(url);
    m_browser->SetFocus();
    UpdateState();
}


/**
 * Method that retrieves the current state from the web control and updates
 * the GUI the reflect this current state.
 */
void ZUserLogin::UpdateState()
{
    // SetTitle(m_browser->GetCurrentTitle());
}

void ZUserLogin::OnIdle(wxIdleEvent &WXUNUSED(evt))
{
    if (m_browser->IsBusy()) {
        wxSetCursor(wxCURSOR_ARROWWAIT);
    } else {
        wxSetCursor(wxNullCursor);
    }
}

// void ZUserLogin::OnClose(wxCloseEvent& evt)
//{
//    this->Hide();
//}

/**
 * Callback invoked when there is a request to load a new page (for instance
 * when the user clicks a link)
 */
void ZUserLogin::OnNavigationRequest(wxWebViewEvent &evt)
{
    UpdateState();
}

/**
 * Callback invoked when a navigation request was accepted
 */
void ZUserLogin::OnNavigationComplete(wxWebViewEvent &evt)
{
    // wxLogMessage("%s", "Navigation complete; url='" + evt.GetURL() + "'");
    m_browser->Show();
    Layout();
    UpdateState();
}

/**
 * Callback invoked when a page is finished loading
 */
void ZUserLogin::OnDocumentLoaded(wxWebViewEvent &evt)
{
    // Only notify if the document is the main frame, not a subframe
    wxString tmpUrl = evt.GetURL();
    NetworkAgent* agent = wxGetApp().getAgent();
    std::string strHost = agent->get_bambulab_host();

    if ( tmpUrl.Contains(strHost) ) {
        m_networkOk = true;
        // wxLogMessage("%s", "Document loaded; url='" + evt.GetURL() + "'");
    }

    UpdateState();
}

/**
 * On new window, we veto to stop extra windows appearing
 */
void ZUserLogin::OnNewWindow(wxWebViewEvent &evt)
{
    wxString flag = " (other)";

    if (evt.GetNavigationAction() == wxWEBVIEW_NAV_ACTION_USER) { flag = " (user)"; }

    // wxLogMessage("%s", "New window; url='" + evt.GetURL() + "'" + flag);

    // If we handle new window events then just load them in this window as we
    // are a single window browser
    m_browser->LoadURL(evt.GetURL());

    UpdateState();
}

void ZUserLogin::OnTitleChanged(wxWebViewEvent &evt)
{
    // SetTitle(evt.GetString());
    // wxLogMessage("%s", "Title changed; title='" + evt.GetString() + "'");
}

void ZUserLogin::OnFullScreenChanged(wxWebViewEvent &evt)
{
    // wxLogMessage("Full screen changed; status = %d", evt.GetInt());
    ShowFullScreen(evt.GetInt() != 0);
}

void ZUserLogin::OnScriptMessage(wxWebViewEvent &evt)
{
    wxString str_input = evt.GetString();
    try {
        json j = json::parse(into_u8(str_input));

        wxString strCmd = j["command"];

        if (strCmd == "autotest_token")
        {
            m_AutotestToken = j["data"]["token"];
        }
        if (strCmd == "user_login") {
            j["data"]["autotest_token"] = m_AutotestToken;
            // Ultra: the fork previously just Close()d here and dropped the Bambu login payload.
            // Forward it to the network agent (Ultra Net) so it stores the token / user info and
            // reports is_user_login()=true; the Account menu then shows the signed-in account.
            if (auto agent = wxGetApp().getAgent()) {
                agent->change_user(j.dump());
                // Ultra P4: the fork gutted the post-login handler, so kick cloud device
                // discovery ourselves. update_user_machine_list_info() does a blocking HTTPS
                // GET + parse, so run it off the UI thread; the SelectMachine/device page then
                // shows the cloud printers (My Devices).
                std::thread([] {
                    if (auto dev = Slic3r::GUI::wxGetApp().getDeviceManager())
                        dev->update_user_machine_list_info();
                }).detach();
            }
            Close();
        }
        else if (strCmd == "user_ticket_login") {
            // Ultra: the Bambu /sign-in page delivers an IN-PAGE (email/password) login as
            // command:"user_ticket_login" carrying data.ticket — a short-lived exchange ticket,
            // NOT a token, and NOT the command:"user_login" the fork previously assumed. It must be
            // exchanged for real tokens, exactly like the OAuth loopback does in
            // HttpServer::bbl_auth_handle_request. Without this branch the message was silently
            // dropped (no else/default), the dialog stayed open, and the page relit its button.
            std::string ticket = (j.contains("data") && j["data"].contains("ticket") && j["data"]["ticket"].is_string())
                                     ? j["data"]["ticket"].get<std::string>() : std::string();
            bool ok = false;
            if (auto agent = wxGetApp().getAgent()) {
                std::string access_token, refresh_token, expires_in_str, refresh_expires_in_str;
                unsigned int tk_code = 0; std::string tk_body;
                if (!ticket.empty() && agent->get_my_token(ticket, &tk_code, &tk_body) == 0) {
                    try {
                        json tj = json::parse(tk_body);
                        if (tj.contains("accessToken"))  access_token  = tj["accessToken"].get<std::string>();
                        if (tj.contains("refreshToken")) refresh_token = tj["refreshToken"].get<std::string>();
                        if (tj.contains("expiresIn"))
                            expires_in_str = tj["expiresIn"].is_string() ? tj["expiresIn"].get<std::string>()
                                                                          : std::to_string(tj["expiresIn"].get<long long>());
                        if (tj.contains("refreshExpiresIn"))
                            refresh_expires_in_str = tj["refreshExpiresIn"].is_string() ? tj["refreshExpiresIn"].get<std::string>()
                                                                                        : std::to_string(tj["refreshExpiresIn"].get<long long>());
                    } catch (const std::exception &e) {
                        BOOST_LOG_TRIVIAL(error) << "user_ticket_login: token JSON parse failed: " << e.what();
                    }
                }
                if (!access_token.empty()) {
                    unsigned int http_code = 0; std::string http_body;
                    if (agent->get_my_profile(access_token, &http_code, &http_body) == 0) {
                        std::string user_id, user_name, user_account, user_avatar;
                        try {
                            json uj = json::parse(http_body);
                            if (uj.contains("uidStr"))  user_id      = uj["uidStr"].get<std::string>();
                            if (uj.contains("name"))    user_name    = uj["name"].get<std::string>();
                            if (uj.contains("avatar"))  user_avatar  = uj["avatar"].get<std::string>();
                            if (uj.contains("account")) user_account = uj["account"].get<std::string>();
                        } catch (const std::exception &e) {
                            BOOST_LOG_TRIVIAL(error) << "user_ticket_login: profile JSON parse failed: " << e.what();
                        }
                        json cu;
                        cu["data"]["refresh_token"]      = refresh_token;
                        cu["data"]["token"]              = access_token;   // literal "token" — matches the agent scraper
                        cu["data"]["expires_in"]         = expires_in_str;
                        cu["data"]["refresh_expires_in"] = refresh_expires_in_str;
                        cu["data"]["user"]["uid"]        = user_id;
                        cu["data"]["user"]["name"]       = user_name;
                        cu["data"]["user"]["account"]    = user_account;
                        cu["data"]["user"]["avatar"]     = user_avatar;
                        cu["data"]["autotest_token"]     = m_AutotestToken;
                        agent->change_user(cu.dump());
                        ok = agent->is_user_login();
                    }
                }
            }
            if (ok) {
                // kick cloud device discovery (My Devices), same as the OAuth loopback path
                wxGetApp().kick_user_device_refresh();
                std::thread([] {
                    if (auto dev = Slic3r::GUI::wxGetApp().getDeviceManager())
                        dev->update_user_machine_list_info();
                }).detach();
                Close();
            } else {
                wxMessageBox("Login failed. Please try again.", "Login", wxICON_WARNING);
            }
        }
        else if (strCmd == "get_localhost_url") {
            BOOST_LOG_TRIVIAL(info) << "thirdparty_login: get_localhost_url";
            // Ultra P4: actually start the loopback OAuth-callback server (was gutted) so
            // the third-party sign-in redirect to 127.0.0.1:<port> is caught. Advertise the
            // REAL bound port in case LOCALHOST_PORT was busy.
            wxGetApp().start_http_server();
            std::string sequence_id = j["sequence_id"].get<std::string>();
            CallAfter([this, sequence_id] {
                json ack_j;
                ack_j["command"] = "get_localhost_url";
                // Ultra: advertise http://localhost:<port> (NOT 127.0.0.1) - bambulab's
                // sign-in callback validates redirect_url and rejects the 127.0.0.1 form.
                // Report the port the server actually bound; it is LOCALHOST_PORT (13618),
                // the port bambulab's third-party redirect is registered against. If the
                // bind fell back to another port (13618 already taken by another slicer),
                // the third-party leg cannot work at all - log it loudly so the owner sees
                // why, instead of the user just getting a 404 in the browser.
                auto bound_port = wxGetApp().get_http_port();
                if (bound_port != LOCALHOST_PORT) {
                    BOOST_LOG_TRIVIAL(error) << "thirdparty_login: loopback bound to port " << bound_port
                                             << " instead of " << LOCALHOST_PORT
                                             << " - third-party (Google) sign-in will fail; is another "
                                                "Bambu Studio / OrcaSlicer running?";
                }
                ack_j["response"]["base_url"] = std::string(BBL_LOGIN_LOCALHOST_URL) + std::to_string(bound_port);
                ack_j["response"]["result"] = "success";
                ack_j["sequence_id"] = sequence_id;
                wxString str_js = wxString::Format("window.postMessage(%s)", ack_j.dump());
                this->RunScript(str_js);
            });
        }
        else if (strCmd == "thirdparty_login") {
            std::string jump_url = j["data"].contains("url") ? j["data"]["url"].get<std::string>() : std::string();
            if (!jump_url.empty()) {
                CallAfter([this, jump_url] {
                    wxString url = wxString::FromUTF8(jump_url);
                    wxLaunchDefaultBrowser(url);
                    });
            }
        }
        else if (strCmd == "new_webpage") {
            std::string jump_url = j["data"].contains("url") ? j["data"]["url"].get<std::string>() : std::string();
            if (!jump_url.empty()) {
                CallAfter([this, jump_url] {
                    wxString url = wxString::FromUTF8(jump_url);
                    wxLaunchDefaultBrowser(url);
                    });
            }
            return;
        }
    } catch (std::exception &e) {
        wxMessageBox(e.what(), "parse json failed", wxICON_WARNING);
        Close();
    }
}

void ZUserLogin::RunScript(const wxString &javascript)
{
    // Remember the script we run in any case, so the next time the user opens
    // the "Run Script" dialog box, it is shown there for convenient updating.
    m_javascript = javascript;

    if (!m_browser) return;

    WebView::RunScript(m_browser, javascript);
}
#if wxUSE_WEBVIEW_IE
void ZUserLogin::OnRunScriptObjectWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){var person = new Object();person.name = 'Foo'; \
    person.lastName = 'Bar';return person;}f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}

void ZUserLogin::OnRunScriptDateWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){var d = new Date('10/08/2017 21:30:40'); \
    var tzoffset = d.getTimezoneOffset() * 60000; return \
    new Date(d.getTime() - tzoffset);}f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}

void ZUserLogin::OnRunScriptArrayWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){ return [\"foo\", \"bar\"]; }f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}
#endif

/**
 * Callback invoked when a loading error occurs
 */
void ZUserLogin::OnError(wxWebViewEvent &event)
{
    auto e = "unknown error";
    switch (event.GetInt()) {
    case wxWEBVIEW_NAV_ERR_CONNECTION: e = "wxWEBVIEW_NAV_ERR_CONNECTION"; break;
    case wxWEBVIEW_NAV_ERR_CERTIFICATE: e = "wxWEBVIEW_NAV_ERR_CERTIFICATE"; break;
    case wxWEBVIEW_NAV_ERR_AUTH: e = "wxWEBVIEW_NAV_ERR_AUTH"; break;
    case wxWEBVIEW_NAV_ERR_SECURITY: e = "wxWEBVIEW_NAV_ERR_SECURITY"; break;
    case wxWEBVIEW_NAV_ERR_NOT_FOUND: e = "wxWEBVIEW_NAV_ERR_NOT_FOUND"; break;
    case wxWEBVIEW_NAV_ERR_REQUEST: e = "wxWEBVIEW_NAV_ERR_REQUEST"; break;
    case wxWEBVIEW_NAV_ERR_USER_CANCELLED: e = "wxWEBVIEW_NAV_ERR_USER_CANCELLED"; break;
    case wxWEBVIEW_NAV_ERR_OTHER: e = "wxWEBVIEW_NAV_ERR_OTHER"; break;
    }
    BOOST_LOG_TRIVIAL(fatal) << __FUNCTION__<< boost::format(":ZUserLogin error loading page %1% %2% %3% %4%") % event.GetURL() % event.GetTarget() %e % event.GetString();
    
}

void ZUserLogin::OnScriptResponseMessage(wxCommandEvent &WXUNUSED(evt))
{
    // if (!m_response_js.empty())
    //{
    //    RunScript(m_response_js);
    //}

    // RunScript("This is a message to Web!");
    // RunScript("postMessage(\"AABBCCDD\");");
}

bool  ZUserLogin::ShowErrorPage()
{
    wxString ErrortUrl = from_u8((boost::filesystem::path(resources_dir()) / "web\\login\\error.html").make_preferred().string());
    load_url(ErrortUrl);

    return true;
}


}} // namespace Slic3r::GUI
