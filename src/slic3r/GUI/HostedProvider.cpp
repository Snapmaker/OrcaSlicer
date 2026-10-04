// The EdgeSlicer push service as a third Provider (see HostedPush.hpp and
// edgeslicer-push/docs/PROTOCOL.md). Hub process only; wx-free, like the rest of the push plane.
#include "HostedPush.hpp"

#include "RemoteHub.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <openssl/rand.h>

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <random>

namespace Slic3r {
namespace GUI {
namespace AppPush {
namespace Hosted {

using json = nlohmann::json;

const char* const DEFAULT_URL = "https://push.edgeslicer.com";

static const long T_CONNECT = 5;
static const long T_MAX     = 20;

// ------------------------------------------------------------------- small helpers ----

static std::string lower(std::string s)
{
    for (char& c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}

static std::string trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// "https://push.edgeslicer.com/" -> "https://push.edgeslicer.com"
static std::string normalise_url(std::string url)
{
    url = trim(url);
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

static std::string host_of(const std::string& url)
{
    const size_t scheme = url.find("://");
    const size_t start  = scheme == std::string::npos ? 0 : scheme + 3;
    const size_t end    = url.find_first_of("/:?#", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

static std::string one_line(const std::string& s, size_t limit = 300)
{
    std::string out;
    for (unsigned char c : s) {
        if (c == '\r' || c == '\n' || c == '\t') { if (!out.empty() && out.back() != ' ') out += ' '; }
        else if (c < 0x20) out += '?';
        else out += (char) c;
        if (out.size() >= limit) break;
    }
    return trim(out);
}

static bool debug_routes()
{
    const char* on = std::getenv("SNORCA_DEBUG_ROUTES");
    return on && std::string(on) == "1";
}

bool url_allowed(const std::string& url_in, std::string& why)
{
    const std::string url = lower(normalise_url(url_in));
    why.clear();
    if (url.empty()) { why = "no push service URL is set"; return false; }
    if (url.size() > 200) { why = "that push service URL is too long"; return false; }
    for (unsigned char c : url)
        if (c <= 0x20 || c >= 0x7f) { why = "the push service URL has a character that cannot be in one"; return false; }
    const std::string host = host_of(url);
    if (host.empty()) { why = "the push service URL has no host"; return false; }
    if (url.compare(0, 8, "https://") == 0) return true;
    if (url.compare(0, 7, "http://") == 0) {
        // Cleartext only ever to the gate's mock on loopback, and only when the debug routes are
        // on: a shipped hub must not be talkable into posting device tokens over plain HTTP.
        if (debug_routes() && (host == "127.0.0.1" || host == "localhost" || host == "[::1]")) return true;
        why = "the push service URL must start with https://";
        return false;
    }
    why = "the push service URL must start with https://";
    return false;
}

Transport http_transport()
{
    return [](const HttpCall& c) {
        HttpReply r;
        Http      h = c.method == "GET" ? Http::get(c.url) : Http::post(c.url);
        // None of the slicer's global client headers (the X-BBL-* set, which carries a device id):
        // the service is told this hub's id and nothing else about the PC.
        h.clear_headers();
        h.timeout_connect(T_CONNECT).timeout_max(T_MAX);
        for (const auto& kv : c.headers) h.header(kv.first, kv.second);
        if (c.method != "GET") h.set_post_body(c.body);
        h.on_header_callback([&r](std::string all) { r.headers = std::move(all); })
            .on_complete([&r](std::string b, unsigned st) { r.status = (int) st; r.body = std::move(b); })
            .on_error([&r](std::string b, std::string e, unsigned st) {
                r.status = (int) st;
                r.body   = std::move(b);
                r.error  = std::move(e);
            })
            .perform_sync();
        return r;
    };
}

// ------------------------------------------------------------------- the bodies ----

std::string body_for_send(const PushRequest& req, long long ts, const std::string& nonce)
{
    json j;
    j["v"]        = 1;
    j["op"]       = "send";
    j["ts"]       = ts;
    j["nonce"]    = nonce;
    j["platform"] = req.platform;
    // APNs only; the service picks api.push.apple.com or the sandbox host from it, per device,
    // exactly as ApnsProvider::host_for does.
    if (req.platform == "apns") j["env"] = req.env == "sandbox" ? "sandbox" : "production";
    j["token"]    = req.device_token;
    j["bundle"]   = req.bundle;
    j["e"]        = req.ciphertext_b64u;
    j["collapse"] = req.collapse_id;
    j["thread"]   = req.thread_id;
    j["priority"] = req.priority >= 10 ? 10 : 5;
    j["ttl"]      = std::max(0, std::min(86400, req.ttl_seconds));
    // Additive and only when the device asked for it: the push service ignores fields it does not
    // know, so this is inert until it builds `aps.interruption-level` from it (research note H8).
    if (req.platform == "apns" && !req.interruption_level.empty()) j["level"] = req.interruption_level;
    return j.dump();
}

std::string body_for_op(const std::string& op, long long ts, const std::string& nonce, const std::string& pubkey_hex)
{
    json j;
    j["v"]     = 1;
    j["op"]    = op;
    j["ts"]    = ts;
    j["nonce"] = nonce;
    if (op == "register") j["pubkey"] = pubkey_hex;
    return j.dump();
}

std::string random_nonce()
{
    static const char* hx = "0123456789abcdef";
    unsigned char      b[16];
    if (RAND_bytes(b, sizeof(b)) != 1) {
        std::random_device rd;
        for (unsigned char& c : b) c = (unsigned char) (rd() & 0xff);
    }
    std::string s;
    for (unsigned char c : b) { s += hx[c >> 4]; s += hx[c & 15]; }
    return s;
}

// ------------------------------------------------------------------- the answers ----

int retry_after_header(const std::string& headers)
{
    // The header block libcurl hands over can hold more than one response (a 100 Continue, a
    // redirect); the last Retry-After is the one that belongs to the final answer.
    int         out = -1;
    size_t      pos = 0;
    const std::string low = lower(headers);
    for (;;) {
        const size_t at = low.find("retry-after:", pos);
        if (at == std::string::npos) break;
        if (at != 0 && low[at - 1] != '\n') { pos = at + 1; continue; }
        size_t i = at + 12;
        while (i < low.size() && (low[i] == ' ' || low[i] == '\t')) ++i;
        size_t j = i;
        while (j < low.size() && std::isdigit((unsigned char) low[j])) ++j;
        // Delta-seconds only. An HTTP-date is not something this service sends; ignore it.
        if (j > i && j - i <= 9) out = std::atoi(low.substr(i, j - i).c_str());
        pos = j;
    }
    return out;
}

Answer parse_answer(const HttpReply& reply, const std::string& host)
{
    Answer a;
    a.http_status  = reply.status;
    a.result.host  = host;
    a.retry_after  = retry_after_header(reply.headers);
    json j;
    try {
        if (!reply.body.empty()) j = json::parse(reply.body);
    } catch (...) { j = json(); }
    if (!j.is_object() || !j.contains("status")) {
        // No JSON from the service at all: DNS, TLS, a refused connection, a timeout - or Caddy's
        // bare 502/503 while the service itself is down. PROTOCOL.md: a transport failure, status 0.
        a.result.status = 0;
        if (!reply.error.empty() && reply.status == 0)
            a.result.error = "the push service could not be reached: " + one_line(reply.error, 200);
        else if (reply.status != 0)
            a.result.error = "the push service is not answering (HTTP " + std::to_string(reply.status) + ")";
        else
            a.result.error = "the push service did not answer";
        return a;
    }
    // Field by field and type-checked: a service that answers a number where a string belongs
    // must not throw out of here (nlohmann's value() does on a type mismatch).
    auto str = [&j](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
    auto flag = [&j](const char* k) { return j.contains(k) && j[k].is_boolean() && j[k].get<bool>(); };
    a.json          = true;
    a.raw           = reply.body;
    a.code          = str("error");
    a.message       = str("message");
    a.source        = str("source");
    a.result.ok     = flag("ok");
    a.result.status = j["status"].is_number_integer() ? j["status"].get<int>() : reply.status;
    // Owner decision (2026-09-23): APNs BadDeviceToken / DeviceTokenNotForTopic are errors, never
    // "gone". They are usually a sandbox/production mix-up, and pruning would silently lose a
    // phone that is alive. Enforced here as well as on the service so an older service cannot
    // make this hub prune on them.
    const bool never_gone = a.code == "BadDeviceToken" || a.code == "DeviceTokenNotForTopic";
    a.result.gone = flag("gone") && a.source != "forwarder" && !never_gone;
    // The service owns its credentials: an upstream auth problem is its problem, never a reason
    // for this hub to re-mint anything.
    a.result.credential_expired = false;
    if (!a.result.ok) {
        a.result.error = a.code.empty() ? ("HTTP " + std::to_string(a.result.status)) : one_line(a.code, 80);
        if (!a.message.empty()) a.result.error += ": " + one_line(a.message, 240);
    }
    const std::string h = str("host");
    if (!h.empty()) a.result.host = one_line(h, 100);
    if (j.contains("retry_after") && j["retry_after"].is_number()) a.retry_after = (int) std::min(86400.0, std::max(0.0, j["retry_after"].get<double>()));
    if (j.contains("server_time") && j["server_time"].is_number_integer()) a.server_time = j["server_time"].get<long long>();
    return a;
}

Next next_step(const Answer& a)
{
    if (!a.json) return Next::Later; // no answer at all: the service is down or unreachable
    if (a.result.ok) return Next::Done;
    if (a.result.gone) return Next::Prune;
    const int          st = a.result.status;
    const std::string& c  = a.code;
    if (a.source == "apns" || a.source == "fcm") {
        // Apple or Google answered: 429 and 5xx come right later, the other 4xx never do.
        if (st == 429 || st >= 500) return Next::Later;
        return Next::Drop;
    }
    // The forwarder refused or failed on its own account.
    if (st == 401) {
        if (c == "unknown_hub") return Next::RetryWithKey;
        if (c == "stale_timestamp") return Next::RetryWithClock;
        return Next::Bug; // bad_signature, bad_hub_id: identity or signing is wrong on this side
    }
    if (st == 403) return c == "suspended" ? Next::Suspended : Next::Drop;
    if (st == 405) return Next::Bug;
    if (st == 409) return Next::RetryNonce;
    if (st == 429) {
        if (c == "daily_quota" || c == "device_limit") return Next::DropQuota;
        return Next::Later; // rate_limited (the burst), registration_limit
    }
    if (st == 503 && c == "credentials_not_installed") return Next::DropQuota;
    if (st >= 500) return Next::Later; // upstream_unreachable, overloaded, busy, internal, ...
    return Next::Drop;                 // 400, 404 not_found, 413, anything unexpected
}

const char* next_name(Next n)
{
    switch (n) {
    case Next::Done: return "done";
    case Next::Prune: return "prune";
    case Next::RetryWithKey: return "retry_with_key";
    case Next::RetryWithClock: return "retry_with_clock";
    case Next::RetryNonce: return "retry_nonce";
    case Next::Later: return "later";
    case Next::Drop: return "drop";
    case Next::DropQuota: return "drop_quota";
    case Next::Suspended: return "suspended";
    case Next::Bug: return "bug";
    }
    return "?";
}

long long backoff_ms(int retry_index, double jitter)
{
    static const long long STEPS[] = { 5000, 30000, 120000, 300000 };
    const long long base = STEPS[std::max(0, std::min(retry_index, 3))];
    jitter = std::max(-1.0, std::min(1.0, jitter));
    return (long long) std::llround((double) base * (1.0 + 0.2 * jitter));
}

// ------------------------------------------------------------------- the queue ----

void Queue::push(Queued q, std::vector<Queued>& evicted)
{
    m_items.push_back(std::move(q));
    while ((int) m_items.size() > QUEUE_MAX) {
        evicted.push_back(std::move(m_items.front()));
        m_items.pop_front();
    }
}

std::vector<Queued> Queue::take_due(long long now_ms, std::vector<Queued>& expired)
{
    std::vector<Queued> due;
    std::deque<Queued>  keep;
    for (Queued& q : m_items) {
        if (now_ms >= q.deadline_ms()) expired.push_back(std::move(q));
        else if (now_ms >= q.next_ms) due.push_back(std::move(q));
        else keep.push_back(std::move(q));
    }
    m_items.swap(keep);
    return due;
}

long long Queue::next_due() const
{
    long long n = 0;
    for (const Queued& q : m_items) {
        const long long t = std::min(q.next_ms, q.deadline_ms());
        if (n == 0 || t < n) n = t;
    }
    return n;
}

std::vector<Queued> Queue::drain()
{
    std::vector<Queued> all(std::make_move_iterator(m_items.begin()), std::make_move_iterator(m_items.end()));
    m_items.clear();
    return all;
}

// ------------------------------------------------------------------- the hooks ----

Hooks default_hooks()
{
    Hooks h;
    h.transport = http_transport();
    h.clock     = []() {
        return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    };
    h.log = [](bool warning, const std::string& line) {
        if (warning) BOOST_LOG_TRIVIAL(warning) << line;
        else BOOST_LOG_TRIVIAL(info) << line;
    };
    h.nonce  = random_nonce;
    h.jitter = []() {
        static std::mutex    mtx;
        static std::mt19937  rng { std::random_device {}() };
        std::lock_guard<std::mutex> lock(mtx);
        return std::uniform_real_distribution<double>(-1.0, 1.0)(rng);
    };
    h.worker = true;
    return h;
}

// ------------------------------------------------------------------- the provider ----

HostedProvider::HostedProvider(Hooks hooks) : m_hooks(std::move(hooks)), m_url(DEFAULT_URL)
{
    const Hooks d = (m_hooks.transport && m_hooks.clock && m_hooks.log && m_hooks.nonce && m_hooks.jitter) ? Hooks() : default_hooks();
    if (!m_hooks.transport) m_hooks.transport = d.transport;
    if (!m_hooks.clock) m_hooks.clock = d.clock;
    if (!m_hooks.log) m_hooks.log = d.log;
    if (!m_hooks.nonce) m_hooks.nonce = d.nonce;
    if (!m_hooks.jitter) m_hooks.jitter = d.jitter;
}

HostedProvider::~HostedProvider() { stop(); }

bool HostedProvider::available(std::string& why) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!url_allowed(m_url, why)) return false;
    if (!m_id.valid()) {
        why = "this hub has no identity key yet, so it cannot sign requests to the push service";
        return false;
    }
    if (m_suspended) {
        why = "the EdgeSlicer push service has suspended this hub" +
              (m_suspended_reason.empty() ? std::string() : (": " + m_suspended_reason));
        return false;
    }
    return true;
}

void HostedProvider::configure(const std::string& config_json)
{
    std::string url;
    try {
        const json c = json::parse(config_json);
        if (c.is_object()) url = c.value("url", std::string());
    } catch (...) {}
    url = normalise_url(url);
    if (url.empty()) url = DEFAULT_URL;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stop = false; // a provider stopped with its hub can be started again with it
    if (url != m_url) {
        // A different service knows nothing of what the last one said about this hub.
        m_suspended = false;
        m_suspended_reason.clear();
        m_quota_blocked_until = 0;
        m_quota_note.clear();
        m_bug.clear();
        m_health_json.clear();
        m_quota_json.clear();
        m_health_at = m_quota_at = 0;
        m_offset_s = 0;
    }
    m_url = url;
}

void HostedProvider::set_identity(const Identity& id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_id = id;
}

void HostedProvider::set_result_sink(ResultSink sink)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sink = std::move(sink);
}

std::string HostedProvider::url() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_url;
}

long long HostedProvider::clock_offset_s() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_offset_s;
}

size_t HostedProvider::queued() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

// No log line or stored error may carry a device token or the ciphertext, whatever the service or
// libcurl chose to put in a message. (The service strips the token itself; this is the belt.)
std::string HostedProvider::scrub(std::string text, const PushRequest* req) const
{
    if (!req) return text;
    for (const std::string* secret : { &req->device_token, &req->ciphertext_b64u, &req->thread_id }) {
        if (secret->size() < 6) continue;
        for (size_t at = text.find(*secret); at != std::string::npos; at = text.find(*secret, at + 3))
            text.replace(at, secret->size(), "***");
    }
    return text;
}

// One signed POST. A fresh ts and nonce every time, signed over exactly the bytes posted.
Answer HostedProvider::post_once(const std::string& op, const PushRequest* req, bool with_key)
{
    std::string url;
    Identity    id;
    long long   offset;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        url    = m_url;
        id     = m_id;
        offset = m_offset_s;
    }
    const std::string host = host_of(url);
    Answer            a;
    a.result.host = host;
    std::string why;
    if (!url_allowed(url, why)) { a.result.error = why; return a; }
    if (!id.valid()) { a.result.error = "this hub has no identity key yet"; return a; }

