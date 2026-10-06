// Relay notifications (see RemoteNotify.hpp). Hub process only; wx-free, like the rest of the
// hub server: libcurl through the fork's Http wrapper, nlohmann::json, one worker thread.
#include "RemoteNotify.hpp"

#include "RemoteEvents.hpp"
#include "WebPush.hpp"
#include "AppPush.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace RemoteNotify {

using json = nlohmann::json;

// A masked credential reads "****" + its last four characters. The page sends the masked value
// straight back when it saves a destination it never saw in full, so anything starting with the
// marker means "keep what is stored" - the secret only ever travels one way.
static const char* const MASK      = "****";
static const size_t      MASK_LEN  = 4;
static const int         MAX_TRIES = 3;      // one send plus two retries
static const int         FAIL_MARK = 5;      // consecutive failures before a destination reads "failing"
static const size_t      MAX_QUEUE = 200;    // a stuck relay must not grow the hub without bound
static const size_t      MAX_BODY  = 3800;   // ntfy caps a message body at 4096 bytes
static const long        T_CONNECT = 5;
static const long        T_MAX     = 15;

struct Dest
{
    std::string id, type, name, min_severity { "info" };
    bool        enabled { true };
    std::vector<std::string> kinds;                      // empty: every kind
    std::string server { "https://ntfy.sh" }, topic, token;         // ntfy
    std::string user_key, app_token;                     // pushover
    std::string url, header_name, header_value;          // webhook
    // status, never persisted
    int         failures { 0 };
    int         last_status { 0 };
    long long   last_sent { 0 };
    std::string last_error;
};

static std::mutex              g_mutex;
static std::vector<Dest>       g_dests;
static std::deque<json>        g_queue;
static std::condition_variable g_cv;
static std::thread             g_worker;
static std::atomic<bool>       g_running { false };
static std::string             g_phone_link;   // what a Click/url points at: the remote link, or the LAN one
static std::string             g_lan_link;     // the LAN link, appended to the body when there is one

// ---------------------------------------------------------------- small helpers ----

static long long now_ms()
{
    return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string random_id()
{
    static const char* hx = "0123456789abcdef";
    std::random_device rd;
    std::string        s;
    for (int i = 0; i < 8; ++i) { const unsigned v = rd() & 0xffu; s += hx[v >> 4]; s += hx[v & 15]; }
    return s;
}

static std::string lower_ascii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

static std::string trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

static std::string mask(const std::string& secret)
{
    if (secret.empty()) return "";
    if (secret.size() <= MASK_LEN) return MASK;
    return MASK + secret.substr(secret.size() - MASK_LEN);
}

static bool is_masked(const std::string& v) { return v.compare(0, MASK_LEN, MASK) == 0; }

// A value the page sent for a credential field: absent or masked means "keep the stored one".
static void take_secret(const json& j, const char* key, std::string& out)
{
    if (!j.contains(key) || !j[key].is_string()) return;
    const std::string v = trim(j[key].get<std::string>());
    if (is_masked(v)) return;
    out = v;
}

static void take_string(const json& j, const char* key, std::string& out)
{
    if (j.contains(key) && j[key].is_string()) out = trim(j[key].get<std::string>());
}

// Nothing a printer, a file name or a relay puts in front of us belongs in an HTTP header
// unfiltered: a CR or LF would end the header and start one of the caller's choosing. Headers
// here are ASCII-only for the same reason ntfy's own docs ask for it.
static std::string header_safe(const std::string& s, size_t limit = 200)
{
    std::string out;
    for (unsigned char c : s) {
        if (c == '\r' || c == '\n' || c == '\t') { if (!out.empty() && out.back() != ' ') out += ' '; }
        else if (c < 0x20 || c >= 0x7f) out += '?';
        else out += (char) c;
        if (out.size() >= limit) break;
    }
    return trim(out);
}

static std::string form_encode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char) c;
        else if (c == ' ') out += '+';
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

