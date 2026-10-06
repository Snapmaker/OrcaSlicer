#include <catch2/catch.hpp>

#include "slic3r/GUI/DeviceControls.hpp"
#include "slic3r/GUI/FilamentCommands.hpp"

using namespace Slic3r::GUI::FilamentCommands;
using nlohmann::json;

// The MQTT payload the Device tab's Load / Unload (and now the phone's) send, captured from the one
// builder both use. Expected values follow Bambu Studio's command_ams_change_filament.
TEST_CASE("AMS load targets ams*4 + slot for AMS units 0..3", "[FilamentCommands]")
{
    for (int ams = 0; ams < 4; ++ams)
        for (int slot = 0; slot < 4; ++slot) {
            const json p = ams_change_filament_json(true, std::to_string(ams), std::to_string(slot), 220, 250, false);
            CHECK(p["command"] == "ams_change_filament");
            CHECK(p["ams_id"] == ams);
            CHECK(p["slot_id"] == slot);
            CHECK(p["target"] == ams * 4 + slot);
            CHECK(p["curr_temp"] == 220);
            CHECK(p["tar_temp"] == 250);
            CHECK_FALSE(p.contains("sequence_id"));
        }
}

TEST_CASE("The AMS range check is numeric, not a string comparison", "[FilamentCommands]")
{
    // "2" > "16" as strings: the old check sent target 2 for AMS 3 slot 1.
    CHECK(ams_change_filament_json(true, "2", "1", -1, -1, false)["target"] == 9);
    CHECK(ams_change_filament_json(true, "3", "3", -1, -1, false)["target"] == 15);
    // An AMS HT (128+) is addressed by its own id; "128" < "16" as strings sent 512 + slot.
    const json ht = ams_change_filament_json(true, "128", "0", 200, 230, true);
    CHECK(ht["ams_id"] == 128);
    CHECK(ht["target"] == 128);
    CHECK(ht["slot_id"] == 0);
    CHECK(ams_change_filament_json(true, "129", "0", -1, -1, true)["target"] == 129);
}

TEST_CASE("External spools: 254 becomes 255 on one extruder, 254 / 255 are left / right on two", "[FilamentCommands]")
{
    SECTION("single extruder (X1C, P1, A1)")
    {
        const json p = ams_change_filament_json(true, "254", "0", 215, 215, false);
        CHECK(p["ams_id"] == 255);
        CHECK(p["target"] == 255);
        CHECK(p["slot_id"] == 0);
        CHECK(ams_change_filament_json(false, "254", "255", 210, 210, false)["ams_id"] == 255);
    }
    SECTION("two extruders (H2D, H2C): passed through as the dual layout names them")
    {
        const json left  = ams_change_filament_json(true, "254", "0", 215, 215, true);
        const json right = ams_change_filament_json(true, "255", "0", 215, 215, true);
        CHECK(left["ams_id"] == 254);
        CHECK(left["target"] == 254);
        CHECK(right["ams_id"] == 255);
        CHECK(right["target"] == 255);
    }
}

TEST_CASE("Unload is target 255, slot 255, with the method's default temperatures", "[FilamentCommands]")
{
    const json p = ams_change_filament_json(false, "1", "255", UNLOAD_DEFAULT_TEMP, UNLOAD_DEFAULT_TEMP, true);
    CHECK(p["ams_id"] == 1);
    CHECK(p["target"] == 255);
    CHECK(p["slot_id"] == 255);
    CHECK(p["curr_temp"] == 210);
    CHECK(p["tar_temp"] == 210);
}

TEST_CASE("Tray temperatures and tray_now", "[FilamentCommands]")
{
    CHECK(tray_mid_temp("190", "230") == 210);
    CHECK(tray_mid_temp("", "230") == -1);
    CHECK(tray_mid_temp("190", "") == -1);
    CHECK(tray_now_of("0", "0") == "0");
    CHECK(tray_now_of("1", "2") == "6");
    CHECK(tray_now_of("128", "0") == "128");
    CHECK(tray_now_of("254", "0") == "254");
}

