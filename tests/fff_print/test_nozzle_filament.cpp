#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// Filament presets follow the nozzle size of their tool head (resolution: test_nozzle_filament_presets.cpp).
// Slices check that a slot with the 0.2 mm preset prints with its values next to a slot with the
// 0.4 mm preset of the same material; expected values are read from the two shipped presets.

namespace {

const char *const U1_MACHINE = "Snapmaker U1 (0.4 nozzle)";
const char *const U1_PROCESS = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)";
const char *const PLA_02     = "Generic PLA @U1 0.2 nozzle";
const char *const PLA_04     = "Generic PLA";

// A U1 plate with a 0.2 mm nozzle on tool head 1 and 0.4 mm nozzles on the others, filament i on
// tool head i, the two given presets in the first two slots, the prime tower on.
DynamicPrintConfig u1_mixed_project_config(PresetBundle &bundle, const std::string &first_slot, const std::string &second_slot)
{
    bundle.load_vendor_configs_from_json(PROFILES_DIR, "Snapmaker", PresetBundle::LoadSystem,
                                         ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr, /*allow_cache=*/false);
    REQUIRE(bundle.printers.select_preset_by_name(U1_MACHINE, true));
    REQUIRE(bundle.prints.select_preset_by_name(U1_PROCESS, true));
    bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = {0.2, 0.4, 0.4, 0.4};
    bundle.filament_presets = {first_slot, second_slot, PLA_04, PLA_04};
    REQUIRE(bundle.filaments.select_preset_by_name(bundle.filament_presets.front(), true));
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2, 3, 4};

    DynamicPrintConfig config = bundle.full_config(false);
    // The application sizes the colours and the flush volumes with the filament list.
    config.set_key_value("filament_colour",      new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}));
    config.set_key_value("flush_multiplier",     new ConfigOptionFloats({1.}));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(std::vector<double>(16, 0.)));
    config.set_key_value("enable_support", new ConfigOptionBool(false));
    config.set_key_value("skirt_loops",    new ConfigOptionInt(0));
    // Layers both nozzle sizes can print.
    config.set_key_value("layer_height",               new ConfigOptionFloat(0.1));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.1));
    // The cooling slowdown would rewrite the feed rates under test.
    config.set_key_value("slow_down_for_layer_cooling",
                         new ConfigOptionBools(std::vector<unsigned char>(config.option<ConfigOptionBools>("slow_down_for_layer_cooling")->size(), 0)));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
    config.set_key_value("wipe_tower_x", new ConfigOptionFloats({20.})); // clear of the cubes
    config.set_key_value("wipe_tower_y", new ConfigOptionFloats({20.}));
    return config;
}

// The first (Standard) column of a value of a shipped filament preset.
template<class Option> auto shipped_value(PresetBundle &bundle, const std::string &preset_name, const std::string &key)
{
    const Preset *preset = bundle.filaments.find_preset(preset_name, false, true);
    REQUIRE(preset != nullptr);
    const auto *option = preset->config.option<Option>(key);
    REQUIRE(option != nullptr);
    REQUIRE_FALSE(option->values.empty());
    return option->values.front();
}

double word_value(const std::string &line, char word)
{
    const std::string command = line.substr(0, line.find(';'));
    for (size_t pos = command.find(word); pos != std::string::npos; pos = command.find(word, pos + 1))
        if (pos > 0 && command[pos - 1] == ' ' && pos + 1 < command.size())
            return std::atof(command.c_str() + pos + 1);
    return -1.;
}

struct ToolFlow {
    double max_wall_flow  = 0.; // mm3/s over wall moves of at least 2 mm
    double max_tower_flow = 0.; // mm3/s over prime tower moves of at least 1 mm
};

