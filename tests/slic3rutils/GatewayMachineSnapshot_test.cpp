#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/GatewayMachineSnapshot.hpp"
#include "slic3r/Utils/GatewayProtocol.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <nlohmann/json.hpp>

using namespace Slic3r::GUI;
using nlohmann::json;

namespace {

json machine_snapshot(const std::string& serial_number)
{
    return json{{"revision", 1},
                {"sn", serial_number},
                {"nozzle_diameters", json::array({"0.4"})},
                {"filaments", json::array({json{{"index", 0},
                                                {"extruder", 0},
                                                {"official", true},
                                                {"vendor", "Snapmaker"},
                                                {"type", "PLA"},
                                                {"sub_type", "NONE"},
                                                {"name", "Snapmaker PLA"},
                                                {"color", "#FFFFFF"},
                                                {"nozzle", "0.4"}}})}};
}

} // namespace

TEST_CASE("machine snapshots wait for the active device", "[gateway][machine-snapshot]")
{
    Slic3r::PresetBundle   preset_bundle;
    GatewayMachineSnapshot snapshot;
    snapshot.set_dependencies([&preset_bundle] { return &preset_bundle; }, [] {});

    SECTION("a snapshot arriving before the active device is replayed after connect")
    {
        snapshot.apply(machine_snapshot("U1-001"));
        REQUIRE(preset_bundle.machine_filaments.empty());

        snapshot.set_active_device("U1-001", true);
        REQUIRE(preset_bundle.machine_filaments.size() == 1);
        REQUIRE(preset_bundle.machine_filaments.begin()->second.first == "Snapmaker PLA");
        REQUIRE(preset_bundle.m_connect_machine_info_list.size() == 1);
    }

    SECTION("a deferred snapshot is discarded when the active device is disconnected")
    {
        snapshot.apply(machine_snapshot("U1-001"));
        snapshot.set_active_device("U1-001", false);
        REQUIRE(preset_bundle.machine_filaments.empty());

        snapshot.set_active_device("U1-001", true);
        REQUIRE(preset_bundle.machine_filaments.empty());
    }

    SECTION("a deferred snapshot for another device is not applied")
    {
        snapshot.apply(machine_snapshot("U1-001"));
        snapshot.set_active_device("U1-002", true);
        REQUIRE(preset_bundle.machine_filaments.empty());
    }
}

TEST_CASE("device object deltas update the machine info cache", "[gateway][machine-snapshot]")
{
    nlohmann::json objects{{"extruder", {{"nozzle_diameter", 0.4}}},
                           {"extruder1", {{"nozzle_diameter", "0.6"}}},
                           {"print_task_config",
                            {{"filament_vendor", json::array({"Snapmaker", "Polymaker"})},
                             {"filament_type", json::array({"PLA", "PETG"})}}}};

    Slic3r::PresetBundle   preset_bundle;
    GatewayMachineSnapshot machine_snapshot;
    machine_snapshot.set_dependencies([&preset_bundle] { return &preset_bundle; }, [] {});
    machine_snapshot.set_active_device("U1-001", true);

    const auto initial_snapshot = Slic3r::Gateway::build_machine_snapshot_from_device_objects(objects, "U1-001");
    REQUIRE(initial_snapshot.has_value());
    machine_snapshot.apply(*initial_snapshot);
    REQUIRE(preset_bundle.m_connect_machine_info_list[1].nozzle_info == "0.6");

    const nlohmann::json delta{{"extruder1", nlohmann::json{{"nozzle_diameter", 0.2}}}};
    Slic3r::Gateway::merge_device_object_changes(objects, delta);
    const auto updated_snapshot = Slic3r::Gateway::build_machine_snapshot_from_device_objects(objects, "U1-001");
    REQUIRE(updated_snapshot.has_value());
    machine_snapshot.apply(*updated_snapshot);
    REQUIRE(preset_bundle.m_connect_machine_info_list[1].nozzle_info == "0.2");
}
