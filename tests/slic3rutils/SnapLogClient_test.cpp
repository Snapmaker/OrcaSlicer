#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <set>
#include <sstream>
#include <string_view>
#include <memory>
#include <mutex>
#include <vector>
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

TEST_CASE("SnapEvent registry maps every enum to a unique stable name", "[snaplog][events]")
{
    REQUIRE(snap_event_name(SnapEvent::SLICE_FAILED) == "slice_failed");
    REQUIRE(snap_event_name(SnapEvent::USER_LOGIN_RESULT) == "user_login_result");
    REQUIRE_FALSE(snap_event_from_name("not_a_snap_event").has_value());

    const std::array<SnapEvent, 6> events = {SnapEvent::DEVICE_DISCOVERY_START,
                                              SnapEvent::FILE_IMPORT_RESULT,
                                              SnapEvent::GCODE_EXPORT_RESULT,
                                              SnapEvent::PROJECT_SAVE_RESULT,
                                              SnapEvent::SLICE_FAILED,
                                              SnapEvent::USER_LOGIN_RESULT};

    std::set<std::string_view> names;
    for (const auto event : events)
        REQUIRE(names.insert(snap_event_name(event)).second);
}

TEST_CASE("SnapLog event routes name the upload channel snaplog", "[snaplog][routing][naming]")
{
    const auto upload = snap_event_route(SnapEvent::FILE_IMPORT_RESULT, SnapLogLevel::Warning);
    REQUIRE(upload.snaplog);
}

TEST_CASE("SnapEvent registry declares complete routes", "[snaplog][routing]")
{
    REQUIRE_FALSE(snap_event_route(SnapEvent::FILE_IMPORT_RESULT, SnapLogLevel::Info).snaplog);
    REQUIRE(snap_event_route(SnapEvent::FILE_IMPORT_RESULT, SnapLogLevel::Warning).snaplog);
    REQUIRE(snap_event_route(SnapEvent::FILE_IMPORT_RESULT, SnapLogLevel::Error).snaplog);
    REQUIRE(snap_event_route(SnapEvent::DEVICE_DISCOVERY_START, SnapLogLevel::Info).snaplog);
    REQUIRE(snap_event_route(SnapEvent::USER_LOGIN_RESULT, SnapLogLevel::Info).snaplog);

    const std::array<SnapEvent, 6> events = {SnapEvent::DEVICE_DISCOVERY_START,
                                              SnapEvent::FILE_IMPORT_RESULT,
                                              SnapEvent::GCODE_EXPORT_RESULT,
                                              SnapEvent::PROJECT_SAVE_RESULT,
                                              SnapEvent::SLICE_FAILED,
                                              SnapEvent::USER_LOGIN_RESULT};
    for (const auto event : events) {
        for (const auto level : {SnapLogLevel::Info, SnapLogLevel::Warning, SnapLogLevel::Error}) {
            const auto route = snap_event_route(event, level);
            CAPTURE(event, level);
            const bool info_only = event == SnapEvent::DEVICE_DISCOVERY_START || event == SnapEvent::USER_LOGIN_RESULT;
            REQUIRE(route.snaplog == (info_only || level != SnapLogLevel::Info));
        }
    }
}


TEST_CASE("SnapLog production endpoints are centralized as constants", "[snaplog][config][endpoints]")
{
    REQUIRE(kSnapLogUploadCnUrl == "https://api.snapmaker.cn");
    REQUIRE(kSnapLogUploadIntlUrl == "https://api.snapmaker.com");

    SnapLogConfig config;
    REQUIRE(config.snaplog_base == kSnapLogUploadIntlUrl);
}

namespace {
struct FakeClient
{
    int64_t           now            = 0;
    std::atomic<int>  request_count{0};
    SnapLogDeps       deps;
    SnapLogConfig     cfg;

    FakeClient()
    {
        deps.do_request = [this](const std::string& /*method*/, const std::string& /*url*/,
                                 std::vector<std::pair<std::string, std::string>> /*headers*/,
                                 const boost::filesystem::path* /*body_file*/, const std::string* /*body_str*/) {
            ++request_count;
            auto h  = std::make_shared<SnapLogHandle>();
            h->prom = std::make_shared<std::promise<SnapLogResult>>();
            h->prom->set_value({200, "", false});
            h->fulfilled.store(true);
            return h;
        };
        deps.consent_ok = []() { return true; };
        deps.machine_id = []() { return std::string("mid"); };
        deps.now_ms     = [this]() { return now++; };
        // Small queue cap for fill/evict tests.
        cfg.default_rate_cap_per_sec       = 1000; // high default so rate limit doesn't interfere
        cfg.total_unknown_rate_cap_per_sec = 10000;
    }

    void init()
    {
        // Copy deps/cfg into the singleton.
        SnapLogDeps   d = deps; // copy (lambdas capture by value via this*)
        SnapLogConfig c = cfg;
        SnapLogClient::instance().init(std::move(d), std::move(c));
        // Default to a mirrored account id for tests that assert payload identity.
        SnapLogClient::instance().set_user_id("user-1");
    }
    void shutdown() { SnapLogClient::instance().shutdown(); }
};
} // anonymous namespace

