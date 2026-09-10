#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/GatewayProtocol.hpp"

#include <nlohmann/json.hpp>

#include <vector>

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

TEST_CASE("parse_active_device accepts direct and nested payloads", "[gateway][protocol]")
{
    const auto parsed = parse_active_device(json{{"sn", "U1-001"},
                                                 {"connected", true},
                                                 {"machine_type", "Snapmaker U1"},
                                                 {"device_name", "Studio U1"},
                                                 {"preset_name", "Snapmaker U1 0.4"},
                                                 {"nozzle_diameters", json::array({"0.4", "0.4"})}});
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->valid);
    REQUIRE(parsed->connected);
    REQUIRE(parsed->serial_number == "U1-001");
    REQUIRE(parsed->machine_type == "Snapmaker U1");
    REQUIRE(parsed->device_name == "Studio U1");
    REQUIRE(parsed->preset_name == "Snapmaker U1 0.4");
    REQUIRE(parsed->nozzle_diameters == std::vector<std::string>{"0.4", "0.4"});

    const auto nested = parse_active_device(json{{"device", {{"device_sn", "U1-002"}, {"connected", false}}}});
    REQUIRE(nested.has_value());
    REQUIRE(nested->valid);
    REQUIRE_FALSE(nested->connected);
    REQUIRE(nested->serial_number == "U1-002");

    REQUIRE_FALSE(parse_active_device(json{{"connected", true}}).has_value());
    REQUIRE_FALSE(parse_active_device(json{{"sn", "U1-001"}, {"connected", "online"}}).has_value());
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

TEST_CASE("device object notifications build machine snapshots", "[gateway][protocol]")
{
    const nlohmann::json params{{"extruder", {{"nozzle_diameter", 0.4}}},
                                {"extruder1", {{"nozzle_diameter", "0.6"}}},
                                {"print_task_config",
                                 {{"filament_vendor", json::array({"Snapmaker", "Polymaker"})},
                                  {"filament_type", json::array({"PLA", "PETG"})},
                                  {"filament_sub_type", json::array({"Matte", "NONE"})},
                                  {"filament_official", json::array({true, false})},
                                  {"filament_exist", json::array({true, false})},
                                  {"filament_color", json::array({4281179737, 4294967295})},
                                  {"filament_color_rgba", json::array({"2D9E59FF", "FFFFFFFF"})},
                                  {"extruder_map_table", json::array({0, 1})},
                                  {"filament_color_multi", json::array({json{{"mode", 2}, {"colors", json::array({"2D9E59", "FFFFFF"})}},
                                                                        json{{"mode", 0}, {"colors", json::array({"FFFFFF"})}}})}}}};

    const auto snapshot = build_machine_snapshot_from_device_objects(params, "U1-001");
    REQUIRE(snapshot.has_value());
    REQUIRE((*snapshot)["sn"] == "U1-001");
    REQUIRE((*snapshot)["nozzle_diameters"] == json::array({"0.4", "0.6"}));
    REQUIRE((*snapshot)["filaments"].size() == 2);
    REQUIRE((*snapshot)["filaments"][0]["vendor"] == "Snapmaker");
    REQUIRE((*snapshot)["filaments"][0]["type"] == "PLA");
    REQUIRE((*snapshot)["filaments"][0]["sub_type"] == "Matte");
    REQUIRE((*snapshot)["filaments"][0]["color"] == "2D9E59FF");
    REQUIRE((*snapshot)["filaments"][0]["multi_colors"] == json::array({"2D9E59", "FFFFFF"}));
    REQUIRE((*snapshot)["filaments"][0]["color_mode"] == 2);
    REQUIRE((*snapshot)["filaments"][1]["vendor"] == "NONE");
    REQUIRE((*snapshot)["filaments"][1]["type"] == "NONE");

    nlohmann::json legacy_params = params;
    legacy_params["print_task_config"].erase("filament_exist");
    legacy_params["print_task_config"].erase("filament_color_rgba");
    const auto legacy_snapshot = build_machine_snapshot_from_device_objects(legacy_params, "U1-001");
    REQUIRE(legacy_snapshot.has_value());
    REQUIRE((*legacy_snapshot)["filaments"][0]["vendor"] == "Snapmaker");
    REQUIRE((*legacy_snapshot)["filaments"][0]["type"] == "PLA");
    REQUIRE((*legacy_snapshot)["filaments"][0]["color"] == "2D9E59FF");
    REQUIRE((*legacy_snapshot)["filaments"][1]["vendor"] == "Polymaker");
    REQUIRE((*legacy_snapshot)["filaments"][1]["type"] == "PETG");

    REQUIRE_FALSE(build_machine_snapshot_from_device_objects(json{{"print_task_config", json::object()}}, "U1-001").has_value());
    REQUIRE_FALSE(build_machine_snapshot_from_device_objects(params, "").has_value());
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
