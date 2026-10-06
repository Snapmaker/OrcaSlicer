#include "RemoteEvents.hpp"

#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "HMS.hpp"
#include "PrintErrorCommands.hpp"
#include "RemoteControl.hpp"
#include "RemoteHub.hpp"
#include "SnapmakerLan.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <wx/utils.h>

namespace Slic3r {
namespace GUI {
namespace RemoteEvents {

using nlohmann::json;

// How often the watcher really looks. The heartbeat ticks every second; a Bambu MachineObject is
// pushed to at a few Hz and a Moonraker printer is polled, so anything faster than this buys
// nothing and costs the GUI thread and the printers' web servers.
static const long long POLL_MS = 5000;

// How long one poll waits for the LAN printers it asked. It is the longest single request a probe
// makes (SnapmakerLan::probe asks for the printer's objects with a five-second timeout), so a
// whole poll can never cost more than one printer's own worst case however many printers there
// are. A printer slower than that is left at the answer its cache holds; its probe finishes in
// the background into that same cache and the next poll reads it there.
static const long long PROBE_BUDGET_MS = 5000;

// A probe worth naming in the poll's timing line.
static const long long SLOW_PROBE_MS = 1000;

static long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string hex8(int code)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", (unsigned) code);
    return buf;
}

std::string notification_body(const std::string& text, const std::string& code)
{
    std::string body = text;
    if (code.empty()) return body;
    // Either spelling counts as already named: the text carries the grouped form when it came
    // from HMSQuery::format_error, and a relayed body may carry the raw one.
    const std::string pretty = HMSQuery::pretty_code(code);
    if (body.find(code) != std::string::npos || (!pretty.empty() && body.find(pretty) != std::string::npos)) return body;
    return body + " (" + pretty + ")";
}

// ------------------------------------------------------------ the rule ----

json Event::to_json(long instance_pid) const
{
    json j;
    j["instance"] = (long long) instance_pid;
    j["printer"]  = { { "id", printer_id }, { "name", printer_name }, { "kind", printer_kind } };
    j["kind"]     = kind;
    j["severity"] = severity;
    j["title"]    = title;
    j["text"]     = text;
    if (!code.empty()) j["code"] = code;
    if (!job.empty()) j["job"] = job;
    // The printer's job id, whenever the event has one: the hub keys "one start per job" on it.
    if (!job_id.empty()) j["job_id"] = job_id;
    // The buttons, when there are any. Left off entirely otherwise, so every consumer that was
    // written before this field keeps seeing exactly the payload it saw - the text, title and
    // severity are untouched by it.
    if (!actions.empty()) {
        json arr = json::array();
        for (const PrintErrorEventAction& a : actions)
            arr.push_back({ { "id", a.id },
                            { "verb", a.verb },
                            { "label", a.label },
                            { "needs_job_id", a.needs_job_id },
                            { "needs_details", a.needs_action_json },
                            { "remote_safe", a.remote_safe } });
        j["actions"] = arr;
    }
    return j;
}

// The word the phone shows for a printer that is doing something.
static std::string job_phrase(const PrinterState& p)
{
    return p.job.empty() ? std::string() : (" \xE2\x80\xA2 " + p.job); // " • <job>"
}

static bool real_job_id(const std::string& id) { return !id.empty() && id != "0"; }

static bool busy_state(const std::string& s) { return s == "printing" || s == "paused" || s == "preparing"; }

static Event make_event(const PrinterState& p, const char* kind, const char* severity, const std::string& title,
                        const std::string& text)
{
    Event e;
    e.printer_id   = p.id;
    e.printer_name = p.name;
    e.printer_kind = p.kind;
    e.kind         = kind;
    e.severity     = severity;
    e.title        = title;
    e.text         = text;
    e.job          = p.job;
    return e;
}

static Event started_event(const PrinterState& p, const std::string& name)
{
    Event e = make_event(p, "started", "info", name + " started printing", name + " started a print" + job_phrase(p) + ".");
    if (real_job_id(p.job_id)) e.job_id = p.job_id;
    return e;
}

// The cooldown key: one printer, one kind, and the thing that makes this occurrence different from
// the last one - the error code where there is one, the job otherwise. So two starts of the same
// file in a minute are one event (a reconnect, a flap), while starting a second file is its own.
static std::string cooldown_key(const Event& e)
{
    return e.printer_id + "|" + e.kind + "|" + (e.code.empty() ? e.job : e.code);
}

// Whether two job names are the same print. A printer that reports the same file with a different
// spelling between polls - a leading "/", a cache prefix, the extension dropped, or the Windows
// separators a print host hands back - was announcing a second start for the same job. Compared on
// the basename, case-folded, with the usual G-code extensions off.
static std::string job_key(const std::string& job)
{
    std::string s = job;
    // Whitespace first: a name that picked up a trailing space still has to have its extension
    // recognised, or " bench.3mf " and "bench" would be told apart by the space alone.
    while (!s.empty() && (s.front() == ' ' || s.front() == '	')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '	')) s.pop_back();
    const size_t cut = s.find_last_of("/\\");
    if (cut != std::string::npos) s = s.substr(cut + 1);
    auto ends_with_ci = [](const std::string& str, const char* suffix) {
        const size_t n = std::strlen(suffix);
        if (str.size() <= n) return false;
        for (size_t i = 0; i < n; ++i)
            if (std::tolower((unsigned char) str[str.size() - n + i]) != std::tolower((unsigned char) suffix[i])) return false;
        return true;
    };
    static const char* const exts[] = { ".gcode.3mf", ".gcode.gz", ".3mf", ".gcode", ".gco", ".bgcode" };
    for (const char* e : exts)
        if (ends_with_ci(s, e)) { s.resize(s.size() - std::strlen(e)); break; }
    for (char& c : s) c = (char) std::tolower((unsigned char) c);
    // Whatever the extension left behind.
    while (!s.empty() && (s.back() == ' ' || s.back() == '	' || s.back() == '.')) s.pop_back();
    return s;
}

static bool terminal_state(const std::string& s) { return s == "finished" || s == "failed" || s == "cancelled"; }

// "No information": the watcher cannot see this printer's print state at all. Not a state change,
// and never a reason to forget what it was doing - a Bambu that was re-seeded, a Snapmaker that
// wanted a login, a print host that stopped answering all land here.
static bool no_information(const PrinterState& p) { return !p.watched || !p.online || p.state.empty(); }

// The one question a "started" has to answer: is this a print nobody has been told about?
//
// Yes when the printer has no job memory at all, when the job is a different file, or when the job
// it remembers has ended (a finished / cancelled / failed came in between). No when the same job is
// already announced and nothing ended it - which is every repeat the flaps produced: an idle blip,
// an offline blink and re-seed, a reconnect, a second spelling of the same file name.
//
// Updates the memory as a side effect, so a caller that asks is the caller that announces.
static bool start_is_new(JobMemory& jm, const std::string& job, const std::string& job_id, long long at)
{
    // The printer's own job id decides where both sides have one: a different id is a different
    // print even under the same file name, and the same id is the same print under any spelling.
    // Without one on either side, the name (job_key) is all there is.
    const bool same_job = jm.announced && (real_job_id(job_id) && real_job_id(jm.job_id) ? job_id == jm.job_id
                                                                                         : job_key(jm.job) == job_key(job));
    if (same_job && jm.terminal.empty()) return false;
    jm.job         = job;
    jm.job_id      = real_job_id(job_id) ? job_id : std::string();
    jm.announced   = true;
    jm.started_at  = at;
    jm.terminal.clear();
    jm.terminal_at = 0;
    return true;
}

std::vector<Event> step(Memory& mem, const Snapshot& now, long long cooldown_ms)
{
    return step(mem, now, cooldown_ms, nullptr);
}

std::vector<Event> step(Memory& mem, const Snapshot& now, long long cooldown_ms, std::vector<RawChange>* raw_changes)
{
    std::vector<Event> out;
    for (const auto& kv : now.printers) {
        const PrinterState& cur = kv.second;
        // Every change in the printer's own words, whether or not it maps to a new normalised
        // state and whether or not the watcher can act on it. This is the log line that names
        // which printer is flapping and how fast.
        {
            auto              rit  = mem.last_raw.find(kv.first);
            const std::string prev_raw = rit == mem.last_raw.end() ? std::string("<none>") : rit->second;
            const std::string this_raw = cur.raw_state.empty() ? std::string("<none>") : cur.raw_state;
            if (rit == mem.last_raw.end() || rit->second != cur.raw_state) {
                if (raw_changes && rit != mem.last_raw.end()) {
                    auto prev_p = mem.last.printers.find(kv.first);
                    RawChange rc;
                    rc.printer_id  = kv.first;
                    rc.from        = prev_raw;
                    rc.to          = this_raw;
                    rc.was_visible = prev_p != mem.last.printers.end() && !no_information(prev_p->second);
                    rc.visible     = !no_information(cur);
                    rc.at          = now.at;
                    raw_changes->push_back(rc);
                }
                mem.last_raw[kv.first] = cur.raw_state;
            }
        }
        // The seeding poll, remembered: the first snapshot in which this printer's state could be
        // read at all. It says nothing about events - it is what a caller reads to know that the
        // watcher has this printer in hand, so the next thing it does will be reported.
        if (cur.watched && cur.online)
            mem.seen_at.emplace(kv.first, now.at);
        else
            mem.seen_at.erase(kv.first);
        // The error codes this printer holds, updated on every snapshot in which it can be seen
        // at all - the seeding one included, so a code that was already up when the watcher first
        // saw the printer is held (and never announced) rather than announced on the next poll.
        bool error_is_new = false;
        if (!no_information(cur)) {
            std::map<std::string, long long>& held    = mem.codes[kv.first];
            std::vector<std::string>          present = cur.active_codes;
            if (!cur.error_code.empty() && std::find(present.begin(), present.end(), cur.error_code) == present.end())
                present.push_back(cur.error_code);
            for (auto it = held.begin(); it != held.end();) {
                if (it->second != 0 && now.at - it->second >= ERROR_CLEAR_MS) {
                    it = held.erase(it); // gone long enough: it cleared, and a return is a new one
                    continue;
                }
                const bool here = std::find(present.begin(), present.end(), it->first) != present.end();
                if (here)
                    it->second = 0;
                else if (it->second == 0)
                    it->second = now.at; // just went missing
                ++it;
            }
            error_is_new = !cur.error_code.empty() && held.count(cur.error_code) == 0;
            for (const std::string& c : present) held[c] = 0;
        }
        auto                prev_it = mem.last.printers.find(kv.first);
        // A printer nobody can see the state of says nothing. Same for one that has only just
        // appeared, or that was offline / unwatched last time: the first watched snapshot seeds the
        // memory and nothing more, so starting the slicer next to a printer that is already halfway
        // through a job does not announce a start, and a reconnect does not replay one.
        //
        // The job memory is deliberately NOT touched here: "I cannot see it" must leave what the
        // printer was last known to be doing exactly as it was, or a printer that blinks offline
        // and comes back mid-print announces its start a second time.
        if (no_information(cur) || prev_it == mem.last.printers.end()) continue;
        const PrinterState& prev = prev_it->second;
        if (no_information(prev)) continue;

        const std::string name = cur.name.empty() ? cur.id : cur.name;
        JobMemory&        jm   = mem.jobs[kv.first];

        // A printer error, whatever the print state is doing: a new code, or a code where there was
        // none. Bambu's HMS text and Klipper's own message both arrive here as error_text.
        //
        // Once per occurrence: a code that is already held - still up, or back within
        // ERROR_CLEAR_MS of going missing - is the same occurrence, however many times the printer
        // re-sends it or another code takes the top spot in between.
        if (error_is_new) {
            // error_text already carries the code when the text is unknown (describe_error), so the
            // bare-code spelling is only reached by a source that supplies neither - a relayed hub
            // or a Klipper printer that named a code and said nothing about it.
            Event e   = make_event(cur, "error", "error", name + " reported an error",
                                   cur.error_text.empty() ? (name + " reported error " + HMSQuery::pretty_code(cur.error_code) + ".")
                                                          : (name + ": " + cur.error_text));
            e.code    = cur.error_code;
            e.actions = cur.error_actions;
            e.job_id  = cur.job_id;
            out.push_back(e);
        }

        if (cur.state != prev.state) {
            if (cur.state == "printing" && prev.state == "paused") {
                out.push_back(make_event(cur, "resumed", "info", name + " resumed", name + " picked the print up again" + job_phrase(cur) + "."));
            } else if (cur.state == "printing") {
                if (start_is_new(jm, cur.job, cur.job_id, now.at))
                    out.push_back(started_event(cur, name));
            } else if (cur.state == "paused") {
                // Stage 6 is the printer's own "Paused due to filament runout"; it is the one pause
                // worth waking somebody for, so it gets its own kind.
                const bool runout = cur.stage_curr == 6;
                Event      e      = make_event(cur, runout ? "runout" : "paused", "warning",
                                               runout ? (name + " ran out of filament") : (name + " paused"),
                                               runout ? (name + " paused because it ran out of filament" + job_phrase(cur) + ".")
                                                      : (name + " paused" + (cur.stage.empty() ? std::string() : " (" + cur.stage + ")") + job_phrase(cur) + "."));
                out.push_back(e);
            } else if (cur.state == "finished" && busy_state(prev.state)) {
                out.push_back(make_event(cur, "finished", "info", name + " finished", name + " finished the print" + job_phrase(prev) + "."));
            } else if (cur.state == "failed") {
                // A failure with a code and no text used to read "X stopped with a failure." and
                // give the owner nothing to act on, so the code is named when that is all there is.
                const std::string why = !cur.error_text.empty() ? ": " + cur.error_text
                                        : !cur.error_code.empty() ? " (error " + HMSQuery::pretty_code(cur.error_code) + ")."
                                                                  : ".";
                Event e = make_event(cur, "failed", "error", name + " failed",
                                     name + " stopped with a failure" + job_phrase(prev) + why);
                e.code  = cur.error_code;
                out.push_back(e);
            } else if (cur.state == "cancelled" && busy_state(prev.state)) {
                out.push_back(make_event(cur, "cancelled", "warning", name + " was stopped", "The print on " + name + " was cancelled" + job_phrase(prev) + "."));
            }
        } else if (cur.state == "printing" && !cur.job.empty() && cur.job != prev.job) {
            // Straight from one job into the next without passing through an idle state. Only when
            // it really is another job: two spellings of the same file are one print (job_key), and
            // a job already announced and not ended is not announced again.
            if (start_is_new(jm, cur.job, cur.job_id, now.at))
                out.push_back(started_event(cur, name));
        }

        // A terminal state closes the current job: the next "printing" for the same name is a new
        // print and gets its own start. Recorded from the state, not from whether the event above
        // survived the cooldown - the memory is about what the printer did, not what was sent.
        if (terminal_state(cur.state) && cur.state != prev.state) {
            jm.terminal    = cur.state;
            jm.terminal_at = now.at;
            jm.announced   = false;
        }
    }

    // A printer that fails usually sets its error code in the same breath, and the failure event
    // already carries that code and its text. One thing that happened is one notification, so the
    // bare `error` is dropped where a `failed` for the same printer came out of the same poll.
    std::vector<Event> merged;
    for (const Event& e : out) {
        if (e.kind == "error") {
            bool failed_too = false;
            for (const Event& f : out)
                if (f.kind == "failed" && f.printer_id == e.printer_id) failed_too = true;
            if (failed_too) continue;
        }
        merged.push_back(e);
    }
    out.swap(merged);

    // The cooldown, last: an event that is really the same thing as one just sent is dropped, and
    // its key is not refreshed, so a genuinely long-running condition reappears once the window
    // has passed rather than never.
    std::vector<Event> kept;
    for (const Event& e : out) {
        const std::string key = cooldown_key(e);
        auto              it  = mem.last_emit.find(key);
        if (it != mem.last_emit.end() && now.at - it->second < cooldown_ms) continue;
        mem.last_emit[key] = now.at;
        kept.push_back(e);
    }
    // Keys of printers that are gone would otherwise accumulate for the life of the process.
    if (mem.last_emit.size() > 256) mem.last_emit.clear();
    for (auto it = mem.seen_at.begin(); it != mem.seen_at.end();)
        if (now.printers.count(it->first))
            ++it;
        else
            it = mem.seen_at.erase(it);
    // A printer that has left the snapshot entirely (removed from the Device tab, gone from
    // discovery for good) is the one case where the job memory is dropped - a printer that is
    // merely offline keeps its entry, which is the whole point of it.
    for (auto it = mem.jobs.begin(); it != mem.jobs.end();)
        it = now.printers.count(it->first) ? std::next(it) : mem.jobs.erase(it);
    for (auto it = mem.last_raw.begin(); it != mem.last_raw.end();)
        it = now.printers.count(it->first) ? std::next(it) : mem.last_raw.erase(it);
    for (auto it = mem.codes.begin(); it != mem.codes.end();)
        it = now.printers.count(it->first) ? std::next(it) : mem.codes.erase(it);
    mem.last = now;
    return kept;
}

// ------------------------------------------------------- snapshot: JSON ----

// Both directions of PrinterState <-> JSON, so the debug route can drive `step` with snapshots a
// test wrote by hand and read back what came out.
static PrinterState state_of_json(const json& j)
{
    PrinterState p;
    p.id         = j.value("id", std::string());
    p.name       = j.value("name", std::string());
    p.kind       = j.value("kind", std::string("bambu"));
    p.watched    = j.value("watched", true);
    p.online     = j.value("online", true);
    p.state      = j.value("state", std::string());
    p.raw_state  = j.value("raw_state", std::string());
    p.job        = j.value("job", std::string());
    p.stage      = j.value("stage", std::string());
    p.stage_curr = j.value("stage_curr", -1);
    p.error_code = j.value("error_code", std::string());
    p.error_text = j.value("error_text", std::string());
    p.job_id     = j.value("job_id", std::string());
    for (const json& c : j.value("active_codes", json::array()))
        if (c.is_string()) p.active_codes.push_back(c.get<std::string>());
    // A snapshot that names a code but no text gets the printer's own sentence, filled in exactly
    // as snapshot_bambu fills it: `id` is the serial and its first three characters pick the HMS
    // table. That is what makes this route a check on the per-device lookup and not only on the
    // transition rule - POST a 31B printer with 05004046 and the event text is the H2C's own
    // sentence, with no printer anywhere near it.
    if (p.error_text.empty() && !p.error_code.empty() && p.kind == "bambu") {
        if (HMSQuery* q = wxGetApp().get_hms_query()) p.error_text = q->describe_error(p.id, p.error_code).ToUTF8().data();
    }
    return p;
}

json replay(const json& in)
{
    Memory          mem;
    const long long cooldown = in.value("cooldown_ms", (long long) 180000);
    json            out;
    out["steps"] = json::array();
    if (!in.contains("snapshots") || !in["snapshots"].is_array()) {
        out["error"] = "snapshots must be an array";
        return out;
    }
    for (const json& s : in["snapshots"]) {
        Snapshot snap;
        snap.at = s.value("at", (long long) 0);
        for (const json& p : s.value("printers", json::array())) {
            PrinterState ps = state_of_json(p);
            if (!ps.id.empty()) snap.printers[ps.id] = ps;
        }
        json step_out;
        step_out["at"]     = snap.at;
        step_out["events"] = json::array();
        std::vector<RawChange> raw;
        for (const Event& e : step(mem, snap, cooldown, &raw)) step_out["events"].push_back(e.to_json(0));
        step_out["raw_changes"] = json::array();
        for (const RawChange& rc : raw)
            step_out["raw_changes"].push_back(json { { "printer", rc.printer_id },
                                                     { "from", rc.from },
                                                     { "to", rc.to },
                                                     { "was_visible", rc.was_visible },
                                                     { "visible", rc.visible } });
        out["steps"].push_back(step_out);
    }
    return out;
}

// ------------------------------------------------- snapshot: the printers ----

// Bambu's print_status, in the words the rule uses.
static std::string bambu_state(const std::string& s)
{
    if (s == "RUNNING") return "printing";
    if (s == "PAUSE") return "paused";
    if (s == "FINISH") return "finished";
    if (s == "FAILED") return "failed";
    if (s == "SLICING" || s == "PREPARE") return "preparing";
    if (s.empty()) return "";
    return "idle"; // IDLE, INIT and anything the printer invents later
}

// Klipper's print_stats.state, likewise.
static std::string klipper_state(const std::string& s)
{
    if (s == "printing") return "printing";
    if (s == "paused") return "paused";
    if (s == "complete") return "finished";
    if (s == "error") return "failed";
    if (s == "cancelled") return "cancelled";
    if (s == "standby") return "idle";
    if (s.empty()) return "";
    return "idle";
}

// The serial picks the table: hms_<lang>_31B.json for an H2C, hms_<lang>_094.json for an H2D,
// the legacy one for an X1 or a P1. Without it the newer machines had no sentence at all and the
// notification was left with the bare code.
// Always a sentence: HMSQuery::describe_print_error spells the code out when the table has no
// text for it, so error_text is never empty and no consumer downstream has to invent a fallback.
static std::string print_error_message(const std::string& dev_id, int code)
{
    if (HMSQuery* q = wxGetApp().get_hms_query()) return q->describe_print_error(dev_id, code).ToUTF8().data();
    return std::string();
}

// The buttons that go with that sentence, in the payload's own flat shape. The lookup, the
// resolver and the remote-safe rule are all shared with the status JSON and the control route -
// this only copies the result into the struct this header can carry.
static std::vector<PrintErrorEventAction> print_error_event_actions(const std::string& dev_id, int print_error,
                                                                    const std::string& job_id, bool has_action_json)
{
    std::vector<PrintErrorEventAction> out;
    for (const PrintErrorRemoteAction& a :
         describe_print_error_actions(RemoteControl::resolved_print_error_actions(dev_id, print_error), !job_id.empty(),
                                      has_action_json)) {
        PrintErrorEventAction e;
        e.id                = a.id;
        e.verb              = a.verb;
        e.label             = a.label;
        e.needs_job_id      = a.needs_job_id;
        e.needs_action_json = a.needs_action_json;
        e.remote_safe       = a.remote_safe;
        out.push_back(e);
    }
    return out;
}

// GUI thread: reading a MachineObject is field access, no network and no locks - which is why the
// Bambu half of the snapshot is taken on the heartbeat itself.
static void snapshot_bambu(Snapshot& s)
{
    DeviceManager* dm = wxGetApp().getDeviceManager();
    if (!dm) return;
    std::map<std::string, MachineObject*> all = dm->get_my_machine_list();
    for (const auto& kv : dm->get_local_machine_list()) all.insert(kv);
    for (const auto& kv : all) {
        MachineObject* m = kv.second;
        if (!m) continue;
        PrinterState p;
        p.id   = m->dev_id;
        p.name = m->dev_name;
        p.kind = "bambu";
        // In LAN mode exactly one printer is connected at a time, and that is the SDK's limit, not
        // a choice: bambu_network_connect_printer and bambu_network_disconnect_printer are scoped
        // to the agent (the disconnect takes no dev_id at all), so one agent holds one LAN session.
        // The others are a discovery entry and nothing else: their print_status is stale or empty,
        // so they are listed here but never watched.
        //
        // That is why a Bambu printer used to report nothing until the owner opened the slicer and
        // went to the Device page: the hidden hub-managed instance selected no machine, so it held
        // no session and every Bambu printer was listed unwatched. It now rotates the one session
        // over its LAN printers (DeviceManager::lan_watch_rotate), so each is watched in turn.
        p.watched   = m->is_connected();
        p.online    = m->is_online();
        p.raw_state = m->print_status;
        p.state     = bambu_state(m->print_status);
        p.job       = m->subtask_name;
        p.stage_curr = m->stage_curr;
        p.job_id     = m->job_id_;
        try {
            p.stage = m->get_curr_stage().ToUTF8().data();
        } catch (...) {}
        if (m->print_error != 0) {
            p.error_code = hex8(m->print_error);
            p.error_text = print_error_message(m->dev_id, m->print_error);
            // The buttons for this code. Same lookup and same resolver as the desktop dialog and
            // the status JSON (RemoteControl::describe_bambu), reached through the one function
            // that knows how, so a notification cannot offer a different set from the page the
            // tap-through lands on.
            p.error_actions = print_error_event_actions(m->dev_id, m->print_error, m->job_id_,
                                                        m->has_remote_command_error_action_json());
        } else {
            // No print error: the worst thing HMS is reporting, if it is serious enough to be worth
            // a notification. HMS_COMMON and HMS_INFO are the printer's chatter and stay off.
            for (HMSItem& item : m->hms_list) {
                if (item.msg_level != HMS_FATAL && item.msg_level != HMS_SERIOUS) continue;
                p.error_code = item.get_long_error_code();
                if (HMSQuery* q = wxGetApp().get_hms_query())
                    p.error_text = q->describe_error(m->dev_id, p.error_code).ToUTF8().data();
                break;
            }
        }
        // Every serious code that is up, the print error's included, so one that another code
        // is sitting on top of stays held instead of being announced again when it resurfaces.
        if (!p.error_code.empty()) p.active_codes.push_back(p.error_code);
        for (HMSItem& item : m->hms_list) {
            if (item.msg_level != HMS_FATAL && item.msg_level != HMS_SERIOUS) continue;
            const std::string c = item.get_long_error_code();
            if (std::find(p.active_codes.begin(), p.active_codes.end(), c) == p.active_codes.end()) p.active_codes.push_back(c);
        }
        s.printers[p.id] = p;
    }
}

// Worker thread: the LAN Snapmakers, over the Moonraker HTTP API they serve themselves. This is
// the same cached probe /api/printers uses (four-second TTL, a retry every ten seconds once a
// printer is offline, and SnapmakerLan::Presence deciding when that is), so a phone polling the
// Devices tab and the watcher share the answer instead of asking twice.
//
// Side by side, not one after the other. Asked in turn, one printer that was switched off added its
// whole connect timeout to every poll - with three Snapmakers on the LAN and one of them off, the
// five-second cadence measured thirteen, and a printer that started a job in that gap was seen for
// the first time already printing, so its start was seeded away instead of announced. Now each
// device gets its own thread and the poll waits PROBE_BUDGET_MS for all of them together.
//
// `slow` collects what to say about the printers that took their time, for the poll's debug line.
static void snapshot_snapmaker(Snapshot& s, std::string& slow)
{
    std::vector<SnapmakerLan::Device> list;
    try {
        list = SnapmakerLan::devices();
    } catch (...) {
        return;
    }
    if (list.empty()) return;

    // One slot per device, filled by its own thread. Detached rather than a future: a device that
    // outruns the budget must not hold this poll open in a destructor - its probe lands in
    // SnapmakerLan's cache, which is where the next poll finds it.
    struct Probes
    {
        std::mutex                        m;
        std::condition_variable           cv;
        std::vector<SnapmakerLan::Status> st;
        std::vector<bool>                 done;
        std::vector<long long>            took;
        size_t                            left { 0 };
    };
    auto probes = std::make_shared<Probes>();
    probes->st.resize(list.size());
    probes->done.assign(list.size(), false);
    probes->took.assign(list.size(), 0);
    probes->left = list.size();
    for (size_t i = 0; i < list.size(); ++i) {
        const SnapmakerLan::Device d = list[i];
        std::thread([probes, d, i]() {
            const long long        at = now_ms();
            SnapmakerLan::Status   st;
            try {
                st = SnapmakerLan::status(d);
            } catch (...) {}
            std::lock_guard<std::mutex> lock(probes->m);
            probes->st[i]   = st;
            probes->done[i] = true;
            probes->took[i] = now_ms() - at;
            if (probes->left > 0) --probes->left;
            probes->cv.notify_all();
        }).detach();
    }

    std::vector<SnapmakerLan::Status> answers;
    std::vector<bool>                 done;
    std::vector<long long>            took;
    {
        std::unique_lock<std::mutex> lock(probes->m);
        probes->cv.wait_for(lock, std::chrono::milliseconds(PROBE_BUDGET_MS), [&probes]() { return probes->left == 0; });
        answers = probes->st;
        done    = probes->done;
        took    = probes->took;
    }

    std::string note;
    auto        say = [&note](const std::string& what) { note += (note.empty() ? "" : ", ") + what; };
    for (size_t i = 0; i < list.size(); ++i) {
        const SnapmakerLan::Device& d  = list[i];
        SnapmakerLan::Status        st = answers[i];
        if (!done[i]) {
            // Still out. This poll's answer is whatever the cache holds; a printer nobody has ever
            // reached has none, and offline is exactly what the rule must be told in that case.
            say(d.id + " over budget");
            SnapmakerLan::Status cached;
            st = SnapmakerLan::cached_status(d, cached) ? cached : SnapmakerLan::Status();
        } else if (took[i] > SLOW_PROBE_MS) {
            say(d.id + " " + std::to_string(took[i]) + " ms" + (st.online ? "" : " (no answer)"));
        }
        PrinterState p;
        p.id        = "sm:" + d.id;
        p.name      = d.name.empty() ? d.ip : d.name;
        p.kind      = "snapmaker";
        p.online    = st.online;
        // A printer that wants a login answers nothing useful; do not pretend to watch it.
        p.watched   = st.online && !st.login_required;
        p.raw_state = st.state;
        p.state     = klipper_state(st.state);
        p.job       = st.filename;
        if (st.state == "error") {
            p.error_code = "error";
            p.error_text = st.message;
        }
        s.printers[p.id] = p;
    }
    if (!note.empty()) slow += (slow.empty() ? "" : "; ") + note;
}

// Worker thread: the printer preset's print host and the Snapmaker connected on the PC's Device
// tab, through the probe /api/printers already runs against them (RemoteControl::describe_hosts,
// which caches and backs off for half a minute on an address that is not a Moonraker printer).
static void snapshot_hosts(Snapshot& s, const std::vector<RemoteControl::HostTarget>& targets_in)
{
    if (targets_in.empty()) return;
    // The Snapmaker the PC's Device tab is connected to is usually the same machine as one of the
    // LAN cards above (merge_app_devices puts it in that list). One machine must produce one event,
    // so an address the LAN half already covered is dropped here rather than watched twice.
    std::vector<RemoteControl::HostTarget> targets;
    std::vector<std::string>               lan;
    try {
        for (const SnapmakerLan::Device& d : SnapmakerLan::devices()) lan.push_back(SnapmakerLan::base_url(d));
    } catch (...) {}
    for (const RemoteControl::HostTarget& t : targets_in)
        if (std::find(lan.begin(), lan.end(), t.base) == lan.end()) targets.push_back(t);
    if (targets.empty()) return;
    json printers = json::array();
    for (const RemoteControl::HostTarget& t : targets) printers.push_back(json { { "id", t.id } });
    try {
        RemoteControl::describe_hosts(targets, printers);
    } catch (...) {
        return;
    }
    for (size_t i = 0; i < targets.size() && i < printers.size(); ++i) {
        const json&  e = printers[i];
        PrinterState p;
        p.id   = targets[i].id;
        p.name = targets[i].base.empty() ? targets[i].id : targets[i].base;
        p.kind = targets[i].id == "connect" ? "connect" : "printhost";
        p.raw_state = e.value("print_status", std::string());
        p.state     = klipper_state(p.raw_state);
        p.job       = e.value("stage", std::string()); // fill_from_print_stats puts the file name there
        // describe_hosts leaves print_status off entirely for an address that did not answer as a
        // Moonraker printer, and that is exactly the case where nothing may be inferred.
        p.online    = e.contains("print_status");
        p.watched   = p.online;
        if (e.contains("print_error") && e["print_error"].is_object()) {
            p.error_code = e["print_error"].value("code", std::string());
            p.error_text = e["print_error"].value("message", std::string());
        }
        s.printers[p.id] = p;
    }
}

// --------------------------------------------------------- the watcher ----

static std::mutex       s_mutex;
static Memory           s_memory;          // worker thread only, guarded for the debug path
static std::deque<json> s_recent;          // this instance's own ring, for GET /api/events
static int              s_next_local = 1;
static std::atomic<bool> s_busy { false }; // one poll in flight at a time
static std::atomic<bool> s_stop { false };
static long long        s_last_poll = 0;   // GUI thread: when the last poll was started
// Guarded by s_mutex, for GET /api/events: when the last poll finished (the snapshot's own time)
// and how long it took. Zero until the watcher has completed one.
static long long        s_last_done = 0;
static long long        s_last_took = 0;

static void remember(const json& e)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_recent.push_back(e);
    while (s_recent.size() > 50) s_recent.pop_front();
}

