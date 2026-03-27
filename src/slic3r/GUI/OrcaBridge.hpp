#pragma once

#include <wx/webview.h>
#include <functional>
#include <string>
#include <map>
#include "nlohmann/json.hpp"

namespace Slic3r {
namespace GUI {

using json = nlohmann::json;

using BridgeHandler = std::function<void(
    const json& data,
    std::function<void(const json&)> reply,
    std::function<void(const std::string&)> fail)>;

class OrcaBridge {
public:
    static void bind(const std::string& cmd, BridgeHandler handler);

    static void dispatch(const std::string& message, wxWebView* webview);

    static void push(wxWebView* webview, const std::string& cmd, const json& data);

private:
    static void sendToJS(wxWebView* wv, const json& msg);
    static std::map<std::string, BridgeHandler> s_handlers;
};

} // namespace GUI
} // namespace Slic3r
