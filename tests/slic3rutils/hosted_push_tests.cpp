// The hosted push provider (src/slic3r/GUI/HostedPush.hpp): the hub side of the EdgeSlicer push
// service, edgeslicer-push/docs/PROTOCOL.md.
//
// Everything but the last section runs with an injected transport, clock, nonce and jitter: no
// socket, no real time, no push service. The last section posts through the real Http to a mock
// forwarder on loopback (never the real service).

#include <catch2/catch.hpp>

#include "slic3r/GUI/HostedPush.hpp"
#include "slic3r/GUI/RemoteHub.hpp"

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <ctime>
#include <vector>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::AppPush;
using namespace Slic3r::GUI::AppPush::Hosted;
using json = nlohmann::json;

namespace {

// RFC 8032 section 7.1, TEST 1 and TEST 2.
const char* const RFC_SEED_1 = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
const char* const RFC_PUB_1  = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
const char* const RFC_SIG_1  = "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065"
                               "224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";
const char* const RFC_SEED_2 = "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
const char* const RFC_SIG_2  = "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
                               "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00";

std::string hex_to_b64url(const std::string& hex)
{
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::vector<unsigned char> b;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) b.push_back((unsigned char) std::stoi(hex.substr(i, 2), nullptr, 16));
    std::string out;
    for (size_t i = 0; i < b.size(); i += 3) {
        const unsigned v = (unsigned(b[i]) << 16) | (i + 1 < b.size() ? unsigned(b[i + 1]) << 8 : 0u) |
                           (i + 2 < b.size() ? unsigned(b[i + 2]) : 0u);
        out += A[(v >> 18) & 63];
        out += A[(v >> 12) & 63];
        if (i + 1 < b.size()) out += A[(v >> 6) & 63];
        if (i + 2 < b.size()) out += A[v & 63];
    }
    return out;
}

Identity test_identity()
{
    const RemoteHub::Testing::HubIdentity h = RemoteHub::Testing::identity_from_seed_hex(RFC_SEED_1);
    Identity id;
    id.hubid       = h.hubid;
    id.public_hex  = h.public_hex;
    id.private_hex = h.private_hex;
    return id;
}

const std::string TOKEN = "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90"; // 64 hex
const std::string CIPHER = "Q2lwaGVydGV4dC1ub2JvZHktY2FuLXJlYWQtdGhpcy1ibG9i";

PushRequest test_request(const std::string& platform = "apns", int ttl = 1800)
{
    PushRequest r;
    r.platform        = platform;
    r.device_token    = platform == "apns" ? TOKEN : "fcm-registration-token-000000000001";
    r.env             = "production";
    r.bundle          = "dev.acerage.ultra1";
    r.ciphertext_b64u = CIPHER;
    r.collapse_id     = "AbCdEfGhIjKlMnOpQrStUvWx";
    r.thread_id       = "printer-0042";
    r.priority        = 10;
    r.ttl_seconds     = ttl;
    return r;
}

HttpReply reply(int status, const std::string& body, const std::string& headers = "")
{
    HttpReply r;
    r.status  = status;
    r.body    = body;
    r.headers = headers;
    return r;
}

std::string fwd(int status, const std::string& code, const std::string& msg = "", const std::string& extra = "")
{
    return "{\"ok\":false,\"status\":" + std::to_string(status) + ",\"error\":\"" + code + "\",\"message\":\"" + msg +
           "\",\"source\":\"forwarder\"" + extra + "}";
}

std::string upstream(int status, const std::string& source, const std::string& code, bool gone, const std::string& msg = "")
{
    return "{\"ok\":false,\"status\":" + std::to_string(status) + ",\"gone\":" + (gone ? "true" : "false") +
           ",\"error\":\"" + code + "\",\"message\":\"" + msg + "\",\"source\":\"" + source +
           "\",\"host\":\"" + (source == "apns" ? "api.push.apple.com" : "fcm.googleapis.com") + "\"}";
}

const std::string OK_SEND = "{\"ok\":true,\"status\":200,\"source\":\"apns\",\"host\":\"api.push.apple.com\",\"upstream_status\":200}";

// A scripted service: every call is recorded, and `script` decides each answer.
struct Harness
{
    long long                                          now { 1789000000000LL };
    int                                                nonce_n { 0 };
    std::vector<HttpCall>                              calls;
    std::function<HttpReply(const HttpCall&, size_t)> script;
    std::vector<std::string>                           logs;
    std::vector<std::pair<std::string, PushResult>>    results;
    std::unique_ptr<HostedProvider>                    p;

    Harness()
    {
        Hooks h;
        h.transport = [this](const HttpCall& c) {
            calls.push_back(c);
            return script ? script(c, calls.size() - 1) : reply(200, OK_SEND);
        };
        h.clock  = [this]() { return now; };
        h.log    = [this](bool, const std::string& line) { logs.push_back(line); };
        h.nonce  = [this]() {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%032x", ++nonce_n);
            return std::string(buf);
        };
        h.jitter = []() { return 0.0; };
        h.worker = false;
        p        = make_hosted_provider(h);
        p->configure("{\"url\":\"https://push.example.test\"}");
        p->set_identity(test_identity());
        p->set_result_sink([this](const std::string& id, const PushResult& r) { results.emplace_back(id, r); });
    }

    json body(size_t i) const { return json::parse(calls.at(i).body); }
    std::string header(size_t i, const std::string& name) const
    {
        for (const auto& kv : calls.at(i).headers)
            if (kv.first == name) return kv.second;
        return "";
    }
    bool has_header(size_t i, const std::string& name) const
    {
        for (const auto& kv : calls.at(i).headers)
            if (kv.first == name) return true;
        return false;
    }
    bool signed_ok(size_t i) const
    {
        return RemoteHub::identity_verify(RFC_PUB_1, calls.at(i).body, header(i, "X-Hub-Sig"));
    }
};

} // namespace

// ======================================================================== signing ====

