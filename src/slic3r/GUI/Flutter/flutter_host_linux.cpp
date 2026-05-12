// Linux implementation: FlutterEngineHost + FlutterViewHost
// Uses Flutter Linux desktop embedding GObject API (flutter_linux.h)
#include <flutter_linux/flutter_linux.h>
#include "flutter_host.h"
#include "wx/gtk/private/win_gtk.h"
#include <cstdlib>
#include <string>
#include <cstring>

// ── Helpers ───────────────────────────────────────────────────────────

static std::string flValueToString(FlValue* v) {
    if (!v) return "";
    switch (fl_value_get_type(v)) {
        case FL_VALUE_TYPE_STRING: return fl_value_get_string(v);
        case FL_VALUE_TYPE_INT:    return std::to_string(fl_value_get_int(v));
        case FL_VALUE_TYPE_FLOAT:  return std::to_string(fl_value_get_float(v));
        case FL_VALUE_TYPE_BOOL:   return fl_value_get_bool(v) ? "true" : "false";
        case FL_VALUE_TYPE_NULL:   return "";
        default:                   return "";
    }
}

// ── FlutterViewHostLinux ──────────────────────────────────────────────

class FlutterViewHostLinux : public FlutterViewHost {
    FlView*            m_view = nullptr;
    FlEngine*          m_engine = nullptr;
    FlMethodChannel*   m_channel = nullptr;
    MethodCallHandler  m_handler;

    static void methodCallHandler(FlMethodChannel* /*channel*/,
                                  FlMethodCall* method_call,
                                  gpointer user_data) {
        auto* self = static_cast<FlutterViewHostLinux*>(user_data);
        if (!self->m_handler) {
            fl_method_call_respond_not_implemented(method_call, nullptr);
            return;
        }

        const gchar* method = fl_method_call_get_name(method_call);
        FlValue* args_fl = fl_method_call_get_args(method_call);
        std::string args = flValueToString(args_fl);

        g_object_ref(method_call);
        auto replied = std::make_shared<bool>(false);

        Reply reply = [method_call, replied](const std::string& result) {
            if (*replied) return;
            *replied = true;

            g_autoptr(GError) error = nullptr;
            g_autoptr(FlValue) val = nullptr;

            // Auto-detect integer strings so Dart invokeMethod<int> works
            if (!result.empty()) {
                char* end = nullptr;
                long long n = std::strtoll(result.c_str(), &end, 10);
                if (end && *end == '\0' && n >= INT_MIN && n <= INT_MAX)
                    val = fl_value_new_int(static_cast<int>(n));
            }
            if (!val) val = fl_value_new_string(result.c_str());

            fl_method_call_respond_success(method_call, val, &error);
            g_object_unref(method_call);
        };

        try {
            self->m_handler(method ? method : "", args, reply);
            if (!*replied) {
                g_object_unref(method_call);
            }
        } catch (const std::exception& e) {
            if (!*replied) {
                *replied = true;
                g_autoptr(GError) error = nullptr;
                fl_method_call_respond_error(method_call, "EXCEPTION", e.what(), nullptr, &error);
                g_object_unref(method_call);
            }
        } catch (...) {
            if (!*replied) {
                *replied = true;
                g_autoptr(GError) error = nullptr;
                fl_method_call_respond_error(method_call, "EXCEPTION", "unknown error", nullptr, &error);
                g_object_unref(method_call);
            }
        }
    }

public:
    FlutterViewHostLinux(FlView* view, FlEngine* engine,
                         FlBinaryMessenger* messenger,
                         const std::string& channelName)
        : m_view(view), m_engine(engine) {

        g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
        m_channel = fl_method_channel_new(
            messenger, channelName.c_str(), FL_METHOD_CODEC(codec));
        fl_method_channel_set_method_call_handler(
            m_channel, methodCallHandler, this, nullptr);
    }