TEST_CASE("site reconfiguration swaps the endpoint without touching the worker", "[snaplog][config][gui]")
{
    FakeClient fc;
    fc.init();
    SnapLogClient::instance().log(SnapLogLevel::Warning, "queued", {{"eventName", "site_swap"}}, SinkSnapLog);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 1);
    const bool worker_before = SnapLogClient::instance().batch_worker_joinable_for_test();

    SnapLogClient::instance().reconfigure_site("CN");
    REQUIRE(SnapLogClient::instance().snaplog_base_for_test() == "https://api.snapmaker.cn");
    SnapLogClient::instance().reconfigure_site("US");
    REQUIRE(SnapLogClient::instance().snaplog_base_for_test() == "https://api.snapmaker.com");
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 1);
    REQUIRE(SnapLogClient::instance().batch_worker_joinable_for_test() == worker_before);
    fc.shutdown();
}

TEST_CASE("level_str wire strings", "[snaplog]")
{
    REQUIRE(std::string_view(level_str(SnapLogLevel::Info)) == "INFO");
    REQUIRE(std::string_view(level_str(SnapLogLevel::Warning)) == "WARN");
    REQUIRE(std::string_view(level_str(SnapLogLevel::Error)) == "ERROR");
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
    REQUIRE(c.snaplog_base == "https://api.snapmaker.com");
    REQUIRE(c.enabled == true);
    REQUIRE(c.default_rate_cap_per_sec == 5);
    REQUIRE(c.total_unknown_rate_cap_per_sec == 20);
    REQUIRE(c.http_connect_timeout_sec == 5);
    REQUIRE(c.http_total_timeout_sec == 30);
    REQUIRE(c.poll_interval_ms == 50);
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
    SnapLogClient::instance().log(SnapLogLevel::Info, "hello", {}, SinkSnapLog);
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
    SnapLogClient::instance().set_user_id("u");
    SnapLogClient::instance().log(SnapLogLevel::Warning, "w", {}, SinkSnapLog);
    SnapLogClient::instance().shutdown();
    // Post-shutdown log() is also a no-op.
    SnapLogClient::instance().log(SnapLogLevel::Error, "post", {}, SinkSnapLog);
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
        cfg.default_rate_cap_per_sec       = 1000;
        cfg.total_unknown_rate_cap_per_sec = 1000;
        SnapLogClient::instance().init(std::move(deps), std::move(cfg));
        SnapLogClient::instance().set_user_id("user");

        std::atomic<bool> release{false};
        auto              log_worker = [&release]() {
            while (!release.load())
                std::this_thread::yield();
            SnapLogClient::instance().log(SnapLogLevel::Info, "race", {{"eventName", "race"}}, SinkSnapLog);
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
        SnapLogClient::instance().log(SnapLogLevel::Error, "after", {{"eventName", "race"}}, SinkSnapLog);
    }
    REQUIRE(true);
}

TEST_CASE("SnapLogClient: user identity changes do not clear public-auth failure", "[snaplog][batch]")
{
    // A public HMAC 401/403 is a transport/configuration failure, not an
    // account-token expiry; changing the mirrored account id must not resurrect
    // a known-dead public credential.
    SnapLogDeps deps;
    deps.consent_ok = []() { return true; };
    deps.now_ms     = []() { return int64_t(0); };
    SnapLogConfig cfg;
    SnapLogClient::instance().init(std::move(deps), std::move(cfg));

    // Simulate the flusher having hit a 401 on create.
    SnapLogClient::instance().set_auth_known_dead_for_test(true);
    REQUIRE(SnapLogClient::instance().auth_known_dead_for_test());

    SnapLogClient::instance().set_user_id("");
    REQUIRE(SnapLogClient::instance().auth_known_dead_for_test());

    SnapLogClient::instance().set_user_id("new-user");
    REQUIRE(SnapLogClient::instance().auth_known_dead_for_test());

    SnapLogClient::instance().shutdown();
}


// ---------------------------------------------------------------------------
// Task 8: log() admission pipeline (consent / disable / rate / queue)
// ---------------------------------------------------------------------------
// Helper: build a SnapLogClient with fake deps. now_ms starts at 0, increments
// on each call so rate-limiter time advances naturally across calls.


TEST_CASE("SnapLog admission does not require a user id", "[snaplog][pipeline][auth]")
{
    FakeClient fc;
    fc.init();
    SnapLogClient::instance().set_user_id("");
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 0);

    SnapLogClient::instance().log(SnapLogLevel::Warning, "anonymous accepted", {{"eventName", "anonymous_event"}}, SinkSnapLog);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 1);

    SnapLogClient::instance().set_user_id("user-1");
    SnapLogClient::instance().log(SnapLogLevel::Warning, "signed-in accepted", {{"eventName", "signed_in_event"}}, SinkSnapLog);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 2);
    fc.shutdown();
}

