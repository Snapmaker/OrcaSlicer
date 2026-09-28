#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/MainThreadGate.hpp"

namespace Slic3r {
namespace GUI {

// This slicer instance's JSON API, on a loopback-only port picked by the OS.
//
// Phones and agents never talk to it directly: the hub process (RemoteHub.hpp) owns the
// token-gated LAN listener and proxies /r/<token>/i/<pid>/api/... here as /api/... .
// The instance announces itself by dropping <datadir>/hub/instances/<pid>.json (pid,
// port, start time); the hub lists those, probes GET /api/info and removes dead ones.
//
// Requests run on the GUI thread (Plater, plates, presets and devices are GUI-thread
// only); confirmation dialogs raised meanwhile answer Yes on their own (auto_confirm).
class RemoteAccess
{
public:
    static RemoteAccess& get();

    // Idempotent; call from the main thread once the Plater exists.
    void start();
    void stop(); // removes the instance file; the listener dies with the process
    int  port();

    // ---- handing work to the GUI thread (MainThreadGate.hpp) ----
    // Every request, send, control and connect thread in the Remote* modules reaches the GUI thread
    // through these two, never through a bare wxApp::CallAfter: once the main window starts closing
    // the gate is shut, queued work that has not started is dropped and nothing new is queued, so no
    // request can run against a Plater that is being (or has been) destroyed.
    static MainCallResult call_on_main(std::function<void()> fn, int timeout_ms); // wait for it
    static bool           post_to_main(std::function<void()> fn);                 // fire and forget
    // For UI code that guards its own lifetime (a weak_ptr to the view): only the app quitting
    // stops it, not a language switch's rebuild - a rebuilt view must still hear back.
    static bool           post_to_app(std::function<void()> fn);
    // MainFrame::shutdown, first thing (both a quit and a language switch's rebuild).
    static void close_gui_gate(bool quitting);
    // GUI_App::recreate_GUI, once the rebuilt main window and Plater exist.
    static void reopen_gui_gate();
    static bool gui_closing();

    // Ultra: window visibility, mirrored into <pid>.json and /api/info so the hub can list
    // and toggle it. GUI thread (or before start()).
    void set_hidden(bool hidden);
    bool hidden();

    // ---- dialog policy + attention (hidden service mode, stage 3) ----
    // Interactive: a person is at the PC (window shown), dialogs show normally.
    // Request:     a phone/agent request is running -> take the affirmative branch.
    // Background:  hidden and nothing asked for this -> take the do-nothing branch.
    enum class Mode { Interactive, Request, Background };
    static Mode dialog_mode();
    // Installed once, before the GUI_App exists (GUI_Init.cpp): a wxModalDialogHook that
    // answers every ShowModal() itself while the instance is hidden or serving a request.
    static void install_dialog_policy();
    // Bring the main window up because something needs a person (any thread).
    static void show_window(const std::string& reason);

    struct Attention { long long time; std::string dialog; std::string answered; };
    void      note_attention(const std::string& dialog, const std::string& answered); // any thread
    // kind: "dialog" (clears when the modal goes), "timeout" (clears after the next completed
    // request), "manual" (clears from the phone, or when the window is hidden again).
    void      raise_attention(const std::string& reason, const char* kind = "manual");
    void      clear_attention(const char* why);
    bool      needs_attention(std::string* reason = nullptr);
    void      note_gui_tick(int modal_depth); // GUI thread heartbeat
    long long gui_stall_ms();
    void      note_request_done();
    void      heartbeat_review(int modal_depth); // the heartbeat's decisions

    // What the hub shows in its instance list (GUI thread, from the Plater's title code).
    void note_project(const std::string& title, const std::string& path);
    // An error the GUI wanted to show while a request ran (show_error); the request reports it.
    void        note_error(const std::string& message);
    std::string take_error();
    // Ultra: what the last headless colour import did, so api_project_open can report it.
    struct ColorImport { size_t input{0}, clusters{0}, reused{0}, added{0}, merged{0}; bool valid{false}; };
    void        note_color_import(const ColorImport& c);
    ColorImport take_color_import();

    // True while a phone/agent request is being executed on the GUI thread. Confirmation
    // dialogs raised meanwhile (e.g. "adjust these settings automatically?") answer Yes on
    // their own instead of blocking a GUI nobody is looking at.
    static bool auto_confirm();
    struct AutoConfirmScope
    {
        AutoConfirmScope();
        ~AutoConfirmScope();
    };

