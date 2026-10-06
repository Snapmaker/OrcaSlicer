#pragma once

// The third push provider: the EdgeSlicer push service ("hosted" mode).
//
// Why it exists. A hub can only reach APNs or FCM with a signing credential for the app it pushes
// to - an Apple .p8 or a Firebase service account - and those belong to whoever publishes the app.
// They never ship (see AppPush.hpp). So a hub without its own keys, which is every hub but the
// owner's, hands each already-encrypted notification to https://push.edgeslicer.com, which holds
// the credentials and passes the notification on. Protocol: edgeslicer-push/docs/PROTOCOL.md.
//
// What the service sees. Exactly what Apple and Google see today, plus this hub's id: the device
// token, the size, the hashed collapse id, a printer-id thread hint, priority and TTL, and the
// ciphertext - which was encrypted on this PC to a key only the phone holds. The service builds the
// same APNs/FCM envelope ApnsProvider/FcmProvider build, byte for byte, so the app is unchanged.
//
// How a request is authenticated. Every POST is signed with this hub's Ed25519 identity (the key
// the hubid is derived from; RemoteHub::identity_sign) over the exact body bytes, and carries a
// fresh timestamp and nonce - so every retry is a new request, re-signed, never a re-post. The
// public key rides along as X-Hub-Key on send and quota, so an unknown hub is registered on first
// use and no separate registration round trip is ever needed.
//
// What is never logged. Device tokens, ciphertext, request bodies. Log lines name the operation,
// the status, the error code and the service's message (which the service has already stripped of
// the token), and every line is scrubbed of the token and ciphertext on top of that.
//
// Everything below the provider class is pure and deterministic given its inputs - the clock, the
// nonce, the jitter and the transport are all injectable - so tests/slic3rutils/hosted_push_tests.cpp
// can drive the retry schedule, the queue and the result mapping without a socket or a real clock.