// Strip a destination's own secrets out of anything we are about to store or log. libcurl's
// error text can carry the URL it failed on, and a webhook URL is a credential.
static std::string scrub(std::string text, const Dest& d)
{
    for (const std::string& s : { d.topic, d.token, d.user_key, d.app_token, d.url, d.header_value }) {
        if (s.size() < 6) continue;
        for (size_t at = text.find(s); at != std::string::npos; at = text.find(s, at + 3))
            text.replace(at, s.size(), "***");
    }
    if (text.size() > 300) text.resize(300);
    return text;
}

// HTTPS for anything off this PC. Plain http is allowed only against the loopback address, which
// is what the gate's mock relay runs on - a real relay over http would put the topic, the user
// key or the webhook token on the wire in clear.
static bool url_allowed(const std::string& url, std::string& why)
{
    const std::string u = lower_ascii(url);
    if (u.compare(0, 8, "https://") == 0) return true;
    if (u.compare(0, 7, "http://") == 0) {
        const std::string rest = u.substr(7);
        if (rest.compare(0, 10, "127.0.0.1:") == 0 || rest.compare(0, 10, "127.0.0.1/") == 0 || rest == "127.0.0.1" ||
            rest.compare(0, 10, "localhost:") == 0 || rest.compare(0, 10, "localhost/") == 0 || rest == "localhost")
            return true;
        why = "plain http is only allowed to 127.0.0.1; use https://";
        return false;
    }
    why = "the address must start with https://";
    return false;
}

static int severity_rank(const std::string& s)
{
    if (s == "error") return 2;
    if (s == "warning") return 1;
    return 0; // info, and anything we do not know
}

// ntfy's Priority is 1 (min) to 5 (max); 3 is its default. An event the watcher called an error
// deserves to break through a phone's quiet hours, an ordinary "finished" does not.
static int ntfy_priority(const std::string& severity)
{
    switch (severity_rank(severity)) {
    case 2: return 5;
    case 1: return 4;
    default: return 3;
    }
}

// Pushover: 1 is "high" (bypasses the phone's quiet hours), 0 is normal. Emergency (2) is
// deliberately not used - it re-alerts until somebody acknowledges it, which is a decision for
// the person, not a default.
static int pushover_priority(const std::string& severity) { return severity_rank(severity) == 2 ? 1 : 0; }

// One GitHub emoji short code per event kind; ntfy turns Tags into the emoji in front of the title.
static const char* kind_tag(const std::string& kind)
{
    if (kind == "started")   return "arrow_forward";
    if (kind == "finished")  return "white_check_mark";
    if (kind == "failed")    return "x";
    if (kind == "cancelled") return "no_entry_sign";
    if (kind == "paused")    return "double_vertical_bar";
    if (kind == "resumed")   return "arrows_counterclockwise";
    if (kind == "runout")    return "thread";
    if (kind == "error")     return "rotating_light";
    return "printer";
}

static std::string ev_str(const json& e, const char* key, const std::string& fallback = "")
{
    if (e.contains(key) && e[key].is_string()) return e[key].get<std::string>();
    return fallback;
}

static std::string printer_name(const json& e)
{
    if (e.contains("printer") && e["printer"].is_object()) {
        const json& p = e["printer"];
        if (p.contains("name") && p["name"].is_string()) return p["name"].get<std::string>();
        if (p.contains("id") && p["id"].is_string()) return p["id"].get<std::string>();
    }
    return "";
}

// The line a person reads on the lock screen: the event's own sentence, with the printer's name
// in front when the title does not already carry it.
static std::string message_text(const json& e)
{
    std::string body = ev_str(e, "text");
    const std::string who = printer_name(e);
    if (body.empty()) body = ev_str(e, "title", "Printer event");
    if (!who.empty() && body.find(who) == std::string::npos) body = who + ": " + body;
    body = RemoteEvents::notification_body(body, ev_str(e, "code"));
    if (body.size() > MAX_BODY) body.resize(MAX_BODY);
    return body;
}

static std::string title_text(const json& e)
{
    std::string t = ev_str(e, "title");
    if (t.empty()) t = "EdgeSlicer";
    return t;
}

// ------------------------------------------------------------------- the senders ----

struct SendResult
{
    bool        ok { false };
    int         status { 0 };
    std::string error;
};

