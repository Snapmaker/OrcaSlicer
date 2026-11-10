#include <catch2/catch_test_macros.hpp>

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
        "device_connected": true,
        "sn": "U1-001",
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
    REQUIRE(health.device_connected);
    REQUIRE(health.device_sn == "U1-001");
    REQUIRE(health.has_device_state);
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

TEST_CASE("parse_health accepts an explicit disconnected device without sn", "[gateway][protocol]")
{
    HealthInfo health;
    const auto body = json{{"status", "ok"},
                           {"components", json{{"ipc_server", "ok"}, {"web_server", "ok"}}},
                           {"device_connected", false},
                           {"server_url", {{"base_url", "http://127.0.0.1:8080/"}, {"home_page", "/index.html"}}}}
                          .dump();

    REQUIRE(!parse_health(body, health));
    REQUIRE(health.has_device_state);
    REQUIRE_FALSE(health.device_connected);
    REQUIRE(health.device_sn.empty());

    const auto invalid = json{{"status", "ok"},
                              {"components", json{{"ipc_server", "ok"}, {"web_server", "ok"}}},
                              {"device_connected", "false"},
                              {"server_url", {{"base_url", "http://127.0.0.1:8080/"}, {"home_page", "/index.html"}}}}
                             .dump();
    const auto error   = parse_health(invalid, health);
    REQUIRE(error);
    REQUIRE(error.code == GatewayErrorCode::InvalidResponse);
}

TEST_CASE("parse_health ignores device state when connection state is absent", "[gateway][protocol]")
{
    HealthInfo health;
    const auto body = json{{"status", "ok"},
                           {"components", json{{"ipc_server", "ok"}, {"web_server", "ok"}}},
                           {"sn", "U1-001"},
                           {"server_url", {{"base_url", "http://127.0.0.1:8080/"}, {"home_page", "/index.html"}}}}
                          .dump();

    REQUIRE(!parse_health(body, health));
    REQUIRE_FALSE(health.has_device_state);
    REQUIRE(health.device_sn.empty());
}

TEST_CASE("parse_device_sn uses the active device contract", "[gateway][protocol]")
{
    for (const char* key : {"sn", "device_sn", "serial_number"}) {
        REQUIRE(parse_device_sn(json{{key, "U1-001"}}) == "U1-001");
        REQUIRE(parse_device_sn(json{{"device", {{key, "U1-002"}}}}) == "U1-002");
    }

    REQUIRE(parse_device_sn(json{{"device", json{{"connected", true}}}}).empty());
    REQUIRE(parse_device_sn(json{{"sn", 1}}).empty());
    REQUIRE(parse_device_sn(json::array()).empty());
}

TEST_CASE("current device results use the compact device contract", "[gateway][protocol]")
{
    const auto connected = parse_current_device(json{{"sn", "U1-001"}, {"connected", true}});
    REQUIRE(connected.has_value());
    REQUIRE(connected->valid);
    REQUIRE(connected->serial_number == "U1-001");
    REQUIRE(connected->connected);

    const auto disconnected = parse_current_device(json{{"sn", ""}, {"connected", false}});
    REQUIRE(disconnected.has_value());
    REQUIRE(disconnected->valid);
    REQUIRE(disconnected->serial_number.empty());
    REQUIRE_FALSE(disconnected->connected);

    const auto missing_connected = parse_current_device(json{{"sn", "U1-001"}});
    REQUIRE(missing_connected.has_value());
    REQUIRE_FALSE(missing_connected->connected);
    REQUIRE_FALSE(parse_current_device(json{{"sn", ""}, {"connected", true}}).has_value());
    REQUIRE_FALSE(parse_current_device(json::array()).has_value());
}

