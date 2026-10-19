#include <catch2/catch_test_macros.hpp>

// Snapmaker Orca: rules of slic3r/GUI/SnapLogWiring, tested through pure functions and a counting
// fake transport (no network). Expected consent is scaled by compiled_in() for -DSLIC3R_SNAPLOG=OFF.
#include "slic3r/GUI/SnapLogWiring.hpp"

#include "libslic3r/AppConfig.hpp"

#include "test_utils.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

using namespace Slic3r;
using namespace Slic3r::SnapLog::v1;
namespace Wiring = Slic3r::GUI::SnapLogWiring;

namespace {

using Headers = std::vector<std::pair<std::string, std::string>>;

const char* const CREATE_URL        = "https://api.snapmaker.com/api/log/upload/create";
const char* const CANCEL_URL        = "https://api.snapmaker.com/api/log/upload/cancel";
const char* const PRINT_URL         = "https://api.snapmaker.com/api/log/upload/print";
const char* const PUBLIC_CREATE_URL = "https://api.snapmaker.com/api/log/public/upload/create";

Headers bearer_headers()
{
    return {{"Authorization", "Bearer token"}, {"X-Client-Type", "Orca"}, {"X-Client-Id", "client"}};
}

Headers signed_headers()
{
    return {{"X-Client-Type", "Orca"}, {"X-Client-Id", "client"}, {"X-Timestamp", "1"}, {"X-Nonce", "n"}, {"X-Sign", "00"}};
}

// What the fake transport saw. Shared, so a closure that outlives a case never dangles.
struct Seen
{
    std::atomic<int>               calls{0};
    std::string                    method;
    std::string                    url;
    Headers                        headers;
    const boost::filesystem::path* body_file = nullptr;
    const std::string*             body_str  = nullptr;
};

std::shared_ptr<SnapLogHandle> answered(int status)
{
    auto handle  = std::make_shared<SnapLogHandle>();
    handle->prom = std::make_shared<std::promise<SnapLogResult>>();
    handle->prom->set_value({status, std::string{}, false});
    handle->fulfilled.store(true);
    return handle;
}

Wiring::DoRequest counting_inner(const std::shared_ptr<Seen>& seen)
{
    return [seen](const std::string& method, const std::string& url, Headers headers, const boost::filesystem::path* body_file,
                  const std::string* body_str) {
        seen->calls.fetch_add(1);
        seen->method    = method;
        seen->url       = url;
        seen->headers   = std::move(headers);
        seen->body_file = body_file;
        seen->body_str  = body_str;
        return answered(200);
    };
}

Wiring::GuardPolicy policy_with_key(bool signed_ok)
{
    Wiring::GuardPolicy policy;
    policy.signed_ok = signed_ok;
    return policy;
}

int status_of(const std::shared_ptr<SnapLogHandle>& handle)
{
    REQUIRE(handle);
    REQUIRE(handle->prom);
    return handle->prom->get_future().get().status;
}

} // namespace

TEST_CASE("The guard refuses a signed request with 403 when the build has no key", "[SnapLogWiring]")
{
    auto       seen  = std::make_shared<Seen>();
    const auto guard = Wiring::guard_do_request(counting_inner(seen), policy_with_key(false));

    const std::string body   = "{}";
    const auto        handle = guard("POST", PUBLIC_CREATE_URL, signed_headers(), nullptr, &body);

    REQUIRE(handle);
    CHECK(handle->done());
    CHECK_FALSE(handle->cancelled.load());
    CHECK_FALSE(static_cast<bool>(handle->http));
    const SnapLogResult result = handle->prom->get_future().get();
    CHECK(result.status == 403);
    CHECK_FALSE(result.cancelled);
    CHECK(seen->calls.load() == 0);

    // The header name is matched without regard to case.
    Headers lower = signed_headers();
    lower.back().first = "x-sign";
    CHECK(status_of(guard("POST", "https://api.snapmaker.com/api/log/public/upload/print", lower, nullptr, &body)) == 403);
    CHECK(seen->calls.load() == 0);
}

TEST_CASE("The guard forwards a signed request when the build has a key", "[SnapLogWiring]")
{
    auto       seen  = std::make_shared<Seen>();
    const auto guard = Wiring::guard_do_request(counting_inner(seen), policy_with_key(true));

    const std::string body = "{}";
    CHECK(status_of(guard("POST", PUBLIC_CREATE_URL, signed_headers(), nullptr, &body)) == 200);
    CHECK(seen->calls.load() == 1);
}

