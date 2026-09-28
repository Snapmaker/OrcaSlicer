#include "RemoteAccess.hpp"

#include "DeviceManager.hpp"
#include "GcodeArchive.hpp"
#include "GLCanvas3D.hpp"
#include "GLToolbar.hpp"
#include "GUI_App.hpp"
#include "HMS.hpp"
#include "MainFrame.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "OptionsGroup.hpp"
#include "PresetComboBoxes.hpp"
#include "RemoteControl.hpp"
#include "RemoteEvents.hpp"
#include "RemoteHub.hpp"
#include "RemoteSend.hpp"
#include "RemoteSnapmaker.hpp"
#include "SnapmakerLan.hpp"
#include "Selection.hpp"
#include "Tab.hpp"
#include "IMSlider.hpp"
#include <cmath>
#include <set>
#include <climits>
#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <chrono>
#include <ctime>
#include <future>
#include <memory>
#include <sstream>
#include <thread>

#include <wx/utils.h>
#include <wx/modalhook.h>
#include <boost/core/demangle.hpp>
#include <typeinfo>
#include <wx/msgdlg.h>
#include <wx/timer.h>
#include <wx/thread.h>

namespace Slic3r {
namespace GUI {

namespace asio = boost::asio;
namespace fs   = boost::filesystem;
using tcp      = asio::ip::tcp;

// ---------------------------------------------------------------- helpers ----

// A receive deadline on a blocking socket: a peer that stops talking mid-request fails the read
// instead of parking this connection's detached thread for ever.
static void set_read_timeout(tcp::socket& s, int seconds)
{
    if (!s.is_open()) return;
#ifdef _WIN32
    DWORD ms = (DWORD) seconds * 1000;
    ::setsockopt(s.native_handle(), SOL_SOCKET, SO_RCVTIMEO, (const char*) &ms, sizeof(ms));
#else
    struct timeval tv { seconds, 0 };
    ::setsockopt(s.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

static std::string percent_decode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char) s[i + 1]) && std::isxdigit((unsigned char) s[i + 2])) {
            out += (char) std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

static std::string query_param(const std::string& query, const std::string& key)
{
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string kv = query.substr(pos, amp - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key)
            return percent_decode(kv.substr(eq + 1));
        pos = amp + 1;
    }
    return "";
}

static void write_all(tcp::socket& s, const std::string& data)
{
    asio::write(s, asio::buffer(data));
}

static void respond(tcp::socket& s, const char* status, const std::string& type, const std::string& body,
                    const std::string& extra_headers = "")
{
    std::ostringstream o;
    o << "HTTP/1.1 " << status << "\r\n"
      << "Content-Type: " << type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: no-store\r\n"
      << extra_headers
      << "Connection: close\r\n\r\n"
      << body;
    write_all(s, o.str());
}

// Run fn on the GUI thread and wait for it (Plater, plates and devices are GUI-thread only).
// Returns false on timeout — e.g. a modal dialog is blocking the app — and fn may still run
// later, so callers only capture shared state.
static int s_auto_confirm_depth = 0; // GUI thread only
bool RemoteAccess::auto_confirm() { return s_auto_confirm_depth > 0; }
RemoteAccess::AutoConfirmScope::AutoConfirmScope() { ++s_auto_confirm_depth; }
RemoteAccess::AutoConfirmScope::~AutoConfirmScope() { --s_auto_confirm_depth; }

RemoteAccess::Mode RemoteAccess::dialog_mode()
{
    if (s_auto_confirm_depth > 0) return Mode::Request;
    if (RemoteAccess::get().hidden()) return Mode::Background;
    return Mode::Interactive;
}

static long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

namespace {

// Modals we let through (Interactive) that are up right now. GUI thread only.
int g_modal_depth = 0;

// Dialogs whose affirmative answer destroys work, needs typed input, or takes the instance
// down. Matched on the wx class name (wxClassInfo), so no translated text is involved.
struct ClassRule { const char* cls; int answer; bool human; };
static const ClassRule k_class_rules[] = {
    // needs typed input or a chosen path -> cancel and ask for a person
    { "wxFileDialog",            wxID_CANCEL, true }, { "wxDirDialog",           wxID_CANCEL, true },
    { "wxTextEntryDialog",       wxID_CANCEL, true }, { "wxColourDialog",        wxID_CANCEL, true },
    { "wxFontDialog",            wxID_CANCEL, true }, { "wxPasswordEntryDialog", wxID_CANCEL, true },
    { "SavePresetDialog",        wxID_CANCEL, true }, { "GuideFrame",            wxID_CANCEL, true },
    { "InputIpAddressDialog",    wxID_CANCEL, true }, { "PingCodeBindDialog",    wxID_CANCEL, true },
    { "BindMachineDialog",       wxID_CANCEL, true }, { "UnBindMachineDialog",   wxID_CANCEL, true },
    { "EditDevNameDialog",       wxID_CANCEL, true }, { "MsgDataIncompatible",   wxID_CANCEL, true },
    // destructive or takes the app down -> decline quietly
    { "DeleteConfirmDialog",     wxID_CANCEL, false }, { "DownloadDialog",       wxID_CANCEL, false },
    { "DownloadProgressDialog",  wxID_CANCEL, false }, { "UpdateVersionDialog",  wxID_CANCEL, false },
    { "UpdatePluginDialog",      wxID_CANCEL, false }, { "MsgUpdateConfig",      wxID_CANCEL, false },
    { "MsgUpdateSlic3r",         wxID_CANCEL, false },
    // print / calibration: never start a print unattended
    { "SelectMachineDialog",     wxID_CANCEL, false }, { "SendToPrinterDialog",  wxID_CANCEL, false },
    { "SendMultiMachinePage",    wxID_CANCEL, false }, { "ConfirmBeforeSendDialog", wxID_CANCEL, false },
    { "PrintHostSendDialog",     wxID_CANCEL, false }, { "CaliHistoryDialog",    wxID_CANCEL, false },
    // colour import (OBJ / glTF): its OK handler is what fills the filament ids, so a returned OK
    // without a click imports without colour anyway; say so instead of pretending to agree
    { "ObjColorDialog",          wxID_CANCEL, false },
};

static bool has_btn(wxDialog* d, int id) { return d->FindWindow(id) != nullptr; }

// The dialog's own class name. wx RTTI only knows classes declared with wxDECLARE_CLASS, which
// none of the fork's dialogs are: through it every one of them reports "wxDialog" and no rule
// above could ever match. The C++ type is always right; wx's name is the fallback for wx's own
// dialogs on toolchains whose demangler gives nothing usable.
static std::string dialog_class_name(wxDialog* dlg)
{
    std::string name = boost::core::demangle(typeid(*dlg).name()); // e.g. "class Slic3r::GUI::SelectMachineDialog"
    const size_t colons = name.rfind("::");
    if (colons != std::string::npos) name = name.substr(colons + 2);
    for (const char* prefix : { "class ", "struct " })
        if (name.compare(0, std::strlen(prefix), prefix) == 0) name = name.substr(std::strlen(prefix));
    while (!name.empty() && !(std::isalnum((unsigned char) name.back()) || name.back() == '_')) name.pop_back();
    if (name.empty() || !(std::isalpha((unsigned char) name[0]) || name[0] == '_'))
        name = dlg->GetClassInfo() ? std::string(wxString(dlg->GetClassInfo()->GetClassName()).ToUTF8().data()) : std::string("wxDialog");
    return name;
}

static long style_of(wxDialog* d)
{
    if (auto* m = dynamic_cast<wxMessageDialogBase*>(d)) return m->GetMessageDialogStyle();
    long s = 0; // the fork's MsgDialog builds real child buttons with these ids
    if (has_btn(d, wxID_OK))     s |= wxOK;
    if (has_btn(d, wxID_YES))    s |= wxYES;
    if (has_btn(d, wxID_NO))     s |= wxNO;
    if (has_btn(d, wxID_CANCEL)) s |= wxCANCEL;
    return s;
}

// affirmative: the phone asked for this, carry it out. Otherwise: do nothing.
static int default_answer(long s, bool affirmative)
{
    if (affirmative) {
        if (s & wxYES) return wxID_YES;
        if (s & wxOK)  return wxID_OK;
    } else {
        if (s & wxNO)     return wxID_NO;
        if (s & wxCANCEL) return wxID_CANCEL;
        if (s & wxOK)     return wxID_OK; // a pure acknowledgement
    }
    if (s & wxCANCEL) return wxID_CANCEL;
    if (s & wxNO)     return wxID_NO;
    if (s & wxOK)     return wxID_OK;
    return wxID_CANCEL;
}

static const char* answer_name(int id)
{
    switch (id) {
    case wxID_YES: return "yes";
    case wxID_NO:  return "no";
    case wxID_OK:  return "ok";
    default:       return "cancel";
    }
}

class DialogPolicyHook : public wxModalDialogHook
{
protected:
    int Enter(wxDialog* dlg) override
    {
        const RemoteAccess::Mode mode = RemoteAccess::dialog_mode();
        if (mode == RemoteAccess::Mode::Interactive) {
            ++g_modal_depth; // somebody is looking; let it show
            return wxID_NONE;
        }
        const std::string cls   = dialog_class_name(dlg);
        const std::string title = dlg->GetTitle().ToUTF8().data();
        const ClassRule*  rule  = nullptr;
        for (const ClassRule& r : k_class_rules)
            if (cls == r.cls) { rule = &r; break; }
        const bool affirmative = mode == RemoteAccess::Mode::Request;
        const int  answer      = rule ? rule->answer : default_answer(style_of(dlg), affirmative);
        RemoteAccess::get().note_attention(cls + (title.empty() ? "" : " \"" + title + "\""), answer_name(answer));
        BOOST_LOG_TRIVIAL(warning) << "hidden-mode dialog answered " << answer_name(answer) << ": " << cls << " \"" << title << "\"";
        if (rule && rule->human)
            RemoteAccess::get().raise_attention(cls + " needs someone at the PC", "manual");
        return answer; // ShowModal() returns this without showing anything
    }
    void Exit(wxDialog*) override
    {
        if (g_modal_depth > 0) --g_modal_depth;
    }
};
static DialogPolicyHook s_dialog_hook;

// Every second on the GUI thread: proves the loop is pumping and reports let-through modals.
class GuiHeartbeat : public wxTimer
{
public:
    void Notify() override
    {
        // The dialog policy, the LAN reconnect and the event watcher all reach into the devices and
        // the main window; none of that may run once it has started closing (it keeps ticking
        // until GUI_App::OnExit stops it).
        if (RemoteAccess::gui_closing())
            return;
        RemoteAccess::get().heartbeat_review(g_modal_depth);
        // The LAN reconnect tick rides here too. A LAN-mode Bambu printer's MQTT session used to be
        // re-established only by MonitorPanel::update, and only with a Bambu cloud login - so with
        // no account a dropped session stayed dropped until the user left the Device tab and came
        // back. The panel is lazily built now and the hidden instance never opens it, so the retry
        // belongs on a tick that always runs. Cheap: a map lookup per selected LAN printer.
        if (DeviceManager* dm = wxGetApp().getDeviceManager()) {
            try {
                dm->lan_reconnect_tick();
            } catch (...) {}
        }
        // The printer event watcher rides on this tick: it needs the GUI thread for the Bambu
        // MachineObjects anyway, and it polls at its own, slower rate (RemoteEvents.cpp).
        RemoteEvents::heartbeat();
    }
};
static GuiHeartbeat* s_heartbeat = nullptr;

} // namespace

void RemoteAccess::install_dialog_policy() { s_dialog_hook.Register(); }

void RemoteAccess::show_window(const std::string& reason)
{
    auto show = [reason]() {
        MainFrame* mf = wxGetApp().mainframe;
        if (mf == nullptr) return;
        if (mf->IsIconized()) mf->Iconize(false);
        mf->Show(true);
        mf->Raise();
        RemoteAccess::get().set_hidden(!mf->IsShown());
        RemoteAccess::get().note_attention("window shown: " + reason, "shown");
    };
    if (wxThread::IsMain()) { if (!gui_closing()) show(); }
    else post_to_main(show);
}

void RemoteAccess::note_attention(const std::string& dialog, const std::string& answered)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_attention.push_back({ (long long) std::time(nullptr), dialog, answered });
    if (m_attention.size() > 50) m_attention.pop_front();
}

void RemoteAccess::raise_attention(const std::string& reason, const char* kind)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_needs_attention && m_attention_reason == reason) return;
        m_needs_attention  = true;
        m_attention_reason = reason;
        m_attention_kind   = kind;
        m_attention_since  = (long long) std::time(nullptr);
        m_requests_done    = 0;
        if (m_on) write_instance_file();
    }
    BOOST_LOG_TRIVIAL(warning) << "RemoteAccess: needs attention (" << kind << "): " << reason;
    if (hidden())
        show_window(reason);
}

void RemoteAccess::clear_attention(const char* why)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_needs_attention) return;
    m_needs_attention = false;
    m_attention_reason.clear();
    m_attention_kind.clear();
    if (m_on) write_instance_file();
    BOOST_LOG_TRIVIAL(warning) << "RemoteAccess: attention cleared (" << why << ")";
}

bool RemoteAccess::needs_attention(std::string* reason)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (reason) *reason = m_attention_reason;
    return m_needs_attention;
}

void RemoteAccess::note_gui_tick(int modal_depth)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_gui_tick_ms = now_ms();
    m_modal_depth = modal_depth;
}

long long RemoteAccess::gui_stall_ms()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_gui_tick_ms ? now_ms() - m_gui_tick_ms : 0;
}

void RemoteAccess::note_request_done()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_requests_done;
}

void RemoteAccess::heartbeat_review(int modal_depth)
{
    note_gui_tick(modal_depth);
    std::string kind;
    bool        needs;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        kind  = m_attention_kind;
        needs = m_needs_attention;
    }
    if (modal_depth > 0 && hidden())
        raise_attention("a dialog on the PC is waiting for an answer", "dialog");
    else if (needs && kind == "dialog" && modal_depth == 0)
        clear_attention("the dialog is gone");
    else if (needs && kind == "timeout") {
        bool done;
        { std::lock_guard<std::mutex> lock(m_mutex); done = m_requests_done > 0; }
        if (done) clear_attention("a later request completed");
    }
}

// Never destroyed: request threads may still ask it something while the process exits.
static MainThreadGate& gui_gate()
{
    static MainThreadGate* gate = new MainThreadGate();
    return *gate;
}
// Closed only when the app quits (never reopened): the wxApp that CallAfter needs is about to go.
static MainThreadGate& app_gate()
{
    static MainThreadGate* gate = new MainThreadGate();
    return *gate;
}

static void post_via_wx(std::function<void()> f) { wxGetApp().CallAfter(std::move(f)); }

MainCallResult RemoteAccess::call_on_main(std::function<void()> fn, int timeout_ms)
{
    return call_and_wait(gui_gate(), post_via_wx, std::move(fn), std::chrono::milliseconds(timeout_ms));
}

bool RemoteAccess::post_to_main(std::function<void()> fn) { return gui_gate().post(post_via_wx, std::move(fn)); }

bool RemoteAccess::post_to_app(std::function<void()> fn) { return app_gate().post(post_via_wx, std::move(fn)); }

void RemoteAccess::close_gui_gate(bool quitting)
{
    if (!gui_gate().closed())
        BOOST_LOG_TRIVIAL(info) << "RemoteAccess: main window closing" << (quitting ? " (quit)" : " (rebuild)")
                                << ", no more GUI-thread work from requests";
    gui_gate().close();
    if (quitting)
        app_gate().close();
}

void RemoteAccess::reopen_gui_gate()
{
    gui_gate().reopen();
    BOOST_LOG_TRIVIAL(info) << "RemoteAccess: main window rebuilt, requests reach the GUI thread again";
}

bool RemoteAccess::gui_closing() { return gui_gate().closed(); }

static bool run_on_main(std::function<void()> fn, int timeout_ms = 15000, const char* what = "a request")
{
    const MainCallResult r = RemoteAccess::call_on_main([fn]() {
        RemoteAccess::AutoConfirmScope auto_yes;
        fn();
    }, timeout_ms);
    if (r == MainCallResult::Done) {
        RemoteAccess::get().note_request_done();
        return true;
    }
    if (r == MainCallResult::Closing) {
        // Not a stall: the window is going away and the request is answered 503 (crash c2a7d4de:
        // this used to run anyway, against a Plater that had already been freed).
        BOOST_LOG_TRIVIAL(info) << "RemoteAccess: " << what << " was not run: the slicer is closing";
        return false;
    }
    RemoteAccess::get().raise_attention(std::string(what) + " did not finish on the PC within " + std::to_string(timeout_ms / 1000) + " s", "timeout");
    return false;
}