static SendResult http_post(const std::string& url, const std::string& content_type, const std::string& body,
                            const std::vector<std::pair<std::string, std::string>>& headers)
{
    SendResult res;
    // Http::post only hands libcurl the POST fields when a body is present; without one it falls
    // back to reading stdin and the hub hangs. Every send here carries a body.
    Http req = Http::post(url);
    req.timeout_connect(T_CONNECT).timeout_max(T_MAX).header("Content-Type", content_type);
    for (const auto& h : headers)
        if (!h.second.empty()) req.header(h.first, h.second);
    req.set_post_body(body)
        .on_complete([&](std::string, unsigned s) { res.ok = true; res.status = (int) s; })
        .on_error([&](std::string, std::string err, unsigned s) { res.status = (int) s; res.error = err.empty() ? ("HTTP " + std::to_string(s)) : err; })
        .perform_sync();
    if (res.error.empty() && !res.ok) res.error = "the relay did not answer";
    return res;
}

// One line, and only when the two links differ: a person at home reading this on ntfy or Pushover
// gets the address that does not go out to the tailnet and back.
static std::string with_lan(const std::string& message, const std::string& lan, const std::string& link)
{
    if (lan.empty() || lan == link) return message;
    return message + "\n\nOn your home network: " + lan;
}

static SendResult send_ntfy(const Dest& d, const json& e, const std::string& link, const std::string& lan)
{
    std::string server = d.server.empty() ? "https://ntfy.sh" : d.server;
    while (!server.empty() && server.back() == '/') server.pop_back();
    const std::string url = server + "/" + d.topic;
    std::string why;
    if (!url_allowed(url, why)) return { false, 0, why };
    if (d.topic.empty()) return { false, 0, "this ntfy destination has no topic" };
    std::vector<std::pair<std::string, std::string>> h;
    h.emplace_back("Title", header_safe(title_text(e)));
    h.emplace_back("Priority", std::to_string(ntfy_priority(ev_str(e, "severity", "info"))));
    h.emplace_back("Tags", kind_tag(ev_str(e, "kind")));
    if (!link.empty()) h.emplace_back("Click", header_safe(link, 400)); // omitted while phone access is off
    if (!d.token.empty()) h.emplace_back("Authorization", "Bearer " + header_safe(d.token, 300));
    return http_post(url, "text/plain; charset=utf-8", with_lan(message_text(e), lan, link), h);
}

// Pushover's endpoint is fixed - it is one service, not a server the person picks. The gate
// needs somewhere to point it that is not the real Pushover, so SNORCA_PUSHOVER_API redirects
// it, and url_allowed() still applies: the override has to be https or 127.0.0.1.
static std::string pushover_endpoint()
{
    const char* env = std::getenv("SNORCA_PUSHOVER_API");
    if (env && *env) {
        std::string why;
        if (url_allowed(env, why)) return env;
    }
    return "https://api.pushover.net/1/messages.json";
}

static SendResult send_pushover(const Dest& d, const json& e, const std::string& link, const std::string& lan)
{
    if (d.app_token.empty() || d.user_key.empty()) return { false, 0, "this Pushover destination needs both an application token and a user key" };
    std::string body = "token=" + form_encode(d.app_token) + "&user=" + form_encode(d.user_key) +
                       "&title=" + form_encode(title_text(e)) + "&message=" + form_encode(with_lan(message_text(e), lan, link)) +
                       "&priority=" + std::to_string(pushover_priority(ev_str(e, "severity", "info")));
    if (!link.empty()) body += "&url=" + form_encode(link) + "&url_title=" + form_encode("Open the printer page");
    return http_post(pushover_endpoint(), "application/x-www-form-urlencoded", body, {});
}

static SendResult send_webhook(const Dest& d, const json& e, const std::string& link, const std::string& lan)
{
    std::string why;
    if (d.url.empty()) return { false, 0, "this webhook has no address" };
    if (!url_allowed(d.url, why)) return { false, 0, why };
    json payload = e;
    if (!link.empty()) payload["link"] = link; // the phone page, for a webhook that wants to link back
    if (!lan.empty()) payload["lan_link"] = lan; // ... and the same page on the home network
    std::vector<std::pair<std::string, std::string>> h;
    if (!d.header_name.empty()) h.emplace_back(header_safe(d.header_name, 100), header_safe(d.header_value, 400));
    return http_post(d.url, "application/json", payload.dump(), h);
}