TEST_CASE("device object query results expose the complete object map", "[gateway][protocol]")
{
    const nlohmann::json objects{{"extruder", json{{"nozzle_diameter", 0.4}}},
                                 {"print_task_config",
                                  json{{"filament_vendor", json::array({"Snapmaker"})}, {"filament_type", json::array({"PLA"})}}}};

    REQUIRE(parse_device_object_query_result(json{{"objects", objects}}) == objects);
    REQUIRE(parse_device_object_query_result(objects) == objects);
    REQUIRE_FALSE(parse_device_object_query_result(json{{"objects", json::array()}}).has_value());
    REQUIRE_FALSE(parse_device_object_query_result(json::object()).has_value());
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
    REQUIRE(notification_frame.params == json{{"user", "u"}});

    const RpcFrame notification_without_params = classify_jsonrpc_message(json{{"jsonrpc", "2.0"}, {"method", "notify.account.changed"}});
    REQUIRE(notification_without_params.type == RpcFrameType::Notification);
    REQUIRE(notification_without_params.params == json::object());

    const RpcFrame array_params_frame = classify_jsonrpc_message(
        json{{"jsonrpc", "2.0"}, {"method", "notify.test"}, {"params", json::array({"A1"})}});
    REQUIRE(array_params_frame.type == RpcFrameType::Notification);
    REQUIRE(array_params_frame.params == json::array({"A1"}));

    const RpcFrame malformed_params_frame = classify_jsonrpc_message(json{{"jsonrpc", "2.0"}, {"method", "notify.test"}, {"params", 7}});
    REQUIRE(malformed_params_frame.type == RpcFrameType::Unknown);
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

TEST_CASE("parse_device_list reads devices and the current device object", "[gateway][protocol]")
{
    const json result = json::parse(R"({
        "devices": [
            {"sn": "U1-001", "name": "U1", "ip": "172.16.0.161", "type": "lan", "connected": false, "online": true},
            {"sn": "U1-002", "ip": "", "type": "WAN", "connected": true},
            {"name": "no serial"},
            "not an object"
        ],
        "currentDevice": {"sn": "U1-001", "connected": false},
        "discovering": false
    })");

    const auto list = parse_device_list(result);
    REQUIRE(list.has_value());
    REQUIRE(list->devices.size() == 2);
    REQUIRE(list->devices[0].serial_number == "U1-001");
    REQUIRE(list->devices[0].ip == "172.16.0.161");
    REQUIRE(list->devices[0].is_lan);
    REQUIRE_FALSE(list->devices[0].connected);
    REQUIRE_FALSE(list->devices[1].is_lan);
    REQUIRE(list->devices[1].connected);
    REQUIRE(list->current_serial_number == "U1-001");
}

TEST_CASE("parse_device_list accepts the current device as a serial string", "[gateway][protocol]")
{
    REQUIRE(parse_device_list(json{{"devices", json::array()}, {"currentDevice", "U1-001"}})->current_serial_number == "U1-001");
    REQUIRE(parse_device_list(json{{"currentDeviceSn", "U1-002"}})->current_serial_number == "U1-002");
    REQUIRE(parse_device_list(json::object())->devices.empty());
}

TEST_CASE("parse_device_list rejects malformed results", "[gateway][protocol]")
{
    REQUIRE_FALSE(parse_device_list(json::array()).has_value());
    REQUIRE_FALSE(parse_device_list(json{{"devices", "U1-001"}}).has_value());
}

TEST_CASE("select_auto_connect_device prefers the CLI current device", "[gateway][protocol]")
{
    DeviceList list;
    list.devices               = {{"U1-001", "172.16.0.161", true, false}, {"U1-002", "172.16.0.162", true, false}};
    list.current_serial_number = "U1-002";

    const auto device = select_auto_connect_device(list, "U1-001");
    REQUIRE(device.has_value());
    REQUIRE(device->serial_number == "U1-002");
}

TEST_CASE("select_auto_connect_device falls back to the remembered device", "[gateway][protocol]")
{
    DeviceList list;
    list.devices               = {{"U1-001", "172.16.0.161", true, false}};
    list.current_serial_number = "GONE";

    const auto device = select_auto_connect_device(list, "U1-001");
    REQUIRE(device.has_value());
    REQUIRE(device->serial_number == "U1-001");
}

TEST_CASE("select_auto_connect_device skips unusable candidates", "[gateway][protocol]")
{
    DeviceList list;
    list.devices = {{"WAN-1", "10.0.0.2", false, false}, {"NO-IP", "", true, false}};

    REQUIRE_FALSE(select_auto_connect_device(list, "WAN-1").has_value());
    REQUIRE_FALSE(select_auto_connect_device(list, "NO-IP").has_value());
    REQUIRE_FALSE(select_auto_connect_device(list, "").has_value());
    REQUIRE_FALSE(select_auto_connect_device(list, "UNKNOWN").has_value());
}

TEST_CASE("select_auto_connect_device does nothing while a device is connected", "[gateway][protocol]")
{
    DeviceList list;
    list.devices               = {{"U1-001", "172.16.0.161", true, false}, {"U1-002", "172.16.0.162", true, true}};
    list.current_serial_number = "U1-001";

    REQUIRE_FALSE(select_auto_connect_device(list, "U1-001").has_value());
}

TEST_CASE("build_device_connect_params matches the Device page LAN request", "[gateway][protocol]")
{
    const json params = build_device_connect_params({"U1-001", "172.16.0.161", true, false});
    REQUIRE(params == json{{"type", "LAN"}, {"sn", "U1-001"}, {"ip", "172.16.0.161"}});

    const json without_ip = build_device_connect_params({"U1-002", "", true, false});
    REQUIRE(without_ip == json{{"type", "LAN"}, {"sn", "U1-002"}});
}