    const long long   ts    = m_hooks.clock() / 1000 + offset;
    const std::string nonce = m_hooks.nonce();
    const std::string body  = (op == "send" && req) ? body_for_send(*req, ts, nonce)
                                                    : body_for_op(op, ts, nonce, id.public_hex);
    RemoteHub::Testing::HubIdentity hid;
    hid.hubid       = id.hubid;
    hid.public_hex  = id.public_hex;
    hid.private_hex = id.private_hex;
    std::string sig;
    if (!RemoteHub::identity_sign(hid, body, sig)) {
        a.result.error = "this hub could not sign the request (Ed25519 unavailable)";
        return a;
    }
    HttpCall call;
    call.method = "POST";
    call.url    = url + "/v1/" + op;
    call.body   = body;
    call.headers.emplace_back("Content-Type", "application/json");
    call.headers.emplace_back("X-Hub-Id", id.hubid);
    call.headers.emplace_back("X-Hub-Sig", sig);
    if (with_key) call.headers.emplace_back("X-Hub-Key", id.public_hex);
    HttpReply rep = m_hooks.transport(call);
    // Scrubbed before anything is parsed or truncated, so no partial token can survive either.
    rep.error = scrub(rep.error, req);
    rep.body  = scrub(rep.body, req);
    a = parse_answer(rep, host);
    a.result.error = scrub(a.result.error, req);
    a.message      = scrub(a.message, req);
    return a;
}

// post_once plus the protocol's three retry-once rules. Anything else goes back to the caller.
Answer HostedProvider::call(const std::string& op, const PushRequest* req)
{
    // send and quota carry X-Hub-Key, so an unknown hub is registered on first use; register has
    // the key in its body; unregister must never register.
    bool   with_key = op == "send" || op == "quota";
    bool   tried_key = false, tried_clock = false, tried_nonce = false;
    Answer a;
    for (;;) {
        a            = post_once(op, req, with_key);
        const Next n = next_step(a);
        if (n == Next::RetryWithKey && !tried_key && op != "unregister" && op != "register") {
            tried_key = true;
            if (with_key) {
                // The service did not take the key from the header (auto-registration off):
                // register explicitly, then try once more.
                const Answer reg = post_once("register", nullptr, false);
                note("register", reg);
            }
            with_key = true;
            continue;
        }
        if (n == Next::RetryWithClock && !tried_clock && a.server_time > 0) {
            tried_clock = true;
            const long long local = m_hooks.clock() / 1000;
            const long long off   = a.server_time - local;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_offset_s = off;
            }
            m_hooks.log(std::llabs(off) > 60,
                        "AppPush hosted: this PC's clock is " + std::to_string(off) +
                            " s off the push service's; correcting for it");
            continue;
        }
        if (n == Next::RetryNonce && !tried_nonce) {
            tried_nonce = true;
            continue;
        }
        break;
    }
    note(op, a);
    return a;
}

// Fold an answer into what the hub page shows, and log it (never the token, never a body).
void HostedProvider::note(const std::string& op, const Answer& a)
{
    const long long now = m_hooks.clock();
    const Next      n   = next_step(a);
    std::string     line;
    bool            warn = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (a.result.ok) {
            m_last_ok_at = now;
            if (op == "send") m_quota_note.clear(); // a delivery proves the quota is not used up
        } else {
            m_last_error    = a.result.error;
            m_last_status   = a.json ? a.result.status : a.http_status;
            m_last_error_at = now;
        }
        switch (n) {
        case Next::Suspended:
            m_suspended        = true;
            m_suspended_reason = a.message;
            break;
        case Next::Bug:
            m_bug = a.code == "bad_signature" || a.code == "bad_hub_id"
                        ? "The push service could not verify this hub's signature (" + a.code +
                              "). This is a bug in EdgeSlicer, not something to fix here."
                        : "The push service refused the request as malformed (" + a.result.error + ").";
            break;
        case Next::DropQuota:
            m_quota_note    = a.result.error;
            m_quota_note_at = now;
            if (a.code == "daily_quota" && a.retry_after > 0)
                m_quota_blocked_until = now + (long long) a.retry_after * 1000;
            break;
        case Next::Drop:
            if (a.source == "forwarder" && a.result.status == 404)
                m_bug = "The push service URL does not look right: " + m_url + " answered 404.";
            break;
        default: break;
        }
        // Any accepted request proves the URL and the signing are right.
        if (a.result.ok) m_bug.clear();
        if ((op == "quota" || op == "register") && a.json) {
            m_quota_json = a.raw;
            m_quota_at   = now;
            try {
                const json q = json::parse(a.raw);
                if (q.value("ok", false)) {
                    m_suspended = q.value("suspended", false);
                    m_suspended_reason = m_suspended ? q.value("reason", std::string()) : std::string();
                    const long long remaining = q.value("remaining", 0LL);
                    if (remaining != 0) m_quota_blocked_until = 0;
                    if (remaining != 0 && m_quota_note.compare(0, 11, "daily_quota") == 0) m_quota_note.clear();
                }
            } catch (...) {}
        }
        if (a.result.ok) line = "AppPush hosted: " + op + " -> " + std::to_string(a.result.status) +
                                (op == "send" ? " via " + a.result.host : std::string());
        else {
            warn = true;
            line = "AppPush hosted: " + op + " failed (" +
                   (a.json ? "HTTP " + std::to_string(a.result.status) : std::string("no answer")) + ", " +
                   next_name(n) + "): " + a.result.error;
        }
    }
    m_hooks.log(warn, line);
}

PushResult HostedProvider::send(const PushRequest& req)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_suspended) {
            PushResult r;
            r.status = 403;
            r.host   = host_of(m_url);
            r.error  = "suspended: the push service has suspended this hub" +
                      (m_suspended_reason.empty() ? std::string() : (" (" + m_suspended_reason + ")"));
            return r;
        }
        if (m_quota_blocked_until > m_hooks.clock()) {
            // The daily quota is used up and the service said until when; asking again before
            // then would only spend its time and ours.
            PushResult r;
            r.status = 429;
            r.host   = host_of(m_url);
            r.error  = "daily_quota: this hub's daily push quota is used up (" + m_quota_note + ")";
            return r;
        }
    }
    return call("send", &req).result;
}