// Two cubes next to each other, the first on tool head 1, the second on tool head 2.
std::map<int, ToolFlow> sliced_tool_flows(const DynamicPrintConfig &config)
{
    REQUIRE(config.opt_bool("use_relative_e_distances"));

    TriangleMesh first = mesh(TestMesh::cube_20x20x20);
    first.scale(Vec3f(1.f, 1.f, 0.1f)); // 2 mm
    first.translate(100.f, 100.f, 0.f);
    TriangleMesh second = mesh(TestMesh::cube_20x20x20);
    second.scale(Vec3f(1.f, 1.f, 0.1f));
    second.translate(140.f, 100.f, 0.f);
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides = {{{"extruder", "1"}}, {{"extruder", "2"}}};
    std::vector<TriangleMesh> meshes;
    meshes.emplace_back(std::move(first));
    meshes.emplace_back(std::move(second));

    Print print;
    Model model;
    init_print(std::move(meshes), print, model, config, &overrides, /*arrange=*/false);
    {
        const StringObjectException err = print.validate();
        INFO(err.string);
        REQUIRE(err.string.empty());
    }
    const std::string text  = gcode(print);
    const size_t      begin = text.find("; EXECUTABLE_BLOCK_START");
    const size_t      end   = text.find("; EXECUTABLE_BLOCK_END");
    REQUIRE(begin != std::string::npos);
    REQUIRE(end != std::string::npos);

    const double filament_diameter = config.option<ConfigOptionFloats>("filament_diameter")->get_at(0);
    const double filament_area     = M_PI * filament_diameter * filament_diameter / 4.;

    std::map<int, ToolFlow> flows;
    std::istringstream      in(text.substr(begin, end - begin));
    std::string             line;
    int                     tool = 0;
    bool                    wall = false, tower = false;
    double                  feedrate = 0., x = 0., y = 0.;
    while (std::getline(in, line)) {
        if (line.rfind(";TYPE:", 0) == 0 || line.rfind("; FEATURE:", 0) == 0) {
            wall  = line.find("wall") != std::string::npos;
            tower = line.find("Prime tower") != std::string::npos;
            continue;
        }
        if (line.size() >= 2 && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1])) && line.find_first_not_of("0123456789", 1) == std::string::npos) {
            tool = std::atoi(line.c_str() + 1);
            continue;
        }
        if (line.rfind("G1 ", 0) == 0 || line.rfind("G0 ", 0) == 0) {
            const double f = word_value(line, 'F');
            if (f > 0.)
                feedrate = f;
            const double new_x  = word_value(line, 'X') >= 0. ? word_value(line, 'X') : x;
            const double new_y  = word_value(line, 'Y') >= 0. ? word_value(line, 'Y') : y;
            const double length = std::hypot(new_x - x, new_y - y);
            const double e      = word_value(line, 'E');
            if (e > 0. && length > 0.) {
                const double flow = e * filament_area / length * feedrate / 60.;
                if (wall && length >= 2.)
                    flows[tool].max_wall_flow = std::max(flows[tool].max_wall_flow, flow);
                if (tower && length >= 1.)
                    flows[tool].max_tower_flow = std::max(flows[tool].max_tower_flow, flow);
            }
            x = new_x;
            y = new_y;
        }
    }
    return flows;
}

struct PresetValues {
    double max_flow;     // filament_max_volumetric_speed
    double ramming_flow; // filament_multitool_ramming_flow
};

PresetValues shipped_values(PresetBundle &bundle, const std::string &preset_name)
{
    REQUIRE(shipped_value<ConfigOptionBools>(bundle, preset_name, "filament_multitool_ramming"));
    return { shipped_value<ConfigOptionFloats>(bundle, preset_name, "filament_max_volumetric_speed"),
             shipped_value<ConfigOptionFloats>(bundle, preset_name, "filament_multitool_ramming_flow") };
}

} // namespace

TEST_CASE("A filament slot prints with the values of the nozzle size preset it holds", "[NozzleFilament][Profiles]")
{
    auto bundle = std::make_unique<PresetBundle>();

    SECTION("the 0.2 mm tool head holds the preset for 0.2 mm, the 0.4 mm tool head the preset for 0.4 mm") {
        const DynamicPrintConfig config = u1_mixed_project_config(*bundle, PLA_02, PLA_04);
        const PresetValues       small  = shipped_values(*bundle, PLA_02);
        const PresetValues       home   = shipped_values(*bundle, PLA_04);
        // Else the presets cannot tell the slots apart.
        REQUIRE(home.max_flow > small.max_flow * 2.);
        REQUIRE(home.ramming_flow > small.ramming_flow * 2.);

        std::map<int, ToolFlow> flows = sliced_tool_flows(config);
        // Walls: tool head 1 runs into the cap of the 0.2 mm preset, tool head 2 prints past it
        // and stays under its own.
        REQUIRE(flows[0].max_wall_flow > 0.);
        CHECK(flows[0].max_wall_flow < small.max_flow * 1.05);
        CHECK(flows[1].max_wall_flow > small.max_flow * 1.2);
        CHECK(flows[1].max_wall_flow < home.max_flow * 1.05);
        // Prime tower: each tool head rams with the flow of its own preset.
        CHECK_THAT(flows[0].max_tower_flow, Catch::Matchers::WithinRel(small.ramming_flow, 0.05));
        CHECK_THAT(flows[1].max_tower_flow, Catch::Matchers::WithinRel(home.ramming_flow, 0.05));
    }

    SECTION("the presets swapped: the values swap with them") {
        const DynamicPrintConfig config = u1_mixed_project_config(*bundle, PLA_04, PLA_02);
        const PresetValues       small  = shipped_values(*bundle, PLA_02);
        const PresetValues       home   = shipped_values(*bundle, PLA_04);

        std::map<int, ToolFlow> flows = sliced_tool_flows(config);
        REQUIRE(flows[1].max_wall_flow > 0.);
        CHECK(flows[1].max_wall_flow < small.max_flow * 1.05);
        CHECK(flows[0].max_wall_flow > small.max_flow * 1.2);
        CHECK(flows[0].max_wall_flow < home.max_flow * 1.05);
        CHECK_THAT(flows[1].max_tower_flow, Catch::Matchers::WithinRel(small.ramming_flow, 0.05));
        CHECK_THAT(flows[0].max_tower_flow, Catch::Matchers::WithinRel(home.ramming_flow, 0.05));
    }
}
