#ifndef slic3r_GUI_DeviceControls_hpp_
#define slic3r_GUI_DeviceControls_hpp_

// What the phone's native printer screen may control on one printer, and the commands behind each
// control - kept free of wx, DeviceManager and the network so it can be unit tested on its own
// (tests/slic3rutils/device_controls_tests.cpp), exactly like NozzleTempRows.hpp.
//
// Two halves:
//
//  * Capabilities. `Caps` is what GET /api/printers adds to a row as `controls`: the heaters with
//    their current reading, their limit and whether a target can be set; the speed control
//    (Bambu's four levels, or Klipper's speed factor); the chamber light; and the fans. A client
//    draws exactly what is here and nothing else, so a printer that has no chamber light never gets
//    a light switch. Built by RemoteControl from a MachineObject (Bambu) or from a Moonraker
//    /printer/objects/query answer (a Snapmaker U1 or another Klipper printer).
//
//  * Commands. The checks POST /api/printers/{id}/control runs on action=set_temp | set_speed |
//    set_light | set_fan before anything is sent, and the Moonraker G-code each one becomes. The
//    Bambu side sends through the same MachineObject calls the desktop Device tab makes
//    (command_set_bed / command_set_nozzle[_new] / command_set_chamber, command_set_printing_speed,
//    command_set_chamber_light, command_control_fan_val); the helpers here only say which call
//    and with what value, so the numbers can be tested without a printer.
//
// Heater ids are one namespace for every printer: "bed", "chamber", and "nozzle<N>" where N is the
// printer's own extruder index - Bambu's extruder id (0 = right/main, 1 = left/deputy on the
// two-nozzle machines) or Klipper's extruder number (extruder, extruder1, ... on the U1).

#include "NozzleTempRows.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace DeviceControls {

// Heating anything above this while the printer is not printing is a confirmed action: the phone
// asks the person first and says so with confirm=1, and the route refuses it without. A print in
// progress is already hot and already attended to, so a change there is not asked about.
constexpr int CONFIRM_ABOVE_C = 50;

// The desktop Device tab's own fallbacks (StatusPanel.cpp: bed_temp_range, nozzle_temp_range,
// nozzle_chamber_range), used when the printer reports no limit of its own.
constexpr int DEFAULT_BED_MAX     = 120;
constexpr int DEFAULT_NOZZLE_MAX  = 300;
constexpr int DEFAULT_CHAMBER_MAX = 60;

// Klipper's speed factor, as a percentage, where the printer accepts M220. The phone offers a few
// steps inside this range; anything outside it is refused before it reaches the printer.
constexpr int SPEED_FACTOR_MIN = 10;
constexpr int SPEED_FACTOR_MAX = 300;

struct Heater
{
    std::string id;     // bed | chamber | nozzle<N>
    std::string label;  // "Bed", "Chamber", "Nozzle", "L", "R", "Nozzle 1".."Nozzle 4"
    double      temp { 0 };
    double      target { 0 };
    int         max { 0 };        // the highest target the printer accepts
    bool        settable { true }; // false: shown, but only read (an X1's chamber sensor)
    bool        active { false };  // the extruder the printer reports as current
};

struct Fan
{
    std::string id;    // part | aux | chamber | cavity
    std::string label;
    int         percent { 0 }; // 0..100, as last reported
};

struct Speed
{
    std::string kind;        // "level" (Bambu: 1 silent .. 4 ludicrous) or "factor" (Klipper M220, %)
    int         value { 0 }; // the level, or the percentage; 0 = not reported
    int         min { 0 }, max { 0 };
    bool        settable { true }; // Bambu takes a speed level only while it prints
};

struct Caps
{
    std::vector<Heater> heaters;
    bool                has_speed { false };
    Speed               speed;
    bool                has_light { false };
    bool                light_on { false };
    std::vector<Fan>    fans;
    // Loading / unloading filament (FilamentCommands.hpp): whether the printer takes the
    // load_filament / unload_filament verbs at all. Which slot can do what right now is on each AMS
    // tray / toolhead as can_load / can_unload. `filament_assumed` marks the U1, whose macros come
    // from U1 owners rather than from Snapmaker (the owner verifies them on hardware).
    bool                filament_actions { false };
    bool                filament_assumed { false };
};

// ------------------------------------------------------------- capabilities ----

inline std::string nozzle_id(int extruder_index) { return "nozzle" + std::to_string(extruder_index); }