static std::string json_error(const std::string& msg)
{
    nlohmann::json j;
    j["error"] = msg;
    return j.dump();
}

// ------------------------------------------------------------ RemoteAccess ----

RemoteAccess& RemoteAccess::get()
{
    static RemoteAccess instance;
    return instance;
}

void RemoteAccess::start()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_on)
        return;
    try {
        // Lives for the process and is never destroyed: the connection threads are detached, and a
        // socket destroyed after its io_context crashes in the socket service (see ServerLifetime.hpp).
        static asio::io_context& ioc = *new asio::io_context();
        auto* acceptor = new tcp::acceptor(ioc, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        acceptor->listen();
        m_acceptor = acceptor;
        m_port     = acceptor->local_endpoint().port();
        m_on       = true;
        std::thread([this]() { accept_loop(); }).detach();
        write_instance_file();
        if (s_heartbeat == nullptr) s_heartbeat = new GuiHeartbeat(); // GUI thread: start() runs there
        s_heartbeat->Start(1000);
        BOOST_LOG_TRIVIAL(info) << "RemoteAccess: instance API on 127.0.0.1:" << m_port;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "RemoteAccess: failed to start: " << e.what();
        m_on   = false;
        m_port = 0;
    }
}

void RemoteAccess::stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_on)
        return;
    m_on = false;
    RemoteEvents::stop();
    if (s_heartbeat) s_heartbeat->Stop();
    boost::system::error_code ig;
    fs::remove(fs::path(RemoteHub::instances_dir()) / (std::to_string(wxGetProcessId()) + ".json"), ig);
    if (auto* acceptor = static_cast<tcp::acceptor*>(m_acceptor)) {
        acceptor->close(ig); // unblocks accept_loop, which deletes the acceptor
        m_acceptor = nullptr;
    }
    m_port = 0;
}

int RemoteAccess::port()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_port;
}

// <datadir>/hub/instances/<pid>.json — how the hub finds this instance (m_mutex held).
void RemoteAccess::write_instance_file()
{
    boost::system::error_code ec;
    fs::create_directories(RemoteHub::instances_dir(), ec);
    nlohmann::json j;
    j["pid"]     = (long) wxGetProcessId();
    j["port"]    = m_port;
    j["started"] = (long long) std::time(nullptr);
    j["version"] = std::string(SLIC3R_VERSION);
    // What the hub's cheap (non-probing) listing shows: the tray menu never has to do HTTP.
    j["hidden"]  = m_hidden;
    j["title"]   = m_title;
    j["path"]    = m_path;
    j["needs_attention"]  = m_needs_attention;
    j["attention_reason"] = m_attention_reason;
    boost::nowide::ofstream f((fs::path(RemoteHub::instances_dir()) / (std::to_string(wxGetProcessId()) + ".json")).string(), std::ios::trunc);
    f << j.dump(2);
}

void RemoteAccess::note_project(const std::string& title, const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_title = title;
    m_path  = path;
    if (m_on) write_instance_file(); // the tray menu label follows the project
}

void RemoteAccess::set_hidden(bool hidden)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_hidden == hidden && m_on) return;
    m_hidden = hidden;
    if (m_on) write_instance_file();
}

bool RemoteAccess::hidden()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_hidden;
}

void RemoteAccess::note_error(const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_last_error = message;
}

std::string RemoteAccess::take_error()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string e;
    e.swap(m_last_error);
    return e;
}

void RemoteAccess::note_color_import(const ColorImport& c)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_last_color_import = c;
}

RemoteAccess::ColorImport RemoteAccess::take_color_import()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ColorImport c;
    std::swap(c, m_last_color_import);
    return c;
}

// ---------------------------------------------------------------- JSON API ----

void RemoteAccess::note_slice_progress(int plate, int percent, const std::string& text)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_slicing = true;
    for (Job& j : m_jobs)
        if (j.kind == "slice" && j.state == "running" && (j.plate == -1 || j.plate == plate)) {
            if (percent >= 0) j.percent = std::max(j.percent, std::min(percent, 99));
            j.text = text;
        }
}

void RemoteAccess::note_slice_done(bool finished_all, bool ok, const std::string& error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_slicing = false;
    for (Job& j : m_jobs)
        if (j.kind == "slice" && j.state == "running") {
            if (!ok) {
                j.state = error.empty() ? "cancelled" : "error";
                j.error = error;
            } else if (finished_all || j.plate != -1) {
                j.state   = "done";
                j.percent = 100;
            }
        }
}

RemoteAccess::ApiResponse RemoteAccess::api_plates()
{
    auto out = std::make_shared<nlohmann::json>();
    bool ok  = run_on_main([out]() {
        Plater*        plater = wxGetApp().plater();
        PresetBundle*  bundle = wxGetApp().preset_bundle;
        PartPlateList& plates = plater->get_partplate_list();
        nlohmann::json& j     = *out;
        j["project"]       = plater->get_project_filename().ToUTF8().data();
        j["printer"]       = bundle->printers.get_selected_preset_name();
        j["current_plate"] = plates.get_curr_plate_index();
        j["slicing"]       = plater->is_background_process_slicing();
        j["filaments"]     = nlohmann::json::array();
        std::vector<double> density;
        if (auto* d = bundle->full_config().option<ConfigOptionFloats>("filament_density"))
            density = d->values;
        const ConfigOptionStrings* colors = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        for (size_t i = 0; i < bundle->filament_presets.size(); ++i) {
            nlohmann::json f;
            f["name"]  = bundle->filament_presets[i];
            f["color"] = (colors && i < colors->values.size()) ? colors->values[i] : "";
            j["filaments"].push_back(f);
        }
        j["plates"] = nlohmann::json::array();
        for (int i = 0; i < plates.get_plate_count(); ++i) {
            PartPlate*     p = plates.get_plate(i);
            nlohmann::json jp;
            jp["index"]   = i;
            jp["name"]    = p->get_plate_name();
            jp["objects"] = nlohmann::json::array();
            jp["boxes"]   = nlohmann::json::array(); // per object: [min_x, min_y, max_x, max_y] in plate-list mm
            for (const ModelObject* o : p->get_objects_on_this_plate()) {
                jp["objects"].push_back(o->name);
                const BoundingBoxf3 bb = o->bounding_box_exact();
                jp["boxes"].push_back({ bb.min.x(), bb.min.y(), bb.max.x(), bb.max.y() });
            }
            const BoundingBoxf3& pb = p->get_bounding_box();
            jp["plate_box"] = { pb.min.x(), pb.min.y(), pb.max.x(), pb.max.y() };
            jp["printable"]       = p->has_printable_instances();
            jp["locked"]          = p->is_locked();
            jp["sliced"]          = p->is_slice_result_valid();
            jp["ready_for_print"] = p->is_slice_result_ready_for_print();
            jp["slicing_percent"] = p->get_slicing_percent();
            if (p->is_slice_result_valid() && p->get_slice_result()) {
                const auto& st = p->get_slice_result()->print_statistics;
                if (!st.modes.empty())
                    jp["time_s"] = st.modes.front().time;
                double mm3 = 0, grams = 0;
                for (const auto& kv : st.total_volumes_per_extruder) {
                    mm3 += kv.second;
                    const double dens = kv.first < density.size() ? density[kv.first] : 1.24;
                    grams += kv.second / 1000.0 * dens;
                }
                jp["filament_mm3"] = mm3;
                jp["filament_g"]   = grams;
            }
            j["plates"].push_back(jp);
        }
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); }
    else       r.body = out->dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_plate_thumbnail(int plate)
{
    auto data = std::make_shared<ThumbnailData>();
    bool ok   = run_on_main([data, plate]() {
        Plater*        plater = wxGetApp().plater();
        PartPlateList& plates = plater->get_partplate_list();
        if (plate < 0 || plate >= plates.get_plate_count())
            return;
        if (!plater->get_view3D_canvas3D()->ensure_gl_ready()) { BOOST_LOG_TRIVIAL(error) << "RemoteAccess: thumbnail: OpenGL is not usable"; return; }
        plater->update_all_plate_thumbnails(false);
        PartPlate* p = plates.get_plate(plate);
        if (!p->thumbnail_data.is_valid()) {
            // The current plate's thumbnail is reset by every edit; render this one now.
            ThumbnailsParams params = { {}, false, true, true, true, plate };
            plater->get_view3D_canvas3D()->render_thumbnail(p->thumbnail_data, p->plate_thumbnail_width, p->plate_thumbnail_height, params, Camera::EType::Ortho);
        }
        *data = p->thumbnail_data;
    }, 30000);
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (!data->is_valid()) { r.status = 404; r.body = json_error("no thumbnail for this plate"); return r; }
    auto png = GCodeThumbnails::compress_thumbnail(*data, GCodeThumbnailsFormat::PNG);
    r.type   = "image/png";
    r.body.assign(static_cast<const char*>(png->data), png->size);
    return r;
}

// ------------------------------------------------------------ slice preview ----

// Put plate `plate`'s sliced result into the PC's preview (current plate + Preview tab, like the
// toolbar's Slice does) so the G-code viewer holds its toolpaths and layers. "" = ready.
static std::string ensure_preview_loaded(int plate)
{
    Plater*        plater = wxGetApp().plater();
    PartPlateList& plates = plater->get_partplate_list();
    if (plate < 0 || plate >= plates.get_plate_count())
        return "no such plate";
    if (plater->is_background_process_slicing())
        return "the slicer is slicing";
    PartPlate* p = plates.get_plate(plate);
    if (!p->is_slice_result_valid() || p->get_slice_result() == nullptr)
        return "this plate is not sliced";
    if (plates.get_curr_plate_index() != plate)
        plater->select_plate(plate, false);
    // The G-code viewer's buffers are built by Preview::load_print_as_fff -> load_gcode_preview with
    // no render pass around it, and load_shells is a no-op until the preview canvas is initialised.
    if (!plater->get_preview_canvas3D()->ensure_gl_ready())
        return "the preview is not ready (OpenGL)";
    if (!plater->is_preview_shown())
        plater->select_view_3D("Preview");
    GLCanvas3D*  canvas = plater->get_preview_canvas3D();
    GCodeViewer& viewer = canvas->get_gcode_viewer();
    if (!viewer.has_data() || viewer.loaded_result_id() != p->get_slice_result()->id)
        plater->refresh_print();
    if (!viewer.has_data() || viewer.get_layers_zs().empty())
        return "no toolpaths to show";
    return "";
}

RemoteAccess::ApiResponse RemoteAccess::api_plate_preview(int plate)
{
    auto out = std::make_shared<nlohmann::json>();
    auto err = std::make_shared<std::string>();
    bool ok  = run_on_main([out, err, plate]() {
        Plater*         plater = wxGetApp().plater();
        PartPlateList&  plates = plater->get_partplate_list();
        nlohmann::json& j      = *out;
        if (plate < 0 || plate >= plates.get_plate_count()) { *err = "no such plate"; return; }
        PartPlate* p       = plates.get_plate(plate);
        const bool sliced  = p->is_slice_result_valid() && p->get_slice_result() != nullptr;
        const bool slicing = plater->is_background_process_slicing();
        j["index"]   = plate;
        j["name"]    = p->get_plate_name();
        j["objects"] = p->get_objects_on_this_plate().size();
        j["sliced"]  = sliced;
        j["slicing"] = slicing;
        j["views"]   = { "front", "rear", "left", "right" };
        if (!sliced || slicing)
            return;
        const std::string e = ensure_preview_loaded(plate);
        if (!e.empty()) { *err = e; return; }
        GCodeViewer& v = plater->get_preview_canvas3D()->get_gcode_viewer();
        j["result_id"] = v.loaded_result_id();
        j["layers"]    = v.get_layers_zs();
        j["range"]     = v.get_layers_z_range();
        const BoundingBoxf3& b = v.get_paths_bounding_box();
        j["box"] = { b.min.x(), b.min.y(), b.min.z(), b.max.x(), b.max.y(), b.max.z() };
    }, 60000);
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (!err->empty()) { r.status = *err == "no such plate" ? 404 : 409; r.body = json_error(*err); return r; }
    r.body = out->dump();
    return r;
}

// One frame of the PC's G-code preview: the toolpaths of layers 0..layer (the PC's layer slider is
// moved there too) from a named orthographic view, on a transparent background.
// Cheap, side-effect free: what the phone polls to notice a re-slice (new result_id) or an edit
// that invalidated the result, without touching what the PC shows.
RemoteAccess::ApiResponse RemoteAccess::api_plate_preview_status(int plate)
{
    auto out = std::make_shared<nlohmann::json>();
    auto err = std::make_shared<std::string>();
    bool ok  = run_on_main([out, err, plate]() {
        Plater*        plater = wxGetApp().plater();
        PartPlateList& plates = plater->get_partplate_list();
        if (plate < 0 || plate >= plates.get_plate_count()) { *err = "no such plate"; return; }
        PartPlate*      p      = plates.get_plate(plate);
        const bool      sliced = p->is_slice_result_valid() && p->get_slice_result() != nullptr;
        nlohmann::json& j      = *out;
        j["index"]           = plate;
        j["sliced"]          = sliced;
        j["slicing"]         = plater->is_background_process_slicing();
        j["slicing_percent"] = p->get_slicing_percent();
        j["result_id"]       = sliced ? p->get_slice_result()->id : 0u;
        j["current_plate"]   = plates.get_curr_plate_index();
        j["preview_shown"]   = plater->is_preview_shown();
    }, 5000);
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (!err->empty()) { r.status = 404; r.body = json_error(*err); return r; }
    r.body = out->dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_plate_preview_png(int plate, const std::string& view_in, int layer, int w, int h, double zoom, double cx, double cy)
{
    static const std::set<std::string> views = { "front", "rear", "left", "right", "top", "bottom", "iso" };
    const std::string view = views.count(view_in) ? view_in : "front";
    w = std::max(64, std::min(2048, w));
    h = std::max(64, std::min(2048, h));
    zoom = std::isfinite(zoom) ? std::max(1.0, std::min(16.0, zoom)) : 1.0;
    cx   = std::isfinite(cx) ? std::max(0.0, std::min(1.0, cx)) : 0.5;
    cy   = std::isfinite(cy) ? std::max(0.0, std::min(1.0, cy)) : 0.5;
    auto data = std::make_shared<ThumbnailData>();
    auto err  = std::make_shared<std::string>();
    auto eff  = std::make_shared<double>(1.0);
    bool ok   = run_on_main([data, err, eff, plate, view, layer, w, h, zoom, cx, cy]() {
        const std::string e = ensure_preview_loaded(plate);
        if (!e.empty()) { *err = e; return; }
        Plater*      plater = wxGetApp().plater();
        GLCanvas3D*  canvas = plater->get_preview_canvas3D();
        GCodeViewer& v      = canvas->get_gcode_viewer();
        const std::vector<double>& zs = v.get_layers_zs();
        const int          last  = (int) zs.size() - 1;
        const unsigned int top   = (unsigned int) std::max(0, std::min(last, layer < 0 ? last : layer));
        const std::array<unsigned int, 2> range = { 0u, top };
        if (v.get_layers_z_range() != range) {
            IMSlider* slider = v.get_layers_slider();
            slider->SetSelectionSpan(0, (int) top);
            slider->set_as_dirty(false);
            v.set_layers_z_range(range);
            canvas->set_volumes_z_range({ slider->GetLowerValueD(), slider->GetHigherValueD() });
            canvas->set_as_dirty();
            canvas->request_extra_frame();
        }
        BoundingBoxf3 box = v.get_paths_bounding_box();
        if (!box.defined) { *err = "no toolpaths to show"; return; }
        box.min.z() = std::min(box.min.z(), -1.0);
        const BoundingBoxf3& pb = plater->get_partplate_list().get_plate(plate)->get_bounding_box();
        const BoundingBoxf3  bed(Vec3d(pb.min.x(), pb.min.y(), -1.0), Vec3d(pb.max.x(), pb.max.y(), 0.0));
        canvas->render_gcode_preview_image(*data, (unsigned int) w, (unsigned int) h, view, box, bed, zoom, cx, cy, eff.get());
    }, 30000);
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (!err->empty()) { r.status = *err == "no such plate" ? 404 : 409; r.body = json_error(*err); return r; }
    if (!data->is_valid()) { r.status = 500; r.body = json_error("the preview could not be rendered"); return r; }
    auto png  = GCodeThumbnails::compress_thumbnail(*data, GCodeThumbnailsFormat::PNG);
    r.type    = "image/png";
    r.headers = "X-Preview-Zoom: " + std::to_string(*eff) + "\r\n";
    r.body.assign(static_cast<const char*>(png->data), png->size);
    return r;
}

// ------------------------------------------------------------ plate layout ----

// Top-down view of one plate: every instance registered on it with the convex hull of the
// transformed model (world mm), bounding box, position / Z rotation / uniform scale and the
// colour of its filament. The phone draws this and edits it through api_object_transform.
RemoteAccess::ApiResponse RemoteAccess::api_plate_layout(int plate)
{
    auto out = std::make_shared<nlohmann::json>();
    bool ok  = run_on_main([out, plate]() {
        Plater*        plater = wxGetApp().plater();
        PartPlateList& plates = plater->get_partplate_list();
        if (plate < 0 || plate >= plates.get_plate_count()) return;
        PartPlate*      p  = plates.get_plate(plate);
        nlohmann::json& j  = *out;
        const BoundingBoxf3& pb = p->get_bounding_box(false);
        j["index"]     = plate;
        j["name"]      = p->get_plate_name();
        j["plate_box"] = { pb.min.x(), pb.min.y(), pb.max.x(), pb.max.y() };
        j["exclude"]   = nlohmann::json::array();
        for (const BoundingBoxf3& e : p->get_exclude_areas())
            j["exclude"].push_back({ e.min.x(), e.min.y(), e.max.x(), e.max.y() });
        const ConfigOptionStrings* colors = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        const Selection& sel = plater->get_view3D_canvas3D()->get_selection();
        Model& model = plater->model();
        j["objects"] = nlohmann::json::array();
        for (size_t oi = 0; oi < model.objects.size(); ++oi) {
            ModelObject* o = model.objects[oi];
            for (size_t ii = 0; ii < o->instances.size(); ++ii) {
                if (plates.find_instance_belongs((int) oi, (int) ii) != plate) continue;
                ModelInstance* mi = o->instances[ii];
                nlohmann::json ji;
                ji["obj"]  = oi;
                ji["inst"] = ii;
                ji["name"] = o->name;
                const BoundingBoxf3 bb = o->instance_bounding_box(ii);
                ji["bbox"] = { bb.min.x(), bb.min.y(), bb.max.x(), bb.max.y() };
                ji["size"] = { bb.size().x(), bb.size().y(), bb.size().z() };
                const Vec3d off = mi->get_offset();
                ji["offset"] = { off.x(), off.y(), off.z() };
                ji["rz"]     = mi->get_rotation().z() * 180.0 / M_PI;
                ji["scale"]  = mi->get_scaling_factor().x();
                ji["hull"]   = nlohmann::json::array();
                {
                    // From the meshes themselves: the volumes' cached 2D hulls are empty until
                    // something (arrange, collision checks) computes them.
                    Points pts;
                    for (const ModelVolume* v : o->volumes)
                        if (v->is_model_part()) {
                            // The volume's 3D convex hull projects to the same outline as the mesh
                            // with a fraction of the vertices; fall back to the mesh when absent.
                            const indexed_triangle_set& its = v->get_convex_hull().its.indices.empty() ? v->mesh().its : v->get_convex_hull().its;
                            const Transform3f t = (mi->get_transformation().get_matrix() * v->get_matrix()).cast<float>();
                            its_collect_mesh_projection_points_above(its, t, -1.e9f, pts);
                        }
                    const Polygon hull = Geometry::convex_hull(std::move(pts));
                    for (const Point& pt : hull.points)
                        ji["hull"].push_back({ unscale<double>(pt.x()), unscale<double>(pt.y()) });
                    if (hull.points.empty()) // degenerate mesh: fall back to the bounding box
                        ji["hull"] = { { bb.min.x(), bb.min.y() }, { bb.max.x(), bb.min.y() }, { bb.max.x(), bb.max.y() }, { bb.min.x(), bb.max.y() } };
                }
                int extruder = 0;
                if (const ConfigOptionInt* e = o->config.get().option<ConfigOptionInt>("extruder")) extruder = e->value;
                std::string color;
                if (colors && !colors->values.empty())
                    color = (extruder > 0 && (size_t) extruder <= colors->values.size()) ? colors->values[extruder - 1] : colors->values[0];
                ji["color"]    = color;
                ji["outside"]  = !(bb.min.x() >= pb.min.x() && bb.max.x() <= pb.max.x() && bb.min.y() >= pb.min.y() && bb.max.y() <= pb.max.y());
                ji["selected"] = sel.is_single_full_instance() && sel.get_object_idx() == (int) oi && sel.get_instance_idx() == (int) ii;
                j["objects"].push_back(ji);
            }
        }
    }, 30000);
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); }
    else if (out->empty()) { r.status = 404; r.body = json_error("no such plate"); }
    else r.body = out->dump();
    return r;
}