TEST_CASE("the RFC 8032 test-1 key has hub id 21fe31dfa154a261", "[HostedPush]")
{
    const auto id = RemoteHub::Testing::identity_from_seed_hex(RFC_SEED_1);
    REQUIRE(id.valid());
    CHECK(id.public_hex == RFC_PUB_1);
    CHECK(id.hubid == "21fe31dfa154a261");
    CHECK(RemoteHub::Testing::hubid_from_public_key_hex(RFC_PUB_1) == "21fe31dfa154a261");
}

TEST_CASE("identity_sign reproduces RFC 8032's signatures, as base64url without padding", "[HostedPush]")
{
    std::string sig;
    REQUIRE(RemoteHub::identity_sign(RemoteHub::Testing::identity_from_seed_hex(RFC_SEED_1), "", sig));
    CHECK(sig == hex_to_b64url(RFC_SIG_1));
    CHECK(sig.size() == 86); // 64 bytes, no '=' padding
    CHECK(sig.find('=') == std::string::npos);

    REQUIRE(RemoteHub::identity_sign(RemoteHub::Testing::identity_from_seed_hex(RFC_SEED_2), std::string("\x72", 1), sig));
    CHECK(sig == hex_to_b64url(RFC_SIG_2));
}

TEST_CASE("a signature covers exactly the bytes signed", "[HostedPush]")
{
    const auto        id   = RemoteHub::Testing::identity_from_seed_hex(RFC_SEED_1);
    const std::string body = body_for_send(test_request(), 1789000000, "9f86d081884c7d659a2feaa0c55ad015");
    std::string       sig;
    REQUIRE(RemoteHub::identity_sign(id, body, sig));
    CHECK(RemoteHub::identity_verify(RFC_PUB_1, body, sig));
    // Padding is tolerated on the way in, as the service tolerates it.
    CHECK(RemoteHub::identity_verify(RFC_PUB_1, body, sig + "=="));
    // One byte different, a re-serialisation with a space, or another key: all refused.
    std::string mutated = body;
    mutated[mutated.size() / 2] ^= 1;
    CHECK_FALSE(RemoteHub::identity_verify(RFC_PUB_1, mutated, sig));
    CHECK_FALSE(RemoteHub::identity_verify(RFC_PUB_1, json::parse(body).dump(1), sig));
    const auto other = RemoteHub::Testing::identity_from_seed_hex(RFC_SEED_2);
    CHECK_FALSE(RemoteHub::identity_verify(other.public_hex, body, sig));
    // A malformed identity cannot sign at all.
    RemoteHub::Testing::HubIdentity bad = id;
    bad.private_hex = "zz" + bad.private_hex.substr(2);
    CHECK_FALSE(RemoteHub::identity_sign(bad, body, sig));
    CHECK(sig.empty());
}

// ======================================================================== the bodies ====

TEST_CASE("the send body carries PushRequest straight through", "[HostedPush]")
{
    const json b = json::parse(body_for_send(test_request(), 1789000000, "9f86d081884c7d659a2feaa0c55ad015"));
    CHECK(b["v"] == 1);
    CHECK(b["op"] == "send");
    CHECK(b["ts"] == 1789000000);
    CHECK(b["nonce"] == "9f86d081884c7d659a2feaa0c55ad015");
    CHECK(b["platform"] == "apns");
    CHECK(b["env"] == "production");
    CHECK(b["token"] == TOKEN);
    CHECK(b["bundle"] == "dev.acerage.ultra1");
    CHECK(b["e"] == CIPHER);
    CHECK(b["collapse"] == "AbCdEfGhIjKlMnOpQrStUvWx");
    CHECK(b["thread"] == "printer-0042");
    CHECK(b["priority"] == 10);
    CHECK(b["ttl"] == 1800);
    CHECK(b.size() == 13);

    PushRequest s = test_request();
    s.env         = "sandbox";
    s.priority    = 5;
    s.ttl_seconds = 999999;
    const json sb = json::parse(body_for_send(s, 1, "00000000000000000000000000000001"));
    CHECK(sb["env"] == "sandbox");
    CHECK(sb["priority"] == 5);
    CHECK(sb["ttl"] == 86400); // the service's range

    // env is an APNs field; an FCM request does not carry one.
    const json fb = json::parse(body_for_send(test_request("fcm"), 1, "00000000000000000000000000000001"));
    CHECK(fb["platform"] == "fcm");
    CHECK_FALSE(fb.contains("env"));
}

