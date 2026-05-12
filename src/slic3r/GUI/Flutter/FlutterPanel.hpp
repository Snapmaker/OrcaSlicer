#pragma once
#include <wx/wx.h>
#include <memory>
#include <functional>
#include <unordered_map>
#include "flutter_host.h"

// Simple dispatch table for Dart↔C++ MethodChannel communication
class Dispatcher {
public:
    using Fn = std::function<void(
        const std::string& args, FlutterViewHost::Reply reply)>;

    Dispatcher& on(const std::string& method, Fn fn) {
        m_map[method] = std::move(fn);
        return *this;
    }

    FlutterViewHost::MethodCallHandler handler() {
        auto map = m_map;
        return [map](const std::string& method,
                     const std::string& args,
                     FlutterViewHost::Reply reply) {
            auto it = map.find(method);
            if (it != map.end()) {
                try {
                    it->second(args, reply);
                } catch (const std::exception& e) {
                    reply(std::string("Error: ") + e.what());
                } catch (...) {
                    reply("Error: unknown");
                }
            } else {
                reply("");
            }
        };
    }

private:
    std::unordered_map<std::string, Fn> m_map;
};

class FlutterPanel : public wxWindow {
public:
    FlutterPanel(wxWindow* parent);
    bool startView(FlutterEngineHost* engine,
                   const std::string& entrypoint,
                   const std::string& channelName,
                   FlutterViewHost::MethodCallHandler handler = {});
    FlutterViewHost* view() { return m_view.get(); }
    void setHandler(FlutterViewHost::MethodCallHandler handler);

    void onSize(wxSizeEvent& event);
    void onSetFocus(wxFocusEvent& event);

protected:
    std::unique_ptr<FlutterViewHost> m_view;
};
