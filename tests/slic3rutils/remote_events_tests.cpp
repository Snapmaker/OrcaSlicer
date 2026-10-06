// The printer event watcher's transition rule (RemoteEvents::step).
//
// The rule is pure: previous memory plus one snapshot gives the events, and `now.at` is the only
// clock it sees. So a whole print - including the flaps that used to make it announce itself three
// times - can be driven through it here with no printer, no network and no hub.
//
// What these cases are about: a repeat "started" was arriving several times, long after the real
// start. Two causes were named. The transition rule fired on every prev != printing -> printing
// flip with no memory of the job, so a printer that blipped through idle / unknown / offline
// re-announced the print it was already on; and the job-change rule fired whenever the job name
// changed spelling. Both are covered below, alongside the cases that must still announce.

#include <catch2/catch.hpp>

#include "slic3r/GUI/LanReconnectLadder.hpp"
#include "slic3r/GUI/PrintErrorCommands.hpp"
#include "slic3r/GUI/RemoteEvents.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::RemoteEvents;

namespace {

// One printer, watched and online, in one state. The defaults are the normal case; a test that
// wants a flap turns one of them off.
PrinterState pr(const std::string& state, const std::string& job = "bench.gcode", const char* raw = nullptr)
{
    PrinterState p;
    p.id        = "P1";
    p.name      = "X1C";
    p.kind      = "bambu";
    p.watched   = true;
    p.online    = true;
    p.state     = state;
    p.raw_state = raw ? raw : state;
    p.job       = job;
    return p;
}

PrinterState offline(const PrinterState& in)
{
    PrinterState p = in;
    p.online       = false;
    p.state.clear();
    p.raw_state.clear();
    return p;
}

PrinterState unwatched(const PrinterState& in)
{
    PrinterState p = in;
    p.watched      = false;
    p.state.clear();
    p.raw_state.clear();
    return p;
}

// One poll. `at` advances well past the three-minute cooldown by default so that what the cases
// measure is the job memory and not the cooldown - the cooldown alone was never the fix.
struct Driver
{
    Memory    mem;
    long long at { 1000000 };

    std::vector<Event> poll(const PrinterState& p, long long advance_ms = 300000)
    {
        at += advance_ms;
        Snapshot s;
        s.at             = at;
        s.printers[p.id] = p;
        return step(mem, s, 180000);
    }
    std::vector<Event> poll(const PrinterState& p, std::vector<RawChange>& raw, long long advance_ms = 300000)
    {
        at += advance_ms;
        Snapshot s;
        s.at             = at;
        s.printers[p.id] = p;
        raw.clear();
        return step(mem, s, 180000, &raw);
    }
};

int count_of(const std::vector<Event>& v, const char* kind)
{
    int n = 0;
    for (const Event& e : v)
        if (e.kind == kind) ++n;
    return n;
}

} // namespace

TEST_CASE("[RemoteEvents] a print announces its start exactly once", "[RemoteEvents]")
{
    Driver d;
    REQUIRE(d.poll(pr("idle", "")).empty()); // seeding poll
    std::vector<Event> ev = d.poll(pr("printing"));
    REQUIRE(count_of(ev, "started") == 1);
    // Still printing, poll after poll: nothing more.
    for (int i = 0; i < 5; ++i) REQUIRE(d.poll(pr("printing")).empty());
}

TEST_CASE("[RemoteEvents] an idle blip mid-print does not re-announce the start", "[RemoteEvents]")
{
    Driver d;
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
    // The flap: a raw state the mapper reads as idle, then back to printing. This is the Klipper /
    // Snapmaker / Elegoo raw-state mapping case and it used to announce a second start.
    d.poll(pr("idle", "bench.gcode", "standby"));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 0);
    // And again, several times over - the cooldown would have expired between each of these.
    for (int i = 0; i < 3; ++i) {
        d.poll(pr("idle", "bench.gcode", "standby"));
        REQUIRE(count_of(d.poll(pr("printing")), "started") == 0);
    }
}