TEST_CASE("register carries the public key; quota and unregister do not", "[HostedPush]")
{
    const json r = json::parse(body_for_op("register", 5, "00000000000000000000000000000001", RFC_PUB_1));
    CHECK(r["op"] == "register");
    CHECK(r["pubkey"] == RFC_PUB_1);
    CHECK_FALSE(json::parse(body_for_op("quota", 5, "00000000000000000000000000000001", RFC_PUB_1)).contains("pubkey"));
    CHECK_FALSE(json::parse(body_for_op("unregister", 5, "00000000000000000000000000000001", RFC_PUB_1)).contains("pubkey"));
    const std::string n = random_nonce();
    CHECK(n.size() == 32);
    CHECK(n.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(random_nonce() != n);
}

TEST_CASE("a send is one signed POST with the hub's headers and nothing else", "[HostedPush]")
{
    Harness h;
    const PushResult r = h.p->send(test_request());
    REQUIRE(r.ok);
    CHECK(r.status == 200);
    CHECK(r.host == "api.push.apple.com");
    REQUIRE(h.calls.size() == 1);
    const HttpCall& c = h.calls[0];
    CHECK(c.method == "POST");
    CHECK(c.url == "https://push.example.test/v1/send");
    CHECK(h.header(0, "Content-Type") == "application/json");
    CHECK(h.header(0, "X-Hub-Id") == "21fe31dfa154a261");
    CHECK(h.header(0, "X-Hub-Key") == RFC_PUB_1); // registered on first use
    CHECK(h.signed_ok(0));
    CHECK(c.headers.size() == 4);
    CHECK(h.body(0)["ts"] == 1789000000);
    CHECK(h.body(0)["nonce"] == "00000000000000000000000000000001");
}

TEST_CASE("quota sends X-Hub-Key, unregister never does", "[HostedPush]")
{
    Harness h;
    h.script = [](const HttpCall& c, size_t) {
        if (c.method == "GET") return reply(200, "{\"ok\":true,\"forwarding\":true,\"apns\":\"ready\",\"fcm\":\"ready\"}");
        return reply(200, "{\"ok\":true,\"status\":200,\"source\":\"forwarder\",\"hubid\":\"21fe31dfa154a261\",\"tier\":\"free\","
                          "\"suspended\":false,\"used\":12,\"limit\":300,\"remaining\":288,\"resets_at\":1789084800}");
    };
    const json st = json::parse(h.p->check(true, true));
    REQUIRE(h.calls.size() == 3);
    CHECK(h.calls[0].method == "GET");
    CHECK(h.calls[0].url == "https://push.example.test/healthz");
    CHECK(h.calls[0].headers.empty()); // anonymous
    CHECK(h.calls[1].url == "https://push.example.test/v1/register");
    CHECK(h.body(1)["pubkey"] == RFC_PUB_1);
    CHECK(h.calls[2].url == "https://push.example.test/v1/quota");
    CHECK(h.header(2, "X-Hub-Key") == RFC_PUB_1);
    CHECK(h.signed_ok(1));
    CHECK(h.signed_ok(2));
    CHECK(st["quota"]["used"] == 12);
    CHECK(st["quota"]["limit"] == 300);
    CHECK(st["health"]["forwarding"] == true);
    CHECK(st["hubid"] == "21fe31dfa154a261");
    CHECK(st["available"] == true);

    h.p->unregister();
    REQUIRE(h.calls.size() == 4);
    CHECK(h.calls[3].url == "https://push.example.test/v1/unregister");
    CHECK_FALSE(h.has_header(3, "X-Hub-Key"));
    CHECK(h.signed_ok(3));
}

TEST_CASE("the URL must be https, except a loopback mock with the debug routes on", "[HostedPush]")
{
    std::string why;
    CHECK(url_allowed("https://push.edgeslicer.com", why));
    CHECK(url_allowed("https://push.edgeslicer.com/", why));
    CHECK_FALSE(url_allowed("http://push.edgeslicer.com", why));
    CHECK_FALSE(url_allowed("ftp://x", why));
    CHECK_FALSE(url_allowed("", why));
    CHECK_FALSE(url_allowed("https://", why));
    CHECK_FALSE(url_allowed("https://a b", why));
}

// ======================================================================== result mapping ====

TEST_CASE("the answer maps onto PushResult as PROTOCOL.md says", "[HostedPush]")
{
    struct Row { int http; std::string body; std::string headers; Next next; int status; bool gone; bool ok; int retry_after; };
    const std::vector<Row> rows = {
        { 200, OK_SEND, "", Next::Done, 200, false, true, -1 },
        // upstream: gone only from Apple or Google
        { 410, upstream(410, "apns", "Unregistered", true), "", Next::Prune, 410, true, false, -1 },
        { 410, upstream(410, "apns", "ExpiredToken", true), "", Next::Prune, 410, true, false, -1 },
        { 404, upstream(404, "fcm", "UNREGISTERED", true), "", Next::Prune, 404, true, false, -1 },
        // owner decision: never prune on these two, whatever the service says
        { 400, upstream(400, "apns", "BadDeviceToken", true), "", Next::Drop, 400, false, false, -1 },
        { 400, upstream(400, "apns", "DeviceTokenNotForTopic", true), "", Next::Drop, 400, false, false, -1 },
        { 400, upstream(400, "apns", "BadDeviceToken", false), "", Next::Drop, 400, false, false, -1 },
        { 400, upstream(400, "apns", "BadCollapseId", false), "", Next::Drop, 400, false, false, -1 },
        { 403, upstream(403, "fcm", "SENDER_ID_MISMATCH", false), "", Next::Drop, 403, false, false, -1 },
        { 413, upstream(413, "apns", "PayloadTooLarge", false), "", Next::Drop, 413, false, false, -1 },
        { 429, upstream(429, "apns", "TooManyRequests", false), "Retry-After: 7\r\n", Next::Later, 429, false, false, 7 },
        { 503, upstream(503, "apns", "upstream_auth: InvalidProviderToken", false), "", Next::Later, 503, false, false, -1 },
        { 502, upstream(502, "apns", "BadTopic", false), "", Next::Later, 502, false, false, -1 },
        // the forwarder's own answers
        { 401, fwd(401, "unknown_hub"), "", Next::RetryWithKey, 401, false, false, -1 },
        { 401, fwd(401, "stale_timestamp", "", ",\"server_time\":1789000500"), "", Next::RetryWithClock, 401, false, false, -1 },
        { 401, fwd(401, "bad_signature"), "", Next::Bug, 401, false, false, -1 },
        { 401, fwd(401, "bad_hub_id"), "", Next::Bug, 401, false, false, -1 },
        { 403, fwd(403, "suspended", "abuse"), "", Next::Suspended, 403, false, false, -1 },
        { 400, fwd(400, "invalid_request"), "", Next::Drop, 400, false, false, -1 },
        { 400, fwd(400, "bundle_not_allowed"), "", Next::Drop, 400, false, false, -1 },
        { 404, fwd(404, "not_found"), "", Next::Drop, 404, false, false, -1 },
        { 405, fwd(405, "method_not_allowed"), "", Next::Bug, 405, false, false, -1 },
        { 409, fwd(409, "replay"), "", Next::RetryNonce, 409, false, false, -1 },
        { 413, fwd(413, "body_too_large"), "", Next::Drop, 413, false, false, -1 },
        { 429, fwd(429, "rate_limited", "", ",\"retry_after\":42"), "Retry-After: 42\r\n", Next::Later, 429, false, false, 42 },
        { 429, fwd(429, "daily_quota", "", ",\"retry_after\":3600"), "", Next::DropQuota, 429, false, false, 3600 },
        { 429, fwd(429, "device_limit"), "", Next::DropQuota, 429, false, false, -1 },
        { 503, fwd(503, "credentials_not_installed", "", ",\"retry_after\":3600"), "", Next::DropQuota, 503, false, false, 3600 },
        { 502, fwd(502, "upstream_unreachable", "", ",\"retry_after\":5"), "", Next::Later, 502, false, false, 5 },
        { 503, fwd(503, "overloaded"), "", Next::Later, 503, false, false, -1 },
        { 500, fwd(500, "internal"), "", Next::Later, 500, false, false, -1 },
        // a forwarder "gone" is never a prune
        { 400, "{\"ok\":false,\"status\":400,\"gone\":true,\"error\":\"invalid_request\",\"source\":\"forwarder\"}", "", Next::Drop, 400, false, false, -1 },
        // Caddy while the service is down: no JSON at all -> status 0, a transport failure
        { 502, "", "HTTP/1.1 502 Bad Gateway\r\nRetry-After: 9\r\n", Next::Later, 0, false, false, 9 },
        { 503, "<html>unavailable</html>", "", Next::Later, 0, false, false, -1 },
        { 0, "", "", Next::Later, 0, false, false, -1 },
    };
    for (const Row& row : rows) {
        INFO("HTTP " << row.http << " " << row.body);
        const Answer a = parse_answer(reply(row.http, row.body, row.headers), "push.example.test");
        CHECK(next_step(a) == row.next);
        CHECK(a.result.status == row.status);
        CHECK(a.result.gone == row.gone);
        CHECK(a.result.ok == row.ok);
        CHECK_FALSE(a.result.credential_expired);
        CHECK(a.retry_after == row.retry_after);
        CHECK(a.http_status == row.http);
        if (!row.ok) CHECK_FALSE(a.result.error.empty());
    }
}

TEST_CASE("error is code: message, and the host is the one that answered", "[HostedPush]")
{
    Answer a = parse_answer(reply(429, fwd(429, "daily_quota", "300 of 300 used today")), "push.example.test");
    CHECK(a.result.error == "daily_quota: 300 of 300 used today");
    CHECK(a.result.host == "push.example.test");
    a = parse_answer(reply(410, upstream(410, "apns", "Unregistered", true)), "push.example.test");
    CHECK(a.result.error == "Unregistered");
    CHECK(a.result.host == "api.push.apple.com");
    a = parse_answer(reply(401, fwd(401, "stale_timestamp", "", ",\"server_time\":1789000500")), "x");
    CHECK(a.server_time == 1789000500);
    HttpReply t;
    t.error = "Couldn't connect to server";
    a       = parse_answer(t, "push.example.test");
    CHECK(a.result.status == 0);
    CHECK(a.result.error.find("could not be reached") != std::string::npos);
    CHECK(retry_after_header("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 429 Too Many\r\nretry-after:  12\r\n") == 12);
    CHECK(retry_after_header("X-Not-Retry-After: 5\r\n") == -1);
    CHECK(retry_after_header("Retry-After: Wed, 21 Oct 2015 07:28:00 GMT\r\n") == -1);
}

// ======================================================================== retry-once rules ====

TEST_CASE("unknown_hub even with X-Hub-Key: register explicitly, then retry once", "[HostedPush]")
{
    Harness h;
    h.script = [](const HttpCall& c, size_t i) {
        if (i == 0) return reply(401, fwd(401, "unknown_hub"));
        if (c.url.find("/v1/register") != std::string::npos)
            return reply(200, "{\"ok\":true,\"status\":200,\"source\":\"forwarder\",\"new\":true}");
        return reply(200, OK_SEND);
    };
    const PushResult r = h.p->send(test_request());
    CHECK(r.ok);
    REQUIRE(h.calls.size() == 3);
    CHECK(h.calls[1].url.find("/v1/register") != std::string::npos);
    CHECK(h.calls[2].url.find("/v1/send") != std::string::npos);
    // ...and only once: a second unknown_hub is returned, not looped on.
    Harness h2;
    h2.script = [](const HttpCall&, size_t) { return reply(401, fwd(401, "unknown_hub")); };
    CHECK_FALSE(h2.p->send(test_request()).ok);
    CHECK(h2.calls.size() == 3);
}

TEST_CASE("stale_timestamp: the clock offset is applied, the send retried once, and a big one warned about", "[HostedPush]")
{
    Harness h;
    const long long server = h.now / 1000 + 500; // this PC is 500 s behind
    h.script = [server](const HttpCall& c, size_t) {
        const long long ts = json::parse(c.body)["ts"].get<long long>();
        if (std::llabs(ts - server) > 300)
            return reply(401, fwd(401, "stale_timestamp", "check this PC's clock", ",\"server_time\":" + std::to_string(server)));
        return reply(200, OK_SEND);
    };
    CHECK(h.p->send(test_request()).ok);
    REQUIRE(h.calls.size() == 2);
    CHECK(h.body(1)["ts"] == server);
    CHECK(h.p->clock_offset_s() == 500);
    CHECK(json::parse(h.p->status_json())["clock_warning"] == true);
    bool warned = false;
    for (const auto& l : h.logs) warned = warned || l.find("500 s off") != std::string::npos;
    CHECK(warned);
    // The offset sticks: the next send is right the first time.
    h.calls.clear();
    CHECK(h.p->send(test_request()).ok);
    CHECK(h.calls.size() == 1);
}

TEST_CASE("every retry is a new request: fresh ts, fresh nonce, re-signed", "[HostedPush]")
{
    Harness h;
    h.script = [&h](const HttpCall&, size_t i) {
        if (i == 0) { h.now += 1500; return reply(409, fwd(409, "replay")); }
        return reply(200, OK_SEND);
    };
    CHECK(h.p->send(test_request()).ok);
    REQUIRE(h.calls.size() == 2);
    CHECK(h.body(0)["nonce"] != h.body(1)["nonce"]);
    CHECK(h.body(0)["ts"] != h.body(1)["ts"]);
    CHECK(h.calls[0].body != h.calls[1].body);
    CHECK(h.header(0, "X-Hub-Sig") != h.header(1, "X-Hub-Sig"));
    CHECK(h.signed_ok(0));
    CHECK(h.signed_ok(1));
}

TEST_CASE("bad_signature is a bug: one request, no retry, and the page says so", "[HostedPush]")
{
    Harness h;
    h.script = [](const HttpCall&, size_t) { return reply(401, fwd(401, "bad_signature")); };
    bool             queued = true;
    const PushResult r      = h.p->deliver("d1", test_request(), queued);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(queued);
    CHECK(h.calls.size() == 1);
    CHECK(json::parse(h.p->status_json())["bug"].get<std::string>().find("bad_signature") != std::string::npos);
}

// ======================================================================== queue and backoff ====

TEST_CASE("the backoff is 5 s, 30 s, 2 min, then 5 min, +-20 %", "[HostedPush]")
{
    CHECK(backoff_ms(0, 0) == 5000);
    CHECK(backoff_ms(1, 0) == 30000);
    CHECK(backoff_ms(2, 0) == 120000);
    CHECK(backoff_ms(3, 0) == 300000);
    CHECK(backoff_ms(9, 0) == 300000);
    CHECK(backoff_ms(0, 1) == 6000);
    CHECK(backoff_ms(0, -1) == 4000);
    CHECK(backoff_ms(3, 1) == 360000);
    CHECK(backoff_ms(3, -5) == 240000); // clamped
}

TEST_CASE("an outage: queued, retried on schedule, and given up at 30 minutes", "[HostedPush]")
{
    Harness h;
    h.script = [](const HttpCall&, size_t) { return reply(502, "", "Retry-After: 1\r\n"); }; // Caddy, no JSON
    bool queued = false;
    const long long t0 = h.now;
    h.p->deliver("d1", test_request("apns", 1800), queued);
    REQUIRE(queued);
    CHECK(h.p->queued() == 1);
    std::vector<long long> tries { 0 };
    // Walk the clock a second at a time for 31 minutes; record when each attempt happens.
    for (int s = 1; s <= 31 * 60; ++s) {
        h.now = t0 + s * 1000LL;
        const size_t before = h.calls.size();
        h.p->pump();
        if (h.calls.size() != before) tries.push_back(s);
    }
    CHECK(tries == std::vector<long long>{ 0, 5, 35, 155, 455, 755, 1055, 1355, 1655 });
    CHECK(h.p->queued() == 0);
    REQUIRE(h.results.size() == 1);
    CHECK(h.results[0].first == "d1");
    CHECK_FALSE(h.results[0].second.ok);
    CHECK_FALSE(h.results[0].second.gone);
    CHECK(h.results[0].second.error.find("gave up") != std::string::npos);
}

TEST_CASE("a routine notification gives up at its own 300 s TTL", "[HostedPush]")
{
    Harness h;
    h.script = [](const HttpCall&, size_t) { HttpReply r; r.error = "Couldn't connect"; return r; };
    bool            queued = false;
    const long long t0     = h.now;
    h.p->deliver("d1", test_request("fcm", 300), queued);
    REQUIRE(queued);
    int attempts = 1;
    for (int s = 1; s <= 400; ++s) {
        h.now = t0 + s * 1000LL;
        const size_t before = h.calls.size();
        h.p->pump();
        attempts += (int) (h.calls.size() - before);
    }
    CHECK(attempts == 4); // 0, 5, 35, 155; the next (455) is past the TTL
    REQUIRE(h.results.size() == 1);
    CHECK_FALSE(h.results[0].second.ok);
}

TEST_CASE("the service comes back: the queued notification is delivered and recorded", "[HostedPush]")
{
    Harness h;
    bool    up = false;
    h.script = [&up](const HttpCall&, size_t) {
        return up ? reply(200, OK_SEND) : reply(503, fwd(503, "overloaded", "", ",\"retry_after\":2"));
    };
    bool queued = false;
    const PushResult first = h.p->deliver("d7", test_request(), queued);
    CHECK_FALSE(first.ok);
    REQUIRE(queued);
    h.now += 4000;
    h.p->pump();
    CHECK(h.calls.size() == 1); // not yet: 5 s
    up = true;
    h.now += 1000;
    h.p->pump();
    CHECK(h.calls.size() == 2);
    CHECK(h.signed_ok(1));
    REQUIRE(h.results.size() == 1);
    CHECK(h.results[0].first == "d7");
    CHECK(h.results[0].second.ok);
    CHECK(h.p->queued() == 0);
}

TEST_CASE("Retry-After is a floor on the next try", "[HostedPush]")
{
    Harness h;
    int n = 0;
    h.script = [&n](const HttpCall&, size_t) {
        return ++n == 1 ? reply(429, fwd(429, "rate_limited", "", ",\"retry_after\":60"), "Retry-After: 60\r\n") : reply(200, OK_SEND);
    };
    bool queued = false;
    h.p->deliver("d1", test_request(), queued);
    REQUIRE(queued);
    h.now += 59000;
    h.p->pump();
    CHECK(h.calls.size() == 1);
    h.now += 1000;
    h.p->pump();
    CHECK(h.calls.size() == 2);
    CHECK(h.results.size() == 1);
}

TEST_CASE("a Retry-After longer than the notification lives drops it now", "[HostedPush]")
{
    Harness h;
    h.script = [](const HttpCall&, size_t) { return reply(429, fwd(429, "registration_limit", "", ",\"retry_after\":3600")); };
    bool             queued = true;
    const PushResult r      = h.p->deliver("d1", test_request("apns", 1800), queued);
    CHECK_FALSE(queued);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.gone);
    CHECK(r.error.find("not retried") != std::string::npos);
    CHECK(h.p->queued() == 0);
}

