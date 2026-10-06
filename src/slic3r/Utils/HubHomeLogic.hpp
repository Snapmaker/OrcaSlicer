#ifndef slic3r_HubHomeLogic_hpp_
#define slic3r_HubHomeLogic_hpp_

#include <string>

// Rules for embedding the phone hub page (resources/web/orca/stream_center.html, served by the
// separate --hub process at /r/<token>/) in a desktop view - the Home tab showed it until it became
// Recent / Print History - kept free of wx so they can be tested on their own
// (tests/slic3rutils/hub_home_tests.cpp). RemoteHub classifies the hub's record with them:
//   * which address it loads (always built from the hub's own /hub/info answer, never a fixed port),
//   * where it may navigate (the hub page only; http(s) links elsewhere go to the system browser),
//   * what a re-check of the hub means for the page on screen (port moved, new link, hub gone),
//   * how the page is told the slicer's theme (?theme= on load, window.__edgeTheme() live).
namespace Slic3r {
namespace HubHome {

// The hub's own token rule (RemoteHub.cpp valid_token): 10-32 characters of [a-z0-9].
bool valid_token(const std::string& token);

// "dark" or "light": the value of the page's ?theme= parameter and of __edgeTheme()'s argument.
const char* theme_name(bool dark);

// http://127.0.0.1:<port>/r/<token>/?embed=1&theme=<dark|light>, or "" when the port or the token
// is unusable (hub not up yet, a token that is not the hub's shape). Nothing but a checked port and
// a checked token is ever put into the address.
std::string hub_page_url(int port, const std::string& token, bool dark);

// true for an address under the hub page this view loaded: http://127.0.0.1:<port>/r/<token>/...
// (query and fragment allowed). Any other origin, port, token or path is false.
bool is_hub_page_url(const std::string& url, int port, const std::string& token);

// What the view may navigate to at all: the hub page (above), plus about:blank and data: for the
// view's own blank state.
bool is_allowed_navigation(const std::string& url, int port, const std::string& token);

// Only plain http(s) addresses are ever handed to the system browser; any other scheme is dropped.
bool is_external_browser_url(const std::string& url);

// What a fresh look at the hub (/hub/info) means for the page that is on screen.
enum class Recheck {
    Keep,    // same hub, same port, same link: leave the page alone (no reload, streams keep running)
    Reload,  // the hub is up but on another port (phone on/off rebinds, fallback port) or with a
             // new link ("New link", a restarted hub): load the new address
    HubDown  // no usable answer. Only the first load / Start acts on this directly; the view's
             // re-checks go through Watch below, which never acts on a single miss
};
Recheck recheck(bool hub_alive, int hub_port, const std::string& hub_token, int loaded_port,
                const std::string& loaded_token);

// ---- telling "the hub is gone" from "the hub did not answer this once" ------------------------
//
// One unanswered /hub/info is not proof of anything: the hub answers it on a request thread that
// can be slow for a moment (a busy PC, a Tailscale status refresh, hub.json being rewritten while
// it is read). So the view never takes the page down on one failed probe. It asks the hub's own
// record in <datadir>/hub (hub.json and the exit note a clean quit leaves) what happened, and
// only a confirmed "gone" or a run of unanswered probes changes what is on screen.

// What the hub's record says when /hub/info did not answer.
enum class Presence {
    Unknown,  // the record could not be read right now (being rewritten, locked): no signal
    Running,  // hub.json names a live process: the hub is there, it just did not answer in time
    NoRecord, // no hub.json: the hub quit cleanly (tray icon, idle exit, replaced) or never ran
    Crashed   // hub.json names a process that no longer exists: it died without cleaning up
};

// Why a hub that quit cleanly quit, from the exit note it leaves next to hub.json.
enum class ExitReason {
    Unknown,   // no note, or one this build does not know
    Tray,      // "Quit hub" in its tray icon's menu
    Requested, // POST /hub/quit: a slicer replacing it with its own version, or a test
    Idle       // phone access off and no slicer running for a minute
};

// The exit note's "reason" field ("tray", "request", "idle") as an ExitReason.
ExitReason exit_reason_from(const std::string& reason);

// The record, classified: file_exists = hub.json is there; parsed = it could be read and parsed;
// pid_alive = the process it names is still running (only meaningful when parsed).
Presence classify_record(bool file_exists, bool parsed, bool pid_alive);

// One look at the hub: /hub/info's answer, and (when it did not answer) what the record says.
struct Probe
{
    bool        answered { false }; // /hub/info answered 200 with alive
    int         port { 0 };
    std::string token;
    Presence    presence { Presence::Unknown };      // only looked at when !answered
    ExitReason  exit_reason { ExitReason::Unknown }; // ditto, and only for NoRecord
};

enum class Verdict {
    Keep,        // the hub answers at the address that is loaded: leave the page alone
    Reload,      // the hub answers at another port / with another link: load that
    Retry,       // no answer, but no proof it is gone either: keep what is on screen, look again soon
    Unreachable, // several answers missed in a row from a hub whose record says it is running
    Gone         // the record says it is gone (quit or crashed), seen twice in a row
};

const char* verdict_name(Verdict v);
const char* presence_name(Presence p);
const char* exit_reason_name(ExitReason r);

// Consecutive-failure bookkeeping for the view's re-checks. Not thread-safe: the view only
// touches it on the GUI thread.
class Watch
{
public:
    static constexpr int UNREACHABLE_AFTER = 3; // unanswered probes in a row, record not saying "gone"
    static constexpr int GONE_AFTER        = 2; // probes in a row whose record says "gone"
    static constexpr int RECHECK_S         = 20; // the steady-state re-check period

    Verdict judge(const Probe& probe, int loaded_port, const std::string& loaded_token);
    // Seconds until the next re-check: RECHECK_S while the hub answers, then 2, 4, 8, 10, 10, ...
    // after misses, so a hiccup is retried quickly and a dead hub is not hammered.
    int  next_check_s() const;
    int  failures() const { return m_failures; }
    void reset() { m_failures = 0; m_gone = 0; }

private:
    int m_failures { 0 };
    int m_gone { 0 };
};

// A wxEVT_WEBVIEW_ERROR, classified. WebView2 only reports main-frame navigations there (images,
// camera streams and favicons never arrive), but cancelled navigations do (our own vetoes, a
// reload interrupting a load) and so can a stale navigation to a page this view has since left.
enum class LoadError {
    Ignore,     // cancelled / aborted, or not about the hub page that is loaded now
    Connection, // the hub page's server could not be reached: the hub may have moved or stopped
    Page        // the hub answered but the page failed (HTTP error, bad response, ...)
};
LoadError classify_load_error(bool cancelled, bool connection, const std::string& url, int loaded_port,
                              const std::string& loaded_token);

// The script that switches an already loaded page to the slicer's theme without a reload.
std::string theme_script(bool dark);

} // namespace HubHome
} // namespace Slic3r

#endif