PushResult HostedProvider::deliver(const std::string& device_id, const PushRequest& req, bool& queued)
{
    queued = false;
    bool short_circuit;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        short_circuit = m_suspended || m_quota_blocked_until > m_hooks.clock();
    }
    if (short_circuit) return send(req); // the local refusal, no request made
    const long long now = m_hooks.clock();
    const Answer    a   = call("send", &req);
    if (next_step(a) != Next::Later) return a.result;

    Queued q;
    q.device_id   = device_id;
    q.req         = req;
    q.first_ms    = now;
    q.expires_ms  = now + (long long) std::max(0, req.ttl_seconds) * 1000;
    q.retries     = 0;
    q.last_error  = a.result.error;
    q.last_status = a.result.status;
    const long long wait = std::max(backoff_ms(0, m_hooks.jitter()), (long long) std::max(0, a.retry_after) * 1000);
    q.next_ms = m_hooks.clock() + wait;
    if (q.next_ms >= q.deadline_ms()) {
        // Retry-After (or the first backoff step) is longer than this notification has left to
        // live: a "finished" that arrives after its TTL is noise. Drop it now.
        PushResult r = a.result;
        r.error += " (not retried: the notification would expire first)";
        m_hooks.log(true, "AppPush hosted: not queueing a notification; " + r.error);
        return r;
    }
    std::vector<Queued> evicted;
    size_t              depth;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(std::move(q), evicted);
        depth = m_queue.size();
    }
    queued = true;
    m_hooks.log(true, "AppPush hosted: the push service is unavailable (" + a.result.error + "); " +
                          std::to_string(depth) + " notification(s) waiting, next try in " +
                          std::to_string(wait / 1000) + " s");
    for (const Queued& old : evicted) {
        PushResult r;
        r.status = old.last_status;
        r.host   = host_of(url());
        r.error  = "dropped: " + std::to_string(QUEUE_MAX) + " newer notifications were already waiting (" + old.last_error + ")";
        finish(old, r);
    }
    ensure_worker();
    m_cv.notify_all();
    return a.result;
}

