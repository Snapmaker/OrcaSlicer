#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <nlohmann/json.hpp>
#include "slic3r/Utils/SnapLogClient.hpp"

using namespace Slic3r::SnapLog::v1;

// Helper: find a header value in a vector of pairs (case-insensitive key).
static std::string hdr(const std::vector<std::pair<std::string, std::string>>& hs, const std::string& key)
{
    for (const auto& kv : hs) {
        bool match = (kv.first.size() == key.size());
        if (match) {
            for (std::size_t i = 0; i < key.size(); ++i) {
                if (std::tolower((unsigned char) kv.first[i]) != std::tolower((unsigned char) key[i])) {
                    match = false;
                    break;
                }
            }
        }
        if (match)
            return kv.second;
    }
    return {};
}

TEST_CASE("SnapLog scaffold compiles", "[snaplog]") { REQUIRE(true); }

TEST_CASE("hmac_sha256_hex matches doc vector", "[snaplog]")
{
    // clientType=Orca clientId=app-123 ts=1778639794110 nonce=9b22...
    auto s = std::string("Orca") + "app-123" + "1778639794110" + "9b2226b29c3e460eae7ff1d3315c522d";
    REQUIRE(hmac_sha256_hex("c25hcG1ha2VyLU9yY2E", s) == "a9f092d93a35c149cad8a3d5bce890248d1bdf41a3e9b2e4fd5f61de6afc2d6d");
}

TEST_CASE("hmac_sha256_hex empty msg", "[snaplog]")
{
    REQUIRE(hmac_sha256_hex("k", "").size() == 64); // 32 bytes hex
}

TEST_CASE("level_str wire strings", "[snaplog]")
{
    REQUIRE(level_str(SnapLogLevel::Info) == "INFO");
    REQUIRE(level_str(SnapLogLevel::Warning) == "WARN");
    REQUIRE(level_str(SnapLogLevel::Error) == "ERROR");
}

TEST_CASE("normalize_logger takes last segment", "[snaplog]")
{
    REQUIRE(normalize_logger("Plater::priv::slice") == "slice");
    REQUIRE(normalize_logger("slice") == "slice");
    REQUIRE(normalize_logger("") == "");
}

// ---------------------------------------------------------------------------
// Task 4: privacy pure functions — redact_path, mask_secret, hash_pii, render
// ---------------------------------------------------------------------------

TEST_CASE("redact_path strips home prefix case/slash-insensitive", "[snaplog][priv]")
{
    REQUIRE(redact_path("C:\\Users\\bob", "C:/Users/bob/AppData/Roaming/x") == "~/AppData/Roaming/x");
    REQUIRE(redact_path("/home/alice", "/home/alice/proj/m.stl") == "~/proj/m.stl");
    REQUIRE(redact_path("/home/alice", "/opt/x") == "/opt/x"); // no match preserved
}

TEST_CASE("mask_secret redacts akia/jwt/ghp/password", "[snaplog][priv]")
{
    REQUIRE(mask_secret_in_value("key=AKIAIOSFODNN7EXAMPLE").find("AKIA") == std::string::npos);
    // JWT itself is masked; the harmless prefix "tok" may pass through.
    REQUIRE(mask_secret_in_value("tok eyJhbGci.x.y").find("eyJ") == std::string::npos);
    REQUIRE(mask_secret_in_value("ghp_0123456789abcdefghijklmnopqrstuvwxyz").find("ghp_") == std::string::npos);
    REQUIRE(mask_secret_in_value("password=hunter2").find("hunter2") == std::string::npos);
}

TEST_CASE("mask_secret leaves normal slicer values untouched", "[snaplog][priv]")
{
    REQUIRE(mask_secret_in_value("layer 42 of 200") == "layer 42 of 200");
    REQUIRE(mask_secret_in_value("tokenize") == "tokenize");
    REQUIRE(mask_secret_in_value("tokens count: 5") == "tokens count: 5");
    REQUIRE(mask_secret_in_value("filament PLA Black 210C") == "filament PLA Black 210C");
}

TEST_CASE("hash_pii stable 16 hex", "[snaplog][priv]")
{
    REQUIRE(hash_pii("desktop-1", "SN123").size() == 16);
    REQUIRE(hash_pii("desktop-1", "SN123") == hash_pii("desktop-1", "SN123"));
}

TEST_CASE("renderers hash printer SN in standard and caller fields", "[snaplog][priv]")
{
    const std::string expected = hash_pii("client-1", "SN12345");

    RealtimeIdentity realtime_id;
    realtime_id.client_id  = "client-1";
    realtime_id.printer_sn = "SN12345";
    auto realtime          = render_realtime_body(SnapLogLevel::Info, "message", {{"sn", "SN12345"}}, 64 * 1024, realtime_id);
    REQUIRE(realtime.find("SN12345") == std::string::npos);
    REQUIRE(realtime.find(expected) != std::string::npos);

    BatchLineContext batch_ctx;
    batch_ctx.clientId = "client-1";
    batch_ctx.print_sn = "SN12345";
    auto batch         = render_batch_line(SnapLogLevel::Info, "message", {{"sn", "SN12345"}}, 64 * 1024, batch_ctx);
    REQUIRE(batch.find("SN12345") == std::string::npos);
    REQUIRE(batch.find(expected) != std::string::npos);
}

TEST_CASE("renderers sanitize message secrets and user paths", "[snaplog][priv]")
{
    const std::string message = "password=hunter2 model C:\\Users\\alice\\part.stl failed";

    RealtimeIdentity realtime_id;
    realtime_id.home_for_redact = "C:/Users/alice";
    auto realtime               = render_realtime_body(SnapLogLevel::Warning, message, {}, 64 * 1024, realtime_id);
    REQUIRE(realtime.find("hunter2") == std::string::npos);
    REQUIRE(realtime.find("Users\\alice") == std::string::npos);

    BatchLineContext batch_ctx;
    batch_ctx.home_for_redact = "C:/Users/alice";
    auto batch                = render_batch_line(SnapLogLevel::Warning, message, {}, 64 * 1024, batch_ctx);
    REQUIRE(batch.find("hunter2") == std::string::npos);
    REQUIRE(batch.find("Users\\alice") == std::string::npos);
}

TEST_CASE("renderers keep valid JSON when UTF-8 truncates or input is invalid", "[snaplog]")
{
    const std::string multibyte = std::string(1000, 'x') + u8"中文日志";
    const std::string invalid   = std::string("bad ") + char(0xFF) + char(0x80) + " bytes";
    BatchLineContext  ctx;

    auto realtime_truncated = render_realtime_body(SnapLogLevel::Info, multibyte, {}, 700);
    (void) nlohmann::json::parse(realtime_truncated);
    auto realtime_invalid = render_realtime_body(SnapLogLevel::Info, invalid, {}, 64 * 1024);
    (void) nlohmann::json::parse(realtime_invalid);

    auto batch_truncated = render_batch_line(SnapLogLevel::Info, multibyte, {}, 700, ctx);
    (void) nlohmann::json::parse(batch_truncated.substr(0, batch_truncated.size() - 1));
    auto batch_invalid = render_batch_line(SnapLogLevel::Info, invalid, {}, 64 * 1024, ctx);
    (void) nlohmann::json::parse(batch_invalid.substr(0, batch_invalid.size() - 1));
}

TEST_CASE("render_realtime_body strips raw* and keeps allowed keys", "[snaplog][priv]")
{
    SnapLogExt ext  = {{"modelName", "a.stl"}, {"rawGcode", "G28"}, {"secret", "x"}};
    auto       body = render_realtime_body(SnapLogLevel::Error, "boom", ext, 64 * 1024);
    REQUIRE(body.find("rawGcode") == std::string::npos);
    REQUIRE(body.find("modelName") != std::string::npos);
    REQUIRE(body.find("boom") != std::string::npos);
}

TEST_CASE("render_realtime_body truncates long message", "[snaplog]")
{
    std::string big(100000, 'x');
    auto        body = render_realtime_body(SnapLogLevel::Info, big, {}, 1024);
    REQUIRE(body.size() <= 2048);
    REQUIRE(body.find("truncated") != std::string::npos);
}

TEST_CASE("render_realtime_body fallback keeps real clientId", "[snaplog][priv]")
{
    RealtimeIdentity id;
    id.client_id = "instance-123";
    auto body    = render_realtime_body(SnapLogLevel::Info, "small", {}, 1, id);
    auto parsed  = nlohmann::json::parse(body);
    REQUIRE(parsed["clientId"].get<std::string>() == "instance-123");
    REQUIRE(parsed["message"].get<std::string>().find("truncated") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Task 5: RateLimiter — per-eventName token bucket + default + total-unknown
// ---------------------------------------------------------------------------

TEST_CASE("rate limiter concurrent calls admit exactly the cap", "[snaplog][rate]")
{
    constexpr int            kCallsPerThread = 200;
    constexpr int            kTotalCalls     = 8 * kCallsPerThread;
    std::atomic<int64_t>     now{0};
    RateLimiter              rl([&now]() { return now.load(); });
    std::atomic<int>         admitted{0};
    std::atomic<bool>        release{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&]() {
            while (!release.load())
                std::this_thread::yield();
            for (int i = 0; i < kCallsPerThread; ++i) {
                if (rl.allow("concurrent_event", kTotalCalls, kTotalCalls, {}))
                    ++admitted;
            }
        });
    }
    release.store(true);
    for (auto& worker : workers)
        worker.join();
    REQUIRE(admitted.load() == kTotalCalls);
}

TEST_CASE("rate limiter honors registered name cap and throttles beyond", "[snaplog][rate]")
{
    // Injected clock: starts at 0, caller advances manually via the mutable ref.
    int64_t     now    = 0;
    auto        now_ms = [&now]() { return now; };
    RateLimiter rl(now_ms);

    std::map<std::string, int> caps = {{"slice_completed", 2}};
    // default_cap=5, total_unknown_cap=20 (unused for registered name).
    // cap=2 → exactly 2 admits in this 1-second window at t=0.
    REQUIRE(rl.allow("slice_completed", 5, 20, caps));
    REQUIRE(rl.allow("slice_completed", 5, 20, caps));
    REQUIRE_FALSE(rl.allow("slice_completed", 5, 20, caps)); // 3rd throttled

    // Advance clock by 1000ms → full refill (2 tokens).
    now += 1000;
    REQUIRE(rl.allow("slice_completed", 5, 20, caps));
    REQUIRE(rl.allow("slice_completed", 5, 20, caps));
    REQUIRE_FALSE(rl.allow("slice_completed", 5, 20, caps));
}

TEST_CASE("rate limiter throttles unregistered name beyond default", "[snaplog][rate]")
{
    int64_t     now    = 0;
    auto        now_ms = [&now]() { return now; };
    RateLimiter rl(now_ms);

    // No registered caps → "print_tick" uses default_cap=3.
    // total_unknown_cap=20 is plenty, so only the per-name bucket binds here.
    std::map<std::string, int> caps;
    REQUIRE(rl.allow("print_tick", 3, 20, caps));
    REQUIRE(rl.allow("print_tick", 3, 20, caps));
    REQUIRE(rl.allow("print_tick", 3, 20, caps));
    REQUIRE_FALSE(rl.allow("print_tick", 3, 20, caps)); // 4th throttled

    // Advance 1000ms → per-name bucket refills to 3.
    now += 1000;
    REQUIRE(rl.allow("print_tick", 3, 20, caps));
    REQUIRE(rl.allow("print_tick", 3, 20, caps));
    REQUIRE(rl.allow("print_tick", 3, 20, caps));
    REQUIRE_FALSE(rl.allow("print_tick", 3, 20, caps));

    // Half-second advance → only 1.5 tokens refill, so 1 admit then throttle.
    now += 500;
    REQUIRE(rl.allow("print_tick", 3, 20, caps));
    REQUIRE_FALSE(rl.allow("print_tick", 3, 20, caps));
}

TEST_CASE("rate limiter total-unknown cap throttles many distinct names", "[snaplog][rate]")
{
    int64_t     now    = 0;
    auto        now_ms = [&now]() { return now; };
    RateLimiter rl(now_ms);

    std::map<std::string, int> caps; // all names unregistered
    // default_cap=5 each, total_unknown_cap=10 aggregate.
    // Emitting 2 distinct names at 5 each = 10 total → exactly at cap.
    for (int i = 0; i < 5; ++i) {
        REQUIRE(rl.allow("u" + std::to_string(i), 5, 10, caps)); // name u0..u4 each once
    }
    // u0 has used 1 of its own 5-token bucket (4 left), but the shared
    // unknown-aggregate bucket has now consumed 5 of 10. Re-hitting u0..u4
    // again drains the aggregate to 10.
    for (int i = 0; i < 5; ++i) {
        REQUIRE(rl.allow("u" + std::to_string(i), 5, 10, caps)); // second hit each
    }
    // Aggregate bucket is now at 0 → a brand-new unregistered name is throttled
    // even though its own per-name bucket (5) is full.
    REQUIRE_FALSE(rl.allow("uBrandNew", 5, 10, caps));

    // Advance 1000ms → both the per-name and aggregate buckets refill.
    now += 1000;
    REQUIRE(rl.allow("uBrandNew", 5, 10, caps)); // now admitted
}

// ---------------------------------------------------------------------------
// Task 6: realtime eviction policy (pure function)
// ---------------------------------------------------------------------------

TEST_CASE("eviction: queue not full -> Admit", "[snaplog][evict]")
{
    std::deque<SnapLogLevel> q = {SnapLogLevel::Info};
    auto                     r = decide_realtime_admit(q, 3, SnapLogLevel::Warning);
    REQUIRE(r.decision == EvictDecision::Admit);
    REQUIRE_FALSE(r.evict_index.has_value());
}

TEST_CASE("eviction: full + incoming Error + has non-Error -> EvictOldestNonError", "[snaplog][evict]")
{
    std::deque<SnapLogLevel> q = {SnapLogLevel::Info, SnapLogLevel::Error, SnapLogLevel::Warning};
    // capacity=3, full; incoming Error; non-Error entries exist.
    auto r = decide_realtime_admit(q, 3, SnapLogLevel::Error);
    REQUIRE(r.decision == EvictDecision::EvictOldestNonError);
    REQUIRE(r.evict_index.has_value());
    REQUIRE(*r.evict_index == 0); // oldest non-Error is Info at index 0
}

TEST_CASE("eviction: full + incoming Error + all Error -> EvictOldestError", "[snaplog][evict]")
{
    std::deque<SnapLogLevel> q = {SnapLogLevel::Error, SnapLogLevel::Error, SnapLogLevel::Error};
    auto                     r = decide_realtime_admit(q, 3, SnapLogLevel::Error);
    REQUIRE(r.decision == EvictDecision::EvictOldestError);
    REQUIRE(r.evict_index.has_value());
    REQUIRE(*r.evict_index == 0); // oldest Error (FIFO) at index 0
}

TEST_CASE("eviction: full + incoming Warning -> DropIncoming", "[snaplog][evict]")
{
    std::deque<SnapLogLevel> q = {SnapLogLevel::Info, SnapLogLevel::Error};
    auto                     r = decide_realtime_admit(q, 2, SnapLogLevel::Warning);
    REQUIRE(r.decision == EvictDecision::DropIncoming);
    REQUIRE_FALSE(r.evict_index.has_value());
}

TEST_CASE("eviction: full + incoming Info -> DropIncoming", "[snaplog][evict]")
{
    std::deque<SnapLogLevel> q = {SnapLogLevel::Error, SnapLogLevel::Error};
    auto                     r = decide_realtime_admit(q, 2, SnapLogLevel::Info);
    REQUIRE(r.decision == EvictDecision::DropIncoming);
    REQUIRE_FALSE(r.evict_index.has_value());
}

TEST_CASE("eviction: mixed queue picks oldest non-Error not any non-Error", "[snaplog][evict]")
{
    // Queue: [Info, Error, Warning, Error], capacity=4, incoming=Error
    // Oldest non-Error is Info (index 0), not Warning (index 2).
    std::deque<SnapLogLevel> q = {SnapLogLevel::Info, SnapLogLevel::Error, SnapLogLevel::Warning, SnapLogLevel::Error};
    auto                     r = decide_realtime_admit(q, 4, SnapLogLevel::Error);
    REQUIRE(r.decision == EvictDecision::EvictOldestNonError);
    REQUIRE(r.evict_index.has_value());
    REQUIRE(*r.evict_index == 0); // Info is oldest non-Error
}

TEST_CASE("eviction: full + incoming Error + non-Error later in queue", "[snaplog][evict]")
{
    // Queue: [Error, Error, Warning], capacity=3, incoming=Error
    // Oldest non-Error is Warning at index 2.
    std::deque<SnapLogLevel> q = {SnapLogLevel::Error, SnapLogLevel::Error, SnapLogLevel::Warning};
    auto                     r = decide_realtime_admit(q, 3, SnapLogLevel::Error);
    REQUIRE(r.decision == EvictDecision::EvictOldestNonError);
    REQUIRE(r.evict_index.has_value());
    REQUIRE(*r.evict_index == 2);
}

// ---------------------------------------------------------------------------
// Task 7: SnapLogHandle / SnapLogDeps / SnapLogConfig / SnapLogClient stubs
// ---------------------------------------------------------------------------

TEST_CASE("SnapLogHandle: done() true when fulfilled", "[snaplog][handle]")
{
    // A test-fake handle: http is null, just toggle the atomics.
    SnapLogHandle h;
    REQUIRE_FALSE(h.done());
    h.fulfilled.store(true);
    REQUIRE(h.done());
}

TEST_CASE("SnapLogHandle: done() true when cancelled", "[snaplog][handle]")
{
    SnapLogHandle h;
    REQUIRE_FALSE(h.done());
    h.cancelled.store(true);
    REQUIRE(h.done());
}

