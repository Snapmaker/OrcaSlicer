#include <catch2/catch.hpp>

#include "slic3r/Utils/GatewayProtocol.hpp"

#include <nlohmann/json.hpp>

using namespace Slic3r::Gateway;
using nlohmann::json;

TEST_CASE("parse_health accepts the documented health response", "[gateway][protocol]")
{
    const std::string body = R"({
        "status": "ok",
        "cli_version": "1.0",
        "components": {"ipc_server": "ok", "web_server": "ok"},
        "server_url": {
            "base_url": "http://127.0.0.1:8080/",
            "home_page": "/index.html?x=1",
            "pre_paint_page": "/pre-paint"
        }
    })";
    HealthInfo        health;
    REQUIRE(!parse_health(body, health));
    REQUIRE(health.cli_version == "1.0");
    REQUIRE(health.base_url == "http://127.0.0.1:8080/");
    REQUIRE(health.pages.at("home_page") == "/index.html?x=1");
    REQUIRE(health.pages.find("base_url") == health.pages.end());
}

TEST_CASE("parse_health rejects unhealthy components", "[gateway][protocol]")
{
    HealthInfo         health;
    const std::string  body  = R"({"status":"ok","components":{"ipc_server":"ok","web_server":"starting"}})";
    const GatewayError error = parse_health(body, health);
    REQUIRE(error);
    REQUIRE(error.code == GatewayErrorCode::HealthNotReady);
}

TEST_CASE("JSON-RPC frames are classified and requests are built", "[gateway][protocol]")
{
    const nlohmann::json request = build_jsonrpc_request(7, "action.device.watch", {{"sn", "A1"}});
    REQUIRE(request["jsonrpc"] == "2.0");
    REQUIRE(request["id"] == 7);
    REQUIRE(request["method"] == "action.device.watch");
    REQUIRE(request["params"] == json{{"sn", "A1"}});

    const RpcFrame result_frame = classify_jsonrpc_message(json{{"jsonrpc", "2.0"}, {"id", 7}, {"result", json{{"ok", true}}}});
    REQUIRE(result_frame.type == RpcFrameType::Result);
    REQUIRE(result_frame.id == 7);

    const RpcFrame notification_frame = classify_jsonrpc_message(
        json{{"jsonrpc", "2.0"}, {"method", "notify.account.changed"}, {"params", json{{"user", "u"}}}});
    REQUIRE(notification_frame.type == RpcFrameType::Notification);
    REQUIRE(notification_frame.method == "notify.account.changed");
}

TEST_CASE("ReconnectPolicy backs off and resets after a stable connection", "[gateway][protocol]")
{
    detail::ReconnectPolicy::Config config;
    config.initial_delay          = std::chrono::milliseconds{250};
    config.max_delay              = std::chrono::milliseconds{5000};
    config.stable_connection_time = std::chrono::milliseconds{30000};
    detail::ReconnectPolicy policy(config);

    policy.record_failure(1000);
    REQUIRE(policy.next_delay() == std::chrono::milliseconds{250});
    policy.record_failure(1100);
    REQUIRE(policy.next_delay() == std::chrono::milliseconds{500});

    policy.record_connected(2000);
    policy.record_failure(40000);
    REQUIRE(policy.next_delay() == std::chrono::milliseconds{250});
    REQUIRE(policy.attempts() == 1);
}