void HostedProvider::finish(const Queued& q, const PushResult& r)
{
    ResultSink sink;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        sink = m_sink;
        if (!r.ok && !r.gone) ++m_dropped;
    }
    if (sink) sink(q.device_id, r);
}

void HostedProvider::pump()
{
    std::vector<Queued> due, expired;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        due = m_queue.take_due(m_hooks.clock(), expired);
    }
    for (const Queued& q : expired) {
        PushResult r;
        r.status = q.last_status;
        r.host   = host_of(url());
        r.error  = "gave up after " + std::to_string((m_hooks.clock() - q.first_ms) / 60000) +
                  " min waiting for the push service (" + q.last_error + ")";
        m_hooks.log(true, "AppPush hosted: " + scrub(r.error, &q.req));
        finish(q, r);
    }
    for (Queued& q : due) {
        if (m_stop) return;
        bool blocked;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            blocked = m_suspended || m_quota_blocked_until > m_hooks.clock();
        }
        const Answer a = blocked ? Answer() : call("send", &q.req);
        const Next   n = blocked ? Next::DropQuota : next_step(a);
        if (n != Next::Later) {
            PushResult r = a.result;
            if (blocked) r = send(q.req); // the local refusal, for its wording
            finish(q, r);
            continue;
        }
        ++q.retries;
        q.last_error  = a.result.error;
        q.last_status = a.result.status;
        const long long now  = m_hooks.clock();
        const long long wait = std::max(backoff_ms(q.retries, m_hooks.jitter()),
                                        (long long) std::max(0, a.retry_after) * 1000);
        q.next_ms = now + wait;
        if (q.next_ms >= q.deadline_ms()) {
            PushResult r = a.result;
            r.error = "gave up after " + std::to_string((now - q.first_ms) / 60000) +
                      " min waiting for the push service (" + a.result.error + ")";
            m_hooks.log(true, "AppPush hosted: " + scrub(r.error, &q.req));
            finish(q, r);
            continue;
        }
        std::vector<Queued> evicted;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push(std::move(q), evicted);
        }
        for (const Queued& old : evicted) {
            PushResult r;
            r.status = old.last_status;
            r.host   = host_of(url());
            r.error  = "dropped: " + std::to_string(QUEUE_MAX) + " newer notifications were already waiting (" + old.last_error + ")";
            finish(old, r);
        }
    }
}

