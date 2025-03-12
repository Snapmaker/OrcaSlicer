#include "WebSMUserLoginDialog.hpp"

#include <string.h>
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r_version.h"

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

BEGIN_EVENT_TABLE(SMUserLogin, wxDialog)
EVT_TIMER(NETWORK_OFFLINE_TIMER_ID, SMUserLogin::OnTimer)
END_EVENT_TABLE()

int SMUserLogin::web_sequence_id = 20000;

SMUserLogin::SMUserLogin(bool isLogout) : wxDialog((wxWindow *) (wxGetApp().mainframe), wxID_ANY, "Snapmaker Orca")
{
    SetBackgroundColour(*wxWHITE);

    BOOST_LOG_TRIVIAL(info) << "login url = " << TargetUrl.ToStdString();

    m_sm_user_agent = wxString::Format("SM-Slicer/v%s", SLIC3R_VERSION);

    // set the frame icon

    // Create the webview
    m_browser = WebView::CreateWebView(this, isLogout ? LogoutUrl : TargetUrl);
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
    Bind(wxEVT_WEBVIEW_NAVIGATING, &SMUserLogin::OnNavigationRequest, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NAVIGATED, &SMUserLogin::OnNavigationComplete, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_LOADED, &SMUserLogin::OnDocumentLoaded, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_ERROR, &SMUserLogin::OnError, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NEWWINDOW, &SMUserLogin::OnNewWindow, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_TITLE_CHANGED, &SMUserLogin::OnTitleChanged, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_FULLSCREEN_CHANGED, &SMUserLogin::OnFullScreenChanged, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &SMUserLogin::OnScriptMessage, this, m_browser->GetId());

    // Connect the idle events
    // Bind(wxEVT_IDLE, &SMUserLogin::OnIdle, this);
    // Bind(wxEVT_CLOSE_WINDOW, &SMUserLogin::OnClose, this);

    // UI
    SetTitle(isLogout ? _L("Log out") : _L("Login"));
    // Set a more sensible size for web browsing
    wxSize pSize = FromDIP(wxSize(650, 840));
    SetSize(pSize);

    int     screenheight = wxSystemSettings::GetMetric(wxSYS_SCREEN_Y, NULL);
    int     screenwidth  = wxSystemSettings::GetMetric(wxSYS_SCREEN_X, NULL);
    int     MaxY         = (screenheight - pSize.y) > 0 ? (screenheight - pSize.y) / 2 : 0;
    wxPoint tmpPT((screenwidth - pSize.x) / 2, MaxY);
    Move(tmpPT);
    wxGetApp().UpdateDlgDarkUI(this);
}

SMUserLogin::~SMUserLogin() {
    if (m_timer != NULL) {
        m_timer->Stop();
        delete m_timer;
        m_timer = NULL;
    }
}

void SMUserLogin::OnTimer(wxTimerEvent &event) {
    m_timer->Stop();

    if (m_networkOk == false)
    {
        ShowErrorPage();
    }
}

bool SMUserLogin::run() {
    m_timer = new wxTimer(this, NETWORK_OFFLINE_TIMER_ID);
    m_timer->Start(8000);

    if (this->ShowModal() == wxID_OK) {
        return true;
    } else {
        return false;
    }
}


void SMUserLogin::load_url(wxString &url)
{
    m_browser->LoadURL(url);
    m_browser->SetFocus();
    UpdateState();
}


/**
 * Method that retrieves the current state from the web control and updates
 * the GUI the reflect this current state.
 */
void SMUserLogin::UpdateState()
{
    // SetTitle(m_browser->GetCurrentTitle());
}

void SMUserLogin::OnIdle(wxIdleEvent &WXUNUSED(evt))
{
    if (m_browser->IsBusy()) {
        wxSetCursor(wxCURSOR_ARROWWAIT);
    } else {
        wxSetCursor(wxNullCursor);
    }
}

// void SMUserLogin::OnClose(wxCloseEvent& evt)
//{
//    this->Hide();
//}