TEST_CASE("business macros hide sinks and accept four typed forms", "[snaplog][api]")
{
    FakeClient fc;
    fc.cfg.default_rate_cap_per_sec                       = 1000;
    fc.cfg.total_unknown_rate_cap_per_sec                 = 10000;
    fc.init();

    SNAP_LOG_INFO(SnapEvent::USER_LOGIN_RESULT);
    SNAP_LOG_WARNING(SnapEvent::FILE_IMPORT_RESULT, {{"success", "false"}});
    SNAP_LOG_ERROR(SnapEvent::SLICE_FAILED, {{"success", "false"}}, "slice failed");
    SNAP_LOG_INFO(SnapEvent::SLICE_FAILED, {{"durationMs", "12"}}, "slice completed");

    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 3);
    fc.shutdown();
}

TEST_CASE("renderers inject and protect version fields", "[snaplog][fields]")
{
    BatchLineContext ctx;
    ctx.eventName      = "app_launch";
    ctx.profileVersion = "02.03.03.03";
    ctx.controlVersion = "1.10.3";

    auto snaplog        = render_batch_line(SnapLogLevel::Info, "", {}, 64 * 1024, ctx);
    auto parsed_snaplog = nlohmann::json::parse(snaplog.substr(0, snaplog.size() - 1));
    REQUIRE(parsed_snaplog["ext"]["profileVersion"] == "02.03.03.03");
    REQUIRE(parsed_snaplog["ext"]["control_version"] == "1.10.3");

    ctx.profileVersion.clear();
    ctx.controlVersion.clear();
    auto empty_snaplog = render_batch_line(SnapLogLevel::Info, "", {}, 64 * 1024, ctx);
    auto parsed_empty  = nlohmann::json::parse(empty_snaplog.substr(0, empty_snaplog.size() - 1));
    REQUIRE(parsed_empty["ext"]["profileVersion"] == "");
    REQUIRE(parsed_empty["ext"]["control_version"] == "");

    ctx.clientId           = "machine-1";
    ctx.profileVersion     = "02.03.03.03";
    auto protected_snaplog = render_batch_line(SnapLogLevel::Info, "",
        {{"clientUUID", "attacker-value"}, {"profileVersion", "attacker-version"}}, 64 * 1024, ctx);
    auto parsed_protected  = nlohmann::json::parse(protected_snaplog.substr(0, protected_snaplog.size() - 1));
    REQUIRE(parsed_protected["ext"]["clientUUID"] == "machine-1");
    REQUIRE(parsed_protected["ext"]["profileVersion"] == "02.03.03.03");
}

TEST_CASE("render_batch_line passes caller opId through", "[snaplog][fields]")
{
    BatchLineContext ctx;
    ctx.eventName = "slice_completed";
    auto line   = render_batch_line(SnapLogLevel::Info, "", {{"opId", "op-1"}}, 64 * 1024, ctx);
    auto parsed = nlohmann::json::parse(line.substr(0, line.size() - 1));
    REQUIRE(parsed["ext"]["opId"] == "op-1");
}

