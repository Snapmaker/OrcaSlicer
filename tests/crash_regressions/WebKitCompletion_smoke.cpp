#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <cstdio>

static bool passed = false;

static void fixed_script_finished(GObject *source, GAsyncResult *result, gpointer)
{
    GError *error = nullptr;
    auto *value = webkit_web_view_run_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
    passed = value && !error;
    std::printf("Discarded completion value: %s\n", passed ? "PASS" : "FAIL");
    if (error) {
        std::printf("%s\n", error->message);
        g_error_free(error);
    }
    if (value)
        webkit_javascript_result_unref(value);
    gtk_main_quit();
}

static void original_script_finished(GObject *source, GAsyncResult *result, gpointer)
{
    GError *error = nullptr;
    auto *value = webkit_web_view_run_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
    std::printf("Original native-object completion: %s\n", error ? error->message : "returned a value");
    if (error)
        g_error_free(error);
    if (value)
        webkit_javascript_result_unref(value);
    webkit_web_view_run_javascript(WEBKIT_WEB_VIEW(source),
                                  "window.crashTest = window.webkit.messageHandlers.crashTest;\n;void 0;",
                                  nullptr, fixed_script_finished, nullptr);
}

static void loaded(WebKitWebView *view, WebKitLoadEvent event, gpointer)
{
    if (event == WEBKIT_LOAD_FINISHED)
        webkit_web_view_run_javascript(view, "window.crashTest = window.webkit.messageHandlers.crashTest;",
                                      nullptr, original_script_finished, nullptr);
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    auto *view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    webkit_user_content_manager_register_script_message_handler(webkit_web_view_get_user_content_manager(view), "crashTest");
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(view));
    gtk_window_set_default_size(GTK_WINDOW(window), 320, 200);
    g_signal_connect(view, "load-changed", G_CALLBACK(loaded), nullptr);
    g_timeout_add_seconds(15, [](gpointer) -> gboolean { gtk_main_quit(); return G_SOURCE_REMOVE; }, nullptr);
    webkit_web_view_load_html(view, "<html><body>WebKit script completion regression check</body></html>", nullptr);
    gtk_widget_show_all(window);
    gtk_main();
    gtk_widget_destroy(window);
    return passed ? 0 : 1;
}