TEST_CASE("[RemoteEvents] going offline and coming back mid-print does not re-announce", "[RemoteEvents]")
{
    Driver d;
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
    // A brief offline reset: the watcher loses sight of the printer, then sees it printing again.
    // The first watched poll only seeds, the next one used to see prev = "" -> "printing".
    d.poll(offline(pr("printing")));
    d.poll(pr("printing")); // re-seed
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 0);
}

TEST_CASE("[RemoteEvents] an unwatched gap mid-print does not re-announce", "[RemoteEvents]")
{
    Driver d;
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
    // The LAN case: the session moved to another printer, so this one is listed but not watched.
    for (int i = 0; i < 3; ++i) d.poll(unwatched(pr("printing")));
    d.poll(pr("printing")); // re-seed on its turn coming round
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 0);
}

TEST_CASE("[RemoteEvents] two spellings of one file are one print", "[RemoteEvents]")
{
    Driver d;
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing", "bench.gcode.3mf")), "started") == 1);
    // Every spelling the same job has been seen wearing: a leading path, the extension dropped,
    // a different case, trailing space. None of them is a new print.
    REQUIRE(count_of(d.poll(pr("printing", "/cache/bench.gcode.3mf")), "started") == 0);
    REQUIRE(count_of(d.poll(pr("printing", "bench")), "started") == 0);
    REQUIRE(count_of(d.poll(pr("printing", "BENCH.GCODE")), "started") == 0);
    REQUIRE(count_of(d.poll(pr("printing", "bench.3mf ")), "started") == 0);
}

TEST_CASE("[RemoteEvents] a genuinely different job does announce", "[RemoteEvents]")
{
    Driver d;
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing", "bench.gcode")), "started") == 1);
    REQUIRE(count_of(d.poll(pr("printing", "calibration_cube.gcode")), "started") == 1);
}

TEST_CASE("[RemoteEvents] the same file printed again after it finished announces again", "[RemoteEvents]")
{
    Driver d;
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
    REQUIRE(count_of(d.poll(pr("finished")), "finished") == 1);
    d.poll(pr("idle", ""));
    // A second run of the same file is a second print and must be announced.
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
}

TEST_CASE("[RemoteEvents] a cancelled print reopens the job, a paused one does not", "[RemoteEvents]")
{
    SECTION("cancelled")
    {
        Driver d;
        d.poll(pr("idle", ""));
        REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
        REQUIRE(count_of(d.poll(pr("cancelled")), "cancelled") == 1);
        d.poll(pr("idle", ""));
        REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
    }
    SECTION("paused and resumed")
    {
        Driver d;
        d.poll(pr("idle", ""));
        REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
        REQUIRE(count_of(d.poll(pr("paused")), "paused") == 1);
        std::vector<Event> back = d.poll(pr("printing"));
        // Resuming is a "resumed", never a second "started".
        REQUIRE(count_of(back, "resumed") == 1);
        REQUIRE(count_of(back, "started") == 0);
    }
}

TEST_CASE("[RemoteEvents] a failure reopens the job", "[RemoteEvents]")
{
    Driver d;
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
    REQUIRE(count_of(d.poll(pr("failed")), "failed") == 1);
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
}

TEST_CASE("[RemoteEvents] the watcher joining a print already running stays silent", "[RemoteEvents]")
{
    Driver d;
    // First sight is already printing: the seeding poll, and nothing may be announced for it.
    REQUIRE(d.poll(pr("printing")).empty());
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 0);
    // Even after it flaps.
    d.poll(offline(pr("printing")));
    d.poll(pr("printing"));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 0);
}

TEST_CASE("[RemoteEvents] raw-state changes are reported for the log", "[RemoteEvents]")
{
    Driver                 d;
    std::vector<RawChange> raw;
    d.poll(pr("idle", "", "IDLE"), raw); // first sight: seeds, reports no change
    REQUIRE(raw.empty());
    d.poll(pr("printing", "bench.gcode", "RUNNING"), raw);
    REQUIRE(raw.size() == 1);
    CHECK(raw[0].from == "IDLE");
    CHECK(raw[0].to == "RUNNING");
    CHECK(raw[0].was_visible);
    CHECK(raw[0].visible);
    // Losing sight of the printer is itself a raw change, and marked as not visible, so the log
    // says whether the printer changed its mind or the watcher simply stopped being able to see it.
    d.poll(offline(pr("printing")), raw);
    REQUIRE(raw.size() == 1);
    CHECK(raw[0].to == "<none>");
    CHECK_FALSE(raw[0].visible);
}