TEST_CASE("consent OFF does not flush backlog on ON", "[snaplog][lifecycle]")
{
    // Events enqueued during OFF period must not be retroactively flushed.
    FakeClient fc;
    fc.init();

    SnapLogClient::instance().set_consent(false);
    // set_consent(false) clears queue; log() gates on consent so nothing enqueues.
    // Now flip ON and verify no stale sends.
    int before = fc.request_count.load();
    SnapLogClient::instance().set_consent(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(fc.request_count.load() == before); // no backlog flush

    fc.shutdown();
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
// Fix #4: renderer ext-value truncation caps
// Per-string ext value capped to <=2000 bytes; total ext <=8000 bytes;
// eventName (and opId) always survive even when ext must be trimmed.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Plan B Task 1: render_batch_line — NDJSON line renderer (full field set)
// Pure function: produces one NDJSON line (JSON object + trailing '\n').
// Spec §4.1.2/§4.1.3. Privacy enforcement point (mirrors realtime renderer).
// ---------------------------------------------------------------------------

TEST_CASE("render_batch_line has core mandatory fields", "[snaplog][batch]")
{
    BatchLineContext ctx;
    ctx.clientId  = "c1";
    ctx.eventName = "slice_completed";
    ctx.batchId   = "b1";
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

TEST_CASE("render_batch_line keeps anonymous userId empty", "[snaplog][batch]")
{
    BatchLineContext ctx;
    ctx.eventName = "anonymous_event";
    auto line     = render_batch_line(SnapLogLevel::Info, "anonymous", {}, 64 * 1024, ctx);
    REQUIRE(line.find("\"userId\":\"\"") != std::string::npos);
}

TEST_CASE("render_batch_line trims non-required context fields", "[snaplog][batch]")
{
    BatchLineContext ctx;
    ctx.userId     = "u1";
    ctx.appVersion = "2.5.0";
    ctx.osVersion  = "10.0.19045";
    ctx.eventName  = "slice_completed";
    auto line      = render_batch_line(SnapLogLevel::Info, "m", {}, 64 * 1024, ctx);
    // Keep the wire-required and functionally required fields.
    REQUIRE(line.find("\"eventName\":\"slice_completed\"") != std::string::npos);
    REQUIRE(line.find("\"userId\":\"u1\"") != std::string::npos);
    // Drop optional diagnostic fields that the snaplog does not require.
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

TEST_CASE("render_batch_line keeps opId through the ext cap", "[snaplog][batch][priv]")
{
    BatchLineContext ctx;
    ctx.eventName = "typed_event";
    SnapLogExt ext{{"opId", "op-1"}};
    for (int i = 0; i < 8; ++i)
        ext.emplace_back("payload" + std::to_string(i), std::string(2000, 'x'));

    auto line   = render_batch_line(SnapLogLevel::Info, "", ext, 64 * 1024, ctx);
    auto parsed = nlohmann::json::parse(line.substr(0, line.size() - 1));
    REQUIRE(parsed["ext"]["opId"] == "op-1");
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
// three file-upload lifecycle endpoints. Every request uses the public HMAC
// contract and receives a fresh timestamp/nonce signature.
// ---------------------------------------------------------------------------

TEST_CASE("build_batch_create_request: public HMAC -> /api/log/public/upload/create", "[snaplog][batch]")
{
    SnapLogConfig cfg;
    cfg.snaplog_base       = "https://api.snapmaker.com";
    cfg.public_hmac_secret = "secret";
    cfg.now_ms             = []() { return int64_t(1778639794110); };
    cfg.next_nonce         = []() { return std::string("nonce1"); };

    BatchRequest req = build_batch_create_request(cfg, /*clientId*/ "app-123", "batch.123.log");

    REQUIRE(req.url == "https://api.snapmaker.com/api/log/public/upload/create");
    REQUIRE(hdr(req.headers, "Authorization").empty());
    REQUIRE(hdr(req.headers, "X-Client-Type") == "Orca");
    REQUIRE(hdr(req.headers, "X-Client-Id") == "app-123");
    REQUIRE(hdr(req.headers, "X-Timestamp") == "1778639794110");
    REQUIRE(hdr(req.headers, "X-Nonce") == "nonce1");
    REQUIRE(hdr(req.headers, "X-Sign") == "f57ff1173a4736366ebee61da3531470592c7f8aea2aa729b01ebfacc9b0ad4b");
    // Body has fileName, no checkSum.
    REQUIRE(req.body.find("\"fileName\"") != std::string::npos);
    REQUIRE(req.body.find("batch.123.log") != std::string::npos);
    REQUIRE(req.body.find("checkSum") == std::string::npos);
}

TEST_CASE("build_batch_request: each endpoint receives a fresh nonce", "[snaplog][batch]")
{
    SnapLogConfig cfg;
    cfg.snaplog_base       = "https://api.snapmaker.com";
    cfg.public_hmac_secret = "secret";
    cfg.now_ms             = []() { return int64_t(1778639794110); };
    int nonce_count = 0;
    cfg.next_nonce = [&nonce_count]() { return "nonce" + std::to_string(++nonce_count); };

    const auto create   = build_batch_create_request(cfg, "app-123", "mylog.ndjson");
    const auto complete = build_batch_complete_request(cfg, "app-123", "key1");
    const auto cancel   = build_batch_cancel_request(cfg, "app-123", "upload1");

    REQUIRE(hdr(create.headers, "X-Nonce") == "nonce1");
    REQUIRE(hdr(complete.headers, "X-Nonce") == "nonce2");
    REQUIRE(hdr(cancel.headers, "X-Nonce") == "nonce3");
    REQUIRE(hdr(create.headers, "X-Sign") != hdr(complete.headers, "X-Sign"));
    REQUIRE(hdr(create.headers, "X-Sign") != hdr(cancel.headers, "X-Sign"));
    REQUIRE(hdr(complete.headers, "X-Sign") != hdr(cancel.headers, "X-Sign"));
}

TEST_CASE("build_batch_request: empty nonce builds no request", "[snaplog][batch]")
{
    SnapLogConfig cfg;
    cfg.snaplog_base       = "https://api.snapmaker.com";
    cfg.public_hmac_secret = "secret";
    cfg.now_ms             = []() { return int64_t(1778639794110); };
    cfg.next_nonce         = []() { return std::string{}; };

    const auto create   = build_batch_create_request(cfg, "app-123", "mylog.ndjson");
    const auto complete = build_batch_complete_request(cfg, "app-123", "key1");
    const auto cancel   = build_batch_cancel_request(cfg, "app-123", "upload1");

    REQUIRE(create.url.empty());
    REQUIRE(create.headers.empty());
    REQUIRE(create.body.empty());
    REQUIRE(complete.url.empty());
    REQUIRE(complete.headers.empty());
    REQUIRE(complete.body.empty());
    REQUIRE(cancel.url.empty());
    REQUIRE(cancel.headers.empty());
    REQUIRE(cancel.body.empty());
}

TEST_CASE("build_batch_request: empty signing secret builds no request", "[snaplog][batch]")
{
    SnapLogConfig cfg;
    cfg.snaplog_base = "https://api.snapmaker.com";
    cfg.now_ms       = []() { return int64_t(1778639794110); };
    cfg.next_nonce   = []() { return std::string("nonce1"); };

    BatchRequest req = build_batch_create_request(cfg, /*clientId*/ "app-123", "mylog.ndjson");

    REQUIRE(req.url.empty());
    REQUIRE(req.headers.empty());
    REQUIRE(req.body.empty());
}

TEST_CASE("build_batch_complete_request: single-part body carries fileName, URL ends /completed", "[snaplog][batch]")
{
    SnapLogConfig cfg;
    cfg.snaplog_base       = "https://api.snapmaker.com";
    cfg.public_hmac_secret = "secret";
    cfg.now_ms             = []() { return int64_t(1778639794110); };
    cfg.next_nonce         = []() { return std::string("nonce2"); };

    BatchRequest req = build_batch_complete_request(cfg, "app-123", /*full_key*/ "logs/2026/batch.abc.ndjson");

    REQUIRE(req.url == "https://api.snapmaker.com/api/log/public/upload/completed");
    REQUIRE(hdr(req.headers, "Authorization").empty());
    REQUIRE(hdr(req.headers, "X-Sign") == "34a742481a46fd90021b0d11e243d31ad3d0f3431de83fcbb2618be394335e8d");
    // completed requires fileName (create's returned S3 key). Server returns
    // 110004 "missing fileName" for body={} (was misread as auth-dead; sealed
    // then stranded on disk after retry exhaustion).
    REQUIRE(req.body.find("\"fileName\":\"logs/2026/batch.abc.ndjson\"") != std::string::npos);
    REQUIRE(req.body.find("uploadId") == std::string::npos);
}

TEST_CASE("build_batch_complete_request: empty signing secret has no request", "[snaplog][batch]")
{
    SnapLogConfig cfg;
    cfg.snaplog_base = "https://gw.example.com";
    cfg.now_ms       = []() { return int64_t(1); };
    cfg.next_nonce   = []() { return std::string("nonce2"); };

    BatchRequest req = build_batch_complete_request(cfg, "cid", "key1");

    REQUIRE(req.url.empty());
    REQUIRE(req.headers.empty());
}

TEST_CASE("build_batch_cancel_request: empty uploadId -> body {}", "[snaplog][batch]")
{
    SnapLogConfig cfg;
    cfg.snaplog_base       = "https://api.snapmaker.com";
    cfg.public_hmac_secret = "secret";
    cfg.now_ms             = []() { return int64_t(1778639794110); };
    cfg.next_nonce         = []() { return std::string("nonce2"); };

    BatchRequest req = build_batch_cancel_request(cfg, "app-123", /*uploadId*/ "");

    REQUIRE(req.url == "https://api.snapmaker.com/api/log/public/upload/cancel");
    REQUIRE(hdr(req.headers, "Authorization").empty());
    // Empty uploadId -> body is "{}".
    nlohmann::json parsed = nlohmann::json::parse(req.body);
    REQUIRE(parsed.is_object());
    REQUIRE(parsed.empty());
}

TEST_CASE("build_batch_cancel_request: non-empty uploadId -> body has it", "[snaplog][batch]")
{
    SnapLogConfig cfg;
    cfg.public_hmac_secret = "secret";
    cfg.now_ms             = []() { return int64_t(1778639794110); };
    cfg.next_nonce         = []() { return std::string("nonce2"); };

    BatchRequest req = build_batch_cancel_request(cfg, "cid", /*uploadId*/ "upload-xyz");

    REQUIRE(req.url == "https://api.snapmaker.com/api/log/public/upload/cancel");
    nlohmann::json parsed = nlohmann::json::parse(req.body);
    REQUIRE(parsed["uploadId"] == "upload-xyz");
}

TEST_CASE("build_batch_cancel_request: empty signing secret has no request", "[snaplog][batch]")
{
    SnapLogConfig cfg;
    cfg.snaplog_base = "https://gw.example.com";
    cfg.now_ms       = []() { return int64_t(1); };
    cfg.next_nonce   = []() { return std::string("nonce2"); };

    BatchRequest req = build_batch_cancel_request(cfg, "cid", "uid1");

    REQUIRE(req.url.empty());
    REQUIRE(req.headers.empty());
}

// ---------------------------------------------------------------------------
// Plan B Task 4: Internals batch fields + log() Buffered routing
// Buffered events go through the same admission pipeline (consent, disable
// list, rate limiter) but land in bt_queue instead of rt_queue. Eviction on
// a full batch queue is FIFO-drop-incoming (NOT the realtime Error-protect
// policy — batch is a disk channel, no Error privilege).
// ---------------------------------------------------------------------------

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
        auto        ts  = std::make_shared<int64_t>(1700000000000);
        deps.now_ms     = [ts]() { return (*ts)++; };
        deps.consent_ok = []() { return true; };
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
        std::string user_id;
        {
            std::lock_guard<std::mutex> tk(in->token_mu);
            user_id = in->user_id;
        }
        std::lock_guard<std::mutex> lk(in->queue_mu);
        in->bt_queue.push_back(SnapLogClient::LogEvent{SnapLogLevel::Info, msg, SnapLogExt{{"eventName", eventName}, {"opId", "op-" + msg}},
                                                       std::move(user_id)});
    }
};
} // anonymous namespace

TEST_CASE("batch log(): SnapLog sink lands in bt_queue", "[snaplog][batch]")
{
    FakeClient fc;
    fc.init();
    SnapLogClient::instance().log(SnapLogLevel::Info, "batch-event", SnapLogExt{{"eventName", "batch_test"}}, SinkSnapLog);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 1);
    fc.shutdown();
}

TEST_CASE("batch log(): consent false drops event", "[snaplog][batch]")
{
    FakeClient fc;
    fc.deps.consent_ok = []() { return false; };
    fc.init();
    SnapLogClient::instance().log(SnapLogLevel::Info, "no-consent", SnapLogExt{{"eventName", "batch_test"}}, SinkSnapLog);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 0);
    fc.shutdown();
}