// Move / rotate / scale one instance the way the sidebar does it (selection + the canvas'
// do_move / do_rotate / do_scale, so undo, plate membership and dirty state follow).
// Form: obj=&inst=[&x=&y= (absolute instance position, mm)][&rz= (absolute Z rotation, deg)]
//       [&scale= (absolute uniform factor)][&center=1 (centre the model on its plate)]
RemoteAccess::ApiResponse RemoteAccess::api_object_transform(const std::string& form)
{
    auto get = [&](const char* k) { return query_param(form, k); };
    auto num = [&](const char* k, double def, bool& has) { const std::string v = get(k); has = !v.empty(); try { return has ? std::stod(v) : def; } catch (...) { has = false; return def; } };
    bool has_obj, has_inst, has_x, has_y, has_rz, has_scale;
    const int    obj   = (int) num("obj", -1, has_obj);
    const int    inst  = (int) num("inst", 0, has_inst);
    const double x     = num("x", 0, has_x), y = num("y", 0, has_y), rz = num("rz", 0, has_rz), scale = num("scale", 1, has_scale);
    const bool   center = get("center") == "1";
    ApiResponse r;
    if (!has_obj || obj < 0) { r.status = 400; r.body = json_error("obj is required"); return r; }
    if (has_scale && (scale < 0.01 || scale > 100)) { r.status = 400; r.body = json_error("scale must be between 0.01 and 100"); return r; }
    if (!has_x && !has_y && !has_rz && !has_scale && !center) { r.status = 400; r.body = json_error("nothing to change"); return r; }
    auto result = std::make_shared<std::pair<int, std::string>>(500, "");
    auto out    = std::make_shared<nlohmann::json>();
    bool ok     = run_on_main([=]() {
        Plater* plater = wxGetApp().plater();
        if (plater->is_background_process_slicing()) { *result = { 409, "slicing in progress" }; return; }
        Model& model = plater->model();
        if (obj >= (int) model.objects.size()) { *result = { 404, "no such object" }; return; }
        ModelObject* o = model.objects[obj];
        if (inst < 0 || inst >= (int) o->instances.size()) { *result = { 404, "no such instance" }; return; }
        plater->select_view_3D("3D");
        GLCanvas3D* canvas = plater->get_view3D_canvas3D();
        Selection&  sel    = canvas->get_selection();
        sel.add_instance((unsigned) obj, (unsigned) inst, true);
        ModelInstance* mi = o->instances[inst];
        if (has_rz) {
            const double cur = mi->get_rotation().z() * 180.0 / M_PI;
            TransformationType t;
            t.set_relative();
            if (sel.is_single_full_instance()) t.set_independent();
            sel.setup_cache();
            sel.rotate(Vec3d(0, 0, (rz - cur) * M_PI / 180.0), t);
            plater->take_snapshot("Set Orientation", UndoRedo::SnapshotType::GizmoAction);
            canvas->do_rotate("");
        }
        if (has_scale) {
            TransformationType t; // absolute, world
            sel.setup_cache();
            sel.scale(Vec3d(scale, scale, scale), t);
            canvas->do_scale("Set Scale");
        }
        Vec3d target = mi->get_offset();
        bool  move   = false;
        if (has_x) { target.x() = x; move = true; }
        if (has_y) { target.y() = y; move = true; }
        if (center) {
            PartPlateList& plates = plater->get_partplate_list();
            const int      pi     = plates.find_instance_belongs(obj, inst);
            PartPlate*     p      = pi >= 0 ? plates.get_plate(pi) : plates.get_curr_plate();
            const BoundingBoxf3  bb = o->instance_bounding_box(inst);
            const BoundingBoxf3& pb = p->get_bounding_box(false);
            target.x() += pb.center().x() - bb.center().x();
            target.y() += pb.center().y() - bb.center().y();
            move = true;
        }
        if (move) {
            const Vec3d cur = mi->get_offset();
            TransformationType t;
            t.set_relative();
            sel.setup_cache();
            sel.translate(target - cur, t);
            plater->take_snapshot("Set Position", UndoRedo::SnapshotType::GizmoAction);
            canvas->do_move("");
        }
        canvas->set_as_dirty();
        const Vec3d off = mi->get_offset();
        (*out)["offset"] = { off.x(), off.y(), off.z() };
        (*out)["rz"]     = mi->get_rotation().z() * 180.0 / M_PI;
        (*out)["scale"]  = mi->get_scaling_factor().x();
        (*out)["plate"]  = plater->get_partplate_list().find_instance_belongs(obj, inst);
        *result = { 200, "" };
    }, 60000);
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (result->first != 200) { r.status = result->first; r.body = json_error(result->second); return r; }
    r.body = out->dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_printers(int plate)
{
    auto out      = std::make_shared<nlohmann::json>();
    auto targets  = std::make_shared<std::vector<RemoteControl::HostTarget>>();
    auto lan_name = std::make_shared<std::string>();
    bool ok  = run_on_main([out, targets, lan_name, plate]() {
        nlohmann::json& j = *out;
        j["printers"]     = nlohmann::json::array();
        *lan_name         = RemoteSend::lan_upload_name(plate);
        DeviceManager* dm = wxGetApp().getDeviceManager();
        // The printer preset's print host and the connected Snapmaker are printers too (RemoteSend).
        // `plate` (from ?plate=) says which plate the upload_name defaults are for; -1 = the current one.
        struct HostsAtEnd { nlohmann::json& list; int plate; ~HostsAtEnd() { try { RemoteSend::list_hosts(list, plate); } catch (...) {} } } hosts_at_end { j["printers"], plate };
        try { RemoteControl::list_host_targets(*targets); } catch (...) {} // where to ask those two what they are doing
        if (!dm)
            return;
        MachineObject* selected = dm->get_selected_machine();
        std::map<std::string, MachineObject*> all = dm->get_my_machine_list();
        for (const auto& kv : dm->get_local_machine_list())
            all.insert(kv);
        for (const auto& kv : all) {
            MachineObject* m = kv.second;
            if (!m) continue;
            nlohmann::json p;
            p["id"]           = m->dev_id;
            p["name"]         = m->dev_name;
            p["model"]        = m->printer_type;
            p["online"]       = m->is_online();
            p["connected"]    = m->is_connected();
            p["status"]       = m->print_status;
            p["printing"]     = m->is_in_printing();
            p["percent"]      = m->mc_print_percent;
            p["left_time_s"]  = m->mc_left_time;
            p["layer"]        = m->curr_layer;
            p["total_layers"] = m->total_layers;
            p["task"]         = m->subtask_name;
            p["bed_temp"]     = m->bed_temp;
            p["bed_target"]   = m->bed_temp_target;
            p["nozzles"]      = nlohmann::json::array();
            for (const Extder& e : m->m_extder_data.extders) {
                nlohmann::json n;
                n["temp"]   = e.temp;
                n["target"] = e.target_temp;
                p["nozzles"].push_back(n);
            }
            p["selected"] = (selected == m);
            RemoteSend::describe_bambu(m, p);    // kind, send capabilities, option defaults
            RemoteControl::describe_bambu(m, p); // can_pause / can_resume / can_stop, stage, print error
            j["printers"].push_back(p);
        }
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    // The Snapmakers on the LAN: listing them probes each one over HTTP (4 s timeout) when the
    // 4 s status cache is stale, so it belongs on this request thread, never on the GUI one - on
    // the GUI thread every Devices-page poll from the phone parked the window for the length of
    // the probes and the queue never drained (2026-09-06 "not responding"). They go first, as the
    // list had them before.
    {
        nlohmann::json lan = nlohmann::json::array();
        try { SnapmakerLan::list_printers(lan); } catch (...) {}
        nlohmann::json merged = nlohmann::json::array();
        for (nlohmann::json& p : lan) {
            if (p.value("kind", std::string()) == "snapmaker") p["upload_name"] = *lan_name;
            merged.push_back(std::move(p));
        }
        for (nlohmann::json& p : (*out)["printers"]) merged.push_back(std::move(p));
        (*out)["printers"] = std::move(merged);
    }
    // A print host has no live status in the app: ask it over Moonraker's HTTP API from this
    // request thread, never from the GUI one (api_snapmaker_devices probes the same way).
    try { RemoteControl::describe_hosts(*targets, (*out)["printers"]); } catch (...) {}
    // One card per printer: the Device tab's connect (often through the Snapmaker cloud) gives way
    // to the same printer's LAN card while that one answers.
    try { SnapmakerLan::prefer_lan((*out)["printers"]); } catch (...) {}
    r.body = out->dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_slice(int plate, bool all)
{
    auto result = std::make_shared<std::pair<int, std::string>>(500, "");
    bool ok     = run_on_main([result, plate, all]() {
        Plater*        plater = wxGetApp().plater();
        PartPlateList& plates = plater->get_partplate_list();
        if (plater->is_background_process_slicing()) { *result = { 409, "already slicing" }; return; }
        if (!all && (plate < 0 || plate >= plates.get_plate_count())) { *result = { 404, "no such plate" }; return; }
        if (all && !plater->has_sliceable_plate_for_slice_all()) { *result = { 409, "nothing to slice" }; return; }
        if (!all) {
            if (!plates.get_plate(plate)->has_printable_instances()) { *result = { 409, "plate is empty" }; return; }
            plater->select_plate(plate);
        }
        plater->exit_gizmo();
        plater->update(true, true);
        // Synchronous, not posted: the pre-slice confirms (temperature mixing, memory) then run
        // inside this request's auto-confirm scope instead of after it.
        SimpleEvent evt(all ? EVT_GLTOOLBAR_SLICE_ALL : EVT_GLTOOLBAR_SLICE_PLATE);
        plater->GetEventHandler()->ProcessEvent(evt);
        *result = { 200, "" };
    }, 60000, "starting a slice");
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (result->first != 200) { r.status = result->first; r.body = json_error(result->second); return r; }
    Job job;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        job.id    = m_next_job++;
        job.plate = all ? -1 : plate;
        job.state = "running";
        m_jobs.push_back(job);
        if (m_jobs.size() > 50)
            m_jobs.erase(m_jobs.begin());
    }
    nlohmann::json j;
    j["job"]   = job.id;
    j["plate"] = job.plate;
    r.body     = j.dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_jobs(int id)
{
    ApiResponse r;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto to_json = [](const Job& j) {
        nlohmann::json o;
        o["id"] = j.id; o["plate"] = j.plate; o["kind"] = j.kind; o["state"] = j.state; o["percent"] = j.percent;
        o["text"] = j.text; o["error"] = j.error;
        if (j.kind == "send" || j.kind == "control") { o["printer"] = j.printer; o["mode"] = j.mode; o["result"] = j.result; }
        if (j.kind == "control") o["action"] = j.mode; // pause | resume | stop, under its own name
        return o;
    };
    if (id < 0) {
        nlohmann::json j;
        j["jobs"] = nlohmann::json::array();
        for (const Job& job : m_jobs) j["jobs"].push_back(to_json(job));
        r.body = j.dump();
        return r;
    }
    for (const Job& job : m_jobs)
        if (job.id == id) { r.body = to_json(job).dump(); return r; }
    r.status = 404;
    r.body   = json_error("no such job");
    return r;
}

// ------------------------------------------------------- the G-code archive ----

// Every file this PC has sent to a printer while the archive was on (Preferences > Ultra >
// G-Code Archive), newest first. The sidecar is handed out as it is, minus this PC's own paths:
// the phone gets an id and a file name, never a place on the disk. Stage 2 (Reprints) sends one
// of these back to a printer; nothing here starts a print.
RemoteAccess::ApiResponse RemoteAccess::api_archive(const std::string& printer)
{
    ApiResponse    r;
    nlohmann::json j;
    j["enabled"] = GcodeArchive::enabled();
    j["max"]     = GcodeArchive::max_records();
    j["records"] = nlohmann::json::array();
    for (const GcodeArchive::Record& rec : GcodeArchive::list(printer)) {
        nlohmann::json o = rec.json;
        o.erase("path"); // a sidecar never holds one, but never hand one out either
        o.erase("project_path");
        o["has_thumbnail"] = rec.has_thumbnail;
        o["exists"]        = rec.file_present;
        j["records"].push_back(o);
    }
    r.body = j.dump();
    return r;
}

// One record, the same shape as a row of GET /api/archive. `exists` says whether the file is
// still on disk, which is the difference between a row that can be reprinted and one that cannot.
RemoteAccess::ApiResponse RemoteAccess::api_archive_one(const std::string& id)
{
    ApiResponse                r;
    const GcodeArchive::Record rec = GcodeArchive::find(id);
    if (rec.id.empty()) { r.status = 404; r.body = json_error("no such record"); return r; }
    nlohmann::json o = rec.json;
    o.erase("path");
    o.erase("project_path");
    o["has_thumbnail"] = rec.has_thumbnail;
    o["exists"]        = rec.file_present;
    r.body = o.dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_archive_thumbnail(const std::string& id)
{
    ApiResponse                r;
    const GcodeArchive::Record rec = GcodeArchive::find(id);
    if (rec.id.empty() || !rec.has_thumbnail) { r.status = 404; r.body = json_error("no thumbnail for this record"); return r; }
    boost::nowide::ifstream f(rec.thumbnail_path.c_str(), std::ios::binary);
    if (!f) { r.status = 404; r.body = json_error("no thumbnail for this record"); return r; }
    std::stringstream ss;
    ss << f.rdbuf();
    r.type = "image/png";
    r.body = ss.str();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_archive_delete(const std::string& id)
{
    ApiResponse r;
    // A delete that fails has two quite different causes, and the phone showed the same words for
    // both: the record is genuinely gone (someone else deleted it, retention dropped it), or it is
    // right there in the listing and the files under it would not go. Say which.
    const bool known = !GcodeArchive::find(id).id.empty();
    if (!GcodeArchive::remove(id)) {
        r.status = known ? 500 : 404;
        r.body   = json_error(known ? "the stored file could not be deleted - it may be open in another program, "
                                      "or the archive folder is read-only"
                                    : "no such record - it may already have been deleted");
        return r;
    }
    nlohmann::json j;
    j["deleted"] = id;
    r.body       = j.dump();
    return r;
}

// Stage 2: send an archived file again. The bytes in the record are the payload - nothing is
// re-sliced, no project has to be open, and the plater is not touched at all - but from the job id
// onwards this is the same send as /api/plates/{i}/send: the same single-flight lock, the same
// RemoteSend::run() on its own thread, the same /api/jobs/{id} progress. Form: [printer=<id>]
// [&mode=upload|print]&confirm=1[&force=1][&dry_run=1][&name=][&mapping=0:1,1:2]
// [&unload_at_end=0|1]; `printer` and
// every option not given fall back to the record.
RemoteAccess::ApiResponse RemoteAccess::api_archive_send(const std::string& id, const std::string& form_body)
{
    ApiResponse r;
    auto get = [&](const char* k) { return query_param(form_body, k); };
    RemoteSend::Request req;
    req.record  = id;
    req.plate   = -1;
    req.printer = get("printer");
    req.mode    = get("mode");
    req.confirm = get("confirm") == "1";
    req.force   = get("force") == "1";
    req.dry_run = get("dry_run") == "1";
    req.name    = get("name");
    req.mapping = get("mapping");
    req.unload_at_end = get("unload_at_end");
    // A Bambu printer: the AMS slot per filament (the preview's "mapping", edited or not) and the
    // print options; anything not given is what the desktop's send dialog would pick.
    auto tri = [&](const char* k) { const std::string v = get(k); return v.empty() ? -1 : ((v == "1" || v == "true") ? 1 : 0); };
    req.ams_mapping        = get("ams_mapping");
    req.bed_leveling       = tri("bed_leveling");
    req.flow_cali          = tri("flow_cali");
    req.timelapse          = tri("timelapse");
    req.use_ams            = tri("use_ams");
    req.nozzle_offset_cali = tri("nozzle_offset_cali");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_send_running) { r.status = 409; r.body = json_error("a send is already running; wait for it to finish"); return r; }
        m_send_running = true; // reserved until the job ends, or preparing fails below
    }
    // Preparing a reprint reads the archive and asks the printer what it holds; neither wants the
    // GUI thread, and the run_on_main rule says network I/O never runs on it. RemoteSend::
    // prepare_from_record steps onto it only for the printer preset.
    std::shared_ptr<RemoteSend::Prepared> p;
    std::pair<int, std::string>           result(500, "not run");
    take_error();
    try {
        result = RemoteSend::prepare_from_record(req, p);
    } catch (const std::exception& e) {
        result = { 500, std::string("preparing the reprint failed: ") + e.what() };
    } catch (...) {
        result = { 500, "preparing the reprint failed" };
    }
    if (result.first != 200 || !p) {
        { std::lock_guard<std::mutex> lock(m_mutex); m_send_running = false; }
        const std::string shown = take_error();
        r.status = result.first == 200 ? 500 : result.first;
        r.body   = json_error(result.second + (shown.empty() ? "" : ": " + shown));
        return r;
    }
    Job job;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        job.id      = m_next_job++;
        job.plate   = -1;
        job.kind    = "send";
        job.state   = "running";
        job.printer = p->printer_id;
        job.mode    = p->mode;
        job.text    = "starting";
        m_jobs.push_back(job);
        if (m_jobs.size() > 50)
            m_jobs.erase(m_jobs.begin());
    }
    const int        jid = job.id;
    RemoteSend::Sink sink;
    sink.progress = [this, jid](int pct, const std::string& text) { update_job(jid, pct, text); };
    sink.done     = [this, jid](bool ok, const std::string& error, const nlohmann::json& res) { finish_job(jid, ok, error, res); };
    std::thread([p, sink]() { RemoteSend::run(p, sink); }).detach();
    nlohmann::json j;
    j["job"]     = jid;
    j["plate"]   = -1;      // a reprint has no plate: the same shape, an index that says so
    j["record"]  = p->record_id;
    j["kind"]    = p->kind;
    j["printer"] = p->printer_name;
    j["mode"]    = p->mode;
    j["dry_run"] = p->dry_run;
    r.body       = j.dump();
    return r;
}

// A Bambu reprint's mapping sheet, before anything is sent (RemoteSend::preview_record): the job
// read back from the archived .gcode.3mf, matched against the target printer's AMS the way the
// desktop's send dialog matches a plate. Form: [printer=<id>][&mode=print|upload][&ams_mapping=
// 0:0-2,1:1-0][&use_ams=&bed_leveling=&flow_cali=&timelapse=&nozzle_offset_cali=]. It selects the
// printer on the PC (which connects it, as picking it in the send dialog does) and may take a few
// seconds while the printer's status and, on an H2C, its nozzle-mapping answer arrive. Nothing is
// uploaded or started; /api/archive/{id}/send does that with the same form plus confirm=1.
RemoteAccess::ApiResponse RemoteAccess::api_archive_preview(const std::string& id, const std::string& form_body)
{
    ApiResponse r;
    auto get = [&](const char* k) { return query_param(form_body, k); };
    auto tri = [&](const char* k) { const std::string v = get(k); return v.empty() ? -1 : ((v == "1" || v == "true") ? 1 : 0); };
    RemoteSend::Request req;
    req.record             = id;
    req.plate              = -1;
    req.printer            = get("printer");
    req.mode               = get("mode");
    req.force              = get("force") == "1";
    req.ams_mapping        = get("ams_mapping");
    req.bed_leveling       = tri("bed_leveling");
    req.flow_cali          = tri("flow_cali");
    req.timelapse          = tri("timelapse");
    req.use_ams            = tri("use_ams");
    req.nozzle_offset_cali = tri("nozzle_offset_cali");
    // One preview at a time, and never while a send is running: both select a printer on the PC.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_send_running) { r.status = 409; r.body = json_error("a send is already running; wait for it to finish"); return r; }
        m_send_running = true;
    }
    nlohmann::json              out;
    std::pair<int, std::string> result(500, "not run");
    take_error();
    try {
        result = RemoteSend::preview_record(req, out);
    } catch (const std::exception& e) {
        result = { 500, std::string("preparing the preview failed: ") + e.what() };
    } catch (...) {
        result = { 500, "preparing the preview failed" };
    }
    { std::lock_guard<std::mutex> lock(m_mutex); m_send_running = false; }
    if (result.first != 200) {
        const std::string shown = take_error();
        r.status = result.first;
        r.body   = json_error(result.second + (shown.empty() ? "" : ": " + shown));
        return r;
    }
    r.body = out.dump();
    return r;
}

// Send the sliced plate to a printer the way the desktop's Print / Send dialogs would, without
// showing them (RemoteSend). Form: printer=<id>&mode=upload|print[&confirm=1][&force=1][&dry_run=1]
// [&bed_leveling=&flow_cali=&timelapse=&vibration_cali=&use_ams=][&name=][&mapping=0:1,1:2]
// [&unload_at_end=0|1] (Snapmaker tool changers: the printer preset's own setting is the default).
// Returns a job id; the
// transfer runs on its own thread and /api/jobs/{id} follows it (kind send).
RemoteAccess::ApiResponse RemoteAccess::api_send(int plate, const std::string& form_body)
{
    ApiResponse r;
    auto get = [&](const char* k) { return query_param(form_body, k); };
    auto tri = [&](const char* k) { const std::string v = get(k); return v.empty() ? -1 : ((v == "1" || v == "true") ? 1 : 0); };
    RemoteSend::Request req;
    req.plate          = plate;
    req.printer        = get("printer");
    req.mode           = get("mode").empty() ? "upload" : get("mode");
    req.confirm        = get("confirm") == "1";
    req.force          = get("force") == "1";
    req.dry_run        = get("dry_run") == "1";
    req.bed_leveling   = tri("bed_leveling");
    req.flow_cali      = tri("flow_cali");
    req.vibration_cali = tri("vibration_cali");
    req.timelapse      = tri("timelapse");
    req.use_ams        = tri("use_ams");
    req.name           = get("name");
    req.mapping        = get("mapping");
    req.unload_at_end  = get("unload_at_end");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_send_running) { r.status = 409; r.body = json_error("a send is already running; wait for it to finish"); return r; }
        m_send_running = true; // reserved until the job ends, or preparing fails below
    }
    auto prepared = std::make_shared<std::shared_ptr<RemoteSend::Prepared>>();
    auto result   = std::make_shared<std::pair<int, std::string>>(500, "not run");
    take_error();
    // A Bambu printer other than the selected one is connected first; its status (SD card, busy)
    // arrives a moment later, and the GUI thread must not be blocked while it does.
    auto wait = std::make_shared<bool>(false);
    bool ok   = run_on_main([result, wait, req]() { *result = RemoteSend::preselect(req, *wait); }, 30000, "selecting a printer");
    if (ok && result->first == 200 && *wait) {
        auto ready = std::make_shared<bool>(false);
        for (int i = 0; i < 30 && !*ready; ++i) { // up to 15 s
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!run_on_main([ready, req]() { *ready = RemoteSend::printer_ready(req.printer); }, 5000, "checking a printer")) break;
        }
        if (*ready) std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // the rest of the first push
    }
    // Exporting the plate's 3mf is part of preparing and can take a while on a big plate.
    if (ok && result->first == 200)
        ok = run_on_main([prepared, result, req]() { *result = RemoteSend::prepare(req, *prepared); }, 5 * 60 * 1000, "preparing a send");
    if (!ok || result->first != 200 || !*prepared) {
        { std::lock_guard<std::mutex> lock(m_mutex); m_send_running = false; }
        if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
        const std::string shown = take_error();
        r.status = result->first == 200 ? 500 : result->first;
        r.body   = json_error(result->second + (shown.empty() ? "" : ": " + shown));
        return r;
    }
    Job job;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        job.id      = m_next_job++;
        job.plate   = plate;
        job.kind    = "send";
        job.state   = "running";
        job.printer = req.printer;
        job.mode    = req.mode;
        job.text    = "starting";
        m_jobs.push_back(job);
        if (m_jobs.size() > 50)
            m_jobs.erase(m_jobs.begin());
    }
    const int        id = job.id;
    RemoteSend::Sink sink;
    sink.progress = [this, id](int pct, const std::string& text) { update_job(id, pct, text); };
    sink.done     = [this, id](bool ok, const std::string& error, const nlohmann::json& res) { finish_job(id, ok, error, res); };
    std::shared_ptr<RemoteSend::Prepared> p = *prepared;
    std::thread([p, sink]() { RemoteSend::run(p, sink); }).detach();
    nlohmann::json j;
    j["job"]     = id;
    j["plate"]   = plate;
    j["kind"]    = p->kind;
    j["printer"] = p->printer_name;
    j["mode"]    = req.mode;
    j["dry_run"] = p->dry_run;
    r.body       = j.dump();
    return r;
}