TEST_CASE("[RemoteEvents] job memory survives a printer going offline but not its removal", "[RemoteEvents]")
{
    Driver d;
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
    d.poll(offline(pr("printing")));
    REQUIRE(d.mem.jobs.count("P1") == 1);
    // The printer leaves the snapshot entirely (removed from the Device tab): now the memory goes.
    Snapshot empty;
    empty.at = d.at += 300000;
    step(d.mem, empty, 180000);
    REQUIRE(d.mem.jobs.count("P1") == 0);
}

TEST_CASE("[RemoteEvents] two printers keep their own job memory", "[RemoteEvents]")
{
    Memory    mem;
    long long at   = 1000000;
    auto      poll = [&mem, &at](const PrinterState& a, const PrinterState& b) {
        at += 300000;
        Snapshot s;
        s.at             = at;
        s.printers[a.id] = a;
        s.printers[b.id] = b;
        return step(mem, s, 180000);
    };
    PrinterState p1 = pr("idle", "");
    PrinterState p2 = pr("idle", "");
    p2.id           = "P2";
    p2.name         = "P1S";
    poll(p1, p2);

    PrinterState p1p = pr("printing", "a.gcode");
    REQUIRE(count_of(poll(p1p, p2), "started") == 1);

    // P2 starting its own print is its own event; P1 flapping at the same time is still not.
    PrinterState p2p = pr("printing", "b.gcode");
    p2p.id           = "P2";
    p2p.name         = "P1S";
    std::vector<Event> ev = poll(pr("idle", "a.gcode", "standby"), p2p);
    REQUIRE(count_of(ev, "started") == 1);
    REQUIRE(ev[0].printer_id == "P2");
    REQUIRE(count_of(poll(p1p, p2p), "started") == 0);
}

// The ladder the LAN reconnect tick walks. It is the real function off DeviceManager, not a copy:
// the cadence a log is meant to show (15 s, 30 s, 60 s, 60 s...) is this and nothing else.
TEST_CASE("[RemoteEvents] the LAN reconnect backoff is 15s, 30s, 60s and then capped", "[RemoteEvents]")
{
    namespace L = Slic3r::LanReconnectLadder;
    CHECK(L::backoff_ms(1) == 15000);
    CHECK(L::backoff_ms(2) == 30000);
    CHECK(L::backoff_ms(3) == 60000);
    CHECK(L::backoff_ms(4) == 60000);
    CHECK(L::backoff_ms(50) == 60000);
    // The grace window before the first retry: a LAN session is "gone" once no push has landed for
    // DISCONNECT_TIMEOUT, and the tick waits that long again before touching it.
    CHECK(L::GRACE_MS == 15000);
    CHECK(L::MAX_MS == 60000);
}

// ---- the LAN reconnect tick's rule (LanReconnectLadder::lan_tick_step) ----
//
// The owner's Device tab dropped and came back every minute or so on Bambu printers. Half of it was
// this tick: it re-dialled any LAN session that had gone 30 s (+15 s grace) without a report, so a
// quiet printer was torn down, and when a second EdgeSlicer process took the session over (same
// MQTT ClientId, fixed in UltraNet) the silence made this process take it straight back. These
// cases drive the rule through simulated minutes, one tick a second, the way the GUI heartbeat does.
namespace {

namespace L = Slic3r::LanReconnectLadder;

// A simulated printer + plug-in, and the host tick over them. The printer pushes a report every
// `report_every_ms` unless `quiet(now)`; it answers a probe (pushall) after 400 ms while its
// session is up. A Reconnect resets the report clock like MachineObject::reset() does and leaves
// the session down until `dial_ok(now)` says a dial would get through.
struct Sim
{
    long long now = 0, last_report = 0, pending_answer = -1;
    bool      session_up = true;
    bool      answers    = true; // the printer answers a pushall
    long long report_every_ms = 1000;
    std::function<bool(long long)> quiet   = [](long long) { return false; };
    std::function<bool(long long)> away    = [](long long) { return false; }; // printer unreachable
    std::function<bool(long long)> dial_ok = [](long long) { return true; };
    L::LinkState st;
    std::vector<long long> probes, redials;
    long long max_silence = 0;