TEST_CASE("daily_quota and device_limit drop without queueing; the quota also stops further requests", "[HostedPush]")
{
    Harness h;
    h.script = [](const HttpCall&, size_t) { return reply(429, fwd(429, "daily_quota", "300 of 300", ",\"retry_after\":3600")); };
    bool queued = true;
    PushResult r = h.p->deliver("d1", test_request(), queued);
    CHECK_FALSE(queued);
    CHECK(r.status == 429);
    CHECK(h.calls.size() == 1);
    // Until the service's reset, nothing is asked of it.
    r = h.p->deliver("d1", test_request(), queued);
    CHECK_FALSE(queued);
    CHECK(r.error.find("daily_quota") == 0);
    CHECK(h.calls.size() == 1);
    CHECK_FALSE(json::parse(h.p->status_json())["quota_note"].get<std::string>().empty());
    h.now += 3601 * 1000LL;
    h.script = [](const HttpCall&, size_t) { return reply(200, OK_SEND); };
    CHECK(h.p->deliver("d1", test_request(), queued).ok);
    CHECK(h.calls.size() == 2);

    Harness d;
    d.script = [](const HttpCall&, size_t) { return reply(429, fwd(429, "device_limit")); };
    d.p->deliver("d1", test_request(), queued);
    CHECK_FALSE(queued);
    CHECK(d.p->queued() == 0);

    Harness c;
    c.script = [](const HttpCall&, size_t) { return reply(503, fwd(503, "credentials_not_installed", "", ",\"retry_after\":3600")); };
    c.p->deliver("d1", test_request(), queued);
    CHECK_FALSE(queued);
    CHECK(c.p->queued() == 0);
}

