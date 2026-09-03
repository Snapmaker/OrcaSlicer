#include <catch2/catch.hpp>

#include "slic3r/Utils/GatewayDevice.hpp"
#include "slic3r/Utils/GatewayService.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace Slic3r::Gateway;

namespace {

struct DeviceFakeHttp final : public HttpTransport
{
    HttpResponse get(const std::string& url) override
    {
        if (url.find("/health") != std::string::npos) {
            return {200,
                    R"({"status":"ok","cli_version":"1.0","components":{"ipc_server":"ok","web_server":"ok"},)"
                    R"("server_url":{"base_url":"http://127.0.0.1:8080/","home_page":"/index","device_control":""}})",
                    {}};
        }
        return {404, {}, "not found"};
    }

    HttpResponse post_json(const std::string& url, const std::string& body) override { return {404, {}, "not found"}; }
};

// Answers machine.system_info with a canned payload (or a -32000 error frame).
struct DeviceFakeWebSocket final : public WebSocketTransport
{
    Listener          listener;
    nlohmann::json    system_info_payload{nlohmann::json::object()};
    bool              answer_with_error{false};
    std::atomic<bool> machine_info_seen{false};

    void set_listener(Listener value) override { listener = std::move(value); }

    void connect(std::uint16_t port, const std::string& path) override { listener.opened(); }

    bool send(const std::string& message) override
    {
        const nlohmann::json request = nlohmann::json::parse(message);
        if (request.value("method", "") != "machine.system_info")
            return true;
        machine_info_seen = true;
        if (answer_with_error) {
            listener.message(nlohmann::json{{"jsonrpc", "2.0"}, {"id", request["id"]}, {"error", {{"code", -32000}, {"message", "not_connected"}}}}.dump());
        } else {
            listener.message(nlohmann::json{{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", {{"system_info", system_info_payload}}}}.dump());
        }
        return true;
    }

    void close() override
    {
        if (listener.closed)
            listener.closed("closed");
    }
};

bool wait_for_state(const GatewayService& service, ConnectionState expected)
{
    for (int i = 0; i < 2000; ++i) {
        if (service.state() == expected)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return service.state() == expected;
}

struct ServiceFixture
{
    std::shared_ptr<ConnectionProcessManager> process;
    std::shared_ptr<DeviceFakeWebSocket>      websocket;
    std::shared_ptr<GatewayService>           service;

    ServiceFixture()
    {
        process = std::make_shared<ConnectionProcessManager>(ConnectionProcessManager::Config{boost::filesystem::path{"snapmaker_connection.exe"}},
                                                             [](const std::vector<std::string>&) {
                                                                 ConnectionProcessManager::ProcessRunResult result;
                                                                 result.stdout_data = "PORT:8888\r\n\r\n";
                                                                 return result;
                                                             });
        GatewayService::Dependencies dependencies;
        dependencies.process_manager = process;
        dependencies.http            = std::make_shared<DeviceFakeHttp>();
        websocket                    = std::make_shared<DeviceFakeWebSocket>();
        dependencies.websocket       = websocket;
        dependencies.dispatcher      = [](std::function<void()> task) { task(); };
        service                      = std::make_shared<GatewayService>(GatewayService::Config{}, std::move(dependencies));
        if (!service->start("zh-CN"))
            return;
        wait_for_state(*service, ConnectionState::Connected);
    }

    ~ServiceFixture() { service->stop(); }
};

const nlohmann::json valid_system_info{
    {"system_info", {{"product_info", {{"machine_type", "Snapmaker U1"},
                                       {"nozzle_diameter", {0.4, 0.4, 0.25, "0.8", 0.6}},
                                       {"device_name", "qiepian-6"},
                                       {"serial_number", "SN1"}}}}}};

} // namespace

TEST_CASE("GatewayDevice query_machine_info parses product_info", "[gateway][device]")
{
    ServiceFixture fixture;
    REQUIRE(fixture.service->is_connected());
    fixture.websocket->system_info_payload = valid_system_info["system_info"];

    std::string              model;
    std::vector<std::string> nozzles;
    std::string              name;
    REQUIRE(GatewayDevice::query_machine_info(fixture.service, model, nozzles, name));
    REQUIRE(model == "Snapmaker U1");
    REQUIRE(name == "qiepian-6");
    // 0.25 is not a supported preset label and is dropped by design; "0.8" as a string is accepted.
    REQUIRE(nozzles == std::vector<std::string>{"0.4", "0.4", "0.8", "0.6"});
    REQUIRE(GatewayDevice::is_device_connected(fixture.service));
}

TEST_CASE("GatewayDevice query_machine_info rejects malformed payloads", "[gateway][device]")
{
    ServiceFixture fixture;
    REQUIRE(fixture.service->is_connected());

    SECTION("empty machine_type")
    {
        fixture.websocket->system_info_payload = {{"product_info", {{"machine_type", ""}, {"device_name", "x"}}}};
    }
    SECTION("missing product_info")
    {
        fixture.websocket->system_info_payload = nlohmann::json::object();
    }
    SECTION("missing machine_type key")
    {
        fixture.websocket->system_info_payload = {{"product_info", {{"device_name", "x"}}}};
    }

    std::string              model = "untouched";
    std::vector<std::string> nozzles{"untouched"};
    std::string              name  = "untouched";
    REQUIRE_FALSE(GatewayDevice::query_machine_info(fixture.service, model, nozzles, name));
    REQUIRE_FALSE(GatewayDevice::is_device_connected(fixture.service));
    // Outputs are not modified on failure.
    REQUIRE(model == "untouched");
    REQUIRE(nozzles == std::vector<std::string>{"untouched"});
    REQUIRE(name == "untouched");
}

TEST_CASE("GatewayDevice query_machine_info reports rpc not_connected", "[gateway][device]")
{
    ServiceFixture fixture;
    REQUIRE(fixture.service->is_connected());
    fixture.websocket->answer_with_error = true;

    std::string              model;
    std::vector<std::string> nozzles;
    std::string              name;
    REQUIRE_FALSE(GatewayDevice::query_machine_info(fixture.service, model, nozzles, name));
    REQUIRE_FALSE(GatewayDevice::is_device_connected(fixture.service));
}

TEST_CASE("GatewayDevice tolerates a null gateway", "[gateway][device]")
{
    std::string              model;
    std::vector<std::string> nozzles;
    std::string              name;
    REQUIRE_FALSE(GatewayDevice::query_machine_info(nullptr, model, nozzles, name));
    REQUIRE_FALSE(GatewayDevice::is_device_connected(nullptr));
}