    void run_until(long long end)
    {
        for (; now < end; now += 1000) {
            if (away(now)) session_up = false;
            if (session_up && !quiet(now) && now % report_every_ms == 0) last_report = now;
            if (session_up && pending_answer >= 0 && now >= pending_answer) { last_report = now; pending_answer = -1; }
            max_silence = std::max(max_silence, now - last_report);
            switch (L::lan_tick_step(st, now, now - last_report, session_up)) {
            case L::TickAction::None: break;
            case L::TickAction::Probe:
                probes.push_back(now);
                if (session_up && answers) pending_answer = now + 400;
                break;
            case L::TickAction::Reconnect:
                redials.push_back(now);
                last_report = now; // MachineObject::reset()
                session_up = !away(now) && dial_ok(now);
                break;
            }
        }
    }
};

} // namespace

TEST_CASE("[RemoteEvents] a LAN session with steady reports is never touched for ten minutes", "[RemoteEvents]")
{
    Sim s;
    s.run_until(10 * 60 * 1000);
    CHECK(s.redials.empty());
    CHECK(s.probes.empty());
    CHECK(s.max_silence <= 1000);
}

TEST_CASE("[RemoteEvents] a quiet LAN printer is asked for a report, never re-dialled", "[RemoteEvents]")
{
    // 90 s without a periodic report in every two minutes, for ten minutes - longer than the old
    // 30 s + 15 s rule ever let a session live.
    Sim s;
    s.quiet = [](long long t) { return t % 120000 >= 30000; };
    s.run_until(10 * 60 * 1000);
    CHECK(s.redials.empty());
    CHECK(!s.probes.empty());
    // Each silence ends with the answer to a probe sent at 20 s: the Device tab (30 s rule) never
    // sees the printer as disconnected.
    CHECK(s.max_silence <= L::PROBE_AFTER_MS + 1000);
    CHECK(s.max_silence < 30000);
}

TEST_CASE("[RemoteEvents] a session the plug-in calls up but that stays mute is re-dialled at two minutes", "[RemoteEvents]")
{
    Sim s;
    // The last report lands at 59 s; after that nothing, and probes go unanswered (a half-open
    // socket) - and the plug-in never says the session is gone.
    s.quiet   = [](long long t) { return t >= 60000; };
    s.answers = false;
    s.run_until(190000);
    REQUIRE(s.redials.size() == 1);
    CHECK(s.redials[0] - 59000 == L::STALE_MS);
    // One probe per 10 s from 20 s of silence until the re-dial.
    CHECK(s.probes.size() == (size_t) ((L::STALE_MS - L::PROBE_AFTER_MS) / L::PROBE_EVERY_MS));
}

TEST_CASE("[RemoteEvents] a real LAN drop is re-dialled after the grace, then on the ladder, until the printer is back", "[RemoteEvents]")
{
    Sim s;
    // The printer leaves at 60 s (the plug-in reports the session lost) and is back at 400 s.
    s.away    = [](long long t) { return t >= 60000 && t < 400000; };
    s.run_until(10 * 60 * 1000);
    REQUIRE(s.redials.size() >= 4);
    // First re-dial: 20 s of silence after the last report (59 s) to notice, then the 15 s grace.
    CHECK(s.redials[0] == 59000 + L::PROBE_AFTER_MS + L::GRACE_MS);
    // Then 20 s (the reset report clock needs 20 s of silence again, which is longer than the
    // 15 s first rung), 30 s, 60 s, 60 s...
    CHECK(s.redials[1] - s.redials[0] == 20000);
    CHECK(s.redials[2] - s.redials[1] == 30000);
    CHECK(s.redials[3] - s.redials[2] == 60000);
    for (size_t i = 4; i < s.redials.size(); ++i) CHECK(s.redials[i] - s.redials[i - 1] == 60000);
    // No probe is wasted on a session the plug-in said is gone.
    CHECK(s.probes.empty());
    // Back at 400 s: the first re-dial after that gets through, and nothing happens afterwards.
    CHECK(s.redials.back() >= 400000);
    CHECK(s.redials.back() < 400000 + 60000 + 1000);
    CHECK(s.session_up);
    CHECK(s.st.attempts == 0);
    CHECK(s.now - s.last_report <= 1000);
}