    // Called by the Plater's slicing handlers (GUI thread) so that /api/jobs can report
    // progress of a slice started through the API.
    void note_slice_progress(int plate, int percent, const std::string& text);
    void note_slice_done(bool finished_all, bool ok, const std::string& error);

    // Send jobs (RemoteSend) report here from their worker thread.
    void update_job(int id, int percent, const std::string& text);
    void finish_job(int id, bool ok, const std::string& error, const nlohmann::json& result);

private:
    RemoteAccess() = default;
    void accept_loop();
    void serve(void* socket); // boost::asio tcp::socket*, owned by the call
    void write_instance_file();

    struct ApiResponse
    {
        int         status { 200 };
        std::string type { "application/json" };
        std::string body;
        std::string headers; // extra "Name: value\r\n" lines
    };
    ApiResponse handle_api(const std::string& method, const std::string& path, const std::string& query, const std::string& body);
    ApiResponse api_info();
    ApiResponse api_window(const std::string& show); // "" = query only, "1"/"0" = set
    ApiResponse api_quit(bool discard);
    ApiResponse api_attention_clear();
    ApiResponse api_debug(const std::string& what, const std::string& query, const std::string& body);
    ApiResponse api_events(int since);
    ApiResponse api_project_open(const std::string& path, const std::string& mode);
    ApiResponse api_plates();
    ApiResponse api_plate_thumbnail(int plate);
    ApiResponse api_plate_layout(int plate);
    ApiResponse api_plate_preview(int plate);
    ApiResponse api_plate_preview_png(int plate, const std::string& view, int layer, int w, int h, double zoom, double cx, double cy);
    ApiResponse api_plate_preview_status(int plate);
    ApiResponse api_object_transform(const std::string& form_body);
    ApiResponse api_printers(int plate = -1);
    ApiResponse api_snapmaker_devices();
    ApiResponse api_snapmaker_add(const std::string& ip, int port);
    ApiResponse api_snapmaker_remove(const std::string& id);
    ApiResponse api_snapmaker_connect(const std::string& id);
    ApiResponse api_snapmaker_disconnect();
    ApiResponse api_slice(int plate, bool all);
    ApiResponse api_send(int plate, const std::string& form_body);
    ApiResponse api_printer_control(const std::string& printer, const std::string& form_body);
    ApiResponse api_jobs(int id);
    // Ultra: the G-code archive (GcodeArchive). Local paths never leave the instance.
    ApiResponse api_archive(const std::string& printer);
    ApiResponse api_archive_one(const std::string& id);
    ApiResponse api_archive_thumbnail(const std::string& id);
    ApiResponse api_archive_delete(const std::string& id);
    ApiResponse api_archive_send(const std::string& id, const std::string& form_body);
    ApiResponse api_archive_preview(const std::string& id, const std::string& form_body);
    ApiResponse api_presets();
    ApiResponse api_select_preset(const std::string& type, const std::string& name, int index);
    ApiResponse api_filament_color(int index, const std::string& color);
    ApiResponse api_filament_add();
    ApiResponse api_process_settings();
    ApiResponse api_process_set(const std::string& form_body);
    ApiResponse api_process_revert();
    ApiResponse api_process_save();

    struct Job
    {
        int         id { 0 };
        int         plate { -1 };       // -1 = all plates
        std::string kind { "slice" };   // slice | send | control
        std::string state;              // running | done | error | cancelled
        int         percent { 0 };
        std::string text, error;
        std::string printer, mode;      // send jobs: the printer id and upload | print
                                        // control jobs: the printer id and pause | resume | stop
        nlohmann::json result;          // what was (or, dry run, would have been) sent
    };

    std::mutex       m_mutex;
    bool             m_on { false };
    int              m_port { 0 };
    void*            m_acceptor { nullptr };
    std::vector<Job> m_jobs;
    int              m_next_job { 1 };
    bool             m_send_running { false }; // one transfer at a time, like the desktop's dialogs
    bool             m_control_running { false }; // and one pause/resume/stop at a time
    std::string      m_title, m_path, m_last_error;
    ColorImport      m_last_color_import;
    bool             m_slicing { false };
    bool             m_hidden { false };
    std::deque<Attention> m_attention; // ring of auto-answered dialogs, 50 entries
    bool             m_needs_attention { false };
    std::string      m_attention_reason, m_attention_kind;
    long long        m_attention_since { 0 };
    long long        m_gui_tick_ms { 0 };
    int              m_modal_depth { 0 };
    int              m_requests_done { 0 };
};

} // namespace GUI
} // namespace Slic3r