json recent(int since)
{
    json out;
    out["events"] = json::array();
    int last      = 0;
    std::lock_guard<std::mutex> lock(s_mutex);
    for (const json& e : s_recent) {
        const int id = e.value("local_id", 0);
        last         = std::max(last, id);
        if (id > since) out["events"].push_back(e);
    }
    out["last_id"] = last;
    // What the live watcher has actually seen. `last_poll` is the snapshot time of the last
    // completed poll and `seen_at` the poll that seeded each printer, so a caller can tell the
    // difference between "the watcher does not know this printer yet" (nothing it does will be
    // reported - the first sight only seeds) and "it is being watched" (the next change is an
    // event). No addresses and no names: this answer is proxied to the phone.
    json w;
    w["poll_ms"]      = (long long) POLL_MS;
    w["last_poll"]    = s_last_done;
    w["last_poll_ms"] = s_last_took;
    w["printers"]     = json::array();
    for (const auto& kv : s_memory.last.printers) {
        const PrinterState& p = kv.second;
        json                j;
        j["id"]      = p.id;
        j["kind"]    = p.kind;
        j["online"]  = p.online;
        j["watched"] = p.watched;
        j["state"]   = p.state;
        auto it      = s_memory.seen_at.find(kv.first);
        if (it != s_memory.seen_at.end()) j["seen_at"] = it->second;
        w["printers"].push_back(j);
    }
    out["watcher"] = w;
    return out;
}