TEST_CASE("batch log(): disabled event name not enqueued", "[snaplog][batch]")
{
    FakeClient fc;
    fc.cfg.event_disable_list.push_back("blocked_batch");
    fc.init();
    SnapLogClient::instance().log(SnapLogLevel::Info, "blocked", SnapLogExt{{"eventName", "blocked_batch"}}, SinkSnapLog);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 0);
    // A non-disabled event IS enqueued.
    SnapLogClient::instance().log(SnapLogLevel::Info, "ok", SnapLogExt{{"eventName", "ok_batch"}}, SinkSnapLog);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 1);
    fc.shutdown();
}

TEST_CASE("batch log(): rate limit throttles beyond cap", "[snaplog][batch]")
{
    FakeClient fc;
    fc.cfg.event_rate_cap_per_sec["batch_throttled"] = 2;
    fc.init();
    for (int i = 0; i < 3; ++i) {
        SnapLogClient::instance().log(SnapLogLevel::Info, "msg", SnapLogExt{{"eventName", "batch_throttled"}}, SinkSnapLog);
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
        SnapLogClient::instance().log(SnapLogLevel::Info, "fill", SnapLogExt{{"eventName", "batch_fill"}}, SinkSnapLog);
    }
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 4);
    REQUIRE(SnapLogClient::instance().batch_queue_dropped_for_test() == 0);

    // 5th event: queue full → drop incoming (FIFO-drop, NOT Error-protect).
    SnapLogClient::instance().log(SnapLogLevel::Error, "overflow", SnapLogExt{{"eventName", "batch_over"}}, SinkSnapLog);
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
    SnapLogClient::instance().log(SnapLogLevel::Info, "i1", SnapLogExt{{"eventName", "b"}}, SinkSnapLog);
    SnapLogClient::instance().log(SnapLogLevel::Info, "i2", SnapLogExt{{"eventName", "b"}}, SinkSnapLog);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 2);
    // Error on full batch queue → dropped, NOT admitted via eviction.
    SnapLogClient::instance().log(SnapLogLevel::Error, "err", SnapLogExt{{"eventName", "b"}}, SinkSnapLog);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 2);
    REQUIRE(SnapLogClient::instance().batch_queue_dropped_for_test() == 1);
    fc.shutdown();
}