static SendResult send_once(const Dest& d, const json& e, const std::string& link, const std::string& lan)
{
    if (d.type == "ntfy") return send_ntfy(d, e, link, lan);
    if (d.type == "pushover") return send_pushover(d, e, link, lan);
    if (d.type == "webhook") return send_webhook(d, e, link, lan);
    return { false, 0, "unknown destination type" };
}

// A transport error, a 429 or a 5xx is worth trying again; any other 4xx is the relay telling us
// the request itself is wrong (a topic that needs a token, a dead Pushover key) and repeating it
// only burns the rate limit.
static bool worth_retrying(const SendResult& r)
{
    if (r.ok) return false;
    if (r.status == 0) return true;
    return r.status == 429 || r.status >= 500;
}

static bool sleep_interruptible(int ms)
{
    for (int slept = 0; slept < ms; slept += 100) {
        if (!g_running) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

// Three attempts, 1 s then 3 s apart. Short enough that a "finished" is still news, long enough
// that a relay hiccup or a laptop's Wi-Fi coming back is ridden out.
static SendResult send_with_retries(const Dest& d, const json& e, const std::string& link, const std::string& lan)
{
    SendResult r;
    for (int attempt = 1; attempt <= MAX_TRIES; ++attempt) {
        r = send_once(d, e, link, lan);
        if (r.ok || !worth_retrying(r)) break;
        if (attempt == MAX_TRIES) break;
        if (!sleep_interruptible(attempt == 1 ? 1000 : 3000)) break;
    }
    return r;
}

// ------------------------------------------------------------------ the queue ----

static bool wants(const Dest& d, const json& e)
{
    if (!d.enabled) return false;
    // The two filters are an AND, and deliberately so: minimum severity is the coarse dial this
    // destination had before per-kind filters existed and keeps meaning what it meant, while the
    // kind list is the fine one. An event is sent when both allow it.
    if (severity_rank(ev_str(e, "severity", "info")) < severity_rank(d.min_severity)) return false;
    return RemoteEvents::kind_allowed(d.kinds, ev_str(e, "kind"));
}

static void record(const std::string& id, const SendResult& r)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Dest& d : g_dests) {
        if (d.id != id) continue;
        d.last_sent   = now_ms();
        d.last_status = r.status;
        if (r.ok) {
            d.failures = 0;
            d.last_error.clear();
        } else {
            ++d.failures;
            d.last_error = scrub(r.error, d);
            // Never disabled behind the person's back: a destination that keeps failing is
            // marked, stays in the list and keeps being tried, so the page can say why.
            BOOST_LOG_TRIVIAL(warning) << "RemoteNotify: " << d.type << " destination " << d.id << " failed ("
                                       << d.failures << " in a row): " << d.last_error;
        }
        return;
    }
}

static void worker()
{
    for (;;) {
        json        ev;
        std::vector<Dest> targets;
        std::string link, lan;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait(lock, [] { return !g_running || !g_queue.empty(); });
            if (!g_running && g_queue.empty()) return;
            if (g_queue.empty()) continue;
            ev = g_queue.front();
            g_queue.pop_front();
            link    = g_phone_link;
            lan     = g_lan_link;
            targets = g_dests;
        }
        for (const Dest& d : targets) {
            if (!g_running) return;
            if (!wants(d, ev)) continue;
            record(d.id, send_with_retries(d, ev, link, lan));
        }
        // Web Push (P7) is not a destination somebody configures - it is a built-in fan-out over
        // whatever phones have subscribed, with its own minimum severity. It rides this worker
        // because the reason for the worker is the same: nobody's request thread may wait on a
        // network service half a world away.
        if (g_running) WebPush::deliver(ev);
        // ... and the same again for the native app (EdgeSlicer app phase 1). A third built-in fan-out,
        // over whatever devices have registered, with its own minimum severity. It rides this
        // worker for the reason the worker exists: APNs and FCM are on the other side of the
        // internet and no request thread may wait on them.
        if (g_running) AppPush::deliver(ev);
    }
}

