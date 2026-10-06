#include <catch2/catch.hpp>

#include "slic3r/GUI/DeviceControls.hpp"

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::DeviceControls;
using nlohmann::json;

// A U1's /printer/objects/query status, as mock_printhost / a real U1 (firmware 1.5.2) answer it,
// with the controls objects the phone's printer screen reads.
static json u1_status()
{
    return json::parse(R"({
        "heater_bed": {"temperature": 24.5, "target": 0.0},
        "extruder":  {"temperature": 25.0, "target": 0.0},
        "extruder1": {"temperature": 26.0, "target": 210.0},
        "extruder2": {"temperature": 25.0, "target": 0.0},
        "extruder3": {"temperature": 25.0, "target": 0.0},
        "gcode_move": {"speed_factor": 1.25, "speed": 1500.0, "extrude_factor": 1.0},
        "fan": {"speed": 0.5},
        "led cavity_led": {"color_data": [[0.0, 0.0, 0.0, 1.0]]},
        "fan_generic cavity_fan": {"speed": 0.0}
    })");
}

TEST_CASE("A U1's objects become its controls: bed, four toolheads, speed factor, light, two fans", "[DeviceControls]")
{
    const Caps caps = moonraker_caps(u1_status(), 100, 300, 4);
    REQUIRE(caps.heaters.size() == 5);
    CHECK(caps.heaters[0].id == "bed");
    CHECK(caps.heaters[0].max == 100);
    CHECK(caps.heaters[1].id == "nozzle0");
    CHECK(caps.heaters[1].label == "Nozzle 1");
    CHECK(caps.heaters[2].id == "nozzle1");
    CHECK(caps.heaters[2].target == Approx(210.0));
    CHECK(caps.heaters[4].label == "Nozzle 4");
    CHECK(caps.heaters[4].max == 300);
    for (const Heater& h : caps.heaters) CHECK(h.settable);

    REQUIRE(caps.has_speed);
    CHECK(caps.speed.kind == "factor");
    CHECK(caps.speed.value == 125);
    CHECK(caps.speed.min == SPEED_FACTOR_MIN);
    CHECK(caps.speed.max == SPEED_FACTOR_MAX);

    CHECK(caps.has_light);
    CHECK(caps.light_on);

    REQUIRE(caps.fans.size() == 2);
    CHECK(caps.fans[0].id == "part");
    CHECK(caps.fans[0].percent == 50);
    CHECK(caps.fans[1].id == "cavity");
    CHECK(caps.fans[1].percent == 0);
}

TEST_CASE("Only what the printer answered is a control", "[DeviceControls]")
{
    SECTION("a one-nozzle Klipper printer without light or speed report")
    {
        const Caps caps = moonraker_caps(json::parse(R"({"heater_bed":{"temperature":20,"target":0},
                                                          "extruder":{"temperature":21,"target":0},
                                                          "extruder1":{}})"));
        REQUIRE(caps.heaters.size() == 2);
        CHECK(caps.heaters[1].label == "Nozzle"); // a single nozzle is not numbered
        CHECK_FALSE(caps.has_speed);
        CHECK_FALSE(caps.has_light);
        CHECK(caps.fans.empty());
        const json j = to_json(caps);
        CHECK_FALSE(j.contains("speed"));
        CHECK_FALSE(j.contains("light"));
        CHECK(j["fans"].empty());
    }
    SECTION("an LED that is dark reads as off")
    {
        json st = u1_status();
        st["led cavity_led"]["color_data"] = json::parse("[[0,0,0,0]]");
        CHECK_FALSE(moonraker_caps(st).light_on);
    }
    SECTION("not an object at all")
    {
        CHECK(moonraker_caps(json()).heaters.empty());
    }
}

TEST_CASE("Bambu nozzles follow the Device tab's rows: one Nozzle, or L above R", "[DeviceControls]")
{
    SECTION("single nozzle (X1C / P1 / A1)")
    {
        const auto hs = bambu_nozzle_heaters({ { 0, 215, 220 } }, 1, 0, -1);
        REQUIRE(hs.size() == 1);
        CHECK(hs[0].id == "nozzle0");
        CHECK(hs[0].label == "Nozzle");
        CHECK(hs[0].max == DEFAULT_NOZZLE_MAX); // the printer reported no limit
    }
    SECTION("H2D / H2C: left (extruder 1) listed first, right (extruder 0) second, with its own limit")
    {
        const auto hs = bambu_nozzle_heaters({ { 0, 30, 0 }, { 1, 220, 220 } }, 2, 1, 350);
        REQUIRE(hs.size() == 2);
        CHECK(hs[0].id == "nozzle1");
        CHECK(hs[0].label == "L");
        CHECK(hs[0].active);
        CHECK(hs[1].id == "nozzle0");
        CHECK(hs[1].label == "R");
        CHECK(hs[1].max == 350);
    }
}