// "nozzle1" -> 1; -1 for anything that is not a nozzle id.
inline int nozzle_index_of(const std::string& heater_id)
{
    if (heater_id.size() < 7 || heater_id.compare(0, 6, "nozzle") != 0) return -1;
    int n = 0;
    for (size_t i = 6; i < heater_id.size(); ++i) {
        const char c = heater_id[i];
        if (c < '0' || c > '9') return -1;
        n = n * 10 + (c - '0');
        if (n > 63) return -1;
    }
    return n;
}

inline const Heater* find_heater(const Caps& caps, const std::string& id)
{
    for (const Heater& h : caps.heaters)
        if (h.id == id) return &h;
    return nullptr;
}

inline const Fan* find_fan(const Caps& caps, const std::string& id)
{
    for (const Fan& f : caps.fans)
        if (f.id == id) return &f;
    return nullptr;
}

// A Bambu printer's nozzles as heaters: one "Nozzle" row on a single-nozzle printer, and L above R
// on the two-nozzle ones - the rows the Device tab draws (NozzleTempRows), addressed by the
// extruder id its set-temperature command takes.
inline std::vector<Heater> bambu_nozzle_heaters(const std::vector<NozzleTempSample>& extders, int total_count, int current_id,
                                                int nozzle_max)
{
    std::vector<Heater> out;
    const int max = nozzle_max > 0 ? nozzle_max : DEFAULT_NOZZLE_MAX;
    for (const NozzleTempRow& r : nozzle_temp_rows(extders, total_count, current_id)) {
        Heater h;
        h.id     = nozzle_id(r.extruder_id);
        h.label  = r.badge.empty() ? std::string("Nozzle") : r.badge;
        h.temp   = r.temp;
        h.target = r.target;
        h.max    = max;
        h.active = r.active;
        out.push_back(h);
    }
    return out;
}

// Klipper reports a fan's speed as 0..1.
inline int percent_of_unit(double v)
{
    if (!(v >= 0)) return 0;
    if (v > 1) v = 1;
    return (int) std::lround(v * 100.0);
}

// Bambu reports fan speeds as 0..255 (DeviceManager's fan_gear / *_fan_speed parsing).
inline int percent_of_byte(int v)
{
    if (v <= 0) return 0;
    if (v >= 255) return 100;
    return (int) std::lround(v * 100.0 / 255.0);
}

inline bool read_num(const nlohmann::json& obj, const char* key, double& out)
{
    if (!obj.is_object() || !obj.contains(key) || !obj[key].is_number()) return false;
    out = obj[key].get<double>();
    return true;
}

// The controls of a Klipper printer, from one /printer/objects/query status object: the bed, every
// extruder it answered for (extruder, extruder1, ...), the speed factor (gcode_move), the part
// fan (fan), and - where the printer has them, as a Snapmaker U1 does - the cavity light
// ("led cavity_led") and the cavity fan ("fan_generic cavity_fan"). Only what answered is listed.
inline Caps moonraker_caps(const nlohmann::json& status, int bed_max = DEFAULT_BED_MAX, int nozzle_max = DEFAULT_NOZZLE_MAX,
                           int max_extruders = 4)
{
    Caps caps;
    if (!status.is_object()) return caps;
    double temp = 0, target = 0;
    if (status.contains("heater_bed") && read_num(status["heater_bed"], "temperature", temp)) {
        read_num(status["heater_bed"], "target", target);
        caps.heaters.push_back(Heater { "bed", "Bed", temp, target, bed_max, true, false });
    }
    std::vector<Heater> nozzles;
    for (int i = 0; i < max_extruders; ++i) {
        const std::string ex = i == 0 ? "extruder" : "extruder" + std::to_string(i);
        if (!status.contains(ex) || !read_num(status[ex], "temperature", temp)) break;
        target = 0;
        read_num(status[ex], "target", target);
        nozzles.push_back(Heater { nozzle_id(i), "", temp, target, nozzle_max, true, false });
    }
    for (Heater& h : nozzles) {
        h.label = nozzles.size() == 1 ? std::string("Nozzle") : "Nozzle " + std::to_string(nozzle_index_of(h.id) + 1);
        caps.heaters.push_back(h);
    }
    double v = 0;
    if (status.contains("gcode_move") && read_num(status["gcode_move"], "speed_factor", v)) {
        caps.has_speed = true;
        caps.speed     = Speed { "factor", (int) std::lround(v * 100.0), SPEED_FACTOR_MIN, SPEED_FACTOR_MAX, true };
    }
    if (status.contains("led cavity_led") && status["led cavity_led"].is_object()) {
        const nlohmann::json& led = status["led cavity_led"];
        // color_data is [[r, g, b, w], ...] per LED in the chain; the light is on when any channel is.
        if (led.contains("color_data") && led["color_data"].is_array()) {
            caps.has_light = true;
            for (const auto& px : led["color_data"])
                if (px.is_array())
                    for (const auto& ch : px)
                        if (ch.is_number() && ch.get<double>() > 0.001) caps.light_on = true;
        }
    }
    if (status.contains("fan") && read_num(status["fan"], "speed", v))
        caps.fans.push_back(Fan { "part", "Part cooling", percent_of_unit(v) });
    if (status.contains("fan_generic cavity_fan") && read_num(status["fan_generic cavity_fan"], "speed", v))
        caps.fans.push_back(Fan { "cavity", "Cavity", percent_of_unit(v) });
    return caps;
}

