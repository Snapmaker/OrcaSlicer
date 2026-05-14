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
    void onShow(wxShowEvent& event);
    void onSetFocus(wxFocusEvent& event);

    // Override SetFocus to transfer keyboard focus to the Flutter child
    // HWND immediately, rather than relying on the CallAfter deferral in
    // onSetFocus (which races with wxWidgets' internal focus bookkeeping).
    // Keeping onSetFocus + CallAfter as a fallback for non-SetFocus paths
    // (keyboard navigation, WM_SETFOCUS from the OS).
    virtual void SetFocus() override;

    // Panel is a transparent wrapper; only the Flutter child accepts focus.
    // Prevents wxWidgets from reclaiming focus via mouse-click auto-focus
    // (MSWWindowProc calls SetFocus on WM_LBUTTONDOWN for IsFocusable windows).
    bool AcceptsFocus() const override { return false; }

    // Attempt deferred embed if it hasn't happened yet.
    void tryEmbed();

protected:
    std::unique_ptr<FlutterViewHost> m_view;
    bool m_embedded = false;   // true after the first successful embedInto

#ifdef __WXMSW__
    virtual WXHWND MSWGetFocusHWND() const override;
    virtual bool ContainsHWND(WXHWND hWnd) const override;
    virtual WXLRESULT MSWWindowProc(WXUINT msg, WXWPARAM wParam, WXLPARAM lParam) override;
#endif
};