TEST_CASE("to_json is the documented `controls` shape", "[DeviceControls]")
{
    Caps caps;
    caps.heaters.push_back(Heater { "bed", "Bed", 60.2, 60, 110, true, false });
    caps.heaters.push_back(Heater { "chamber", "Chamber", 31, 0, 60, false, false });
    caps.has_speed = true;
    caps.speed     = Speed { "level", 2, 1, 4, false };
    caps.has_light = true;
    caps.light_on  = false;
    caps.fans.push_back(Fan { "aux", "Aux", 40 });
    const json j = to_json(caps);
    REQUIRE(j["heaters"].size() == 2);
    CHECK(j["heaters"][0]["id"] == "bed");
    CHECK(j["heaters"][0]["min"] == 0);
    CHECK(j["heaters"][0]["max"] == 110);
    CHECK(j["heaters"][1]["settable"] == false);
    CHECK(j["speed"]["kind"] == "level");
    CHECK(j["speed"]["settable"] == false);
    REQUIRE(j["speed"]["levels"].size() == 4);
    CHECK(j["speed"]["levels"][3]["label"] == "Ludicrous");
    CHECK(j["light"]["on"] == false);
    CHECK(j["fans"][0]["id"] == "aux");
    CHECK(j["fans"][0]["percent"] == 40);
}

TEST_CASE("set_temp: heater, limit, whole degrees, and the idle confirmation above 50 C", "[DeviceControls]")
{
    const Caps caps = moonraker_caps(u1_status(), 100, 300, 4);
    int         t   = -1;
    std::string why;

    CHECK(check_set_temp(caps, "bed", "60", true, false, t, why) == 0);
    CHECK(t == 60);
    CHECK(check_set_temp(caps, "nozzle3", "0", false, false, t, why) == 0); // off needs nothing
    CHECK(t == 0);
    CHECK(check_set_temp(caps, "nozzle1", "50", false, false, t, why) == 0); // at the line: no confirm
    CHECK(check_set_temp(caps, "bed", "101", true, false, t, why) == 400);
    CHECK(why.find("100") != std::string::npos);
    CHECK(check_set_temp(caps, "nozzle0", "301", true, true, t, why) == 400);
    CHECK(check_set_temp(caps, "bed", "60.5", true, false, t, why) == 400);
    CHECK(check_set_temp(caps, "bed", "-5", true, false, t, why) == 400);
    CHECK(check_set_temp(caps, "bed", "", true, false, t, why) == 400);
    CHECK(check_set_temp(caps, "chamber", "40", true, false, t, why) == 404); // the U1 has none
    CHECK(check_set_temp(caps, "", "40", true, false, t, why) == 400);

    SECTION("idle above 50 C: refused without confirm, accepted with it")
    {
        CHECK(check_set_temp(caps, "nozzle0", "210", false, false, t, why) == 400);
        CHECK(why.find("confirm") != std::string::npos);
        CHECK(check_set_temp(caps, "nozzle0", "210", true, false, t, why) == 0);
    }
    SECTION("printing: a change needs no confirm")
    {
        CHECK(check_set_temp(caps, "nozzle0", "215", false, true, t, why) == 0);
    }
    SECTION("a read-only heater is 409")
    {
        Caps x1;
        x1.heaters.push_back(Heater { "chamber", "Chamber", 30, 0, 60, false, false });
        CHECK(check_set_temp(x1, "chamber", "40", true, false, t, why) == 409);
    }
}

