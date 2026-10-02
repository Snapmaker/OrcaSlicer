#include <catch2/catch.hpp>

#include "slic3r/GUI/ChamberLights.hpp"

#include <fstream>
#include <iterator>

using namespace Slic3r::GUI::ChamberLights;
using nlohmann::json;

namespace {
// What MachineObject::command_set_chamber_light publishes for one toggle.
std::vector<json> toggle(const std::string& printer_series, bool chamber2_reported, Mode mode, int first_seq = 100)
{
    int seq = first_seq;
    return chamber_commands(has_two_lights(printer_series, chamber2_reported), mode, [&seq] { return std::to_string(seq++); });
}

std::string printer_series_in_resources(const std::string& model_id)
{
    std::ifstream f(std::string(SLIC3R_TEST_RESOURCES_DIR) + "/printers/" + model_id + ".json");
    if (!f) return {};
    const json j = json::parse(f, nullptr, false);
    return j.is_object() ? j.value("printer_series", std::string()) : std::string();
}
} // namespace

TEST_CASE("An H2 light toggle sends one ledctrl to each of the two chamber lights", "[ChamberLights]")
{
    for (Mode mode : { Mode::On, Mode::Off }) {
        const std::vector<json> cmds = toggle("series_o", false, mode);
        REQUIRE(cmds.size() == 2);
        CHECK(cmds[0]["system"]["command"] == "ledctrl");
        CHECK(cmds[0]["system"]["led_node"] == "chamber_light");
        CHECK(cmds[1]["system"]["command"] == "ledctrl");
        CHECK(cmds[1]["system"]["led_node"] == "chamber_light2");
        for (const json& c : cmds) {
            CHECK(c["system"]["led_mode"] == (mode == Mode::On ? "on" : "off"));
            CHECK(c["system"]["led_on_time"] == 500);
            CHECK(c["system"]["led_off_time"] == 500);
            CHECK(c["system"]["loop_times"] == 1);
            CHECK(c["system"]["interval_time"] == 1000);
        }
        // Each command carries its own sequence id.
        CHECK(cmds[0]["system"]["sequence_id"] == "100");
        CHECK(cmds[1]["system"]["sequence_id"] == "101");
    }
}

TEST_CASE("H2D, H2C and H2S are all series_o and so all have two lights", "[ChamberLights]")
{
    for (const char* model : { "O1D", "O1C2", "O1S" }) {
        INFO(model);
        const std::string series = printer_series_in_resources(model);
        CHECK(series == "series_o");
        CHECK(has_two_lights(series, false));
        CHECK(toggle(series, false, Mode::Off).size() == 2);
    }
}

TEST_CASE("An X1C, P1 or A1 light toggle stays one ledctrl to chamber_light", "[ChamberLights]")
{
    for (const char* model : { "BL-P001", "C12", "N2S" }) {
        INFO(model);
        const std::string series = printer_series_in_resources(model);
        CHECK(series != "series_o");
        const std::vector<json> cmds = toggle(series, false, Mode::On);
        REQUIRE(cmds.size() == 1);
        CHECK(cmds[0]["system"]["led_node"] == "chamber_light");
        CHECK(cmds[0]["system"]["led_mode"] == "on");
        CHECK(cmds[0]["system"]["sequence_id"] == "100");
    }
    // An X1C with the series it ships: the single command is byte-for-byte what it always was.
    const json x1c = toggle("series_x1", false, Mode::Off)[0];
    CHECK(x1c == json::parse(R"({"system":{"command":"ledctrl","led_node":"chamber_light","sequence_id":"100",
        "led_mode":"off","led_on_time":500,"led_off_time":500,"loop_times":1,"interval_time":1000}})"));
}

TEST_CASE("A printer that reports a chamber_light2 node gets both commands whatever its series", "[ChamberLights]")
{
    CHECK(toggle("", true, Mode::On).size() == 2);
    CHECK(toggle("series_p1p", true, Mode::On).size() == 2);
    CHECK(toggle("", false, Mode::On).size() == 1);
}

TEST_CASE("lights_report with two lights is read node by node", "[ChamberLights]")
{
    const Report r = parse_lights_report(json::parse(R"([
        {"node": "chamber_light",  "mode": "on"},
        {"node": "chamber_light2", "mode": "off"},
        {"node": "work_light",     "mode": "flashing"}])"));
    CHECK(r.has_chamber);
    CHECK(r.chamber == Mode::On);
    CHECK(r.has_chamber2);
    CHECK(r.chamber2 == Mode::Off);
    CHECK(r.has_work);
    CHECK(r.work == Mode::Flashing);
}

TEST_CASE("lights_report from a single-light printer has no second light", "[ChamberLights]")
{
    const Report r = parse_lights_report(json::parse(R"([{"node": "chamber_light", "mode": "off"},
                                                          {"node": "work_light", "mode": "on"}])"));
    CHECK(r.chamber == Mode::Off);
    CHECK_FALSE(r.has_chamber2);
    CHECK(r.chamber2 == Mode::Unknown);
}

TEST_CASE("lights_report that is malformed is skipped, not thrown on", "[ChamberLights]")
{
    CHECK_FALSE(parse_lights_report(json::parse("{}")).has_chamber);
    CHECK_FALSE(parse_lights_report(json::parse("null")).has_chamber);
    const Report r = parse_lights_report(json::parse(R"([1, {"node": 5, "mode": "on"}, {"node": "chamber_light"},
                                                         {"node": "chamber_light2", "mode": "on"}])"));
    CHECK_FALSE(r.has_chamber);
    CHECK(r.has_chamber2);
    CHECK(r.chamber2 == Mode::On);
}

TEST_CASE("The state shown covers both lights on an H2", "[ChamberLights]")
{
    // Two lights: on when either is on, so the switch never reads off while a lamp is lit.
    CHECK(combined(Mode::On, Mode::On, true) == Mode::On);
    CHECK(combined(Mode::On, Mode::Off, true) == Mode::On);
    CHECK(combined(Mode::Off, Mode::On, true) == Mode::On);
    CHECK(combined(Mode::Off, Mode::Off, true) == Mode::Off);
    CHECK(combined(Mode::Off, Mode::Flashing, true) == Mode::Flashing);
    CHECK(combined(Mode::Unknown, Mode::Off, true) == Mode::Off);
    CHECK(combined(Mode::Unknown, Mode::Unknown, true) == Mode::Unknown);
    // One light: the second light's value is never looked at.
    CHECK(combined(Mode::Off, Mode::On, false) == Mode::Off);
    CHECK(combined(Mode::On, Mode::Unknown, false) == Mode::On);
    CHECK(combined(Mode::Unknown, Mode::On, false) == Mode::Unknown);
}

TEST_CASE("A push_status with a lit second light reads as on, then off once both are off", "[ChamberLights]")
{
    const Report lit = parse_lights_report(json::parse(R"([{"node": "chamber_light", "mode": "off"},
                                                            {"node": "chamber_light2", "mode": "on"}])"));
    CHECK(combined(lit.chamber, lit.chamber2, has_two_lights("series_o", lit.has_chamber2)) == Mode::On);
    const Report dark = parse_lights_report(json::parse(R"([{"node": "chamber_light", "mode": "off"},
                                                             {"node": "chamber_light2", "mode": "off"}])"));
    CHECK(combined(dark.chamber, dark.chamber2, has_two_lights("series_o", dark.has_chamber2)) == Mode::Off);
}