TEST_CASE("suspended: stop sending, say why", "[HostedPush]")
{
    Harness h;
    h.script = [](const HttpCall&, size_t) { return reply(403, fwd(403, "suspended", "too many pushes")); };
    bool queued = true;
    h.p->deliver("d1", test_request(), queued);
    CHECK_FALSE(queued);
    std::string why;
    CHECK_FALSE(h.p->available(why));
    CHECK(why.find("suspended") != std::string::npos);
    CHECK(why.find("too many pushes") != std::string::npos);
    const size_t n = h.calls.size();
    CHECK_FALSE(h.p->send(test_request()).ok);
    CHECK(h.calls.size() == n); // no request made
}

TEST_CASE("a gone answer on a retry reaches the sink as gone", "[HostedPush]")
{
    Harness h;
    int n = 0;
    h.script = [&n](const HttpCall&, size_t) {
        return ++n == 1 ? reply(0, "") : reply(410, upstream(410, "apns", "Unregistered", true));
    };
    bool queued = false;
    h.p->deliver("d1", test_request(), queued);
    REQUIRE(queued);
    h.now += 5000;
    h.p->pump();
    REQUIRE(h.results.size() == 1);
    CHECK(h.results[0].second.gone);
}

TEST_CASE("the queue holds 32; the oldest is dropped first", "[HostedPush]")
{
    Harness h;
    h.script = [](const HttpCall&, size_t) { return reply(0, ""); };
    bool queued = false;
    for (int i = 0; i < QUEUE_MAX + 3; ++i) {
        h.p->deliver("d" + std::to_string(i), test_request(), queued);
        CHECK(queued);
        h.now += 10;
    }
    CHECK(h.p->queued() == (size_t) QUEUE_MAX);
    REQUIRE(h.results.size() == 3);
    CHECK(h.results[0].first == "d0");
    CHECK(h.results[1].first == "d1");
    CHECK(h.results[2].first == "d2");
    CHECK(h.results[0].second.error.find("dropped") == 0);
}

