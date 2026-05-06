// Linux implementation: FlutterEngineHost + FlutterViewHost
// Uses Flutter Linux desktop embedding C API (flutter_linux.h)
#include <flutter_linux.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include "flutter_host.h"
#include <string>
#include <vector>
#include <cstring>

// ── Helpers ───────────────────────────────────────────────────────────

static std::string messageToString(const uint8_t* data, size_t size) {
    if (!data || size == 0) return "";
    return std::string(reinterpret_cast<const char*>(data), size);
}

// ── FlutterViewHostLinux ──────────────────────────────────────────────

class FlutterViewHostLinux : public FlutterViewHost {
    FlutterDesktopViewRef          m_view = nullptr;
    FlutterDesktopEngineRef        m_engine = nullptr;
    std::string                    m_channel;
    MethodCallHandler              m_handler;
    FlutterDesktopMessengerRef     m_messenger = nullptr;

    static void messageCallback(const FlutterDesktopMessage* msg, void* userData) {
        auto* self = static_cast<FlutterViewHostLinux*>(userData);
        if (!self->m_handler || !msg) return;

        std::string method = msg->channel ? msg->channel : "";
        std::string args = messageToString(msg->message, msg->message_size);

        FlutterDesktopMessage responseHandle = *msg;

        Reply reply = [self, responseHandle](const std::string& result) {
            if (self->m_messenger) {
                FlutterDesktopMessengerSendResponse(
                    self->m_messenger,
                    &responseHandle,
                    reinterpret_cast<const uint8_t*>(result.data()),
                    result.size());
            }
        };

        self->m_handler(method, args, reply);
    }

public:
    FlutterViewHostLinux(FlutterDesktopViewRef view,
                         FlutterDesktopEngineRef engine,
                         const std::string& channelName)
        : m_view(view), m_engine(engine), m_channel(channelName) {

        m_messenger = FlutterDesktopEngineGetMessenger(engine);
        if (m_messenger) {
            FlutterDesktopMessengerSetCallback(
                m_messenger, m_channel.c_str(), messageCallback, this);
        }
    }

    ~FlutterViewHostLinux() override {
        if (m_messenger) {
            FlutterDesktopMessengerSetCallback(
                m_messenger, m_channel.c_str(), nullptr, nullptr);
        }
        if (m_view) {
            FlutterDesktopViewDestroy(m_view);
        }
    }

    void embedInto(void* parentHandle) override {
        if (!m_view) return;

        // parentHandle is wxWindow::GetHandle() → GtkWidget*
        GtkWidget* parentWidget = static_cast<GtkWidget*>(parentHandle);
        if (!parentWidget) return;

        GdkWindow* parentGdkWindow = gtk_widget_get_window(parentWidget);
        if (!parentGdkWindow) return;

        Display* display = gdk_x11_display_get_xdisplay(
            gdk_window_get_display(parentGdkWindow));
        Window parentXid = gdk_x11_window_get_xid(parentGdkWindow);
        Window childXid = FlutterDesktopViewGetId(m_view);

        // Reparent Flutter's X11 window into wxWidgets window
        XReparentWindow(display, childXid, parentXid, 0, 0);

        // Resize to fill parent
        int w = gdk_window_get_width(parentGdkWindow);
        int h = gdk_window_get_height(parentGdkWindow);
        XResizeWindow(display, childXid, w, h);
        XMapWindow(display, childXid);
        XFlush(display);
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

// ── FlutterEngineHostLinux ────────────────────────────────────────────

class FlutterEngineHostLinux : public FlutterEngineHost {
    bool m_started = false;

public:
    ~FlutterEngineHostLinux() override { stop(); }

    bool start() override {
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
        props.assets_path = nullptr;
        props.icu_data_path = nullptr;

        FlutterDesktopEngineRef engine =
            FlutterDesktopEngineCreate(&props);
        if (!engine) return nullptr;

        const char* ep = entrypoint.empty() ? nullptr : entrypoint.c_str();
        if (!FlutterDesktopEngineRun(engine, ep)) {
            FlutterDesktopEngineDestroy(engine);
            return nullptr;
        }

        // Create view and connect to engine
        FlutterDesktopViewRef view = FlutterDesktopViewCreate(800, 600, engine);
        if (!view) {
            FlutterDesktopEngineDestroy(engine);
            return nullptr;
        }

        auto* hostView = new FlutterViewHostLinux(view, engine, channelName);
        return std::unique_ptr<FlutterViewHost>(hostView);
    }
};

// ── Factory ────────────────────────────────────────────────────────────

std::unique_ptr<FlutterEngineHost> createFlutterEngine(
    const std::string&, const std::string&) {
    return std::make_unique<FlutterEngineHostLinux>();
}