// Pause, resume or stop the print on a printer, the way the desktop's own Pause / Resume / Stop
// buttons do (RemoteControl). Form: action=pause|resume|stop[&confirm=1][&dry_run=1]; stop needs
// the confirmation because it throws the print away. Returns a job id; the command and the few
// seconds of watching what the printer then reports run on their own thread, so the phone's
// request comes straight back. Nothing here can start a print.
//
// The same route carries the printer-error actions - the buttons the desktop's error dialog draws
// for one specific code, the subset of them a person can judge without standing at the printer:
// action=resume_error|stop_error|ignore_error|idle_ignore_error|ack_close&err=<code>. Those need
// `err` and are refused (409) unless it is still the code the printer is reporting; the verbs this
// phase keeps desktop-only are refused 403. Which are offered for a given error is in that
// printer's print_error.actions on GET /api/printers.
//
// And the settings the desktop Device tab changes, for the phone's native printer screen:
// action=set_temp&heater=<bed|chamber|nozzle<N>>&target=<C>[&confirm=1] (confirm=1 above 50 C while
// the printer is idle), action=set_speed&value=<1..4 | %>, action=set_light&on=<1|0> and
// action=set_fan&fan=<id>&percent=<0..100>. Which a printer takes, with its limits, is that row's
// `controls` on GET /api/printers; anything else is refused before it is sent.
RemoteAccess::ApiResponse RemoteAccess::api_printer_control(const std::string& printer, const std::string& form_body)
{
    ApiResponse r;
    auto get = [&](const char* k) { return query_param(form_body, k); };
    RemoteControl::Request req;
    req.printer = printer;
    req.action  = get("action");
    req.confirm = get("confirm") == "1";
    req.dry_run = get("dry_run") == "1";
    // The error code a printer-error action is answering. Required by those verbs and checked
    // against what the printer is reporting right now (RemoteControl::prepare), so a status page
    // left open cannot resume an error that has since been replaced by another one.
    req.err     = get("err");
    // The settings verbs (set_temp | set_speed | set_light | set_fan): what to set and to what.
    // Checked against the printer's own `controls` in RemoteControl::prepare, never trusted here.
    req.heater  = get("heater");
    req.target  = get("target");
    req.value   = get("value");
    req.fan     = get("fan");
    req.percent = get("percent");
    req.on      = get("on");
    // load_filament / unload_filament: which AMS / spool and slot (Bambu), or which toolhead (U1).
    req.ams     = get("ams");
    req.slot    = get("slot");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_control_running) { r.status = 409; r.body = json_error("another pause / resume / stop is still running"); return r; }
        m_control_running = true; // reserved until the job ends, or preparing fails below
    }
    auto prepared = std::make_shared<std::shared_ptr<RemoteControl::Prepared>>();
    auto result   = std::make_shared<std::pair<int, std::string>>(500, "not run");
    take_error();
    const bool ok = run_on_main([prepared, result, req]() { *result = RemoteControl::prepare(req, *prepared); }, 20000, "a printer control command");
    if (!ok || result->first != 200 || !*prepared) {
        { std::lock_guard<std::mutex> lock(m_mutex); m_control_running = false; }
        if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
        const std::string shown = take_error();
        r.status = result->first == 200 ? 500 : result->first;
        r.body   = json_error(result->second + (shown.empty() ? "" : ": " + shown));
        return r;
    }
    Job job;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        job.id      = m_next_job++;
        job.kind    = "control";
        job.state   = "running";
        job.printer = req.printer;
        job.mode    = req.action;
        job.text    = "sending " + req.action;
        m_jobs.push_back(job);
        if (m_jobs.size() > 50)
            m_jobs.erase(m_jobs.begin());
    }
    const int           id = job.id;
    RemoteControl::Sink sink;
    sink.progress = [this, id](int pct, const std::string& text) { update_job(id, pct, text); };
    sink.done     = [this, id](bool ok2, const std::string& error, const nlohmann::json& res) { finish_job(id, ok2, error, res); };
    std::shared_ptr<RemoteControl::Prepared> p = *prepared;
    std::thread([p, sink]() { RemoteControl::run(p, sink); }).detach();
    nlohmann::json j;
    j["job"]     = id;
    j["kind"]    = p->kind;
    j["printer"] = p->printer_id;
    j["name"]    = p->printer_name;
    j["action"]  = p->action;
    j["dry_run"] = p->dry_run;
    r.body       = j.dump();
    return r;
}