TEST_CASE("[RemoteEvents] a report publish that finds no session counts as a drop", "[RemoteEvents]")
{
    // A plug-in that drops the session without telling the host (or the callback was lost): the
    // probe cannot be published, the tick is told session_up = false, and the ladder takes over.
    L::LinkState st;
    const long long t0 = 100000; // the host's clock is epoch milliseconds, never 0
    CHECK(L::lan_tick_step(st, t0, 25000, true) == L::TickAction::Probe);
    // The host saw rc != 0 from the publish and marked the session down: the grace window runs
    // from the first silent tick.
    CHECK(L::lan_tick_step(st, t0 + 1000, 26000, false) == L::TickAction::None);
    CHECK(L::lan_tick_step(st, t0 + L::GRACE_MS, 25000 + L::GRACE_MS, false) == L::TickAction::Reconnect);
    // A report from a live session afterwards clears everything.
    CHECK(L::lan_tick_step(st, t0 + L::GRACE_MS + 1000, 500, true) == L::TickAction::None);
    CHECK(st.attempts == 0);
    CHECK(st.down_since == 0);
}

// The notification body, shared by ntfy, Web Push and the native-app push plane.
//
// The error text now carries its own code (HMSQuery::format_error), so the old unconditional
// `body += " (" + code + ")"` printed it twice: "... (0C00 0100 0002 0015). (0C00010000020015)"
// on the lock screen. The code is still appended when the text does not name it, because an event
// from a relayed hub or a Klipper printer may carry a code and a sentence that never mentions it.
TEST_CASE("[RemoteEvents] a push body names the error code exactly once", "[RemoteEvents]")
{
    // Already named in the grouped spelling the text was built with: not repeated.
    CHECK(notification_body("X1C: Nozzle Camera is malfunctioning. (0C00 0100 0002 0015)", "0C00010000020015") ==
          "X1C: Nozzle Camera is malfunctioning. (0C00 0100 0002 0015)");

    // Already named in the raw spelling, as a relayed body may carry it: also not repeated.
    CHECK(notification_body("X1C reported error 0C00010000020015.", "0C00010000020015") ==
          "X1C reported error 0C00010000020015.");

    // Not named at all - the code is added, grouped, so the owner has something to look up.
    CHECK(notification_body("X1C: the bed is too cold", "05004046") == "X1C: the bed is too cold (0500 4046)");
    // A non-Bambu code is appended as it is; the notify gate looks for it verbatim.
    CHECK(notification_body("Printer error", "HMS_0300") == "Printer error (HMS_0300)");

    // No code: nothing is appended, and an empty body stays empty rather than becoming " ()".
    CHECK(notification_body("X1C finished the print", "") == "X1C finished the print");
    CHECK(notification_body("", "") == "");
}


