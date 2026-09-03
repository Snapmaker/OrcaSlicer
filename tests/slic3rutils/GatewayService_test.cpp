#include <catch2/catch.hpp>

#include "slic3r/Utils/GatewayService.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace Slic3r::Gateway;

namespace {

struct FakeHttp final : public HttpTransport
{
    std::vector<std::string> urls;
    std::vector<std::string> posts;
    std::string              store_response{R"({"ok":true,"data":{"file_exists":true}})"};

    HttpResponse get(const std::string& url) override
    {
        urls.push_back(url);
        if (url.find("/health") != std::string::npos) {
            return {200,
                    R"({"status":"ok","cli_version":"1.0","components":{"ipc_server":"ok","web_server":"ok"},)"
                    R"("server_url":{"base_url":"http://127.0.0.1:8080/","home_page":"/index","device_control":""}})",
                    {}};
        }
        if (url.find("/api/device") != std::string::npos)
            return {200, R"({"devices":[{"sn":"A1"}]})", {}};
        if (url.find("/api/account") != std::string::npos)
            return {200, R"({"user":"u"})", {}};
        return {404, {}, "not found"};
    }

    HttpResponse post_json(const std::string& url, const std::string& body) override
    {
        posts.push_back(url + "\n" + body);
        return {200, store_response, {}};
    }
};

struct FakeWebSocket final : public WebSocketTransport
{
    Listener                 listener;
    std::atomic<int>         connections{0};
    std::atomic<bool>        fail_first{false};
    std::vector<std::string> sent;
    std::mutex               mutex;

    void set_listener(Listener value) override { listener = std::move(value); }

    void connect(std::uint16_t port, const std::string& path) override
    {
        REQUIRE(port == 8888);
        REQUIRE(path == "/ws");
        const int connection = connections.fetch_add(1) + 1;
        if (connection == 1 && fail_first.load()) {
            listener.closed("connection refused");
            return;
        }
        listener.opened();
    }