// The query string that asks a Klipper printer for everything moonraker_caps reads, on top of the
// print state. Object names with a space are percent-encoded, as Moonraker expects them.
inline std::string moonraker_controls_query() { return "gcode_move&fan&led%20cavity_led&fan_generic%20cavity_fan"; }

inline nlohmann::json to_json(const Caps& caps)
{
    nlohmann::json j = nlohmann::json::object();
    nlohmann::json hs = nlohmann::json::array();
    for (const Heater& h : caps.heaters)
        hs.push_back({ { "id", h.id },         { "label", h.label }, { "temp", h.temp },     { "target", h.target },
                       { "min", 0 },           { "max", h.max },     { "settable", h.settable }, { "active", h.active } });
    j["heaters"] = hs;
    if (caps.has_speed) {
        nlohmann::json s = { { "kind", caps.speed.kind }, { "value", caps.speed.value }, { "min", caps.speed.min },
                             { "max", caps.speed.max },   { "settable", caps.speed.settable } };
        if (caps.speed.kind == "level")
            s["levels"] = nlohmann::json::array({ { { "value", 1 }, { "label", "Silent" } },
                                                  { { "value", 2 }, { "label", "Standard" } },
                                                  { { "value", 3 }, { "label", "Sport" } },
                                                  { { "value", 4 }, { "label", "Ludicrous" } } });
        j["speed"] = s;
    }
    if (caps.has_light) j["light"] = { { "on", caps.light_on } };
    nlohmann::json fs = nlohmann::json::array();
    for (const Fan& f : caps.fans) fs.push_back({ { "id", f.id }, { "label", f.label }, { "percent", f.percent } });
    j["fans"] = fs;
    if (caps.filament_actions) {
        j["filament"] = { { "load", true }, { "unload", true } };
        if (caps.filament_assumed) j["filament"]["assumed"] = true;
    }
    return j;
}

// ---------------------------------------------------------------- commands ----

// A whole number of degrees, 0..999, and nothing else: "60", not "60.5", "-1" or "60C".
inline bool parse_int(const std::string& text, int lo, int hi, int& out)
{
    if (text.empty() || text.size() > 4) return false;
    int n = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + (c - '0');
    }
    if (n < lo || n > hi) return false;
    out = n;
    return true;
}

// The checks set_temp runs before anything is sent. 0 when it may go out, otherwise the HTTP status
// and a sentence for the phone. `printing` is the printer's own "a print is in progress".
inline int check_set_temp(const Caps& caps, const std::string& heater, const std::string& target_text, bool confirm,
                          bool printing, int& target, std::string& why)
{
    const Heater* h = find_heater(caps, heater);
    if (!h) {
        why = heater.empty() ? "heater is required (bed, chamber or nozzle<N>, as controls.heaters lists them)"
                             : "this printer has no heater called " + heater;
        return heater.empty() ? 400 : 404;
    }
    if (!h->settable) {
        why = "the " + h->label + " temperature can only be read on this printer";
        return 409;
    }
    if (!parse_int(target_text, 0, 999, target)) {
        why = "target must be a whole number of degrees";
        return 400;
    }
    if (target > h->max) {
        why = "the " + h->label + " limit is " + std::to_string(h->max) + " C";
        return 400;
    }
    if (target > CONFIRM_ABOVE_C && !printing && !confirm) {
        why = "heating above " + std::to_string(CONFIRM_ABOVE_C) + " C while the printer is idle needs confirm=1";
        return 400;
    }
    return 0;
}