#include "AppPushProvider.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace AppPush {
namespace Hosted {

extern const char* const DEFAULT_URL; // "https://push.edgeslicer.com"

// The retry schedule (plan section 3.5): 5 s, 30 s, 2 min, then every 5 min, each +-20 % jitter;
// give up after 30 minutes or at the notification's own TTL, whichever comes first.
static const int       QUEUE_MAX  = 32;
static const long long GIVE_UP_MS = 30LL * 60 * 1000;

// This hub's signing identity: the same three strings RemoteHub keeps (hubid = 16 lowercase hex,
// the public key and the private seed as 64 hex each).
struct Identity
{
    std::string hubid, public_hex, private_hex;
    bool        valid() const { return hubid.size() == 16 && public_hex.size() == 64 && private_hex.size() == 64; }
};

// ------------------------------------------------------------------- transport ----

struct HttpCall
{
    std::string                                      method; // "POST" or "GET"
    std::string                                      url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string                                      body;
};

struct HttpReply
{
    int         status { 0 }; // 0 = no HTTP answer at all (DNS, TLS, refused, timed out)
    std::string body;
    std::string headers;      // the raw response header block, for Retry-After
    std::string error;        // the transport's own error text, if any
};

using Transport = std::function<HttpReply(const HttpCall&)>;

// The real one: the fork's Http, 5 s to connect, 20 s in all, and none of the slicer's global
// client headers (X-BBL-*): the service learns this hub's id and nothing else about the PC.
Transport http_transport();

// https:// always; http:// only to a loopback host and only with SNORCA_DEBUG_ROUTES=1 (the gate's
// mock forwarder). Anything else is refused with a sentence in `why`.
bool url_allowed(const std::string& url, std::string& why);

// ------------------------------------------------------------------- the bodies ----

// The exact bytes that are signed and posted. Built once per attempt and never re-serialised.
// send: v, op, ts, nonce, platform, env (APNs only), token, bundle, e, collapse, thread, priority, ttl.
std::string body_for_send(const PushRequest& req, long long ts, const std::string& nonce);
// register / quota / unregister: v, op, ts, nonce (+ pubkey for register).
std::string body_for_op(const std::string& op, long long ts, const std::string& nonce, const std::string& pubkey_hex = "");

// 32 lowercase hex characters from the OS CSPRNG.
std::string random_nonce();

// ------------------------------------------------------------------- the answers ----

// One answer from the service, mapped onto PushResult the way PROTOCOL.md's "Mapping into
// PushResult" says, plus what the retry logic and the hub page need beside it.
struct Answer
{
    PushResult  result;           // ok, status (0 = no JSON), gone, credential_expired (always false), error, host
    bool        json { false };   // the service answered with its JSON at all
    int         http_status { 0 };// the raw HTTP status, even where result.status is 0 (a bare Caddy 502)
    std::string code;             // the "error" field: unknown_hub, daily_quota, Unregistered, ...
    std::string message;
    std::string source;           // "apns" / "fcm" (the upstream answered) or "forwarder"
    int         retry_after { -1 };  // seconds, from the body or the Retry-After header; -1 = none
    long long   server_time { 0 };   // on stale_timestamp only
    std::string raw;              // the JSON body (quota / register fields); never logged
};

Answer parse_answer(const HttpReply& reply, const std::string& host);

// The Retry-After header's value in seconds (delta-seconds only), or -1.
int retry_after_header(const std::string& headers);

// What to do with an answer.
enum class Next {
    Done,           // delivered
    Prune,          // the platform says the device is dead: the row goes
    RetryWithKey,   // 401 unknown_hub: register (X-Hub-Key or /v1/register), retry once
    RetryWithClock, // 401 stale_timestamp: correct the clock by server_time, retry once
    RetryNonce,     // 409 replay: retry once with a fresh nonce
    Later,          // no answer, 429 rate_limited, 5xx: queue it and honour Retry-After
    Drop,           // 400 / 404 / 413 / upstream 4xx: do not retry, do not prune
    DropQuota,      // 429 daily_quota / device_limit, 503 credentials_not_installed: drop, show on the page
    Suspended,      // 403 suspended: stop sending
    Bug             // 401 bad_signature / bad_hub_id, 405: our bug; do not retry, say so
};
Next        next_step(const Answer& a);
const char* next_name(Next n);

// The delay before retry number `retry_index` (0 = the first retry): 5 s, 30 s, 2 min, then 5 min,
// scaled by (1 + 0.2 * jitter) with jitter in [-1, 1].
long long backoff_ms(int retry_index, double jitter);

// ------------------------------------------------------------------- the queue ----

// One notification waiting for the service. Held in memory only: it carries a device token and
// ciphertext, and a hub restart is a perfectly good reason to forget both.
struct Queued
{
    std::string device_id;
    PushRequest req;
    long long   first_ms { 0 };   // when the first attempt was made
    long long   expires_ms { 0 }; // first_ms + TTL: past this the notification is noise
    long long   next_ms { 0 };    // when to try next
    int         retries { 0 };    // retries made so far (0 = only the first attempt)
    std::string last_error;
    int         last_status { 0 };

    // The last moment a retry may be made: the TTL or 30 minutes after the first try.
    long long deadline_ms() const { return std::min(expires_ms, first_ms + GIVE_UP_MS); }
};

class Queue
{
public:
    // Adds one; if that makes more than QUEUE_MAX, the oldest are moved into `evicted`.
    void push(Queued q, std::vector<Queued>& evicted);
    // Removes and returns everything due at `now_ms`; whatever expired meanwhile goes to `expired`.
    std::vector<Queued> take_due(long long now_ms, std::vector<Queued>& expired);
    long long           next_due() const; // 0 when empty
    size_t              size() const { return m_items.size(); }
    std::vector<Queued> drain();          // everything, oldest first
    // Removes everything queued for that device ("" = every device); answers how many went.
    size_t              forget(const std::string& device_id);
private:
    std::deque<Queued> m_items; // oldest first
};

// ------------------------------------------------------------------- the provider ----

using Clock      = std::function<long long()>;                    // unix milliseconds
using Log        = std::function<void(bool warning, const std::string& line)>;
using Nonce      = std::function<std::string()>;
using Jitter     = std::function<double()>;                       // uniform in [-1, 1]
using ResultSink = std::function<void(const std::string& device_id, const PushResult& result)>;

struct Hooks
{
    Transport transport;
    Clock     clock;
    Log       log;
    Nonce     nonce;
    Jitter    jitter;
    bool      worker { true }; // false in the tests: they call pump() themselves with a fake clock
};

// The production hooks: Http, the system clock, Boost.Log, RAND_bytes, a Mersenne twister.
Hooks default_hooks();

class HostedProvider : public Provider
{
public:
    explicit HostedProvider(Hooks hooks);
    ~HostedProvider() override;

