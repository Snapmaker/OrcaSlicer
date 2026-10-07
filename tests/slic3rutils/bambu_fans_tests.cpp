#include <catch2/catch.hpp>

// The wx / Windows headers first, as every GUI source has them (GUI_App.hpp): a libslic3r header
// read before them leaves std::byte and the SDK's byte ambiguous (see bambu_reprint_tests.cpp).
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/BambuFans.hpp"
#include "slic3r/GUI/DeviceControls.hpp"
#include "slic3r/GUI/DeviceManager.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using nlohmann::json;

TEST_CASE("Classic 0..15 fan steps become 0..255 as Bambu Studio converts them", "[BambuFans]")
{
    CHECK(BambuFans::classic_fan_byte(json("15")) == 255);
    CHECK(BambuFans::classic_fan_byte(json("10")) == 153); // floor(10 / 1.5) = 6 -> 6 * 25.5
    CHECK(BambuFans::classic_fan_byte(json("0")) == 0);
    CHECK(BambuFans::classic_fan_byte(json(15)) == 255);   // a number is read too
    CHECK(BambuFans::classic_fan_byte(json("x")) == -1);   // unreadable, instead of throwing
    CHECK(BambuFans::classic_fan_byte(json()) == -1);
    // Through to what the phone shows: a full-speed X1C part fan is 100 %, not 2 %.
    CHECK(DeviceControls::percent_of_byte(BambuFans::classic_fan_byte(json("15"))) == 100);
}

TEST_CASE("Percent and 0..255 convert both ways", "[BambuFans]")
{
    CHECK(BambuFans::byte_of_percent(0) == 0);
    CHECK(BambuFans::byte_of_percent(100) == 255);
    CHECK(BambuFans::byte_of_percent(70) == 179);
    CHECK(BambuFans::byte_of_percent(-5) == 0);
    CHECK(BambuFans::byte_of_percent(150) == 255);
    for (int p = 0; p <= 100; p += 10) CHECK(DeviceControls::percent_of_byte(BambuFans::byte_of_percent(p)) == p);
}

// An H2-series device block: fan ids in bits 4..12 of "id" (type in bits 0..3), speed in percent in
// bits 0..7 of "state".
static json h2_device(int part, int aux, int chamber)
{
    return json::parse(R"({"airduct": {"modeCur": 0, "subMode": -1, "modeList": [],
        "parts": [
            {"id": )" + std::to_string((1 << 4) | 0) + R"(, "func": 1, "state": )" + std::to_string(part) + R"(, "range": 6553600},
            {"id": )" + std::to_string((2 << 4) | 0) + R"(, "func": 2, "state": )" + std::to_string(aux) + R"(, "range": 6553600},
            {"id": )" + std::to_string((3 << 4) | 0) + R"(, "func": 3, "state": )" + std::to_string(chamber) + R"(, "range": 6553600},
            {"id": )" + std::to_string((0 << 4) | 0) + R"(, "func": 0, "state": 100, "range": 6553600}
        ]}})");
}

TEST_CASE("The airduct block gives each fan's speed in percent", "[BambuFans]")
{
    const BambuFans::AirductFans f = BambuFans::parse_airduct_fans(h2_device(100, 70, 40));
    CHECK(f.present);
    CHECK(f.part == 100);
    CHECK(f.aux == 70);
    CHECK(f.chamber == 40);

    const BambuFans::AirductFans none = BambuFans::parse_airduct_fans(json::parse(R"({"fan": 1911})"));
    CHECK_FALSE(none.present);
    CHECK(none.part == -1);
    CHECK(none.aux == -1);
    CHECK(none.chamber == -1);

    // Only what "parts" lists, and a malformed entry is skipped rather than thrown on.
    const BambuFans::AirductFans partial = BambuFans::parse_airduct_fans(
        json::parse(R"({"airduct": {"parts": [{"id": 16, "state": 50}, {"id": "x", "state": 1}, {"state": 9}]}})"));
    CHECK(partial.present);
    CHECK(partial.part == 50);
    CHECK(partial.aux == -1);
    CHECK(partial.chamber == -1);
}

