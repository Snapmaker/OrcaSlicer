#ifndef slic3r_GUI_FilamentCommands_hpp_
#define slic3r_GUI_FilamentCommands_hpp_

// Loading and unloading filament from the phone: the Bambu AMS command the Device tab's Load /
// Unload buttons send, the Snapmaker U1 toolhead macros, and when each is on offer. Free of wx,
// DeviceManager and the network so it is unit tested on its own
// (tests/slic3rutils/filament_commands_tests.cpp), like DeviceControls.hpp.
//
// ---- Bambu ----------------------------------------------------------------------------------
// One MQTT command, "ams_change_filament" (MachineObject::command_ams_change_filament, which now
// builds its payload with ams_change_filament_json below, so the desktop and the phone cannot send
// different things):
//   load   {ams_id, target = ams*4 + slot for an AMS (ids 0..15), or the unit's own id for an AMS HT
//           (128+) and an external spool (254 / 255), slot_id, curr_temp, tar_temp}
//   unload {ams_id, target 255, slot_id 255}
// The temperatures are what StatusPanel::on_ams_load_curr computes: the middle of the loaded tray's
// nozzle range (curr_temp) and of the target tray's (tar_temp), -1 when a tray has none; an unload
// keeps the method's defaults (210 / 210), as StatusPanel::on_ams_unload does.
// External spools: 254 on a single-extruder printer is rewritten to 255 (its firmware's name for
// the one spool holder); on a two-extruder printer 254 is the left (deputy) spool and 255 the right
// (main) one and pass through unchanged - the mapping the dual AMS layout (PR #146) uses.
// The nozzle an AMS feeds is fixed by where it is plugged in, so no extruder id is sent; Bambu
// Studio only adds one for the Filament Track Switch accessory, which this fork does not support.
//
// ---- Snapmaker U1 ---------------------------------------------------------------------------
// ASSUMPTION, to verify on hardware: the U1's stock macros are not documented, and the Snapmaker
// app's own device page sends no load / unload command the hub could copy. The names below are the
// ones U1 owners use in their start / end G-code (forum.snapmaker.com t/42590):
//   unload  T<n> / INNER_FILAMENT_UNLOAD TEMP=<t> NOZZLE_DIAMETER=<d> / PARK_EXTRUDER
//   load    SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=<n> TEMP=140 / SM_PRINT_AUTO_FEED EXTRUDER=<n>
// So the hub does not assume them: a U1 offers load / unload only when its own G-code help
// (GET /printer/gcode/help) lists every command the script uses (u1_filament_macros_available).

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace FilamentCommands {

constexpr int BAMBU_EXT_LEFT  = 254; // VIRTUAL_TRAY_ID; the only external spool on a one-extruder printer
constexpr int BAMBU_EXT_RIGHT = 255;
constexpr int BAMBU_HT_FIRST  = 128; // AMS HT units report ids from here up
constexpr int UNLOAD_DEFAULT_TEMP = 210; // command_ams_change_filament's defaults, which unload keeps

inline int to_int(const std::string& s)
{
    if (s.empty()) return 0;
    return std::atoi(s.c_str());
}

// The "print" object of an ams_change_filament command, without its sequence_id.
// `ams_id` / `slot_id` as the Device tab holds them ("0".."3", "128", "254", "255"; slot "0".."3").
//
// The AMS range check is numeric. It used to be a string comparison (ams_id < "16"), under which
// "2".."9" compared greater than "16" and an AMS HT's "128" compared smaller, so a load from the
// third AMS sent target 2 instead of 8 + slot and an AMS HT load sent 512 + slot. Bambu Studio
// compares atoi(ams_id) < 16.
inline nlohmann::json ams_change_filament_json(bool load, std::string ams_id, const std::string& slot_id, int old_temp,
                                               int new_temp, bool multi_extruders)
{
    nlohmann::json p;
    int            tray_id = 0;
    const int      ams     = to_int(ams_id);
    if (ams < 16) tray_id = ams * 4 + to_int(slot_id);
    if (ams_id == std::to_string(BAMBU_EXT_LEFT) && !multi_extruders) ams_id = std::to_string(BAMBU_EXT_RIGHT);

    p["command"]   = "ams_change_filament";
    p["curr_temp"] = old_temp;
    p["tar_temp"]  = new_temp;
    p["ams_id"]    = to_int(ams_id);
    if (!load) {
        p["target"]  = 255;
        p["slot_id"] = 255; // the new protocol's "unload"
    } else {
        p["target"]  = tray_id == 0 ? to_int(ams_id) : tray_id;
        p["slot_id"] = to_int(slot_id);
    }
    return p;
}

// The middle of a tray's nozzle range, -1 when it has none (StatusPanel::on_ams_load_curr).
inline int tray_mid_temp(const std::string& min, const std::string& max)
{
    if (min.empty() || max.empty()) return -1;
    return (to_int(min) + to_int(max)) / 2;
}

// Whether a slot's Load / Unload is on offer, and why not when it is not.
struct Availability
{
    bool        can_load { false };
    bool        can_unload { false };
    std::string why; // empty when both are as the slot allows
};

// The printer as the phone's rules see it. A print in progress or paused takes neither (the owner's
// rule; the desktop allows a few cases while paused, which the phone leaves to the PC); neither
// does an AMS already changing filament or a flow calibration.
struct PrinterState
{
    bool printing { false };         // printing or paused
    bool changing_filament { false }; // Bambu ams_status_main == FILAMENT_CHANGE
    bool calibrating { false };      // Bambu extrusion calibration
    bool filament_at_extruder { true }; // Bambu hw_switch_state (unknown counts as yes, as the desktop does)
};

inline Availability availability(const PrinterState& printer, bool slot_exists, bool slot_loaded, bool flexible = false)
{
    Availability a;
    if (printer.printing) { a.why = "not while a print is running or paused"; return a; }
    if (printer.changing_filament) { a.why = "the AMS is already changing filament"; return a; }
    if (printer.calibrating) { a.why = "not during a calibration"; return a; }
    a.can_load   = slot_exists && !slot_loaded;
    a.can_unload = slot_loaded && printer.filament_at_extruder && !flexible;
    if (slot_loaded && flexible) a.why = "flexible filament is unloaded by hand";
    else if (!slot_exists) a.why = "the slot is empty";
    return a;
}

inline void write_availability(const Availability& a, nlohmann::json& j)
{
    j["can_load"]   = a.can_load;
    j["can_unload"] = a.can_unload;
    if (!a.why.empty()) j["filament_why"] = a.why;
}

// "0".."15" for an AMS slot, the unit id for an AMS HT or an external spool: what the printer's
// tray_now says while that slot feeds (non-np printers).
inline std::string tray_now_of(const std::string& ams_id, const std::string& slot_id)
{
    const int ams = to_int(ams_id);
    return ams < 16 ? std::to_string(ams * 4 + to_int(slot_id)) : ams_id;
}

// ---- U1 ----

inline bool is_flexible(const std::string& type)
{
    std::string t;
    for (char c : type) t += (char) std::toupper((unsigned char) c);
    return t.find("TPU") != std::string::npos || t.find("TPE") != std::string::npos || t.find("FLEX") != std::string::npos;
}

// A nozzle temperature that softens the filament enough to pull it: the printing temperature of the
// common materials plus a margin (the community's end G-code uses the slicer's nozzle temperature
// + 15). ASSUMPTION: the firmware's own unload may pick its own temperature.
inline int u1_unload_temp(const std::string& type)
{
    std::string t;
    for (char c : type) t += (char) std::toupper((unsigned char) c);
    if (t.find("PETG") != std::string::npos) return 250;
    if (t.find("ABS") != std::string::npos || t.find("ASA") != std::string::npos) return 260;
    if (t.find("PC") != std::string::npos || t.find("PA") != std::string::npos) return 270;
    if (t.find("PLA") != std::string::npos) return 220;
    return 230;
}

inline std::string u1_unload_script(int toolhead, int temp, double nozzle)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", nozzle > 0 ? nozzle : 0.4);
    return "T" + std::to_string(toolhead) + "\nINNER_FILAMENT_UNLOAD TEMP=" + std::to_string(temp) + " NOZZLE_DIAMETER=" + buf +
           "\nPARK_EXTRUDER";
}