TEST_CASE("The guard refuses a URL that is not https with 403", "[SnapLogWiring]")
{
    auto       seen  = std::make_shared<Seen>();
    const auto guard = Wiring::guard_do_request(counting_inner(seen), policy_with_key(true));

    const std::string body = "{}";
    CHECK(status_of(guard("POST", "http://api.snapmaker.com/api/log/upload/print", bearer_headers(), nullptr, &body)) == 403);
    CHECK(status_of(guard("POST", "ftp://api.snapmaker.com/api/log/upload/print", bearer_headers(), nullptr, &body)) == 403);
    CHECK(status_of(guard("POST", "", bearer_headers(), nullptr, &body)) == 403);
    CHECK(seen->calls.load() == 0);

    // The scheme is matched without regard to case.
    CHECK(status_of(guard("POST", "HTTPS://api.snapmaker.com/api/log/upload/print", bearer_headers(), nullptr, &body)) == 200);
    CHECK(seen->calls.load() == 1);
}

TEST_CASE("The guard forwards an https Bearer request unchanged", "[SnapLogWiring]")
{
    auto       seen  = std::make_shared<Seen>();
    const auto guard = Wiring::guard_do_request(counting_inner(seen), policy_with_key(false));

    const std::string             body = "{\"k\":1}";
    const boost::filesystem::path file("sealed-file");

    CHECK(status_of(guard("POST", PRINT_URL, bearer_headers(), nullptr, &body)) == 200);
    CHECK(seen->calls.load() == 1);
    CHECK(seen->method == "POST");
    CHECK(seen->url == PRINT_URL);
    CHECK(seen->headers == bearer_headers());
    CHECK(seen->body_file == nullptr);
    CHECK(seen->body_str == &body);

    // The presigned upload: no headers, a file instead of a string.
    const std::string put_url = "https://bucket.example/key?X-Amz-Signature=abc";
    CHECK(status_of(guard("PUT", put_url, Headers{}, &file, nullptr)) == 200);
    CHECK(seen->calls.load() == 2);
    CHECK(seen->method == "PUT");
    CHECK(seen->url == put_url);
    CHECK(seen->headers.empty());
    CHECK(seen->body_file == &file);
    CHECK(seen->body_str == nullptr);
}

TEST_CASE("An insecure PUT makes the guard refuse the next create", "[SnapLogWiring]")
{
    auto       seen  = std::make_shared<Seen>();
    const auto guard = Wiring::guard_do_request(counting_inner(seen), policy_with_key(true));

    const std::string             body = "{}";
    const boost::filesystem::path file("sealed-file");

    CHECK(status_of(guard("PUT", "http://bucket.example/key", Headers{}, &file, nullptr)) == 403);
    CHECK(status_of(guard("POST", CREATE_URL, bearer_headers(), nullptr, &body)) == 403);
    CHECK(status_of(guard("POST", CREATE_URL, bearer_headers(), nullptr, &body)) == 403);
    CHECK(seen->calls.load() == 0);

    // A plain-http request that is not a PUT is refused, but poisons nothing.
    auto       seen_post  = std::make_shared<Seen>();
    const auto guard_post = Wiring::guard_do_request(counting_inner(seen_post), policy_with_key(true));
    CHECK(status_of(guard_post("POST", "http://api.snapmaker.com/api/log/upload/print", bearer_headers(), nullptr, &body)) == 403);
    CHECK(status_of(guard_post("POST", CREATE_URL, bearer_headers(), nullptr, &body)) == 200);
    CHECK(seen_post->calls.load() == 1);
}

TEST_CASE("The guard paces create requests with a token bucket", "[SnapLogWiring]")
{
    auto seen = std::make_shared<Seen>();
    auto now  = std::make_shared<std::atomic<int64_t>>(1000000);

    Wiring::GuardPolicy policy = policy_with_key(true);
    policy.steady_ms           = [now]() { return now->load(); };
    REQUIRE(policy.create_burst == 8);
    REQUIRE(policy.create_refill_sec == 15);
    const auto guard = Wiring::guard_do_request(counting_inner(seen), policy);

    const std::string body = "{}";

    // The burst: eight creates leave, and their cancels too.
    for (int i = 0; i < 8; ++i)
        CHECK(status_of(guard("POST", CREATE_URL, bearer_headers(), nullptr, &body)) == 200);
    CHECK(seen->calls.load() == 8);
    CHECK(status_of(guard("POST", CANCEL_URL, bearer_headers(), nullptr, &body)) == 200);
    CHECK(seen->calls.load() == 9);

    // The ninth is refused as a transport failure, and so is the cancel that follows it.
    CHECK(status_of(guard("POST", CREATE_URL, bearer_headers(), nullptr, &body)) == 0);
    CHECK(status_of(guard("POST", CANCEL_URL, bearer_headers(), nullptr, &body)) == 0);
    CHECK(seen->calls.load() == 9);

    // One millisecond short of a token.
    now->store(1000000 + 14999);
    CHECK(status_of(guard("POST", CREATE_URL, bearer_headers(), nullptr, &body)) == 0);
    CHECK(seen->calls.load() == 9);

    // After 15 000 ms one more create leaves, and a cancel is forwarded again.
    now->store(1000000 + 15000);
    CHECK(status_of(guard("POST", CREATE_URL, bearer_headers(), nullptr, &body)) == 200);
    CHECK(seen->calls.load() == 10);
    CHECK(status_of(guard("POST", CANCEL_URL, bearer_headers(), nullptr, &body)) == 200);
    CHECK(seen->calls.load() == 11);
    CHECK(status_of(guard("POST", CREATE_URL, bearer_headers(), nullptr, &body)) == 0);
    CHECK(seen->calls.load() == 11);

    // A long pause refills the bucket to its capacity, not beyond it.
    now->store(1000000 + 15000 + 3600 * 1000);
    for (int i = 0; i < 8; ++i)
        CHECK(status_of(guard("POST", CREATE_URL, bearer_headers(), nullptr, &body)) == 200);
    CHECK(status_of(guard("POST", CREATE_URL, bearer_headers(), nullptr, &body)) == 0);
    CHECK(seen->calls.load() == 19);

    // The anonymous endpoint shares the bucket; a query string does not hide the path.
    CHECK(status_of(guard("POST", std::string(PUBLIC_CREATE_URL) + "?x=1", signed_headers(), nullptr, &body)) == 0);
    CHECK(seen->calls.load() == 19);
}

