#ifndef slic3r_GUI_ChamberLights_hpp_
#define slic3r_GUI_ChamberLights_hpp_

// The chamber light(s) of a Bambu printer, kept free of wx, DeviceManager and the network so the
// commands and the state reading can be unit tested on their own
// (tests/slic3rutils/chamber_lights_tests.cpp).
//
// The H2 series (H2D, H2C, H2S - "series_o" in resources/printers) has two interior lights. They are
// two nodes of the printer's `ledctrl` system command, "chamber_light" and "chamber_light2", and the
// printer reports each one in the push_status `lights_report` array. Bambu Studio switches both (its
// DevLamp::CtrlSetChamberLight sends one ledctrl per node), but only ever reads the first node back.
// Sending one command only switched one lamp - the report from an H2 owner, 2026-10-02.
//
// Single-light printers (X1, P1, A1) are unchanged: one command, to "chamber_light".

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace ChamberLights {

enum class Mode { On, Off, Flashing, Unknown };

inline Mode parse_mode(const std::string& s)
{
    if (s == "on") return Mode::On;
    if (s == "off") return Mode::Off;
    if (s == "flashing") return Mode::Flashing;
    return Mode::Unknown;
}

inline const char* mode_str(Mode m)
{
    switch (m) {
    case Mode::On: return "on";
    case Mode::Off: return "off";
    case Mode::Flashing: return "flashing";
    default: return "unknown";
    }
}

constexpr const char* NODE_CHAMBER  = "chamber_light";
constexpr const char* NODE_CHAMBER2 = "chamber_light2";
constexpr const char* NODE_WORK     = "work_light";

// What a push_status `lights_report` says, node by node. `has_chamber2` is true when the printer
// reported a "chamber_light2" node at all, which is how a second light announces itself.
struct Report
{
    Mode chamber { Mode::Unknown };
    Mode chamber2 { Mode::Unknown };
    Mode work { Mode::Unknown };
    bool has_chamber { false };
    bool has_chamber2 { false };
    bool has_work { false };
};

// Reads one `lights_report` array ([{"node": "chamber_light", "mode": "on"}, ...]). Anything that is
// not an array, or an entry without a string node and mode, is skipped.
inline Report parse_lights_report(const nlohmann::json& lights_report)
{
    Report r;
    if (!lights_report.is_array()) return r;
    for (const nlohmann::json& e : lights_report) {
        if (!e.is_object() || !e.contains("node") || !e["node"].is_string() || !e.contains("mode") || !e["mode"].is_string())
            continue;
        const std::string node = e["node"].get<std::string>();
        const Mode        mode = parse_mode(e["mode"].get<std::string>());
        if (node == NODE_CHAMBER) { r.chamber = mode; r.has_chamber = true; }
        else if (node == NODE_CHAMBER2) { r.chamber2 = mode; r.has_chamber2 = true; }
        else if (node == NODE_WORK) { r.work = mode; r.has_work = true; }
    }
    return r;
}

// Whether a printer has two chamber lights: the H2 series ("series_o"), or any printer that has
// reported a "chamber_light2" node.
inline bool has_two_lights(const std::string& printer_series, bool chamber2_reported)
{
    return chamber2_reported || printer_series == "series_o";
}

// The chamber light nodes a toggle is sent to.
inline std::vector<std::string> chamber_nodes(bool two_lights)
{
    return two_lights ? std::vector<std::string> { NODE_CHAMBER, NODE_CHAMBER2 } : std::vector<std::string> { NODE_CHAMBER };
}

// One `ledctrl` system command, the same shape Bambu Studio sends (DevLamp::command_set_chamber_light).
inline nlohmann::json ledctrl_command(const std::string& node, Mode mode, const std::string& sequence_id, int on_time = 500,
                                      int off_time = 500, int loops = 1, int interval = 1000)
{
    nlohmann::json j;
    j["system"]["command"]       = "ledctrl";
    j["system"]["led_node"]      = node;
    j["system"]["sequence_id"]   = sequence_id;
    j["system"]["led_mode"]      = mode_str(mode);
    j["system"]["led_on_time"]   = on_time;
    j["system"]["led_off_time"]  = off_time;
    j["system"]["loop_times"]    = loops;
    j["system"]["interval_time"] = interval;
    return j;
}

// The commands one chamber light toggle sends: a `ledctrl` per chamber light node, in order, each with
// its own sequence id from `next_sequence_id`.
inline std::vector<nlohmann::json> chamber_commands(bool two_lights, Mode mode, const std::function<std::string()>& next_sequence_id,
                                                    int on_time = 500, int off_time = 500, int loops = 1, int interval = 1000)
{
    std::vector<nlohmann::json> out;
    for (const std::string& node : chamber_nodes(two_lights))
        out.push_back(ledctrl_command(node, mode, next_sequence_id(), on_time, off_time, loops, interval));
    return out;
}

// The chamber light state to show for the whole printer. A single-light printer shows its one light.
// With two lights the printer reads as on when either one is, so the switch, which then turns both
// off, is never stuck showing "off" while a lamp is still lit; flashing outranks off, and unknown is
// only the answer when neither light has reported.
inline Mode combined(Mode chamber, Mode chamber2, bool two_lights)
{
    if (!two_lights) return chamber;
    if (chamber == Mode::On || chamber2 == Mode::On) return Mode::On;
    if (chamber == Mode::Flashing || chamber2 == Mode::Flashing) return Mode::Flashing;
    if (chamber == Mode::Off || chamber2 == Mode::Off) return Mode::Off;
    return Mode::Unknown;
}

}}} // namespace Slic3r::GUI::ChamberLights

#endif // slic3r_GUI_ChamberLights_hpp_