// ------------------------------------------------------------------ persistence ----

static Dest from_json(const json& j)
{
    Dest d;
    d.id           = j.value("id", "");
    d.type         = j.value("type", "");
    d.name         = j.value("name", "");
    d.enabled      = j.value("enabled", true);
    d.min_severity = j.value("min_severity", std::string("info"));
    // A kind nobody emits is dropped rather than refused: settings.json is a file a person may
    // have hand-edited, and one stale name must not cost them the destination.
    if (j.contains("kinds") && j["kinds"].is_array())
        for (const auto& k : j["kinds"])
            if (k.is_string() && RemoteEvents::is_kind(k.get<std::string>())) d.kinds.push_back(k.get<std::string>());
    d.server       = j.value("server", std::string("https://ntfy.sh"));
    d.topic        = j.value("topic", "");
    d.token        = j.value("token", "");
    d.user_key     = j.value("user_key", "");
    d.app_token    = j.value("app_token", "");
    d.url          = j.value("url", "");
    d.header_name  = j.value("header_name", "");
    d.header_value = j.value("header_value", "");
    return d;
}

static json to_json(const Dest& d, bool masked)
{
    json j;
    j["id"]           = d.id;
    j["type"]         = d.type;
    j["name"]         = d.name;
    j["enabled"]      = d.enabled;
    j["min_severity"] = d.min_severity;
    j["kinds"]        = d.kinds;
    // The same filter as a map of every kind, for the page's checkboxes. Derived, never stored.
    if (masked) j["events"] = RemoteEvents::events_map(d.kinds);
    if (d.type == "ntfy") {
        j["server"] = d.server;
        j["topic"]  = masked ? mask(d.topic) : d.topic;
        j["token"]  = masked ? mask(d.token) : d.token;
        if (masked) j["has_token"] = !d.token.empty();
    } else if (d.type == "pushover") {
        j["user_key"]  = masked ? mask(d.user_key) : d.user_key;
        j["app_token"] = masked ? mask(d.app_token) : d.app_token;
    } else if (d.type == "webhook") {
        // A webhook URL is a credential too (a Discord or Slack hook is nothing but its URL), so
        // it is masked like any other; only its scheme and host go out separately, for the page
        // to show. The masked form starts with the marker, so saving it back keeps the stored URL.
        if (masked) {
            const size_t slashes  = d.url.find("//");
            const size_t host_end = slashes == std::string::npos ? std::string::npos : d.url.find('/', slashes + 2);
            j["url"]      = mask(d.url);
            j["url_host"] = host_end == std::string::npos ? d.url : d.url.substr(0, host_end);
        } else {
            j["url"] = d.url;
        }
        j["header_name"]  = d.header_name;
        j["header_value"] = masked ? mask(d.header_value) : d.header_value;
    }
    if (masked) {
        j["status"]      = d.failures >= FAIL_MARK ? "failing" : (d.failures > 0 ? "retrying" : "ok");
        j["failures"]    = d.failures;
        j["last_status"] = d.last_status;
        j["last_sent"]   = d.last_sent;
        j["last_error"]  = d.last_error;
    }
    return j;
}

json settings_json()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    json j;
    j["destinations"] = json::array();
    for (const Dest& d : g_dests) j["destinations"].push_back(to_json(d, false));
    return j;
}

json masked_json()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    json j;
    j["destinations"] = json::array();
    for (const Dest& d : g_dests) j["destinations"].push_back(to_json(d, true));
    // The catalogue, not a filter: `kinds` has been this list since P5 and stays that, and
    // `all_kinds` is the same list under the name the other two channels use for it.
    j["kinds"]      = RemoteEvents::all_kinds();
    j["all_kinds"]  = RemoteEvents::all_kinds();
    j["severities"] = json::array({ "info", "warning", "error" });
    j["phone_link"] = !g_phone_link.empty(); // whether a Click/url link is attached, never the link itself
    return j;
}