    ~FlutterViewHostLinux() override {
        fl_method_channel_set_method_call_handler(
            m_channel, nullptr, nullptr, nullptr);
        g_clear_object(&m_channel);
        g_clear_object(&m_engine);
        g_clear_object(&m_view);
    }

    void embedInto(void* parentHandle) override {
        GtkWidget* parent = GTK_WIDGET(parentHandle);
        if (!parent || !m_view) return;

        GtkAllocation alloc;
        gtk_widget_get_allocation(parent, &alloc);
        int w = alloc.width > 1 ? alloc.width : 800;
        int h = alloc.height > 1 ? alloc.height : 600;

        if (!WX_IS_PIZZA(parent)) return;

        GtkWidget* view_widget = GTK_WIDGET(m_view);

        // Wrap in GtkEventBox so the FlView participates in GTK's focus chain.
        // GtkFixed (wxPizza's parent class) does not propagate focus to children.
        GtkWidget* event_box = gtk_event_box_new();
        gtk_container_add(GTK_CONTAINER(event_box), view_widget);
        gtk_widget_set_can_focus(event_box, TRUE);
        gtk_widget_set_can_focus(view_widget, TRUE);

        wxPizza* pizza = WX_PIZZA(parent);
        pizza->put(event_box, 0, 0, w, h);

        gtk_widget_show_all(event_box);
        if (gtk_widget_get_realized(parent))
            gtk_widget_realize(view_widget);
        gtk_widget_grab_focus(view_widget);
    }

    void resize(int width, int height) override {
        if (!m_view) return;
        GtkWidget* widget = GTK_WIDGET(m_view);
        // m_view is inside a GtkEventBox inside the pizza — walk up to pizza
        GtkWidget* eb = gtk_widget_get_parent(widget);
        if (!eb || !GTK_IS_EVENT_BOX(eb)) return;
        GtkWidget* parent = gtk_widget_get_parent(eb);
        if (!parent || !WX_IS_PIZZA(parent)) return;
        if (width <= 0 || height <= 0) return;

        wxPizza* pizza = WX_PIZZA(parent);
        pizza->move(eb, 0, 0, width, height);
        gtk_widget_set_size_request(widget, width, height);
        if (gtk_widget_get_visible(eb)) {
            pizza->size_allocate_child(eb, 0, 0, width, height);
        }
    }

    void invokeMethod(const std::string& method,
                      const std::string& arguments) override {
        if (!m_channel) return;
        g_autoptr(FlValue) args = fl_value_new_string(arguments.c_str());
        fl_method_channel_invoke_method(
            m_channel, method.c_str(), args,
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

        setenv("FLUTTER_LINUX_RENDERER", "software", 1);

        g_autoptr(FlDartProject) project = fl_dart_project_new();
        if (!project) return nullptr;

        // Override AOT library name: Flutter defaults to lib/libapp.so,
        // but our build system names it libflutter_app.so.
        {
            g_autofree gchar* exe_path = g_file_read_link("/proc/self/exe", nullptr);
            if (exe_path) {
                g_autofree gchar* exe_dir = g_path_get_dirname(exe_path);
                g_autofree gchar* aot_path = g_build_filename(exe_dir, "lib", "libflutter_app.so", nullptr);
                fl_dart_project_set_aot_library_path(project, aot_path);
            }
        }

        // Override icudtl.dat path when present.
        {
            g_autofree gchar* exe_path = g_file_read_link("/proc/self/exe", nullptr);
            if (exe_path) {
                g_autofree gchar* exe_dir = g_path_get_dirname(exe_path);
                g_autofree gchar* icu_path = g_build_filename(exe_dir, "data", "icudtl.dat", nullptr);
                if (g_file_test(icu_path, G_FILE_TEST_EXISTS)) {
                    fl_dart_project_set_icu_data_path(project, icu_path);
                }
            }
        }

        FlView* view = fl_view_new(project);
        if (!view) return nullptr;

        FlEngine* engine = fl_view_get_engine(view);
        g_object_ref(engine);
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
