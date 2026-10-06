#include "StreamPanel.hpp"

#include "RemoteAccess.hpp"
#include "RemoteHub.hpp"
#include "GUI_App.hpp"
#include "HttpServer.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/Utils/Http.hpp"

#include <nlohmann/json.hpp>
#include <wx/sizer.h>
#include <wx/webview.h>
#include <wx/event.h>
#include <wx/frame.h>
#include <wx/uri.h>
#include <wx/weakref.h>

#include <thread>

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace GUI {

StreamPanel::StreamPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
    std::string path = "/web/orca/stream_center.html";
    // Seed the page's host list from the legacy camera-address preference, if set.
    const std::string seed = wxGetApp().app_config->get("hd_camera_host");
    if (!seed.empty())
        path += "?seed=" + wxURI(wxString::FromUTF8(seed)).BuildURI().ToStdString(wxConvUTF8);
    wxString url = wxString::FromUTF8(wxGetApp().page_url(path));

    m_browser = WebView::CreateWebView(this, url);
    if (m_browser == nullptr)
        return;

    // The page talks to us over the app-wide "wx" script channel that CreateWebView already
    // injects; a second named handler is never injected by the Edge backend, so don't add one.
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &StreamPanel::OnScriptMessage, this, m_browser->GetId());

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    SetSizer(sizer);

    Bind(wxEVT_SHOW, &StreamPanel::OnShow, this);
    // A page that loads while its tab is hidden would otherwise start its tiles and keep them.
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &StreamPanel::OnPageLoaded, this, m_browser->GetId());
    if (wxWindow* top = wxGetTopLevelParent(this))
        top->Bind(wxEVT_ICONIZE, &StreamPanel::OnIconize, this);

    // The instance API and the hub handshake used to start here; they now start from
    // GUI_App::start_remote_access() so a hidden instance (no Stream tab) registers too.
}

void StreamPanel::SetPageActive(bool active)
{
    if (m_browser == nullptr || active == m_active)
        return;
    m_active = active;
    WebView::RunScript(m_browser, wxString::Format("if (window.__snorcaTilesActive) window.__snorcaTilesActive(%s);",
                                                   active ? "true" : "false"));
}

void StreamPanel::OnShow(wxShowEvent& evt)
{
    evt.Skip();
    SetPageActive(evt.IsShown() && !m_iconized);
}

void StreamPanel::OnIconize(wxIconizeEvent& evt)
{
    evt.Skip(); // the frame's own handlers still run
    m_iconized = evt.IsIconized();
    SetPageActive(!m_iconized && IsShownOnScreen());
}

void StreamPanel::OnPageLoaded(wxWebViewEvent& evt)
{
    evt.Skip();
    // The page has just (re)started with every tile running: say so if it is not on screen.
    const bool on = !m_iconized && IsShownOnScreen();
    if (!on) {
        m_active = true; // force the call: the new page has not been told anything
        SetPageActive(false);
    }
}