TEST_CASE("batch log(): config disabled drops event", "[snaplog][batch]")
{
    FakeClient fc;
    fc.cfg.enabled = false;
    fc.init();
    SnapLogClient::instance().log(SnapLogLevel::Info, "disabled", SnapLogExt{{"eventName", "batch_test"}}, SinkSnapLog);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 0);
    fc.shutdown();
}

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

TEST_CASE("bt_worker_loop renders the event-time userId", "[snaplog][batch]")
{
    BatchFlusherFixture f;
    {
        std::lock_guard<std::mutex> tk(f.in->token_mu);
        f.in->user_id = "";
    }
    f.enqueue("anonymous-event", "identity_snapshot");
    {
        std::lock_guard<std::mutex> tk(f.in->token_mu);
        f.in->user_id = "user-after";
    }
    f.in->stop_receiving.store(true);

    SnapLogClient::bt_worker_loop(f.in);

    REQUIRE(boost::filesystem::exists(f.in->active_path));
    std::ifstream   ifs(f.in->active_path.string());
    std::string     content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    nlohmann::json parsed = nlohmann::json::parse(content.substr(0, content.size() - 1));
    REQUIRE(parsed["message"] == "anonymous-event");
    REQUIRE(parsed["userId"] == "");
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
        auto        ts  = std::make_shared<int64_t>(1700000000000);
        deps.now_ms     = [ts]() { return (*ts)++; };
        deps.consent_ok = []() { return true; };
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
        cfg.poll_interval_ms     = 1;
        cfg.drain_batch_per_tick = 256;
        cfg.drain_wall_budget_ms = 50;
        cfg.active_max_bytes     = 1024;
        cfg.batch_max_events     = 1000;
        cfg.batch_max_bytes      = 1 * 1024 * 1024;
        cfg.sealed_max_disk_mb   = 256; // big — backpressure off in upload tests
        cfg.batch_max_retry      = 3;
        cfg.line_max_bytes       = 64 * 1024;
        cfg.home_for_redact      = "/home/test";
        cfg.public_hmac_secret   = "test-public-secret";
        cfg.now_ms               = [ts]() { return (*ts)++; };
        cfg.next_nonce           = [nonce_count = std::make_shared<int>(0)]() {
            return "nonce-" + std::to_string((*nonce_count)++);
        };

        in                     = SnapLogClient::make_internals_for_test(std::move(deps), std::move(cfg));
        in->spool_dir_resolved = spool;
        in->active_path        = spool / "active.log";
        in->stop_receiving.store(false);
        in->drain_and_flush.store(false);
        in->current_batch_id = "batch00000001";
        in->events_in_active = 0;
        // Seed the account id used by the SnapLog admission gate.
        {
            std::lock_guard<std::mutex> tk(in->token_mu);
            in->user_id = "user-A";
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

TEST_CASE("bt upload: non-HTTPS create URL is refused", "[snaplog][batch]")
{
    BatchUploadFixture f;
    auto               sealed = f.make_sealed("aaa112");
    f.script.respond = [](const std::string& kind) -> SnapLogResult {
        if (kind == "create")
            return {200, R"({"code":200,"data":{"url":"http://evil.example/put","key":"k"}})", false};
        return {200, std::string{}, false};
    };

    std::thread t([&]() { SnapLogClient::bt_worker_loop(f.in); });
    REQUIRE(f.wait_until([&]() { return f.create_count.load() >= 1; }));
    REQUIRE(f.wait_until([&]() { return f.in->stop_uploads.load(); }));
    if (t.joinable())
        t.join();

    // No PUT/completed was issued and the sealed file is retained.
    REQUIRE(f.create_count.load() == 1);
    REQUIRE(f.put_count.load() == 0);
    REQUIRE(f.completed_count.load() == 0);
    REQUIRE(boost::filesystem::exists(sealed));
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

TEST_CASE("bt upload: public auth carries no account Authorization", "[snaplog][batch]")
{
    BatchUploadFixture f;
    auto               sealed = f.make_sealed("ddd444");
    // Track Authorization per kind; public lifecycle requests never carry it.
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
            std::lock_guard<std::mutex> lk(m);
            completed_tok = last_tok;
            return {200, ok_body(), false};
        }
        return {200, std::string{}, false};
    };

    std::thread t([&]() { SnapLogClient::bt_worker_loop(f.in); });

    // After create fires, the lifecycle continues with public credentials.
    REQUIRE(f.wait_until([&]() { return f.create_count.load() >= 1; }));

    REQUIRE(f.wait_until([&]() { return f.completed_count.load() >= 1; }));
    REQUIRE(f.wait_until([&]() { return !boost::filesystem::exists(sealed); }));
    f.in->stop_receiving.store(true);
    f.in->drain_and_flush.store(true);
    if (t.joinable())
        t.join();

    // Public requests carry no account Authorization.
    REQUIRE(create_tok.empty());
    REQUIRE(completed_tok.empty());
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
        c.home_for_redact          = "/home/test";
        f.in->cached_context_valid = true;
    }

    // Enqueue events that must be drained under deps_invalid.
    {
        std::lock_guard<std::mutex> lk(f.in->queue_mu);
        f.in->bt_queue.push_back(
            SnapLogClient::LogEvent{SnapLogLevel::Info, "deps-invalid-event-1", SnapLogExt{{"eventName", "di"}, {"opId", "op-di1"}}});
        f.in->bt_queue.push_back(
            SnapLogClient::LogEvent{SnapLogLevel::Info, "deps-invalid-event-2", SnapLogExt{{"eventName", "di"}, {"opId", "op-di2"}}});
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
        cfg.public_hmac_secret      = "test-public-secret";
        cfg.now_ms                  = [this]() { return now++; };
        cfg.next_nonce              = [nonce_count = 0]() mutable {
            return "lifecycle-nonce-" + std::to_string(nonce_count++);
        };
    }

    void init()
    {
        SnapLogDeps   d = deps;
        SnapLogConfig c = cfg;
        SnapLogClient::instance().init(std::move(d), std::move(c));
        SnapLogClient::instance().set_user_id("user-1"); // payload identity
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

TEST_CASE("batch lifecycle: consent OFF purge retries after spool lock is released", "[snaplog][batch]")
{
    BatchLifecycleFixture f;
    namespace fs = boost::filesystem;

    {
        fs::ofstream ofs((f.spool / "active.log").string(), std::ios::binary);
        ofs << "{\"level\":\"INFO\",\"message\":\"locked-recovery\"}\n";
    }
    auto prior_sealed = f.spool / "batch.1699999999000.locked0000.sealed";
    {
        fs::ofstream ofs(prior_sealed.string(), std::ios::binary);
        ofs << "{\"level\":\"WARN\",\"message\":\"locked-sealed\"}\n";
    }

    auto other_owner = SnapLogClient::try_acquire_spool_lock(f.spool);
    REQUIRE(other_owner.acquired);

    f.deps.consent_ok = []() { return false; };
    f.init();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    other_owner = {};
    REQUIRE(f.wait_until(
        [&]() {
            boost::system::error_code ec;
            return list_sealed(f.spool).empty() && !fs::exists(f.spool / "active.log", ec);
        },
        3000));

    REQUIRE_FALSE(SnapLogClient::instance().batch_worker_joinable_for_test());
    REQUIRE(f.create_count.load() == 0);
    f.shutdown();
}

TEST_CASE("batch lifecycle: anonymous state uploads sealed files", "[snaplog][batch]")
{
    BatchLifecycleFixture f;
    namespace fs = boost::filesystem;

    f.init();
    SnapLogClient::instance().set_user_id("");

    auto sealed = f.spool / "batch.1699999999000.logout0000.sealed";
    {
        fs::ofstream ofs(sealed.string(), std::ios::binary);
        ofs << "{\"level\":\"INFO\",\"message\":\"logout-sealed\"}\n";
    }

    REQUIRE(f.wait_until([&]() { return f.completed_count.load() == 1 && !fs::exists(sealed); }, 3000));
    REQUIRE(f.create_count.load() == 1);
    REQUIRE(f.put_count.load() == 1);
    REQUIRE(f.completed_count.load() == 1);
    REQUIRE(f.cancel_count.load() == 0);
    REQUIRE_FALSE(fs::exists(sealed));
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
// ---------------------------------------------------------------------------
// Fix A: make_upload_file_name produces unique fileNames per call.
// Two calls with the same batchId must differ (different now_ms / seq).
// ---------------------------------------------------------------------------
TEST_CASE("batch lifecycle: shutdown drain flushes bt_queue to sealed and uploads", "[snaplog][batch]")
{
    BatchLifecycleFixture f;

    f.init();

    // Enqueue some batch events.
    for (int i = 0; i < 3; ++i) {
        SnapLogClient::instance().log(SnapLogLevel::Info, "drain-event-" + std::to_string(i),
                                      SnapLogExt{{"eventName", "drain_test"}, {"opId", "op" + std::to_string(i)}}, SinkSnapLog);
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
                                  SinkSnapLog);
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
    SnapLogClient::instance().log(SnapLogLevel::Info, "after-off", SnapLogExt{{"eventName", "consent_test"}, {"opId", "op2"}}, SinkSnapLog);
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
                                  SinkSnapLog);
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
    SnapLogClient::instance().log(SnapLogLevel::Info, "flush-event", SnapLogExt{{"eventName", "flush_test"}, {"opId", "opf"}}, SinkSnapLog);
    // Periodic flush (1s) -> rotate -> create+PUT+completed -> sealed deleted.
    REQUIRE(f.wait_until([&]() { return list_sealed(f.spool).empty() && f.create_count.load() >= 1; }, 5000));
    f.shutdown();
}

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

    auto now = std::make_shared<int64_t>(1700000000000);

    SnapLogDeps deps;
    deps.now_ms     = [now]() { return (*now)++; };
    deps.consent_ok = []() { return true; };
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
    cfg.public_hmac_secret      = "test-public-secret";
    cfg.now_ms                  = [now]() { return (*now)++; };
    cfg.next_nonce              = [nonce_count = 0]() mutable {
        return "timeout-nonce-" + std::to_string(nonce_count++);
    };

    {
        SnapLogDeps   d = deps;
        SnapLogConfig c = cfg;
        SnapLogClient::instance().init(std::move(d), std::move(c));
    }
    SnapLogClient::instance().set_user_id("user-1"); // payload identity

    // Enqueue an event; the worker drains it, rotates to sealed, then calls
    // create (which blocks on the gate).
    SnapLogClient::instance().log(SnapLogLevel::Info, "stuck-upload", SnapLogExt{{"eventName", "timeout_test"}, {"opId", "op1"}},
                                  SinkSnapLog);

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
    SnapLogClient::instance().set_user_id("user-1");
    REQUIRE_FALSE(SnapLogClient::instance().batch_worker_joinable_for_test());

    // OFF -> ON records consent as true, but the replacement waits for the
    // retired worker so two batch owners never touch the spool concurrently.
    SnapLogClient::instance().set_consent(true);
    REQUIRE_FALSE(SnapLogClient::instance().batch_worker_joinable_for_test());

    // Events accepted after ON remain queued across the deferred purge.
    SnapLogClient::instance().log(SnapLogLevel::Info, "replacement-upload", SnapLogExt{{"eventName", "timeout_recovery"}, {"opId", "op2"}},
                                  SinkSnapLog);

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

TEST_CASE("SnapLog sink is dropped while another process owns the spool lock", "[snaplog][batch][pipeline]")
{
    BatchLifecycleFixture f;

    auto other_owner = SnapLogClient::try_acquire_spool_lock(f.spool);
    REQUIRE(other_owner.acquired);

    f.init();
    REQUIRE_FALSE(SnapLogClient::instance().batch_worker_joinable_for_test());

    SnapLogClient::instance().log(SnapLogLevel::Info, "locked", SnapLogExt{{"eventName", "spool_locked"}}, SinkSnapLog);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 0);

    other_owner = {};
    f.shutdown();
}