TEST_CASE("Queue::take_due separates due, expired and waiting", "[HostedPush]")
{
    Queue               q;
    std::vector<Queued> ev;
    Queued a; a.device_id = "a"; a.first_ms = 0; a.expires_ms = 100000; a.next_ms = 5000;
    Queued b; b.device_id = "b"; b.first_ms = 0; b.expires_ms = 3000;   b.next_ms = 5000;
    Queued c; c.device_id = "c"; c.first_ms = 0; c.expires_ms = 100000; c.next_ms = 9000;
    q.push(a, ev); q.push(b, ev); q.push(c, ev);
    CHECK(ev.empty());
    CHECK(q.next_due() == 3000);
    std::vector<Queued> expired;
    const auto due = q.take_due(6000, expired);
    REQUIRE(due.size() == 1);
    CHECK(due[0].device_id == "a");
    REQUIRE(expired.size() == 1);
    CHECK(expired[0].device_id == "b");
    CHECK(q.size() == 1);
    CHECK(q.next_due() == 9000);
    // The 30-minute cap applies even with a longer TTL.
    Queued d; d.first_ms = 0; d.expires_ms = 86400000; d.next_ms = 0;
    CHECK(d.deadline_ms() == GIVE_UP_MS);
}

// ======================================================================== privacy ====