void StreamPanel::OnScriptMessage(wxWebViewEvent& evt)
{
    // Ultra: only our own stream page may start the hub or read the remote-access state.
    if (m_browser == nullptr || !wxGetApp().is_own_page_url(m_browser->GetCurrentURL().ToUTF8().data())) {
        BOOST_LOG_TRIVIAL(warning) << "StreamPanel: ignored a message from a page that is not ours";
        return;
    }
    const wxString msg = evt.GetString();
    if (msg == "hub_start") {
        // The page needs a relay (go2rtc for RTSP/ONVIF, MJPEG for Bambu P1): make sure the
        // hub runs and hand back its ports.
        wxWeakRef<StreamPanel> weak(this);
        std::thread([weak]() {
            RemoteHub::Info info = RemoteHub::ensure_running("", false);
            wxGetApp().CallAfter([weak, info]() {
                if (weak == nullptr || weak->m_browser == nullptr)
                    return;
                // The page embeds the player through the hub (never go2rtc directly) and proves
                // itself with the hub secret; 0 means the relay is not available.
                WebView::RunScript(weak->m_browser, wxString::Format("if (window.__hubReady) window.__hubReady(%d, %d, '%s');",
                    info.go2rtc_port ? info.port : 0, info.relay_port, wxString::FromUTF8(info.secret)));
            });
        }).detach();
    } else if (msg == "onvif_discover") {
        // ONVIF discovery runs in go2rtc; the page reaches it through us and the hub.
        wxWeakRef<StreamPanel> weak(this);
        std::thread([weak]() {
            const auto res = RemoteHub::onvif_discover();
            wxGetApp().CallAfter([weak, res]() {
                if (weak == nullptr || weak->m_browser == nullptr)
                    return;
                WebView::RunScript(weak->m_browser, wxString::Format("if (window.__onvifResult) window.__onvifResult(%d, %s);",
                    res.first, wxString::FromUTF8(nlohmann::json(res.second).dump())));
            });
        }).detach();
    } else if (msg == "stream_state_get") {
        // The page opens with the hub's camera list, not its own localStorage: that is per
        // origin, and each window's page gets its own port, so a window on another port would
        // otherwise push an old list over the hub's and drop cameras from go2rtc.
        wxWeakRef<StreamPanel> weak(this);
        std::thread([weak]() {
            const std::string state = RemoteHub::saved_state();
            wxGetApp().CallAfter([weak, state]() {
                if (weak == nullptr || weak->m_browser == nullptr)
                    return;
                std::string arg = "null";
                try {
                    if (!state.empty()) arg = nlohmann::json::parse(state).dump();
                } catch (...) {}
                WebView::RunScript(weak->m_browser, wxString::Format("if (window.__hubState) window.__hubState(%s);", wxString::FromUTF8(arg)));
            });
        }).detach();
    } else if (msg.StartsWith("stream_state:")) {
        // The page's full camera list (with credentials): the hub keeps it for the phone and
        // for streams that outlive this window. Remembered here if no hub runs yet.
        const std::string state = msg.Mid(13).ToStdString(wxConvUTF8);
        std::thread([state]() { RemoteHub::post_state(state); }).detach();
    } else if (msg == "remote_on" || msg == "remote_off" || msg == "remote_info" || msg == "remote_newlink") {
        // The toggle is remembered, and so is the link: the same one comes back after off/on and
        // after a restart, because it is in QR codes people scanned and icons they installed.
        // "New link" is the only thing that replaces it, and the page says what that breaks.
        wxWeakRef<StreamPanel> weak(this);
        const std::string      saved_token = wxGetApp().app_config->get("stream_phone_token");
        const std::string      what        = msg.ToStdString();
        std::thread([weak, what, saved_token]() {
            RemoteHub::Info info;
            if (what == "remote_on")
                info = RemoteHub::ensure_running(saved_token, true);
            else if (what == "remote_off")
                info = RemoteHub::set_phone(false);
            else if (what == "remote_newlink")
                info = RemoteHub::new_link();
            else
                info = RemoteHub::query();
            wxGetApp().CallAfter([weak, what, info]() {
                auto* cfg = wxGetApp().app_config;
                if (what == "remote_on")
                    cfg->set("stream_phone_access", info.alive && info.phone ? "1" : "0");
                else if (what == "remote_off")
                    cfg->set("stream_phone_access", "0");
                // Remember whatever link the hub is using now - after a new one, and after it was
                // turned off too: off stops the link, it does not throw it away. This is what a
                // later hub is seeded with if it comes up with no memory of its own.
                if (!info.token.empty())
                    cfg->set("stream_phone_token", info.token);
                if (weak == nullptr || weak->m_browser == nullptr)
                    return;
                WebView::RunScript(weak->m_browser, wxString::Format("if (window.__remoteInfo) window.__remoteInfo(%s);", wxString::FromUTF8(info.json())));
            });
        }).detach();
    } else if (msg.StartsWith("ff_detail:")) {
        // Flashforge new-gen LAN API: POST http://<ip>:8898/detail returns device
        // detail JSON including cameraStreamUrl (rtsp:// on Creator 5, MJPEG http://
        // on the Adventurer 5M family). The page cannot POST there itself (no CORS
        // headers from the printer), so probe from here and hand the URL back.
        const std::string ip = msg.Mid(10).ToStdString();
        if (ip.empty() || ip.find_first_of("\"'\\<>") != std::string::npos)
            return;
        wxWeakRef<StreamPanel> weak(this);
        auto reply = [weak, ip](const std::string& url) {
            wxGetApp().CallAfter([weak, ip, url]() {
                if (weak == nullptr || weak->m_browser == nullptr)
                    return;
                nlohmann::json esc_ip = ip, esc_url = url; // JSON-escape for JS literals
                WebView::RunScript(weak->m_browser,
                    wxString::Format("if (window.__ffDetail) window.__ffDetail(%s, %s);",
                                     wxString::FromUTF8(esc_ip.dump()), wxString::FromUTF8(esc_url.dump())));
            });
        };
        auto http = Http::post("http://" + ip + ":8898/detail");
        http.tls_policy(Http::TlsPolicy::PrintHost); // printer: keep accepting self-signed certificates
        http.timeout_connect(4)
            .timeout_max(8)
            .header("Content-Type", "application/json")
            .set_post_body(std::string("{\"serialNumber\":\"\",\"checkCode\":\"\"}"))
            .on_error([reply](std::string, std::string, unsigned) { reply(""); })
            .on_complete([reply](std::string body, unsigned) {
                std::string url;
                try {
                    nlohmann::json j = nlohmann::json::parse(body);
                    if (j.contains("detail") && j["detail"].is_object() && j["detail"].contains("cameraStreamUrl"))
                        url = j["detail"]["cameraStreamUrl"].get<std::string>();
                    else if (j.contains("cameraStreamUrl"))
                        url = j["cameraStreamUrl"].get<std::string>();
                } catch (...) {}
                reply(url);
            })
            .perform();
    }
}

} // namespace GUI
} // namespace Slic3r