void HostedProvider::ensure_worker()
{
    if (!m_hooks.worker) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_worker_running || m_stop) return;
    m_worker_running = true;
    m_worker         = std::thread([this]() { worker_main(); });
}

void HostedProvider::worker_main()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_stop) {
        const long long next = m_queue.next_due();
        const long long now  = m_hooks.clock();
        // Wake at the next due time, but at least once a second so a changed clock or a stop is
        // noticed promptly; an empty queue just idles.
        long long wait = next == 0 ? 1000 : std::max(0LL, std::min(1000LL, next - now));
        m_cv.wait_for(lock, std::chrono::milliseconds(wait));
        if (m_stop) break;
        if (m_queue.size() == 0 || m_queue.next_due() > m_hooks.clock()) continue;
        lock.unlock();
        try { pump(); } catch (...) {}
        lock.lock();
    }
}

void HostedProvider::stop()
{
    std::thread worker;
    std::vector<Queued> left;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
        worker.swap(m_worker);
        m_worker_running = false;
        left = m_queue.drain(); // forgotten, not persisted: they hold tokens and ciphertext
    }
    m_cv.notify_all();
    if (worker.joinable()) worker.join();
    if (!left.empty()) m_hooks.log(false, "AppPush hosted: " + std::to_string(left.size()) +
                                              " queued notification(s) dropped on shutdown");
}

