#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/GatewayDevice.hpp"
#include "slic3r/Utils/GatewayService.hpp"

#include "libslic3r/PresetBundle.hpp"

#include <nlohmann/json.hpp>

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
    // Serialized body of GET /api/cache/all. Empty means the device endpoint answers 404.
    std::string device_response;

    HttpResponse get(const std::string& url) override
    {
        if (url.find("/api/health") != std::string::npos) {
            return {200,
                    R"({"status":"ok","cli_version":"1.0","components":{"ipc_server":"ok","web_server":"ok"},)"
                    R"("server_url":{"base_url":"http://127.0.0.1:8080/","home_page":"/index","device_control":""}})",
                    {}};
        }
        if (url.find("/api/cache/all") != std::string::npos) {
            if (device_response.empty())
                return {404, {}, "not found"};
            return {200, device_response, {}};
        }
        return {404, {}, "not found"};
    }

    HttpResponse post_json(const std::string& url, const std::string& body) override { return {404, {}, "not found"}; }
};

struct DeviceFakeWebSocket final : public WebSocketTransport
{
    Listener listener;

    void set_listener(Listener value) override { listener = std::move(value); }

    void connect(std::uint16_t port, const std::string& path) override { listener.opened(); }

    bool send(const std::string& message) override { return true; }

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
    std::shared_ptr<DeviceFakeHttp>           http;
    std::shared_ptr<DeviceFakeWebSocket>      websocket;
    std::shared_ptr<GatewayService>           service;