TEST_CASE("SnapLogHandle: cancel() sets cancelled flag (null http)", "[snaplog][handle]")
{
    // http is null in test fakes — cancel() must still set the flag and not crash.
    SnapLogHandle h;
    h.prom      = std::make_shared<std::promise<SnapLogResult>>();
    auto future = h.prom->get_future();
    REQUIRE_FALSE(h.cancelled.load());
    h.cancel();
    REQUIRE(h.cancelled.load());
    REQUIRE(h.done());
    REQUIRE(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    auto result = future.get();
    REQUIRE(result.cancelled);
    REQUIRE(result.status == 0);
}

TEST_CASE("SnapLogConfig defaults match spec", "[snaplog][config]")
{
    SnapLogConfig c;
    REQUIRE(c.gateway_base == "https://api.snapmaker.com");
    REQUIRE(c.enabled == true);
    REQUIRE(c.default_rate_cap_per_sec == 5);
    REQUIRE(c.total_unknown_rate_cap_per_sec == 20);
    REQUIRE(c.realtime_queue_cap == 1024);
    REQUIRE(c.http_connect_timeout_sec == 5);
    REQUIRE(c.http_total_timeout_sec == 30);
    REQUIRE(c.poll_interval_ms == 50);
    REQUIRE(c.realtime_join_deadline_sec == 5);
}

TEST_CASE("SnapLogClient: instance() returns same reference (singleton)", "[snaplog][client]")
{
    auto& a = SnapLogClient::instance();
    auto& b = SnapLogClient::instance();
    REQUIRE(&a == &b);
}

TEST_CASE("SnapLogClient: log() on un-init client is a no-op (no crash)", "[snaplog][client]")
{
    // No init() call — m_int is null. log() must return safely.
    SnapLogClient::instance().log(SnapLogLevel::Info, "hello", {}, SnapLogPolicy::Realtime, __FUNCTION__, __LINE__);
    REQUIRE(true); // reached without crashing
}

TEST_CASE("SnapLogClient: init + set_consent + shutdown round-trip is safe", "[snaplog][client]")
{
    SnapLogDeps deps;
    deps.consent_ok = []() { return true; };
    deps.now_ms     = []() { return int64_t(0); };
    SnapLogConfig cfg;
    SnapLogClient::instance().init(std::move(deps), std::move(cfg));
    SnapLogClient::instance().set_consent(false);
    SnapLogClient::instance().set_user_token("t");
    SnapLogClient::instance().set_user_id("u");
    SnapLogClient::instance().set_device_id("d");
    SnapLogClient::instance().log(SnapLogLevel::Warning, "w", {}, SnapLogPolicy::Realtime, __FUNCTION__, __LINE__);
    SnapLogClient::instance().shutdown();
    // Post-shutdown log() is also a no-op.
    SnapLogClient::instance().log(SnapLogLevel::Error, "post", {}, SnapLogPolicy::Realtime, __FUNCTION__, __LINE__);
    REQUIRE(true);
}

TEST_CASE("SnapLogClient: concurrent log and shutdown are safe", "[snaplog][client]")
{
    for (int round = 0; round < 8; ++round) {
        SnapLogDeps deps;
        deps.consent_ok = []() { return true; };
        deps.now_ms     = []() { return int64_t(0); };
        SnapLogConfig cfg;
        cfg.poll_interval_ms               = 1;
        cfg.realtime_join_deadline_sec     = 1;
        cfg.default_rate_cap_per_sec       = 1000;
        cfg.total_unknown_rate_cap_per_sec = 1000;
        SnapLogClient::instance().init(std::move(deps), std::move(cfg));
        SnapLogClient::instance().set_user_token("token");

        std::atomic<bool> release{false};
        auto              log_worker = [&release]() {
            while (!release.load())
                std::this_thread::yield();
            SnapLogClient::instance().log(SnapLogLevel::Info, "race", {{"eventName", "race"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                          __LINE__);
        };
        auto shutdown_worker = [&release]() {
            while (!release.load())
                std::this_thread::yield();
            SnapLogClient::instance().shutdown();
        };
        std::thread logger(log_worker);
        std::thread closer(shutdown_worker);
        release.store(true);
        logger.join();
        closer.join();
        SnapLogClient::instance().log(SnapLogLevel::Error, "after", {{"eventName", "race"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                      __LINE__);
    }
    REQUIRE(true);
}

TEST_CASE("SnapLogClient: re-login clears auth_known_dead; empty token does not", "[snaplog][batch]")
{
    // Spec §4.4: after a 401/403 marks auth_known_dead, a re-login (a new
    // non-empty token pushed via set_user_token) MUST clear the flag so batch
    // uploads resume. An empty token (logout) MUST NOT clear it (avoid
    // resurrecting a known-dead credential).
    SnapLogDeps deps;
    deps.consent_ok = []() { return true; };
    deps.now_ms     = []() { return int64_t(0); };
    SnapLogConfig cfg;
    SnapLogClient::instance().init(std::move(deps), std::move(cfg));

    // Simulate the flusher having hit a 401 on create.
    SnapLogClient::instance().set_auth_known_dead_for_test(true);
    REQUIRE(SnapLogClient::instance().auth_known_dead_for_test());

    // Empty token (logout) must NOT clear the flag.
    SnapLogClient::instance().set_user_token("");
    REQUIRE(SnapLogClient::instance().auth_known_dead_for_test());

    // Non-empty token (re-login) MUST clear the flag.
    SnapLogClient::instance().set_user_token("new-token");
    REQUIRE_FALSE(SnapLogClient::instance().auth_known_dead_for_test());

    SnapLogClient::instance().shutdown();
}

TEST_CASE("SNAP_LOG macro compiles and is a safe no-op un-init'd", "[snaplog]")
{
    SNAP_LOG(Info, "smoke", {"eventName", "smoke_test"});
    SNAP_LOG_BATCH(Error, "smoke2", {"eventName", "smoke_test2"}, {"k", "v"});
    REQUIRE(true); // reached without crash
}

// ---------------------------------------------------------------------------
// Task 8: log() realtime admission pipeline (consent / disable / rate / queue)
// ---------------------------------------------------------------------------
// Helper: build a SnapLogClient with fake deps. now_ms starts at 0, increments
// on each call so rate-limiter time advances naturally across calls.

namespace {
struct FakeClient
{
    int64_t       now           = 0;
    int           request_count = 0;
    SnapLogDeps   deps;
    SnapLogConfig cfg;

    FakeClient()
    {
        deps.do_request = [this](const std::string& /*method*/, const std::string& /*url*/,
                                 std::vector<std::pair<std::string, std::string>> /*headers*/, const boost::filesystem::path* /*body_file*/,
                                 const std::string* /*body_str*/) {
            ++request_count;
            auto h = std::make_shared<SnapLogHandle>();
            h->fulfilled.store(true);
            return h;
        };
        deps.consent_ok = []() { return true; };
        deps.user_token = []() { return std::string("tok"); };
        deps.user_id    = []() { return std::string("uid"); };
        deps.device_id  = []() { return std::string("did"); };
        deps.machine_id = []() { return std::string("mid"); };
        deps.now_ms     = [this]() { return now++; };
        // Small queue cap for fill/evict tests.
        cfg.realtime_queue_cap             = 4;
        cfg.default_rate_cap_per_sec       = 1000; // high default so rate limit doesn't interfere
        cfg.total_unknown_rate_cap_per_sec = 10000;
    }

    void init()
    {
        // Copy deps/cfg into the singleton.
        SnapLogDeps   d = deps; // copy (lambdas capture by value via this*)
        SnapLogConfig c = cfg;
        SnapLogClient::instance().init(std::move(d), std::move(c));
        // Default to logged-in so existing tests pass the login gate (log()
        // drops events when user_token is empty). Tests that exercise the
        // gate itself call set_user_token("") explicitly.
        SnapLogClient::instance().set_user_token("tok");
    }
    void shutdown() { SnapLogClient::instance().shutdown(); }
};
} // anonymous namespace

TEST_CASE("log(): consent false drops all events", "[snaplog][pipeline]")
{
    FakeClient fc;
    fc.deps.consent_ok = []() { return false; };
    fc.init();
    // consent is snapshotted from deps.consent_ok() at init time.
    SnapLogClient::instance().log(SnapLogLevel::Info, "msg", SnapLogExt{{"eventName", "test"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                  __LINE__);
    REQUIRE(SnapLogClient::instance().realtime_queue_size_for_test() == 0);
    fc.shutdown();
}

TEST_CASE("log(): disabled event name not enqueued", "[snaplog][pipeline]")
{
    FakeClient fc;
    fc.cfg.event_disable_list.push_back("blocked_event");
    fc.init();
    SnapLogClient::instance().log(SnapLogLevel::Info, "msg", SnapLogExt{{"eventName", "blocked_event"}}, SnapLogPolicy::Realtime,
                                  __FUNCTION__, __LINE__);
    REQUIRE(SnapLogClient::instance().realtime_queue_size_for_test() == 0);
    // A non-disabled event IS enqueued.
    SnapLogClient::instance().log(SnapLogLevel::Info, "msg", SnapLogExt{{"eventName", "ok_event"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                  __LINE__);
    REQUIRE(SnapLogClient::instance().realtime_queue_size_for_test() == 1);
    fc.shutdown();
}

TEST_CASE("log(): rate limit throttles beyond cap", "[snaplog][pipeline]")
{
    FakeClient fc;
    fc.cfg.event_rate_cap_per_sec["throttled"] = 2;
    fc.init();
    // First 2 pass (cap=2 at t=0), 3rd is throttled.
    for (int i = 0; i < 3; ++i) {
        SnapLogClient::instance().log(SnapLogLevel::Info, "msg", SnapLogExt{{"eventName", "throttled"}}, SnapLogPolicy::Realtime,
                                      __FUNCTION__, __LINE__);
    }
    REQUIRE(SnapLogClient::instance().realtime_queue_size_for_test() == 2);
    fc.shutdown();
}

TEST_CASE("log(): queue fills to cap, Error evicts oldest non-Error", "[snaplog][pipeline]")
{
    FakeClient fc;
    // cap=4; rate caps high so nothing throttles.
    fc.init();
    // Fill with 4 Info events.
    for (int i = 0; i < 4; ++i) {
        SnapLogClient::instance().log(SnapLogLevel::Info, "fill", SnapLogExt{{"eventName", "fill"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                      __LINE__);
    }
    REQUIRE(SnapLogClient::instance().realtime_queue_size_for_test() == 4);
    // Now push an Error while full of non-Error → EvictOldestNonError.
    // Size stays at cap (4), and the queue should contain the Error.
    SnapLogClient::instance().log(SnapLogLevel::Error, "boom", SnapLogExt{{"eventName", "err"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                  __LINE__);
    REQUIRE(SnapLogClient::instance().realtime_queue_size_for_test() == 4);
    // Verify an Error is present in the queue.
    auto levels    = SnapLogClient::instance().realtime_queue_levels_for_test();
    bool has_error = false;
    for (auto l : levels)
        if (l == SnapLogLevel::Error) {
            has_error = true;
            break;
        }
    REQUIRE(has_error);
    fc.shutdown();
}

TEST_CASE("log(): full queue + non-Error incoming -> dropped counter increments", "[snaplog][pipeline]")
{
    FakeClient fc;
    fc.init();
    // Fill with 4 Info events.
    for (int i = 0; i < 4; ++i) {
        SnapLogClient::instance().log(SnapLogLevel::Info, "fill", SnapLogExt{{"eventName", "fill"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                      __LINE__);
    }
    REQUIRE(SnapLogClient::instance().dropped_for_test() == 0);
    // 5th Info is dropped (queue full, incoming non-Error).
    SnapLogClient::instance().log(SnapLogLevel::Info, "overflow", SnapLogExt{{"eventName", "over"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                  __LINE__);
    REQUIRE(SnapLogClient::instance().realtime_queue_size_for_test() == 4);
    REQUIRE(SnapLogClient::instance().dropped_for_test() == 1);
    fc.shutdown();
}

TEST_CASE("log(): Buffered policy does not land in realtime queue", "[snaplog][pipeline]")
{
    FakeClient fc;
    fc.init();
    SnapLogClient::instance().log(SnapLogLevel::Info, "buf", SnapLogExt{{"eventName", "buf"}}, SnapLogPolicy::Buffered, __FUNCTION__,
                                  __LINE__);
    REQUIRE(SnapLogClient::instance().realtime_queue_size_for_test() == 0);
    fc.shutdown();
}

TEST_CASE("log(): no eventName in ext still enqueues (unknown bucket)", "[snaplog][pipeline]")
{
    FakeClient fc;
    fc.init();
    SnapLogClient::instance().log(SnapLogLevel::Info, "no-event", SnapLogExt{{"someKey", "someVal"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                  __LINE__);
    REQUIRE(SnapLogClient::instance().realtime_queue_size_for_test() == 1);
    fc.shutdown();
}

TEST_CASE("log(): set_consent(false) after init drops events", "[snaplog][pipeline]")
{
    FakeClient fc;
    fc.init();
    // consent starts true (deps.consent_ok returns true).
    SnapLogClient::instance().log(SnapLogLevel::Info, "before", SnapLogExt{{"eventName", "x"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                  __LINE__);
    REQUIRE(SnapLogClient::instance().realtime_queue_size_for_test() == 1);
    // set_consent(false) clears the queue (spec §4.6: OFF transition purges).
    SnapLogClient::instance().set_consent(false);
    REQUIRE(SnapLogClient::instance().realtime_queue_size_for_test() == 0);
    SnapLogClient::instance().log(SnapLogLevel::Info, "after", SnapLogExt{{"eventName", "x"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                  __LINE__);
    // "after" event gated by consent → not enqueued.
    REQUIRE(SnapLogClient::instance().realtime_queue_size_for_test() == 0);
    fc.shutdown();
}

TEST_CASE("log(): empty user_token drops events (login gate)", "[snaplog][pipeline]")
{
    FakeClient fc;
    fc.init();
    SnapLogClient::instance().set_user_token(""); // ensure not logged in
    SnapLogClient::instance().log(SnapLogLevel::Info, "anon", SnapLogExt{{"eventName", "x"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                  __LINE__);
    REQUIRE(SnapLogClient::instance().realtime_queue_size_for_test() == 0);
    SnapLogClient::instance().set_user_token("tok"); // login
    SnapLogClient::instance().log(SnapLogLevel::Info, "authed", SnapLogExt{{"eventName", "x"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                  __LINE__);
    REQUIRE(SnapLogClient::instance().realtime_queue_size_for_test() == 1);
    fc.shutdown();
}

// ---------------------------------------------------------------------------
// Task 10: build_realtime_request — auth selection (Bearer vs HMAC public)
// ---------------------------------------------------------------------------

TEST_CASE("build_realtime_request: token present -> Bearer + upload/print", "[snaplog][auth]")
{
    SnapLogDeps deps;
    int64_t     ts = 1778639794110;
    deps.now_ms    = [&ts]() { return ts; };
    SnapLogConfig cfg;
    cfg.gateway_base = "https://api.snapmaker.com";
    cfg.hmac_secret  = "c25hcG1ha2VyLU9yY2E";

    RealtimeRequest req = build_realtime_request(deps, cfg, /*clientId*/ "app-123", /*token*/ "tok-xyz",
                                                 /*nonce_gen*/ []() { return std::string("9b2226b29c3e460eae7ff1d3315c522d"); });

    REQUIRE(req.url == "https://api.snapmaker.com/api/log/upload/print");
    REQUIRE(hdr(req.headers, "Authorization") == "Bearer tok-xyz");
    REQUIRE(hdr(req.headers, "X-Client-Type") == "Orca");
    REQUIRE(hdr(req.headers, "X-Client-Id") == "app-123");
    // Logged-in path must NOT carry the HMAC signing headers.
    REQUIRE(hdr(req.headers, "X-Sign") == "");
    REQUIRE(hdr(req.headers, "X-Nonce") == "");
    REQUIRE(hdr(req.headers, "X-Timestamp") == "");
}

TEST_CASE("build_realtime_request: token empty -> public + HMAC headers", "[snaplog][auth]")
{
    SnapLogDeps deps;
    int64_t     ts = 1778639794110;
    deps.now_ms    = [&ts]() { return ts; };
    SnapLogConfig cfg;
    cfg.gateway_base = "https://api.snapmaker.com";
    cfg.hmac_secret  = "c25hcG1ha2VyLU9yY2E";

    RealtimeRequest req = build_realtime_request(deps, cfg, /*clientId*/ "app-123", /*token*/ "",
                                                 /*nonce_gen*/ []() { return std::string("9b22cafe00ll00000000ff00dd00ee00"); });

    REQUIRE(req.url == "https://api.snapmaker.com/api/log/public/upload/print");
    REQUIRE(hdr(req.headers, "Authorization") == "");
    REQUIRE(hdr(req.headers, "X-Client-Type") == "Orca");
    REQUIRE(hdr(req.headers, "X-Client-Id") == "app-123");
    REQUIRE(hdr(req.headers, "X-Timestamp") == "1778639794110");
    REQUIRE(hdr(req.headers, "X-Nonce") == "9b22cafe00ll00000000ff00dd00ee00");
    // X-Sign must equal HMAC over "Orca" || clientId || ts || nonce (no sep).
    std::string expected = hmac_sha256_hex("c25hcG1ha2VyLU9yY2E",
                                           std::string("Orca") + "app-123" + "1778639794110" + "9b22cafe00ll00000000ff00dd00ee00");
    REQUIRE(hdr(req.headers, "X-Sign") == expected);
}

TEST_CASE("build_realtime_request: X-Sign reproduces doc vector", "[snaplog][auth]")
{
    // Known doc vector: clientType=Orca ... but our clientType is always "Orca".
    // Verify the formula reproduces for our fixed inputs by computing expected
    // directly. (clientType=desktop here, not "Orca" — this test pins OUR formula.)
    SnapLogDeps deps;
    int64_t     ts = 1778639794110;
    deps.now_ms    = [&ts]() { return ts; };
    SnapLogConfig cfg;
    cfg.hmac_secret = "c25hcG1ha2VyLU9yY2E";

    RealtimeRequest req = build_realtime_request(deps, cfg, "app-123", "", []() { return std::string("9b2226b29c3e460eae7ff1d3315c522d"); });

    std::string expected = hmac_sha256_hex("c25hcG1ha2VyLU9yY2E",
                                           std::string("Orca") + "app-123" + "1778639794110" + "9b2226b29c3e460eae7ff1d3315c522d");
    REQUIRE(hdr(req.headers, "X-Sign") == expected);
    REQUIRE(hdr(req.headers, "X-Sign").size() == 64); // 32 bytes lowercase hex
}

// ---------------------------------------------------------------------------
// Task 9: realtime worker poll-loop (in-flight=1, drains queue, consent stop)
// ---------------------------------------------------------------------------

namespace {
// Worker test fixture: a do_request that the test controls. Returns a handle
// whose `fulfilled` is NOT set by default; the test sets it when it wants the
// worker to consider the request done.
struct WorkerFake
{
    int64_t                                     now = 1000;
    std::atomic<int>                            request_count{0};
    std::vector<std::shared_ptr<SnapLogHandle>> live_handles;
    std::mutex                                  handles_mu;
    bool                                        auto_fulfill = false; // if true, fake completes each request immediately
    SnapLogDeps                                 deps;
    SnapLogConfig                               cfg;

    WorkerFake()
    {
        deps.do_request = [this](const std::string& /*method*/, const std::string& /*url*/,
                                 std::vector<std::pair<std::string, std::string>> /*headers*/, const boost::filesystem::path* /*body_file*/,
                                 const std::string* /*body_str*/) {
            auto h = std::make_shared<SnapLogHandle>();
            if (auto_fulfill)
                h->fulfilled.store(true);
            {
                std::lock_guard<std::mutex> lk(handles_mu);
                live_handles.push_back(h);
            }
            ++request_count;
            return h;
        };
        deps.consent_ok                    = []() { return true; };
        deps.user_token                    = []() { return std::string("tok"); };
        deps.machine_id                    = []() { return std::string("mid-client"); };
        deps.now_ms                        = [this]() { return now; };
        cfg.realtime_queue_cap             = 64;
        cfg.default_rate_cap_per_sec       = 10000;
        cfg.total_unknown_rate_cap_per_sec = 100000;
        cfg.poll_interval_ms               = 1; // fast poll for tests
    }

    void init()
    {
        SnapLogDeps   d = deps;
        SnapLogConfig c = cfg;
        SnapLogClient::instance().init(std::move(d), std::move(c));
        SnapLogClient::instance().set_user_token("tok"); // logged-in default (login gate)
    }
    void shutdown() { SnapLogClient::instance().shutdown(); }

    // Wait until the queue drains to `target` (or timeout). Returns true if met.
    bool wait_queue(std::size_t target, int timeout_ms = 2000)
    {
        auto t0 = std::chrono::steady_clock::now();
        while (SnapLogClient::instance().realtime_queue_size_for_test() > target) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > timeout_ms)
                return false;
        }
        return true;
    }
    // Wait until at least `n` requests have been issued.
    bool wait_requests(int n, int timeout_ms = 2000)
    {
        auto t0 = std::chrono::steady_clock::now();
        while (request_count.load() < n) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > timeout_ms)
                return false;
        }
        return true;
    }
    void fulfill_all_live()
    {
        std::lock_guard<std::mutex> lk(handles_mu);
        for (auto& h : live_handles)
            h->fulfilled.store(true);
    }
};
} // anonymous namespace

TEST_CASE("worker: drains queue when handles auto-fulfill", "[snaplog][worker]")
{
    WorkerFake wf;
    wf.auto_fulfill = true;
    wf.init();
    for (int i = 0; i < 5; ++i) {
        SnapLogClient::instance().log(SnapLogLevel::Info, "m", SnapLogExt{{"eventName", "w"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                      __LINE__);
    }
    REQUIRE(wf.wait_requests(5));
    REQUIRE(wf.wait_queue(0));
    REQUIRE(wf.request_count.load() == 5);
    wf.shutdown();
}

TEST_CASE("worker: in-flight=1 — blocks while handle not done", "[snaplog][worker]")
{
    WorkerFake wf;
    wf.auto_fulfill = false; // handles stay not-done
    wf.init();
    for (int i = 0; i < 5; ++i) {
        SnapLogClient::instance().log(SnapLogLevel::Info, "m", SnapLogExt{{"eventName", "w"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                      __LINE__);
    }
    // Wait a bit; exactly 1 request should fire (the first), rest queued.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(wf.request_count.load() == 1);
    REQUIRE(SnapLogClient::instance().realtime_queue_size_for_test() >= 1);

    // Now fulfill the in-flight handle — worker should send the next one.
    wf.fulfill_all_live();
    // Let remaining drain. (Each subsequent handle also stays not-done, so
    // only one more fires at a time.)
    for (int round = 2; round <= 5; ++round) {
        // After fulfilling, the worker clears the slot and sends next.
        REQUIRE(wf.wait_requests(round));
        wf.fulfill_all_live();
    }
    REQUIRE(wf.wait_queue(0));
    REQUIRE(wf.request_count.load() == 5);
    wf.shutdown();
}

TEST_CASE("worker: consent false mid-run stops further sends", "[snaplog][worker]")
{
    WorkerFake wf;
    wf.auto_fulfill = true;
    wf.init();
    // Send one event and let it drain.
    SnapLogClient::instance().log(SnapLogLevel::Info, "first", SnapLogExt{{"eventName", "w"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                  __LINE__);
    REQUIRE(wf.wait_requests(1));
    REQUIRE(wf.wait_queue(0));
    int before = wf.request_count.load();

    // Flip consent off, then push more events. Worker must NOT send them.
    SnapLogClient::instance().set_consent(false);
    for (int i = 0; i < 3; ++i) {
        SnapLogClient::instance().log(SnapLogLevel::Info, "blocked", SnapLogExt{{"eventName", "w"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                      __LINE__);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(wf.request_count.load() == before); // no new requests
    wf.shutdown();
}

TEST_CASE("worker: shutdown joins within reasonable time", "[snaplog][worker]")
{
    WorkerFake wf;
    wf.auto_fulfill = true;
    wf.init();
    for (int i = 0; i < 3; ++i) {
        SnapLogClient::instance().log(SnapLogLevel::Info, "m", SnapLogExt{{"eventName", "w"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                      __LINE__);
    }
    // shutdown() must join the worker thread and return.
    auto t0 = std::chrono::steady_clock::now();
    wf.shutdown();
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    // poll_interval=1ms so the worker sees stop_receiving quickly. Generous bound.
    REQUIRE(dt < 3000);
    REQUIRE(true); // reached without hanging
}

// ---------------------------------------------------------------------------
// Task 11: lifecycle hardening — bounded shutdown, consent gate, cancel
// ---------------------------------------------------------------------------

TEST_CASE("shutdown returns within deadline with slow handle", "[snaplog][lifecycle]")
{
    // Fake do_request returns a handle that NEVER auto-fulfills. Without
    // cancel-then-join, the worker would hold the not-done handle and block
    // shutdown indefinitely. With cancel_current before join, the handle's
    // cancelled flag trips done() so the worker exits within ~poll_interval.
    WorkerFake wf;
    wf.auto_fulfill                   = false; // handles stay not-done forever
    wf.cfg.realtime_join_deadline_sec = 2;     // short deadline for test
    wf.init();
    // Enqueue one event so the worker fires do_request and gets a pending handle.
    SnapLogClient::instance().log(SnapLogLevel::Info, "slow", SnapLogExt{{"eventName", "w"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                  __LINE__);
    REQUIRE(wf.wait_requests(1));                               // request fired
    std::this_thread::sleep_for(std::chrono::milliseconds(30)); // let worker store handle

    auto t0 = std::chrono::steady_clock::now();
    wf.shutdown();
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    // Must return well within the deadline (cancel trips done() so join is fast).
    REQUIRE(dt < 1500); // generous; typically <100ms
}

TEST_CASE("shutdown cancels in-flight handle", "[snaplog][lifecycle]")
{
    // Verify that the in-flight handle was cancelled by shutdown.
    WorkerFake wf;
    wf.auto_fulfill                   = false;
    wf.cfg.realtime_join_deadline_sec = 2;
    wf.init();
    SnapLogClient::instance().log(SnapLogLevel::Info, "m", SnapLogExt{{"eventName", "w"}}, SnapLogPolicy::Realtime, __FUNCTION__, __LINE__);
    REQUIRE(wf.wait_requests(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    wf.shutdown();
    // After shutdown, the live handle must have been cancelled.
    std::lock_guard<std::mutex> lk(wf.handles_mu);
    REQUIRE(!wf.live_handles.empty());
    bool any_cancelled = false;
    for (const auto& h : wf.live_handles) {
        if (h->cancelled.load())
            any_cancelled = true;
    }
    REQUIRE(any_cancelled);
}

TEST_CASE("consent OFF cancels in-flight and clears queue", "[snaplog][lifecycle]")
{
    WorkerFake wf;
    wf.auto_fulfill = false; // handle stays pending
    wf.init();
    // Fire one request so there's an in-flight handle.
    SnapLogClient::instance().log(SnapLogLevel::Info, "first", SnapLogExt{{"eventName", "w"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                  __LINE__);
    REQUIRE(wf.wait_requests(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Flip consent OFF. This must cancel the in-flight handle and clear queue.
    SnapLogClient::instance().set_consent(false);

    // Push more events AFTER consent OFF — they should NOT enqueue (log() gates).
    for (int i = 0; i < 3; ++i) {
        SnapLogClient::instance().log(SnapLogLevel::Info, "after-off", SnapLogExt{{"eventName", "w"}}, SnapLogPolicy::Realtime,
                                      __FUNCTION__, __LINE__);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Queue must be empty (cleared by set_consent(false) + log() gating).
    REQUIRE(SnapLogClient::instance().realtime_queue_size_for_test() == 0);

    // In-flight handle must be cancelled.
    {
        std::lock_guard<std::mutex> lk(wf.handles_mu);
        bool                        any_cancelled = false;
        for (const auto& h : wf.live_handles) {
            if (h->cancelled.load())
                any_cancelled = true;
        }
        REQUIRE(any_cancelled);
    }

    // No further requests fire while consent is OFF.
    int req_after = wf.request_count.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(wf.request_count.load() == req_after);

    wf.shutdown();
}

TEST_CASE("consent ON resumes sending after OFF", "[snaplog][lifecycle]")
{
    WorkerFake wf;
    wf.auto_fulfill = true;
    wf.init();

    // Start with consent OFF.
    SnapLogClient::instance().set_consent(false);
    SnapLogClient::instance().log(SnapLogLevel::Info, "blocked", SnapLogExt{{"eventName", "w"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                  __LINE__);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    REQUIRE(wf.request_count.load() == 0);

    // Flip consent ON — worker is still running (never exited), sends resume.
    SnapLogClient::instance().set_consent(true);
    SnapLogClient::instance().log(SnapLogLevel::Info, "resume", SnapLogExt{{"eventName", "w"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                  __LINE__);
    REQUIRE(wf.wait_requests(1));
    REQUIRE(wf.wait_queue(0));

    wf.shutdown();
}

TEST_CASE("consent OFF does not flush backlog on ON", "[snaplog][lifecycle]")
{
    // Events enqueued during OFF period must not be retroactively flushed.
    WorkerFake wf;
    wf.auto_fulfill = true;
    wf.init();

    SnapLogClient::instance().set_consent(false);
    // set_consent(false) clears queue; log() gates on consent so nothing enqueues.
    // Now flip ON and verify no stale sends.
    int before = wf.request_count.load();
    SnapLogClient::instance().set_consent(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(wf.request_count.load() == before); // no backlog flush

    wf.shutdown();
}

// ---------------------------------------------------------------------------
// Fix #3: redact_path fallback — cross-home path collapse
// When the authoritative home prefix does NOT match but the path looks like
// it is under a user-home pattern anywhere, collapse the WHOLE path to "~".
// ---------------------------------------------------------------------------

TEST_CASE("redact_path cross-home Windows path collapses to tilde", "[snaplog][priv]")
{
    // home is /home/alice (posix); path is a Windows path under a different
    // user's home (C:/Users/bob/...). Must collapse entirely — no orphan
    // drive letter, no leaked structure.
    auto out = redact_path("/home/alice", "C:/Users/bob/AppData/Roaming/x");
    REQUIRE(out == "~");
    REQUIRE(out.find("AppData") == std::string::npos);
    REQUIRE(out.find("bob") == std::string::npos);
    REQUIRE(out.find("C:") == std::string::npos);
}

TEST_CASE("redact_path different drive user-home pattern collapses to tilde", "[snaplog][priv]")
{
    // home is on C: drive; path is on D: drive under a user home.
    auto out = redact_path("C:\\Users\\bob", "D:/Users/carol/proj/m.stl");
    REQUIRE(out == "~");
    REQUIRE(out.find("carol") == std::string::npos);
    REQUIRE(out.find("proj") == std::string::npos);
}

TEST_CASE("redact_path authoritative prefix still works (regression)", "[snaplog][priv]")
{
    // Existing behavior must be preserved.
    REQUIRE(redact_path("/home/alice", "/home/alice/proj/m.stl") == "~/proj/m.stl");
    REQUIRE(redact_path("C:\\Users\\bob", "C:/Users/bob/AppData/Roaming/x") == "~/AppData/Roaming/x");
    // Non-home path preserved unchanged.
    REQUIRE(redact_path("/home/alice", "/opt/x") == "/opt/x");
}

TEST_CASE("redact_path backslash variant collapses to tilde", "[snaplog][priv]")
{
    // Backslash-only Windows path under a user home (no drive letter).
    auto out = redact_path("/home/alice", "\\Users\\dave\\secret.txt");
    REQUIRE(out == "~");
}

// ---------------------------------------------------------------------------
// Fix #4: render_realtime_body ext-value truncation caps
// Per-string ext value capped to <=2000 bytes; total ext <=8000 bytes;
// eventName (and opId) always survive even when ext must be trimmed.
// ---------------------------------------------------------------------------

TEST_CASE("render_realtime_body: huge ext value truncated, eventName survives", "[snaplog]")
{
    // 100KB ext value in "reason" + eventName in ext.
    SnapLogExt ext  = {{"reason", std::string(100000, 'x')}, {"eventName", "slice_failed"}};
    auto       body = render_realtime_body(SnapLogLevel::Error, "boom", ext, 64 * 1024);
    REQUIRE(body.size() <= 64 * 1024);
    REQUIRE(body.find("eventName") != std::string::npos);
    REQUIRE(body.find("slice_failed") != std::string::npos);
    // The huge value must have been truncated — body should be well under 8KB+overhead.
    REQUIRE(body.size() < 12000);
    REQUIRE(body.find("truncated") != std::string::npos);
}

TEST_CASE("render_realtime_body: many ext entries capped at 8KB, eventName kept", "[snaplog]")
{
    // 10 entries of ~2KB each = ~20KB, well over the 8KB ext cap.
    SnapLogExt ext;
    for (int i = 0; i < 10; ++i) {
        ext.emplace_back("k" + std::to_string(i), std::string(2000, 'a' + i));
    }
    ext.emplace_back("eventName", "op_done");
    ext.emplace_back("opId", "op-42");
    auto body = render_realtime_body(SnapLogLevel::Info, "msg", ext, 64 * 1024);
    REQUIRE(body.size() <= 64 * 1024);
    REQUIRE(body.find("eventName") != std::string::npos);
    REQUIRE(body.find("op_done") != std::string::npos);
    // opId must also survive (it's a protected key).
    REQUIRE(body.find("opId") != std::string::npos);
    REQUIRE(body.find("op-42") != std::string::npos);
}

TEST_CASE("render_realtime_body: per-value truncation keeps eventName in ext", "[snaplog]")
{
    // Single huge value; verify eventName is NOT lost due to whole-ext drop.
    SnapLogExt ext  = {{"reason", std::string(50000, 'z')}, {"eventName", "slice_failed"}, {"opId", "job-1"}};
    auto       body = render_realtime_body(SnapLogLevel::Error, "err", ext, 64 * 1024);
    // eventName and opId must be present — never dropped by the ext-size fallback.
    REQUIRE(body.find("eventName") != std::string::npos);
    REQUIRE(body.find("slice_failed") != std::string::npos);
    REQUIRE(body.find("opId") != std::string::npos);
    REQUIRE(body.find("job-1") != std::string::npos);
    // The reason value must be truncated.
    REQUIRE(body.find("truncated") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Plan B Task 1: render_batch_line — NDJSON line renderer (full field set)
// Pure function: produces one NDJSON line (JSON object + trailing '\n').
// Spec §4.1.2/§4.1.3. Privacy enforcement point (mirrors realtime renderer).
// ---------------------------------------------------------------------------

TEST_CASE("render_batch_line has core mandatory fields", "[snaplog][batch]")
{
    BatchLineContext ctx;
    ctx.clientId  = "c1";
    ctx.nodeId    = "n1";
    ctx.eventName = "slice_completed";
    ctx.batchId   = "b1";
    ctx.opId      = "o1";
    ctx.logger    = "slice";
    ctx.thread    = "main";
    ctx.ts_ms     = 1778639794110;

    std::string line = render_batch_line(SnapLogLevel::Info, "done", SnapLogExt{{"durationMs", "100"}}, 64 * 1024, ctx);
    REQUIRE(line.find("\"timestamp\"") != std::string::npos);
    REQUIRE(line.find("\"level\":\"INFO\"") != std::string::npos);
    REQUIRE(line.find("\"service\":\"snapmaker-desktop\"") != std::string::npos);
    REQUIRE(line.find("\"message\":\"done\"") != std::string::npos);
    REQUIRE(line.find("\"eventName\":\"slice_completed\"") != std::string::npos);
    // Fix B: batchId lives in ext, NOT at top-level (strict server mapping).
    {
        auto nl = line.back() == '\n' ? line.substr(0, line.size() - 1) : line;
        auto j  = nlohmann::json::parse(nl);
        REQUIRE(!j.contains("batchId"));       // not top-level
        REQUIRE(j["ext"].contains("batchId")); // in ext
        REQUIRE(j["ext"]["batchId"] == "b1");
    }
    REQUIRE(line.find("\"clientType\":\"Orca\"") != std::string::npos);
    // Optional context fields are trimmed; see the trims test below.
    REQUIRE(line.find("\"logger\"") == std::string::npos);
    REQUIRE(line.find("\"thread\"") == std::string::npos);
    REQUIRE(line.find("\"requestId\"") == std::string::npos);
    REQUIRE(line.find("\"nodeId\"") == std::string::npos);
    REQUIRE(line.back() == '\n'); // NDJSON: line ends with newline
}

TEST_CASE("render_batch_line ISO8601 timestamp UTC from ts_ms", "[snaplog][batch]")
{
    BatchLineContext ctx;
    ctx.ts_ms = 1778639794110; // 2026-04-12T13:16:34.110Z
    auto line = render_batch_line(SnapLogLevel::Info, "m", {}, 64 * 1024, ctx);
    // YYYY-MM-DDThh:mm:ss.sssZ — manual formatting, UTC.
    REQUIRE(line.find("\"timestamp\":\"2026-") != std::string::npos);
    REQUIRE(line.find("T") != std::string::npos);
    REQUIRE(line.find(".110Z\"") != std::string::npos);
}

TEST_CASE("render_batch_line trims non-required context fields", "[snaplog][batch]")
{
    BatchLineContext ctx;
    ctx.userId      = "u1";
    ctx.deviceId    = "d1";
    ctx.appVersion  = "2.5.0";
    ctx.appBuild    = "12345";
    ctx.platform    = "win64";
    ctx.osVersion   = "10.0.19045";
    ctx.sessionId   = "s1";
    ctx.region      = "us";
    ctx.nodeId      = "n1";
    ctx.logger      = "lg";
    ctx.thread      = "th";
    ctx.opId        = "o1";
    ctx.eventName   = "slice_completed";
    auto line       = render_batch_line(SnapLogLevel::Info, "m", {}, 64 * 1024, ctx);
    // Keep the wire-required and functionally required fields.
    REQUIRE(line.find("\"eventName\":\"slice_completed\"") != std::string::npos);
    REQUIRE(line.find("\"userId\":\"u1\"") != std::string::npos);
    // Drop optional diagnostic fields that the gateway does not require.
    REQUIRE(line.find("\"logger\"") == std::string::npos);
    REQUIRE(line.find("\"thread\"") == std::string::npos);
    REQUIRE(line.find("\"requestId\"") == std::string::npos);
    REQUIRE(line.find("\"nodeId\"") == std::string::npos);
    REQUIRE(line.find("\"deviceId\"") == std::string::npos);
    REQUIRE(line.find("\"appVersion\"") == std::string::npos);
    REQUIRE(line.find("\"appBuild\"") == std::string::npos);
    REQUIRE(line.find("\"platform\"") == std::string::npos);
    REQUIRE(line.find("\"osVersion\"") == std::string::npos);
    REQUIRE(line.find("\"environment\"") == std::string::npos);
    REQUIRE(line.find("\"sessionId\"") == std::string::npos);
    REQUIRE(line.find("\"region\"") == std::string::npos);
}

TEST_CASE("render_batch_line ext has clientUUID + event-specific keys", "[snaplog][batch]")
{
    BatchLineContext ctx;
    ctx.clientId  = "my-client";
    ctx.eventName = "e1";
    auto line     = render_batch_line(SnapLogLevel::Info, "m", SnapLogExt{{"durationMs", "100"}}, 64 * 1024, ctx);
    // The InstanceID value is emitted in ext as clientUUID (renamed per spec).
    REQUIRE(line.find("\"clientUUID\":\"my-client\"") != std::string::npos);
    REQUIRE(line.find("\"durationMs\":\"100\"") != std::string::npos);
}

TEST_CASE("render_batch_line rejects raw* and masks secrets, truncates to cap", "[snaplog][batch][priv]")
{
    BatchLineContext ctx;
    ctx.eventName = "x";
    auto line = render_batch_line(SnapLogLevel::Error, "boom", SnapLogExt{{"rawGcode", "G28"}, {"password", "hunter2"}}, 64 * 1024, ctx);
    REQUIRE(line.find("rawGcode") == std::string::npos);
    REQUIRE(line.find("hunter2") == std::string::npos);
    std::string big(200000, 'x');
    auto        l2 = render_batch_line(SnapLogLevel::Info, big, {}, 1024, ctx);
    REQUIRE(l2.size() <= 2048);
    REQUIRE(l2.find("truncated") != std::string::npos);
}

TEST_CASE("render_batch_line redacts path-like ext values", "[snaplog][batch][priv]")
{
    BatchLineContext ctx;
    ctx.clientId        = "c1";
    ctx.eventName       = "e1";
    ctx.home_for_redact = "C:/Users/bob";
    auto line           = render_batch_line(SnapLogLevel::Info, "m", SnapLogExt{{"modelPath", "C:/Users/bob/secret.stl"}}, 64 * 1024, ctx);
    // home prefix must be stripped from path-like ext values.
    REQUIRE(line.find("C:/Users/bob") == std::string::npos);
    REQUIRE(line.find("~") != std::string::npos);
}

TEST_CASE("render_batch_line hashes printerSerial PII", "[snaplog][batch][priv]")
{
    BatchLineContext ctx;
    ctx.clientId  = "c1";
    ctx.eventName = "e1";
    auto line     = render_batch_line(SnapLogLevel::Info, "m", SnapLogExt{{"printerSerial", "SN12345"}}, 64 * 1024, ctx);
    // printerSerial must be hashed (16 hex chars), not the raw serial.
    REQUIRE(line.find("SN12345") == std::string::npos);
    REQUIRE(line.find("\"printerSerial\":\"") != std::string::npos);
}

TEST_CASE("ext carries the standard identity fields", "[snaplog][batch]")
{
    SECTION("batch: userId top-level; standard identity in ext")
    {
        BatchLineContext ctx;
        ctx.clientId   = "inst-uuid";
        ctx.processId  = "12345";
        ctx.eventName  = "e1";
        ctx.userId     = "8751";
        ctx.deviceId   = "dev-99";
        ctx.appVersion = "Orca 1.2.3";
        ctx.osVersion  = "Windows 11";
        ctx.batchId    = "ff00aabbccddeeff";
        ctx.ts_ms      = 0; // non-positive -> localTime ""

        auto line = render_batch_line(SnapLogLevel::Info, "m", {}, 64 * 1024, ctx);
        // Strip trailing newline and parse to tell top-level apart from ext.
        std::string    json_str = line.substr(0, line.size() - 1);
        nlohmann::json parsed   = nlohmann::json::parse(json_str);

        // userId stays top-level; deviceId is omitted from batch payloads.
        REQUIRE_FALSE(parsed.contains("deviceId"));
        REQUIRE(parsed.contains("userId"));
        REQUIRE(parsed["userId"] == "8751");
        REQUIRE_FALSE(parsed["ext"].contains("deviceId"));
        REQUIRE_FALSE(parsed["ext"].contains("userId"));

        // Remaining identity fields live in ext.
        REQUIRE(parsed["ext"]["clientUUID"] == "inst-uuid");
        REQUIRE(parsed["ext"]["processId"] == "12345");
        REQUIRE(parsed["ext"]["orcaVersion"] == "Orca 1.2.3");
        REQUIRE(parsed["ext"]["os"] == "Windows 11");
        REQUIRE(parsed["ext"]["batchId"] == "ff00aabbccddeeff");
        REQUIRE(parsed["ext"].contains("connect_clientid"));
        REQUIRE(parsed["ext"].contains("printerSN"));

        ctx.connect_clientid = "mqtt-cid-42";
        ctx.print_sn         = "SN0099";
        auto line2           = render_batch_line(SnapLogLevel::Info, "m", {}, 64 * 1024, ctx);
        REQUIRE(line2.find("\"connect_clientid\":\"mqtt-cid-42\"") != std::string::npos);
        REQUIRE(line2.find("SN0099") == std::string::npos);
        REQUIRE(line2.find("\"printerSN\":\"") != std::string::npos);
    }

    SECTION("realtime: full identity set in ext, incl. deviceId/userId")
    {
        SnapLogExt ext;
        // Default RealtimeIdentity -> every ext identity key present as "".
        auto empty_body = render_realtime_body(SnapLogLevel::Info, "m", ext, 64 * 1024);
        for (const char* k : {"clientUUID", "connect_clientid", "printerSN", "deviceId", "userId", "orcaVersion", "os", "batchId",
                              "localTime", "processId"}) {
            std::string needle = std::string("\"") + k + "\":\"\"";
            REQUIRE(empty_body.find(needle) != std::string::npos);
        }

        RealtimeIdentity id;
        id.client_id        = "inst-uuid";
        id.process_id       = "12346";
        id.connect_clientid = "mqtt-cid-7";
        id.printer_sn       = "SN123";
        id.device_id        = "dev-x";
        id.user_id          = "8751";
        id.orca_version     = "Orca 1.2.3";
        id.os               = "Windows 11";
        id.batch_id         = "ff00aabbccddeeff";
        id.ts_ms            = 0; // localTime "" for non-positive ts
        auto body           = render_realtime_body(SnapLogLevel::Info, "m", ext, 64 * 1024, id);
        REQUIRE(body.find("\"clientUUID\":\"inst-uuid\"") != std::string::npos);
        REQUIRE(body.find("\"processId\":\"12346\"") != std::string::npos);
        REQUIRE(body.find("\"connect_clientid\":\"mqtt-cid-7\"") != std::string::npos);
        REQUIRE(body.find("SN123") == std::string::npos);
        REQUIRE(body.find("\"printerSN\":\"") != std::string::npos);
        REQUIRE(body.find("\"deviceId\":\"dev-x\"") != std::string::npos);
        REQUIRE(body.find("\"userId\":\"8751\"") != std::string::npos);
        REQUIRE(body.find("\"orcaVersion\":\"Orca 1.2.3\"") != std::string::npos);
        REQUIRE(body.find("\"os\":\"Windows 11\"") != std::string::npos);
        REQUIRE(body.find("\"batchId\":\"ff00aabbccddeeff\"") != std::string::npos);
        // Top-level clientId envelope (API contract) is preserved.
        REQUIRE(body.find("\"clientId\":\"inst-uuid\"") != std::string::npos);
    }
}

TEST_CASE("SnapLogClient: log does not wait for a bounded shutdown join", "[snaplog][client]")
{
    std::atomic<bool> request_entered{false};
    std::atomic<bool> release_request{false};
    SnapLogDeps       deps;
    deps.consent_ok = []() { return true; };
    deps.now_ms     = []() { return int64_t(0); };
    deps.do_request = [&request_entered, &release_request](const std::string&, const std::string&,
                                                           std::vector<std::pair<std::string, std::string>>, const boost::filesystem::path*,
                                                           const std::string*) {
        request_entered.store(true);
        while (!release_request.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto h  = std::make_shared<SnapLogHandle>();
        h->prom = std::make_shared<std::promise<SnapLogResult>>();
        h->fulfilled.store(true);
        h->prom->set_value({200, std::string{}, false});
        return h;
    };

    SnapLogConfig cfg;
    cfg.poll_interval_ms           = 1;
    cfg.realtime_join_deadline_sec = 1;
    SnapLogClient::instance().init(std::move(deps), std::move(cfg));
    SnapLogClient::instance().set_user_token("token");
    SnapLogClient::instance().log(SnapLogLevel::Info, "blocked", {{"eventName", "test"}}, SnapLogPolicy::Realtime, __FUNCTION__, __LINE__);
    while (!request_entered.load())
        std::this_thread::yield();

    std::thread closer([]() { SnapLogClient::instance().shutdown(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto start = std::chrono::steady_clock::now();
    SnapLogClient::instance().log(SnapLogLevel::Info, "not-blocked", {{"eventName", "test"}}, SnapLogPolicy::Realtime, __FUNCTION__,
                                  __LINE__);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    REQUIRE(elapsed < 500);

    release_request.store(true);
    closer.join();
}

TEST_CASE("render_batch_line output is valid JSON + trailing newline", "[snaplog][batch]")
{
    BatchLineContext ctx;
    ctx.eventName = "e1";
    auto line     = render_batch_line(SnapLogLevel::Info, "hello \"world\"", {}, 64 * 1024, ctx);
    REQUIRE(line.back() == '\n');
    // Strip the trailing newline and parse as JSON.
    std::string    json_str = line.substr(0, line.size() - 1);
    nlohmann::json parsed   = nlohmann::json::parse(json_str); // throws on invalid
    REQUIRE(parsed.is_object());
    REQUIRE(parsed["message"] == "hello \"world\"");
    REQUIRE(parsed["level"] == "INFO");
}

// ---------------------------------------------------------------------------
// Plan B Task 2: spool file primitives — sanitize_file_name, append_line,
// rotate_active, list_sealed, sealed_total_bytes.
// Uses boost::filesystem temp dir with scope-guard cleanup.
// ---------------------------------------------------------------------------

#include <boost/filesystem.hpp>
#include <fstream>

namespace {
// RAII scope guard: removes a directory tree on destruction.
struct TempSpoolGuard
{
    boost::filesystem::path dir;
    explicit TempSpoolGuard(boost::filesystem::path d) : dir(std::move(d)) { boost::filesystem::create_directories(dir); }
    ~TempSpoolGuard()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(dir, ec);
    }
};
} // anonymous namespace

TEST_CASE("sanitize_file_name strips dangerous chars and caps length", "[snaplog][batch]")
{
    REQUIRE(sanitize_file_name("test<script>.log").find('<') == std::string::npos);
    REQUIRE(sanitize_file_name("test<script>.log").find('>') == std::string::npos);
    REQUIRE(sanitize_file_name(std::string(300, 'a')).size() <= 128);
    REQUIRE(!sanitize_file_name("").empty());
    REQUIRE(!sanitize_file_name("///\\\\").empty()); // all-unsafe -> non-empty fallback
    // Control chars removed.
    REQUIRE(sanitize_file_name("a\x01b\x02c").find('\x01') == std::string::npos);
    REQUIRE(sanitize_file_name("a\x01b\x02c").find('\x02') == std::string::npos);
    // Pipe and question mark removed.
    REQUIRE(sanitize_file_name("a|b?c*d").find('|') == std::string::npos);
    REQUIRE(sanitize_file_name("a|b?c*d").find('?') == std::string::npos);
    REQUIRE(sanitize_file_name("a|b?c*d").find('*') == std::string::npos);
}

TEST_CASE("sanitize_file_name collapses whitespace", "[snaplog][batch]")
{
    auto out = sanitize_file_name("hello   world\ttab");
    REQUIRE(out.find("  ") == std::string::npos); // no double spaces
}

TEST_CASE("append_line writes content and creates parent dir", "[snaplog][batch]")
{
    namespace fs         = boost::filesystem;
    auto           spool = fs::temp_directory_path() / fs::unique_path("snaplog-%%%%.dir");
    TempSpoolGuard guard(spool);
    auto           active = spool / "active.log";

    REQUIRE(append_line(active, "first line\n"));
    REQUIRE(append_line(active, "second line\n"));
    REQUIRE(fs::exists(active));
    REQUIRE(fs::file_size(active) == 23); // "first line\n" + "second line\n"

    // Read back and verify.
    std::ifstream ifs(active.string());
    std::string   content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    REQUIRE(content == "first line\nsecond line\n");
}

TEST_CASE("append_line creates nested missing parent dir", "[snaplog][batch]")
{
    namespace fs         = boost::filesystem;
    auto           spool = fs::temp_directory_path() / fs::unique_path("snaplog-%%%%.dir");
    TempSpoolGuard guard(spool);
    auto           active = spool / "sub" / "deep" / "active.log";

    REQUIRE(append_line(active, "data\n"));
    REQUIRE(fs::exists(active));
}

TEST_CASE("rotate_active renames non-empty active and keeps active_path", "[snaplog][batch]")
{
    namespace fs         = boost::filesystem;
    auto           spool = fs::temp_directory_path() / fs::unique_path("snaplog-%%%%.dir");
    TempSpoolGuard guard(spool);
    auto           active = spool / "active.log";
    {
        std::ofstream o(active.string());
        o << "line1\n";
    }

    std::string             err;
    boost::filesystem::path ap = active;
    // Inject deterministic clock for reproducible filename.
    int64_t fake_ts = 1700000000000;
    auto    clock   = [&fake_ts]() { return fake_ts; };
    REQUIRE(rotate_active(spool, ap, "batch1", err, clock));
    REQUIRE(err.empty());
    REQUIRE(ap == active);        // active_path still points at active.log
    REQUIRE(!fs::exists(active)); // active was renamed away

    auto sealed = list_sealed(spool);
    REQUIRE(sealed.size() == 1);
    REQUIRE(sealed[0].filename().string().find("batch1") != std::string::npos);
    REQUIRE(sealed[0].filename().string().find("1700000000000") != std::string::npos);
}

TEST_CASE("rotate_active no-op on absent active", "[snaplog][batch]")
{
    namespace fs         = boost::filesystem;
    auto           spool = fs::temp_directory_path() / fs::unique_path("snaplog-%%%%.dir");
    TempSpoolGuard guard(spool);
    auto           active = spool / "active.log";

    std::string             err;
    boost::filesystem::path ap = active;
    REQUIRE(rotate_active(spool, ap, "b1", err));
    REQUIRE(err.empty());
    REQUIRE(list_sealed(spool).empty());
}

TEST_CASE("rotate_active no-op on empty active", "[snaplog][batch]")
{
    namespace fs         = boost::filesystem;
    auto           spool = fs::temp_directory_path() / fs::unique_path("snaplog-%%%%.dir");
    TempSpoolGuard guard(spool);
    auto           active = spool / "active.log";
    {
        std::ofstream o(active.string());
    } // create empty file

    std::string             err;
    boost::filesystem::path ap = active;
    REQUIRE(rotate_active(spool, ap, "b1", err));
    REQUIRE(err.empty());
    REQUIRE(list_sealed(spool).empty());
}

TEST_CASE("list_sealed returns sorted by ts then batchId", "[snaplog][batch]")
{
    namespace fs         = boost::filesystem;
    auto           spool = fs::temp_directory_path() / fs::unique_path("snaplog-%%%%.dir");
    TempSpoolGuard guard(spool);

    // Create three sealed files with different timestamps.
    fs::ofstream(spool / "batch.3000.zzz.sealed") << "x";
    fs::ofstream(spool / "batch.1000.aaa.sealed") << "x";
    fs::ofstream(spool / "batch.2000.bbb.sealed") << "x";
    // Non-sealed file should be ignored.
    fs::ofstream(spool / "active.log") << "y";
    fs::ofstream(spool / "notasealed.txt") << "z";

    auto sealed = list_sealed(spool);
    REQUIRE(sealed.size() == 3);
    // Sorted lexicographically by filename (ts dominates).
    REQUIRE(sealed[0].filename().string() == "batch.1000.aaa.sealed");
    REQUIRE(sealed[1].filename().string() == "batch.2000.bbb.sealed");
    REQUIRE(sealed[2].filename().string() == "batch.3000.zzz.sealed");
}

TEST_CASE("sealed_total_bytes sums all sealed files", "[snaplog][batch]")
{
    namespace fs         = boost::filesystem;
    auto           spool = fs::temp_directory_path() / fs::unique_path("snaplog-%%%%.dir");
    TempSpoolGuard guard(spool);

    fs::ofstream(spool / "batch.1000.a.sealed") << "12345"; // 5 bytes
    fs::ofstream(spool / "batch.2000.b.sealed") << "abc";   // 3 bytes
    fs::ofstream(spool / "active.log") << "active data";    // not counted

    REQUIRE(sealed_total_bytes(spool, nullptr) == 8);
}

TEST_CASE("sealed_total_bytes excludes in-flight path", "[snaplog][batch]")
{
    namespace fs         = boost::filesystem;
    auto           spool = fs::temp_directory_path() / fs::unique_path("snaplog-%%%%.dir");
    TempSpoolGuard guard(spool);

    fs::ofstream(spool / "batch.1000.a.sealed") << "12345"; // 5 bytes
    auto in_flight = spool / "batch.2000.b.sealed";
    fs::ofstream(in_flight) << "abc"; // 3 bytes — should be excluded

    REQUIRE(sealed_total_bytes(spool, &in_flight) == 5);
}

TEST_CASE("rotate_active handles Windows target-exists collision", "[snaplog][batch]")
{
    // Two rotates in quick succession with the same batchId + clock must not
    // collide: the second should succeed with a suffix.
    namespace fs         = boost::filesystem;
    auto           spool = fs::temp_directory_path() / fs::unique_path("snaplog-%%%%.dir");
    TempSpoolGuard guard(spool);
    auto           active = spool / "active.log";

    int64_t fake_ts = 1700000000000;
    auto    clock   = [&fake_ts]() { return fake_ts; };

    // First rotation.
    {
        std::ofstream o(active.string());
        o << "first\n";
    }
    std::string             err;
    boost::filesystem::path ap = active;
    REQUIRE(rotate_active(spool, ap, "batch1", err, clock));
    REQUIRE(list_sealed(spool).size() == 1);

    // Second rotation with SAME ts (simulates collision).
    {
        std::ofstream o(active.string());
        o << "second\n";
    }
    REQUIRE(rotate_active(spool, ap, "batch1", err, clock));
    auto sealed = list_sealed(spool);
    REQUIRE(sealed.size() == 2); // both sealed files exist
}

// ---------------------------------------------------------------------------
// Plan B Task 3: batch create/complete/cancel request building
// Pure functions that produce BatchRequest { url, headers, body } for the
// three file-upload lifecycle endpoints. Auth selection mirrors realtime:
// token non-empty -> Bearer; empty -> HMAC public path.
// ---------------------------------------------------------------------------

TEST_CASE("build_batch_create_request: Bearer -> /api/log/upload/create", "[snaplog][batch]")
{
    SnapLogDeps deps;
    int64_t     ts = 1778639794110;
    deps.now_ms    = [&ts]() { return ts; };
    SnapLogConfig cfg;
    cfg.gateway_base = "https://api.snapmaker.com";
    cfg.hmac_secret  = "c25hcG1ha2VyLU9yY2E";

    BatchRequest req = build_batch_create_request(
        deps, cfg, /*clientId*/ "app-123", /*token*/ "tok-xyz",
        /*nonce_gen*/ []() { return std::string("9b2226b29c3e460eae7ff1d3315c522d"); },
        /*fileName*/ "batch.123.log");

    REQUIRE(req.url == "https://api.snapmaker.com/api/log/upload/create");
    REQUIRE(hdr(req.headers, "Authorization") == "Bearer tok-xyz");
    REQUIRE(hdr(req.headers, "X-Client-Type") == "Orca");
    REQUIRE(hdr(req.headers, "X-Client-Id") == "app-123");
    // Logged-in path must NOT carry the HMAC signing headers.
    REQUIRE(hdr(req.headers, "X-Sign") == "");
    REQUIRE(hdr(req.headers, "X-Nonce") == "");
    REQUIRE(hdr(req.headers, "X-Timestamp") == "");
    // Body has fileName, no checkSum.
    REQUIRE(req.body.find("\"fileName\"") != std::string::npos);
    REQUIRE(req.body.find("batch.123.log") != std::string::npos);
    REQUIRE(req.body.find("checkSum") == std::string::npos);
}

TEST_CASE("build_batch_create_request: empty token -> public + HMAC headers", "[snaplog][batch]")
{
    SnapLogDeps deps;
    int64_t     ts = 1778639794110;
    deps.now_ms    = [&ts]() { return ts; };
    SnapLogConfig cfg;
    cfg.gateway_base = "https://api.snapmaker.com";
    cfg.hmac_secret  = "c25hcG1ha2VyLU9yY2E";

    BatchRequest req = build_batch_create_request(
        deps, cfg, /*clientId*/ "app-123", /*token*/ "",
        /*nonce_gen*/ []() { return std::string("9b22cafe00ll00000000ff00dd00ee00"); },
        /*fileName*/ "mylog.ndjson");

    REQUIRE(req.url == "https://api.snapmaker.com/api/log/public/upload/create");
    REQUIRE(hdr(req.headers, "Authorization") == "");
    REQUIRE(hdr(req.headers, "X-Client-Type") == "Orca");
    REQUIRE(hdr(req.headers, "X-Client-Id") == "app-123");
    REQUIRE(hdr(req.headers, "X-Timestamp") == "1778639794110");
    REQUIRE(hdr(req.headers, "X-Nonce") == "9b22cafe00ll00000000ff00dd00ee00");
    // X-Sign must equal HMAC over "Orca" || clientId || ts || nonce (no sep).
    // Same rule as realtime — the value must be identical for identical inputs.
    std::string expected = hmac_sha256_hex("c25hcG1ha2VyLU9yY2E",
                                           std::string("Orca") + "app-123" + "1778639794110" + "9b22cafe00ll00000000ff00dd00ee00");
    REQUIRE(hdr(req.headers, "X-Sign") == expected);
    REQUIRE(hdr(req.headers, "X-Sign").size() == 64);
    // Body still has fileName.
    REQUIRE(req.body.find("\"fileName\"") != std::string::npos);
    REQUIRE(req.body.find("mylog.ndjson") != std::string::npos);
}

TEST_CASE("build_batch_create_request: X-Sign matches realtime for same inputs", "[snaplog][batch]")
{
    // The signing rule is identical between realtime and batch — verify by
    // computing the realtime X-Sign with the same inputs and asserting equality.
    SnapLogDeps deps;
    int64_t     ts = 1778639794110;
    deps.now_ms    = [&ts]() { return ts; };
    SnapLogConfig cfg;
    cfg.hmac_secret = "c25hcG1ha2VyLU9yY2E";

    RealtimeRequest rt = build_realtime_request(deps, cfg, "app-123", "", []() { return std::string("9b2226b29c3e460eae7ff1d3315c522d"); });

    BatchRequest bq =
        build_batch_create_request(deps, cfg, "app-123", "", []() { return std::string("9b2226b29c3e460eae7ff1d3315c522d"); }, "f.log");

    REQUIRE(hdr(rt.headers, "X-Sign") == hdr(bq.headers, "X-Sign"));
}

TEST_CASE("build_batch_complete_request: single-part body carries fileName, URL ends /completed", "[snaplog][batch]")
{
    SnapLogDeps deps;
    int64_t     ts = 1778639794110;
    deps.now_ms    = [&ts]() { return ts; };
    SnapLogConfig cfg;
    cfg.gateway_base = "https://api.snapmaker.com";
    cfg.hmac_secret  = "secret";

    BatchRequest req = build_batch_complete_request(
        deps, cfg, "app-123", "tok", []() { return std::string("nonce123"); },
        /*full_key*/ "logs/2026/batch.abc.ndjson");

    REQUIRE(req.url == "https://api.snapmaker.com/api/log/upload/completed");
    REQUIRE(hdr(req.headers, "Authorization") == "Bearer tok");
    // completed requires fileName (create's returned S3 key). Server returns
    // 110004 "missing fileName" for body={} (was misread as auth-dead; sealed
    // then stranded on disk after retry exhaustion).
    REQUIRE(req.body.find("\"fileName\":\"logs/2026/batch.abc.ndjson\"") != std::string::npos);
    REQUIRE(req.body.find("uploadId") == std::string::npos);
}

TEST_CASE("build_batch_complete_request: empty token -> public completed", "[snaplog][batch]")
{
    SnapLogDeps deps;
    deps.now_ms = []() { return int64_t(100); };
    SnapLogConfig cfg;
    cfg.gateway_base = "https://gw.example.com";

    BatchRequest req = build_batch_complete_request(deps, cfg, "cid", "", []() { return std::string("n"); }, "key1");

    REQUIRE(req.url == "https://gw.example.com/api/log/public/upload/completed");
    REQUIRE(hdr(req.headers, "Authorization") == "");
    REQUIRE(hdr(req.headers, "X-Sign") != "");
}

TEST_CASE("build_batch_cancel_request: empty uploadId -> body {}", "[snaplog][batch]")
{
    SnapLogDeps deps;
    int64_t     ts = 1778639794110;
    deps.now_ms    = [&ts]() { return ts; };
    SnapLogConfig cfg;
    cfg.gateway_base = "https://api.snapmaker.com";

    BatchRequest req = build_batch_cancel_request(
        deps, cfg, "app-123", "tok", []() { return std::string("n"); },
        /*uploadId*/ "");

    REQUIRE(req.url == "https://api.snapmaker.com/api/log/upload/cancel");
    REQUIRE(hdr(req.headers, "Authorization") == "Bearer tok");
    // Empty uploadId -> body is "{}".
    nlohmann::json parsed = nlohmann::json::parse(req.body);
    REQUIRE(parsed.is_object());
    REQUIRE(parsed.empty());
}

TEST_CASE("build_batch_cancel_request: non-empty uploadId -> body has it", "[snaplog][batch]")
{
    SnapLogDeps deps;
    deps.now_ms = []() { return int64_t(0); };
    SnapLogConfig cfg;

    BatchRequest req = build_batch_cancel_request(
        deps, cfg, "cid", "tok", []() { return std::string("n"); },
        /*uploadId*/ "upload-xyz");

    REQUIRE(req.url.find("/cancel") != std::string::npos);
    nlohmann::json parsed = nlohmann::json::parse(req.body);
    REQUIRE(parsed["uploadId"] == "upload-xyz");
}

TEST_CASE("build_batch_cancel_request: empty token -> public cancel + HMAC", "[snaplog][batch]")
{
    SnapLogDeps deps;
    int64_t     ts = 42;
    deps.now_ms    = [&ts]() { return ts; };
    SnapLogConfig cfg;
    cfg.gateway_base = "https://gw.example.com";
    cfg.hmac_secret  = "sec";

    BatchRequest req = build_batch_cancel_request(deps, cfg, "cid", "", []() { return std::string("nonce1"); }, "uid1");

    REQUIRE(req.url == "https://gw.example.com/api/log/public/upload/cancel");
    REQUIRE(hdr(req.headers, "Authorization") == "");
    REQUIRE(hdr(req.headers, "X-Sign") != "");
    REQUIRE(hdr(req.headers, "X-Timestamp") == "42");
    REQUIRE(hdr(req.headers, "X-Nonce") == "nonce1");
}

// ---------------------------------------------------------------------------
// Plan B Task 4: Internals batch fields + log() Buffered routing
// Buffered events go through the same admission pipeline (consent, disable
// list, rate limiter) but land in bt_queue instead of rt_queue. Eviction on
// a full batch queue is FIFO-drop-incoming (NOT the realtime Error-protect
// policy — batch is a disk channel, no Error privilege).
// ---------------------------------------------------------------------------

TEST_CASE("batch log(): SNAP_LOG_BATCH lands in bt_queue", "[snaplog][batch]")
{
    FakeClient fc;
    fc.init();
    SnapLogClient::instance().log(SnapLogLevel::Info, "batch-event", SnapLogExt{{"eventName", "batch_test"}}, SnapLogPolicy::Buffered,
                                  __FUNCTION__, __LINE__);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 1);
    REQUIRE(SnapLogClient::instance().realtime_queue_size_for_test() == 0);
    fc.shutdown();
}

TEST_CASE("batch log(): consent false drops event", "[snaplog][batch]")
{
    FakeClient fc;
    fc.deps.consent_ok = []() { return false; };
    fc.init();
    SnapLogClient::instance().log(SnapLogLevel::Info, "no-consent", SnapLogExt{{"eventName", "batch_test"}}, SnapLogPolicy::Buffered,
                                  __FUNCTION__, __LINE__);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 0);
    fc.shutdown();
}

TEST_CASE("batch log(): disabled event name not enqueued", "[snaplog][batch]")
{
    FakeClient fc;
    fc.cfg.event_disable_list.push_back("blocked_batch");
    fc.init();
    SnapLogClient::instance().log(SnapLogLevel::Info, "blocked", SnapLogExt{{"eventName", "blocked_batch"}}, SnapLogPolicy::Buffered,
                                  __FUNCTION__, __LINE__);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 0);
    // A non-disabled event IS enqueued.
    SnapLogClient::instance().log(SnapLogLevel::Info, "ok", SnapLogExt{{"eventName", "ok_batch"}}, SnapLogPolicy::Buffered, __FUNCTION__,
                                  __LINE__);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 1);
    fc.shutdown();
}

TEST_CASE("batch log(): rate limit throttles beyond cap", "[snaplog][batch]")
{
    FakeClient fc;
    fc.cfg.event_rate_cap_per_sec["batch_throttled"] = 2;
    fc.init();
    for (int i = 0; i < 3; ++i) {
        SnapLogClient::instance().log(SnapLogLevel::Info, "msg", SnapLogExt{{"eventName", "batch_throttled"}}, SnapLogPolicy::Buffered,
                                      __FUNCTION__, __LINE__);
    }
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 2);
    fc.shutdown();
}

TEST_CASE("batch log(): full queue drops incoming + increments counter", "[snaplog][batch]")
{
    FakeClient fc;
    fc.cfg.batch_queue_cap = 4;
    fc.init();
    for (std::size_t i = 0; i < 4; ++i) {
        SnapLogClient::instance().log(SnapLogLevel::Info, "fill", SnapLogExt{{"eventName", "batch_fill"}}, SnapLogPolicy::Buffered,
                                      __FUNCTION__, __LINE__);
    }
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 4);
    REQUIRE(SnapLogClient::instance().batch_queue_dropped_for_test() == 0);

    // 5th event: queue full → drop incoming (FIFO-drop, NOT Error-protect).
    SnapLogClient::instance().log(SnapLogLevel::Error, "overflow", SnapLogExt{{"eventName", "batch_over"}}, SnapLogPolicy::Buffered,
                                  __FUNCTION__, __LINE__);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 4);
    REQUIRE(SnapLogClient::instance().batch_queue_dropped_for_test() == 1);
    fc.shutdown();
}

TEST_CASE("batch log(): full queue drops Error too (no Error privilege)", "[snaplog][batch]")
{
    // Batch queue uses FIFO-drop-incoming, NOT the realtime Error-protect policy.
    // Even an incoming Error on a full batch queue is dropped.
    FakeClient fc;
    fc.cfg.batch_queue_cap = 2;
    fc.init();
    // Fill with Info events.
    SnapLogClient::instance().log(SnapLogLevel::Info, "i1", SnapLogExt{{"eventName", "b"}}, SnapLogPolicy::Buffered, __FUNCTION__, __LINE__);
    SnapLogClient::instance().log(SnapLogLevel::Info, "i2", SnapLogExt{{"eventName", "b"}}, SnapLogPolicy::Buffered, __FUNCTION__, __LINE__);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 2);
    // Error on full batch queue → dropped, NOT admitted via eviction.
    SnapLogClient::instance().log(SnapLogLevel::Error, "err", SnapLogExt{{"eventName", "b"}}, SnapLogPolicy::Buffered, __FUNCTION__,
                                  __LINE__);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 2);
    REQUIRE(SnapLogClient::instance().batch_queue_dropped_for_test() == 1);
    fc.shutdown();
}

TEST_CASE("batch log(): config disabled drops event", "[snaplog][batch]")
{
    FakeClient fc;
    fc.cfg.enabled = false;
    fc.init();
    SnapLogClient::instance().log(SnapLogLevel::Info, "disabled", SnapLogExt{{"eventName", "batch_test"}}, SnapLogPolicy::Buffered,
                                  __FUNCTION__, __LINE__);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 0);
    fc.shutdown();
}

// ---------------------------------------------------------------------------
// Plan B Task 5 (Part A): bt_worker_loop skeleton
// Drains bt_queue -> active.log, rotates to sealed at thresholds, evicts
// oldest sealed under backpressure, and exits on the right conditions.
// Upload is stubbed (attempt_upload returns false); sealed accumulate.
// Tests drive bt_worker_loop directly on a fake Internals (temp spool dir).
// ---------------------------------------------------------------------------

// Helper: build a fake Internals wired for a batch flusher test.
// Returns the Internals + the temp spool dir (caller owns cleanup via guard).
namespace {
struct BatchFlusherFixture
{
    boost::filesystem::path                   spool;
    TempSpoolGuard                            guard;
    std::shared_ptr<SnapLogClient::Internals> in;

    BatchFlusherFixture()
        : spool(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("snaplog-bt-%%%%.dir")), guard(spool)
    {
        SnapLogDeps deps;
        int64_t     ts  = 1700000000000;
        deps.now_ms     = [&ts]() { return ts++; };
        deps.consent_ok = []() { return true; };
        deps.user_token = []() { return std::string("tok"); };
        deps.user_id    = []() { return std::string("uid-1"); };
        deps.device_id  = []() { return std::string("did-1"); };
        deps.machine_id = []() { return std::string("mid-xyz"); };

        SnapLogConfig cfg;
        cfg.poll_interval_ms     = 1;
        cfg.drain_batch_per_tick = 256;
        cfg.drain_wall_budget_ms = 50;
        cfg.active_max_bytes     = 1024; // small for rotate test
        cfg.batch_max_events     = 1000; // high — size-based rotate primary
        cfg.batch_max_bytes      = 1 * 1024 * 1024;
        cfg.sealed_max_disk_mb   = 1; // 1 MB cap for backpressure test
        cfg.line_max_bytes       = 64 * 1024;
        cfg.home_for_redact      = "/home/test";

        in                     = SnapLogClient::make_internals_for_test(std::move(deps), std::move(cfg));
        in->spool_dir_resolved = spool;
        in->active_path        = spool / "active.log";
        in->stop_receiving.store(false);
        in->drain_and_flush.store(false);
        in->current_batch_id = "batch00000001";
        in->events_in_active = 0;
    }

    void enqueue(const std::string& msg, const std::string& eventName = "bt_test")
    {
        std::lock_guard<std::mutex> lk(in->queue_mu);
        in->bt_queue.push_back(
            SnapLogClient::RtEvent{SnapLogLevel::Info, msg, SnapLogExt{{"eventName", eventName}, {"opId", "op-" + msg}}, "bt_test_logger"});
    }
};
} // anonymous namespace

TEST_CASE("bt_worker_loop drains bt_queue into active.log", "[snaplog][batch]")
{
    BatchFlusherFixture f;
    f.enqueue("event-one");
    f.enqueue("event-two");
    // stop_receiving + empty queue after drain -> loop exits.
    f.in->stop_receiving.store(true);

    SnapLogClient::bt_worker_loop(f.in);

    // active.log should exist and contain both events.
    REQUIRE(boost::filesystem::exists(f.in->active_path));
    std::ifstream ifs(f.in->active_path.string());
    std::string   content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    REQUIRE(content.find("event-one") != std::string::npos);
    REQUIRE(content.find("event-two") != std::string::npos);
    // No sealed files (nothing rotated).
    REQUIRE(list_sealed(f.spool).empty());
    // Queue is empty.
    std::size_t qsize;
    {
        std::lock_guard<std::mutex> qk(f.in->queue_mu);
        qsize = f.in->bt_queue.size();
    }
    REQUIRE(qsize == 0);
}

TEST_CASE("bt_worker_loop rotates active to sealed at active_max_bytes", "[snaplog][batch]")
{
    BatchFlusherFixture f;
    // active_max_bytes is 1024. Each line is ~300+ bytes. Enqueue ~5 events
    // so the active exceeds 1024 bytes and rotates.
    for (int i = 0; i < 6; ++i) {
        f.enqueue(std::string(200, 'x') + std::to_string(i));
    }
    f.in->stop_receiving.store(true);

    SnapLogClient::bt_worker_loop(f.in);

    // After rotate, at least one sealed file should exist.
    auto sealed = list_sealed(f.spool);
    REQUIRE(!sealed.empty());
    // active.log should still exist (new batch after rotate, possibly empty
    // or with the tail events that didn't trigger a second rotate).
    // The sealed file should contain the rotated content.
    std::ifstream ifs(sealed[0].string());
    std::string   sealed_content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    REQUIRE(!sealed_content.empty());
    // batchId in the sealed filename should match the initial batch id.
    REQUIRE(sealed[0].filename().string().find("batch00000001") != std::string::npos);
}

TEST_CASE("bt_worker_loop rotates at batch_max_events", "[snaplog][batch]")
{
    BatchFlusherFixture f;
    // Set a tiny event cap so we trigger rotate by count, not size.
    f.in->cfg.batch_max_events = 3;
    f.in->cfg.active_max_bytes = 10 * 1024 * 1024; // huge — count is the trigger
    f.enqueue("a");
    f.enqueue("b");
    f.enqueue("c");
    f.enqueue("d");
    f.in->stop_receiving.store(true);

    SnapLogClient::bt_worker_loop(f.in);

    auto sealed = list_sealed(f.spool);
    REQUIRE(!sealed.empty()); // at least one sealed after 3 events
}

TEST_CASE("bt_worker_loop backpressure deletes oldest sealed over cap", "[snaplog][batch]")
{
    BatchFlusherFixture f;
    // Set a tiny sealed cap (1 MB default). Create sealed files totaling > 1MB
    // manually so we don't depend on drain timing, then run the loop with an
    // empty queue + drain_and_flush to force a single tick that checks pressure.
    namespace fs = boost::filesystem;
    // Create 3 sealed files, each ~500KB, totaling 1.5MB > 1MB cap.
    for (int i = 0; i < 3; ++i) {
        auto         p = f.spool / ("batch." + std::to_string(1000000 + i) + ".id" + std::to_string(i) + ".sealed");
        fs::ofstream ofs(p.string(), std::ios::binary);
        std::string  data(500 * 1024, static_cast<char>('a' + i));
        ofs.write(data.data(), data.size());
    }
    REQUIRE(list_sealed(f.spool).size() == 3);
    uint64_t before = f.in->sealed_evicted.load();

    // drain_and_flush with empty queue + no in-flight + sealed exist but
    // auth_known_dead=false. The loop won't exit immediately because sealed
    // exist and auth is alive — but it WILL run one backpressure check tick
    // before re-evaluating exit. To force exit after that, set auth_known_dead.
    // Actually: the exit condition says "no sealed || auth_known_dead". Since
    // we want it to exit AND we want backpressure to run, we set auth_known_dead
    // so the exit condition (drained + no inflight + sealed_exist + auth_dead)
    // fires. But backpressure runs AFTER the exit check in the loop body...
    // So instead: use stop_receiving (not drain_and_flush) + empty queue. The
    // loop body runs once (drain finds nothing, rotate no-op, upload stub,
    // backpressure deletes oldest), THEN the next iteration's top sees
    // stop_receiving + empty queue -> exit.
    f.in->stop_receiving.store(true);

    SnapLogClient::bt_worker_loop(f.in);

    // At least one sealed (the oldest) should have been evicted.
    uint64_t after = f.in->sealed_evicted.load();
    REQUIRE(after > before);
    // sealed count should be <= 3 (at least one deleted).
    auto sealed = list_sealed(f.spool);
    REQUIRE(sealed.size() <= 2);
}

TEST_CASE("bt_worker_loop drain_and_flush exits after draining queue", "[snaplog][batch]")
{
    BatchFlusherFixture f;
    f.enqueue("flush-1");
    f.enqueue("flush-2");
    // drain_and_flush + auth_known_dead: exit when queue empty + no in-flight.
    // auth_known_dead makes the "sealed exist" branch of the exit condition
    // short-circuit to true so we don't block on upload.
    f.in->drain_and_flush.store(true);
    f.in->auth_known_dead.store(true);

    SnapLogClient::bt_worker_loop(f.in);

    // Queue drained.
    std::size_t qsize;
    {
        std::lock_guard<std::mutex> qk(f.in->queue_mu);
        qsize = f.in->bt_queue.size();
    }
    REQUIRE(qsize == 0);
    // Events landed in active.log.
    REQUIRE(boost::filesystem::exists(f.in->active_path));
    std::ifstream ifs(f.in->active_path.string());
    std::string   content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    REQUIRE(content.find("flush-1") != std::string::npos);
    REQUIRE(content.find("flush-2") != std::string::npos);
}

TEST_CASE("bt_worker_loop exits immediately on consent false", "[snaplog][batch]")
{
    BatchFlusherFixture f;
    f.enqueue("never-drained");
    f.in->consent.store(false);

    SnapLogClient::bt_worker_loop(f.in);

    // Consent-false exits WITHOUT draining. active.log should not exist.
    REQUIRE(!boost::filesystem::exists(f.in->active_path));
    std::size_t qsize;
    {
        std::lock_guard<std::mutex> qk(f.in->queue_mu);
        qsize = f.in->bt_queue.size();
    }
    REQUIRE(qsize == 1);
}

TEST_CASE("bt_worker_loop renders batchId + eventName in output lines", "[snaplog][batch]")
{
    BatchFlusherFixture f;
    f.enqueue("tagged-event", "slice_done");
    f.in->stop_receiving.store(true);

    SnapLogClient::bt_worker_loop(f.in);

    std::ifstream ifs(f.in->active_path.string());
    std::string   content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    REQUIRE(content.find("\"eventName\":\"slice_done\"") != std::string::npos);
    REQUIRE(content.find("\"batchId\":\"batch00000001\"") != std::string::npos);
    // clientUUID from machine_id snapshot.
    REQUIRE(content.find("\"clientUUID\":\"mid-xyz\"") != std::string::npos);
}

// ---- Plan B Task 5 (Part B): batch upload state machine tests ---------------
//
// These tests drive bt_worker_loop against a FAKE deps.do_request whose
// responses are fully scripted per (method, url-suffix). The fake builds a real
// SnapLogHandle and immediately fulfills it (fulfilled=true + prom->set_value)
// with the scripted SnapLogResult, so the flusher sees done()==true on its next
// poll tick — fully deterministic, no sleeps. Each call is recorded in a log so
// tests can assert create/PUT/completed/cancel ordering and the token carried.

namespace {

// One recorded do_request call. token_in_auth is the Bearer value ("" if none).
struct FakeBatchCall
{
    std::string             method;
    std::string             url;
    std::string             token_in_auth; // Bearer value if Authorization header present
    boost::filesystem::path body_file;     // PUT body file path (empty for POSTs)
    std::string             body_str;      // POST body string
};

// Scripted response keyed by request kind. kind is inferred from method+url:
// "create" if url ends /create, "completed" if /completed, "cancel" if /cancel,
// "put" if method==PUT.
struct FakeBatchScript
{
    // Return the SnapLogResult to fulfill the handle with. Default: {200,""}.
    std::function<SnapLogResult(const std::string& kind)> respond;
};

struct BatchUploadFixture
{
    boost::filesystem::path                   spool;
    TempSpoolGuard                            guard;
    std::shared_ptr<SnapLogClient::Internals> in;

    std::mutex                 calls_mu;
    std::vector<FakeBatchCall> calls; // appended per do_request (under calls_mu)
    std::atomic<int>           create_count{0};
    std::atomic<int>           put_count{0};
    std::atomic<int>           completed_count{0};
    std::atomic<int>           cancel_count{0};

    // The token the flusher reads from Internals->user_token. Mutable so the
    // token-freeze test can flip it mid-flight.
    std::string live_token{"tok-A"};

    FakeBatchScript script;

    // Classify a (method,url) into a kind string.
    static std::string classify(const std::string& method, const std::string& url)
    {
        if (method == "PUT")
            return "put";
        if (url.find("/create") != std::string::npos && url.find("/completed") == std::string::npos)
            return "create";
        if (url.find("/completed") != std::string::npos)
            return "completed";
        if (url.find("/cancel") != std::string::npos)
            return "cancel";
        return "other";
    }

    BatchUploadFixture()
        : spool(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("snaplog-bt-up-%%%%.dir")), guard(spool)
    {
        SnapLogDeps deps;
        int64_t     ts  = 1700000000000;
        deps.now_ms     = [&ts]() { return ts++; };
        deps.consent_ok = []() { return true; };
        deps.user_token = [this]() { return this->live_token; };
        deps.user_id    = []() { return std::string("uid-1"); };
        deps.device_id  = []() { return std::string("did-1"); };
        deps.machine_id = []() { return std::string("mid-xyz"); };

        // Scripted do_request: classify, record, build handle, fulfill.
        deps.do_request = [this](const std::string& method, const std::string& url,
                                 std::vector<std::pair<std::string, std::string>> headers, const boost::filesystem::path* body_file,
                                 const std::string* body_str) {
            std::string kind = classify(method, url);
            std::string tok;
            for (const auto& kv : headers) {
                std::string k = kv.first;
                for (auto& c : k)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (k == "authorization") {
                    // "Bearer <token>"
                    const std::string pref = "Bearer ";
                    if (kv.second.rfind(pref, 0) == 0)
                        tok = kv.second.substr(pref.size());
                    else
                        tok = kv.second;
                    break;
                }
            }
            {
                std::lock_guard<std::mutex> lk(calls_mu);
                calls.push_back(
                    {method, url, tok, body_file ? *body_file : boost::filesystem::path(), body_str ? *body_str : std::string()});
            }
            if (kind == "create")
                ++create_count;
            else if (kind == "put")
                ++put_count;
            else if (kind == "completed")
                ++completed_count;
            else if (kind == "cancel")
                ++cancel_count;

            auto h  = std::make_shared<SnapLogHandle>();
            h->prom = std::make_shared<std::promise<SnapLogResult>>();
            SnapLogResult res{200, std::string{}, false};
            if (script.respond)
                res = script.respond(kind);
            h->fulfilled.store(true);
            h->prom->set_value(res);
            return h;
        };

        SnapLogConfig cfg;
        cfg.poll_interval_ms        = 1;
        cfg.drain_batch_per_tick    = 256;
        cfg.drain_wall_budget_ms    = 50;
        cfg.active_max_bytes        = 1024;
        cfg.batch_max_events        = 1000;
        cfg.batch_max_bytes         = 1 * 1024 * 1024;
        cfg.sealed_max_disk_mb      = 256; // big — backpressure off in upload tests
        cfg.sealed_max_upload_bytes = 5 * 1024 * 1024;
        cfg.batch_max_retry         = 3;
        cfg.line_max_bytes          = 64 * 1024;
    cfg.home_for_redact         = "/home/test";

        in                     = SnapLogClient::make_internals_for_test(std::move(deps), std::move(cfg));
        in->spool_dir_resolved = spool;
        in->active_path        = spool / "active.log";
        in->stop_receiving.store(false);
        in->drain_and_flush.store(false);
        in->current_batch_id = "batch00000001";
        in->events_in_active = 0;
        // Seed the token Internals->user_token (flusher freezes from here).
        {
            std::lock_guard<std::mutex> tk(in->token_mu);
            in->user_token = live_token;
        }
    }

    // Create a sealed file directly in the spool (skip the drain+rotate path).
    boost::filesystem::path make_sealed(const std::string& batchId, const std::string& content = "x")
    {
        namespace fs = boost::filesystem;
        // Use a distinct timestamp per call so names differ.
        int64_t      ts = 1700000000000 + static_cast<int64_t>(calls.size());
        auto         p  = spool / ("batch." + std::to_string(ts) + "." + batchId + ".sealed");
        fs::ofstream ofs(p.string(), std::ios::binary);
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        ofs.close();
        return p;
    }

    // Wait until `pred` is true (polls calls_mu), up to timeout_ms.
    bool wait_until(std::function<bool()> pred, int timeout_ms = 3000)
    {
        auto t0 = std::chrono::steady_clock::now();
        while (true) {
            {
                std::lock_guard<std::mutex> lk(calls_mu);
                if (pred())
                    return true;
            }
            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > timeout_ms)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

// Helper: build a create-success JSON body with a presigned url + key.
static std::string create_ok_body(const std::string& url, const std::string& key)
{
    nlohmann::json j;
    j["code"] = 200;
    j["data"] = nlohmann::json{{"method", "PUT"}, {"url", url}, {"key", key}};
    return j.dump();
}
static std::string ok_body() { return nlohmann::json{{"code", 200}, {"msg", "ok"}}.dump(); }
static std::string err_body(int code) { return nlohmann::json{{"code", code}, {"msg", "bad"}}.dump(); }

} // anonymous namespace

TEST_CASE("bt upload: happy path create->PUT->completed deletes sealed", "[snaplog][batch]")
{
    BatchUploadFixture f;
    auto               sealed = f.make_sealed("deadbeef", "hello-bytes");
    // Script: create -> 200 + {url,key}; put -> 200; completed -> 200 + code 200.
    f.script.respond = [](const std::string& kind) -> SnapLogResult {
        if (kind == "create")
            return {200, create_ok_body("https://s3.example/presigned-put", "logs/fullkey.ndjson"), false};
        if (kind == "put")
            return {200, std::string{}, false};
        if (kind == "completed")
            return {200, ok_body(), false};
        return {200, std::string{}, false}; // cancel (not expected here)
    };

    // stop_receiving + drain_and_flush so the loop exits after the upload.
    f.in->stop_receiving.store(true);
    f.in->drain_and_flush.store(true);

    // Run the loop in a thread (it will exit once sealed is consumed + uploaded).
    std::thread t([&]() { SnapLogClient::bt_worker_loop(f.in); });

    // Wait for: completed call seen + sealed file gone.
    REQUIRE(f.wait_until([&]() { return f.completed_count.load() > 0; }));
    REQUIRE(f.wait_until([&]() { return !boost::filesystem::exists(sealed); }));

    // Nudge exit if needed (drain_and_flush exit needs queue empty + no inflight +
    // no sealed/auth_dead). sealed is gone now so exit should fire.
    if (t.joinable())
        t.join();

    REQUIRE(f.create_count.load() >= 1);
    REQUIRE(f.put_count.load() >= 1);
    REQUIRE(f.completed_count.load() >= 1);
    REQUIRE(f.cancel_count.load() == 0);
    // Sealed deleted on success.
    REQUIRE(!boost::filesystem::exists(sealed));
    // Counter bumped.
    REQUIRE(f.in->bt_upload_sealed_deleted.load() >= 1);
}

TEST_CASE("bt upload: create 401 sets auth_known_dead + skips create on later ticks", "[snaplog][batch]")
{
    BatchUploadFixture f;
    auto               sealed = f.make_sealed("aaa111");
    // create returns 401.
    f.script.respond = [](const std::string& kind) -> SnapLogResult {
        if (kind == "create")
            return {401, err_body(401), false};
        return {200, std::string{}, false};
    };
    // Do NOT set stop/drain yet — let the loop run so create fires, auth_known_dead
    // flips, and subsequent ticks hit the skip branch (bumping the counter).

    std::thread t([&]() { SnapLogClient::bt_worker_loop(f.in); });

    // create fires, returns 401 -> auth_known_dead set.
    REQUIRE(f.wait_until([&]() { return f.in->auth_known_dead.load(); }));
    REQUIRE(f.wait_until([&]() { return f.create_count.load() >= 1; }));
    // Subsequent ticks: attempt_upload hits the auth_known_dead skip branch and
    // bumps auth_dead_create_skipped. The loop keeps running (consent true, not
    // stop_receiving, queue empty -> no exit fires) so we observe the skip.
    REQUIRE(f.wait_until([&]() { return f.in->auth_dead_create_skipped.load() >= 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(f.in->auth_dead_create_skipped.load() == 1);

    // Now force exit: stop_receiving + empty queue -> step-1 exit.
    f.in->stop_receiving.store(true);
    if (t.joinable())
        t.join();

    REQUIRE(f.in->auth_known_dead.load());
    // create was NOT retried (401 is terminal, not a retry path).
    REQUIRE(f.create_count.load() == 1);
    // cancel/PUT/completed never happened.
    REQUIRE(f.put_count.load() == 0);
    REQUIRE(f.completed_count.load() == 0);
    // Sealed retained (at-least-once: retried next session).
    REQUIRE(boost::filesystem::exists(sealed));
    // Skip counter incremented on subsequent ticks (auth_known_dead -> skip create).
    REQUIRE(f.in->auth_dead_create_skipped.load() >= 1);
}

TEST_CASE("bt upload: PUT fails -> cancel called + retry re-creates fresh URL", "[snaplog][batch]")
{
    BatchUploadFixture f;
    auto               sealed = f.make_sealed("bbb222");
    f.in->current_batch_id    = "new-active-batch";
    int        create_seen    = 0;
    std::mutex m;
    // First create ok; PUT fails; retry create ok again; PUT ok; completed ok.
    f.script.respond = [&](const std::string& kind) -> SnapLogResult {
        if (kind == "create") {
            std::lock_guard<std::mutex> lk(m);
            ++create_seen;
            return {200,
                    create_ok_body("https://s3.example/u" + std::to_string(create_seen), "key" + std::to_string(create_seen) + ".ndjson"),
                    false};
        }
        if (kind == "put") {
            std::lock_guard<std::mutex> lk(m);
            // First PUT fails, subsequent PUTs succeed.
            static int put_seen = 0;
            ++put_seen;
            if (put_seen == 1)
                return {500, std::string{}, false};
            return {200, std::string{}, false};
        }
        if (kind == "completed")
            return {200, ok_body(), false};
        return {200, std::string{}, false}; // cancel
    };

    std::thread t([&]() { SnapLogClient::bt_worker_loop(f.in); });

    // Expect: at least 2 creates (retry), a cancel call, and sealed deleted.
    REQUIRE(f.wait_until([&]() { return f.cancel_count.load() >= 1; }));
    REQUIRE(f.wait_until([&]() { return f.create_count.load() >= 2; }));
    REQUIRE(f.wait_until([&]() { return !boost::filesystem::exists(sealed); }));

    // Force exit.
    f.in->stop_receiving.store(true);
    f.in->drain_and_flush.store(true);
    if (t.joinable())
        t.join();

    REQUIRE(f.create_count.load() >= 2); // retried create with fresh URL
    REQUIRE(f.cancel_count.load() >= 1); // cancel fired on PUT failure
    REQUIRE(!boost::filesystem::exists(sealed));
    REQUIRE(f.in->bt_upload_retries.load() >= 1);

    std::vector<std::string> create_file_names;
    {
        std::lock_guard<std::mutex> lk(f.calls_mu);
        for (const auto& call : f.calls) {
            if (BatchUploadFixture::classify(call.method, call.url) == "create") {
                create_file_names.push_back(nlohmann::json::parse(call.body_str).at("fileName").get<std::string>());
            }
        }
    }
    REQUIRE(create_file_names.size() >= 2);
    for (const auto& file_name : create_file_names) {
        REQUIRE(file_name.find("/bbb222.") != std::string::npos);
        REQUIRE(file_name.find("new-active-batch") == std::string::npos);
    }
}

TEST_CASE("bt upload: completed fails -> sealed retained (at-least-once)", "[snaplog][batch]")
{
    BatchUploadFixture f;
    // Lower retry cap so the loop terminates after exhausting retries.
    f.in->cfg.batch_max_retry = 1;
    auto sealed               = f.make_sealed("ccc333");
    // create ok, PUT ok, completed always fails (code != 200).
    f.script.respond = [](const std::string& kind) -> SnapLogResult {
        if (kind == "create")
            return {200, create_ok_body("https://s3.example/u", "k.ndjson"), false};
        if (kind == "put")
            return {200, std::string{}, false};
        if (kind == "completed")
            return {200, err_body(500), false}; // 2xx but code!=200
        return {200, std::string{}, false};     // cancel
    };

    std::thread t([&]() { SnapLogClient::bt_worker_loop(f.in); });

    // Wait until at least one completed attempt + a cancel fire, retries exhaust.
    REQUIRE(f.wait_until([&]() { return f.completed_count.load() >= 1; }));
    REQUIRE(f.wait_until([&]() { return f.cancel_count.load() >= 1; }));
    // Retries exhaust (batch_max_retry=1): after attempt 1 fails, no more retries.
    // Give the loop a moment to settle, then force exit.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    f.in->stop_receiving.store(true);
    f.in->drain_and_flush.store(true);
    f.in->auth_known_dead.store(true); // force drain_and_flush exit (sealed exist)
    if (t.joinable())
        t.join();

    // Sealed retained (completed never succeeded).
    REQUIRE(boost::filesystem::exists(sealed));
    REQUIRE(f.in->bt_upload_sealed_retained.load() >= 1);
    REQUIRE(f.completed_count.load() >= 1);
}

TEST_CASE("bt upload: token frozen across create+completed (mid-flight change ignored)", "[snaplog][batch]")
{
    BatchUploadFixture f;
    auto               sealed = f.make_sealed("ddd444");
    // Track the token seen per kind. create should freeze "tok-A"; after create,
    // the test flips live_token to "tok-B", but completed must still use "tok-A".
    std::mutex  m;
    std::string create_tok, completed_tok;
    bool        create_done = false;
    f.script.respond        = [&](const std::string& kind) -> SnapLogResult {
        // Record the token from the recorded call (calls is appended before this
        // by the do_request closure; read the last call's token).
        std::string last_tok;
        {
            std::lock_guard<std::mutex> lk(f.calls_mu);
            if (!f.calls.empty())
                last_tok = f.calls.back().token_in_auth;
        }
        if (kind == "create") {
            std::lock_guard<std::mutex> lk(m);
            create_tok  = last_tok;
            create_done = true;
            return {200, create_ok_body("https://s3.example/u", "k.ndjson"), false};
        }
        if (kind == "put")
            return {200, std::string{}, false};
        if (kind == "completed") {
            // Flip the live token AFTER create but BEFORE completed uses it.
            // If frozen, completed_tok should be the pre-create value.
            std::lock_guard<std::mutex> lk(m);
            completed_tok = last_tok;
            return {200, ok_body(), false};
        }
        return {200, std::string{}, false};
    };

    std::thread t([&]() { SnapLogClient::bt_worker_loop(f.in); });

    // After create fires, flip the live token Internals sees.
    REQUIRE(f.wait_until([&]() { return f.create_count.load() >= 1; }));
    {
        std::lock_guard<std::mutex> tk(f.in->token_mu);
        f.in->user_token = "tok-B";
    }

    REQUIRE(f.wait_until([&]() { return f.completed_count.load() >= 1; }));
    REQUIRE(f.wait_until([&]() { return !boost::filesystem::exists(sealed); }));
    f.in->stop_receiving.store(true);
    f.in->drain_and_flush.store(true);
    if (t.joinable())
        t.join();

    // Both create and completed must carry the FROZEN token (tok-A), even though
    // user_token was flipped to tok-B before completed fired.
    REQUIRE(create_tok == "tok-A");
    REQUIRE(completed_tok == "tok-A");
}

TEST_CASE("bt upload: deps_invalid drains queue with cached_context + skips create", "[snaplog][batch]")
{
    // Spec §4.6: the batch flusher, on deps_invalid, must keep draining the
    // queue to active.log using the CACHED context (no Deps calls) and skip
    // uploads (no create). This lets a detached post-timeout flusher persist
    // queued events without touching destructed GUI objects. We prime the cache
    // manually (as a real first drain tick would), enqueue events, set
    // deps_invalid, and verify they still land on disk with no do_request calls.
    BatchUploadFixture f;
    // Prime the cached context exactly as a normal drain tick would.
    f.in->machine_id_snapshot = "mid-xyz";
    {
        BatchLineContext& c        = f.in->cached_batch_context;
        c.clientId                 = "mid-xyz";
        c.service                  = "snapmaker-desktop";
        c.clientType               = "Orca";
        c.batchId                  = f.in->current_batch_id;
        c.thread                   = "bt-worker";
        c.home_for_redact          = "/home/test";
        f.in->cached_context_valid = true;
    }

    // Enqueue events that must be drained under deps_invalid.
    {
        std::lock_guard<std::mutex> lk(f.in->queue_mu);
        f.in->bt_queue.push_back(SnapLogClient::RtEvent{SnapLogLevel::Info, "deps-invalid-event-1",
                                                        SnapLogExt{{"eventName", "di"}, {"opId", "op-di1"}}, "di_logger"});
        f.in->bt_queue.push_back(SnapLogClient::RtEvent{SnapLogLevel::Info, "deps-invalid-event-2",
                                                        SnapLogExt{{"eventName", "di"}, {"opId", "op-di2"}}, "di_logger"});
    }

    // Flip deps_invalid. attempt_upload must skip (no create); the drain must
    // use the cached context. drain_and_flush + auth_known_dead so the loop
    // exits once the queue drains (deps_invalid exit also fires on empty queue).
    f.in->deps_invalid.store(true);
    f.in->drain_and_flush.store(true);
    f.in->auth_known_dead.store(true);

    SnapLogClient::bt_worker_loop(f.in);

    // Events drained to active.log using the cached context.
    std::ifstream ifs(f.in->active_path.string());
    std::string   content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    REQUIRE(content.find("deps-invalid-event-1") != std::string::npos);
    REQUIRE(content.find("deps-invalid-event-2") != std::string::npos);
    REQUIRE(content.find("\"batchId\":\"batch00000001\"") != std::string::npos);
    // Queue drained.
    std::size_t qsize;
    {
        std::lock_guard<std::mutex> qk(f.in->queue_mu);
        qsize = f.in->bt_queue.size();
    }
    REQUIRE(qsize == 0);
    // No upload create attempted under deps_invalid.
    REQUIRE(f.create_count.load() == 0);
}

TEST_CASE("bt upload: backpressure excludes in-flight sealed from eviction", "[snaplog][batch]")
{
    // Create a BatchUploadFixture with a small sealed cap, one in-flight sealed
    // (phase != Idle), and other sealed pushing over cap. The in-flight sealed
    // must NOT be evicted; an older NON-in-flight sealed should be.
    BatchUploadFixture f;
    f.in->cfg.sealed_max_disk_mb = 1; // 1MB cap so backpressure triggers easily
    // create ok + hang (don't fulfill quickly) — but our fake auto-fulfills, so
    // to keep the in-flight "pinned" we just set the phase fields directly to
    // simulate an upload in progress, then run one backpressure tick.
    namespace fs  = boost::filesystem;
    auto inflight = f.make_sealed("INFLIGHT", std::string(600 * 1024, 'I'));
    auto older    = f.make_sealed("OLDER000", std::string(600 * 1024, 'O'));
    // Make 'older' sort BEFORE 'inflight' by renaming with a small timestamp.
    // make_sealed uses ts = base + calls.size(); the first-created has smaller ts,
    // so 'inflight' (created first) is actually older by name. To get a clear
    // distinction, recreate: remove both, make older with smaller ts explicitly.
    fs::remove(inflight);
    fs::remove(older);
    // Two non-in-flight sealed files (600KB each = 1.2MB > 1MB cap when
    // excluding in-flight) + one in-flight (600KB). Backpressure must evict
    // the oldest non-in-flight, not the in-flight one.
    auto older_path    = f.spool / "batch.1700000000000.OLDER000.sealed";
    auto mid_path      = f.spool / "batch.1700000000005.MID00000.sealed";
    auto inflight_path = f.spool / "batch.1700000000009.INFLIGHT.sealed";
    for (const auto& p : {older_path, mid_path, inflight_path}) {
        char         fill = (p == older_path) ? 'O' : (p == mid_path) ? 'M' : 'I';
        fs::ofstream ofs(p.string(), std::ios::binary);
        std::string  data(600 * 1024, fill);
        ofs.write(data.data(), data.size());
    }
    REQUIRE(fs::exists(inflight_path));
    REQUIRE(fs::exists(older_path));

    // Simulate an upload in progress: set phase != Idle + in_flight_sealed.
    f.in->bt_upload_phase     = SnapLogClient::Internals::BtUploadPhase::AwaitPut;
    f.in->bt_in_flight_sealed = inflight_path;

    // Empty queue + stop_receiving so the loop runs one tick (backpressure at
    // step 0 evicts OLDER, then step-1 exit fires).
    f.in->stop_receiving.store(true);

    // Run the loop: it should evict the OLDER sealed (not the in-flight one),
    // then exit on stop_receiving+empty-queue.
    std::thread t([&]() { SnapLogClient::bt_worker_loop(f.in); });
    if (t.joinable())
        t.join();

    // OLDER evicted, INFLIGHT retained.
    REQUIRE(!fs::exists(older_path));
    REQUIRE(fs::exists(inflight_path));
    REQUIRE(f.in->sealed_evicted.load() >= 1);
}

// ---------------------------------------------------------------------------
// Plan B Task 6: lifecycle — init recovery / shutdown drain / consent purge
// These tests drive the REAL singleton (init/shutdown/set_consent) with a
// temp spool dir + fake scripted deps. They verify:
//   1. init() recovery: non-empty active.log -> rotated to sealed; sealed
//      uploaded + deleted via the fake create/PUT/completed script.
//   2. shutdown() drain: events in bt_queue -> drained to active.log -> rotated
//      to sealed -> uploaded + deleted before bt_worker exits.
//   3. consent OFF -> spool purged (remove_all + recreate); no further create
//      calls after consent goes false.
//   4. consent ON (after OFF) -> bt_queue cleared + bt_worker restarted.
// ---------------------------------------------------------------------------

namespace {

// Lifecycle fixture: builds a FakeClient-like setup but wires a temp spool_dir
// into cfg so init() resolves it and starts bt_worker. The do_request script
// auto-fulfills create/PUT/completed so sealed files get uploaded + deleted.
struct BatchLifecycleFixture
{
    boost::filesystem::path spool;
    TempSpoolGuard          guard;
    int64_t                 now = 1700000000000;
    std::atomic<int>        create_count{0};
    std::atomic<int>        put_count{0};
    std::atomic<int>        completed_count{0};
    std::atomic<int>        cancel_count{0};
    SnapLogDeps             deps;
    SnapLogConfig           cfg;

    BatchLifecycleFixture()
        : spool(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("snaplog-bt-lc-%%%%.dir")), guard(spool)
    {
        deps.now_ms     = [this]() { return now++; };
        deps.consent_ok = []() { return true; };
        deps.user_token = []() { return std::string("tok"); };
        deps.user_id    = []() { return std::string("uid-1"); };
        deps.device_id  = []() { return std::string("did-1"); };
        deps.machine_id = []() { return std::string("mid-xyz"); };
        deps.do_request = [this](const std::string& method, const std::string&                                                url,
                                 std::vector<std::pair<std::string, std::string>> /*headers*/, const boost::filesystem::path* body_file,
                                 const std::string* /*body_str*/) {
            std::string kind;
            if (method == "PUT")
                kind = "put";
            else if (url.find("/create") != std::string::npos && url.find("/completed") == std::string::npos)
                kind = "create";
            else if (url.find("/completed") != std::string::npos)
                kind = "completed";
            else if (url.find("/cancel") != std::string::npos)
                kind = "cancel";
            else
                kind = "other";

            if (kind == "create")
                ++create_count;
            else if (kind == "put")
                ++put_count;
            else if (kind == "completed")
                ++completed_count;
            else if (kind == "cancel")
                ++cancel_count;

            (void) body_file; // unused in the fake

            auto h  = std::make_shared<SnapLogHandle>();
            h->prom = std::make_shared<std::promise<SnapLogResult>>();
            h->fulfilled.store(true);
            SnapLogResult res{200, std::string{}, false};
            if (kind == "create") {
                nlohmann::json j;
                j["code"] = 200;
                j["data"] = nlohmann::json{{"method", "PUT"}, {"url", "https://s3.example/presigned"}, {"key", "logs/fullkey.ndjson"}};
                res.body  = j.dump();
            } else if (kind == "completed") {
                res.body = nlohmann::json{{"code", 200}, {"msg", "ok"}}.dump();
            }
            h->prom->set_value(res);
            return h;
        };

        cfg.poll_interval_ms        = 1;
        cfg.drain_batch_per_tick    = 256;
        cfg.drain_wall_budget_ms    = 50;
        cfg.active_max_bytes        = 512; // trimmed lines ~270B: 1 event (<512) keeps active.log, 3 events (>512) rotate
        cfg.batch_max_events        = 1000;
        cfg.batch_max_bytes         = 1 * 1024 * 1024;
        cfg.sealed_max_disk_mb      = 256;
        cfg.batch_max_retry         = 3;
        cfg.line_max_bytes          = 64 * 1024;
        cfg.home_for_redact         = "/home/test";
        cfg.spool_dir               = spool.string(); // wire the temp spool
        cfg.batch_join_deadline_sec = 5;              // short for tests
    }

    void init()
    {
        SnapLogDeps   d = deps;
        SnapLogConfig c = cfg;
        SnapLogClient::instance().init(std::move(d), std::move(c));
        SnapLogClient::instance().set_user_token("tok"); // logged-in default (login gate)
    }
    void shutdown() { SnapLogClient::instance().shutdown(); }

    // Wait until `pred` returns true, polling every 2ms, up to timeout_ms.
    bool wait_until(std::function<bool()> pred, int timeout_ms = 5000)
    {
        auto t0 = std::chrono::steady_clock::now();
        while (true) {
            if (pred())
                return true;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > timeout_ms)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
};

} // anonymous namespace

TEST_CASE("batch lifecycle: init recovery seals non-empty active + uploads sealed", "[snaplog][batch]")
{
    BatchLifecycleFixture f;
    namespace fs = boost::filesystem;

    // Pre-populate active.log with content (simulates a crash-recovery scenario).
    {
        fs::ofstream ofs((f.spool / "active.log").string(), std::ios::binary);
        ofs << "{\"level\":\"INFO\",\"message\":\"crashed-event\"}\n";
    }
    // Also pre-populate a sealed file from a prior session.
    auto prior_sealed = f.spool / "batch.1699999999000.prior0000.sealed";
    {
        fs::ofstream ofs(prior_sealed.string(), std::ios::binary);
        ofs << "{\"level\":\"WARN\",\"message\":\"prior-sealed\"}\n";
    }

    f.init();

    // init() should: create_directories (already exists), rotate active.log
    // (non-empty) into a sealed, then bt_worker uploads BOTH sealed files.
    // Wait for both sealed files to be uploaded + deleted.
    REQUIRE(f.wait_until([&]() { return f.completed_count.load() >= 2; }, 5000));

    // Both sealed files should be deleted after successful completed.
    REQUIRE(f.wait_until([&]() { return list_sealed(f.spool).empty(); }, 3000));

    // active.log should NOT exist after rotate (it was renamed away).
    REQUIRE(!fs::exists(f.spool / "active.log"));

    f.shutdown();
}

TEST_CASE("batch lifecycle: init consent OFF purges recovery without worker", "[snaplog][batch]")
{
    BatchLifecycleFixture f;
    namespace fs = boost::filesystem;

    {
        fs::ofstream ofs((f.spool / "active.log").string(), std::ios::binary);
        ofs << "{\"level\":\"INFO\",\"message\":\"do-not-recover\"}\n";
    }
    auto prior_sealed = f.spool / "batch.1699999999000.prior0000.sealed";
    {
        fs::ofstream ofs(prior_sealed.string(), std::ios::binary);
        ofs << "{\"level\":\"WARN\",\"message\":\"prior-sealed\"}\n";
    }
    const auto empty_sealed = f.spool / "batch.1699999999001.empty0000.sealed";
    {
        fs::ofstream ofs(empty_sealed.string(), std::ios::binary);
    }

    f.deps.consent_ok = []() { return false; };
    f.init();

    REQUIRE(f.wait_until(
        [&]() {
            boost::system::error_code ec;
            return list_sealed(f.spool).empty() && !fs::exists(f.spool / "active.log", ec);
        },
        3000));
    REQUIRE(fs::exists(f.spool / ".lock"));
    REQUIRE_FALSE(SnapLogClient::instance().batch_worker_joinable_for_test());
    REQUIRE(f.create_count.load() == 0);

    f.shutdown();
}

TEST_CASE("batch lifecycle: shutdown drain flushes bt_queue to sealed and uploads", "[snaplog][batch]")
{
    BatchLifecycleFixture f;

    f.init();

    // Enqueue some batch events.
    for (int i = 0; i < 3; ++i) {
        SnapLogClient::instance().log(SnapLogLevel::Info, "drain-event-" + std::to_string(i),
                                      SnapLogExt{{"eventName", "drain_test"}, {"opId", "op" + std::to_string(i)}}, SnapLogPolicy::Buffered,
                                      __FUNCTION__, __LINE__);
    }
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 3);

    // shutdown() should set drain_and_flush, drain the queue to active.log,
    // rotate to sealed, upload via create/PUT/completed, then join bt_worker.
    f.shutdown();

    // After shutdown: bt_queue empty (drained), all sealed uploaded + deleted.
    REQUIRE(list_sealed(f.spool).empty());
    // At least one create+completed happened (the drained events).
    REQUIRE(f.create_count.load() >= 1);
    REQUIRE(f.completed_count.load() >= 1);
}

TEST_CASE("batch lifecycle: consent OFF purges spool + no create after", "[snaplog][batch]")
{
    BatchLifecycleFixture f;
    namespace fs = boost::filesystem;

    f.init();

    // Enqueue a batch event — the worker drains it to active.log. The event
    // may or may not rotate to sealed (depends on thresholds + timing), but
    // either way there will be at least an active.log present.
    SnapLogClient::instance().log(SnapLogLevel::Info, "pre-consent-off", SnapLogExt{{"eventName", "consent_test"}, {"opId", "op1"}},
                                  SnapLogPolicy::Buffered, __FUNCTION__, __LINE__);
    // Wait for the worker to have drained (queue should be empty).
    REQUIRE(f.wait_until([&]() { return SnapLogClient::instance().batch_queue_size_for_test() == 0; }, 3000));

    // Consent OFF → should cancel bt_worker, purge spool, no further creates.
    SnapLogClient::instance().set_consent(false);

    // Spool should be purged: no sealed files, no active.log.
    REQUIRE(f.wait_until(
        [&]() {
            boost::system::error_code ec;
            bool                      no_active = !fs::exists(f.spool / "active.log", ec);
            return list_sealed(f.spool).empty() && no_active;
        },
        3000));
    REQUIRE(fs::exists(f.spool / ".lock"));

    // Give a brief moment to ensure no new create calls fire after consent OFF.
    int creates_after_consent_off = f.create_count.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // No new creates should have fired after consent went false (bt_worker exits
    // on consent false, so no uploads happen).
    REQUIRE(f.create_count.load() == creates_after_consent_off);

    // Enqueuing a new batch event while consent is OFF → dropped (no-op).
    SnapLogClient::instance().log(SnapLogLevel::Info, "after-off", SnapLogExt{{"eventName", "consent_test"}, {"opId", "op2"}},
                                  SnapLogPolicy::Buffered, __FUNCTION__, __LINE__);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 0);

    f.shutdown();
}

TEST_CASE("batch lifecycle: consent ON after OFF clears bt_queue + restarts worker", "[snaplog][batch]")
{
    BatchLifecycleFixture f;

    f.init();

    // Consent OFF first.
    SnapLogClient::instance().set_consent(false);
    REQUIRE(f.wait_until([&]() { return list_sealed(f.spool).empty(); }, 3000));

    // Turn consent back ON → should clear bt_queue + restart bt_worker.
    SnapLogClient::instance().set_consent(true);

    // Now enqueue a batch event — it should be accepted and drained.
    SnapLogClient::instance().log(SnapLogLevel::Info, "after-on", SnapLogExt{{"eventName", "consent_on_test"}, {"opId", "op3"}},
                                  SnapLogPolicy::Buffered, __FUNCTION__, __LINE__);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() <= 1);

    // Worker should drain the event to active.log (file exists).
    REQUIRE(f.wait_until([&]() { return boost::filesystem::exists(f.spool / "active.log"); }, 3000));

    f.shutdown();
}

TEST_CASE("batch lifecycle: periodic flush rotates+uploads after batch_flush_sec", "[snaplog][batch]")
{
    BatchLifecycleFixture f;
    f.cfg.batch_flush_sec  = 1;                // 1s periodic flush
    f.cfg.active_max_bytes = 10 * 1024 * 1024; // huge: only the timer can rotate
    f.cfg.batch_max_events = 10000;
    f.init();

    // 1 event — below size/count thresholds, so only the flush timer rotates it.
    SnapLogClient::instance().log(SnapLogLevel::Info, "flush-event", SnapLogExt{{"eventName", "flush_test"}, {"opId", "opf"}},
                                  SnapLogPolicy::Buffered, __FUNCTION__, __LINE__);
    // Periodic flush (1s) -> rotate -> create+PUT+completed -> sealed deleted.
    REQUIRE(f.wait_until([&]() { return list_sealed(f.spool).empty() && f.create_count.load() >= 1; }, 5000));
    f.shutdown();
}

// ---------------------------------------------------------------------------
// Task B6 fix: consent-OFF must return without waiting for a stuck worker. The
// worker is moved out immediately; OFF->ON defers the replacement until that
// worker has exited and the privacy purge has completed.
//
// We force a REAL join timeout by making the FIRST create call BLOCK inside
// do_request until the test releases a gate. Because bt_worker_loop only checks
// consent at the top of each tick, and attempt_upload calls do_request
// synchronously, a do_request that blocks parks the worker mid-tick — it never
// returns to the top to observe consent-false, so the bounded join in
// set_consent(false) times out. Pre-fix, the timeout branch only set
// deps_invalid and left bt_worker joinable -> OFF->ON never restarts (guard is
// !bt_worker.joinable()). Post-fix, the worker is moved out of bt_worker so the
// object is non-joinable immediately and OFF->ON restarts.
// ---------------------------------------------------------------------------
TEST_CASE("batch lifecycle: consent OFF join timeout leaves bt_worker restartable", "[snaplog][batch]")
{
    namespace fs = boost::filesystem;

    fs::path       spool = fs::temp_directory_path() / fs::unique_path("snaplog-bt-timeout-%%%%.dir");
    TempSpoolGuard guard(spool);

    // Gate that blocks the first create until the test releases it.
    std::mutex              gate_mu;
    std::condition_variable gate_cv;
    bool                    release_gate = false;
    std::atomic<bool>       first_create_entered{false};
    std::atomic<bool>       first_create_returned{false};
    std::atomic<int>        create_count{0};
    std::atomic<int>        put_count{0};
    std::atomic<int>        completed_count{0};

    int64_t now = 1700000000000;

    SnapLogDeps deps;
    deps.now_ms     = [&now]() { return now++; };
    deps.consent_ok = []() { return true; };
    deps.user_token = []() { return std::string("tok"); };
    deps.user_id    = []() { return std::string("uid-1"); };
    deps.device_id  = []() { return std::string("did-1"); };
    deps.machine_id = []() { return std::string("mid-xyz"); };
    deps.do_request = [&](const std::string& method, const std::string& url, std::vector<std::pair<std::string, std::string>> /*headers*/,
                          const boost::filesystem::path* /*body_file*/, const std::string* /*body_str*/) -> std::shared_ptr<SnapLogHandle> {
        bool is_create    = (url.find("/create") != std::string::npos && url.find("/completed") == std::string::npos);
        bool is_put       = (method == "PUT");
        bool is_completed = (url.find("/completed") != std::string::npos);
        if (is_put)
            ++put_count;
        if (is_completed)
            ++completed_count;
        if (is_create) {
            int n = ++create_count;
            if (n == 1) {
                // Block the worker inside this tick so it can't observe
                // consent-false -> forces the bounded join to time out.
                first_create_entered.store(true);
                std::unique_lock<std::mutex> lk(gate_mu);
                gate_cv.wait(lk, [&]() { return release_gate; });
                // After release, return a null handle so the worker unwinds
                // cleanly. The generation fence must prevent all subsequent
                // state changes and requests from this retired worker.
                first_create_returned.store(true);
                return nullptr;
            }
        }
        // Non-blocking fulfilled handle for any other call.
        auto h  = std::make_shared<SnapLogHandle>();
        h->prom = std::make_shared<std::promise<SnapLogResult>>();
        h->fulfilled.store(true);
        SnapLogResult res{200, std::string{}, false};
        if (is_create) {
            nlohmann::json j;
            j["code"] = 200;
            j["data"] = nlohmann::json{{"method", "PUT"}, {"url", "https://s3.example/presigned"}, {"key", "logs/fullkey.ndjson"}};
            res.body  = j.dump();
        } else if (url.find("/completed") != std::string::npos) {
            res.body = nlohmann::json{{"code", 200}, {"msg", "ok"}}.dump();
        }
        h->prom->set_value(res);
        return h;
    };

    SnapLogConfig cfg;
    cfg.poll_interval_ms        = 1;
    cfg.drain_batch_per_tick    = 256;
    cfg.drain_wall_budget_ms    = 50;
    cfg.active_max_bytes        = 1; // tiny: any event rotates to sealed
    cfg.batch_max_events        = 1; // rotate after 1 event
    cfg.batch_max_bytes         = 1 * 1024 * 1024;
    cfg.sealed_max_disk_mb      = 256;
    cfg.batch_max_retry         = 3;
    cfg.line_max_bytes          = 64 * 1024;
        cfg.home_for_redact         = "/home/test";
    cfg.spool_dir               = spool.string();
    cfg.batch_join_deadline_sec = 1; // short so the timeout branch fires fast

    {
        SnapLogDeps   d = deps;
        SnapLogConfig c = cfg;
        SnapLogClient::instance().init(std::move(d), std::move(c));
    }
    SnapLogClient::instance().set_user_token("tok"); // login gate

    // Enqueue an event; the worker drains it, rotates to sealed, then calls
    // create (which blocks on the gate).
    SnapLogClient::instance().log(SnapLogLevel::Info, "stuck-upload", SnapLogExt{{"eventName", "timeout_test"}, {"opId", "op1"}},
                                  SnapLogPolicy::Buffered, __FUNCTION__, __LINE__);

    // Wait until the worker is parked inside the blocking create.
    auto wait_flag = [](std::atomic<bool>& fl, int timeout_ms) {
        auto t0 = std::chrono::steady_clock::now();
        while (!fl.load()) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > timeout_ms)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    };
    REQUIRE(wait_flag(first_create_entered, 3000));

    // Consent OFF must not wait for the blocked create. The fix makes
    // bt_worker non-joinable immediately (moved out to the joiner).
    const auto consent_off_start = std::chrono::steady_clock::now();
    SnapLogClient::instance().set_consent(false);
    REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - consent_off_start).count() < 500);

    // INVARIANT: after a timed-out consent-OFF, bt_worker is non-joinable.
    REQUIRE_FALSE(SnapLogClient::instance().batch_worker_joinable_for_test());

    // Re-init while the retired worker still owns the spool. The replacement
    // must also wait for the old privacy purge; it cannot steal the lock.
    {
        SnapLogDeps   reinit_deps = deps;
        SnapLogConfig reinit_cfg  = cfg;
        SnapLogClient::instance().init(std::move(reinit_deps), std::move(reinit_cfg));
    }
    SnapLogClient::instance().set_user_token("tok");
    REQUIRE_FALSE(SnapLogClient::instance().batch_worker_joinable_for_test());

    // OFF -> ON records consent as true, but the replacement waits for the
    // retired worker so two batch owners never touch the spool concurrently.
    SnapLogClient::instance().set_consent(true);
    REQUIRE_FALSE(SnapLogClient::instance().batch_worker_joinable_for_test());

    // Events accepted after ON remain queued across the deferred purge.
    SnapLogClient::instance().log(SnapLogLevel::Info, "replacement-upload", SnapLogExt{{"eventName", "timeout_recovery"}, {"opId", "op2"}},
                                  SnapLogPolicy::Buffered, __FUNCTION__, __LINE__);

    REQUIRE(create_count.load() == 1);
    REQUIRE(put_count.load() == 0);
    REQUIRE(completed_count.load() == 0);

    // Release the stuck original worker. Its joiner purges the old spool and
    // starts the replacement, which must complete a full upload cycle.
    {
        std::lock_guard<std::mutex> lk(gate_mu);
        release_gate = true;
    }
    gate_cv.notify_all();

    auto wait_count = [](std::atomic<int>& value, int expected, int timeout_ms) {
        auto t0 = std::chrono::steady_clock::now();
        while (value.load() < expected) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > timeout_ms)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    };
    REQUIRE(wait_count(create_count, 2, 3000));
    REQUIRE(wait_count(put_count, 1, 3000));
    REQUIRE(wait_count(completed_count, 1, 3000));
    REQUIRE(wait_flag(first_create_returned, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(create_count.load() == 2);
    REQUIRE(put_count.load() == 1);
    REQUIRE(completed_count.load() == 1);

    SnapLogClient::instance().shutdown();
}

// ---------------------------------------------------------------------------
// Fix A: make_upload_file_name produces unique fileNames per call.
// Two calls with the same batchId must differ (different now_ms / seq).
// ---------------------------------------------------------------------------
TEST_CASE("make_upload_file_name: unique per call with same batchId", "[snaplog][batch]")
{
    std::string fn1 = make_upload_file_name("client-A", "batch-001", 1700000000000, 0);
    std::string fn2 = make_upload_file_name("client-A", "batch-001", 1700000000001, 1);
    REQUIRE(fn1 != fn2);
    REQUIRE(fn1.find("client-A/batch-001.") == 0);
    REQUIRE(fn1.find(".ndjson") != std::string::npos);
    // Both contain the batchId segment.
    REQUIRE(fn1.find("batch-001") != std::string::npos);
    REQUIRE(fn2.find("batch-001") != std::string::npos);
    // The now_ms and seq suffixes differ.
    REQUIRE(fn1.find("1700000000000") != std::string::npos);
    REQUIRE(fn2.find("1700000000001") != std::string::npos);
}

TEST_CASE("make_upload_file_name: different seq with same now_ms still unique", "[snaplog][batch]")
{
    int64_t     same_ts = 1700000000000;
    std::string fn1     = make_upload_file_name("c1", "b1", same_ts, 0);
    std::string fn2     = make_upload_file_name("c1", "b1", same_ts, 1);
    REQUIRE(fn1 != fn2);
}

// ---------------------------------------------------------------------------
// Fix C: create response body code==110004 triggers auth_known_dead
// (even on non-4xx HTTP status). Previously only HTTP 401/403 triggered it.
// ---------------------------------------------------------------------------
TEST_CASE("bt upload: create body code 110004 sets auth_known_dead", "[snaplog][batch]")
{
    BatchUploadFixture f;
    auto               sealed = f.make_sealed("ccc333");
    // create returns 200 HTTP but body code==110004 (token expired at app level).
    f.script.respond = [](const std::string& kind) -> SnapLogResult {
        if (kind == "create")
            return {200, err_body(110004), false};
        return {200, std::string{}, false};
    };

    std::thread t([&]() { SnapLogClient::bt_worker_loop(f.in); });

    // create fires, body code==110004 -> auth_known_dead set.
    REQUIRE(f.wait_until([&]() { return f.in->auth_known_dead.load(); }));
    REQUIRE(f.wait_until([&]() { return f.create_count.load() >= 1; }));

    // Force exit.
    f.in->stop_receiving.store(true);
    if (t.joinable())
        t.join();

    REQUIRE(f.in->auth_known_dead.load());
    // create was NOT retried (110004 is terminal like 401).
    REQUIRE(f.create_count.load() == 1);
    // No PUT/completed happened.
    REQUIRE(f.put_count.load() == 0);
    REQUIRE(f.completed_count.load() == 0);
    // Sealed retained.
    REQUIRE(boost::filesystem::exists(sealed));
}
TEST_CASE("SpoolLock: exclusive — second acquire fails, release enables retry", "[snaplog][batch]")
{
    namespace fs = boost::filesystem;
    auto dir     = fs::unique_path(fs::temp_directory_path() / "snaplog-lock-%%%%.dir");
    using namespace Slic3r::SnapLog::v1;

    auto lk1 = SnapLogClient::try_acquire_spool_lock(dir);
    REQUIRE(lk1.acquired);

    auto lk2 = SnapLogClient::try_acquire_spool_lock(dir);
    REQUIRE_FALSE(lk2.acquired); // same dir already locked

    lk1 = SnapLogClient::SpoolLock{}; // move-assign default → old fd closed → lock released

    {
        auto lk3 = SnapLogClient::try_acquire_spool_lock(dir);
        REQUIRE(lk3.acquired); // re-acquired after release
    } // lk3 released

    fs::remove_all(dir);
}