TEST_CASE("no device token, ciphertext or body reaches a log line or a stored error", "[HostedPush]")
{
    Harness h;
    // A hostile or buggy service that echoes the token and the ciphertext back in its message.
    int n = 0;
    h.script = [&n](const HttpCall&, size_t) {
        ++n;
        const std::string echo = "token " + TOKEN + " e " + CIPHER;
        if (n == 1) return reply(400, fwd(400, "invalid_request", echo));
        if (n == 2) return reply(503, fwd(503, "overloaded", echo));
        // libcurl's own error text names the URL it failed on; for APNs that URL is the token.
        if (n == 3) { HttpReply r; r.error = "Failed to connect to /3/device/" + TOKEN + "?e=" + CIPHER; return r; }
        return reply(410, upstream(410, "apns", "Unregistered", true, echo));
    };
    std::vector<PushResult> seen;
    bool                    queued = false;
    seen.push_back(h.p->deliver("d1", test_request(), queued));
    seen.push_back(h.p->deliver("d2", test_request(), queued));
    h.now += 6000;
    h.p->pump();
    h.now += 40000;
    h.p->pump();
    for (const auto& r : h.results) seen.push_back(r.second);
    REQUIRE(n >= 4);
    for (const std::string& line : h.logs) {
        INFO(line);
        CHECK(line.find(TOKEN) == std::string::npos);
        CHECK(line.find(CIPHER) == std::string::npos);
        CHECK(line.find("\"nonce\"") == std::string::npos); // no request body
        CHECK(line.find("printer-0042") == std::string::npos);
    }
    for (const PushResult& r : seen) {
        INFO(r.error);
        CHECK(r.error.find(TOKEN) == std::string::npos);
        CHECK(r.error.find(CIPHER) == std::string::npos);
    }
    const std::string st = h.p->status_json();
    CHECK(st.find(TOKEN) == std::string::npos);
    CHECK(st.find(CIPHER) == std::string::npos);
    CHECK(st.find(test_identity().private_hex) == std::string::npos);
}

// ======================================================================== integration ====
//
// The real Http, the real signing, against a mock forwarder on loopback that verifies every
// signature, refuses a replayed nonce and a stale timestamp, and plays a scripted outage.

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;

struct MockForwarder
{
    struct Seen { std::string path, body, hubid, sig, key; bool sig_ok { false }; };

    asio::io_context          ioc;
    tcp::acceptor             acceptor { ioc };
    std::thread               thread;
    unsigned short            port { 0 };
    std::atomic<bool>         done { false };
    std::mutex                mtx;
    std::vector<Seen>         seen;
    std::set<std::string>     nonces;
    long long                 server_offset { 0 }; // the mock's clock minus the real one
    int                       outage { 0 };        // answer this many sends with a bare 502 first

    MockForwarder()
    {
        acceptor.open(tcp::v4());
        acceptor.bind(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), 0));
        acceptor.listen();
        port   = acceptor.local_endpoint().port();
        thread = std::thread([this]() { serve(); });
    }

    ~MockForwarder()
    {
        done = true;
        boost::system::error_code ig;
        acceptor.close(ig);
        if (thread.joinable()) thread.join();
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }

    static std::string header(const std::string& head, const std::string& name)
    {
        std::string low = head, key = name + ":";
        for (auto& c : low) c = (char) std::tolower((unsigned char) c);
        for (auto& c : key) c = (char) std::tolower((unsigned char) c);
        const size_t at = low.find("\r\n" + key);
        if (at == std::string::npos) return "";
        size_t i = at + 2 + key.size();
        while (i < head.size() && head[i] == ' ') ++i;
        return head.substr(i, head.find("\r\n", i) - i);
    }

    void answer(tcp::socket& s, int status, const std::string& body, const std::string& extra = "")
    {
        const std::string head = "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
                                 std::to_string(body.size()) + "\r\n" + extra + "Connection: close\r\n\r\n";
        boost::system::error_code ec;
        asio::write(s, asio::buffer(head + body), ec);
        s.shutdown(tcp::socket::shutdown_both, ec);
    }

    void serve()
    {
        while (!done) {
            boost::system::error_code ec;
            tcp::socket               s(ioc);
            acceptor.accept(s, ec);
            if (ec) return;
            std::string req;
            char        buf[8192];
            while (req.find("\r\n\r\n") == std::string::npos) {
                const size_t n = s.read_some(asio::buffer(buf), ec);
                if (ec || n == 0) break;
                req.append(buf, n);
            }
            const size_t hend = req.find("\r\n\r\n");
            if (hend == std::string::npos) continue;
            const std::string head = req.substr(0, hend);
            std::string       body = req.substr(hend + 4);
            const size_t      len  = (size_t) std::atoi(header(head, "Content-Length").c_str());
            while (body.size() < len) {
                const size_t n = s.read_some(asio::buffer(buf), ec);
                if (ec || n == 0) break;
                body.append(buf, n);
            }
            const std::string line = head.substr(0, head.find("\r\n"));
            const std::string path = line.substr(line.find(' ') + 1, line.rfind(' ') - line.find(' ') - 1);
            if (path == "/healthz") {
                answer(s, 200, "{\"ok\":true,\"forwarding\":true,\"apns\":\"ready\",\"fcm\":\"ready\",\"hubs\":1}");
                continue;
            }
            Seen v;
            v.path   = path;
            v.body   = body;
            v.hubid  = header(head, "X-Hub-Id");
            v.sig    = header(head, "X-Hub-Sig");
            v.key    = header(head, "X-Hub-Key");
            v.sig_ok = RemoteHub::identity_verify(RFC_PUB_1, body, v.sig);
            json j;
            try { j = json::parse(body); } catch (...) {}
            const long long now = (long long) std::time(nullptr) + server_offset;
            bool            replay;
            {
                std::lock_guard<std::mutex> lock(mtx);
                seen.push_back(v);
                replay = !nonces.insert(j.value("nonce", std::string())).second;
            }
            if (v.hubid != "21fe31dfa154a261") { answer(s, 401, fwd(401, "bad_hub_id")); continue; }
            if (!v.sig_ok) { answer(s, 401, fwd(401, "bad_signature")); continue; }
            if (std::llabs(now - j.value("ts", 0LL)) > 300) {
                answer(s, 401, fwd(401, "stale_timestamp", "", ",\"server_time\":" + std::to_string(now)));
                continue;
            }
            if (replay) { answer(s, 409, fwd(409, "replay")); continue; }
            if (path == "/v1/send" && outage > 0) {
                --outage;
                const std::string head502 = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nRetry-After: 1\r\nConnection: close\r\n\r\n";
                asio::write(s, asio::buffer(head502), ec);
                s.shutdown(tcp::socket::shutdown_both, ec);
                continue;
            }
            if (path == "/v1/send") answer(s, 200, OK_SEND);
            else if (path == "/v1/quota" || path == "/v1/register")
                answer(s, 200, "{\"ok\":true,\"status\":200,\"source\":\"forwarder\",\"hubid\":\"21fe31dfa154a261\",\"tier\":\"free\","
                               "\"suspended\":false,\"used\":3,\"limit\":300,\"remaining\":297,\"resets_at\":1789084800}");
            else if (path == "/v1/unregister") answer(s, 200, "{\"ok\":true,\"status\":200,\"source\":\"forwarder\"}");
            else answer(s, 404, fwd(404, "not_found"));
        }
    }
};

