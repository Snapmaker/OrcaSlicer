#include "OrcaBridge.hpp"
#include "Widgets/WebView.hpp"
#include "GUI_App.hpp"
#include <boost/log/trivial.hpp>
#include <wx/filename.h>

namespace Slic3r {
namespace GUI {

std::map<std::string, BridgeHandler> OrcaBridge::s_handlers;

void OrcaBridge::bind(const std::string& cmd, BridgeHandler handler)
{
    s_handlers[cmd] = std::move(handler);
}

void OrcaBridge::dispatch(const std::string& message, wxWebView* webview)
{
    try {
        auto j = json::parse(message);
        auto cmd = j.value("cmd", "");
        auto id  = j.value("id", "");
        auto data = j.value("data", json::object());

        BOOST_LOG_TRIVIAL(info) << "OrcaBridge: cmd=" << cmd << " id=" << id;

        auto it = s_handlers.find(cmd);
        if (it == s_handlers.end()) {
            if (!id.empty()) {
                json resp;
                resp["id"] = id;
                resp["error"] = "unknown command: " + cmd;
                sendToJS(webview, resp);
            }
            return;
        }

        // reply callback
        auto reply = [webview, id](const json& result) {
            if (id.empty()) return;
            json resp;
            resp["id"] = id;
            resp["data"] = result;
            wxGetApp().CallAfter([webview, resp]() {
                sendToJS(webview, resp);
            });
        };

        // fail callback
        auto fail = [webview, id](const std::string& err) {
            if (id.empty()) return;
            json resp;
            resp["id"] = id;
            resp["error"] = err;
            wxGetApp().CallAfter([webview, resp]() {
                sendToJS(webview, resp);
            });
        };

        it->second(data, reply, fail);

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaBridge::dispatch error: " << e.what();
    }
}

void OrcaBridge::push(wxWebView* webview, const std::string& cmd, const json& data)
{
    json msg;
    msg["cmd"] = cmd;
    msg["data"] = data;
    sendToJS(webview, msg);
}

void OrcaBridge::sendToJS(wxWebView* wv, const json& msg)
{
    if (!wv || !wv->GetRefData()) return;
    std::string js = "window.postMessage(JSON.stringify(" + msg.dump() + "), '*');";
    WebView::RunScript(wv, js);
}

} // namespace GUI
} // namespace Slic3r