TEST_CASE("Load / unload availability", "[FilamentCommands]")
{
    PrinterState idle;
    SECTION("idle: an empty-feed slot loads, the loaded one unloads")
    {
        auto a = availability(idle, true, false);
        CHECK(a.can_load);
        CHECK_FALSE(a.can_unload);
        a = availability(idle, true, true);
        CHECK_FALSE(a.can_load);
        CHECK(a.can_unload);
    }
    SECTION("a slot with nothing in it loads nothing")
    {
        const auto a = availability(idle, false, false);
        CHECK_FALSE(a.can_load);
        CHECK(a.why == "the slot is empty");
    }
    SECTION("never while printing or paused, changing filament or calibrating")
    {
        PrinterState s;
        s.printing = true;
        auto a     = availability(s, true, true);
        CHECK_FALSE(a.can_load);
        CHECK_FALSE(a.can_unload);
        CHECK(a.why.find("print") != std::string::npos);
        s          = PrinterState();
        s.changing_filament = true;
        CHECK_FALSE(availability(s, true, false).can_load);
        s             = PrinterState();
        s.calibrating = true;
        CHECK_FALSE(availability(s, true, true).can_unload);
    }
    SECTION("no filament at the extruder: nothing to unload")
    {
        PrinterState s;
        s.filament_at_extruder = false;
        CHECK_FALSE(availability(s, true, true).can_unload);
    }
    SECTION("flexible filament is unloaded by hand (U1)")
    {
        const auto a = availability(idle, true, true, is_flexible("TPU 95A"));
        CHECK_FALSE(a.can_unload);
        CHECK(a.why == "flexible filament is unloaded by hand");
    }
    SECTION("the JSON fields")
    {
        json j;
        write_availability(availability(idle, true, false), j);
        CHECK(j["can_load"] == true);
        CHECK(j["can_unload"] == false);
        CHECK_FALSE(j.contains("filament_why"));
    }
}

TEST_CASE("U1 scripts (assumed macros) and their temperatures", "[FilamentCommands]")
{
    CHECK(u1_unload_script(2, 220, 0.4) == "T2\nINNER_FILAMENT_UNLOAD TEMP=220 NOZZLE_DIAMETER=0.4\nPARK_EXTRUDER");
    CHECK(u1_unload_script(0, 250, 0) == "T0\nINNER_FILAMENT_UNLOAD TEMP=250 NOZZLE_DIAMETER=0.4\nPARK_EXTRUDER");
    CHECK(u1_load_script(3) == "SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=3 TEMP=140\nSM_PRINT_AUTO_FEED EXTRUDER=3");
    CHECK(u1_unload_temp("PLA") == 220);
    CHECK(u1_unload_temp("PETG") == 250);
    CHECK(u1_unload_temp("ABS") == 260);
    CHECK(u1_unload_temp("PA-CF") == 270);
    CHECK(u1_unload_temp("") == 230);
    CHECK(is_flexible("tpu"));
    CHECK_FALSE(is_flexible("PLA"));
}

TEST_CASE("A U1 offers load / unload only when its G-code help lists every command", "[FilamentCommands]")
{
    json help = json::parse(R"({"result": {"INNER_FILAMENT_UNLOAD": "", "PARK_EXTRUDER": "", "SM_PRINT_EXTRUDER_PREHEAT": "",
                                           "SM_PRINT_AUTO_FEED": "", "G28": "Home"}})");
    CHECK(u1_filament_macros_available(help));
    help["result"].erase("SM_PRINT_AUTO_FEED");
    CHECK_FALSE(u1_filament_macros_available(help));
    CHECK_FALSE(u1_filament_macros_available(json::parse(R"({"result": {}})")));
    CHECK_FALSE(u1_filament_macros_available(json()));
    // Klipper lists macros upper-case; a lower-case listing still counts.
    CHECK(u1_filament_macros_available(json::parse(R"({"inner_filament_unload": "", "park_extruder": "",
                                                     "sm_print_extruder_preheat": "", "sm_print_auto_feed": ""})")));
}

TEST_CASE("controls.filament marks a printer that takes the verbs, and an assumed U1", "[FilamentCommands]")
{
    using namespace Slic3r::GUI::DeviceControls;
    Caps caps;
    CHECK_FALSE(to_json(caps).contains("filament"));
    caps.filament_actions = true;
    json j = to_json(caps);
    CHECK(j["filament"]["load"] == true);
    CHECK(j["filament"]["unload"] == true);
    CHECK_FALSE(j["filament"].contains("assumed"));
    caps.filament_assumed = true;
    CHECK(to_json(caps)["filament"]["assumed"] == true);
}