inline int check_set_speed(const Caps& caps, const std::string& value_text, int& value, std::string& why)
{
    if (!caps.has_speed) { why = "this printer's speed cannot be changed from here"; return 409; }
    if (!caps.speed.settable) { why = "the speed can only be changed while a print is running"; return 409; }
    if (!parse_int(value_text, caps.speed.min, caps.speed.max, value)) {
        why = "value must be " + std::to_string(caps.speed.min) + ".." + std::to_string(caps.speed.max) +
              (caps.speed.kind == "level" ? " (1 silent, 2 standard, 3 sport, 4 ludicrous)" : " (percent)");
        return 400;
    }
    return 0;
}

inline int check_set_light(const Caps& caps, const std::string& on_text, bool& on, std::string& why)
{
    if (!caps.has_light) { why = "this printer has no light the phone can switch"; return 409; }
    if (on_text != "0" && on_text != "1") { why = "on must be 1 or 0"; return 400; }
    on = on_text == "1";
    return 0;
}

inline int check_set_fan(const Caps& caps, const std::string& fan, const std::string& percent_text, int& percent, std::string& why)
{
    if (!find_fan(caps, fan)) {
        why = fan.empty() ? "fan is required (as controls.fans lists them)" : "this printer has no fan called " + fan;
        return fan.empty() ? 400 : 404;
    }
    if (!parse_int(percent_text, 0, 100, percent)) { why = "percent must be 0..100"; return 400; }
    return 0;
}

// Klipper's own name for a heater id: heater_bed, extruder, extruder1, ...; empty for one it has not.
inline std::string klipper_heater(const std::string& heater_id)
{
    if (heater_id == "bed") return "heater_bed";
    const int n = nozzle_index_of(heater_id);
    if (n < 0) return std::string();
    return n == 0 ? std::string("extruder") : "extruder" + std::to_string(n);
}

// The G-code a Klipper printer is sent for each control - what the Snapmaker app's own device page
// sends for a toolhead (SET_HEATER_TEMPERATURE HEATER=extruder<N> TARGET=<t>), and the standard
// Klipper commands for the rest. Empty when the control has no Klipper spelling.
inline std::string moonraker_temp_script(const std::string& heater_id, int target)
{
    const std::string heater = klipper_heater(heater_id);
    if (heater.empty()) return std::string();
    return "SET_HEATER_TEMPERATURE HEATER=" + heater + " TARGET=" + std::to_string(target);
}

inline std::string moonraker_speed_script(int percent) { return "M220 S" + std::to_string(percent); }

inline std::string moonraker_light_script(bool on) { return std::string("SET_LED LED=cavity_led WHITE=") + (on ? "1" : "0"); }

inline std::string moonraker_fan_script(const std::string& fan_id, int percent)
{
    if (fan_id == "part") return "M106 S" + std::to_string((int) std::lround(percent * 255.0 / 100.0));
    if (fan_id == "cavity") {
        char buf[64];
        std::snprintf(buf, sizeof buf, "SET_FAN_SPEED FAN=cavity_fan SPEED=%.2f", percent / 100.0);
        return buf;
    }
    return std::string();
}

// Bambu: the desktop's fan popup works in ten steps and sends floor(step * 25.5) through
// command_control_fan_val (Widgets/FanControl.cpp). A percentage is rounded to the nearest step
// first, so 50 % sends what the popup's fifth step sends.
inline int bambu_fan_value(int percent)
{
    int step = (int) std::lround(percent / 10.0);
    if (step < 0) step = 0;
    if (step > 10) step = 10;
    return (int) std::floor(step * 25.5f);
}

// MachineObject::FanType: COOLING_FAN = 1 (part), BIG_COOLING_FAN = 2 (aux), CHAMBER_FAN = 3.
inline int bambu_fan_type(const std::string& fan_id)
{
    if (fan_id == "part") return 1;
    if (fan_id == "aux") return 2;
    if (fan_id == "chamber") return 3;
    return 0;
}

}}} // namespace Slic3r::GUI::DeviceControls

#endif // slic3r_GUI_DeviceControls_hpp_