struct DebugRoutes
{
    DebugRoutes() { set("1"); }
    ~DebugRoutes() { set(""); }
    static void set(const char* v)
    {
#ifdef _WIN32
        _putenv_s("SNORCA_DEBUG_ROUTES", v);
#else
        if (*v) setenv("SNORCA_DEBUG_ROUTES", v, 1); else unsetenv("SNORCA_DEBUG_ROUTES");
#endif
    }
};

} // namespace

TEST_CASE("integration: real Http against a local mock forwarder", "[HostedPush][socket]")
{
    DebugRoutes   dbg; // http:// to loopback is only accepted with the debug routes on
    MockForwarder mock;

    std::vector<std::string> logs;
    std::vector<std::pair<std::string, PushResult>> results;
    Hooks h = default_hooks();
    h.log    = [&logs](bool, const std::string& l) { logs.push_back(l); };
    h.worker = false;
    auto p   = make_hosted_provider(h);
    p->configure("{\"url\":\"" + mock.url() + "\"}");
    p->set_identity(test_identity());
    p->set_result_sink([&results](const std::string& id, const PushResult& r) { results.emplace_back(id, r); });

    std::string why;
    REQUIRE(p->available(why));

    SECTION("a signed send is delivered")
    {
        const PushResult r = p->send(test_request());
        INFO(r.error);
        CHECK(r.ok);
        CHECK(r.status == 200);
        REQUIRE(mock.seen.size() == 1);
        CHECK(mock.seen[0].path == "/v1/send");
        CHECK(mock.seen[0].sig_ok);
        CHECK(mock.seen[0].hubid == "21fe31dfa154a261");
        CHECK(mock.seen[0].key == RFC_PUB_1);
        CHECK(json::parse(mock.seen[0].body)["token"] == TOKEN);
    }
    SECTION("health, register, quota and unregister")
    {
        const json st = json::parse(p->check(true, true));
        CHECK(st["health"]["forwarding"] == true);
        CHECK(st["quota"]["remaining"] == 297);
        REQUIRE(mock.seen.size() == 2);
        CHECK(mock.seen[0].path == "/v1/register");
        CHECK(mock.seen[1].path == "/v1/quota");
        CHECK(mock.seen[0].sig_ok);
        CHECK(mock.seen[1].sig_ok);
        CHECK(json::parse(p->unregister())["ok"] == true);
        CHECK(mock.seen.back().path == "/v1/unregister");
        CHECK(mock.seen.back().key.empty());
    }
    SECTION("a PC clock 10 minutes slow is corrected after one stale_timestamp")
    {
        mock.server_offset = 600;
        const PushResult r = p->send(test_request());
        INFO(r.error);
        CHECK(r.ok);
        REQUIRE(mock.seen.size() == 2);
        CHECK(p->clock_offset_s() >= 599);
        CHECK(p->clock_offset_s() <= 601);
    }
    SECTION("Caddy's bare 502 during an outage queues; the retry is delivered")
    {
        mock.outage = 1;
        bool queued = false;
        const PushResult first = p->deliver("d1", test_request(), queued);
        CHECK_FALSE(first.ok);
        CHECK(first.status == 0);
        REQUIRE(queued);
        CHECK(p->queued() == 1);
        // pump() uses the real clock here; the first retry is due after 5 s (+-20 %).
        for (int i = 0; i < 80 && results.empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            p->pump();
        }
        REQUIRE(results.size() == 1);
        CHECK(results[0].second.ok);
        REQUIRE(mock.seen.size() == 2);
        CHECK(mock.seen[0].body != mock.seen[1].body); // re-signed with a fresh nonce and ts
        CHECK(mock.seen[1].sig_ok);
    }
    for (const auto& l : logs) {
        CHECK(l.find(TOKEN) == std::string::npos);
        CHECK(l.find(CIPHER) == std::string::npos);
    }
}

// Privacy audit 2026-10: a removed device (hub page Remove, the app unpairing, a new phone link)
// gets nothing more - not even what was still queued for the push service.
TEST_CASE("forget drops what is queued for a removed device, and only for it", "[HostedPush]")
{
    Harness h;
    h.script = [](const HttpCall&, size_t) { return reply(502, "", "Retry-After: 1\r\n"); };
    bool queued = false;
    h.p->deliver("d1", test_request("apns", 1800), queued);
    REQUIRE(queued);
    h.p->deliver("d2", test_request("fcm", 1800), queued);
    REQUIRE(queued);
    h.p->deliver("d1", test_request("apns", 1800), queued);
    REQUIRE(h.p->queued() == 3);

    CHECK(h.p->forget("d1") == 2);
    CHECK(h.p->queued() == 1);
    CHECK(h.p->forget("nobody") == 0);

    // The service comes back: only d2's notification is still sent.
    h.script = [](const HttpCall&, size_t) { return reply(200, OK_SEND); };
    const size_t before = h.calls.size();
    h.now += 60 * 1000LL;
    h.p->pump();
    REQUIRE(h.calls.size() == before + 1);
    CHECK(h.body(before)["platform"] == "fcm");

    // "" forgets every device's notifications (a new phone link).
    h.script = [](const HttpCall&, size_t) { return reply(502, "", "Retry-After: 1\r\n"); };
    h.p->deliver("d3", test_request(), queued);
    h.p->deliver("d4", test_request(), queued);
    REQUIRE(h.p->queued() == 2);
    CHECK(h.p->forget("") == 2);
    CHECK(h.p->queued() == 0);
}