void stop() { s_stop = true; }

void heartbeat()
{
    if (s_stop || s_busy.load()) return;
    const long long at = now_ms();
    if (s_last_poll != 0 && at - s_last_poll < POLL_MS) return;
    s_last_poll = at;

    // The Bambu half here and now: this is the GUI thread, where MachineObject lives. The preset's
    // print host is read here too - the preset bundle is GUI-thread state as well.
    auto snap    = std::make_shared<Snapshot>();
    auto targets = std::make_shared<std::vector<RemoteControl::HostTarget>>();
    snap->at     = at;
    try {
        snapshot_bambu(*snap);
        RemoteControl::list_host_targets(*targets);
    } catch (...) {}

    // Everything that needs the network, the diff and the POST to the hub go to a worker: none of
    // it may run on the GUI thread, and the whole point of the watcher is that nobody waits on it.
    s_busy = true;
    std::thread([snap, targets]() {
        const long long began = now_ms();
        long long       lan_ms = 0, hosts_ms = 0;
        std::string     slow;
        try {
            const long long a = now_ms();
            snapshot_snapmaker(*snap, slow);
            const long long b = now_ms();
            snapshot_hosts(*snap, *targets);
            const long long c = now_ms();
            lan_ms            = b - a;
            hosts_ms          = c - b;
            std::vector<Event> events;
            std::vector<RawChange> raw;
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                events      = step(s_memory, *snap, 180000, &raw);
                s_last_done = snap->at;
                s_last_took = now_ms() - began;
            }
            // Every raw-state change, with the poll's own timestamp. This is the line that names a
            // flap source: a printer that walks RUNNING -> <none> -> RUNNING every few polls, or a
            // Klipper that reports "standby" between layers, shows up here as a run of changes with
            // nothing between them, and the "not visible" marks say whether the watcher lost sight
            // of it (a dropped LAN session, a login timeout) or the printer really said something
            // different.
            for (const RawChange& rc : raw)
                BOOST_LOG_TRIVIAL(info) << "RemoteEvents: raw-state at=" << rc.at << " " << rc.printer_id << ": " << rc.from
                                        << " -> " << rc.to << (rc.was_visible ? "" : " (was not visible)")
                                        << (rc.visible ? "" : " (not visible)");
            const long pid = (long) wxGetProcessId();
            for (const Event& e : events) {
                // What goes to the hub is the event without id and time: the hub assigns both, so
                // ids stay in one increasing sequence however many instances are running.
                const std::string body = e.to_json(pid).dump();
                json              mine = e.to_json(pid);
                {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    mine["local_id"] = s_next_local++;
                }
                mine["time"] = snap->at;
                remember(mine);
                BOOST_LOG_TRIVIAL(info) << "RemoteEvents: " << e.kind << " on " << e.printer_id << ": " << e.title;
                // Fire and forget: with no hub running this is a failed connect to a closed port
                // and the event stays in this instance's own ring. Nothing waits on delivery.
                RemoteHub::post_event(body);
            }
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(debug) << "RemoteEvents: poll failed: " << ex.what();
        } catch (...) {}
        // The cadence, in the log: the poll is meant to cost a fraction of POLL_MS, and when it
        // does not this line says which printer took the time.
        BOOST_LOG_TRIVIAL(debug) << "RemoteEvents: poll took " << (now_ms() - began) << " ms (" << snap->printers.size()
                                 << " printers; lan " << lan_ms << " ms, hosts " << hosts_ms << " ms)"
                                 << (slow.empty() ? std::string() : "; slow: " + slow);
        s_busy = false;
    }).detach();
}

} // namespace RemoteEvents
} // namespace GUI
} // namespace Slic3r
