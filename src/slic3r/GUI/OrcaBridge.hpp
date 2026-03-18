#pragma once

#include <wx/webview.h>
#include <functional>
#include <string>
#include <map>
#include "nlohmann/json.hpp"

namespace Slic3r {
namespace GUI {

using json = nlohmann::json;

/// Handler 签名: (data, reply, fail)
/// - data:  请求参数
/// - reply: 调用 reply(result_json) 返回成功
/// - fail:  调用 fail("error msg") 返回失败
using BridgeHandler = std::function<void(
    const json& data,
    std::function<void(const json&)> reply,
    std::function<void(const std::string&)> fail)>;

class OrcaBridge {
public:
    /// 注册命令: OrcaBridge::bind("ping", handler);
    static void bind(const std::string& cmd, BridgeHandler handler);

    /// WebView 收到 JS 消息时调用
    static void dispatch(const std::string& message, wxWebView* webview);

    /// C++ 主动推送事件给 JS
    static void push(wxWebView* webview, const std::string& cmd, const json& data);

private:
    static void sendToJS(wxWebView* wv, const json& msg);
    static std::map<std::string, BridgeHandler> s_handlers;
};

} // namespace GUI
} // namespace Slic3r