// ---- the printer-error event's action buttons (phase 2) ----
//
// The app's notification used to carry a sentence and nothing else: "X1C: the toolhead camera is
// not working properly". Tapping it opened the app, which opened the page, which is where the
// buttons were. These cases pin the payload that lets the app draw them on the card itself - and,
// more importantly, pin that adding them changed nothing about the text, the title or the
// severity, because every existing consumer reads those.
TEST_CASE("[RemoteEvents] an error event carries the actions the app can offer", "[RemoteEvents]")
{
    PrinterState p = pr("printing");
    p.error_code   = "05008051";
    p.error_text   = "the filament is tangled (0500 8051)";
    p.job_id       = "42";
    // What RemoteEvents fills in from the printer: the same resolved set the status JSON carries.
    bool fallback = false;
    for (const PrintErrorRemoteAction& a :
         describe_print_error_actions(resolve_print_error_actions({23, 3}, fallback), true)) {
        PrintErrorEventAction e;
        e.id           = a.id;
        e.verb         = a.verb;
        e.label        = a.label;
        e.needs_job_id = a.needs_job_id;
        e.remote_safe  = a.remote_safe;
        p.error_actions.push_back(e);
    }

    Driver d;
    REQUIRE(d.poll(pr("printing")).empty()); // seeding poll, no error yet
    std::vector<Event> ev = d.poll(p);

    REQUIRE(count_of(ev, "error") == 1);
    const Event* e = nullptr;
    for (const Event& x : ev)
        if (x.kind == "error") e = &x;
    REQUIRE(e);

    SECTION("the actions reach the event, and the text is untouched by them")
    {
        REQUIRE(e->actions.size() == 2);
        REQUIRE(e->actions[0].verb == "idle_ignore_error");
        REQUIRE(e->actions[1].verb == "resume_error");
        REQUIRE(e->job_id == "42");
        // Unchanged: this is what every channel already renders.
        // The rule's own wording, unchanged: "<printer>: <what the printer said>".
        REQUIRE(e->text == "X1C: the filament is tangled (0500 8051)");
        REQUIRE(e->severity == "error");
        REQUIRE(e->code == "05008051");
    }

    SECTION("the JSON carries them as ids, verbs and labels")
    {
        const nlohmann::json j = e->to_json(0);
        REQUIRE(j["actions"].is_array());
        REQUIRE(j["actions"].size() == 2);
        REQUIRE(j["actions"][1]["id"] == PrintErrorAction::RESUME_PRINTING_DEFECTS);
        REQUIRE(j["actions"][1]["verb"] == "resume_error");
        REQUIRE(j["actions"][1]["needs_job_id"] == true);
        REQUIRE(j["actions"][1]["remote_safe"] == true);
        REQUIRE_FALSE(j["actions"][1]["label"].get<std::string>().empty());
        REQUIRE(j["job_id"] == "42");
        // And the fields that were there before are exactly as they were.
        REQUIRE(j["kind"] == "error");
        REQUIRE(j["code"] == "05008051");
        REQUIRE(j["text"] == "X1C: the filament is tangled (0500 8051)");
    }
}

TEST_CASE("[RemoteEvents] an error with no actions carries no actions field at all", "[RemoteEvents]")
{
    // A Klipper printer's error, an HMS item, a relayed hub's event: none of them has a button
    // set. The payload must then be byte-for-byte what a consumer written before this field saw,
    // rather than an empty array it has to learn to ignore.
    PrinterState p = pr("printing");
    p.error_code   = "05008051";
    p.error_text   = "something went wrong";

    Driver d;
    REQUIRE(d.poll(pr("printing")).empty());
    std::vector<Event> ev = d.poll(p);
    REQUIRE(count_of(ev, "error") == 1);

    for (const Event& e : ev) {
        if (e.kind != "error") continue;
        const nlohmann::json j = e.to_json(0);
        REQUIRE_FALSE(j.contains("actions"));
        REQUIRE_FALSE(j.contains("job_id"));
        REQUIRE(j["code"] == "05008051");
    }
}

// ---- one event per occurrence (2026-09-25: an HMS code three times, a start twice) ----
//
// The owner's history showed "AMS B slot 3 feed resistance is too high" three times in one minute
// and "Nozzle Camera is malfunctioning" three times. The watcher used to fire on every change of
// the printer's single worst code, so a printer that re-sent the item, dropped it for a status
// push, or let another code take the top spot, announced it again once the cooldown ran out.

namespace {
PrinterState with_code(const PrinterState& in, const std::string& code, std::vector<std::string> also = {})
{
    PrinterState p = in;
    p.error_code   = code;
    p.error_text   = code.empty() ? std::string() : "problem " + code;
    p.active_codes = also;
    if (!code.empty()) p.active_codes.insert(p.active_codes.begin(), code);
    return p;
}
} // namespace