    ServiceFixture()
    {
        process = std::make_shared<ConnectionProcessManager>(ConnectionProcessManager::Config{boost::filesystem::path{
                                                                 "snapmaker_connection.exe"}},
                                                             [](const std::vector<std::string>&) {
                                                                 ConnectionProcessManager::ProcessRunResult result;
                                                                 result.stdout_data = "PORT:8888\r\n\r\n";
                                                                 return result;
                                                             });
        GatewayService::Dependencies dependencies;
        dependencies.process_manager = process;
        http                         = std::make_shared<DeviceFakeHttp>();
        dependencies.http            = http;
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

// Mirrors the GET /api/cache/all 200 contract from the snapmaker_connection API doc.
const nlohmann::json valid_device_snapshot{{"ok", true},
                                           {"data",
                                            {{"schema", 1},
                                             {"sn", "SN1"},
                                             {"current", true},
                                             {"identity",
                                              {{"sn", "SN1"},
                                               {"name", "qiepian-6"},
                                               {"device_name", "qiepian-6"},
                                               {"connected", true},
                                               {"productName", "Snapmaker U1"},
                                               {"deviceModel", "U1"}}},
                                             {"info",
                                              {{"product",
                                                {{"machine_type", "Snapmaker U1"},
                                                 {"serial_number", "SN1"},
                                                 {"device_name", "Data Registration"},
                                                 {"nozzle_diameter", {0.4, 0.4, 0.25, "0.8", 0.6}}}}}},
                                             {"objects",
                                              {{"extruder", {{"nozzle_diameter", 0.4}, {"nozzle_volume_type", "high_flow"}}},
                                               {"extruder1", {{"nozzle_diameter", 0.4}, {"nozzle_volume_type", "standard"}}},
                                               {"extruder2", {{"nozzle_diameter", 0.25}, {"nozzle_volume_type", "tpu_high_flow"}}},
                                               {"extruder3", {{"nozzle_diameter", "0.8"}, {"nozzle_volume_type", "high_flow"}}},
                                               {"extruder4", {{"nozzle_diameter", 0.6}, {"nozzle_volume_type", "standard"}}}}}}}};

} // namespace

namespace {

nlohmann::json machine_objects()
{
    return nlohmann::json{{"extruder", nlohmann::json{{"nozzle_diameter", 0.4}, {"nozzle_volume_type", "standard"}}},
                          {"extruder1", nlohmann::json{{"nozzle_diameter", "0.6"}, {"nozzle_volume_type", "high_flow"}}},
                          {"print_task_config",
                           nlohmann::json{{"filament_vendor", nlohmann::json::array({"Snapmaker", "Polymaker"})},
                                          {"filament_type", nlohmann::json::array({"PLA", "PETG"})},
                                          {"filament_sub_type", nlohmann::json::array({"Matte", "NONE"})},
                                          {"filament_official", nlohmann::json::array({true, false})},
                                          {"filament_exist", nlohmann::json::array({true, true})},
                                          {"filament_color", nlohmann::json::array({4281179737, 4294967295})},
                                          {"filament_color_rgba", nlohmann::json::array({"2D9E59FF", "FFFFFFFF"})},
                                          {"extruder_map_table", nlohmann::json::array({0, 1})},
                                          {"filament_color_multi",
                                           nlohmann::json::array(
                                               {nlohmann::json{{"mode", 2}, {"colors", nlohmann::json::array({"2D9E59", "FFFFFF"})}},
                                                nlohmann::json{{"mode", 0}, {"colors", nlohmann::json::array({"FFFFFF"})}}})}}}};
}

} // namespace

TEST_CASE("GatewayDevice query_machine_info parses the device snapshot", "[gateway][device]")
{
    ServiceFixture fixture;
    REQUIRE(fixture.service->is_connected());
    fixture.http->device_response = valid_device_snapshot.dump();

    std::string              model;
    std::vector<std::string> nozzles;
    std::vector<std::string> flows;
    std::string              name;
    REQUIRE(GatewayDevice::query_machine_info(fixture.service, model, nozzles, flows, name));
    REQUIRE(model == "Snapmaker U1");
    REQUIRE(name == "qiepian-6");
    // 0.25 is not a supported preset label and is dropped by design; "0.8" as a string is accepted.
    REQUIRE(nozzles == std::vector<std::string>{"0.4", "0.4", "0.8", "0.6"});
    // Flow types follow the sorted extruder objects; the 0.25 extruder does not participate.
    REQUIRE(flows == std::vector<std::string>{"high_flow", "standard", "high_flow", "standard"});
}

TEST_CASE("GatewayDevice parse_machine_slots parses the complete object query", "[gateway][device][machine-slots]")
{
    std::vector<ConnectMachineInfo> slots;
    REQUIRE(GatewayDevice::parse_machine_slots(nlohmann::json{{"objects", machine_objects()}}, slots));
    REQUIRE(slots.size() == 2);
    REQUIRE(slots[0].index == 0);
    REQUIRE(slots[0].filament_info == "Snapmaker PLA Matte");
    REQUIRE(slots[0].filament_type == "PLA");
    REQUIRE(slots[0].nozzle_info == "0.4");
    REQUIRE(slots[0].nozzle_volume_type == "standard");
    REQUIRE(slots[0].color_info == "#2D9E59");
    REQUIRE(slots[0].multiColors == std::vector<std::string>{"#2D9E59", "#FFFFFF"});
    REQUIRE(slots[1].filament_info == "Polymaker PETG");
    REQUIRE(slots[1].nozzle_info == "0.6");
    REQUIRE(slots[1].nozzle_volume_type == "high_flow");
}

TEST_CASE("GatewayDevice parse_machine_slots supports legacy and missing optional fields", "[gateway][device][machine-slots]")
{
    nlohmann::json objects = machine_objects();
    objects["print_task_config"].erase("filament_exist");
    objects["print_task_config"].erase("filament_color_rgba");
    objects["print_task_config"].erase("filament_color_multi");
    objects["extruder1"].erase("nozzle_volume_type");

    std::vector<ConnectMachineInfo> slots;
    REQUIRE(GatewayDevice::parse_machine_slots(objects, slots));
    REQUIRE(slots.size() == 2);
    REQUIRE(slots[0].color_info == "#2D9E59");
    REQUIRE(slots[0].multiColors == std::vector<std::string>{"#2D9E59"});
    REQUIRE(slots[1].filament_info == "Polymaker PETG");
    REQUIRE(slots[1].nozzle_info == "0.6");
    REQUIRE(slots[1].nozzle_volume_type == "standard");
}

TEST_CASE("GatewayDevice parse_machine_slots renders empty slots as NONE", "[gateway][device][machine-slots]")
{
    nlohmann::json objects                            = machine_objects();
    objects["print_task_config"]["filament_sub_type"] = nlohmann::json::array({"Support", "NONE"});
    objects["print_task_config"]["filament_exist"]    = nlohmann::json::array({false, true});

    std::vector<ConnectMachineInfo> slots;
    REQUIRE(GatewayDevice::parse_machine_slots(objects, slots));
    REQUIRE(slots.size() == 2);
    REQUIRE(slots[0].filament_info == "NONE");
    REQUIRE(slots[0].filament_type == "NONE");
    REQUIRE(slots[1].filament_info == "Polymaker PETG");
}

TEST_CASE("GatewayDevice parse_machine_slots overrides only present config nozzles", "[gateway][device][machine-slots]")
{
    nlohmann::json objects                           = machine_objects();
    objects["print_task_config"]["nozzle_diameters"] = nlohmann::json::array({0.8});

    std::vector<ConnectMachineInfo> slots;
    REQUIRE(GatewayDevice::parse_machine_slots(objects, slots));
    REQUIRE(slots.size() == 2);
    REQUIRE(slots[0].nozzle_info == "0.8");
    REQUIRE(slots[1].nozzle_info == "0.6");
}

TEST_CASE("GatewayDevice parse_machine_slots rejects malformed payloads", "[gateway][device][machine-slots]")
{
    std::vector<ConnectMachineInfo> slots;
    REQUIRE_FALSE(GatewayDevice::parse_machine_slots(nlohmann::json::object(), slots));
    REQUIRE_FALSE(GatewayDevice::parse_machine_slots(nlohmann::json{{"objects", nlohmann::json::object()}}, slots));
    REQUIRE_FALSE(
        GatewayDevice::parse_machine_slots(nlohmann::json{{"objects", nlohmann::json{{"print_task_config", nlohmann::json::object()}}}},
                                           slots));

    nlohmann::json missing_nozzle = machine_objects();
    missing_nozzle.erase("extruder");
    missing_nozzle.erase("extruder1");
    REQUIRE_FALSE(GatewayDevice::parse_machine_slots(missing_nozzle, slots));
    REQUIRE(slots.empty());
}

TEST_CASE("GatewayDevice machine_slots_equal includes nozzle flow", "[gateway][device][machine-slots]")
{
    std::vector<ConnectMachineInfo> left;
    std::vector<ConnectMachineInfo> right;
    REQUIRE(GatewayDevice::parse_machine_slots(machine_objects(), left));
    right = left;
    REQUIRE(GatewayDevice::machine_slots_equal(left, right));
    right[1].nozzle_volume_type = "standard";
    REQUIRE_FALSE(GatewayDevice::machine_slots_equal(left, right));
}

TEST_CASE("GatewayDevice delta_affects_machine_slots ignores unrelated objects", "[gateway][device][machine-slots]")
{
    REQUIRE(GatewayDevice::delta_affects_machine_slots(nlohmann::json{{"print_task_config", nlohmann::json::object()}}));
    REQUIRE(GatewayDevice::delta_affects_machine_slots(nlohmann::json{{"extruder", nlohmann::json{{"temperature", 25}}}}));
    REQUIRE(GatewayDevice::delta_affects_machine_slots(nlohmann::json{{"extruder1", nlohmann::json{{"nozzle_diameter", 0.2}}}}));
    REQUIRE(
        GatewayDevice::delta_affects_machine_slots(nlohmann::json{{"objects", nlohmann::json{{"extruder2", nlohmann::json::object()}}}}));
    REQUIRE_FALSE(GatewayDevice::delta_affects_machine_slots(nlohmann::json{{"print_stats", nlohmann::json::object()}}));
    REQUIRE_FALSE(GatewayDevice::delta_affects_machine_slots(nlohmann::json{{"extruder_sensor", nlohmann::json::object()}}));
    REQUIRE_FALSE(GatewayDevice::delta_affects_machine_slots(nlohmann::json::array()));
}

TEST_CASE("GatewayDevice query_machine_info falls back to identity fields", "[gateway][device]")
{
    ServiceFixture fixture;
    REQUIRE(fixture.service->is_connected());
    nlohmann::json snapshot                             = valid_device_snapshot;
    snapshot["data"]["info"]["product"]["machine_type"] = nullptr;
    snapshot["data"]["info"]["product"]["device_name"]  = "";
    fixture.http->device_response                       = snapshot.dump();

    std::string              model;
    std::vector<std::string> nozzles;
    std::vector<std::string> flows;
    std::string              name;
    REQUIRE(GatewayDevice::query_machine_info(fixture.service, model, nozzles, flows, name));
    REQUIRE(model == "Snapmaker U1"); // from identity.productName
    REQUIRE(name == "qiepian-6");     // from identity.device_name
    REQUIRE(flows == std::vector<std::string>{"high_flow", "standard", "high_flow", "standard"});
}

TEST_CASE("GatewayDevice query_machine_info falls back to the product device name", "[gateway][device]")
{
    ServiceFixture fixture;
    REQUIRE(fixture.service->is_connected());
    nlohmann::json snapshot = valid_device_snapshot;
    snapshot["data"]["identity"].erase("name");
    snapshot["data"]["identity"].erase("device_name");
    fixture.http->device_response = snapshot.dump();

    std::string              model;
    std::vector<std::string> nozzles;
    std::vector<std::string> flows;
    std::string              name;
    REQUIRE(GatewayDevice::query_machine_info(fixture.service, model, nozzles, flows, name));
    REQUIRE(name == "Data Registration");
}

TEST_CASE("GatewayDevice query_machine_info rejects malformed payloads", "[gateway][device]")
{
    ServiceFixture fixture;
    REQUIRE(fixture.service->is_connected());

    SECTION("empty machine_type")
    {
        nlohmann::json snapshot                             = valid_device_snapshot;
        snapshot["data"]["info"]["product"]["machine_type"] = "";
        snapshot["data"]["identity"].erase("productName");
        fixture.http->device_response = snapshot.dump();
    }
    SECTION("missing product")
    {
        nlohmann::json snapshot = valid_device_snapshot;
        snapshot["data"]["info"].erase("product");
        snapshot["data"]["identity"].erase("productName");
        fixture.http->device_response = snapshot.dump();
    }
    SECTION("missing machine_type key")
    {
        nlohmann::json snapshot = valid_device_snapshot;
        snapshot["data"]["info"]["product"].erase("machine_type");
        snapshot["data"]["identity"].erase("productName");
        fixture.http->device_response = snapshot.dump();
    }

    std::string              model = "untouched";
    std::vector<std::string> nozzles{"untouched"};
    std::vector<std::string> flows{"untouched"};
    std::string              name = "untouched";
    REQUIRE_FALSE(GatewayDevice::query_machine_info(fixture.service, model, nozzles, flows, name));
    // Outputs are not modified on failure.
    REQUIRE(model == "untouched");
    REQUIRE(nozzles == std::vector<std::string>{"untouched"});
    REQUIRE(flows == std::vector<std::string>{"untouched"});
    REQUIRE(name == "untouched");
}

TEST_CASE("GatewayDevice query_machine_info reports http errors", "[gateway][device]")
{
    ServiceFixture fixture;
    REQUIRE(fixture.service->is_connected());
    // Default DeviceFakeHttp answers 404 on /api/cache/all (no device snapshot available).

    std::string              model;
    std::vector<std::string> nozzles;
    std::vector<std::string> flows;
    std::string              name;
    REQUIRE_FALSE(GatewayDevice::query_machine_info(fixture.service, model, nozzles, flows, name));
}

TEST_CASE("GatewayDevice tolerates a null gateway", "[gateway][device]")
{
    std::string              model;
    std::vector<std::string> nozzles;
    std::vector<std::string> flows;
    std::string              name;
    REQUIRE_FALSE(GatewayDevice::query_machine_info(nullptr, model, nozzles, flows, name));
}

TEST_CASE("GatewayDevice query_machine_info nozzle_volume_type edge cases", "[gateway][device]")
{
    ServiceFixture fixture;
    REQUIRE(fixture.service->is_connected());

    std::string              model;
    std::vector<std::string> nozzles;
    std::vector<std::string> flows;
    std::string              name;

    SECTION("unknown flow strings normalize to standard")
    {
        nlohmann::json snapshot                                       = valid_device_snapshot;
        snapshot["data"]["objects"]["extruder"]["nozzle_volume_type"] = "ultra_flow";
        fixture.http->device_response                                 = snapshot.dump();

        REQUIRE(GatewayDevice::query_machine_info(fixture.service, model, nozzles, flows, name));
        REQUIRE(flows == std::vector<std::string>{"standard", "standard", "high_flow", "standard"});
    }
    SECTION("a missing flow type leaves the vector empty for the caller fallback")
    {
        nlohmann::json snapshot = valid_device_snapshot;
        snapshot["data"]["objects"]["extruder1"].erase("nozzle_volume_type");
        fixture.http->device_response = snapshot.dump();

        REQUIRE(GatewayDevice::query_machine_info(fixture.service, model, nozzles, flows, name));
        REQUIRE(flows.empty());
    }
    SECTION("no extruder objects fall back to standard per nozzle")
    {
        nlohmann::json snapshot       = valid_device_snapshot;
        snapshot["data"]["objects"]   = nlohmann::json::object();
        fixture.http->device_response = snapshot.dump();

        REQUIRE(GatewayDevice::query_machine_info(fixture.service, model, nozzles, flows, name));
        REQUIRE(flows == std::vector<std::string>{"standard", "standard", "standard", "standard"});
    }
    SECTION("a partial extruder report leaves flow types empty")
    {
        nlohmann::json snapshot = valid_device_snapshot;
        snapshot["data"]["objects"].erase("extruder4");
        fixture.http->device_response = snapshot.dump();

        REQUIRE(GatewayDevice::query_machine_info(fixture.service, model, nozzles, flows, name));
        REQUIRE(nozzles == std::vector<std::string>{"0.4", "0.4", "0.8", "0.6"});
        REQUIRE(flows.empty());
    }
}