inline std::string u1_load_script(int toolhead)
{
    const std::string n = std::to_string(toolhead);
    return "SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=" + n + " TEMP=140\nSM_PRINT_AUTO_FEED EXTRUDER=" + n;
}

// Every command the U1 scripts use.
inline const std::vector<std::string>& u1_filament_commands()
{
    static const std::vector<std::string> names { "INNER_FILAMENT_UNLOAD", "PARK_EXTRUDER", "SM_PRINT_EXTRUDER_PREHEAT",
                                                  "SM_PRINT_AUTO_FEED" };
    return names;
}

// GET /printer/gcode/help answers {"result": {"<COMMAND>": "<help>", ...}}; Klipper lists macros
// and extras' commands there, upper-case. True only when every command the scripts use is listed.
inline bool u1_filament_macros_available(const nlohmann::json& help)
{
    const nlohmann::json* r = &help;
    if (help.is_object() && help.contains("result")) r = &help["result"];
    if (!r->is_object()) return false;
    for (const std::string& name : u1_filament_commands()) {
        bool found = false;
        for (auto it = r->begin(); it != r->end() && !found; ++it) {
            std::string k;
            for (char c : it.key()) k += (char) std::toupper((unsigned char) c);
            found = k == name;
        }
        if (!found) return false;
    }
    return true;
}

}}} // namespace Slic3r::GUI::FilamentCommands

#endif // slic3r_GUI_FilamentCommands_hpp_
