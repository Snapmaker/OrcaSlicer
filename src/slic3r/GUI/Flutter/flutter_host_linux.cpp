// Linux implementation: FlutterEngineHost + FlutterViewHost
// Uses Flutter Linux desktop embedding GObject API (flutter_linux.h)
#include <flutter_linux/flutter_linux.h>
#include "flutter_host.h"
#include <string>
#include <cstring>

// ── FlutterViewHostLinux ──────────────────────────────────────────────

class FlutterViewHostLinux : public FlutterViewHost {
    FlView*            m_view = nullptr;
    FlEngine*          m_engine = nullptr;
    FlBinaryMessenger* m_messenger = nullptr;
    std::string        m_channel;
    MethodCallHandler  m_handler;

    static void messageHandler(FlBinaryMessenger* messenger,
                               const gchar* channel,
                               GBytes* message,
                               FlBinaryMessengerResponseHandle* response_handle,
                               gpointer user_data) {
        auto* self = static_cast<FlutterViewHostLinux*>(user_data);
        if (!self->m_handler) return;

        gsize data_size = 0;
        const guint8* data = message
            ? static_cast<const guint8*>(g_bytes_get_data(message, &data_size))
            : nullptr;

        std::string args;
        if (data && data_size > 0)
            args = std::string(reinterpret_cast<const char*>(data), data_size);

        // Take a ref on the response handle to use in the reply lambda
        g_object_ref(response_handle);

        Reply reply = [self, response_handle](const std::string& result) {
            g_autoptr(GBytes) bytes = g_bytes_new(result.data(), result.size());
            g_autoptr(GError) error = nullptr;
            fl_binary_messenger_send_response(
                self->m_messenger, response_handle, bytes, &error);
            g_object_unref(response_handle);
        };

        self->m_handler(channel ? channel : "", args, reply);
    }

public:
    FlutterViewHostLinux(FlView* view, FlEngine* engine,
                         FlBinaryMessenger* messenger,
                         const std::string& channelName)
        : m_view(view), m_engine(engine),
          m_messenger(messenger), m_channel(channelName) {

        fl_binary_messenger_set_message_handler_on_channel(
            m_messenger, m_channel.c_str(), messageHandler, this, nullptr);
    }

    ~FlutterViewHostLinux() override {
        if (m_messenger) {
            fl_binary_messenger_set_message_handler_on_channel(
                m_messenger, m_channel.c_str(), nullptr, nullptr, nullptr);
        }
        g_clear_object(&m_view);
        g_clear_object(&m_engine);
    }

    void embedInto(void* parentHandle) override {
        GtkWidget* parent = GTK_WIDGET(parentHandle);
        if (!parent || !m_view) return;

        gtk_widget_set_size_request(GTK_WIDGET(m_view),
                                     gtk_widget_get_allocated_width(parent),
                                     gtk_widget_get_allocated_height(parent));
        gtk_container_add(GTK_CONTAINER(parent), GTK_WIDGET(m_view));
        gtk_widget_show_all(GTK_WIDGET(m_view));
    }

    void invokeMethod(const std::string& method,
                      const std::string& arguments) override {
        if (!m_messenger) return;

        g_autoptr(GBytes) message = g_bytes_new(arguments.data(),
                                                 arguments.size());
        fl_binary_messenger_send_on_channel(
            m_messenger, m_channel.c_str(), message,
            nullptr, nullptr, nullptr);
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

        g_autoptr(FlDartProject) project = fl_dart_project_new();
        if (!project) return nullptr;

        FlEngine* engine = fl_engine_new(project);
        if (!engine) return nullptr;

        FlView* view = fl_view_new_for_engine(engine);
        if (!view) {
            g_object_unref(engine);
            return nullptr;
        }

        FlBinaryMessenger* messenger = fl_engine_get_binary_messenger(engine);

        auto* host = new FlutterViewHostLinux(view, engine, messenger, channelName);
        return std::unique_ptr<FlutterViewHost>(host);
    }
};

// ── Factory ────────────────────────────────────────────────────────────

std::unique_ptr<FlutterEngineHost> createFlutterEngine(
    const std::string&, const std::string&) {
    return std::make_unique<FlutterEngineHostLinux>();
}