    bool send(const std::string& message) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            sent.push_back(message);
        }
        const nlohmann::json request = nlohmann::json::parse(message);
        if (request.value("method", "") == "action.device.watch") {
            listener.message(nlohmann::json{{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", {{"watching", true}}}}.dump());
        } else if (request.value("method", "") == "sync.echo") {
            listener.message(nlohmann::json{{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", {{"echoed", true}}}}.dump());
        }
        // "sync.silent" intentionally gets no response (timeout / failure scenarios).
        return true;
    }

    void close() override
    {
        if (listener.closed)
            listener.closed("closed");
    }

    void notify(const std::string& method, const nlohmann::json& params)
    { listener.message(nlohmann::json{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}}.dump()); }
};

bool wait_for_state(const GatewayService& service, ConnectionState expected)
{
    std::mutex                   mutex;
    std::condition_variable      condition;
    bool                         ready = false;
    std::thread                  waiter([&] {
        while (service.state() != expected)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        std::lock_guard<std::mutex> lock(mutex);
        ready = true;
        condition.notify_all();
    });
    std::unique_lock<std::mutex> lock(mutex);
    const bool                   result = condition.wait_for(lock, std::chrono::seconds{2}, [&] { return ready; });
    lock.unlock();
    waiter.join();
    return result;
}

GatewayService::Dependencies make_dependencies(std::shared_ptr<ConnectionProcessManager>& process,
                                               std::shared_ptr<FakeHttp>&                 http,
                                               std::shared_ptr<FakeWebSocket>&            websocket)
{
    process   = std::make_shared<ConnectionProcessManager>(ConnectionProcessManager::Config{boost::filesystem::path{
                                                               "snapmaker_connection.exe"}},
                                                           [](const std::vector<std::string>&) {
                                                             ConnectionProcessManager::ProcessRunResult result;
                                                             result.stdout_data = "PORT:8888\r\n\r\n";
                                                             return result;
                                                           });
    http      = std::make_shared<FakeHttp>();
    websocket = std::make_shared<FakeWebSocket>();
    GatewayService::Dependencies dependencies;
    dependencies.process_manager = process;
    dependencies.http            = http;
    dependencies.websocket       = websocket;
    return dependencies;
}

} // namespace

TEST_CASE("GatewayService connects, handles device watch, and calls HTTP APIs", "[gateway][service]")
{
    std::shared_ptr<ConnectionProcessManager> process;
    std::shared_ptr<FakeHttp>                 http;
    std::shared_ptr<FakeWebSocket>            websocket;
    GatewayService                            service(GatewayService::Config{}, make_dependencies(process, http, websocket));

    std::atomic<int> account_changes{0};
    service.set_notification_handler("notify.account.changed", [&](const nlohmann::json&) { account_changes += 1; });
    REQUIRE(service.start("zh-CN"));
    REQUIRE(wait_for_state(service, ConnectionState::Connected));
    REQUIRE(service.port() == 8888);
    REQUIRE(service.health().cli_version == "1.0");
    REQUIRE(service.web_url("home_page") == "http://127.0.0.1:8080/index");
    REQUIRE(service.web_url("device_control").empty());
    REQUIRE(service.base_url() == "http://127.0.0.1:8080/");
    REQUIRE(service.localfile_url("folder/a file.gcode") == "http://127.0.0.1:8080/localfile/folder%2Fa%20file.gcode");

    std::promise<nlohmann::json> watch_promise;
    auto                         watch_future = watch_promise.get_future();
    const std::int64_t request_id = service.watch_device({{"sn", "A1"}}, [&watch_promise](GatewayError error, const nlohmann::json& result) {
        REQUIRE(!error);
        watch_promise.set_value(result);
    });
    REQUIRE(request_id > 0);
    REQUIRE(watch_future.get().at("watching") == true);

    const auto device = service.get_device();
    REQUIRE(!device.error);
    REQUIRE(device.value.at("devices").at(0).at("sn") == "A1");

    const auto account = service.get_account();
    REQUIRE(!account.error);
    REQUIRE(account.value.at("user") == "u");

    const auto store = service.store_preprint_context("store-id", {{"file_path", "C:/tmp/a.gcode"}}, 900);
    REQUIRE(!store.error);
    REQUIRE(store.ok);
    REQUIRE(store.file_exists);
    REQUIRE(http->posts.size() == 1);
    const auto posted = nlohmann::json::parse(http->posts[0].substr(http->posts[0].find('\n') + 1));
    REQUIRE(http->posts[0].substr(0, http->posts[0].find('\n')) == "http://127.0.0.1:8080/api/store");
    REQUIRE(posted.at("id") == "store-id");
    REQUIRE(posted.at("ttl_seconds") == 900);
    REQUIRE(posted.at("payload").at("file_path") == "C:/tmp/a.gcode");

    http->store_response = R"({"ok":true,"data":{"file_exists":false}})";
    const auto missing_file = service.store_preprint_context("store-id", {{"file_path", "C:/tmp/missing.gcode"}}, 900);
    REQUIRE(missing_file.ok);
    REQUIRE_FALSE(missing_file.file_exists);

    http->store_response = R"({"ok":false})";
    const auto failed_store = service.store_preprint_context("store-id", {{"file_path", "C:/tmp/a.gcode"}}, 900);
    REQUIRE_FALSE(failed_store.ok);
    REQUIRE(failed_store.error.code == GatewayErrorCode::InvalidResponse);

    const auto invalid_request = service.store_preprint_context("", nlohmann::json::object(), 900);
    REQUIRE(invalid_request.error.code == GatewayErrorCode::InvalidRequest);
    REQUIRE(http->posts.size() == 3);

    websocket->notify("notify.account.changed", {{"reason", "login"}});
    REQUIRE(account_changes.load() == 1);

    service.stop();
    REQUIRE(service.state() == ConnectionState::Disconnected);
}

TEST_CASE("GatewayService reconnects after the first websocket failure", "[gateway][service]")
{
    std::shared_ptr<ConnectionProcessManager> process;
    std::shared_ptr<FakeHttp>                 http;
    std::shared_ptr<FakeWebSocket>            websocket;
    GatewayService::Config                    config;
    config.reconnect.initial_delay = std::chrono::milliseconds{1};
    config.reconnect.max_delay     = std::chrono::milliseconds{1};
    GatewayService service(config, make_dependencies(process, http, websocket));
    websocket->fail_first = true;

    REQUIRE(service.start("en-US"));
    REQUIRE(wait_for_state(service, ConnectionState::Connected));
    REQUIRE(service.wait_for_connected(std::chrono::milliseconds{100}));
    REQUIRE(websocket->connections.load() >= 2);
    service.stop();
}

TEST_CASE("GatewayService bounded wait fails when the gateway cannot start", "[gateway][service]")
{
    std::shared_ptr<ConnectionProcessManager> process =
        std::make_shared<ConnectionProcessManager>(ConnectionProcessManager::Config{boost::filesystem::path{"snapmaker_connection.exe"}},
                                                   [](const std::vector<std::string>&) {
                                                       ConnectionProcessManager::ProcessRunResult result;
                                                       result.launch_failed = true;
                                                       return result;
                                                   });

    GatewayService::Config config;
    config.reconnect.initial_delay = std::chrono::milliseconds{1};
    config.reconnect.max_delay     = std::chrono::milliseconds{1};
    GatewayService::Dependencies dependencies;
    dependencies.process_manager = process;
    dependencies.http            = std::make_shared<FakeHttp>();
    dependencies.websocket       = std::make_shared<FakeWebSocket>();
    GatewayService service(config, dependencies);

    REQUIRE(service.start("en-US"));
    REQUIRE_FALSE(service.wait_for_connected(std::chrono::milliseconds{20}));
    REQUIRE(service.localfile_url("a.gcode").empty());
    service.stop();
}

TEST_CASE("request_sync returns the rpc result", "[gateway][service][sync]")
{
    std::shared_ptr<ConnectionProcessManager> process;
    std::shared_ptr<FakeHttp>                 http;
    std::shared_ptr<FakeWebSocket>            websocket;
    GatewayService                            service(GatewayService::Config{}, make_dependencies(process, http, websocket));

    REQUIRE(service.start("zh-CN"));
    REQUIRE(wait_for_state(service, ConnectionState::Connected));

    const auto result = service.request_sync("sync.echo", nlohmann::json::object(), std::chrono::milliseconds{1000});
    REQUIRE_FALSE(result.error);
    REQUIRE(result.value.at("echoed") == true);

    service.stop();
}

TEST_CASE("request_sync times out when nothing answers", "[gateway][service][sync]")
{
    std::shared_ptr<ConnectionProcessManager> process;
    std::shared_ptr<FakeHttp>                 http;
    std::shared_ptr<FakeWebSocket>            websocket;
    GatewayService                            service(GatewayService::Config{}, make_dependencies(process, http, websocket));

    REQUIRE(service.start("zh-CN"));
    REQUIRE(wait_for_state(service, ConnectionState::Connected));

    const auto start  = std::chrono::steady_clock::now();
    const auto result = service.request_sync("sync.silent", nlohmann::json::object(), std::chrono::milliseconds{200});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(result.error);
    REQUIRE(result.error.code == GatewayErrorCode::TransportError);
    REQUIRE(result.error.message.find("timed out") != std::string::npos);
    REQUIRE(elapsed < std::chrono::seconds{2});

    service.stop();
}

TEST_CASE("request_sync fails fast when the websocket drops while waiting", "[gateway][service][sync]")
{
    std::shared_ptr<ConnectionProcessManager> process;
    std::shared_ptr<FakeHttp>                 http;
    std::shared_ptr<FakeWebSocket>            websocket;
    GatewayService                            service(GatewayService::Config{}, make_dependencies(process, http, websocket));

    REQUIRE(service.start("zh-CN"));
    REQUIRE(wait_for_state(service, ConnectionState::Connected));

    std::atomic<int>          error_code{-1};
    std::atomic<long long>    elapsed_ms{0};
    std::thread               caller([&] {
        const auto start  = std::chrono::steady_clock::now();
        const auto result = service.request_sync("sync.silent", nlohmann::json::object(), std::chrono::milliseconds{10000});
        const auto end    = std::chrono::steady_clock::now();
        elapsed_ms        = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        if (result.error)
            error_code = static_cast<int>(result.error.code);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    websocket->close(); // fail_pending must complete the sync request directly, not via the dispatcher

    caller.join();
    REQUIRE(error_code.load() == static_cast<int>(GatewayErrorCode::NotConnected));
    REQUIRE(elapsed_ms.load() < 3000); // nowhere near the 10 s timeout

    service.stop();
}

TEST_CASE("request_sync fails fast when the service stops while waiting", "[gateway][service][sync]")
{
    std::shared_ptr<ConnectionProcessManager> process;
    std::shared_ptr<FakeHttp>                 http;
    std::shared_ptr<FakeWebSocket>            websocket;
    GatewayService                            service(GatewayService::Config{}, make_dependencies(process, http, websocket));

    REQUIRE(service.start("zh-CN"));
    REQUIRE(wait_for_state(service, ConnectionState::Connected));

    std::atomic<int>       error_code{-1};
    std::atomic<long long> elapsed_ms{0};
    std::thread            caller([&] {
        const auto start  = std::chrono::steady_clock::now();
        const auto result = service.request_sync("sync.silent", nlohmann::json::object(), std::chrono::milliseconds{10000});
        const auto end    = std::chrono::steady_clock::now();
        elapsed_ms        = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        if (result.error)
            error_code = static_cast<int>(result.error.code);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    service.stop(); // fail_pending must complete the sync request directly, not via the dispatcher

    caller.join();
    const int code = error_code.load();
    REQUIRE((code == static_cast<int>(GatewayErrorCode::NotConnected) || code == static_cast<int>(GatewayErrorCode::Cancelled)));
    REQUIRE(elapsed_ms.load() < 3000); // nowhere near the 10 s timeout
}