// ------------------------------------------------------------------ public API ----

void start(const json& saved)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_dests.clear();
        try {
            if (saved.is_object() && saved.contains("destinations") && saved["destinations"].is_array())
                for (const auto& e : saved["destinations"]) {
                    Dest d = from_json(e);
                    if (d.id.empty()) d.id = random_id();
                    if (d.type == "ntfy" || d.type == "pushover" || d.type == "webhook") g_dests.push_back(d);
                }
        } catch (...) {} // a settings.json somebody hand-edited must not stop the hub starting
        if (g_running) return;
        g_running = true;
    }
    BOOST_LOG_TRIVIAL(info) << "RemoteNotify: " << g_dests.size() << " notification destination(s)";
    g_worker = std::thread(worker);
}

void stop()
{
    if (!g_running) return;
    g_running = false;
    g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();
}

void set_phone_links(const std::string& remote, const std::string& lan)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    // Remote first: a relay's one link has to work from wherever the phone happens to be.
    g_phone_link = remote.empty() ? lan : remote;
    g_lan_link   = lan;
}

bool has_destinations()
{
    if (WebPush::has_subscriptions() || AppPush::has_devices()) return true;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const Dest& d : g_dests)
        if (d.enabled) return true;
    return false;
}

void deliver(const json& event)
{
    // Nowhere to send it - no relay and no subscribed phone - means there is nothing to queue;
    // the tray balloon and the event ring are the hub's own, and happen either way.
    const bool phones = WebPush::has_subscriptions() || AppPush::has_devices();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_dests.empty() && !phones) return;
    g_queue.push_back(event);
    if (g_queue.size() > MAX_QUEUE) g_queue.pop_front();
    g_cv.notify_one();
}

std::pair<int, std::string> configure(const std::string& body)
{
    json in;
    try {
        in = json::parse(body);
    } catch (...) {
        return { 400, json({ { "error", "the body must be JSON" } }).dump() };
    }
    if (!in.is_object()) return { 400, json({ { "error", "the body must be a JSON object" } }).dump() };
    const std::string type = trim(in.value("type", ""));
    const std::string id   = trim(in.value("id", ""));
    if (id.empty() && type != "ntfy" && type != "pushover" && type != "webhook")
        return { 400, json({ { "error", "type must be ntfy, pushover or webhook" } }).dump() };

    std::string why;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Dest* existing = nullptr;
        for (Dest& d : g_dests)
            if (!id.empty() && d.id == id) existing = &d;
        if (!id.empty() && !existing) return { 404, json({ { "error", "no such destination" } }).dump() };

        Dest d = existing ? *existing : Dest();
        if (!existing) {
            d.id   = random_id();
            d.type = type;
        }
        take_string(in, "name", d.name);
        if (in.contains("enabled") && in["enabled"].is_boolean()) d.enabled = in["enabled"].get<bool>();
        if (in.contains("min_severity") && in["min_severity"].is_string()) {
            const std::string s = in["min_severity"].get<std::string>();
            if (s != "info" && s != "warning" && s != "error") return { 400, json({ { "error", "min_severity must be info, warning or error" } }).dump() };
            d.min_severity = s;
        }
        if (RemoteEvents::has_kind_filter(in)) {
            std::string bad;
            if (!RemoteEvents::read_kinds(in, d.kinds, bad)) return { 400, json({ { "error", bad } }).dump() };
        }
        if (d.type == "ntfy") {
            take_string(in, "server", d.server);
            if (d.server.empty()) d.server = "https://ntfy.sh";
            while (!d.server.empty() && d.server.back() == '/') d.server.pop_back();
            take_secret(in, "topic", d.topic);
            take_secret(in, "token", d.token);
            if (d.topic.empty()) return { 400, json({ { "error", "an ntfy destination needs a topic" } }).dump() };
            if (d.topic.find_first_of("/? #") != std::string::npos)
                return { 400, json({ { "error", "an ntfy topic cannot contain a slash, a space or a question mark" } }).dump() };
            if (!url_allowed(d.server + "/" + d.topic, why)) return { 400, json({ { "error", why } }).dump() };
        } else if (d.type == "pushover") {
            take_secret(in, "user_key", d.user_key);
            take_secret(in, "app_token", d.app_token);
            if (d.user_key.empty() || d.app_token.empty())
                return { 400, json({ { "error", "Pushover needs both your user key and an application token" } }).dump() };
        } else if (d.type == "webhook") {
            take_secret(in, "url", d.url);
            take_string(in, "header_name", d.header_name);
            take_secret(in, "header_value", d.header_value);
            if (!url_allowed(d.url, why)) return { 400, json({ { "error", why } }).dump() };
        } else {
            return { 400, json({ { "error", "type must be ntfy, pushover or webhook" } }).dump() };
        }
        if (d.name.empty()) d.name = d.type;
        // A corrected topic, key or address is a fresh start - a destination that was marked
        // "failing" must not stay marked once it is fixed. Renaming it, or changing which events
        // it wants, tells us nothing about whether it works, so the mark stands.
        const bool where_changed = !existing || existing->server != d.server || existing->topic != d.topic ||
                                   existing->token != d.token || existing->user_key != d.user_key ||
                                   existing->app_token != d.app_token || existing->url != d.url ||
                                   existing->header_name != d.header_name || existing->header_value != d.header_value;
        if (where_changed) { d.failures = 0; d.last_error.clear(); }
        if (existing) *existing = d;
        else g_dests.push_back(d);
    }
    BOOST_LOG_TRIVIAL(info) << "RemoteNotify: destination " << (id.empty() ? "added" : "updated") << " (" << (type.empty() ? "existing" : type) << ")";
    return { 200, masked_json().dump() };
}

