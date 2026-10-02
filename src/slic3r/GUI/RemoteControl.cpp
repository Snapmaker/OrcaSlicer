#include "RemoteControl.hpp"

#include "DeviceControls.hpp"
#include "FilamentCommands.hpp"
#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "HMS.hpp"
#include "PrintErrorCommands.hpp"
#include "RemoteAccess.hpp"
#include "SnapmakerLan.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/PrintHostDevices.hpp"
#include "slic3r/Utils/PrusaLinkStatus.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <wx/utils.h>

namespace Slic3r {
namespace GUI {
namespace RemoteControl {

using nlohmann::json;

// ---------------------------------------------------------------- helpers ----

// From the worker thread: run fn on the GUI thread and wait for it (bounded), as RemoteSend does.
static bool on_main(std::function<void()> fn, int timeout_ms = 10000)
{
    // Through the gate (MainThreadGate.hpp): once the main window is closing this is false at once
    // and fn never runs against a Plater that is going away.
    return RemoteAccess::call_on_main(std::move(fn), timeout_ms) == MainCallResult::Done;
}

static bool env_flag(const char* name)
{
    wxString v;
    return wxGetEnv(name, &v) && v == "1";
}

static MachineObject* find_machine(DeviceManager* dm, const std::string& id)
{
    std::map<std::string, MachineObject*> all = dm->get_my_machine_list();
    for (const auto& kv : dm->get_local_machine_list()) all.insert(kv);
    for (const auto& kv : all)
        if (kv.second && kv.second->dev_id == id) return kv.second;
    return nullptr;
}

static std::string error_code_text(int code)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", (unsigned) code);
    return buf;
}

// The printer's own words for a print error, when the HMS table has them (StatusPanel shows the
// same). The serial is what picks the table: the newer machines have one of their own, and the
// same code says something else on each of them.
static std::string print_error_message(const std::string& dev_id, int code)
{
    if (HMSQuery* q = wxGetApp().get_hms_query()) return q->describe_print_error(dev_id, code).ToUTF8().data();
    return std::string();
}

// The button set the desktop's error dialog would draw for this code, as data. Same lookup
// (HMSQuery::query_print_error_url_action against the shipped hms_action_<devtype>.json), same
// resolver (resolve_print_error_actions, generic fallback included), same 0300-800x liveview
// special case StatusPanel applies - so a surface that reads this JSON cannot end up offering a
// different set from the dialog on the PC standing next to the printer.
//
// GUI thread only: the HMS tables are read under the query's own lock and the MachineObject is
// the GUI's.
std::vector<int> resolved_print_error_actions(const std::string& dev_id, int print_error)
{
    HMSQuery* q = wxGetApp().get_hms_query();
    if (!q) return std::vector<int>();
    std::vector<int> table_actions;
    q->query_print_error_url_action(dev_id, print_error, table_actions);
    const std::string code = HMSQuery::print_error_code(print_error);
    if (code == "03008003" || code == "03008002" || code == "0300800A")
        table_actions.push_back(PrintErrorAction::JUMP_TO_LIVEVIEW);
    bool used_fallback = false;
    return resolve_print_error_actions(table_actions, used_fallback);
}

// {code, message, job_id, has_details, actions[]} - the one shape the status JSON, the event
// payload and the control route all agree on. `job_id` is named because an action that needs one
// is only offered when the printer has one, and a client that wants to say why a button is off
// needs to see it; `has_details` is the same fact for the action_json blob behind Proceed and
// Don't remind, which only a refused command supplies.
static json print_error_json(MachineObject* m)
{
    const bool has_blob = m->has_remote_command_error_action_json();

    json e;
    e["code"]        = error_code_text(m->print_error);
    e["message"]     = print_error_message(m->dev_id, m->print_error);
    e["job_id"]      = m->job_id_;
    e["has_details"] = has_blob;
    json actions = json::array();
    for (const PrintErrorRemoteAction& a : describe_print_error_actions(resolved_print_error_actions(m->dev_id, m->print_error),
                                                                       !m->job_id_.empty(), has_blob)) {
        actions.push_back({ { "id", a.id },
                            { "verb", a.verb },
                            { "label", a.label },
                            { "needs_job_id", a.needs_job_id },
                            { "needs_details", a.needs_action_json },
                            { "remote_safe", a.remote_safe } });
    }
    e["actions"] = actions;
    return e;
}

// What the phone may set on a Bambu printer, read off the same MachineObject fields the desktop
// Device tab reads and gated by the same capability flags:
//   * the bed (limit get_bed_temperature_limit), each nozzle as the Device tab lists them (one row,
//     or L above R on the two-nozzle machines; limit nozzle_max_temperature, else 300), and the
//     chamber - settable where the printer says support_chamber_temp_edit (limit 60, the Device
//     tab's own), read-only on an X1 whose chamber is only a sensor;
//   * the speed level, which the Device tab only lets a person change while a print runs;
//   * the chamber light, when the printer reports one (lights_report);
//   * the part fan, and the aux and chamber fans where is_support_aux_fan / is_support_chamber_fan.
static DeviceControls::Caps bambu_caps(MachineObject* m)
{
    using namespace DeviceControls;
    using DeviceControls::Fan; // not the Device tab's widget of the same name (Widgets/FanControl.hpp)
    Caps caps;
    const int bed_limit = m->get_bed_temperature_limit();
    caps.heaters.push_back(Heater { "bed", "Bed", m->bed_temp, m->bed_temp_target, bed_limit > 0 ? bed_limit : DEFAULT_BED_MAX, true, false });
    std::vector<NozzleTempSample> samples;
    for (const Extder& e : m->m_extder_data.extders) samples.push_back({ e.id, e.temp, e.target_temp });
    for (Heater& h : bambu_nozzle_heaters(samples, m->m_extder_data.total_extder_count, m->m_extder_data.current_extder_id,
                                          m->nozzle_max_temperature))
        caps.heaters.push_back(h);
    if (m->is_support_chamber_edit)
        caps.heaters.push_back(Heater { "chamber", "Chamber", m->chamber_temp, m->chamber_temp_target, DEFAULT_CHAMBER_MAX, true, false });
    else if (m->get_printer_series() == PrinterSeries::SERIES_X1)
        caps.heaters.push_back(Heater { "chamber", "Chamber", m->chamber_temp, 0, DEFAULT_CHAMBER_MAX, false, false });

    caps.has_speed = true;
    caps.speed     = Speed { "level", (int) m->printing_speed_lvl, 1, 4, m->is_in_printing() };
    const MachineObject::LIGHT_EFFECT light = m->chamber_light_state(); // both of an H2's lights
    if (light != MachineObject::LIGHT_EFFECT::LIGHT_EFFECT_UNKOWN) {
        caps.has_light = true;
        caps.light_on  = light == MachineObject::LIGHT_EFFECT::LIGHT_EFFECT_ON;
    }
    caps.fans.push_back(Fan { "part", "Part cooling", percent_of_byte(m->cooling_fan_speed) });
    if (m->is_support_aux_fan) caps.fans.push_back(Fan { "aux", "Aux", percent_of_byte(m->big_fan1_speed) });
    if (m->is_support_chamber_fan) caps.fans.push_back(Fan { "chamber", "Chamber", percent_of_byte(m->big_fan2_speed) });
    caps.filament_actions = true; // StatusPanel's Load / Unload, which every Bambu printer has
    return caps;
}

// ------------------------------------------------------ filament load / unload ----

// The printer as the load / unload rules see it (FilamentCommands::availability).
static FilamentCommands::PrinterState filament_state(MachineObject* m)
{
    FilamentCommands::PrinterState s;
    s.printing             = m->is_in_printing();
    s.changing_filament    = m->ams_status_main == AmsStatusMain::AMS_STATUS_MAIN_FILAMENT_CHANGE;
    s.calibrating          = m->is_in_extrusion_cali();
    s.filament_at_extruder = m->is_filament_at_extruder();
    return s;
}

// Whether this slot is the one feeding: an extruder's tray_now on the new protocol (two-extruder
// printers), the printer's single tray_now otherwise - the same test StatusPanel's unload makes.
static bool slot_loaded(MachineObject* m, const std::string& ams_id, const std::string& slot_id)
{
    if (m->is_enable_np) {
        for (const Extder& e : m->m_extder_data.extders)
            if (e.snow.ams_id == ams_id && (e.snow.slot_id == slot_id || ams_id == "254" || ams_id == "255")) return true;
        return false;
    }
    return m->m_tray_now == FilamentCommands::tray_now_of(ams_id, slot_id);
}

// The external spools: vir_slots on a two-extruder printer (254 left, 255 right), the one
// vt_tray (254) otherwise.
static std::vector<const AmsTray*> ext_trays(MachineObject* m)
{
    std::vector<const AmsTray*> out;
    if (m->is_multi_extruders() && !m->vir_slots.empty()) {
        for (const AmsTray& t : m->vir_slots) out.push_back(&t);
    } else {
        out.push_back(&m->vt_tray);
    }
    return out;
}

static bool tray_present(const AmsTray& t) { return t.is_exists || !t.type.empty(); }

// "#RRGGBB" out of the tray's RRGGBBAA; empty when there is no colour.
static std::string tray_rgb(const std::string& rrggbbaa)
{
    if (rrggbbaa.size() < 6) return std::string();
    for (size_t i = 0; i < 6; ++i)
        if (!std::isxdigit((unsigned char) rrggbbaa[i])) return std::string();
    std::string out = "#" + rrggbbaa.substr(0, 6);
    for (char& c : out) c = (char) std::toupper((unsigned char) c);
    return out;
}

// Every AMS unit the printer reports, read-only: which nozzle it feeds (on the two-nozzle machines
// the Device tab's own L / R split), how damp it is, and each tray's colour, material and what is
// left. The desktop's AMS panel reads the same fields.
static json ams_json(MachineObject* m)
{
    json out = json::array();
    const bool dual = m->m_extder_data.total_extder_count > 1;
    for (const auto& kv : m->amsList) {
        const Ams* a = kv.second;
        if (!a) continue;
        json u;
        u["id"]             = a->id;
        u["nozzle"]         = a->nozzle;
        u["side"]           = dual ? (a->nozzle == 1 ? "L" : "R") : "";
        u["type"]           = a->type;
        u["humidity_level"] = a->humidity;
        if (a->humidity_raw >= 0) u["humidity_pct"] = a->humidity_raw;
        if (a->current_temperature != INVALID_AMS_TEMPERATURE) u["temp"] = a->current_temperature;
        json trays = json::array();
        for (const auto& tk : a->trayList) {
            const AmsTray* t = tk.second;
            if (!t) continue;
            AmsTray copy = *t; // get_display_filament_type is not const
            json j;
            j["id"]       = t->id;
            j["exists"]   = t->is_exists;
            j["type"]     = copy.get_display_filament_type();
            j["sub_type"] = t->sub_brands;
            j["color"]    = tray_rgb(t->color);
            j["remain"]   = t->remain;
            FilamentCommands::write_availability(
                FilamentCommands::availability(filament_state(m), t->is_exists, slot_loaded(m, a->id, t->id)), j);
            trays.push_back(j);
        }
        u["trays"] = trays;
        out.push_back(u);
    }
    return out;
}

// The external spool holders, in the tray shape plus the id the load / unload verbs take:
// [{ams_id "254"|"255", side L|R|"", exists, type, sub_type, color, can_load, can_unload}].
static json ext_spools_json(MachineObject* m)
{
    json       out  = json::array();
    const bool dual = m->is_multi_extruders();
    for (const AmsTray* t : ext_trays(m)) {
        const std::string id = dual ? (t->id.empty() ? std::string("254") : t->id) : std::string("254");
        AmsTray copy = *t;
        json    j;
        j["ams_id"]   = id;
        j["side"]     = dual ? (id == "254" ? "L" : "R") : "";
        j["exists"]   = tray_present(*t);
        j["type"]     = copy.get_display_filament_type();
        j["sub_type"] = t->sub_brands;
        j["color"]    = tray_rgb(t->color);
        FilamentCommands::write_availability(
            FilamentCommands::availability(filament_state(m), tray_present(*t), slot_loaded(m, id, "0")), j);
        out.push_back(j);
    }
    return out;
}

// A print host address turned into the base URL of its Moonraker HTTP API. This is
// Moonraker::make_url's own rule (MoonRaker.cpp), which is protected: an address without a scheme
// becomes http://, and the MQTT ports the Device tab stores (1884 plain, 8883 TLS) are dropped so
// the printer's HTTP API on port 80 is addressed instead.
static std::string moonraker_base(const std::string& host)
{
    std::string h = host;
    while (!h.empty() && (h.back() == '/' || h.back() == ' ')) h.pop_back();
    if (h.empty()) return h;
    if (h.compare(0, 7, "http://") == 0 || h.compare(0, 8, "https://") == 0) return h;
    const size_t mqtt_plain = h.find(":1884"), mqtt_tls = h.find(":8883");
    if (mqtt_plain != std::string::npos) h = h.substr(0, mqtt_plain);
    else if (mqtt_tls != std::string::npos) h = h.substr(0, mqtt_tls);
    return "http://" + h;
}

// One Moonraker HTTP call. Short timeouts: /api/printers probes with this on every poll and a
// printer that is off must not hold the answer up. Any thread but the GUI one.
static bool moonraker_http(const std::string& url, bool post, std::string& body, std::string& error, int timeout_s)
{
    bool ok = false;
    auto http = post ? Http::post(url) : Http::get(url);
    http.tls_policy(Http::TlsPolicy::PrintHost); // printer: keep accepting self-signed certificates
    // An empty JSON object rather than no body at all: CURLOPT_POST without post fields would fall
    // through to the wrapper's file-upload read callback. Moonraker ignores the body of these three.
    if (post) http.header("Content-Type", "application/json").set_post_body(std::string("{}"));
    http.timeout_connect(timeout_s)
        .timeout_max(timeout_s)
        .size_limit(256 * 1024)
        .on_error([&](std::string reply, std::string err, unsigned status) {
            error = err.empty() ? ("HTTP " + std::to_string(status)) : err;
            if (!reply.empty()) body = reply;
        })
        .on_complete([&](std::string reply, unsigned) {
            body = reply;
            ok   = true;
        })
        .perform_sync();
    // A 3xx answers through neither callback (Http::priv::http_perform); say something either way.
    if (!ok && error.empty()) error = "no answer";
    return ok;
}

static json parse_or_raw(const std::string& body)
{
    try {
        return json::parse(body);
    } catch (...) {
        return json(body.substr(0, 400));
    }
}

// What an address answered the last time it was asked. prepare() runs on the GUI thread and must
// not touch the network, so it reads what the last /api/printers probe (describe_hosts, every 5 s
// while the phone's Devices tab is open) found there: whether it is a Moonraker printer at all, and
// what it said it was doing - the print host's equivalent of a Bambu printer's can_* predicates.
struct Probe
{
    bool        moonraker { false };
    std::string state; // Klipper print_stats.state: standby | printing | paused | complete | ...
    long long   when { 0 };
    // What the same answer said the printer can be told (heaters and their limits, speed factor,
    // light, fans): the settings verbs are checked against it, since prepare() cannot ask now.
    DeviceControls::Caps caps;
};
static std::mutex                   s_probe_mutex;
static std::map<std::string, Probe> s_probes;

static long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void remember_probe(const std::string& base, bool moonraker, const std::string& state,
                           const DeviceControls::Caps& caps = DeviceControls::Caps())
{
    std::lock_guard<std::mutex> lock(s_probe_mutex);
    s_probes[base] = Probe { moonraker, state, now_ms(), caps };
}

// An address that did not answer is not asked again for half a minute: /api/printers is polled
// every 5 s and must not wait on a printer that is off, or on a print host that is not a Klipper
// one at all (an OctoPrint address never will be).
static bool ask_again(const std::string& base)
{
    std::lock_guard<std::mutex> lock(s_probe_mutex);
    auto it = s_probes.find(base);
    return it == s_probes.end() || it->second.moonraker || now_ms() - it->second.when > 30000;
}

// tri-state: 1 = a Moonraker printer, 0 = it answered something else, -1 = never asked.
static int probed(const std::string& base, std::string* state = nullptr, DeviceControls::Caps* caps = nullptr)
{
    std::lock_guard<std::mutex> lock(s_probe_mutex);
    auto it = s_probes.find(base);
    if (it == s_probes.end()) return -1;
    if (state) *state = it->second.state;
    if (caps) *caps = it->second.caps;
    return it->second.moonraker ? 1 : 0;
}

// The same rule fill_from_print_stats renders as can_pause / can_resume / can_stop.
static bool klipper_allows(const std::string& action, const std::string& state)
{
    if (action == "pause")  return state == "printing";
    if (action == "resume") return state == "paused";
    return state == "printing" || state == "paused";
}

// ---------------------------------------------------------------- prepare ----

// The three actions, in every spelling this feature needs: what the phone asks for, the
// MachineObject call the desktop's buttons make, and Moonraker's own endpoint and method name.
struct ActionNames
{
    const char* action;
    const char* bambu_call;
    const char* bambu_command;
    const char* moonraker_path;
    const char* moonraker_method;
};
static const ActionNames k_actions[] = {
    { "pause",  "command_task_pause",  "pause",  "printer/print/pause",  "printer.print.pause" },
    { "resume", "command_task_resume", "resume", "printer/print/resume", "printer.print.resume" },
    // "stop" is the phone's word for what the desktop calls Cancel print (an abort); Moonraker's
    // own name for it is cancel.
    { "stop",   "command_task_abort",  "stop",   "printer/print/cancel", "printer.print.cancel" },
};

static const ActionNames* action_names(const std::string& action)
{
    for (const ActionNames& a : k_actions)
        if (action == a.action) return &a;
    return nullptr;
}

static std::pair<int, std::string> prepare_bambu(const Request& req, const ActionNames& a, std::shared_ptr<Prepared> p,
                                                 std::shared_ptr<Prepared>& out)
{
    DeviceManager* dm = wxGetApp().getDeviceManager();
    if (!dm) return { 503, "no device manager" };
    MachineObject* obj = find_machine(dm, req.printer);
    if (!obj) return { 404, "no such printer: " + req.printer };
    if (!obj->is_online()) return { 409, obj->dev_name + " is offline" };
    // The same preconditions a send has: without the access code (LAN printers) or a live
    // connection the printer never sees the command, and its reported state is stale.
    if (obj->is_lan_mode_printer() && !obj->has_access_right())
        return { 409, obj->dev_name + " needs its access code entered on the PC first" };
    if (!obj->is_connected())
        return { 409, obj->dev_name + " is not connected; open it on the PC's Device tab once, or pick it in a send" };
    // The desktop's own buttons are enabled by exactly these three predicates (StatusPanel).
    const bool allowed = req.action == "pause" ? obj->can_pause() : req.action == "resume" ? obj->can_resume() : obj->can_abort();
    if (!allowed) {
        const std::string what = req.action == "pause" ? "paused" : req.action == "resume" ? "resumed" : "stopped";
        return { 409, obj->dev_name + " cannot be " + what + " right now (it reports " +
                          (obj->print_status.empty() ? "no print state" : obj->print_status) + ")" };
    }
    p->kind               = "bambu";
    p->printer_name       = obj->dev_name;
    p->call               = a.bambu_call;
    p->command            = a.bambu_command;
    p->status_before      = obj->print_status;
    p->print_error_before = obj->print_error;
    out                   = p;
    return { 200, "" };
}

static std::pair<int, std::string> prepare_host(const Request& req, const ActionNames& a, std::shared_ptr<Prepared> p,
                                                std::shared_ptr<Prepared>& out)
{
    PresetBundle*              bundle = wxGetApp().preset_bundle;
    std::shared_ptr<PrintHost> host;
    std::string                address;
    if (req.printer == "connect") {
        wxGetApp().get_connect_host(host);
        if (!host) return { 409, "no Snapmaker printer is connected on the PC's Device tab" };
        p->kind         = "connect";
        p->printer_name = "Snapmaker " + host->get_host();
        address         = host->get_host();
        p->host         = host; // the live MQTT socket, in case the printer's HTTP API refuses
    } else {
        if (!bundle) return { 503, "no preset bundle" };
        DynamicPrintConfig& cfg = bundle->printers.get_edited_preset().config;
        if (bundle->use_bbl_network()) return { 409, "the current printer preset sends through the Bambu network; pick that printer by its id" };
        address = cfg.opt_string("print_host");
        if (address.empty()) return { 409, "the printer preset has no print host address" };
        std::unique_ptr<PrintHost> h(PrintHost::get_print_host(&cfg, false));
        p->kind         = "printhost";
        p->printer_name = (h ? std::string(h->get_name()) + " " : std::string()) + address;
    }
    const std::string base = moonraker_base(address);
    if (base.empty()) return { 409, "this printer has no address" };
    // These three controls are Moonraker's; the printer itself decides whether it speaks it. The
    // last status probe (describe_hosts, from every /api/printers) is the answer when there is one
    // - the GUI thread cannot ask now. Never asked yet: let the command find out. A printer the PC
    // holds an MQTT socket to is never refused for the first reason: that socket is run()'s fallback.
    std::string state;
    const int   seen = probed(base, &state);
    if (!p->host && seen == 0)
        return { 409, p->printer_name + " does not answer as a Klipper / Moonraker printer, so it cannot be paused, resumed or stopped from here" };
    if (seen == 1 && !state.empty() && !klipper_allows(req.action, state)) {
        const std::string what = req.action == "pause" ? "paused" : req.action == "resume" ? "resumed" : "stopped";
        return { 409, p->printer_name + " cannot be " + what + " right now (it reports " + state + ")" };
    }
    p->url              = base + "/" + a.moonraker_path;
    p->moonraker_method = a.moonraker_method;
    out                 = p;
    return { 200, "" };
}

// A Snapmaker the LAN list knows (printer sm:<id>): Moonraker over the printer's own HTTP API,
// gated by what the list's last probe saw. That probe runs off the GUI thread whenever the phone
// lists printers, so this only reads it; a printer never probed is asked nothing here and the
// command itself finds out. No MQTT socket to fall back on: the LAN path never opens one.
static std::pair<int, std::string> prepare_snapmaker_lan(const Request& req, const ActionNames& a, std::shared_ptr<Prepared> p,
                                                         std::shared_ptr<Prepared>& out)
{
    SnapmakerLan::Device d;
    if (!SnapmakerLan::find(req.printer.substr(3), d)) return { 404, "no such printer: " + req.printer };
    p->kind         = "snapmaker";
    p->printer_name = d.name.empty() ? d.ip : d.name;
    SnapmakerLan::Status s;
    if (SnapmakerLan::cached_status(d, s)) {
        if (!s.online) return { 409, p->printer_name + " is offline" };
        if (s.login_required) return { 409, p->printer_name + " requires a login for its LAN API, so it cannot be controlled from here" };
        if (!s.state.empty() && !klipper_allows(req.action, s.state)) {
            const std::string what = req.action == "pause" ? "paused" : req.action == "resume" ? "resumed" : "stopped";
            return { 409, p->printer_name + " cannot be " + what + " right now (it reports " + s.state + ")" };
        }
    }
    p->url              = SnapmakerLan::base_url(d) + "/" + a.moonraker_path;
    p->moonraker_method = a.moonraker_method;
    out                 = p;
    return { 200, "" };
}

// The MachineObject method behind each error verb, for the log line and the job result: the phone
// gets told which command was sent, exactly as the generic controls report command_task_*.
static const char* error_action_call(const std::string& verb)
{
    if (verb == "resume_error")      return "command_hms_resume";
    if (verb == "stop_error")        return "command_hms_stop";
    if (verb == "ignore_error")      return "command_hms_ignore";
    if (verb == "idle_ignore_error") return "command_hms_idle_ignore";
    if (verb == "ack_proceed")       return "command_ack_proceed";
    if (verb == "dont_remind")       return "command_dont_remind_next_time";
    if (verb == "ack_close")         return "command_clean_print_error_uiop";
    return "";
}

// ------------------------------------------------- the printer-error actions ----
//
// One error code, one action, one command - and three guards that all have to pass before anything
// is published, because every one of them is a way to have a command land on the wrong thing:
//
//   * the printer must still be reporting the code the request names. Status pages are left open;
//     a resume composed against a filament runout must not reach the nozzle crash that replaced
//     it, and a resume for an error that has already cleared must not reach a healthy print.
//   * the commands that carry "job_id" need one. Firmware drops them silently without it, which
//     is indistinguishable from the printer ignoring the person.
//   * the verb must be one this phase offers remotely. The AMS controls, the drying stop, the
//     nozzle recheck, the buzzer and the purification switch are described in the status JSON so
//     a client can show them greyed with a reason, and refused here.
static std::pair<int, std::string> prepare_error_action(const Request& req, std::shared_ptr<Prepared> p,
                                                        std::shared_ptr<Prepared>& out)
{
    DeviceManager* dm = wxGetApp().getDeviceManager();
    if (!dm) return { 503, "no device manager" };
    MachineObject* obj = find_machine(dm, req.printer);
    if (!obj) return { 404, "no such printer: " + req.printer };
    if (!obj->is_online()) return { 409, obj->dev_name + " is offline" };
    if (obj->is_lan_mode_printer() && !obj->has_access_right())
        return { 409, obj->dev_name + " needs its access code entered on the PC first" };
    if (!obj->is_connected())
        return { 409, obj->dev_name + " is not connected; open it on the PC's Device tab once, or pick it in a send" };

    // Everything else is the pure check (PrintErrorCommands.hpp), against what the printer is
    // reporting at this instant - not against anything the request says about it.
    PrintErrorActionRequest c;
    c.verb         = req.action;
    c.asked_err    = req.err;
    c.confirm      = req.confirm;
    c.current_err  = obj->print_error == 0 ? std::string() : error_code_text(obj->print_error);
    c.job_id       = obj->job_id_;
    c.printer_name = obj->dev_name;
    // Only a refused command leaves a blob, and only for as long as its own code is the one being
    // reported - so this is false for the ordinary status-push error, and the two verbs built from
    // the blob are refused 409 there rather than reaching a builder with nothing to build from.
    c.has_action_json = obj->has_remote_command_error_action_json();
    if (obj->print_error != 0) c.offered = resolved_print_error_actions(obj->dev_id, obj->print_error);
    std::string why;
    const int   status = check_print_error_action(c, why);
    if (status != 0) {
        BOOST_LOG_TRIVIAL(info) << "RemoteControl: " << req.action << " refused for " << obj->dev_name
                                << " (" << status << "): " << why;
        return { status, why };
    }

    p->kind               = "bambu";
    p->printer_name       = obj->dev_name;
    p->is_error_action    = true;
    p->err_code           = c.current_err;
    // The decimal spelling, which is what these commands carry - see PrintErrorCommands.hpp.
    p->err_arg            = std::to_string(obj->print_error);
    p->job_id             = obj->job_id_;
    if (c.has_action_json) p->action_json = obj->get_command_error_action_json();
    p->call               = error_action_call(req.action);
    p->command            = req.action;
    p->status_before      = obj->print_status;
    p->print_error_before = obj->print_error;
    out                   = p;
    BOOST_LOG_TRIVIAL(info) << "RemoteControl: " << req.action << " (" << p->call << ") prepared for "
                            << obj->dev_name << ", error " << p->err_code << ", job_id "
                            << (p->job_id.empty() ? std::string("<none>") : p->job_id);
    return { 200, "" };
}

// ------------------------------------------------------ the settings verbs ----
//
// set_temp, set_speed, set_light and set_fan: the Device tab's own temperature boxes, speed popup,
// lamp switch and fan popup, for the phone. Each is checked against the printer's `controls` (the
// same Caps GET /api/printers shows) at the moment it is prepared, so a request for a heater the
// printer has not got, a target above its limit, or a light it does not have is refused before
// anything is sent. Heating above DeviceControls::CONFIRM_ABOVE_C while idle needs confirm=1.

bool is_setting_verb(const std::string& action)
{
    return action == "set_temp" || action == "set_speed" || action == "set_light" || action == "set_fan";
}

static const char* speed_level_name(int level)
{
    switch (level) {
    case 1: return "Silent";
    case 2: return "Standard";
    case 3: return "Sport";
    case 4: return "Ludicrous";
    default: return "?";
    }
}

// The pure checks, shared by every kind of printer. Fills the parts of `p` that do not depend on how
// the command travels: the verb's value, and the sentence the job reports.
static std::pair<int, std::string> check_setting(const Request& req, const DeviceControls::Caps& caps, bool printing,
                                                 Prepared& p)
{
    using namespace DeviceControls;
    std::string why;
    int         status = 0;
    if (req.action == "set_temp") {
        status = check_set_temp(caps, req.heater, req.target, req.confirm, printing, p.int_value, why);
        if (status == 0) {
            p.heater = req.heater;
            const Heater*     h    = find_heater(caps, req.heater);
            const std::string what = (h->label == "L" || h->label == "R") ? h->label + " nozzle" : h->label;
            p.setting_text = what + (p.int_value == 0 ? std::string(" off") : " to " + std::to_string(p.int_value) + " C");
        }
    } else if (req.action == "set_speed") {
        status = check_set_speed(caps, req.value, p.int_value, why);
        if (status == 0)
            p.setting_text = caps.speed.kind == "level" ? std::string("speed to ") + speed_level_name(p.int_value)
                                                        : "speed to " + std::to_string(p.int_value) + " %";
    } else if (req.action == "set_light") {
        status = check_set_light(caps, req.on, p.light_on, why);
        if (status == 0) p.setting_text = p.light_on ? "light on" : "light off";
    } else { // set_fan
        int percent = 0;
        status      = check_set_fan(caps, req.fan, req.percent, percent, why);
        if (status == 0) {
            p.heater       = req.fan; // the fan id rides in the same slot; run() reads it by verb
            p.int_value    = percent;
            p.setting_text = find_fan(caps, req.fan)->label + " fan to " + std::to_string(percent) + " %";
        }
    }
    if (status != 0) return { status, why };
    p.is_setting = true;
    return { 200, "" };
}

// The Klipper G-code for a checked setting.
static std::string moonraker_setting_script(const Prepared& p)
{
    using namespace DeviceControls;
    if (p.action == "set_temp") return moonraker_temp_script(p.heater, p.int_value);
    if (p.action == "set_speed") return moonraker_speed_script(p.int_value);
    if (p.action == "set_light") return moonraker_light_script(p.light_on);
    return moonraker_fan_script(p.heater, p.int_value);
}

static std::pair<int, std::string> prepare_setting_bambu(const Request& req, std::shared_ptr<Prepared> p,
                                                         std::shared_ptr<Prepared>& out)
{
    DeviceManager* dm = wxGetApp().getDeviceManager();
    if (!dm) return { 503, "no device manager" };
    MachineObject* obj = find_machine(dm, req.printer);
    if (!obj) return { 404, "no such printer: " + req.printer };
    if (!obj->is_online()) return { 409, obj->dev_name + " is offline" };
    if (obj->is_lan_mode_printer() && !obj->has_access_right())
        return { 409, obj->dev_name + " needs its access code entered on the PC first" };
    if (!obj->is_connected())
        return { 409, obj->dev_name + " is not connected; open it on the PC's Device tab once, or pick it in a send" };
    const DeviceControls::Caps caps    = bambu_caps(obj);
    const auto                 checked = check_setting(req, caps, obj->is_in_printing(), *p);
    if (checked.first != 200) return checked;
    p->kind         = "bambu";
    p->printer_name = obj->dev_name;
    p->command      = req.action;
    if (req.action == "set_temp") {
        const int n = DeviceControls::nozzle_index_of(p->heater);
        if (n >= 0) {
            int nozzles = 0;
            for (const DeviceControls::Heater& h : caps.heaters)
                if (DeviceControls::nozzle_index_of(h.id) >= 0) ++nozzles;
            p->extruder_id = n;
            p->dual_nozzle = nozzles > 1;
            // The Device tab's own guard: the printer ignores a target for an empty hotend slot.
            if (p->dual_nozzle)
                for (const Extder& e : obj->m_extder_data.extders)
                    if (e.id == n && !e.nozzle_exist)
                        return { 409, std::string(n == 1 ? "Left" : "Right") + " hotend not detected on " + obj->dev_name +
                                          ", so its temperature cannot be set" };
            p->call = p->dual_nozzle ? "command_set_nozzle_new" : "command_set_nozzle";
        } else {
            p->call = p->heater == "bed" ? "command_set_bed" : "command_set_chamber";
        }
    } else if (req.action == "set_speed") {
        p->call = "command_set_printing_speed";
    } else if (req.action == "set_light") {
        p->call = "command_set_chamber_light";
    } else {
        p->fan_type = DeviceControls::bambu_fan_type(p->heater);
        p->call     = "command_control_fan_val";
    }
    p->status_before      = obj->print_status;
    p->print_error_before = obj->print_error;
    out                   = p;
    BOOST_LOG_TRIVIAL(info) << "RemoteControl: " << p->call << " (" << p->setting_text << ") prepared for " << obj->dev_name;
    return { 200, "" };
}

// A Moonraker printer: the print host "host", the Device tab's "connect", or a LAN Snapmaker.
static std::pair<int, std::string> prepare_setting_moonraker(const Request& req, std::shared_ptr<Prepared> p,
                                                             std::shared_ptr<Prepared>& out)
{
    std::string          base;
    DeviceControls::Caps caps;
    std::string          state;
    if (req.printer.compare(0, 3, "sm:") == 0) {
        SnapmakerLan::Device d;
        if (!SnapmakerLan::find(req.printer.substr(3), d)) return { 404, "no such printer: " + req.printer };
        p->kind         = "snapmaker";
        p->printer_name = d.name.empty() ? d.ip : d.name;
        SnapmakerLan::Status s;
        if (!SnapmakerLan::cached_status(d, s))
            return { 409, p->printer_name + " has not answered yet; open its card once and try again" };
        if (!s.online) return { 409, p->printer_name + " is offline" };
        if (s.login_required)
            return { 409, p->printer_name + " requires a login for its LAN API, so it cannot be controlled from here" };
        caps  = s.caps;
        state = s.state;
        base  = SnapmakerLan::base_url(d);
    } else {
        std::shared_ptr<PrintHost> host;
        std::string                address;
        if (req.printer == "connect") {
            wxGetApp().get_connect_host(host);
            if (!host) return { 409, "no Snapmaker printer is connected on the PC's Device tab" };
            p->kind         = "connect";
            p->printer_name = "Snapmaker " + host->get_host();
            address         = host->get_host();
        } else {
            PresetBundle* bundle = wxGetApp().preset_bundle;
            if (!bundle) return { 503, "no preset bundle" };
            if (bundle->use_bbl_network())
                return { 409, "the current printer preset sends through the Bambu network; pick that printer by its id" };
            address = bundle->printers.get_edited_preset().config.opt_string("print_host");
            if (address.empty()) return { 409, "the printer preset has no print host address" };
            p->kind         = "printhost";
            p->printer_name = address;
        }
        base = moonraker_base(address);
        if (base.empty()) return { 409, "this printer has no address" };
        if (probed(base, &state, &caps) != 1)
            return { 409, p->printer_name + " has not answered as a Klipper / Moonraker printer yet, so it cannot be set from here" };
    }
    const bool printing = state == "printing" || state == "paused";
    const auto checked  = check_setting(req, caps, printing, *p);
    if (checked.first != 200) return checked;
    p->script = moonraker_setting_script(*p);
    if (p->script.empty()) return { 409, "this printer has no command for that" };
    p->url              = base + "/printer/gcode/script?script=" + Http::url_encode(p->script);
    p->moonraker_method = "printer.gcode.script";
    out                 = p;
    BOOST_LOG_TRIVIAL(info) << "RemoteControl: " << p->script << " prepared for " << p->printer_name;
    return { 200, "" };
}

bool is_filament_verb(const std::string& action) { return action == "load_filament" || action == "unload_filament"; }

static bool all_digits(const std::string& s)
{
    if (s.empty() || s.size() > 3) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// A Bambu printer: StatusPanel::on_ams_load_curr / on_ams_unload, for one named slot.
static std::pair<int, std::string> prepare_filament_bambu(const Request& req, std::shared_ptr<Prepared> p, std::shared_ptr<Prepared>& out)
{
    const bool load = req.action == "load_filament";
    if (!all_digits(req.ams) || !all_digits(req.slot))
        return { 400, "ams and slot are required (ams: the AMS id, 128+ for an AMS HT, 254 / 255 for an external spool; slot: 0..3)" };
    DeviceManager* dm = wxGetApp().getDeviceManager();
    if (!dm) return { 503, "no device manager" };
    MachineObject* obj = find_machine(dm, req.printer);
    if (!obj) return { 404, "no such printer: " + req.printer };
    if (!obj->is_online()) return { 409, obj->dev_name + " is offline" };
    if (obj->is_lan_mode_printer() && !obj->has_access_right())
        return { 409, obj->dev_name + " needs its access code entered on the PC first" };
    if (!obj->is_connected())
        return { 409, obj->dev_name + " is not connected; open it on the PC's Device tab once, or pick it in a send" };

    const std::string ams  = std::to_string(std::atoi(req.ams.c_str()));
    const std::string slot = std::to_string(std::atoi(req.slot.c_str()));
    const bool        ext  = ams == "254" || (ams == "255" && obj->is_multi_extruders());
    const AmsTray*    tray = nullptr;
    if (ext) {
        for (const AmsTray* t : ext_trays(obj))
            if (!obj->is_multi_extruders() || t->id == ams) tray = t;
        if (!tray) return { 404, obj->dev_name + " has no external spool " + ams };
    } else {
        tray = obj->get_ams_tray(ams, slot);
        if (!tray) return { 404, obj->dev_name + " has no AMS " + ams + " slot " + slot };
    }
    const bool exists = ext ? tray_present(*tray) : tray->is_exists;
    const auto a      = FilamentCommands::availability(filament_state(obj), exists, slot_loaded(obj, ams, ext ? "0" : slot));
    if (load && !a.can_load)
        return { 409, "cannot load that slot: " + (a.why.empty() ? std::string("it is already loaded") : a.why) };
    if (!load && !a.can_unload)
        return { 409, "cannot unload that slot: " + (a.why.empty() ? std::string("it is not the one loaded") : a.why) };

    p->kind            = "bambu";
    p->printer_name    = obj->dev_name;
    p->is_setting      = true;
    p->command         = req.action;
    p->call            = "command_ams_change_filament";
    p->multi_extruders = obj->is_multi_extruders();
    const std::string where = ext ? (p->multi_extruders ? (ams == "254" ? "left external spool" : "right external spool") : "external spool")
                                  : (std::atoi(ams.c_str()) >= FilamentCommands::BAMBU_HT_FIRST ? "AMS HT " + std::to_string(std::atoi(ams.c_str()) - 127)
                                                                                                 : "AMS " + std::to_string(std::atoi(ams.c_str()) + 1)) +
                                        " slot " + std::to_string(std::atoi(slot.c_str()) + 1);
    if (load) {
        if (ext) {
            // The external spool's own range, both ways (StatusPanel: old and new both from it);
            // the new protocol names the spool, the old one always 254.
            p->fil_old_temp = p->fil_new_temp = FilamentCommands::tray_mid_temp(tray->nozzle_temp_min, tray->nozzle_temp_max);
            p->fil_ams      = (obj->is_enable_np || obj->is_enable_ams_np) ? ams : std::string("254");
            p->fil_slot     = "0";
        } else {
            const AmsTray* curr = obj->get_curr_tray();
            if (curr) {
                p->fil_old_temp = FilamentCommands::tray_mid_temp(curr->nozzle_temp_min, curr->nozzle_temp_max);
                p->fil_new_temp = FilamentCommands::tray_mid_temp(tray->nozzle_temp_min, tray->nozzle_temp_max);
            }
            p->fil_ams  = ams;
            p->fil_slot = slot;
        }
        p->setting_text = "load " + where;
    } else {
        p->fil_ams      = ams;
        p->fil_slot     = "255";
        p->fil_old_temp = p->fil_new_temp = FilamentCommands::UNLOAD_DEFAULT_TEMP;
        p->setting_text = "unload " + where;
    }
    p->status_before      = obj->print_status;
    p->print_error_before = obj->print_error;
    out                   = p;
    BOOST_LOG_TRIVIAL(info) << "RemoteControl: " << p->setting_text << " prepared for " << obj->dev_name;
    return { 200, "" };
}

// A Snapmaker U1 over the LAN: the toolhead macros, only where its G-code help lists them.
static std::pair<int, std::string> prepare_filament_u1(const Request& req, std::shared_ptr<Prepared> p, std::shared_ptr<Prepared>& out)
{
    const bool load = req.action == "load_filament";
    if (!all_digits(req.slot)) return { 400, "slot is required (the toolhead, 0..3)" };
    SnapmakerLan::Device d;
    if (!SnapmakerLan::find(req.printer.substr(3), d)) return { 404, "no such printer: " + req.printer };
    p->kind         = "snapmaker";
    p->printer_name = d.name.empty() ? d.ip : d.name;
    SnapmakerLan::Status s;
    if (!SnapmakerLan::cached_status(d, s)) return { 409, p->printer_name + " has not answered yet; open its card once and try again" };
    if (!s.online) return { 409, p->printer_name + " is offline" };
    if (s.login_required) return { 409, p->printer_name + " requires a login for its LAN API, so it cannot be controlled from here" };
    if (!s.filament_macros)
        return { 409, p->printer_name + " does not list the load / unload commands (INNER_FILAMENT_UNLOAD, SM_PRINT_AUTO_FEED, ...), so this is left to its screen" };
    const int                                 index = std::atoi(req.slot.c_str());
    const std::vector<SnapmakerLan::Toolhead> heads = SnapmakerLan::toolheads(d);
    if (index < 0 || index >= (int) heads.size()) return { 404, p->printer_name + " has no toolhead " + std::to_string(index + 1) };
    const SnapmakerLan::Toolhead& h = heads[index];
    FilamentCommands::PrinterState ps;
    ps.printing  = s.printing();
    const auto a = FilamentCommands::availability(ps, true, h.loaded, FilamentCommands::is_flexible(h.type));
    if (load && !a.can_load) return { 409, "cannot load toolhead " + std::to_string(index + 1) + ": " + (a.why.empty() ? std::string("it is already loaded") : a.why) };
    if (!load && !a.can_unload) return { 409, "cannot unload toolhead " + std::to_string(index + 1) + ": " + (a.why.empty() ? std::string("it is empty") : a.why) };

    p->is_setting       = true;
    p->script           = load ? FilamentCommands::u1_load_script(index)
                               : FilamentCommands::u1_unload_script(index, FilamentCommands::u1_unload_temp(h.type), h.nozzle);
    p->setting_text     = (load ? "load toolhead " : "unload toolhead ") + std::to_string(index + 1);
    p->url              = SnapmakerLan::base_url(d) + "/printer/gcode/script?script=" + Http::url_encode(p->script);
    p->moonraker_method = "printer.gcode.script";
    p->timeout_s        = 300; // heating and feeding: the printer answers when the macro is done
    out                 = p;
    BOOST_LOG_TRIVIAL(info) << "RemoteControl: " << p->setting_text << " prepared for " << p->printer_name << " (assumed macros)";
    return { 200, "" };
}

std::pair<int, std::string> prepare(const Request& req, std::shared_ptr<Prepared>& out)
{
    if (is_filament_verb(req.action)) {
        if (req.printer.empty()) return { 400, "printer is required" };
        auto p        = std::make_shared<Prepared>();
        p->action     = req.action;
        p->dry_run    = req.dry_run || env_flag("SNORCA_SEND_DRYRUN");
        p->printer_id = req.printer;
        if (req.printer.compare(0, 3, "sm:") == 0) return prepare_filament_u1(req, p, out);
        if (req.printer == "host" || req.printer == "connect" || req.printer.compare(0, 3, "ph:") == 0)
            return { 409, "loading and unloading filament is not offered for this printer" };
        return prepare_filament_bambu(req, p, out);
    }
    if (is_setting_verb(req.action)) {
        if (req.printer.empty()) return { 400, "printer is required" };
        if (req.printer.compare(0, 3, "ph:") == 0) return { 409, "this printer's settings cannot be changed from here" };
        auto p        = std::make_shared<Prepared>();
        p->action     = req.action;
        p->dry_run    = req.dry_run || env_flag("SNORCA_SEND_DRYRUN");
        p->printer_id = req.printer;
        if (req.printer == "host" || req.printer == "connect" || req.printer.compare(0, 3, "sm:") == 0)
            return prepare_setting_moonraker(req, p, out);
        return prepare_setting_bambu(req, p, out);
    }
    if (is_print_error_verb(req.action)) {
        if (req.printer.empty()) return { 400, "printer is required" };
        // Only a Bambu printer has printer errors in this sense; "host", "connect" and sm:<id> are
        // Moonraker printers whose faults come through a different channel entirely.
        if (req.printer == "host" || req.printer == "connect" || req.printer.compare(0, 3, "sm:") == 0)
            return { 409, "printer-error actions are for Bambu printers; this one reports faults another way" };
        auto p        = std::make_shared<Prepared>();
        p->action     = req.action;
        p->dry_run    = req.dry_run || env_flag("SNORCA_SEND_DRYRUN");
        p->printer_id = req.printer;
        return prepare_error_action(req, p, out);
    }

    const ActionNames* a = action_names(req.action);
    if (!a) return { 400, "action must be pause, resume or stop, or one of the printer-error actions "
                          "/api/printers lists for the error it is reporting" };
    // Stopping a print throws the print away; pause and resume are reversible and the desktop does
    // not confirm them either (StatusPanel::on_subtask_pause_resume).
    if (req.action == "stop" && !req.confirm) return { 400, "stopping a print needs confirm=1" };
    if (req.printer.empty()) return { 400, "printer is required" };

    auto p     = std::make_shared<Prepared>();
    p->action  = req.action;
    p->dry_run = req.dry_run || env_flag("SNORCA_SEND_DRYRUN");
    p->printer_id = req.printer;
    if (req.printer == "host" || req.printer == "connect") return prepare_host(req, *a, p, out);
    if (req.printer.compare(0, 3, "sm:") == 0) return prepare_snapmaker_lan(req, *a, p, out);
    return prepare_bambu(req, *a, p, out);
}

// -------------------------------------------------------------------- run ----

// What the phone is told while the command is in flight.
static std::string error_wait_text(const std::shared_ptr<Prepared>& p)
{
    if (p->is_error_action || p->is_setting) return "waiting for the printer to answer";
    if (p->action == "pause")  return "waiting for the printer to pause";
    if (p->action == "resume") return "waiting for the printer to resume";
    return "waiting for the printer to stop";
}

// What the printer reports after a control command, watched for a few seconds so the phone learns
// whether it took (an H2-series printer without LAN-only mode + Developer Mode refuses third-party
// commands with "command verification failed", exactly as it does for a print). For an error
// action the thing watched is the error code going away, not a print-state transition.
static void watch_bambu(std::shared_ptr<Prepared> p, json& result)
{
    struct Watch { std::mutex m; std::string state { "unknown" }, status, err_text; int err { 0 }; };
    auto w = std::make_shared<Watch>(); // shared: a timed-out GUI call may still run after this loop
    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        on_main([w, p]() {
            DeviceManager* dm = wxGetApp().getDeviceManager();
            if (!dm) return;
            MachineObject* obj = find_machine(dm, p->printer_id);
            if (!obj) return;
            std::lock_guard<std::mutex> lock(w->m);
            if (w->state != "unknown") return;
            w->status = obj->print_status;
            if (obj->print_error != 0 && obj->print_error != p->print_error_before) {
                w->err      = obj->print_error;
                w->state    = "error";
                w->err_text = print_error_message(obj->dev_id, w->err);
            } else if (p->is_error_action) {
                // What an error action is waiting for is the code going away. Whether the print
                // then runs, stops or stays paused is the action's own business - an ignore that
                // leaves it paused has still worked - so the print state is reported and not
                // judged. A code that is simply still there after ten seconds is not a failure
                // either: the printer may need longer, and the status is the answer.
                if (obj->print_error == 0) w->state = "error_cleared";
            } else if (p->action == "pause" && obj->print_status == "PAUSE") {
                w->state = "paused";
            } else if (p->action == "resume" && obj->print_status == "RUNNING") {
                w->state = "printing";
            } else if (p->action == "stop" && !MachineObject::is_in_printing_status(obj->print_status)) {
                w->state = "stopped";
            }
        }, 3000);
        std::lock_guard<std::mutex> lock(w->m);
        if (w->state != "unknown") break;
    }
    std::lock_guard<std::mutex> lock(w->m);
    result["printer_state"]  = w->state;
    result["status_after"]   = w->status;
    if (w->state == "error")
        result["printer_error"] = { { "code", error_code_text(w->err) }, { "message", w->err_text } };
}

static void run_bambu(std::shared_ptr<Prepared> p, Sink& sink)
{
    json result;
    result["kind"]          = "bambu";
    result["action"]        = p->action;
    result["printer"]       = { { "id", p->printer_id }, { "name", p->printer_name } };
    result["call"]          = p->call;
    result["command"]       = p->command;
    result["status_before"] = p->status_before;
    if (p->is_error_action) {
        result["error_action"] = true;
        result["err"]          = p->err_code;
        result["err_arg"]      = p->err_arg;
        result["job_id"]       = p->job_id;
    }
    if (p->is_setting) {
        result["setting"] = p->setting_text;
        json args         = json::object();
        if (p->action == "set_temp") {
            args["heater"] = p->heater;
            args["target"] = p->int_value;
            if (p->extruder_id >= 0) args["extruder_id"] = p->extruder_id;
        } else if (p->action == "set_speed") {
            args["level"] = p->int_value;
        } else if (p->action == "set_light") {
            args["on"] = p->light_on;
        } else if (is_filament_verb(p->action)) {
            args["ams_id"]    = p->fil_ams;
            args["slot_id"]   = p->fil_slot;
            args["curr_temp"] = p->fil_old_temp;
            args["tar_temp"]  = p->fil_new_temp;
            // What goes over MQTT, bar the sequence id: the same builder the Device tab uses.
            args["payload"]   = FilamentCommands::ams_change_filament_json(p->action == "load_filament", p->fil_ams, p->fil_slot,
                                                                           p->fil_old_temp, p->fil_new_temp, p->multi_extruders);
        } else {
            args["fan"]      = p->heater;
            args["fan_type"] = p->fan_type;
            args["percent"]  = p->int_value;
            args["value"]    = DeviceControls::bambu_fan_value(p->int_value);
        }
        result["args"] = args;
    }
    if (p->dry_run) {
        result["dry_run"] = true;
        sink.progress(99, "dry run: nothing was sent");
        sink.done(true, "", result);
        return;
    }
    // The command itself is one MQTT publish; it is sent from the GUI thread because the
    // MachineObject lives there, exactly as the desktop's own buttons do.
    auto rc = std::make_shared<int>(-1);
    const bool ran = on_main([rc, p]() {
        DeviceManager* dm = wxGetApp().getDeviceManager();
        if (!dm) return;
        MachineObject* obj = find_machine(dm, p->printer_id);
        if (!obj) return;
        if (p->is_error_action) {
            // The same last-moment check prepare() made, repeated on the GUI thread the instant
            // before the publish: between the two the printer may have cleared the error or hit
            // another one, and this is the only place where "the code is still this one" and "the
            // command goes out" are not separated by a thread hop.
            if (obj->print_error == 0 || error_code_text(obj->print_error) != p->err_code) {
                *rc = -2;
                return;
            }
            if (p->action == "resume_error")           *rc = obj->command_hms_resume(p->err_arg, p->job_id);
            else if (p->action == "stop_error")        *rc = obj->command_hms_stop(p->err_arg, p->job_id);
            else if (p->action == "ignore_error")      *rc = obj->command_hms_ignore(p->err_arg, p->job_id);
            else if (p->action == "idle_ignore_error") *rc = obj->command_hms_idle_ignore(p->err_arg, 0);
            else if (p->action == "ack_close")         *rc = obj->command_clean_print_error_uiop(obj->print_error);
            // Both of these are built from the blob captured in prepare(), not from whatever the
            // object holds now: between the two the printer may have been refused another command
            // and replaced it. A blob that went away in the meantime is the same -2 case as a code
            // that moved on, because answering with a stale one is exactly what must not happen.
            else if (p->action == "ack_proceed" || p->action == "dont_remind") {
                if (p->action_json.is_null() || !obj->has_remote_command_error_action_json()) { *rc = -2; return; }
                *rc = p->action == "ack_proceed" ? obj->command_ack_proceed(p->action_json)
                                                 : obj->command_dont_remind_next_time(p->action_json);
            }
            else                                       *rc = -3; // no path here builds one of the rest
            return;
        }
        if (p->is_setting) {
            // Exactly the calls the Device tab's own controls make (StatusPanel::on_set_bed_temp,
            // send_nozzle_temp, on_set_chamber_temp, the speed popup, on_lamp_switch, FanControl).
            if (is_filament_verb(p->action)) {
                *rc = obj->command_ams_change_filament(p->action == "load_filament", p->fil_ams, p->fil_slot, p->fil_old_temp,
                                                       p->fil_new_temp);
            } else if (p->action == "set_temp") {
                if (p->heater == "bed")          *rc = obj->command_set_bed(p->int_value);
                else if (p->heater == "chamber") *rc = obj->command_set_chamber(p->int_value);
                else if (p->dual_nozzle)         *rc = obj->command_set_nozzle_new(p->extruder_id, p->int_value);
                else                             *rc = obj->command_set_nozzle(p->int_value);
            } else if (p->action == "set_speed") {
                *rc = obj->command_set_printing_speed((PrintingSpeedLevel) p->int_value);
            } else if (p->action == "set_light") {
                *rc = obj->command_set_chamber_light(p->light_on ? MachineObject::LIGHT_EFFECT::LIGHT_EFFECT_ON
                                                                 : MachineObject::LIGHT_EFFECT::LIGHT_EFFECT_OFF);
            } else {
                *rc = obj->command_control_fan_val((MachineObject::FanType) p->fan_type, DeviceControls::bambu_fan_value(p->int_value));
            }
            return;
        }
        if (p->action == "pause")       *rc = obj->command_task_pause();
        else if (p->action == "resume") *rc = obj->command_task_resume();
        else                            *rc = obj->command_task_abort();
    }, 10000);
    result["result_code"] = *rc;
    if (!ran) { sink.done(false, "the PC did not send the command in time", result); return; }
    if (*rc == -2) {
        // Either the code moved on or, for the two verbs built from it, the printer's details for
        // this error went with it - which only happens when the error itself is gone, since the
        // blob is dropped the moment its code stops being the one reported.
        sink.done(false, p->printer_name + " is no longer reporting error " + HMSQuery::pretty_code(p->err_code) +
                             ", so nothing was sent", result);
        return;
    }
    if (*rc != 0) { sink.done(false, p->printer_name + " did not accept the command (code " + std::to_string(*rc) + ")", result); return; }

    BOOST_LOG_TRIVIAL(info) << "RemoteControl: " << p->call << " sent to " << p->printer_name
                            << (p->is_error_action ? " for error " + p->err_code : std::string())
                            << (p->is_setting ? " (" + p->setting_text + ")" : std::string());
    // A setting is one publish with nothing to watch for: the new target, level or switch shows up
    // in the printer's next status push, which the phone's next refresh reads.
    if (p->is_setting) { sink.done(true, "", result); return; }
    sink.progress(60, error_wait_text(p));
    watch_bambu(p, result);
    if (result["printer_state"] == "error") {
        const std::string code = result["printer_error"]["code"];
        const std::string msg  = result["printer_error"]["message"];
        sink.done(false, "the printer refused the command (error " + code + (msg.empty() ? "" : ": " + msg) + ")", result);
        return;
    }
    // An error action that did not clear its code in the ten seconds watched: the command went
    // out and the printer has not let go of the fault. Reported as a success with the fact in the
    // text, not as a failure - the fault may simply still be there (an ignore does not fix it,
    // and a resume on a condition that persists comes straight back), and calling that "the
    // command failed" would send the person looking in the wrong place.
    if (p->is_error_action && result["printer_state"] == "unknown")
        result["note"] = "the printer is still reporting error " + HMSQuery::pretty_code(p->err_code);
    sink.done(true, "", result);
}

// A Snapmaker over Moonraker. The HTTP API the printer serves is the path both a connected printer
// and one the PC has never connected to can use, so it is tried first; a printer reached only over
// the Device tab's MQTT socket falls back to that socket's own printer.print.* method.
static void run_host(std::shared_ptr<Prepared> p, Sink& sink)
{
    json result;
    result["kind"]      = p->kind;
    result["action"]    = p->action;
    result["printer"]   = { { "id", p->printer_id }, { "name", p->printer_name } };
    result["url"]       = p->url;
    result["method"]    = p->moonraker_method;
    result["transport"] = "http";
    if (p->is_setting) {
        result["setting"] = p->setting_text;
        result["script"]  = p->script;
    }
    if (p->dry_run) {
        result["dry_run"] = true;
        sink.progress(99, "dry run: nothing was sent");
        sink.done(true, "", result);
        return;
    }
    sink.progress(40, "sending " + p->action + " to the printer");
    std::string body, error;
    if (is_filament_verb(p->action)) sink.progress(30, p->setting_text + ": the printer is heating and feeding");
    if (moonraker_http(p->url, true, body, error, p->timeout_s)) {
        result["reply"] = parse_or_raw(body);
        sink.done(true, "", result);
        return;
    }
    result["http_error"] = error;
    if (!body.empty()) result["reply"] = parse_or_raw(body);
    // A setting has no MQTT fallback: the Device tab's socket speaks print controls, not G-code.
    if (p->is_setting) { sink.done(false, p->printer_name + " refused the command: " + error, result); return; }
    if (!p->host) { sink.done(false, p->printer_name + " refused the command: " + error, result); return; }

    // The MQTT fallback: the socket the PC's Device tab opened, the way its own page pauses.
    sink.progress(70, "the printer's web API refused; trying the connection the PC holds");
    result["transport"] = "mqtt";
    auto reply = std::make_shared<std::promise<json>>();
    auto once  = std::make_shared<std::atomic<bool>>(false);
    auto fut   = reply->get_future();
    auto cb    = [reply, once](const json& r) { if (!once->exchange(true)) reply->set_value(r); };
    if (p->action == "pause")       p->host->async_pause_print_job(cb);
    else if (p->action == "resume") p->host->async_resume_print_job(cb);
    else                            p->host->async_cancel_print_job(cb);
    if (fut.wait_for(std::chrono::seconds(15)) != std::future_status::ready) {
        sink.done(false, p->printer_name + " did not answer the " + p->action + " (its web API said: " + error + ")", result);
        return;
    }
    const json r    = fut.get();
    result["reply"] = r;
    if (r.is_null() || (r.is_object() && r.contains("error"))) {
        sink.done(false, "the printer refused the " + p->action + ": " + (r.is_null() ? std::string("no reply") : r["error"].dump()), result);
        return;
    }
    sink.done(true, "", result);
}

void run(std::shared_ptr<Prepared> p, Sink sink)
{
    try {
        if (p->kind == "bambu") run_bambu(p, sink);
        else                    run_host(p, sink);
    } catch (const std::exception& e) {
        sink.done(false, std::string("the command failed: ") + e.what(), json::object());
    } catch (...) {
        sink.done(false, "the command failed", json::object());
    }
}

// ----------------------------------------------------------- /api/printers ----

void describe_bambu(MachineObject* m, json& p)
{
    // The desktop's own three predicates, so the phone's buttons light up exactly as its do.
    p["can_pause"]  = m->can_pause();
    p["can_resume"] = m->can_resume();
    p["can_stop"]   = m->can_abort();
    // print_status under its own name: "status" has carried the same string since the first
    // version of this API, and the control UI reads the field the desktop's code names.
    p["print_status"] = m->print_status;
    p["stage"]        = std::string(m->get_curr_stage().ToUTF8().data());
    if (m->print_error != 0)
        p["print_error"] = print_error_json(m);
    else
        p["print_error"] = nullptr;
    // The HMS summary: how many the printer is reporting and what the first one says, so a card can
    // show "why" without a second request.
    json hms = json::object();
    hms["count"] = (int) m->hms_list.size();
    if (!m->hms_list.empty()) {
        HMSItem&          first = m->hms_list.front();
        const std::string code  = first.get_long_error_code();
        hms["code"]             = code;
        if (HMSQuery* q = wxGetApp().get_hms_query())
            hms["message"] = std::string(q->describe_error(m->dev_id, code).ToUTF8().data());
    }
    p["hms"] = hms;
    // The native printer screen's controls and the AMS, from the same object.
    p["controls"]   = DeviceControls::to_json(bambu_caps(m));
    p["ams"]        = ams_json(m);
    p["ext_spools"] = ext_spools_json(m);
}

void list_host_targets(std::vector<HostTarget>& out)
{
    // Any print host address, not only one the app calls a Moonraker: the fork has no host_type
    // string for Moonraker (only the Device tab's MQTT connect sets that enum), so what a printer
    // speaks is decided by what it answers, not by the preset.
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle && !bundle->use_bbl_network()) {
        const DynamicPrintConfig& cfg = bundle->printers.get_edited_preset().config;
        const std::string         url = cfg.opt_string("print_host");
        if (!url.empty()) {
            // The preset says what it is: a PrusaLink or PrusaConnect preset is asked over its own
            // REST API with the credentials it holds, everything else as a Moonraker printer (the
            // fork has no host_type key for Moonraker, so that stays decided by what it answers).
            const PrintHostDevices::Device d = PrintHostDevices::from_config(cfg);
            if (PrusaLinkStatus::speaks_prusalink(d.host_type))
                out.push_back({ "host", PrusaLinkStatus::base_url(url), d.host_type, d.auth_type,
                                d.apikey, d.user, d.password });
            else
                out.push_back({ "host", moonraker_base(url), "", "", "", "", "" });
        }
    }
    std::shared_ptr<PrintHost> connected;
    wxGetApp().get_connect_host(connected);
    // Not when the Device tab reached the printer through the Snapmaker cloud: its host is then the
    // cloud's MQTT broker, which answers no Moonraker request - asking it cost a timeout on every
    // poll and could only ever say "offline". The connect card keeps the MQTT link's own state.
    if (connected && !SnapmakerLan::is_cloud_host(connected->get_host()))
        out.push_back({ "connect", moonraker_base(connected->get_host()), "", "", "", "", "" });
    // The model's own devices (<datadir>/hub/print_host_devices.json), under the ids /api/printers
    // gives them. Only the Moonraker-shaped ones are worth asking - an Elegoo Link box answers SDCP
    // over its own websocket and would just spend this call's timeout - so the rest are left with
    // the "unknown" status list_hosts gave them.
    if (bundle && !bundle->use_bbl_network()) try {
        // api_printers calls this before RemoteSend::list_hosts (its HostsAtEnd runs last), so the
        // import has to happen here too or the very first poll would list the devices without ever
        // having probed them.
        PrintHostDevices::migrate_from_presets(*bundle);
        const std::string model_key = PrintHostDevices::current_model_key(*bundle);
        for (const PrintHostDevices::Device& d : PrintHostDevices::devices(model_key)) {
            if (d.address.empty()) continue;
            if (PrintHostDevices::speaks_moonraker(d.host_type)) {
                out.push_back({ "ph:" + d.id, moonraker_base(d.address), "", "", "", "", "" });
            } else if (PrusaLinkStatus::speaks_prusalink(d.host_type)) {
                // Its own base (no MQTT-port stripping: a PrusaLink box is addressed exactly as
                // the preset holds it) and its own credentials, which the probe needs.
                out.push_back({ "ph:" + d.id, PrusaLinkStatus::base_url(d.address), d.host_type,
                                d.auth_type, d.apikey, d.user, d.password });
            }
        }
    } catch (...) {}
}

// Klipper's print_stats.state, in the words the rest of this API uses. Only the control fields are
// filled: a print host's progress is not this feature's business.
static void fill_from_print_stats(const json& stats, json& p)
{
    const std::string state = stats.value("state", std::string());
    p["print_status"] = state;
    p["can_pause"]    = state == "printing";
    p["can_resume"]   = state == "paused";
    p["can_stop"]     = state == "printing" || state == "paused";
    p["stage"]        = stats.value("filename", std::string()); // the job it is on
    const std::string message = stats.value("message", std::string());
    if (state == "error" || !message.empty())
        p["print_error"] = { { "code", state == "error" ? "error" : "" }, { "message", message } };
    else
        p["print_error"] = nullptr;
}

// A number the printer reported under `key`, or nothing. Klipper answers an object it does not
// have (extruder2 on a two-nozzle printer) with an empty one, or leaves it out altogether.
static bool num_of(const json& obj, const char* key, double& out)
{
    if (!obj.is_object() || !obj.contains(key) || !obj[key].is_number()) return false;
    out = obj[key].get<double>();
    return true;
}

// The temperatures a Bambu entry carries, from the same answer: the bed and every extruder Klipper
// has (extruder, extruder1, ...). A host that reports none is left without bed_temp / nozzles, and
// the phone then leaves the temperature block off its card instead of rendering NaN.
static void fill_from_heaters(const json& status, json& p)
{
    double temp = 0, target = 0;
    if (status.contains("heater_bed") && num_of(status["heater_bed"], "temperature", temp) &&
        num_of(status["heater_bed"], "target", target)) {
        p["bed_temp"]   = temp;
        p["bed_target"] = target;
    }
    json nozzles = json::array();
    for (int i = 0; i < 4; ++i) {
        const std::string ex = i == 0 ? "extruder" : ("extruder" + std::to_string(i));
        if (!status.contains(ex) || !num_of(status[ex], "temperature", temp) || !num_of(status[ex], "target", target)) continue;
        nozzles.push_back(json { { "temp", temp }, { "target", target } });
    }
    if (!nozzles.empty()) p["nozzles"] = nozzles;
}

// A PrusaLink answer in the fields a card already reads. The state vocabulary is mapped to
// Klipper's by PrusaLinkStatus::to_klipper_state, so the page, the buttons and the event watcher
// need no PrusaLink-specific code: "printing" is "printing" whoever said it.
static void fill_from_prusalink(const PrusaLinkStatus::Status& pl, json& p)
{
    p["print_status"] = pl.state;
    p["can_pause"]    = pl.state == "printing";
    p["can_resume"]   = pl.state == "paused";
    p["can_stop"]     = pl.state == "printing" || pl.state == "paused";
    p["stage"]        = pl.filename; // the job it is on, where fill_from_print_stats puts it
    if (pl.state == "error")
        p["print_error"] = { { "code", pl.raw_state }, { "message", std::string() } };
    else
        p["print_error"] = nullptr;
    // The same temperature shape a Bambu entry and a Moonraker host carry: a Buddy board has one
    // bed and one nozzle, so the nozzles array has exactly one entry when it reported one.
    if (pl.has_bed) {
        p["bed_temp"]   = pl.bed_temp;
        p["bed_target"] = pl.bed_target;
    }
    if (pl.has_nozzle)
        p["nozzles"] = json::array({ json { { "temp", pl.nozzle_temp }, { "target", pl.nozzle_target } } });
    // What Moonraker never gave us and PrusaLink does: how far along it is, and how long is left.
    // Under the names a Bambu and a Snapmaker-LAN entry already use, so the card's existing
    // "62% - 24m left" line renders for a Prusa printer with no page change. Only ever set while it
    // is actually mid-job and actually said so: a card that gets no percent shows none.
    if ((pl.state == "printing" || pl.state == "paused") && pl.has_progress) {
        p["printing"]    = true;
        p["percent"]     = (int) (pl.progress + 0.5);
        p["left_time_s"] = pl.has_time_remaining ? pl.time_remaining : 0;
    }
    // The raw pair as well, un-rounded, for anything that wants them (the app's /api/printers).
    if (pl.has_progress)       p["progress"]       = pl.progress;
    if (pl.has_time_remaining) p["time_remaining"] = pl.time_remaining;
    if (pl.has_time_printing)  p["time_printing"]  = pl.time_printing;
}

// One address's answer, or the fact that it was not asked (the backoff).
struct HostAnswer
{
    bool                     asked { false };
    json                     status, stats;
    std::string              error;
    bool                     prusalink { false }; // answered as a PrusaLink printer, not a Moonraker one
    PrusaLinkStatus::Status  pl;
};

void describe_hosts(const std::vector<HostTarget>& targets, json& printers)
{
    if (targets.empty() || !printers.is_array()) return;
    // Side by side, not one after the other: an address that is off must cost this call its own
    // two-second timeout and not everybody else's as well. Waiting for all of them is therefore
    // bounded by the longest single request, which is what the caller (the /api/printers probe and
    // the event watcher's five-second poll) can afford.
    std::vector<std::future<HostAnswer>> pending;
    pending.reserve(targets.size());
    for (const HostTarget& t : targets)
        pending.push_back(std::async(std::launch::async, [t]() {
            HostAnswer a;
            if (t.base.empty() || !ask_again(t.base)) return a;
            a.asked = true;
            if (PrusaLinkStatus::speaks_prusalink(t.host_type)) {
                // Its own REST API, with the preset's credentials. Read-only: /api/v1/status, and
                // /api/v1/job only while it says it is printing. Never a command.
                PrusaLinkStatus::Auth auth;
                auth.auth_type = t.auth_type;
                auth.apikey    = t.apikey;
                auth.user      = t.user;
                auth.password  = t.password;
                a.pl           = PrusaLinkStatus::probe(t.base, auth, 2);
                a.prusalink    = a.pl.answered;
                a.error        = a.pl.error;
                if (!a.pl.answered && !a.pl.authorized)
                    a.error = "the printer refused the API key or password in its preset";
                return a;
            }
            // Read-only: what the printer says it is doing and how warm it is (the objects the LAN
            // list asks a Snapmaker for; extruder1.. answer empty where there is no such nozzle).
            // Never a command.
            std::string body;
            if (moonraker_http(t.base + "/printer/objects/query?print_stats&heater_bed&extruder&extruder1&extruder2&extruder3&" +
                                   DeviceControls::moonraker_controls_query(),
                               false, body, a.error, 2)) {
                const json j = parse_or_raw(body);
                if (j.is_object()) {
                    a.status = j.value("result", json::object()).value("status", json::object());
                    if (a.status.is_object()) a.stats = a.status.value("print_stats", json::object());
                }
            }
            return a;
        }));
    for (size_t i = 0; i < targets.size(); ++i) {
        const HostTarget& t = targets[i];
        HostAnswer        a;
        try {
            a = pending[i].get();
        } catch (...) {}
        json* entry = nullptr;
        for (json& p : printers)
            if (p.is_object() && p.value("id", std::string()) == t.id) { entry = &p; break; }
        if (!entry || t.base.empty()) continue;
        const std::string& error    = a.error;
        const bool         answered = a.prusalink || (a.stats.is_object() && !a.stats.empty());
        const std::string  state    = a.prusalink ? a.pl.state :
                                      (answered ? a.stats.value("state", std::string()) : std::string());
        // The backoff cache is keyed on "this address answers a status API we speak", which a
        // PrusaLink printer does - so it is polled every five seconds like a Moonraker one, and an
        // address that answered neither is left alone for half a minute.
        const DeviceControls::Caps caps = (answered && !a.prusalink) ? DeviceControls::moonraker_caps(a.status) : DeviceControls::Caps();
        if (a.asked) remember_probe(t.base, answered, state, caps);
        const bool is_device = t.id.compare(0, 3, "ph:") == 0; // a print-host device, not the preset
        if (answered) {
            if (a.prusalink) {
                fill_from_prusalink(a.pl, *entry);
            } else {
                fill_from_print_stats(a.stats, *entry);
                fill_from_heaters(a.status, *entry);
                // The settings a Moonraker printer takes from the phone. Only the preset's host and
                // the Device tab's connect can be told anything (a ph: device is read-only here).
                if (!is_device && !caps.heaters.empty()) (*entry)["controls"] = DeviceControls::to_json(caps);
            }
            // A device card carries the same `status` string a Snapmaker card does; list_hosts left
            // it "unknown" for everything that was not probed.
            if (is_device) {
                (*entry)["status"] = state;
                (*entry)["online"] = true;
            }
        } else {
            if (is_device && a.asked) (*entry)["online"] = false;
            // It is not a Moonraker printer, or it is off: leave every button off rather than guess.
            (*entry)["can_pause"]   = false;
            (*entry)["can_resume"]  = false;
            (*entry)["can_stop"]    = false;
            (*entry)["print_error"] = nullptr;
            if (!error.empty()) (*entry)["status_error"] = error;
        }
    }
}

} // namespace RemoteControl
} // namespace GUI
} // namespace Slic3r