TEST_CASE("set_speed / set_light / set_fan checks", "[DeviceControls]")
{
    const Caps  u1 = moonraker_caps(u1_status(), 100, 300, 4);
    int         v  = 0;
    bool        on = false;
    std::string why;

    CHECK(check_set_speed(u1, "150", v, why) == 0);
    CHECK(v == 150);
    CHECK(check_set_speed(u1, "5", v, why) == 400);
    CHECK(check_set_speed(u1, "301", v, why) == 400);

    Caps bambu;
    bambu.has_speed = true;
    bambu.speed     = Speed { "level", 2, 1, 4, true };
    CHECK(check_set_speed(bambu, "4", v, why) == 0);
    CHECK(check_set_speed(bambu, "5", v, why) == 400);
    CHECK(check_set_speed(bambu, "0", v, why) == 400);
    bambu.speed.settable = false; // not printing
    CHECK(check_set_speed(bambu, "3", v, why) == 409);
    CHECK(check_set_speed(Caps(), "3", v, why) == 409);

    CHECK(check_set_light(u1, "1", on, why) == 0);
    CHECK(on);
    CHECK(check_set_light(u1, "0", on, why) == 0);
    CHECK_FALSE(on);
    CHECK(check_set_light(u1, "yes", on, why) == 400);
    CHECK(check_set_light(Caps(), "1", on, why) == 409);

    CHECK(check_set_fan(u1, "part", "70", v, why) == 0);
    CHECK(v == 70);
    CHECK(check_set_fan(u1, "part", "101", v, why) == 400);
    CHECK(check_set_fan(u1, "aux", "50", v, why) == 404); // the U1 has no aux fan
    CHECK(check_set_fan(u1, "", "50", v, why) == 400);
}

TEST_CASE("Klipper G-code for each control", "[DeviceControls]")
{
    // What the Snapmaker app's own device page sends for a toolhead.
    CHECK(moonraker_temp_script("nozzle0", 210) == "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=210");
    CHECK(moonraker_temp_script("nozzle3", 0) == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");
    CHECK(moonraker_temp_script("bed", 60) == "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=60");
    CHECK(moonraker_temp_script("chamber", 40).empty());
    CHECK(moonraker_temp_script("nozzle", 40).empty());
    CHECK(moonraker_speed_script(125) == "M220 S125");
    CHECK(moonraker_light_script(true) == "SET_LED LED=cavity_led WHITE=1");
    CHECK(moonraker_light_script(false) == "SET_LED LED=cavity_led WHITE=0");
    CHECK(moonraker_fan_script("part", 100) == "M106 S255");
    CHECK(moonraker_fan_script("part", 50) == "M106 S128");
    CHECK(moonraker_fan_script("part", 0) == "M106 S0");
    CHECK(moonraker_fan_script("cavity", 40) == "SET_FAN_SPEED FAN=cavity_fan SPEED=0.40");
    CHECK(moonraker_fan_script("aux", 40).empty());
    CHECK(moonraker_controls_query() == "gcode_move&fan&led%20cavity_led&fan_generic%20cavity_fan");
}

TEST_CASE("Bambu fan values are the Device tab popup's ten steps", "[DeviceControls]")
{
    // FanControl::command_control_fan: floor(step * 25.5) for step 0..10.
    CHECK(bambu_fan_value(0) == 0);
    CHECK(bambu_fan_value(10) == 25);
    CHECK(bambu_fan_value(50) == 127);
    CHECK(bambu_fan_value(54) == 127); // rounds to step 5
    CHECK(bambu_fan_value(56) == 153); // rounds to step 6
    CHECK(bambu_fan_value(100) == 255);
    CHECK(bambu_fan_type("part") == 1);
    CHECK(bambu_fan_type("aux") == 2);
    CHECK(bambu_fan_type("chamber") == 3);
    CHECK(bambu_fan_type("cavity") == 0);
    CHECK(percent_of_byte(255) == 100);
    CHECK(percent_of_byte(127) == 50);
    CHECK(percent_of_byte(0) == 0);
}

TEST_CASE("Heater ids", "[DeviceControls]")
{
    CHECK(nozzle_index_of("nozzle0") == 0);
    CHECK(nozzle_index_of("nozzle12") == 12);
    CHECK(nozzle_index_of("nozzle") == -1);
    CHECK(nozzle_index_of("nozzleX") == -1);
    CHECK(nozzle_index_of("bed") == -1);
    CHECK(klipper_heater("nozzle1") == "extruder1");
    CHECK(klipper_heater("nozzle0") == "extruder");
    CHECK(klipper_heater("bed") == "heater_bed");
    CHECK(klipper_heater("chamber").empty());
}