// ------------------------------------------------------- Snapmaker connect ----

// The Snapmaker printers on the LAN, with what each is doing right now. They are found four ways:
// the printers this PC has paired on its Device tab, an mDNS pass, the Stream tab's cameras, and
// what someone typed in on the phone. Nothing here needs the PC to be connected to a printer - the
// connect the Device tab makes is reported alongside, for the phone's secondary control.
RemoteAccess::ApiResponse RemoteAccess::api_snapmaker_devices()
{
    // The app's own device list lives on the GUI thread; everything else is network work.
    auto connect = std::make_shared<nlohmann::json>();
    bool ok      = run_on_main([connect]() {
        try { SnapmakerLan::merge_app_devices(); } catch (...) {}
        RemoteSnapmaker::list(*connect);
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    try { SnapmakerLan::start_discovery(); } catch (...) {}      // mDNS, at most once a minute
    try { SnapmakerLan::merge_stream_devices(); } catch (...) {} // the Stream tab's cameras
    nlohmann::json out;
    SnapmakerLan::list_json(out);
    // The PC-side MQTT connect: optional, and only interesting for the printers it knows.
    nlohmann::json c;
    c["connected"]      = (*connect)["connected"];
    c["host"]           = (*connect)["host"];
    c["devices"]        = (*connect)["devices"];
    out["connect"]        = c;
    out["printer_preset"] = (*connect)["printer_preset"];
    out["is_snapmaker"]   = (*connect)["is_snapmaker"];
    r.body = out.dump();
    return r;
}

// Add a printer by address: it has to answer as a Snapmaker before it is remembered.
RemoteAccess::ApiResponse RemoteAccess::api_snapmaker_add(const std::string& ip, int port)
{
    ApiResponse r;
    if (ip.empty()) { r.status = 400; r.body = json_error("ip is required"); return r; }
    SnapmakerLan::Device d;
    std::string          error;
    if (!SnapmakerLan::add(ip, port, d, error)) { r.status = 404; r.body = json_error(error); return r; }
    nlohmann::json j;
    j["ok"]     = true;
    j["id"]     = d.id;
    j["name"]   = d.name;
    j["model"]  = d.model;
    j["ip"]     = d.ip;
    r.body      = j.dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_snapmaker_remove(const std::string& id)
{
    ApiResponse r;
    if (id.empty()) { r.status = 400; r.body = json_error("id is required"); return r; }
    if (!SnapmakerLan::remove(id)) { r.status = 404; r.body = json_error("no such printer: " + id); return r; }
    r.body = "{\"ok\":true}";
    return r;
}

// Connect / disconnect a paired Snapmaker the way a pick in the PC's Device page does. Blocks on
// the MQTT connect (this is a request thread, not the GUI thread).
RemoteAccess::ApiResponse RemoteAccess::api_snapmaker_connect(const std::string& id)
{
    ApiResponse r;
    if (id.empty()) { r.status = 400; r.body = json_error("id is required (see /api/snapmaker/devices)"); return r; }
    const std::pair<int, std::string> res = RemoteSnapmaker::connect(id);
    if (res.first != 200) { r.status = res.first; r.body = json_error(res.second); return r; }
    r.body = "{\"ok\":true}";
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_snapmaker_disconnect()
{
    ApiResponse r;
    const std::pair<int, std::string> res = RemoteSnapmaker::disconnect();
    if (res.first != 200) { r.status = res.first; r.body = json_error(res.second); return r; }
    r.body = "{\"ok\":true}";
    return r;
}

void RemoteAccess::update_job(int id, int percent, const std::string& text)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Job& j : m_jobs)
        if (j.id == id && j.state == "running") {
            if (percent >= 0) j.percent = std::min(percent, 99);
            j.text = text;
        }
}

void RemoteAccess::finish_job(int id, bool ok, const std::string& error, const nlohmann::json& result)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Job& j : m_jobs)
        if (j.id == id && j.state == "running") {
            j.state   = ok ? "done" : "error";
            j.percent = ok ? 100 : j.percent;
            j.error   = error;
            j.result  = result;
            const bool dry = result.is_object() && result.value("dry_run", false);
            if (ok) j.text = j.kind != "control" ? "sent" : dry ? "dry run: nothing was sent" : "sent to the printer";
            // Sends and controls each have their own single-flight flag; release this job's.
            if (j.kind == "control") m_control_running = false;
            else                     m_send_running = false;
        }
    BOOST_LOG_TRIVIAL(info) << "RemoteAccess: job " << id << (ok ? " done" : " failed: " + error);
}

// The sidebar's own combo boxes are the source of truth for "what can be picked" and selecting
// through them runs the exact code path a click in the sidebar does (Plater::priv::on_select_preset).
static bool is_label_item(PresetComboBox* combo, unsigned int i)
{
    const size_t marker = reinterpret_cast<size_t>(combo->GetClientData(i));
    return marker >= PresetComboBox::LABEL_ITEM_MARKER && marker < PresetComboBox::LABEL_ITEM_MAX;
}

static std::string combo_item_value(PresetComboBox* combo, unsigned int i, Preset::Type type)
{
    const std::string display = combo->GetString(i).ToUTF8().data();
    return wxGetApp().preset_bundle->get_preset_name_by_alias(type, Preset::remove_suffix_modified(display));
}

// The process preset has no sidebar combo in this fork; its list lives on the Process tab.
static PresetComboBox* process_combo()
{
    Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
    return tab ? tab->get_combo_box() : nullptr;
}

static nlohmann::json combo_items(PresetComboBox* combo, Preset::Type type)
{
    nlohmann::json items = nlohmann::json::array();
    if (!combo)
        return items;
    const int sel = combo->GetSelection();
    for (unsigned int i = 0; i < combo->GetCount(); ++i) {
        if (is_label_item(combo, i))
            continue;
        nlohmann::json it;
        it["name"]     = combo->GetString(i).ToUTF8().data();
        it["value"]    = combo_item_value(combo, i, type);
        it["selected"] = ((int) i == sel);
        items.push_back(it);
    }
    return items;
}

RemoteAccess::ApiResponse RemoteAccess::api_presets()
{
    auto out = std::make_shared<nlohmann::json>();
    bool ok  = run_on_main([out]() {
        nlohmann::json& j      = *out;
        Sidebar&        sb     = wxGetApp().sidebar();
        PresetBundle*   bundle = wxGetApp().preset_bundle;
        j["printer"]      = combo_items(sb.combo_printer(), Preset::TYPE_PRINTER);
        j["process"]      = combo_items(process_combo(), Preset::TYPE_PRINT);
        j["printer_name"] = bundle->printers.get_selected_preset_name();
        j["process_name"] = bundle->prints.get_selected_preset_name();
        auto& combos = sb.combos_filament();
        j["filament_choices"] = combos.empty() ? nlohmann::json::array() : combo_items(combos.front(), Preset::TYPE_FILAMENT);
        j["filaments"]        = nlohmann::json::array();
        const ConfigOptionStrings* colors = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        for (size_t i = 0; i < bundle->filament_presets.size(); ++i) {
            nlohmann::json f;
            f["index"] = i;
            f["value"] = bundle->filament_presets[i];
            f["color"] = (colors && i < colors->values.size()) ? colors->values[i] : "";
            j["filaments"].push_back(f);
        }
        // The sidebar's nozzle notebook: the visible variants of this printer model, and the
        // one in use (Sidebar::update_nozzle_settings reads printer_variant, not nozzle_diameter).
        nlohmann::json nozzles;
        nozzles["choices"] = nlohmann::json::array();
        for (const std::string& d : bundle->printers.diameters_of_selected_printer())
            nozzles["choices"].push_back(d);
        const ConfigOptionString* variant = bundle->printers.get_edited_preset().config.option<ConfigOptionString>("printer_variant");
        nozzles["current"] = variant ? variant->value : "";
        j["nozzles"]       = nozzles;
        j["dirty"]["printer"]  = bundle->printers.current_is_dirty();
        j["dirty"]["process"]  = bundle->prints.current_is_dirty();
        j["dirty"]["filament"] = bundle->filaments.current_is_dirty();
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); }
    else       r.body = out->dump();
    return r;
}

// Unsaved modifications of a preset collection, ready to be re-applied to the newly selected
// preset (what the transfer/discard dialog's "Transfer" button does).
static DynamicPrintConfig capture_dirty(PresetCollection& presets)
{
    DynamicPrintConfig dirty;
    if (!presets.current_is_dirty())
        return dirty;
    const Preset& edited = presets.get_edited_preset();
    for (const std::string& opt : presets.current_dirty_options())
        if (const ConfigOption* o = edited.config.option(opt))
            dirty.set_key_value(opt, o->clone());
    return dirty;
}