TEST_CASE("[RemoteEvents] a persisting HMS code is announced once until it clears", "[RemoteEvents]")
{
    Driver d;
    REQUIRE(d.poll(pr("printing")).empty()); // seeding
    REQUIRE(count_of(d.poll(with_code(pr("printing"), "0701220000020025")), "error") == 1);
    // Re-sent on every status push, long past the three-minute cooldown: still the one occurrence.
    for (int i = 0; i < 5; ++i) REQUIRE(count_of(d.poll(with_code(pr("printing"), "0701220000020025")), "error") == 0);
    // Dropped for one status push (a few seconds) and back: the same occurrence.
    REQUIRE(d.poll(pr("printing"), 5000).empty());
    REQUIRE(count_of(d.poll(with_code(pr("printing"), "0701220000020025"), 5000), "error") == 0);
    // Gone for longer than ERROR_CLEAR_MS: it cleared, and coming back is a new occurrence.
    REQUIRE(d.poll(pr("printing"), 5000).empty());
    REQUIRE(d.poll(pr("printing"), ERROR_CLEAR_MS).empty());
    REQUIRE(count_of(d.poll(with_code(pr("printing"), "0701220000020025")), "error") == 1);
}

TEST_CASE("[RemoteEvents] two codes taking turns at the top are each announced once", "[RemoteEvents]")
{
    const std::string a = "0701220000020025", b = "0C00010000020015";
    Driver d;
    d.poll(pr("printing"));
    REQUIRE(count_of(d.poll(with_code(pr("printing"), a)), "error") == 1);
    REQUIRE(count_of(d.poll(with_code(pr("printing"), b, { a })), "error") == 1);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(count_of(d.poll(with_code(pr("printing"), a, { b })), "error") == 0);
        REQUIRE(count_of(d.poll(with_code(pr("printing"), b, { a })), "error") == 0);
    }
}

TEST_CASE("[RemoteEvents] a code already up when the watcher first sees the printer stays quiet", "[RemoteEvents]")
{
    Driver d;
    REQUIRE(d.poll(with_code(pr("printing"), "0C00010000020015")).empty()); // seeding, with the code up
    REQUIRE(d.poll(with_code(pr("printing"), "0C00010000020015")).empty());
    // Offline and back with the code still up: not a new occurrence either.
    d.poll(offline(pr("printing")));
    REQUIRE(d.poll(with_code(pr("printing"), "0C00010000020015")).empty());
    REQUIRE(d.poll(with_code(pr("printing"), "0C00010000020015")).empty());
}

TEST_CASE("[RemoteEvents] a start is keyed on the printer's job id when it has one", "[RemoteEvents]")
{
    auto job = [](const std::string& name, const std::string& id) {
        PrinterState p = pr("printing", name);
        p.job_id       = id;
        return p;
    };
    SECTION("the same job id under another name is the same print")
    {
        Driver d;
        d.poll(pr("idle", ""));
        std::vector<Event> ev = d.poll(job("0623_Golurk_x_Franky_MC", "4471"));
        REQUIRE(count_of(ev, "started") == 1);
        REQUIRE(ev[0].job_id == "4471");
        REQUIRE(ev[0].to_json(0)["job_id"] == "4471");
        REQUIRE(count_of(d.poll(job("Golurk plate 1", "4471")), "started") == 0);
    }
    SECTION("a new job id is a new print, even under the same name")
    {
        Driver d;
        d.poll(pr("idle", ""));
        REQUIRE(count_of(d.poll(job("Cube", "100")), "started") == 1);
        d.poll(pr("idle", "Cube", "IDLE"));
        REQUIRE(count_of(d.poll(job("Cube", "101")), "started") == 1);
    }
    SECTION("no job id on one side falls back to the name")
    {
        Driver d;
        d.poll(pr("idle", ""));
        REQUIRE(count_of(d.poll(job("Cube", "")), "started") == 1);
        d.poll(pr("idle", "Cube", "IDLE"));
        REQUIRE(count_of(d.poll(job("Cube", "0")), "started") == 0);
    }
}