std::string HostedProvider::check(bool do_register, bool with_quota)
{
    std::string url, why;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        url = m_url;
    }
    if (url_allowed(url, why)) {
        // /healthz is anonymous: no signature, no hub id - it says whether the service is up and
        // whether it can forward to Apple and Google at all.
        HttpCall hc;
        hc.method = "GET";
        hc.url    = url + "/healthz";
        const HttpReply r = m_hooks.transport(hc);
        json            h;
        try {
            if (!r.body.empty()) h = json::parse(r.body);
        } catch (...) { h = json(); }
        if (!h.is_object()) {
            h          = json::object();
            h["ok"]    = false;
            h["error"] = r.status == 0 ? ("the push service could not be reached" +
                                          (r.error.empty() ? std::string() : (": " + one_line(r.error, 200))))
                                       : ("the push service is not answering (HTTP " + std::to_string(r.status) + ")");
        }
        h["http_status"] = r.status;
        if (h.contains("time") && h["time"].is_number_integer())
            h["clock_skew_s"] = h["time"].get<long long>() - m_hooks.clock() / 1000;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_health_json = h.dump();
        m_health_at   = m_hooks.clock();
    }
    if (do_register) call("register", nullptr);
    if (with_quota) call("quota", nullptr);
    return status_json();
}