TEST_CASE("set_fan on an airduct printer sends the popup's step as a percentage", "[BambuFans]")
{
    CHECK(BambuFans::airduct_fan_index(1) == BambuFans::AIRDUCT_PART);
    CHECK(BambuFans::airduct_fan_index(2) == BambuFans::AIRDUCT_AUX);
    CHECK(BambuFans::airduct_fan_index(3) == BambuFans::AIRDUCT_CHAMBER);
    CHECK(BambuFans::airduct_fan_index(4) == -1);

    // The phone's 50 % -> DeviceControls::bambu_fan_value -> 127 -> speed 50 again.
    json j = BambuFans::set_fan_command(1, DeviceControls::bambu_fan_value(50), "7");
    CHECK(j["print"]["command"] == "set_fan");
    CHECK(j["print"]["sequence_id"] == "7");
    CHECK(j["print"]["fan_index"] == 1);
    CHECK(j["print"]["speed"] == 50);
    for (int p = 0; p <= 100; p += 10)
        CHECK(BambuFans::set_fan_command(2, DeviceControls::bambu_fan_value(p), "1")["print"]["speed"] == p);
}

// The report a printer on the new protocol sends: cfg / fun / aux / stat mark it, and device.fan is
// present. Empty flag strings keep parse_new_info from touching anything but the device block.
static json np_print(const json& device)
{
    json p;
    p["cfg"]    = "";
    p["fun"]    = "";
    p["aux"]    = "";
    p["stat"]   = "";
    p["device"] = device;
    return p;
}

TEST_CASE("device.fan no longer overwrites the classic speeds (X1C: Device tab 0 %, phone 2 %)", "[BambuFans]")
{
    MachineObject obj { nullptr, "fake-x1c", "FAKEX1C0001", "127.0.0.1" };
    // What the classic report left: part and chamber full, aux at step 10 of 15.
    obj.cooling_fan_speed = 255;
    obj.big_fan1_speed    = 153;
    obj.big_fan2_speed    = 255;
    obj.heatbreak_fan_speed = 15;

    // 0x0565: the old 3-bit reading made this part 5, aux 5, chamber 6 out of 255.
    obj.parse_new_info(np_print(json::parse(R"({"fan": 1381})")));
    CHECK(obj.is_enable_np);
    CHECK(obj.cooling_fan_speed == 255);
    CHECK(obj.big_fan1_speed == 153);
    CHECK(obj.big_fan2_speed == 255);
    CHECK(obj.heatbreak_fan_speed == 15);
    CHECK_FALSE(obj.m_airduct_fans.present);
    CHECK(DeviceControls::percent_of_byte(obj.cooling_fan_speed) == 100);
}

TEST_CASE("An H2 printer's airduct speeds replace the classic values and list its fans", "[BambuFans]")
{
    MachineObject obj { nullptr, "fake-h2s", "FAKEH2S0001", "127.0.0.1" };
    obj.cooling_fan_speed = 0; // the classic fields said nothing
    CHECK_FALSE(obj.has_aux_fan());
    CHECK_FALSE(obj.has_chamber_fan());

    obj.parse_new_info(np_print(h2_device(100, 70, 40)));
    CHECK(DeviceControls::percent_of_byte(obj.cooling_fan_speed) == 100);
    CHECK(DeviceControls::percent_of_byte(obj.big_fan1_speed) == 70);
    CHECK(DeviceControls::percent_of_byte(obj.big_fan2_speed) == 40);
    // The Device tab's ten steps: round(v / 25.5).
    CHECK((int) std::round(obj.big_fan1_speed / 25.5f) == 7);
    CHECK(obj.has_aux_fan());
    CHECK(obj.has_chamber_fan());

    // A report off the new protocol forgets the airduct fans.
    obj.parse_new_info(json::object());
    CHECK_FALSE(obj.m_airduct_fans.present);
    CHECK_FALSE(obj.has_aux_fan());
}