TEST_CASE("The guard never paces the realtime print URL", "[SnapLogWiring]")
{
    auto seen = std::make_shared<Seen>();

    Wiring::GuardPolicy policy = policy_with_key(true);
    policy.steady_ms           = []() { return int64_t(5); }; // the clock never moves
    const auto guard           = Wiring::guard_do_request(counting_inner(seen), policy);

    const std::string body = "{}";
    for (int i = 0; i < 100; ++i)
        CHECK(status_of(guard("POST", PRINT_URL, bearer_headers(), nullptr, &body)) == 200);
    CHECK(seen->calls.load() == 100);

    // Neither are "completed" and the presigned PUT, also with the bucket empty.
    for (int i = 0; i < 9; ++i)
        guard("POST", CREATE_URL, bearer_headers(), nullptr, &body);
    CHECK(seen->calls.load() == 108);
    const boost::filesystem::path file("sealed-file");
    CHECK(status_of(guard("POST", "https://api.snapmaker.com/api/log/upload/completed", bearer_headers(), nullptr, &body)) == 200);
    CHECK(status_of(guard("PUT", "https://bucket.example/key", Headers{}, &file, nullptr)) == 200);
    CHECK(seen->calls.load() == 110);
}

TEST_CASE("A refused handle is consumed exactly once", "[SnapLogWiring]")
{
    auto       seen  = std::make_shared<Seen>();
    const auto guard = Wiring::guard_do_request(counting_inner(seen), policy_with_key(false));

    const std::string body   = "{}";
    const auto        handle = guard("POST", "http://insecure.example/api/log/upload/print", bearer_headers(), nullptr, &body);
    REQUIRE(handle);
    REQUIRE(handle->prom);

    // The guard must not have taken the future: the client takes it, once.
    std::future<SnapLogResult> future = handle->prom->get_future();
    REQUIRE(future.valid());
    REQUIRE(future.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    const SnapLogResult result = future.get();
    CHECK(result.status == 403);
    CHECK(result.body.empty());
    CHECK_FALSE(result.cancelled);
    CHECK_THROWS_AS(handle->prom->get_future(), std::future_error);

    // cancel() on a fulfilled handle must not fulfil the promise a second time.
    handle->cancel();
    CHECK(handle->done());
}

TEST_CASE("The guard refuses instead of calling an empty transport", "[SnapLogWiring]")
{
    const auto        guard = Wiring::guard_do_request(Wiring::DoRequest{}, policy_with_key(true));
    const std::string body  = "{}";
    CHECK(status_of(guard("POST", PRINT_URL, bearer_headers(), nullptr, &body)) == 0);
}

TEST_CASE("Effective consent needs the build switch, the programme flag and the upload switch", "[SnapLogWiring]")
{
    const char* const PRIVACY_KEY = "privacy_policy_isagree";

    // A new profile: the programme flag is unseeded, the upload switch is seeded on.
    {
        AppConfig fresh;
        CHECK(fresh.get(PRIVACY_KEY).empty());
        CHECK(fresh.get_bool("snaplog_upload"));
        CHECK_FALSE(Wiring::effective_consent(fresh));
    }

    const std::vector<std::string> privacy_values = {"", "true", "false", "1", "0"}; // "" = unset
    const std::vector<std::string> upload_values  = {"true", "false", "1", "0"};

    for (const std::string& privacy : privacy_values) {
        for (const std::string& upload : upload_values) {
            AppConfig config;
            if (!privacy.empty())
                config.set("app", PRIVACY_KEY, privacy);
            config.set("app", "snaplog_upload", upload);

            const bool privacy_bool = privacy == "true" || privacy == "1";
            const bool upload_bool  = upload == "true" || upload == "1";
            const bool expected     = Wiring::compiled_in() && privacy_bool && upload_bool;

            INFO("privacy '" << privacy << "' upload '" << upload << "'");
            CHECK(Wiring::effective_consent(config) == expected);
        }
    }

    // A key needs a build that compiles the upload.
    if (!Wiring::compiled_in()) {
        CHECK_FALSE(Wiring::has_public_key());
    }
}

TEST_CASE("Scrubbed text keeps the first line only", "[SnapLogWiring]")
{
    const std::string home = "/Users/someone";
    CHECK(Wiring::scrub_text("first line\nsecond line\nthird", home) == "first line");
    CHECK(Wiring::scrub_text("first line\r\nsecond line", home) == "first line");
    CHECK(Wiring::scrub_text("", home).empty());
    CHECK(Wiring::scrub_text("\nonly a second line", home).empty());
}

TEST_CASE("Scrubbed text hides the home directory anywhere in the line", "[SnapLogWiring]")
{
    const std::string home = "/Users/someone";
    CHECK(Wiring::scrub_text("Failed to read /Users/someone/models/a.stl and /Users/someone/b.3mf", home) ==
          "Failed to read ~/models/a.stl and ~/b.3mf");
    CHECK(Wiring::scrub_text("/Users/someone/a.stl", home) == "~/a.stl");
    // No home known, or a home that is a single separator: the text stays as it is.
    CHECK(Wiring::scrub_text("see /Users/someone/a.stl", "") == "see /Users/someone/a.stl");
    CHECK(Wiring::scrub_text("see /Users/someone/a.stl", "/") == "see /Users/someone/a.stl");
}

TEST_CASE("Scrubbed text is capped at 512 bytes on a UTF-8 boundary", "[SnapLogWiring]")
{
    const std::string home = "/Users/someone";
    CHECK(Wiring::scrub_text(std::string(512, 'a'), home).size() == 512);
    CHECK(Wiring::scrub_text(std::string(2000, 'a'), home) == std::string(512, 'a'));

    // 511 ASCII bytes, then two-byte characters: byte 512 would split the first of them.
    const std::string e_acute = "\xC3\xA9";
    std::string       text(511, 'a');
    for (int i = 0; i < 10; ++i)
        text += e_acute;
    CHECK(Wiring::scrub_text(text, home) == std::string(511, 'a'));

    // 510 ASCII bytes: the first two-byte character ends exactly at the cap and stays.
    std::string fits(510, 'a');
    for (int i = 0; i < 10; ++i)
        fits += e_acute;
    CHECK(Wiring::scrub_text(fits, home) == std::string(510, 'a') + e_acute);

    // A three-byte character across the cap is dropped as a whole.
    const std::string euro = "\xE2\x82\xAC";
    std::string       wide(510, 'a');
    wide += euro;
    wide += euro;
    CHECK(Wiring::scrub_text(wide, home) == std::string(510, 'a'));
}

TEST_CASE("A build without key parks a leftover sealed file without traffic", "[SnapLogWiring]")
{
    namespace fs = boost::filesystem;

    const ScopedTemporaryDir spool_dir("snaplog-wiring");
    const fs::path           spool  = spool_dir.path();
    const fs::path           sealed = spool / "batch.1700000000000.wiring01.sealed";
    {
        fs::ofstream out(sealed, std::ios::binary);
        out << "{\"eventName\":\"leftover\"}\n";
    }
    REQUIRE(fs::exists(sealed));

    auto seen = std::make_shared<Seen>();

    SnapLogDeps deps;
    deps.do_request = Wiring::guard_do_request(counting_inner(seen), policy_with_key(false));
    deps.consent_ok = []() { return true; };
    deps.machine_id = []() { return std::string("wiring-test-client"); };
    deps.now_ms     = []() { return int64_t(0); };

    SnapLogConfig cfg;
    cfg.spool_dir        = spool.string();
    cfg.poll_interval_ms = 1;
    // Signed out (no token) and no key: the create request of the client carries X-Sign.
    SnapLogClient::instance().init(std::move(deps), std::move(cfg));

    // The latch is expected within 500 ms; the wait is longer for a loaded machine.
    bool       latched = false;
    const auto start   = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        if (SnapLogClient::instance().auth_known_dead_for_test()) {
            latched = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    INFO("waited " << waited_ms << " ms");

    CHECK(latched);
    // A few more ticks: the latch must hold the client still.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(seen->calls.load() == 0);
    CHECK(fs::exists(sealed));

    SnapLogClient::instance().shutdown();
    CHECK(fs::exists(sealed));
}