// Phone selections never raise the transfer/discard dialog: modifications are carried over to
// the new preset (they stay "modified" there, so Revert on the PC still discards them).
RemoteAccess::ApiResponse RemoteAccess::api_select_preset(const std::string& type, const std::string& name_in, int index)
{
    auto result = std::make_shared<std::pair<int, std::string>>(500, "");
    bool ok     = run_on_main([result, type, name_in, index]() {
        const std::string& name = name_in;
        Sidebar&      sb     = wxGetApp().sidebar();
        Plater*       plater = wxGetApp().plater();
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (plater->is_background_process_slicing()) { *result = { 409, "slicing in progress" }; return; }
        auto reapply = [](Preset::Type t, const DynamicPrintConfig& dirty) {
            if (!dirty.empty())
                wxGetApp().get_tab(t)->load_config(dirty);
        };
        if (type == "printer") {
            std::string name = name_in;
            if (name == bundle->printers.get_selected_preset_name()) { *result = { 200, "" }; return; }
            if (!bundle->printers.find_preset(name)) {
                // The sidebar lists printer models ("Bambu Lab H2D") as well as presets; resolve a
                // model to its preset the way Plater::priv::on_select_preset does.
                Preset* similar = bundle->get_similar_printer_preset(name, {});
                if (!similar) { *result = { 404, "preset not in the list: " + name }; return; }
                similar->is_visible = true;
                name = similar->name;
                if (name == bundle->printers.get_selected_preset_name()) { *result = { 200, "" }; return; }
            }
            const DynamicPrintConfig printer_dirty = capture_dirty(bundle->printers);
            const DynamicPrintConfig process_dirty = capture_dirty(bundle->prints);
            const DynamicPrintConfig filament_dirty = capture_dirty(bundle->filaments);
            bundle->physical_printers.unselect_printer();
            // force_select: no dialogs; incompatible process/filament presets are remapped silently.
            wxGetApp().get_tab(Preset::TYPE_PRINTER)->select_preset(name, false, "", true);
            reapply(Preset::TYPE_PRINTER, printer_dirty);
            reapply(Preset::TYPE_PRINT, process_dirty);
            reapply(Preset::TYPE_FILAMENT, filament_dirty);
            plater->on_config_change(bundle->full_config());
            wxGetApp().app_config->set("preferred_printer", bundle->printers.get_selected_preset_name());
            // What the sidebar's own printer combo does after select_preset and the API did not:
            // rebuild the nozzle notebook, and let the Device tab follow (a Snapmaker device page
            // is only loaded again when asked - update_all_preset_comboboxes latches it).
            sb.update_all_preset_comboboxes(true);
            sb.update_nozzle_settings(true);
            *result = { 200, "" };
        } else if (type == "nozzle") {
            // The sidebar's nozzle combo: the same printer preset with another variant. force so a
            // phone selection never raises the transfer/discard dialog, then the two refreshes the
            // desktop's own device-driven nozzle sync performs (Plater.cpp ~2252).
            // get_similar_printer_preset falls back to *some* preset of the model when nothing
            // matches, so the choice is checked against the list the combo offers first.
            const std::vector<std::string> choices = bundle->printers.diameters_of_selected_printer();
            if (std::find(choices.begin(), choices.end(), name) == choices.end()) {
                *result = { 404, "this printer has no " + name + " nozzle (see /api/presets nozzles.choices)" };
                return;
            }
            Preset* preset = bundle->get_similar_printer_preset({}, name);
            if (!preset) { *result = { 404, "no printer preset with nozzle " + name }; return; }
            preset->is_visible = true;
            if (preset->name == bundle->printers.get_selected_preset_name()) { *result = { 200, "" }; return; }
            const DynamicPrintConfig printer_dirty  = capture_dirty(bundle->printers);
            const DynamicPrintConfig process_dirty  = capture_dirty(bundle->prints);
            const DynamicPrintConfig filament_dirty = capture_dirty(bundle->filaments);
            wxGetApp().get_tab(Preset::TYPE_PRINTER)->select_preset(preset->name, false, "", true);
            reapply(Preset::TYPE_PRINTER, printer_dirty);
            reapply(Preset::TYPE_PRINT, process_dirty);
            reapply(Preset::TYPE_FILAMENT, filament_dirty);
            plater->on_config_change(bundle->full_config());
            wxGetApp().app_config->set("preferred_printer", bundle->printers.get_selected_preset_name());
            sb.update_all_preset_comboboxes(true);
            sb.update_nozzle_settings(true);
            *result = { 200, "" };
        } else if (type == "process") {
            if (name == bundle->prints.get_selected_preset_name()) { *result = { 200, "" }; return; }
            if (!bundle->prints.find_preset(name)) { *result = { 404, "preset not in the list: " + name }; return; }
            const DynamicPrintConfig process_dirty = capture_dirty(bundle->prints);
            wxGetApp().get_tab(Preset::TYPE_PRINT)->select_preset(name, false, "", true);
            reapply(Preset::TYPE_PRINT, process_dirty);
            plater->on_config_change(bundle->full_config());
            plater->record_preferred_print_profile();
            *result = { 200, "" };
        } else if (type == "filament") {
            auto& combos = sb.combos_filament();
            if (index < 0 || index >= (int) combos.size()) { *result = { 404, "no such filament slot" }; return; }
            if (index < (int) bundle->filament_presets.size() && bundle->filament_presets[index] == name) { *result = { 200, "" }; return; }
            if (!bundle->filaments.find_preset(name)) { *result = { 404, "preset not in the list: " + name }; return; }
            // Same steps as Plater::priv::on_select_preset for TYPE_FILAMENT, with force on the tab.
            const DynamicPrintConfig filament_dirty = capture_dirty(bundle->filaments);
            bundle->set_filament_preset(index, name);
            plater->update_project_dirty_from_presets();
            bundle->export_selections(*wxGetApp().app_config);
            sb.update_dynamic_filament_list();
            sb.update_color_mix_panel();
            if (sb.is_multifilament())
                combos[index]->update();
            else {
                wxGetApp().get_tab(Preset::TYPE_FILAMENT)->select_preset(name, false, "", true);
                reapply(Preset::TYPE_FILAMENT, filament_dirty);
            }
            plater->on_config_change(bundle->full_config());
            *result = { 200, "" };
        } else {
            *result = { 404, "type must be printer, nozzle, process or filament" };
        }
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (result->first != 200) { r.status = result->first; r.body = json_error(result->second); return r; }
    r.body = "{\"ok\":true}";
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_filament_color(int index, const std::string& color)
{
    ApiResponse r;
    if (color.size() != 7 || color[0] != '#' || color.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos) {
        r.status = 404; r.body = json_error("color must be #RRGGBB"); return r;
    }
    auto result = std::make_shared<int>(500);
    bool ok     = run_on_main([result, index, color]() {
        auto& combos = wxGetApp().sidebar().combos_filament();
        if (index < 0 || index >= (int) combos.size()) { *result = 404; return; }
        combos[index]->ApplyFilamentColor(FilamentColor::FromColors({ color }, FilamentColorMode::Segment));
        *result = 200;
    });
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (*result != 200) { r.status = *result; r.body = json_error("no such filament slot"); return r; }
    r.body = "{\"ok\":true}";
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_filament_add()
{
    auto count = std::make_shared<size_t>(0);
    bool ok    = run_on_main([count]() {
        wxGetApp().sidebar().add_filament();
        *count = wxGetApp().preset_bundle->filament_presets.size();
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    nlohmann::json j;
    j["filaments"] = *count;
    r.body = j.dump();
    return r;
}

// ------------------------------------------------- process settings editor ----

static const char* option_type_name(ConfigOptionType t)
{
    switch (t) {
    case coFloat: return "float";           case coFloats: return "floats";
    case coInt: return "int";               case coInts: return "ints";
    case coString: return "string";         case coStrings: return "strings";
    case coPercent: return "percent";       case coPercents: return "percents";
    case coFloatOrPercent: return "float_or_percent"; case coFloatsOrPercents: return "floats_or_percents";
    case coPoint: return "point";           case coPoints: return "points";
    case coBool: return "bool";             case coBools: return "bools";
    case coEnum: return "enum";             case coEnums: return "enums";
    default: return "other";
    }
}

static const char* mode_name(ConfigOptionMode m)
{
    return m == comSimple ? "simple" : m == comAdvanced ? "advanced" : "develop";
}

static nlohmann::json process_state_json()
{
    nlohmann::json j;
    PresetBundle* bundle = wxGetApp().preset_bundle;
    const Preset& edited = bundle->prints.get_edited_preset();
    j["preset"]    = edited.name;
    j["is_system"] = edited.is_system;
    j["dirty"]     = bundle->prints.current_dirty_options();
    return j;
}

// The Process tab's pages / option groups / lines, with the definition, current value and the
// last-saved value of every option — the phone renders exactly the slicer's layout from this.
RemoteAccess::ApiResponse RemoteAccess::api_process_settings()
{
    auto out = std::make_shared<nlohmann::json>();
    bool ok  = run_on_main([out]() {
        nlohmann::json& j      = *out;
        PresetBundle*   bundle = wxGetApp().preset_bundle;
        Tab*            tab    = wxGetApp().get_tab(Preset::TYPE_PRINT);
        if (!tab) return;
        j        = process_state_json();
        j["mode"] = mode_name(wxGetApp().get_mode());
        const DynamicPrintConfig& edited = bundle->prints.get_edited_preset().config;
        const DynamicPrintConfig& saved  = bundle->prints.get_selected_preset().config;
        std::set<std::string> dirty;
        for (const std::string& k : bundle->prints.current_dirty_options()) dirty.insert(k);
        j["pages"] = nlohmann::json::array();
        for (const PageShp& page : tab->get_pages()) {
            nlohmann::json jp;
            jp["title"]  = page->title().ToUTF8().data();
            jp["groups"] = nlohmann::json::array();
            for (const ConfigOptionsGroupShp& group : page->m_optgroups) {
                nlohmann::json jg;
                jg["title"] = group->title.ToUTF8().data();
                jg["lines"] = nlohmann::json::array();
                for (const Line& line : group->get_lines()) {
                    if (line.is_separator()) continue;
                    nlohmann::json jl;
                    jl["label"]   = line.label.ToUTF8().data();
                    jl["tooltip"] = line.label_tooltip.ToUTF8().data();
                    jl["options"] = nlohmann::json::array();
                    for (const Option& opt : line.get_options()) {
                        std::string key = opt.opt_id;
                        int         idx = -1;
                        const size_t hash = key.find('#');
                        if (hash != std::string::npos) { idx = std::atoi(key.c_str() + hash + 1); key = key.substr(0, hash); }
                        const ConfigOption* cur = edited.option(key);
                        if (!cur) continue;
                        const ConfigOption* sav = saved.option(key);
                        const ConfigOptionDef& def = opt.opt;
                        nlohmann::json jo;
                        jo["key"]     = key;
                        if (idx >= 0) jo["index"] = idx;
                        jo["label"]   = _(def.label).ToUTF8().data();
                        jo["tooltip"] = _(def.tooltip).ToUTF8().data();
                        jo["type"]    = option_type_name(def.type);
                        jo["unit"]    = _(def.sidetext).ToUTF8().data();
                        jo["mode"]    = mode_name(def.mode);
                        jo["readonly"] = def.readonly || opt.readonly;
                        if (def.min != INT_MIN) jo["min"] = def.min;
                        if (def.max != INT_MAX) jo["max"] = def.max;
                        if (!def.enum_values.empty()) {
                            jo["enum"] = nlohmann::json::array();
                            for (size_t e = 0; e < def.enum_values.size(); ++e) {
                                nlohmann::json je;
                                je["value"] = def.enum_values[e];
                                je["label"] = e < def.enum_labels.size() ? _(def.enum_labels[e]).ToUTF8().data() : def.enum_values[e];
                                jo["enum"].push_back(je);
                            }
                        }
                        jo["value"] = cur->serialize();
                        jo["saved"] = sav ? sav->serialize() : "";
                        jo["dirty"] = dirty.count(key) > 0;
                        jl["options"].push_back(jo);
                    }
                    if (!jl["options"].empty())
                        jg["lines"].push_back(jl);
                }
                if (!jg["lines"].empty())
                    jp["groups"].push_back(jg);
            }
            if (!jp["groups"].empty())
                j["pages"].push_back(jp);
        }
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); }
    else if (out->empty()) { r.status = 404; r.body = json_error("no process tab"); }
    else r.body = out->dump();
    return r;
}

// Apply key=value pairs (form-encoded, serialized option values) to the edited process
// preset, exactly like typing them into the Process tab; the preset becomes "modified".
RemoteAccess::ApiResponse RemoteAccess::api_process_set(const std::string& form_body)
{
    std::vector<std::pair<std::string, std::string>> pairs;
    size_t pos = 0;
    while (pos <= form_body.size()) {
        size_t amp = form_body.find('&', pos);
        if (amp == std::string::npos) amp = form_body.size();
        const std::string kv = form_body.substr(pos, amp - pos);
        const size_t      eq = kv.find('=');
        if (eq != std::string::npos && eq > 0)
            pairs.emplace_back(percent_decode(kv.substr(0, eq)), percent_decode(kv.substr(eq + 1)));
        pos = amp + 1;
    }
    ApiResponse r;
    if (pairs.empty()) { r.status = 404; r.body = json_error("no key=value pairs"); return r; }
    auto result = std::make_shared<std::pair<int, std::string>>(500, "");
    auto state  = std::make_shared<nlohmann::json>();
    bool ok     = run_on_main([result, state, pairs]() {
        Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
        if (!tab) { *result = { 404, "no process tab" }; return; }
        DynamicPrintConfig cfg;
        for (const auto& kv : pairs) {
            if (!print_config_def.has(kv.first)) { *result = { 404, "unknown setting: " + kv.first }; return; }
            try {
                cfg.set_deserialize_strict(kv.first, kv.second);
            } catch (const std::exception& e) {
                *result = { 400, "bad value for " + kv.first + ": " + e.what() };
                return;
            }
        }
        const auto t0 = std::chrono::steady_clock::now();
        tab->load_config(cfg);
        const auto t1 = std::chrono::steady_clock::now();
        wxGetApp().plater()->on_config_change(cfg);
        const auto t2 = std::chrono::steady_clock::now();
        BOOST_LOG_TRIVIAL(info) << "RemoteAccess: process settings applied, load_config "
                                << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() << " ms, on_config_change "
                                << std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count() << " ms";
        *state  = process_state_json();
        *result = { 200, "" };
    }, 60000); // the first apply after a project load has been seen to take >15 s
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (result->first != 200) { r.status = result->first; r.body = json_error(result->second); return r; }
    r.body = state->dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_process_revert()
{
    auto state = std::make_shared<nlohmann::json>();
    bool ok    = run_on_main([state]() {
        Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
        if (!tab) return;
        tab->on_roll_back_value(false); // "reset all settings to the last saved preset"
        wxGetApp().plater()->on_config_change(wxGetApp().preset_bundle->full_config());
        *state = process_state_json();
    }, 60000);
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); }
    else       r.body = state->dump();
    return r;
}

// Save under the same name; a system preset saves to its "<name> - Custom" shadow (the fork's
// own no-prompt rule in Tab::save_preset), which then becomes the selected preset.
RemoteAccess::ApiResponse RemoteAccess::api_process_save()
{
    auto state = std::make_shared<nlohmann::json>();
    bool ok    = run_on_main([state]() {
        Tab*          tab    = wxGetApp().get_tab(Preset::TYPE_PRINT);
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (!tab) return;
        const Preset& edited = bundle->prints.get_edited_preset();
        if (edited.is_dirty)
            // An explicit name: SavePresetDialog (a text field nobody can fill) is never constructed,
            // whatever the auto_shadow_system_presets preference says.
            tab->save_preset(edited.is_system ? edited.name + " - Custom" : edited.name);
        *state = process_state_json();
    }, 30000);
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); }
    else       r.body = state->dump();
    return r;
}

// --------------------------------------------------------- instance + files ----

// Cheap (no GUI thread): what the hub lists for this instance.
RemoteAccess::ApiResponse RemoteAccess::api_info()
{
    ApiResponse r;
    nlohmann::json j;
    std::lock_guard<std::mutex> lock(m_mutex);
    j["pid"]     = (long) wxGetProcessId();
    j["port"]    = m_port;
    j["title"]   = m_title;
    j["path"]    = m_path;
    j["slicing"] = m_slicing;
    j["hidden"]  = m_hidden;
    j["version"] = std::string(SLIC3R_VERSION);
    // Where this PC keeps the G-code archive. The hub reads it so that a running job's
    // thumbnail still resolves once every slicer window is closed (GET /r/<token>/printers/
    // <id>/thumbnail.png): the sidecars are plain files, but only an instance knows which
    // folder the preference points at. A path on this PC, and this API is loopback-only.
    j["archive_dir"] = GcodeArchive::dir();
    // And whether it is on, and how many records it keeps, so the hub can list the archive itself
    // (GET /r/<token>/api/archive) with the same "storing is off" note the instance's own list has.
    j["archive_enabled"] = GcodeArchive::enabled();
    j["archive_max"]     = GcodeArchive::max_records();
    j["needs_attention"]  = m_needs_attention;
    j["attention_reason"] = m_attention_reason;
    j["attention_kind"]   = m_attention_kind;
    j["attention_since"]  = m_attention_since;
    j["modal_open"]       = m_modal_depth;
    j["gui_stall_ms"]     = m_gui_tick_ms ? now_ms() - m_gui_tick_ms : 0;
    j["attention"]        = nlohmann::json::array();
    for (const Attention& a : m_attention)
        j["attention"].push_back({ {"time", a.time}, {"dialog", a.dialog}, {"answered", a.answered} });
    r.body       = j.dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_attention_clear()
{
    clear_attention("dismissed from the phone");
    return api_info();
}

// What this instance's watcher has seen (RemoteEvents). The hub keeps the real, merged history;
// this ring is per instance and is what a person debugging one printer wants to read.
RemoteAccess::ApiResponse RemoteAccess::api_events(int since)
{
    ApiResponse r;
    r.body = RemoteEvents::recent(since).dump();
    return r;
}

// Test hooks, only with SNORCA_DEBUG_ROUTES=1: sleep = block the GUI thread (watchdog), modal =
// a plain wxMessageDialog through the policy hook, file = a wxFileDialog (needs a person),
// events = run the event watcher's transition rule over snapshots from the body (no printers).
RemoteAccess::ApiResponse RemoteAccess::api_debug(const std::string& what, const std::string& query, const std::string& body)
{
    ApiResponse r;
    wxString    on;
    if (!wxGetEnv("SNORCA_DEBUG_ROUTES", &on) || on != "1") { r.status = 404; r.body = json_error("debug routes are off"); return r; }
    if (what == "hms") {
        // Which HMS table a serial picks and what that table says about one code - the same
        // lookup snapshot_bambu does, with every piece that decides it spelled out.
        //   /api/debug/hms?dev=<serial>&code=<8 or 16 hex>[&lang=<code>]
        // `local_*` reads the shipped and downloaded tables directly; `live_*` goes through the
        // app's own HMSQuery, which also refreshes from the cloud in the background. They differ
        // only when the app has no HMSQuery at all (stealth mode, GUI_App.cpp) or the app's
        // language is not the one asked for.
        const std::string dev  = query_param(query, "dev");
        std::string       code = query_param(query, "code");
        for (char& c : code) c = (char) ::toupper((unsigned char) c);
        std::string lang = query_param(query, "lang");
        if (lang.empty()) lang = HMSQuery::hms_language_code();
        const std::string   series = HMSQuery::get_dev_id_type(dev);
        const unsigned long print_error = std::strtoul(code.c_str(), nullptr, 16);
        nlohmann::json      out;
        out["dev_id"]        = dev;
        out["dev_id_type"]   = series;
        out["code"]          = code;
        out["lang"]          = lang;
        out["app_lang"]      = HMSQuery::hms_language_code();
        out["file"]          = HMSQuery::get_hms_file(QUERY_HMS_INFO, lang, series);
        out["data_dir"]      = data_dir();
        out["resources_dir"] = resources_dir();
        {
            HMSQuery probe;
            wxString msg;
            const bool found = code.size() > 8 ? !(msg = probe.query_hms_msg_local(dev, code, lang)).IsEmpty()
                                               : probe.query_print_error_msg_local(dev, (int) print_error, lang, msg);
            out["local_found"] = found;
            out["local_text"]  = std::string(msg.ToUTF8().data());
        }
        HMSQuery* q     = wxGetApp().get_hms_query();
        out["has_query"] = (q != nullptr);
        if (q) {
            wxString msg;
            const bool found = code.size() > 8 ? !(msg = q->query_hms_msg(dev, code)).IsEmpty()
                                               : q->query_print_error_msg(dev, (int) print_error, msg);
            out["live_found"] = found;
            out["live_text"]  = std::string(msg.ToUTF8().data());
        }
        r.body = out.dump();
        return r;
    }
    if (what == "events") {
        // Pure: no GUI thread, no printer, no clock. This is how the transition rule is covered.
        try {
            r.body = RemoteEvents::replay(nlohmann::json::parse(body.empty() ? "{}" : body)).dump();
        } catch (const std::exception& e) {
            r.status = 400;
            r.body   = json_error(std::string("the snapshots are not JSON: ") + e.what());
        }
        return r;
    }
    auto out = std::make_shared<nlohmann::json>();
    int  ms  = 20000;
    try { const std::string v = query_param(query, "ms"); if (!v.empty()) ms = std::stoi(v); } catch (...) {}
    bool ok = run_on_main([out, what, ms]() {
        if (what == "sleep") {
            wxMilliSleep(ms);
            (*out)["slept_ms"] = ms;
        } else if (what == "modal") {
            wxMessageDialog dlg(wxGetApp().mainframe, "Debug: a question nobody should have to answer", "Debug modal", wxYES_NO | wxCANCEL);
            (*out)["answer"] = dlg.ShowModal();
        } else if (what == "file") {
            wxFileDialog dlg(wxGetApp().mainframe, "Debug: choose a file", "", "", "*.*", wxFD_OPEN);
            (*out)["answer"] = dlg.ShowModal();
        } else {
            (*out)["error"] = "unknown debug route";
        }
    }, 5000, "a debug request");
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    r.body = out->dump();
    return r;
}

// Show or hide this instance's window. Without `show` it only reports.
RemoteAccess::ApiResponse RemoteAccess::api_window(const std::string& show)
{
    ApiResponse r;
    const int want = show.empty() ? -1 : ((show == "0" || show == "false") ? 0 : 1);
    auto out = std::make_shared<nlohmann::json>();
    bool ok  = run_on_main([out, want]() {
        MainFrame* mf = wxGetApp().mainframe;
        if (mf == nullptr) return;
        if (want == 1) {
            if (mf->IsIconized()) mf->Iconize(false);
            mf->Show(true);
            mf->Raise();
#ifdef _WIN32
            ::SetForegroundWindow((HWND) mf->GetHandle()); // the hub called AllowSetForegroundWindow(pid)
#endif
        } else if (want == 0) {
            mf->Hide();
        }
        const bool hidden = !mf->IsShown();
        RemoteAccess::get().set_hidden(hidden);
        (*out)["hidden"]   = hidden;
        (*out)["iconized"] = mf->IsIconized();
    }, 5000);
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (out->empty()) { r.status = 503; r.body = json_error("no main window"); return r; }
    r.body = out->dump();
    return r;
}

// End this instance. discard=1 skips the unsaved-project handling; otherwise the usual close
// path runs and its auto-confirmed Yes saves - so an unnamed dirty project first gets a name
// under <datadir>/hub/saves (never a file dialog), exactly as api_project_open does.
RemoteAccess::ApiResponse RemoteAccess::api_quit(bool discard)
{
    ApiResponse r;
    auto result = std::make_shared<std::pair<int, std::string>>(500, "not run");
    bool ok     = run_on_main([result, discard]() {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr || wxGetApp().mainframe == nullptr) { *result = { 500, "no main window" }; return; }
        if (!discard && plater->is_background_process_slicing()) { *result = { 409, "slicing in progress" }; return; }
        if (!discard && plater->is_project_dirty() && plater->get_project_filename(".3mf").IsEmpty() && !plater->model().objects.empty()) {
            boost::system::error_code ig;
            fs::create_directories(RemoteHub::saves_dir(), ig);
            std::time_t t = std::time(nullptr);
            std::tm     tm {};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
            plater->set_project_filename(wxString::FromUTF8((fs::path(RemoteHub::saves_dir()) / (std::string("Untitled_") + stamp + ".3mf")).string()));
        }
        *result = { 200, "" };
    }, 15000);
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (result->first != 200) { r.status = result->first; r.body = json_error(result->second); return r; }
    // The close runs after this response has been written.
    post_to_main([discard]() {
        if (wxGetApp().mainframe) wxGetApp().mainframe->request_quit(discard);
    });
    r.body = "{\"ok\":true}";
    return r;
}

// Open a file the hub spooled from the phone. mode=load (.3mf only): the project on screen
// is saved first — to its own file, or to <datadir>/hub/saves when it was never saved —
// then the uploaded project replaces it. mode=import: the model is added to the current
// plate. Prompts along the way answer themselves (auto_confirm).
RemoteAccess::ApiResponse RemoteAccess::api_project_open(const std::string& path_in, const std::string& mode_in)
{
    ApiResponse r;
    boost::system::error_code ec;
    const fs::path path    = fs::weakly_canonical(fs::path(path_in), ec);
    const fs::path uploads = fs::weakly_canonical(fs::path(RemoteHub::uploads_dir()), ec);
    if (path_in.empty() || !fs::is_regular_file(path, ec)) { r.status = 404; r.body = json_error("no such file"); return r; }
    if (path.parent_path().parent_path() != uploads) { r.status = 400; r.body = json_error("only files uploaded through the hub can be opened"); return r; }
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    const std::string mode = mode_in.empty() ? (ext == ".3mf" ? "load" : "import") : mode_in;
    if (mode != "load" && mode != "import") { r.status = 400; r.body = json_error("mode must be load or import"); return r; }
    if (mode == "load" && ext != ".3mf") { r.status = 400; r.body = json_error("only a .3mf can be loaded as a project; use mode=import"); return r; }

    auto result = std::make_shared<std::pair<int, std::string>>(500, "");
    auto out    = std::make_shared<nlohmann::json>();
    take_error();
    bool ok     = run_on_main([result, out, path, mode]() {
        Plater* plater = wxGetApp().plater();
        if (plater->is_background_process_slicing()) { *result = { 409, "slicing in progress" }; return; }
        const wxString file = wxString::FromUTF8(path.string());
        if (mode == "load") {
            if (!plater->model().objects.empty() || plater->is_project_dirty()) {
                wxString current = plater->get_project_filename(".3mf");
                if (current.IsEmpty()) {
                    boost::system::error_code ig;
                    fs::create_directories(RemoteHub::saves_dir(), ig);
                    std::time_t t = std::time(nullptr);
                    std::tm     tm {};
#ifdef _WIN32
                    localtime_s(&tm, &t);
#else
                    localtime_r(&t, &tm);
#endif
                    char stamp[32];
                    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
                    current = wxString::FromUTF8((fs::path(RemoteHub::saves_dir()) / (std::string("Untitled_") + stamp + ".3mf")).string());
                    plater->set_project_filename(current);
                }
                if (plater->save_project(false) != wxID_YES) { *result = { 500, "could not save the current project" }; return; }
                (*out)["saved"] = current.ToUTF8().data();
            }
            // "<loadall>" skips the open-project-or-import-geometry question.
            plater->load_project(file, "<loadall>");
            boost::system::error_code ig;
            const fs::path now(plater->get_project_filename(".3mf").ToUTF8().data());
            if (!fs::exists(now, ig) || !fs::equivalent(now, path, ig)) { *result = { 500, "the project did not load" }; return; }
        } else {
            const std::vector<std::string> files { path.string() };
            const std::vector<size_t>      res = plater->load_files(files, LoadStrategy::LoadModel);
            if (res.empty()) { *result = { 500, "nothing was imported" }; return; }
            (*out)["objects"] = res.size();
            const ColorImport ci = RemoteAccess::get().take_color_import();
            if (ci.valid)
                (*out)["colors"] = { {"input", ci.input}, {"clusters", ci.clusters},
                                     {"reused", ci.reused}, {"added", ci.added}, {"merged", ci.merged} };
        }
        wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
        (*out)["project"] = plater->get_project_filename(".3mf").ToUTF8().data();
        *result           = { 200, "" };
    }, 15 * 60 * 1000); // big projects take a while to save and load
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    const std::string shown = take_error(); // what the slicer would have put in a dialog
    if (result->first != 200) { r.status = result->first; r.body = json_error(result->second + (shown.empty() ? "" : ": " + shown)); return r; }
    r.body = out->dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::handle_api(const std::string& method, const std::string& path, const std::string& query, const std::string& body)
{
    ApiResponse r;
    // The main window is going away: say so at once rather than queueing work for a GUI thread
    // that will not run it (and must not - the Plater may already be gone).
    if (gui_closing()) {
        r.status = 503;
        r.body   = json_error("the slicer is closing");
        return r;
    }
    auto num  = [](const std::string& s, int def) { try { return s.empty() ? def : std::stoi(s); } catch (...) { return def; } };
    auto numd = [](const std::string& s, double def) { try { return s.empty() ? def : std::stod(s); } catch (...) { return def; } };
    if (path.empty() || path == "/") {
        nlohmann::json j;
        j["name"]    = "EdgeSlicer remote API";
        j["version"] = 2;
        j["routes"]  = nlohmann::json::array({
            { {"method", "GET"},  {"path", "/api"},                        {"description", "this manifest"} },
            { {"method", "GET"},  {"path", "/api/info"},                   {"description", "this instance: pid, project title and path, slicing flag, hidden flag, archive_dir (where the G-code archive keeps its sidecars)"} },
            { {"method", "GET"},  {"path", "/api/window"},                 {"description", "is this instance's window shown? {hidden, iconized}"} },
            { {"method", "POST"}, {"path", "/api/window?show=1|0"},        {"description", "show (and raise) or hide this instance's window"} },
            { {"method", "POST"}, {"path", "/api/quit[?discard=1]"},       {"description", "close this instance; without discard the unsaved project is saved first (an unnamed one under <datadir>/hub/saves)"} },
            { {"method", "POST"}, {"path", "/api/attention/clear"},        {"description", "dismiss the needs-attention flag (see /api/info: needs_attention, attention_reason, attention[])"} },
            { {"method", "POST"}, {"path", "/api/project/open"},           {"description", "form body path={file uploaded through the hub}&mode=load|import; load (.3mf) saves the current project first, import adds the model to the plate"} },
            { {"method", "GET"},  {"path", "/api/plates"},                 {"description", "project, printer preset, filaments and every plate with objects, slice state, time and filament estimates"} },
            { {"method", "GET"},  {"path", "/api/plates/{index}/thumbnail.png"}, {"description", "rendered plate preview"} },
            { {"method", "GET"},  {"path", "/api/plates/{index}/layout"},  {"description", "top-down layout: plate box, exclude areas, every instance with its convex hull (mm), bbox, position, Z rotation, scale, colour"} },
            { {"method", "GET"},  {"path", "/api/plates/{index}/preview"}, {"description", "slice preview state: sliced flag, layer heights, current layer range; loads that plate's G-code into the PC's preview"} },
            { {"method", "GET"},  {"path", "/api/plates/{index}/preview.png?view=front|rear|left|right&layer={index}&w=&h=[&zoom=&cx=&cy=]"}, {"description", "orthographic render of the toolpaths up to that layer (the PC's layer slider follows); zoom over the fit and the fitted-image fraction shown at the centre; X-Preview-Zoom = zoom really used"} },
            { {"method", "GET"},  {"path", "/api/plates/{index}/preview/status"}, {"description", "sliced / slicing / slicing_percent / result_id for that plate, without changing what the PC shows"} },
            { {"method", "POST"}, {"path", "/api/objects/transform"},       {"description", "form obj=&inst=[&x=&y=][&rz=][&scale=][&center=1]: move / rotate / scale one instance like the sidebar (undoable)"} },
            { {"method", "GET"},  {"path", "/api/printers[?plate={index}]"}, {"description", "known printers with live status and what a send needs: kind bambu|printhost|connect|snapmaker, online, lan_mode, access_code_set, sdcard, has_ams, model_matches, can_upload, can_print, options (the desktop's remembered defaults), upload_name (the file name the desktop's export would give plate {index}, the current plate without it - print hosts, the connected Snapmaker and a Snapmaker over the LAN); a Snapmaker over the LAN adds ip, port, added_by, toolheads, layer, total_layers and left_time_s, so one card can show everything /api/snapmaker/devices reports; and what a control needs: can_pause, can_resume, can_stop, print_status, stage, print_error {code, message, job_id, actions[]} (null when there is none; each action is {id, verb, label, needs_job_id, remote_safe} - the same buttons the desktop error dialog draws for that code) and the hms summary; a print host or connected Snapmaker that answers as a Moonraker printer adds bed_temp, bed_target and nozzles too (absent when it does not); and what the phone's native printer screen draws: controls {heaters[] {id bed|chamber|nozzle{N}, label, temp, target, min, max, settable, active}, speed {kind level|factor, value, min, max, settable, levels[]}, light {on} (absent without one), fans[] {id, label, percent}} on a Bambu printer, a LAN Snapmaker and a Moonraker print host, and ams[] {id, nozzle, side L|R|'', humidity_level, humidity_pct, temp, trays[] {id, exists, type, sub_type, color, remain, can_load, can_unload, filament_why}} plus ext_spools[] {ams_id 254|255, side, exists, type, color, can_load, can_unload} on a Bambu printer; controls.filament {load, unload, assumed} where load / unload is offered, and can_load / can_unload on each U1 toolhead"} },
            { {"method", "POST"}, {"path", "/api/printers/{id}/control"},  {"description", "form action=pause|resume|stop[&confirm=1][&dry_run=1]: pause, resume or stop the print on that printer, exactly as the desktop's own buttons do (stop = cancel the print and needs confirm=1; pause and resume do not). Returns a job id; the job's result says what the printer then reported. 409 when the printer's own state does not allow it (see can_pause / can_resume / can_stop). {id} is any id /api/printers lists, sm:{id} for a Snapmaker over the LAN included. The same route answers a Bambu printer error with the buttons its own dialog would draw: action=resume_error|stop_error|ignore_error|idle_ignore_error|ack_close&err={code} (stop_error needs confirm=1). Which of them a given error offers is print_error.actions on GET /api/printers, where each entry is {id, verb, label, needs_job_id, remote_safe}; a verb the error does not offer, one whose code is no longer the one the printer reports, or one that needs a job_id the printer has not got, is 409, and a verb this phase keeps desktop-only (the AMS controls, drying, nozzle recheck, buzzer, purification) is 403. The same route also changes the settings the desktop Device tab changes, as that printer's `controls` on GET /api/printers lists them: action=set_temp&heater=bed|chamber|nozzle{N}&target={C} (0 = off; confirm=1 is required above 50 C while the printer is not printing; 400 above the heater's max, 409 for a read-only heater), action=set_speed&value={level 1..4 on a Bambu, only while printing | speed factor % on a Klipper printer}, action=set_light&on=1|0, action=set_fan&fan={id}&percent=0..100 - a Bambu printer through the same MachineObject calls the Device tab makes, a Moonraker printer as one /printer/gcode/script. And filament: action=load_filament|unload_filament&ams={AMS id, 128+ AMS HT, 254/255 external spool}&slot={0..3} on a Bambu printer (command_ams_change_filament, as the Device tab's Load / Unload), slot={toolhead 0..3} on a Snapmaker U1 whose G-code help lists the macros (controls.filament.assumed: INNER_FILAMENT_UNLOAD / SM_PRINT_AUTO_FEED, unverified); 409 while printing or paused, or when that slot's can_load / can_unload is false"} },
            { {"method", "POST"}, {"path", "/api/slice?plate={index}|all"}, {"description", "start slicing one plate (selects it) or all; returns a job id; 409 while slicing"} },
            { {"method", "POST"}, {"path", "/api/plates/{index}/send"},    {"description", "form printer={id}&mode=upload|print[&confirm=1][&force=1][&dry_run=1][&bed_leveling=0|1&flow_cali=0|1&timelapse=0|1&vibration_cali=0|1&use_ams=0|1][&name=][&mapping=0:1,1:2]: send the sliced plate to a printer exactly like the desktop's Send / Print dialogs (upload = to the printer's storage, print = start it; print needs confirm=1). A Snapmaker over the LAN (printer sm:{id}) takes `mapping` = which toolhead prints each of the file's filaments, defaulting to the colour match its own app makes; returns a job id; 409 unless the plate is sliced and no other send is running"} },
            { {"method", "GET"},  {"path", "/api/events?since={id}"},      {"description", "what this instance's printer watcher has seen, newest last: {events, last_id, watcher}. Each event is {local_id, time, instance, printer {id, name, kind}, kind started|finished|failed|cancelled|paused|resumed|runout|error, severity info|warning|error, title, text, code?, job?}. The hub keeps the merged history of every instance at /events on the phone link. `watcher` says what the live poll has seen: {poll_ms, last_poll, last_poll_ms, printers[{id, kind, online, watched, state, seen_at?}]}, where seen_at is the poll that seeded that printer - until it has one, and until last_poll has moved past it, a change on that printer seeds the watcher instead of making an event"} },
            { {"method", "GET"},  {"path", "/api/jobs"},                   {"description", "recent jobs"} },
            { {"method", "GET"},  {"path", "/api/jobs/{id}"},              {"description", "job state: kind slice|send, running | done | error | cancelled, percent, text; send jobs add printer, mode and result (what was sent, or the composed parameters of a dry run)"} },
            { {"method", "GET"},  {"path", "/api/snapmaker/devices"},      {"description", "the Snapmaker printers on the LAN (from this PC's paired devices, an mDNS pass, the Stream tab's cameras and what was added by hand) with live state, temperatures and what each toolhead holds; plus `connect`, the optional MQTT connection the PC's Device tab makes"} },
            { {"method", "POST"}, {"path", "/api/snapmaker/add?ip={address}"}, {"description", "remember a printer at that address (it has to answer as a Snapmaker first)"} },
            { {"method", "POST"}, {"path", "/api/snapmaker/remove?id={device}"}, {"description", "forget a printer from the list"} },
            { {"method", "POST"}, {"path", "/api/snapmaker/connect?id={device}"}, {"description", "optional: connect that paired Snapmaker over MQTT the way picking it on the PC's Device page does; sending does not need it"} },
            { {"method", "POST"}, {"path", "/api/snapmaker/disconnect"},     {"description", "disconnect the MQTT-connected Snapmaker"} },
            { {"method", "GET"},  {"path", "/api/presets"},                {"description", "printer / process choices as the sidebar shows them, the nozzle choices of the printer model, filament choices and the current filament slots with colours"} },
            { {"method", "POST"}, {"path", "/api/presets/select?type=printer|nozzle|process|filament&name={value}[&index={slot}]"}, {"description", "select a preset the way the sidebar does (nozzle = the same printer with another nozzle variant); 409 when that preset has unsaved changes on the PC"} },
            { {"method", "POST"}, {"path", "/api/presets/filament_color?index={slot}&color=%23RRGGBB"}, {"description", "set a filament slot colour"} },
            { {"method", "POST"}, {"path", "/api/presets/filament_add"},   {"description", "add a filament slot"} },
            { {"method", "GET"},  {"path", "/api/settings/process"},       {"description", "the Process tab: pages > groups > lines > options with definition, current and saved values, dirty flags, app mode"} },
            { {"method", "POST"}, {"path", "/api/settings/process"},       {"description", "form body key=value[&key=value…] (serialized option values); applies like typing into the tab, returns preset/dirty state"} },
            { {"method", "POST"}, {"path", "/api/settings/process/revert"}, {"description", "reset all settings to the last saved preset"} },
            { {"method", "POST"}, {"path", "/api/settings/process/save"},  {"description", "save modifications under the same name (system presets save to '<name> - Custom')"} },
            { {"method", "GET"},  {"path", "/api/archive[?printer={id}]"}, {"description", "the G-code archive (Preferences > Ultra > Store G-Code Files): every file this PC has sent to a printer while it was on, newest first, as {enabled, max, records}. Each record is {id, time, file, sent_name, size, sha256, printer {id, kind bambu|snapmaker|printhost|connect, name, model}, plate, plate_name, project_title, filaments [{index, type, colour, grams}], estimated_time_s, estimated_weight_g, source desktop|phone, mode upload|print, has_thumbnail, exists (its file is still on disk), mapping (the toolhead mapping a Snapmaker send used, in the wire form /send takes) and spoolman_deduct (a Spoolman deduction was asked for)} - names and sizes only, never a path on the PC. `printer` filters by the printer id a send used"} },
            { {"method", "GET"},  {"path", "/api/archive/{id}"},        {"description", "one record, the same fields a row of /api/archive carries plus `exists` (its file is still on disk)"} },
            { {"method", "GET"},  {"path", "/api/archive/{id}/thumbnail.png"}, {"description", "the plate preview stored with that record"} },
            { {"method", "POST"}, {"path", "/api/archive/{id}/send"},     {"description", "form [printer={id}][&mode=upload|print]&confirm=1[&force=1][&dry_run=1][&name=][&mapping=0:1,1:2][&ams_mapping=0:0-2,1:128-0][&use_ams=&bed_leveling=&flow_cali=&timelapse=&nozzle_offset_cali=]: send that archived file to a printer again - the stored bytes are the payload, so nothing is re-sliced and no project has to be open. `printer` and every option not given fall back to the record; confirm=1 is always required. Returns the same job id and progress shape as /api/plates/{index}/send (kind send, followed through /api/jobs/{id}). 404 for an unknown record, 409 when its file is gone or the target printer is of another kind (a .gcode.3mf cannot go to a Moonraker host); reprinting over the PC's Snapmaker connection (\"connect\") is not supported. `printer` may name another printer of the same kind and model as the record's (a sliced file fits any printer of its model); a record with no known model may only go back to its own printer, and a file is started in place only on the very printer it was sent to. A Bambu printer gets the job read back from the .gcode.3mf and sent the way the desktop's send dialog sends a plate: `ams_mapping` is the AMS slot per filament (<filament>:<ams_id>-<slot_id>, what /preview proposes; empty = the automatic mapping), the options default to the dialog's remembered choices, and the send is refused with the reason when the printer is offline, busy, of another model or nozzle diameter, has no usable storage, or a filament has no slot of its type on the side it was sliced for. Every reprint is added to the record's `reprints` history"} },
            { {"method", "POST"}, {"path", "/api/archive/{id}/preview"},  {"description", "form [printer={id}][&mode=print|upload][&ams_mapping=][&use_ams=&bed_leveling=&flow_cali=&timelapse=&nozzle_offset_cali=][&force=1]: a Bambu reprint's mapping sheet, nothing sent. Selects the printer on the PC (connecting it) and answers within ~25 s with {record, mode, file, printer {id, name, model, model_name, dual, has_ams, lan_mode, nozzle_rack}, job {plate, model, model_name, dual, nozzle_diameters, bed_type, estimated_time_s, estimated_weight_g}, filaments [{index, type, colour, grams, side L|R|\"\", tray \"<ams>-<slot>\"|null, auto, problems []}], trays [{id \"<ams>-<slot>\", ams_id, slot_id, name A1|HT-A, side, exists, ready, type, match_type, colour}], mapping (the ams_mapping to send back), problems [{filament, code unmapped|type|side, text}], options {bed_leveling|flow_cali|timelapse|use_ams|nozzle_offset_cali: {value, shown}}, warnings [], nozzle_mapping {applies, state accepted|refused|no_answer, reason?} (H2C, advisory), can_send}. 409 with the reason when the printer cannot take the job at all (offline, busy, another model or nozzle, no storage, no LAN access)"} },
            { {"method", "POST"}, {"path", "/api/archive/{id}/delete"},   {"description", "delete one stored file with its details and preview"} },
            { {"method", "DELETE"}, {"path", "/api/archive/{id}"},        {"description", "delete one stored file with its details and preview (the same as POST /api/archive/{id}/delete)"} }
        });
        r.body = j.dump();
        return r;
    }
    if (path == "/info" && method == "GET")
        return api_info();
    if (path == "/window" && (method == "GET" || method == "POST")) {
        std::string show = query_param(query, "show");
        if (show.empty()) show = query_param(body, "show");
        return api_window(method == "GET" ? std::string() : show);
    }
    if (path == "/attention/clear" && method == "POST")
        return api_attention_clear();
    if (path.compare(0, 7, "/debug/") == 0 && method == "POST")
        return api_debug(path.substr(7), query, body);
    if (path == "/events" && method == "GET")
        return api_events(num(query_param(query, "since"), 0));
    if (path == "/quit" && method == "POST") {
        std::string d = query_param(query, "discard");
        if (d.empty()) d = query_param(body, "discard");
        return api_quit(d == "1" || d == "true");
    }
    if (path == "/project/open" && method == "POST") {
        auto get = [&](const char* k) { std::string v = query_param(body, k); return v.empty() ? query_param(query, k) : v; };
        return api_project_open(get("path"), get("mode"));
    }
    if (path == "/settings/process" && method == "GET")
        return api_process_settings();
    if (path == "/settings/process" && method == "POST")
        return api_process_set(body.empty() ? query : body);
    if (path == "/settings/process/revert" && method == "POST")
        return api_process_revert();
    if (path == "/settings/process/save" && method == "POST")
        return api_process_save();
    if (path == "/presets" && method == "GET")
        return api_presets();
    if (path == "/presets/select" && method == "POST") {
        auto get = [&](const char* k) { std::string v = query_param(query, k); return v.empty() ? query_param(body, k) : v; };
        return api_select_preset(get("type"), get("name"), num(get("index"), -1));
    }
    if (path == "/presets/filament_color" && method == "POST") {
        auto get = [&](const char* k) { std::string v = query_param(query, k); return v.empty() ? query_param(body, k) : v; };
        return api_filament_color(num(get("index"), -1), get("color"));
    }
    if (path == "/presets/filament_add" && method == "POST")
        return api_filament_add();
    if (path == "/plates" && method == "GET")
        return api_plates();
    if (path.compare(0, 8, "/plates/") == 0 && method == "GET") {
        const std::string rest = path.substr(8);
        const size_t      slash = rest.find('/');
        if (slash != std::string::npos && rest.substr(slash) == "/thumbnail.png")
            return api_plate_thumbnail(num(rest.substr(0, slash), -1));
        if (slash != std::string::npos && rest.substr(slash) == "/layout")
            return api_plate_layout(num(rest.substr(0, slash), -1));
        if (slash != std::string::npos && rest.substr(slash) == "/preview")
            return api_plate_preview(num(rest.substr(0, slash), -1));
        if (slash != std::string::npos && rest.substr(slash) == "/preview.png")
            return api_plate_preview_png(num(rest.substr(0, slash), -1), query_param(query, "view"), num(query_param(query, "layer"), -1),
                                         num(query_param(query, "w"), 800), num(query_param(query, "h"), 800),
                                         numd(query_param(query, "zoom"), 1.0), numd(query_param(query, "cx"), 0.5), numd(query_param(query, "cy"), 0.5));
        if (slash != std::string::npos && rest.substr(slash) == "/preview/status")
            return api_plate_preview_status(num(rest.substr(0, slash), -1));
    }
    if (path.compare(0, 8, "/plates/") == 0 && method == "POST") {
        const std::string rest  = path.substr(8);
        const size_t      slash = rest.find('/');
        if (slash != std::string::npos && rest.substr(slash) == "/send")
            return api_send(num(rest.substr(0, slash), -1), body.empty() ? query : body);
    }
    if (path == "/objects/transform" && method == "POST")
        return api_object_transform(body.empty() ? query : body);
    if (path == "/printers" && method == "GET")
        return api_printers(num(query_param(query, "plate"), -1));
    if (path.compare(0, 10, "/printers/") == 0 && method == "POST") {
        const std::string rest  = path.substr(10);
        const size_t      slash = rest.find('/');
        // The id is a Bambu dev_id or the literal "host" / "connect", never a number: a plain
        // string segment, unlike the plate and job indices above, and the page sends it through
        // encodeURIComponent - so it is decoded here, unlike every other path in this dispatch.
        if (slash != std::string::npos && rest.substr(slash) == "/control")
            return api_printer_control(percent_decode(rest.substr(0, slash)), body.empty() ? query : body);
    }
    if (path == "/snapmaker/devices" && method == "GET")
        return api_snapmaker_devices();
    if (path == "/snapmaker/add" && method == "POST") {
        auto get = [&](const char* k) { std::string v = query_param(query, k); return v.empty() ? query_param(body, k) : v; };
        return api_snapmaker_add(get("ip"), num(get("port"), 80));
    }
    if (path == "/snapmaker/remove" && method == "POST") {
        std::string id = query_param(query, "id");
        if (id.empty()) id = query_param(body, "id");
        return api_snapmaker_remove(id);
    }
    if (path == "/snapmaker/connect" && method == "POST") {
        std::string id = query_param(query, "id");
        if (id.empty()) id = query_param(body, "id");
        return api_snapmaker_connect(id);
    }
    if (path == "/snapmaker/disconnect" && method == "POST")
        return api_snapmaker_disconnect();
    if (path == "/slice" && method == "POST") {
        std::string plate = query_param(query, "plate");
        if (plate.empty()) plate = query_param(body, "plate");
        return api_slice(num(plate, -1), plate == "all");
    }
    if (path == "/jobs" && method == "GET")
        return api_jobs(-1);
    if (path.compare(0, 6, "/jobs/") == 0 && method == "GET")
        return api_jobs(num(path.substr(6), -1));
    // The G-code archive. The id is a name this PC made (letters, digits, . - _), so it needs no
    // decoding; GcodeArchive checks it against that alphabet again before touching the folder.
    if (path == "/archive" && method == "GET")
        return api_archive(query_param(query, "printer"));
    if (path.compare(0, 9, "/archive/") == 0) {
        const std::string rest  = path.substr(9);
        const size_t      slash = rest.find('/');
        const std::string what  = slash == std::string::npos ? std::string() : rest.substr(slash);
        if (slash == std::string::npos && method == "GET")
            return api_archive_one(rest);
        if (slash == std::string::npos && method == "DELETE")
            return api_archive_delete(rest);
        if (what == "/thumbnail.png" && method == "GET")
            return api_archive_thumbnail(rest.substr(0, slash));
        // POST, not DELETE, is the route the phone uses: the hub proxies both, but a POST verb is
        // what every other action on this API takes and what a form can send.
        if (what == "/delete" && method == "POST")
            return api_archive_delete(rest.substr(0, slash));
        if (what == "/send" && method == "POST")
            return api_archive_send(rest.substr(0, slash), body.empty() ? query : body);
        if (what == "/preview" && method == "POST")
            return api_archive_preview(rest.substr(0, slash), body.empty() ? query : body);
    }
    r.status = 404;
    r.body   = json_error("no such route; see /api");
    return r;
}

void RemoteAccess::accept_loop()
{
    auto* acceptor = static_cast<tcp::acceptor*>(m_acceptor);
    for (;;) {
        boost::system::error_code ec;
        auto* sock = new tcp::socket(acceptor->get_executor());
        acceptor->accept(*sock, ec);
        if (ec) {
            delete sock;
            break; // closed by stop()
        }
        std::thread([this, sock]() { serve(sock); }).detach();
    }
    delete acceptor;
}

// Loopback peers only (the hub); small bodies (form parameters); /api/... routes.
void RemoteAccess::serve(void* socket_ptr)
{
    std::unique_ptr<tcp::socket> owner(static_cast<tcp::socket*>(socket_ptr));
    tcp::socket&                 client = *owner;
    try {
        boost::system::error_code ec;
        const auto peer = client.remote_endpoint(ec).address();
        if (ec || !peer.is_loopback())
            return;
        client.set_option(tcp::no_delay(true));

        // Bounded and deadlined, like the hub's own listener: this API is reachable from a phone
        // through the hub's proxy, so a client that dribbles bytes must not grow the buffer or
        // hold this detached thread. 16 KiB of head is plenty for the hub's replayed requests.
        set_read_timeout(client, 15);
        asio::streambuf req(16 * 1024);
        asio::read_until(client, req, "\r\n\r\n");
        std::string head(asio::buffers_begin(req.data()), asio::buffers_end(req.data()));
        const size_t head_end = head.find("\r\n\r\n") + 4;
        std::string  body     = head.substr(head_end); // any body bytes already read
        head.resize(head_end);

        std::istringstream first(head.substr(0, head.find("\r\n")));
        std::string        method, target, version;
        first >> method >> target >> version;
        size_t content_length = 0;
        {
            size_t pos = head.find("\r\n") + 2;
            while (pos < head_end - 2) {
                size_t nl = head.find("\r\n", pos);
                std::string line = head.substr(pos, nl - pos);
                std::string key  = line.substr(0, std::min<size_t>(15, line.size()));
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char) std::tolower(c); });
                if (key == "content-length:")
                    content_length = (size_t) std::max(0, std::atoi(line.c_str() + 15));
                pos = nl + 2;
            }
        }
        const size_t q     = target.find('?');
        std::string  path  = q == std::string::npos ? target : target.substr(0, q);
        std::string  query = q == std::string::npos ? "" : target.substr(q + 1);

        if (path.compare(0, 4, "/api") != 0 || (path.size() > 4 && path[4] != '/')) {
            respond(client, "404 Not Found", "application/json", json_error("no such route; see /api"));
            return;
        }
        if (content_length > 64 * 1024) {
            respond(client, "413 Payload Too Large", "application/json", json_error("body too large"));
            return;
        }
        while (body.size() < content_length) {
            char   buf[4096];
            size_t n = client.read_some(asio::buffer(buf, std::min(sizeof(buf), content_length - body.size())));
            body.append(buf, n);
        }
        ApiResponse ar = handle_api(method, path.substr(4), query, body);
        if (ar.status == 503 && gui_closing()) // it began before the close; its GUI step was dropped
            ar.body = json_error("the slicer is closing");
        const char* status = ar.status == 200 ? "200 OK" : ar.status == 400 ? "400 Bad Request" : ar.status == 404 ? "404 Not Found"
                           : ar.status == 409 ? "409 Conflict" : ar.status == 413 ? "413 Payload Too Large"
                           : ar.status == 502 ? "502 Bad Gateway" : ar.status == 504 ? "504 Gateway Timeout"
                           : ar.status == 503 ? "503 Service Unavailable" : "500 Internal Server Error";
        respond(client, status, ar.type, ar.body, ar.headers);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(debug) << "RemoteAccess: session ended: " << e.what();
    }
}

} // namespace GUI
} // namespace Slic3r
