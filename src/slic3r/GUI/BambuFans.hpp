#ifndef slic3r_GUI_BambuFans_hpp_
#define slic3r_GUI_BambuFans_hpp_

// How a Bambu printer reports its fan speeds and how a speed is sent back - kept free of wx and
// DeviceManager so the numbers can be unit tested (tests/slic3rutils/bambu_fans_tests.cpp).
//
// MachineObject keeps every fan as 0..255 (cooling_fan_speed = part, big_fan1_speed = aux,
// big_fan2_speed = chamber); the Device tab shows round(v / 25.5) * 10 % and the phone hub
// DeviceControls::percent_of_byte(v). Two report formats feed those fields:
//
//  * The classic print report: "cooling_fan_speed" / "big_fan1_speed" / "big_fan2_speed" as
//    strings in 0..15 steps (every Bambu printer, X1C included). classic_fan_byte() is Bambu
//    Studio's own conversion (DevFan::ParseV1_0).
//
//  * The new-protocol "device.airduct" block (H2D / H2S / H2C and newer): "parts" entries whose
//    "id" carries the fan in bits 4..12 and whose "state" carries its speed in percent in bits
//    0..7 (DevFan::ParseV3_0; the Device popup shows state / 10 as its ten steps).
//
// The new-protocol "device.fan" word is NOT a speed: an old Bambu Studio read 3-bit fields out of
// it and Bambu removed that in Dec 2024 ("FIX:fixed obtaining incorrect bits", 207d81c76). Reading
// it overwrote the classic 0..255 values with 0..7, so the Device tab showed 0 % and the phone 2 %.

#include <nlohmann/json.hpp>

#include <cmath>
#include <string>

namespace Slic3r { namespace GUI { namespace BambuFans {

// Airduct fan ids (Bambu Studio's AIR_FUN) for the three fans the Device tab and the phone show.
enum AirductFanId : int {
    AIRDUCT_PART    = 1, // FAN_COOLING_0_AIRDOOR
    AIRDUCT_AUX     = 2, // FAN_REMOTE_COOLING_0_IDX
    AIRDUCT_CHAMBER = 3, // FAN_CHAMBER_0_IDX
};

// A classic report value ("0".."15", a string; a number is accepted too) as 0..255, or -1 when it
// is missing or unreadable. 15 -> 255, 10 -> 153, 0 -> 0.
inline int classic_fan_byte(const nlohmann::json& v)
{
    int step = -1;
    try {
        if (v.is_string())
            step = std::stoi(v.get<std::string>());
        else if (v.is_number())
            step = v.get<int>();
    } catch (...) {
        return -1;
    }
    if (step < 0) return -1;
    if (step > 15) step = 15;
    return (int) std::lround(std::floor(step / 1.5f) * 25.5f);
}

// A percentage as the 0..255 the MachineObject fields hold. 100 -> 255, 70 -> 179, 0 -> 0.
inline int byte_of_percent(int percent)
{
    if (percent <= 0) return 0;
    if (percent >= 100) return 255;
    return (int) std::lround(percent * 255.0 / 100.0);
}

// The fans an airduct block reports. A speed is -1 when that fan is not in "parts".
struct AirductFans
{
    bool present = false; // the device block has an "airduct" object
    int  part    = -1;    // percent
    int  aux     = -1;
    int  chamber = -1;
};

inline AirductFans parse_airduct_fans(const nlohmann::json& device)
{
    AirductFans out;
    if (!device.is_object() || !device.contains("airduct") || !device["airduct"].is_object()) return out;
    out.present = true;
    const nlohmann::json& airduct = device["airduct"];
    if (!airduct.contains("parts") || !airduct["parts"].is_array()) return out;
    for (const nlohmann::json& p : airduct["parts"]) {
        if (!p.is_object() || !p.contains("id") || !p["id"].is_number_integer() || !p.contains("state") ||
            !p["state"].is_number_integer())
            continue;
        const long long raw_id = p["id"].get<long long>();
        const int       id     = (int) ((raw_id >> 4) & 0x1FF);
        int             state  = (int) (p["state"].get<long long>() & 0xFF);
        if (state > 100) state = 100;
        switch (id) {
        case AIRDUCT_PART: out.part = state; break;
        case AIRDUCT_AUX: out.aux = state; break;
        case AIRDUCT_CHAMBER: out.chamber = state; break;
        default: break;
        }
    }
    return out;
}

// MachineObject::FanType (1 part, 2 aux, 3 chamber) as the airduct fan_index of a "set_fan"
// command - the same numbers, named here so the mapping is written down once.
inline int airduct_fan_index(int fan_type)
{
    switch (fan_type) {
    case 1: return AIRDUCT_PART;
    case 2: return AIRDUCT_AUX;
    case 3: return AIRDUCT_CHAMBER;
    default: return -1;
    }
}

// The new-protocol set_fan command (Bambu Studio's DevFan::command_control_fan_new) for an airduct
// printer: the speed in percent, from the 0..255 value the Device tab's popup and the phone hub
// produce (floor(step * 25.5), so it is rounded back to the step's ten percent).
inline nlohmann::json set_fan_command(int fan_index, int byte_value, const std::string& sequence_id)
{
    int step = (int) std::lround(byte_value / 25.5);
    if (step < 0) step = 0;
    if (step > 10) step = 10;
    nlohmann::json j;
    j["print"]["command"]     = "set_fan";
    j["print"]["sequence_id"] = sequence_id;
    j["print"]["fan_index"]   = fan_index;
    j["print"]["speed"]       = step * 10;
    return j;
}

}}} // namespace Slic3r::GUI::BambuFans

#endif // slic3r_GUI_BambuFans_hpp_
