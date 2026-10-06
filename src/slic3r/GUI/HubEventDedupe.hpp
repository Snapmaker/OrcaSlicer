#pragma once

// The hub's rule for "is this printer event one it already has?" - pure, wx-free and clock-free,
// so the hub (RemoteHub::accept_event) and the unit tests run exactly the same code.
//
// Why the hub needs one at all: every slicer window on a data dir runs its own watcher
// (RemoteEvents) and each posts what it sees to the one hub. A print start seen by three windows
// arrives three times, an HMS code that one window's LAN session reports a minute after another's
// arrives twice, and so on. The watcher is edge-triggered per window; only the hub sees all of
// them, so only the hub can say "that is the same happening".
//
// The subject of an event is what makes this occurrence this one:
//   error / failed          the printer's error code (the text when a source has no code)
//   started and the others  the printer's job id when it reported one, else the job name
//                           normalised the way the watcher compares names (job_key)
//
// A candidate is a duplicate of a stored event when the printer and kind match, the subject
// matches, and:
//   started   no finished / failed / cancelled for that printer since the stored start, and the
//             stored start is less than START_MEMORY_MS old. One start per job, however many
//             windows saw it and however far apart they saw it.
//   error     the stored one is less than ERROR_WINDOW_MS old. The watcher itself holds a code
//             until it has cleared (RemoteEvents::step), so a second report inside this window can
//             only be another window seeing the same code.
//   others    the stored one is less than OTHER_WINDOW_MS old.
// ...and it came from ANOTHER instance (window). A window repeating itself is left alone: its
// watcher only reports edges, so a repeat from it is a real second occurrence (paused, resumed,
// paused again), and the synthetic events the gates post all come from one instance id.

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstring>
#include <deque>
#include <string>

namespace Slic3r {
namespace GUI {
namespace HubEventDedupe {

static constexpr long long START_MEMORY_MS = 12LL * 3600 * 1000; // a print longer than this is rare; the ring is 200 long anyway
static constexpr long long ERROR_WINDOW_MS = 30LL * 60 * 1000;
static constexpr long long OTHER_WINDOW_MS = 2LL * 60 * 1000;

inline std::string str_of(const nlohmann::json& j, const char* key)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_string()) return std::string();
    return j[key].get<std::string>();
}

inline long long int_of(const nlohmann::json& j, const char* key)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_number()) return 0;
    return j[key].get<long long>();
}

inline std::string printer_of(const nlohmann::json& e)
{
    if (!e.is_object() || !e.contains("printer") || !e["printer"].is_object()) return std::string();
    return str_of(e["printer"], "id");
}

// The same normalisation RemoteEvents uses to tell two spellings of one file apart: basename,
// case-folded, the usual G-code extensions and surrounding whitespace off.
inline std::string job_key(const std::string& job)
{
    std::string s = job;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
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
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '.')) s.pop_back();
    return s;
}

inline bool is_terminal(const std::string& kind) { return kind == "finished" || kind == "failed" || kind == "cancelled"; }

// What makes this occurrence this one (see the header comment).
inline std::string subject(const nlohmann::json& e)
{
    const std::string kind = str_of(e, "kind");
    if (kind == "error" || kind == "failed") {
        const std::string code = str_of(e, "code");
        if (!code.empty()) return "code:" + code;
        return "text:" + str_of(e, "text");
    }
    const std::string job_id = str_of(e, "job_id");
    if (!job_id.empty() && job_id != "0") return "jobid:" + job_id;
    return "job:" + job_key(str_of(e, "job"));
}

// Two subjects name the same thing. A job id and a job name cannot be compared with each other,
// so a start that one window reported with the printer's job id and another without it (an older
// slicer, a printer that has not said its job id yet) falls back to the names.
inline bool same_subject(const nlohmann::json& a, const nlohmann::json& b)
{
    const std::string sa = subject(a), sb = subject(b);
    if (sa == sb) return true;
    const std::string kind = str_of(a, "kind");
    if (kind == "error" || kind == "failed") return false;
    const bool ida = sa.compare(0, 6, "jobid:") == 0, idb = sb.compare(0, 6, "jobid:") == 0;
    if (ida == idb) return false; // both ids (different) or both names (different)
    return job_key(str_of(a, "job")) == job_key(str_of(b, "job"));
}

// The index in `ring` (oldest first) of the stored event `e` repeats, or -1. `now_ms` is the time
// the hub is about to stamp on `e`; the stored events carry their own "time".
inline int find_duplicate(const std::deque<nlohmann::json>& ring, const nlohmann::json& e, long long now_ms)
{
    const std::string printer = printer_of(e);
    if (printer.empty()) return -1; // a test event, a hub-level message: nothing to compare it to
    const std::string kind     = str_of(e, "kind");
    const long long   instance = int_of(e, "instance");
    const long long   horizon  = kind == "started" ? START_MEMORY_MS : kind == "error" ? ERROR_WINDOW_MS : OTHER_WINDOW_MS;
    for (int i = (int) ring.size() - 1; i >= 0; --i) {
        const nlohmann::json& o = ring[(size_t) i];
        if (now_ms - int_of(o, "time") > horizon) break; // the ring is in time order
        if (printer_of(o) != printer) continue;
        const std::string okind = str_of(o, "kind");
        // A start is closed by the job ending: anything older than that ending belongs to a
        // print that is over, and the same file printed again is a new print.
        if (kind == "started" && is_terminal(okind)) return -1;
        if (okind != kind) continue;
        if (!same_subject(o, e)) continue;
        if (int_of(o, "instance") == instance) continue; // the same window repeating itself: real
        return i;
    }
    return -1;
}

// The event's stable id: the hub's own id, qualified by the data dir's hub identity. The plain id
// restarts at 1 on a fresh data dir and two hubs on one PC count independently, so the id alone
// is not an identity; this is, and it never changes once the event is stored. The app keys its
// history on it.
inline std::string uid(const std::string& hub_instance, long long id)
{
    return (hub_instance.empty() ? std::string("hub") : hub_instance) + "-" + std::to_string(id);
}

} // namespace HubEventDedupe
} // namespace GUI
} // namespace Slic3r