std::string HostedProvider::unregister()
{
    const Answer a = call("unregister", nullptr);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (a.result.ok) {
            m_quota_json.clear();
            m_quota_at = 0;
        }
    }
    json j;
    j["ok"]     = a.result.ok;
    j["status"] = a.result.status;
    j["error"]  = a.result.ok ? std::string() : a.result.error;
    try {
        j["hosted"] = json::parse(status_json());
    } catch (...) {}
    return j.dump();
}

std::string HostedProvider::status_json() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    json        j;
    std::string why;
    bool        ok = url_allowed(m_url, why);
    if (ok && !m_id.valid()) { ok = false; why = "this hub has no identity key yet"; }
    if (ok && m_suspended) { ok = false; why = "suspended"; }
    j["url"]        = m_url;
    j["hubid"]      = m_id.hubid; // not a secret: it is derived from the public key
    j["available"]  = ok;
    j["why"]        = ok ? std::string() : why;
    j["suspended"]  = m_suspended;
    j["suspended_reason"] = m_suspended_reason;
    j["queued"]     = (int) m_queue.size();
    j["queue_max"]  = QUEUE_MAX;
    j["dropped"]    = m_dropped;
    j["clock_offset_s"] = m_offset_s;
    j["clock_warning"]  = std::llabs(m_offset_s) > 60;
    j["quota_note"]     = m_quota_note;
    j["quota_note_at"]  = m_quota_note_at;
    j["quota_blocked_until"] = m_quota_blocked_until;
    j["bug"]            = m_bug;
    j["last_error"]     = m_last_error;
    j["last_status"]    = m_last_status;
    j["last_error_at"]  = m_last_error_at;
    j["last_ok_at"]     = m_last_ok_at;
    json health = json::object(), quota = json::object();
    try { if (!m_health_json.empty()) health = json::parse(m_health_json); } catch (...) {}
    try { if (!m_quota_json.empty()) quota = json::parse(m_quota_json); } catch (...) {}
    // Only the fields the page draws; everything the service says is public anyway, but a stray
    // field is not something to pass on without looking at it.
    json q = json::object();
    for (const char* k : { "ok", "status", "error", "message", "tier", "suspended", "reason", "day", "used", "limit",
                           "remaining", "resets_at", "burst_per_minute", "max_devices_per_day", "apns", "fcm", "new" })
        if (quota.contains(k)) q[k] = quota[k];
    j["health"]    = health;
    j["health_at"] = m_health_at;
    j["quota"]     = q;
    j["quota_at"]  = m_quota_at;
    return j.dump();
}

std::unique_ptr<HostedProvider> make_hosted_provider(Hooks hooks)
{
    return std::unique_ptr<HostedProvider>(new HostedProvider(std::move(hooks)));
}

} // namespace Hosted
} // namespace AppPush
} // namespace GUI
} // namespace Slic3r
