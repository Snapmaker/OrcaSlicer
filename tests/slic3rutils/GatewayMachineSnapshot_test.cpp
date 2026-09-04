#include <catch2/catch.hpp>

#include "slic3r/GUI/GatewayMachineSnapshot.hpp"
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