/**
 * Callback invoked when there is a request to load a new page (for instance
 * when the user clicks a link)
 */
void SMUserLogin::OnNavigationRequest(wxWebViewEvent &evt)
{   
    wxString tmpUrl = evt.GetURL();
    
    size_t start = tmpUrl.find("token=");
    if (start != std::string::npos) {
        std::string token;
        
        start += std::string("token=").size(); // 跳过"token="的长度
        size_t end = tmpUrl.find("?", start);
        if (end != std::string::npos) {
            token = tmpUrl.substr(start, end - start).ToStdString();
        } else {
            token = tmpUrl.substr(start).ToStdString();
        }

        auto info = wxGetApp().sm_get_userinfo();
        info->set_user_login(true);
        info->set_user_token(token);

        this->EndModal(wxID_OK);

        wxGetApp().CallAfter([info, this]() {
            std::string url  = m_userInfoUrl.ToStdString();
            auto http = Http::get(url);
            std::string token = info->get_user_token();
            http.header("Authorization",token);
            http.on_complete([&](std::string body, unsigned status) {
                    if (status == 200) {
                        json response = json::parse(body);
                        wxGetApp().CallAfter([this, response]() {
                            if (response.count("data")) {
                                json data = response["data"];
                                if (data.count("nickname")) {
                                    wxGetApp().sm_get_userinfo()->set_user_name(data["nickname"].get<std::string>());
                                }
                                if (data.count("icon")) {
                                    wxGetApp().sm_get_userinfo()->set_user_icon_url(data["icon"].get<std::string>());
                                }
                            }
                        });
                    }
                })
                .on_error([&](std::string body, std::string error, unsigned status) {
                   
                })
                .perform(); 
        });
        this->RunScript("document.cookie = '';");
        // load_url(m_home_url);
    }
    UpdateState();
}

/**
 * Callback invoked when a navigation request was accepted
 */
void SMUserLogin::OnNavigationComplete(wxWebViewEvent &evt)
{
    // wxLogMessage("%s", "Navigation complete; url='" + evt.GetURL() + "'");
    m_browser->Show();
    Layout();
    UpdateState();
}

/**
 * Callback invoked when a page is finished loading
 */
void SMUserLogin::OnDocumentLoaded(wxWebViewEvent &evt)
{
    // Only notify if the document is the main frame, not a subframe
    wxString tmpUrl = evt.GetURL();
    std::string strHost = "https://id.snapmaker.com";

    if ( tmpUrl.Contains(strHost) ) {
        m_networkOk = true;
        // wxLogMessage("%s", "Document loaded; url='" + evt.GetURL() + "'");
    }

    UpdateState();
}

/**
 * On new window, we veto to stop extra windows appearing
 */
void SMUserLogin::OnNewWindow(wxWebViewEvent &evt)
{
    wxString flag = " (other)";

    if (evt.GetNavigationAction() == wxWEBVIEW_NAV_ACTION_USER) { flag = " (user)"; }

    // wxLogMessage("%s", "New window; url='" + evt.GetURL() + "'" + flag);

    // If we handle new window events then just load them in this window as we
    // are a single window browser
    m_browser->LoadURL(evt.GetURL());

    UpdateState();
}

void SMUserLogin::OnTitleChanged(wxWebViewEvent &evt)
{
    // SetTitle(evt.GetString());
    // wxLogMessage("%s", "Title changed; title='" + evt.GetString() + "'");
}

void SMUserLogin::OnFullScreenChanged(wxWebViewEvent &evt)
{
    // wxLogMessage("Full screen changed; status = %d", evt.GetInt());
    ShowFullScreen(evt.GetInt() != 0);
}

