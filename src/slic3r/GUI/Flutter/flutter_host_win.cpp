// Windows implementation: FlutterEngineHost + FlutterViewHost
// Uses Flutter Windows desktop embedding C API (flutter_windows.h)
#include <windows.h>
#include <flutter_windows.h>
#include "flutter_host.h"
#include <string>
#include <vector>
#include <cstring>

// ── Helpers ───────────────────────────────────────────────────────────

static std::string messageToString(const uint8_t* data, size_t size) {
    if (!data || size == 0) return "";
    return std::string(reinterpret_cast<const char*>(data), size);
}

// ── FlutterViewHostWin ─────────────────────────────────────────────────

class FlutterViewHostWin : public FlutterViewHost {
    FlutterDesktopViewControllerRef m_controller = nullptr;
    FlutterDesktopEngineRef        m_engine = nullptr;
    std::string                    m_channel;
    MethodCallHandler              m_handler;
    FlutterDesktopMessengerRef     m_messenger = nullptr;

    static void messageCallback(FlutterDesktopMessengerRef messenger,
                                 const FlutterDesktopMessage* msg,
                                 void* userData) {
        auto* self = static_cast<FlutterViewHostWin*>(userData);
        if (!self->m_handler || !msg) return;

        std::string method = msg->channel ? msg->channel : "";
        std::string args = messageToString(msg->message, msg->message_size);

        Reply reply = [messenger, response_handle = msg->response_handle](const std::string& result) {
            if (messenger && response_handle) {
                FlutterDesktopMessengerSendResponse(
                    messenger,
                    response_handle,
                    reinterpret_cast<const uint8_t*>(result.data()),
                    result.size());
            }
        };

        self->m_handler(method, args, reply);
    }

public:
    FlutterViewHostWin(FlutterDesktopViewControllerRef ctrl,
                       FlutterDesktopEngineRef engine,
                       const std::string& channelName)
        : m_controller(ctrl), m_engine(engine), m_channel(channelName) {

        m_messenger = FlutterDesktopEngineGetMessenger(engine);
        if (m_messenger) {
            FlutterDesktopMessengerSetCallback(
                m_messenger, m_channel.c_str(), messageCallback, this);
        }
    }

    ~FlutterViewHostWin() override {
        if (m_messenger) {
            FlutterDesktopMessengerSetCallback(
                m_messenger, m_channel.c_str(), nullptr, nullptr);
        }
        if (m_controller) {
            FlutterDesktopViewControllerDestroy(m_controller);
        }
    }

    void resize(int, int) override {}

    void embedInto(void* parentView) override {
        FlutterDesktopViewRef view =
            FlutterDesktopViewControllerGetView(m_controller);
        HWND childHwnd = FlutterDesktopViewGetHWND(view);
        HWND parentHwnd = static_cast<HWND>(parentView);

        if (!childHwnd || !parentHwnd) return;

        SetParent(childHwnd, parentHwnd);

        RECT rect;
        GetClientRect(parentHwnd, &rect);
        SetWindowPos(childHwnd, nullptr,
                     0, 0, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOZORDER | SWP_SHOWWINDOW);
    }

    void invokeMethod(const std::string& method,
                      const std::string& arguments) override {
        if (!m_messenger) return;
        FlutterDesktopMessengerSend(
            m_messenger, m_channel.c_str(),
            reinterpret_cast<const uint8_t*>(arguments.data()),
            arguments.size());
    }

    void setMethodCallHandler(MethodCallHandler h) override {
        m_handler = std::move(h);
    }
};

// ── FlutterEngineHostWin ───────────────────────────────────────────────

class FlutterEngineHostWin : public FlutterEngineHost {
    bool m_started = false;

public:
    ~FlutterEngineHostWin() override { stop(); }

    bool start() override {
        // On Windows, the engine is created per-view (not one shared engine).
        // This is a no-op at the process level.
        m_started = true;
        return true;
    }

    void stop() override {
        m_started = false;
    }

    std::unique_ptr<FlutterViewHost> createView(
        const std::string& entrypoint,
        const std::string& channelName) override {

        FlutterDesktopEngineProperties props = {};
        props.assets_path = L"data\\flutter_assets";
        props.icu_data_path = L"data\\icudtl.dat";
        props.aot_library_path = L"flutter_app.dll";

        FlutterDesktopEngineRef engine =
            FlutterDesktopEngineCreate(&props);
        if (!engine) return nullptr;

        const char* ep = entrypoint.empty() ? nullptr : entrypoint.c_str();
        if (!FlutterDesktopEngineRun(engine, ep)) {
            FlutterDesktopEngineDestroy(engine);
            return nullptr;
        }

        // Create view controller bound to this engine
        FlutterDesktopViewControllerRef controller =
            FlutterDesktopViewControllerCreate(800, 600, engine);
        if (!controller) {
            FlutterDesktopEngineDestroy(engine);
            return nullptr;
        }

        auto* view = new FlutterViewHostWin(controller, engine, channelName);
        return std::unique_ptr<FlutterViewHost>(view);
    }
};

// ── Factory ────────────────────────────────────────────────────────────

std::unique_ptr<FlutterEngineHost> createFlutterEngine(
    const std::string&, const std::string&) {
    return std::make_unique<FlutterEngineHostWin>();
}
