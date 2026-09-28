#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/GatewayMachineSlots.hpp"

#include <nlohmann/json.hpp>

using namespace Slic3r::Gateway;
using nlohmann::json;

namespace {

json machine_objects()
{
    return json{{"extruder", json{{"nozzle_diameter", 0.4}, {"nozzle_volume_type", "standard"}}},
                {"extruder1", json{{"nozzle_diameter", "0.6"}, {"nozzle_volume_type", "high_flow"}}},
                {"print_task_config",
                 json{{"filament_vendor", json::array({"Snapmaker", "Polymaker"})},
                      {"filament_type", json::array({"PLA", "PETG"})},
                      {"filament_sub_type", json::array({"Matte", "NONE"})},
                      {"filament_official", json::array({true, false})},
                      {"filament_exist", json::array({true, true})},
                      {"filament_color", json::array({4281179737, 4294967295})},
                      {"filament_color_rgba", json::array({"2D9E59FF", "FFFFFFFF"})},
                      {"extruder_map_table", json::array({0, 1})},
                      {"filament_color_multi", json::array({json{{"mode", 2}, {"colors", json::array({"2D9E59", "FFFFFF"})}},
                                                            json{{"mode", 0}, {"colors", json::array({"FFFFFF"})}}})}}}};
}

} // namespace

TEST_CASE("gateway machine slots parse the complete object query", "[gateway][machine-slots]")
{
    std::vector<::ConnectMachineInfo> slots;
    REQUIRE(parse_gateway_machine_slots(json{{"objects", machine_objects()}}, slots));
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

TEST_CASE("gateway machine slots support legacy and missing optional fields", "[gateway][machine-slots]")
{
    json objects = machine_objects();
    objects["print_task_config"].erase("filament_exist");
    objects["print_task_config"].erase("filament_color_rgba");
    objects["print_task_config"].erase("filament_color_multi");
    objects["extruder1"].erase("nozzle_volume_type");

    std::vector<::ConnectMachineInfo> slots;
    REQUIRE(parse_gateway_machine_slots(objects, slots));
    REQUIRE(slots.size() == 2);
    REQUIRE(slots[0].color_info == "#2D9E59");
    REQUIRE(slots[0].multiColors == std::vector<std::string>{"#2D9E59"});
    REQUIRE(slots[1].filament_info == "Polymaker PETG");
    REQUIRE(slots[1].nozzle_info == "0.6");
    REQUIRE(slots[1].nozzle_volume_type == "standard");
}

TEST_CASE("short config nozzle arrays only override present slots", "[gateway][machine-slots]")
{
    json objects                                     = machine_objects();
    objects["print_task_config"]["nozzle_diameters"] = json::array({0.8});

    std::vector<::ConnectMachineInfo> slots;
    REQUIRE(parse_gateway_machine_slots(objects, slots));
    REQUIRE(slots.size() == 2);
    REQUIRE(slots[0].nozzle_info == "0.8");
    REQUIRE(slots[1].nozzle_info == "0.6");
}

TEST_CASE("gateway machine slots reject malformed payloads", "[gateway][machine-slots]")
{
    std::vector<::ConnectMachineInfo> slots;
    REQUIRE_FALSE(parse_gateway_machine_slots(json::object(), slots));
    REQUIRE_FALSE(parse_gateway_machine_slots(json{{"objects", json::object()}}, slots));
    REQUIRE_FALSE(parse_gateway_machine_slots(json{{"objects", json{{"print_task_config", json::object()}}}}, slots));

    json missing_nozzle = machine_objects();
    missing_nozzle.erase("extruder");
    missing_nozzle.erase("extruder1");
    REQUIRE_FALSE(parse_gateway_machine_slots(missing_nozzle, slots));
    REQUIRE(slots.empty());
}

TEST_CASE("gateway machine slot equality includes nozzle flow", "[gateway][machine-slots]")
{
    std::vector<::ConnectMachineInfo> left;
    std::vector<::ConnectMachineInfo> right;
    REQUIRE(parse_gateway_machine_slots(machine_objects(), left));
    right = left;
    REQUIRE(gateway_machine_slots_equal(left, right));
    right[1].nozzle_volume_type = "standard";
    REQUIRE_FALSE(gateway_machine_slots_equal(left, right));
}

TEST_CASE("only slot-related device object deltas trigger a refresh", "[gateway][machine-slots]")
{
    REQUIRE(gateway_delta_affects_machine_slots(json{{"print_task_config", json::object()}}));
    REQUIRE(gateway_delta_affects_machine_slots(json{{"extruder", json{{"temperature", 25}}}}));
    REQUIRE(gateway_delta_affects_machine_slots(json{{"extruder1", json{{"nozzle_diameter", 0.2}}}}));
    REQUIRE(gateway_delta_affects_machine_slots(json{{"objects", json{{"extruder2", json::object()}}}}));
    REQUIRE_FALSE(gateway_delta_affects_machine_slots(json{{"print_stats", json::object()}}));
    REQUIRE_FALSE(gateway_delta_affects_machine_slots(json{{"extruder_sensor", json::object()}}));
    REQUIRE_FALSE(gateway_delta_affects_machine_slots(json::array()));
}