void SMUserLogin::OnScriptMessage(wxWebViewEvent &evt)
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
            wxGetApp().handle_script_message(j.dump());
            Close();
        }
        else if (strCmd == "get_localhost_url") {
            BOOST_LOG_TRIVIAL(info) << "thirdparty_login: get_localhost_url";
            wxGetApp().start_http_server();
            std::string sequence_id = j["sequence_id"].get<std::string>();
            CallAfter([this, sequence_id] {
                json ack_j;
                ack_j["command"] = "get_localhost_url";
                ack_j["response"]["base_url"] = std::string(LOCALHOST_URL) + std::to_string(LOCALHOST_PORT);
                ack_j["response"]["result"] = "success";
                ack_j["sequence_id"] = sequence_id;
                wxString str_js = wxString::Format("window.postMessage(%s)", ack_j.dump());
                this->RunScript(str_js);
            });
        }
        else if (strCmd == "thirdparty_login") {
            BOOST_LOG_TRIVIAL(info) << "thirdparty_login: thirdparty_login";
            if (j["data"].contains("url")) {
                std::string jump_url = j["data"]["url"].get<std::string>();
                CallAfter([this, jump_url] {
                    wxString url = wxString::FromUTF8(jump_url);
                    wxLaunchDefaultBrowser(url);
                    });
            }
        }
        else if (strCmd == "new_webpage") {
            if (j["data"].contains("url")) {
                std::string jump_url = j["data"]["url"].get<std::string>();
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

void SMUserLogin::RunScript(const wxString &javascript)
{
    // Remember the script we run in any case, so the next time the user opens
    // the "Run Script" dialog box, it is shown there for convenient updating.
    m_javascript = javascript;

    if (!m_browser) return;

    WebView::RunScript(m_browser, javascript);
}
#if wxUSE_WEBVIEW_IE
void SMUserLogin::OnRunScriptObjectWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){var person = new Object();person.name = 'Foo'; \
    person.lastName = 'Bar';return person;}f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}

void SMUserLogin::OnRunScriptDateWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){var d = new Date('10/08/2017 21:30:40'); \
    var tzoffset = d.getTimezoneOffset() * 60000; return \
    new Date(d.getTime() - tzoffset);}f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}

void SMUserLogin::OnRunScriptArrayWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){ return [\"foo\", \"bar\"]; }f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}
#endif

/**
 * Callback invoked when a loading error occurs
 */
void SMUserLogin::OnError(wxWebViewEvent &evt)
{
#define WX_ERROR_CASE(type) \
    case type: category = #type; break;

    wxString category;
    switch (evt.GetInt()) {
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_CONNECTION);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_CERTIFICATE);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_AUTH);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_SECURITY);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_NOT_FOUND);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_REQUEST);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_USER_CANCELLED);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_OTHER);
    }

    if( evt.GetInt()==wxWEBVIEW_NAV_ERR_CONNECTION )
    {
        if(m_timer!=NULL)
            m_timer->Stop();

        m_networkOk = false;

        if (m_networkOk==false)
            ShowErrorPage();
    }

    // wxLogMessage("%s", "Error; url='" + evt.GetURL() + "', error='" +
    // category + " (" + evt.GetString() + ")'");

    // Show the info bar with an error
    // m_info->ShowMessage(_L("An error occurred loading ") + evt.GetURL() +
    // "\n" + "'" + category + "'", wxICON_ERROR);

    UpdateState();
}

void SMUserLogin::OnScriptResponseMessage(wxCommandEvent &WXUNUSED(evt))
{
    // if (!m_response_js.empty())
    //{
    //    RunScript(m_response_js);
    //}

    // RunScript("This is a message to Web!");
    // RunScript("postMessage(\"AABBCCDD\");");
}

bool  SMUserLogin::ShowErrorPage()
{
    wxString ErrorUrl = from_u8((boost::filesystem::path(resources_dir()) / "web\\login\\error.html").make_preferred().string());
    wxString strlang   = wxGetApp().current_language_code_safe();
    if (strlang != "")
        ErrorUrl = wxString::Format("file://%s/web/login/error.html?lang=%s", from_u8(resources_dir()), strlang);
    load_url(ErrorUrl);

    return true;
}

}} // namespace Slic3r::GUI