std::pair<int, std::string> remove(const std::string& id)
{
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (size_t i = 0; i < g_dests.size(); ++i)
            if (g_dests[i].id == id) { g_dests.erase(g_dests.begin() + i); found = true; break; }
    }
    if (!found) return { 404, json({ { "error", "no such destination" } }).dump() };
    BOOST_LOG_TRIVIAL(info) << "RemoteNotify: destination removed";
    return { 200, masked_json().dump() };
}

std::pair<int, std::string> test(const std::string& id, const std::string& phone_link)
{
    Dest d;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        bool found = false;
        for (const Dest& x : g_dests)
            if (x.id == id) { d = x; found = true; break; }
        if (!found) return { 404, json({ { "error", "no such destination" } }).dump() };
    }
    // Sent on this thread, once, with no retries: the person is waiting for the answer and a
    // wrong topic or a dead server should show as itself, not as a queued retry.
    json e;
    e["id"]       = 0;
    e["time"]     = now_ms();
    e["instance"] = 0;
    e["printer"]  = json{ { "id", "test" }, { "name", "Test" }, { "kind", "printhost" } };
    // Dressed as a kind this destination actually wants, so the test reads like the thing it
    // stands in for - the emoji, the tag and the priority a real one would carry. The test is
    // still sent whatever the filters say: the button is here to prove the relay works.
    e["kind"]     = RemoteEvents::test_kind(d.kinds);
    e["severity"] = "info";
    e["title"]    = "EdgeSlicer test";
    e["text"]     = "This is a test notification from the hub on your PC. If you can read it, notifications work.";
    e["test"]     = true;

    std::string link = phone_link, lan;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (link.empty()) link = g_phone_link;
        lan = g_lan_link;
    }
    const SendResult r = send_once(d, e, link, lan);
    record(d.id, r);
    json out;
    out["ok"]     = r.ok;
    out["status"] = r.status; // the relay's own HTTP status, straight through
    out["error"]  = scrub(r.error, d);
    // What this destination would let through, so the page can say "sent as finished; started is
    // off" instead of leaving somebody to wonder why the real thing never arrives.
    out["kind"]   = e["kind"];
    out["kinds"]  = RemoteEvents::enabled_kinds(d.kinds);
    out["events"] = RemoteEvents::events_map(d.kinds);
    return { 200, out.dump() };
}

} // namespace RemoteNotify
} // namespace GUI
} // namespace Slic3r