    const char* name() const override { return "hosted"; }
    bool        available(std::string& why) const override;
    // {"url": "..."}; an empty or missing url means the default service.
    void        configure(const std::string& config_json) override;
    // One notification, now: one attempt plus the protocol's retry-once rules (unknown_hub,
    // stale_timestamp, replay). No queue - this is what the hub page's test button uses.
    PushResult  send(const PushRequest& req) override;

    void set_identity(const Identity& id);
    // Where a queued notification's final outcome goes (delivered, gone, or given up on).
    void set_result_sink(ResultSink sink);

    // The event path: send() once, and if the answer says "later", queue it instead of failing.
    // `queued` true means the outcome will arrive through the result sink.
    PushResult deliver(const std::string& device_id, const PushRequest& req, bool& queued);

    // One pass over the queue at the hooks' current time. The worker thread calls this; the tests
    // call it directly after moving their fake clock.
    void pump();

    // The hub page's "Check": GET /healthz, optionally POST /v1/register and /v1/quota. Answers the
    // status JSON below.
    std::string check(bool do_register, bool with_quota);
    // POST /v1/unregister: the service forgets this hub and its counters. Answers
    // {"ok":..,"status":..,"error":..} plus the status JSON under "hosted".
    std::string unregister();

    // For GET /hub/apppush: the URL, whether the service is usable and why not, the last health
    // and quota answers, a suspension or quota banner, the queue depth, the clock correction and
    // the last error. Nothing secret: no token, no key, no body.
    std::string status_json() const;

    size_t    queued() const;
    long long clock_offset_s() const;
    std::string url() const;

    // A device row was removed (the hub page, the app unpairing, or a new phone link): drop what is
    // still queued for it, so a removed phone gets nothing more ("" = every device). A send already
    // in flight on the worker cannot be recalled. Answers how many were dropped.
    size_t forget(const std::string& device_id);

    // Stop the worker and forget the queue (a hub that is quitting keeps nothing).
    void stop();

private:
    Answer call(const std::string& op, const PushRequest* req);
    Answer post_once(const std::string& op, const PushRequest* req, bool with_key);
    void   note(const std::string& op, const Answer& a);
    void   finish(const Queued& q, const PushResult& r);
    void   ensure_worker();
    void   worker_main();
    std::string scrub(std::string text, const PushRequest* req) const;

    Hooks                   m_hooks;
    mutable std::mutex      m_mutex;
    std::string             m_url;
    Identity                m_id;
    long long               m_offset_s { 0 };
    ResultSink              m_sink;
    Queue                   m_queue;

    // What the page shows. Plain values, all non-secret.
    bool        m_suspended { false };
    std::string m_suspended_reason;
    long long   m_quota_blocked_until { 0 }; // daily quota used: no request until then (ms)
    std::string m_quota_note;                // daily_quota / device_limit / credentials_not_installed
    long long   m_quota_note_at { 0 };
    std::string m_bug;                       // bad_signature / bad_hub_id / wrong URL
    std::string m_last_error;
    int         m_last_status { 0 };
    long long   m_last_error_at { 0 };
    long long   m_last_ok_at { 0 };
    std::string m_health_json;               // last /healthz answer (or {"error":..})
    long long   m_health_at { 0 };
    std::string m_quota_json;                // last /v1/quota or /v1/register answer
    long long   m_quota_at { 0 };
    int         m_dropped { 0 };             // queued notifications given up on since start

    std::thread             m_worker;
    std::condition_variable m_cv;
    std::atomic<bool>       m_stop { false };
    bool                    m_worker_running { false };
};

std::unique_ptr<HostedProvider> make_hosted_provider(Hooks hooks = default_hooks());

} // namespace Hosted
} // namespace AppPush
} // namespace GUI
} // namespace Slic3r
