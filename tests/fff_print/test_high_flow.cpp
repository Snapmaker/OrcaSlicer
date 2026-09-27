#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <regex>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_helpers.hpp"
#include "snapmaker_high_flow_fixture.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// High Flow on the Snapmaker U1: flow type per tool head (nozzle_volume_type); filament and process
// print with the column of that head's flow type. Checks the sliced G-code; column resolution itself
// is in tests/libslic3r/test_config_variant_expansion.cpp.

namespace {

const char *const STANDARD  = "Direct Drive Standard";
const char *const HIGH_FLOW = "Direct Drive High Flow";

constexpr size_t HEADS = 4;

// Column values chosen so a wall feed rate tells the column apart:
//  - Standard: 50 mm/s, 2 mm3/s max; a 0.2 mm layer with >= 0.3 mm lines caps it below 40 mm/s.
//  - High Flow: 120 mm/s, 40 mm3/s max, which caps nothing at this layer height.
constexpr double STANDARD_WALL_SPEED   = 50.;
constexpr double HIGH_FLOW_WALL_SPEED  = 120.;
constexpr double STANDARD_MAX_FLOW     = 2.;
constexpr double HIGH_FLOW_MAX_FLOW    = 40.;
constexpr double STANDARD_CAPPED_SPEED = 40.;
constexpr int    STANDARD_TEMPERATURE  = 215;
constexpr int    HIGH_FLOW_TEMPERATURE = 220;
// Pressure advance and fan speed differ per flow type and per filament, so a value read from the
// wrong column shows. Min and max fan speed of a column agree: one speed whatever the layer time.
double pressure_advance_of(size_t filament, NozzleVolumeType flow_type)
{
    return (flow_type == nvtHighFlow ? 0.03 : 0.02) + 0.002 * double(filament);
}
double fan_speed_of(size_t filament, NozzleVolumeType flow_type) // percent
{
    return (flow_type == nvtHighFlow ? 60. : 20.) + 20. * double(filament % 2);
}
// The minimal purge on the prime tower, mm3.
constexpr double MINIMAL_PURGE = 15.;

enum class ColumnLayout {
    // One column per tool head / process / filament: a printer that declares no flow variants.
    SingleColumn,
    // Machine: Standard and High Flow column per tool head (8). Process: flow-only layout (2).
    // Filaments: Standard and High Flow column each (2).
    FlowColumns,
};

template<class T> std::vector<T> repeated(const std::vector<T> &pattern, size_t times)
{
    std::vector<T> out;
    for (size_t i = 0; i < times; ++i)
        out.insert(out.end(), pattern.begin(), pattern.end());
    return out;
}

// The values of every filament in the layout of a filament key: the Standard column of a filament,
// then (with flow columns) its High Flow column.
std::vector<double> filament_columns(bool flow, double (*value_of)(size_t, NozzleVolumeType))
{
    std::vector<double> out;
    for (size_t filament = 0; filament < HEADS; ++filament) {
        out.emplace_back(value_of(filament, nvtStandard));
        if (flow)
            out.emplace_back(value_of(filament, nvtHighFlow));
    }
    return out;
}

// A U1-shaped printer: four tool heads with a 0.4 mm nozzle each, filament i on tool head i.
DynamicPrintConfig u1_shaped_config(ColumnLayout layout, const std::vector<NozzleVolumeType> &head_flow_types)
{
    REQUIRE(head_flow_types.size() == HEADS);
    const bool   flow    = layout == ColumnLayout::FlowColumns;
    const size_t columns = flow ? 2 : 1;

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("layer_height",               new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("enable_prime_tower",         new ConfigOptionBool(false));
    config.set_key_value("enable_support",             new ConfigOptionBool(false));
    config.set_key_value("use_relative_e_distances",   new ConfigOptionBool(false));
    config.set_key_value("skirt_loops",                new ConfigOptionInt(0));

    // Printer.
    config.set_key_value("nozzle_diameter",       new ConfigOptionFloats(std::vector<double>(HEADS, 0.4)));
    config.set_key_value("extruder_layer_height", new ConfigOptionFloats(std::vector<double>(HEADS, 0.)));
    config.set_key_value("min_layer_height",      new ConfigOptionFloats(std::vector<double>(HEADS, 0.07)));
    config.set_key_value("max_layer_height",      new ConfigOptionFloats(std::vector<double>(HEADS, 0.3)));
    config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = std::vector<int>(HEADS, int(etDirectDrive));
    {
        std::vector<int> flow_types;
        for (NozzleVolumeType type : head_flow_types)
            flow_types.emplace_back(int(type));
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = flow_types;
    }
    config.set_key_value("extruder_variant_list",
                         new ConfigOptionStrings(std::vector<std::string>(HEADS, flow ? std::string(STANDARD) + "," + HIGH_FLOW : std::string(STANDARD))));
    // A machine value that is not the default, one per column; both columns of a head agree.
    config.set_key_value("retraction_length", new ConfigOptionFloats(std::vector<double>(HEADS * columns, 1.5)));
    config.set_key_value("z_hop",             new ConfigOptionFloats(std::vector<double>(HEADS * columns, 0.)));

    // Process.
    config.set_key_value("print_extruder_id", new ConfigOptionInts(std::vector<int>(columns, 1)));
    config.set_key_value("print_extruder_variant",
                         new ConfigOptionStrings(flow ? std::vector<std::string>{STANDARD, HIGH_FLOW} : std::vector<std::string>{STANDARD}));
    const std::vector<double> wall_speed = flow ? std::vector<double>{STANDARD_WALL_SPEED, HIGH_FLOW_WALL_SPEED} :
                                                  std::vector<double>{STANDARD_WALL_SPEED};
    for (const char *key : {"outer_wall_speed", "inner_wall_speed", "sparse_infill_speed", "internal_solid_infill_speed", "top_surface_speed"})
        config.set_key_value(key, new ConfigOptionFloats(wall_speed));

    // Filaments.
    config.set_key_value("filament_diameter",       new ConfigOptionFloats(std::vector<double>(HEADS, 1.75)));
    config.set_key_value("filament_colour",         new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}));
    config.set_key_value("default_filament_colour", new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}));
    config.set_key_value("filament_type",           new ConfigOptionStrings(std::vector<std::string>(HEADS, "PLA")));
    config.set_key_value("filament_map",            new ConfigOptionInts({1, 2, 3, 4}));
    config.set_key_value("flush_multiplier",        new ConfigOptionFloats({1.}));
    config.set_key_value("flush_volumes_matrix",    new ConfigOptionFloats(std::vector<double>(HEADS * HEADS, 0.)));
    config.set_key_value("nozzle_temperature_range_low",  new ConfigOptionInts(std::vector<int>(HEADS, 190)));
    config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts(std::vector<int>(HEADS, 240)));
    // The cooling slowdown would rewrite the feed rates under test.
    config.set_key_value("slow_down_for_layer_cooling", new ConfigOptionBools(std::vector<unsigned char>(HEADS, 0)));
    // The fan runs from the first layer on and only at the speed of the filament's column.
    config.set_key_value("close_fan_the_first_x_layers", new ConfigOptionInts(std::vector<int>(HEADS, 0)));
    config.set_key_value("reduce_fan_stop_start_freq",   new ConfigOptionBools(std::vector<unsigned char>(HEADS, 1)));
    config.set_key_value("enable_overhang_bridge_fan",   new ConfigOptionBools(std::vector<unsigned char>(HEADS, 0)));
    config.set_key_value("enable_pressure_advance", new ConfigOptionBools(std::vector<unsigned char>(HEADS * columns, 1)));
    config.set_key_value("pressure_advance", new ConfigOptionFloats(filament_columns(flow, pressure_advance_of)));
    config.set_key_value("fan_min_speed",    new ConfigOptionFloats(filament_columns(flow, fan_speed_of)));
    config.set_key_value("fan_max_speed",    new ConfigOptionFloats(filament_columns(flow, fan_speed_of)));
    config.set_key_value("filament_minimal_purge_on_wipe_tower", new ConfigOptionFloats(std::vector<double>(HEADS * columns, MINIMAL_PURGE)));
    if (flow) {
        config.set_key_value("filament_extruder_variant", new ConfigOptionStrings(repeated<std::string>({STANDARD, HIGH_FLOW}, HEADS)));
        config.set_key_value("filament_self_index",       new ConfigOptionInts({1, 1, 2, 2, 3, 3, 4, 4}));
        config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats(repeated<double>({STANDARD_MAX_FLOW, HIGH_FLOW_MAX_FLOW}, HEADS)));
        config.set_key_value("nozzle_temperature",               new ConfigOptionInts(repeated<int>({STANDARD_TEMPERATURE, HIGH_FLOW_TEMPERATURE}, HEADS)));
        config.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts(repeated<int>({STANDARD_TEMPERATURE, HIGH_FLOW_TEMPERATURE}, HEADS)));
    } else {
        config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats(std::vector<double>(HEADS, STANDARD_MAX_FLOW)));
        config.set_key_value("nozzle_temperature",               new ConfigOptionInts(std::vector<int>(HEADS, STANDARD_TEMPERATURE)));
        config.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts(std::vector<int>(HEADS, STANDARD_TEMPERATURE)));
    }
    return config;
}

// Two 20 mm cubes next to each other, the first on tool head 1, the second on tool head 2.
// The cubes are z-scaled by z_scale.
void init_two_head_print(Print &print, Model &model, const DynamicPrintConfig &config, float z_scale)
{
    TriangleMesh first = mesh(TestMesh::cube_20x20x20);
    first.scale(Vec3f(1.f, 1.f, z_scale));
    first.translate(100.f, 100.f, 0.f);
    TriangleMesh second = mesh(TestMesh::cube_20x20x20);
    second.scale(Vec3f(1.f, 1.f, z_scale));
    second.translate(140.f, 100.f, 0.f);

    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides = {{{"extruder", "1"}}, {{"extruder", "2"}}};
    std::vector<TriangleMesh> meshes;
    meshes.emplace_back(std::move(first));
    meshes.emplace_back(std::move(second));
    init_print(std::move(meshes), print, model, config, &overrides, /*arrange=*/false);
}

void with_prime_tower(DynamicPrintConfig &config)
{
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
    config.set_key_value("wipe_tower_x", new ConfigOptionFloats({20.})); // clear of the cubes, inside the 200 x 200 test bed
    config.set_key_value("wipe_tower_y", new ConfigOptionFloats({20.}));
    // The prime tower needs relative extruder addressing, which in turn needs the E reset.
    config.set_key_value("use_relative_e_distances", new ConfigOptionBool(true));
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0"));
}

std::string two_head_gcode(const DynamicPrintConfig &config)
{
    Print print;
    Model model;
    init_two_head_print(print, model, config, 0.1f); // 2 mm = 10 layers of 0.2 mm
    {
        const StringObjectException err = print.validate();
        INFO(err.string);
        REQUIRE(err.string.empty());
    }
    return gcode(print);
}

// The part of the G-code a printer executes: header, thumbnails and the config block left out.
// Object labels carry an id from a process-wide counter ("id:23", "_id_23_"), which differs between
// two prints of one test run; the number is dropped.
std::string executable_block(const std::string &gcode)
{
    const size_t begin = gcode.find("; EXECUTABLE_BLOCK_START");
    const size_t end   = gcode.find("; EXECUTABLE_BLOCK_END");
    REQUIRE(begin != std::string::npos);
    REQUIRE(end != std::string::npos);
    static const std::regex object_id("id([:_])[0-9]+");
    return std::regex_replace(gcode.substr(begin, end - begin), object_id, "id$1#");
}

struct ToolFacts {
    double              max_wall_feedrate = 0.; // mm/min, extruding moves of outer and inner walls
    // The same per layer, keyed by the count of layer change tags before the move: a failure
    // message that tells the layers a tool printed walls on, and how fast, from one that lists
    // no walls at all above the first layer.
    std::map<int, double> max_wall_feedrate_per_layer;
    std::vector<int>    temperatures;           // every S value an M104 / M109 sets for this tool
    std::vector<double> pressure_advances;      // every value set while this tool is active
    std::vector<int>    wall_fan_speeds;        // the fan speed (0-255) in force when a wall move of this tool extrudes
};

double word_value(const std::string &line, char word)
{
    // The first "<word><number>" of the command part of the line.
    const std::string command = line.substr(0, line.find(';'));
    for (size_t pos = command.find(word); pos != std::string::npos; pos = command.find(word, pos + 1))
        if (pos > 0 && command[pos - 1] == ' ' && pos + 1 < command.size())
            return std::atof(command.c_str() + pos + 1);
    return -1.;
}

// Feed rates and temperatures per tool. A temperature command names its tool ("T1") or applies to
// the active one.
std::map<int, ToolFacts> tool_facts(const std::string &gcode)
{
    std::map<int, ToolFacts> facts;
    std::istringstream       in(executable_block(gcode));
    std::string              line;
    int                      tool     = 0;
    int                      layer    = 0;
    bool                     wall     = false;
    double                   feedrate = 0.;
    int                      fan      = 0;
    while (std::getline(in, line)) {
        if (line.rfind("M900 K", 0) == 0) {
            facts[tool].pressure_advances.emplace_back(std::atof(line.c_str() + 6));
            continue;
        }
        // The layer change tag of either tag set (";LAYER_CHANGE", "; CHANGE_LAYER").
        if (line.rfind(";LAYER_CHANGE", 0) == 0 || line.rfind("; CHANGE_LAYER", 0) == 0) {
            ++layer;
            continue;
        }
        if (line.rfind("M106", 0) == 0 && word_value(line, 'P') < 0.) {
            fan = int(word_value(line, 'S'));
            continue;
        }
        if (line.rfind("M107", 0) == 0) {
            fan = 0;
            continue;
        }
        if (line.rfind(";TYPE:", 0) == 0 || line.rfind("; FEATURE:", 0) == 0) {
            wall = line.find("wall") != std::string::npos;
            continue;
        }
        if (line.size() >= 2 && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1]))) {
            tool = std::atoi(line.c_str() + 1);
            continue;
        }
        if (line.rfind("M104", 0) == 0 || line.rfind("M109", 0) == 0) {
            const double s = word_value(line, 'S');
            const double t = word_value(line, 'T');
            if (s > 0.)
                facts[t >= 0. ? int(t) : tool].temperatures.emplace_back(int(s));
            continue;
        }
        if (line.rfind("G1 ", 0) == 0) {
            const double f = word_value(line, 'F');
            if (f > 0.)
                feedrate = f;
            const bool moves = word_value(line, 'X') >= 0. || word_value(line, 'Y') >= 0.;
            if (wall && moves && word_value(line, 'E') > 0.) {
                facts[tool].max_wall_feedrate = std::max(facts[tool].max_wall_feedrate, feedrate);
                double &layer_max = facts[tool].max_wall_feedrate_per_layer[layer];
                layer_max         = std::max(layer_max, feedrate);
                if (facts[tool].wall_fan_speeds.empty() || facts[tool].wall_fan_speeds.back() != fan)
                    facts[tool].wall_fan_speeds.emplace_back(fan);
            }
        }
    }
    return facts;
}

// "T0: L1=1800 L2=2400 ...; T1: ..." - the maximum wall feed rate (mm/min) of every tool on every
// layer it printed walls on, for the message of a failed speed assertion.
std::string wall_feedrates_by_layer(const std::map<int, ToolFacts> &facts)
{
    std::ostringstream out;
    for (const auto &[tool, tool_facts] : facts) {
        out << "T" << tool << ":";
        for (const auto &[layer, feedrate] : tool_facts.max_wall_feedrate_per_layer)
            out << " L" << layer << "=" << feedrate;
        out << ";";
    }
    return out.str();
}

// Writes a G-code and the reference it was compared with to the system temp directory and names
// both files. The texts are too long for an assertion message, and the slice cannot be repeated
// afterwards, so a mismatch that is not reproduced on a rerun leaves its evidence here.
std::string keep_mismatch(const std::string &label, const std::string &reference, const std::string &candidate)
{
    namespace fs = boost::filesystem;
    const fs::path dir            = fs::temp_directory_path();
    const fs::path reference_path = dir / fs::unique_path("orca-prime-tower-reference-%%%%.gcode");
    const fs::path candidate_path = dir / fs::unique_path("orca-prime-tower-" + label + "-%%%%.gcode");
    {
        std::ofstream out(reference_path.string());
        out << reference;
    }
    {
        std::ofstream out(candidate_path.string());
        out << candidate;
    }
    return label + ": " + candidate_path.string() + " against " + reference_path.string();
}

} // namespace

TEST_CASE("A High Flow tool head prints its filament and its walls with the High Flow columns", "[HighFlow]")
{
    // One tool head carries a High Flow nozzle, the other heads Standard ones. Filament i owns the
    // columns 2i (Standard) and 2i + 1 (High Flow), so with the High Flow nozzle on the first head
    // no printing filament finds its value at the position of its own id.
    const int high_flow_tool = GENERATE(0, 1);
    const int standard_tool  = 1 - high_flow_tool;
    CAPTURE(high_flow_tool);
    std::vector<NozzleVolumeType> head_flow_types(HEADS, nvtStandard);
    head_flow_types[size_t(high_flow_tool)] = nvtHighFlow;

    const DynamicPrintConfig config = u1_shaped_config(ColumnLayout::FlowColumns, head_flow_types);
    const std::string        gcode  = two_head_gcode(config);
    std::map<int, ToolFacts> facts  = tool_facts(gcode);

    SECTION("each tool head heats to the temperature of its own column") {
        REQUIRE_FALSE(facts[standard_tool].temperatures.empty());
        REQUIRE_FALSE(facts[high_flow_tool].temperatures.empty());
        for (int temperature : facts[standard_tool].temperatures)
            CHECK(temperature == STANDARD_TEMPERATURE);
        for (int temperature : facts[high_flow_tool].temperatures)
            CHECK(temperature == HIGH_FLOW_TEMPERATURE);
    }

    SECTION("the Standard head stays below the Standard flow cap, the High Flow head reaches the High Flow wall speed") {
        // Printed on failure only. The first layer prints at initial_layer_speed (30 mm/s, F1800)
        // and is expected below both bounds; the layers above it decide the assertions.
        INFO("max wall feed rate (mm/min) per tool and layer: " << wall_feedrates_by_layer(facts));
        REQUIRE(facts[standard_tool].max_wall_feedrate > 0.);
        CHECK(facts[standard_tool].max_wall_feedrate < STANDARD_CAPPED_SPEED * 60.);
        CHECK_THAT(facts[high_flow_tool].max_wall_feedrate, Catch::Matchers::WithinAbs(HIGH_FLOW_WALL_SPEED * 60., 1.));
    }

    SECTION("each tool head gets the pressure advance of its own filament and column") {
        REQUIRE_FALSE(facts[standard_tool].pressure_advances.empty());
        REQUIRE_FALSE(facts[high_flow_tool].pressure_advances.empty());
        for (double pressure_advance : facts[standard_tool].pressure_advances)
            CHECK_THAT(pressure_advance, Catch::Matchers::WithinAbs(pressure_advance_of(size_t(standard_tool), nvtStandard), 1e-6));
        for (double pressure_advance : facts[high_flow_tool].pressure_advances)
            CHECK_THAT(pressure_advance, Catch::Matchers::WithinAbs(pressure_advance_of(size_t(high_flow_tool), nvtHighFlow), 1e-6));
    }

    SECTION("the walls of each tool head are cooled with the fan speed of its own filament and column") {
        // The cooling filter sets the fan when a tool becomes active, so every wall of a head is
        // printed under the one fan speed of its column.
        auto fan_command = [](double percent) { return int(percent * 255. / 100. + 0.5); };
        CHECK(facts[standard_tool].wall_fan_speeds == std::vector<int>{fan_command(fan_speed_of(size_t(standard_tool), nvtStandard))});
        CHECK(facts[high_flow_tool].wall_fan_speeds == std::vector<int>{fan_command(fan_speed_of(size_t(high_flow_tool), nvtHighFlow))});
    }
}

TEST_CASE("The prime tower purges a filament by the column of the tool head that prints it", "[HighFlow]")
{
    // Tool head 1 carries the High Flow nozzle: its filament owns the columns 0 (Standard) and
    // 1 (High Flow), the filament of the Standard head 2 the columns 2 and 3. A column that no
    // printing head selects must not reach the G-code, a selected one must.
    const std::vector<NozzleVolumeType> head_flow_types = {nvtHighFlow, nvtStandard, nvtStandard, nvtStandard};
    constexpr double LARGE_PURGE = 120.; // mm3, against MINIMAL_PURGE everywhere else

    auto tower_gcode = [&head_flow_types](int raised_column) {
        DynamicPrintConfig config = u1_shaped_config(ColumnLayout::FlowColumns, head_flow_types);
        with_prime_tower(config);
        if (raised_column >= 0)
            config.option<ConfigOptionFloats>("filament_minimal_purge_on_wipe_tower")->values[size_t(raised_column)] = LARGE_PURGE;
        return executable_block(two_head_gcode(config));
    };

    const std::string reference = tower_gcode(-1);
    REQUIRE(reference.find("\nT0") != std::string::npos);
    REQUIRE(reference.find("\nT1") != std::string::npos);

    // Compared as bools: a mismatch would otherwise print two whole G-code files. A G-code that
    // differs where it must not is kept on disk instead (see keep_mismatch).
    const std::string unselected_standard_column_raised  = tower_gcode(0);
    const std::string unselected_high_flow_column_raised = tower_gcode(3);
    const bool unselected_standard_column_ignored  = unselected_standard_column_raised == reference;
    const bool unselected_high_flow_column_ignored = unselected_high_flow_column_raised == reference;
    const bool selected_high_flow_column_read      = tower_gcode(1) != reference;
    const bool selected_standard_column_read       = tower_gcode(2) != reference;
    std::string kept;
    if (!unselected_standard_column_ignored)
        kept += keep_mismatch("unselected-standard-column-0", reference, unselected_standard_column_raised);
    if (!unselected_high_flow_column_ignored)
        kept += (kept.empty() ? "" : "; ") + keep_mismatch("unselected-high-flow-column-3", reference, unselected_high_flow_column_raised);
    INFO("G-code kept for a mismatch: " << (kept.empty() ? std::string("none") : kept));
    CHECK(unselected_standard_column_ignored);
    CHECK(unselected_high_flow_column_ignored);
    CHECK(selected_high_flow_column_read);
    CHECK(selected_standard_column_read);
}

TEST_CASE("Switching a tool head to High Flow leaves the per-extruder layer plan alone", "[HighFlow][MultiNozzleLayerHeight]")
{
    // Object layers of 0.12 mm; tool head 2 prefers 0.24 mm, i.e. runs of two object layers.
    // The flow type of a head selects value columns only, the layer plan reads none of them.
    struct LayerFacts {
        double             print_z;
        double             height;
        std::vector<bool>  region_prints; // per region: does it extrude on this layer
    };
    auto layer_plan = [](NozzleVolumeType second_head) {
        DynamicPrintConfig config = u1_shaped_config(ColumnLayout::FlowColumns, {nvtStandard, second_head, nvtStandard, nvtStandard});
        config.set_key_value("layer_height",               new ConfigOptionFloat(0.12));
        config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.24));
        config.set_key_value("extruder_layer_height",      new ConfigOptionFloats({0., 0.24, 0., 0.}));
        Print print;
        Model model;
        init_two_head_print(print, model, config, 0.12f); // 2.4 mm = 0.24 + 18 x 0.12
        {
            const StringObjectException err = print.validate();
            INFO(err.string);
            REQUIRE(err.string.empty());
        }
        print.process();
        std::vector<std::vector<LayerFacts>> plan;
        for (const PrintObject *object : print.objects()) {
            plan.emplace_back();
            for (const Layer *layer : object->layers()) {
                LayerFacts facts{layer->print_z, layer->height, {}};
                for (const LayerRegion *region : layer->regions())
                    facts.region_prints.emplace_back(!region->perimeters.empty() || !region->fills.empty());
                plan.back().emplace_back(std::move(facts));
            }
        }
        return plan;
    };

    const auto standard  = layer_plan(nvtStandard);
    const auto high_flow = layer_plan(nvtHighFlow);

    REQUIRE(standard.size() == 2);
    REQUIRE(high_flow.size() == standard.size());
    for (size_t object = 0; object < standard.size(); ++object) {
        REQUIRE(high_flow[object].size() == standard[object].size());
        for (size_t layer = 0; layer < standard[object].size(); ++layer) {
            INFO("object " << object << ", layer " << layer);
            CHECK_THAT(high_flow[object][layer].print_z, Catch::Matchers::WithinAbs(standard[object][layer].print_z, EPSILON));
            CHECK_THAT(high_flow[object][layer].height, Catch::Matchers::WithinAbs(standard[object][layer].height, EPSILON));
            CHECK(high_flow[object][layer].region_prints == standard[object][layer].region_prints);
        }
    }

    // The plan under test is a real one: the second cube prints in runs, so some of its layers
    // carry no extrusion, while the first cube prints on every layer.
    auto printing_layers = [](const std::vector<LayerFacts> &layers) {
        return std::count_if(layers.begin(), layers.end(), [](const LayerFacts &facts) {
            return std::find(facts.region_prints.begin(), facts.region_prints.end(), true) != facts.region_prints.end();
        });
    };
    CHECK(size_t(printing_layers(standard[0])) == standard[0].size());
    CHECK(size_t(printing_layers(standard[1])) < standard[1].size());
}

TEST_CASE("With every tool head on Standard the flow columns do not change the G-code", "[HighFlow][hf_all_standard_gcode_unchanged]")
{
    // The same printer, process and filaments written once with one column each (the layout
    // without High Flow) and once with Standard and High Flow columns. No head is
    // switched, so the second columns must stay unread.
    const std::vector<NozzleVolumeType> all_standard(HEADS, nvtStandard);
    DynamicPrintConfig single_column = u1_shaped_config(ColumnLayout::SingleColumn, all_standard);
    DynamicPrintConfig flow_columns  = u1_shaped_config(ColumnLayout::FlowColumns, all_standard);
    // The prime tower reads per-filament and per-head values of its own; keep it in.
    with_prime_tower(single_column);
    with_prime_tower(flow_columns);

    const std::string before = executable_block(two_head_gcode(single_column));
    const std::string after  = executable_block(two_head_gcode(flow_columns));

    // Both tool heads print, so tool changes and the prime tower are part of the comparison.
    REQUIRE(before.find("\nT0") != std::string::npos);
    REQUIRE(before.find("\nT1") != std::string::npos);
    // Compared as a bool first: a mismatch would otherwise print two whole G-code files.
    const bool identical = before == after;
    if (!identical) {
        std::istringstream a(before), b(after);
        std::string        line_a, line_b;
        size_t             number = 0;
        while (std::getline(a, line_a) && std::getline(b, line_b)) {
            ++number;
            if (line_a != line_b) {
                FAIL_CHECK("first difference in line " << number << " of the executable block: \"" << line_a << "\" / \"" << line_b << "\"");
                break;
            }
        }
    }
    CHECK(identical);
}

// ---------------------------------------------------------------------------------------------
// The shipped Snapmaker U1 presets, composed the way the application composes a project.
// ---------------------------------------------------------------------------------------------

namespace {

const char *const U1_MACHINE  = "Snapmaker U1 (0.4 nozzle)";
const char *const U1_PROCESS  = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)";
// Tool heads 1 and 2 print the same filament preset, so every difference between the two tools
// in the G-code comes from the flow type of the head.
const char *const U1_FILAMENT = "Snapmaker PLA Matte @U1";

// The presets of a U1 plate: filament i on tool head i.
struct U1Plate {
    const char              *machine;
    const char              *process;
    std::vector<std::string> filaments;
};

const U1Plate U1_PLATE_0_4 = {U1_MACHINE, U1_PROCESS, {U1_FILAMENT, U1_FILAMENT, "Snapmaker PLA SnapSpeed @U1", "Snapmaker PETG HF"}};

// The un-narrowed config (as handed to Print::apply()) of a U1 project with the given plate, head
// flow types and nozzle sizes (empty: the preset's); `vendor_fixture` edits the loaded vendor first.
DynamicPrintConfig u1_plate_config(PresetBundle &bundle, const U1Plate &plate, const std::vector<NozzleVolumeType> &head_flow_types,
                                   const std::vector<double> &diameters = {}, const std::function<void(PresetBundle &)> &vendor_fixture = {})
{
    bundle.load_vendor_configs_from_json(PROFILES_DIR, "Snapmaker", PresetBundle::LoadSystem,
                                         ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr, /*allow_cache=*/false);
    if (vendor_fixture)
        vendor_fixture(bundle);
    REQUIRE(bundle.printers.select_preset_by_name(plate.machine, true));
    if (!diameters.empty()) {
        // As the sidebar sets a tool head's size: on the edited printer preset.
        REQUIRE(diameters.size() == HEADS);
        bundle.printers.get_edited_preset().config.set_key_value("nozzle_diameter", new ConfigOptionFloats(diameters));
    }
    REQUIRE(bundle.prints.select_preset_by_name(plate.process, true));
    REQUIRE(plate.filaments.size() == HEADS);
    bundle.filament_presets = plate.filaments;
    REQUIRE(bundle.filaments.select_preset_by_name(bundle.filament_presets.front(), true));
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2, 3, 4};
    std::vector<int> flow_types;
    for (NozzleVolumeType type : head_flow_types)
        flow_types.emplace_back(int(type));
    bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = flow_types;

    DynamicPrintConfig config = bundle.full_config(false);
    // The application sizes the colours and the flush volumes with the filament list; nothing is
    // flushed here.
    config.set_key_value("filament_colour",      new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}));
    config.set_key_value("flush_multiplier",     new ConfigOptionFloats({1.}));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(std::vector<double>(HEADS * HEADS, 0.)));
    config.set_key_value("enable_support", new ConfigOptionBool(false));
    config.set_key_value("skirt_loops",    new ConfigOptionInt(0));
    // The cooling slowdown would rewrite the feed rates under test.
    config.set_key_value("slow_down_for_layer_cooling",
                         new ConfigOptionBools(std::vector<unsigned char>(config.option<ConfigOptionBools>("slow_down_for_layer_cooling")->size(), 0)));
    return config;
}

// The shipped 0.4 plate of the cases below.
DynamicPrintConfig u1_project_config(PresetBundle &bundle, const std::vector<NozzleVolumeType> &head_flow_types)
{
    return u1_plate_config(bundle, U1_PLATE_0_4, head_flow_types);
}

// The Standard or High Flow column of a shipped filament preset.
template<class Option> auto preset_column(PresetBundle &bundle, const std::string &preset_name, const std::string &key, NozzleVolumeType flow_type)
{
    const Preset *preset = bundle.filaments.find_preset(preset_name, false, true);
    REQUIRE(preset != nullptr);
    const auto *variants = preset->config.option<ConfigOptionStrings>("filament_extruder_variant");
    REQUIRE(variants != nullptr);
    REQUIRE(variants->values == std::vector<std::string>{STANDARD, HIGH_FLOW});
    const auto *option = preset->config.option<Option>(key);
    REQUIRE(option != nullptr);
    REQUIRE(option->values.size() == 2);
    return option->values[flow_type == nvtHighFlow ? 1 : 0];
}

// The same for the filament preset of the shipped 0.4 plate.
template<class Option> auto shipped_column(PresetBundle &bundle, const std::string &key, NozzleVolumeType flow_type)
{
    return preset_column<Option>(bundle, U1_FILAMENT, key, flow_type);
}

struct ShippedToolFacts {
    int              print_temperature = 0;  // the highest temperature the tool is sent to
    std::set<double> pressure_advances;      // set by the slicer while the tool is active
    std::set<int>    auxiliary_fan_speeds;   // non-zero auxiliary fan speeds ("M106 P2") in force when a wall of the tool extrudes
    double           max_wall_flow = 0.;     // mm3/s over wall moves of at least 2 mm
};

std::map<int, ShippedToolFacts> shipped_tool_facts(const std::string &gcode, double filament_diameter)
{
    std::map<int, ShippedToolFacts> facts;
    std::istringstream              in(executable_block(gcode));
    std::string                     line;
    int                             tool     = 0;
    bool                            wall     = false;
    double                          feedrate = 0.;
    double                          x = 0., y = 0.;
    int                             auxiliary_fan = 0;
    const double                    filament_area = M_PI * filament_diameter * filament_diameter / 4.;
    while (std::getline(in, line)) {
        if (line.rfind("SET_PRESSURE_ADVANCE ADVANCE=", 0) == 0 && line.find("Override pressure advance value") != std::string::npos) {
            facts[tool].pressure_advances.insert(std::atof(line.c_str() + std::string("SET_PRESSURE_ADVANCE ADVANCE=").size()));
            continue;
        }
        // The fan of the next tool is set ahead of the tool change, so a speed counts for the tool
        // that prints under it, not for the one that is active when it is set.
        if (line.rfind("M106", 0) == 0) {
            if (int(word_value(line, 'P')) == 2)
                auxiliary_fan = std::max(0, int(word_value(line, 'S')));
            continue;
        }
        if (line.rfind("M107", 0) == 0) {
            if (int(word_value(line, 'P')) == 2)
                auxiliary_fan = 0;
            continue;
        }
        if (line.rfind(";TYPE:", 0) == 0 || line.rfind("; FEATURE:", 0) == 0) {
            wall = line.find("wall") != std::string::npos;
            continue;
        }
        if (line.size() >= 2 && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1])) && line.find_first_not_of("0123456789", 1) == std::string::npos) {
            tool = std::atoi(line.c_str() + 1);
            continue;
        }
        if (line.rfind("M104", 0) == 0 || line.rfind("M109", 0) == 0) {
            const double s = word_value(line, 'S');
            const double t = word_value(line, 'T');
            ShippedToolFacts &target = facts[t >= 0. ? int(t) : tool];
            target.print_temperature = std::max(target.print_temperature, int(s));
            continue;
        }
        if (line.rfind("G1 ", 0) == 0 || line.rfind("G0 ", 0) == 0) {
            const double f = word_value(line, 'F');
            if (f > 0.)
                feedrate = f;
            const double new_x = word_value(line, 'X') >= 0. ? word_value(line, 'X') : x;
            const double new_y = word_value(line, 'Y') >= 0. ? word_value(line, 'Y') : y;
            const double length = std::hypot(new_x - x, new_y - y);
            const double e      = word_value(line, 'E');
            if (wall && e > 0. && auxiliary_fan > 0)
                facts[tool].auxiliary_fan_speeds.insert(auxiliary_fan);
            if (wall && e > 0. && length >= 2.)
                facts[tool].max_wall_flow = std::max(facts[tool].max_wall_flow, e * filament_area / length * feedrate / 60.);
            x = new_x;
            y = new_y;
        }
    }
    return facts;
}

// Per object of a two-cube print on `plate`: print_z, height and whether the layer extrudes.
// Object layers of `layer_height`; tool head 2 prefers `second_head_layer_height`, i.e. runs of
// several object layers.
using LayerPlan = std::vector<std::vector<std::tuple<double, double, bool>>>;
LayerPlan layer_plan(const U1Plate &plate, double layer_height, double second_head_layer_height, NozzleVolumeType second_head)
{
    auto               bundle = std::make_unique<PresetBundle>();
    DynamicPrintConfig config = u1_plate_config(*bundle, plate, {nvtStandard, second_head, nvtStandard, nvtStandard});
    config.set_key_value("layer_height",          new ConfigOptionFloat(layer_height));
    config.set_key_value("extruder_layer_height", new ConfigOptionFloats({0., second_head_layer_height, 0., 0.}));
    Print print;
    Model model;
    init_two_head_print(print, model, config, 0.1f); // 2 mm
    {
        const StringObjectException err = print.validate();
        INFO(err.string);
        REQUIRE(err.string.empty());
    }
    print.process();
    LayerPlan plan;
    for (const PrintObject *object : print.objects()) {
        plan.emplace_back();
        for (const Layer *layer : object->layers()) {
            bool prints = false;
            for (const LayerRegion *region : layer->regions())
                prints = prints || !region->perimeters.empty() || !region->fills.empty();
            plan.back().emplace_back(layer->print_z, layer->height, prints);
        }
    }
    return plan;
}

// The plan with tool head 2 on High Flow equals the one with every head on Standard.
void check_layer_plan_unchanged(const U1Plate &plate, double layer_height, double second_head_layer_height)
{
    const LayerPlan standard  = layer_plan(plate, layer_height, second_head_layer_height, nvtStandard);
    const LayerPlan high_flow = layer_plan(plate, layer_height, second_head_layer_height, nvtHighFlow);
    REQUIRE(standard.size() == 2);
    REQUIRE(high_flow.size() == standard.size());
    for (size_t object = 0; object < standard.size(); ++object) {
        REQUIRE(high_flow[object].size() == standard[object].size());
        for (size_t layer = 0; layer < standard[object].size(); ++layer) {
            INFO("object " << object << ", layer " << layer);
            CHECK_THAT(std::get<0>(high_flow[object][layer]), Catch::Matchers::WithinAbs(std::get<0>(standard[object][layer]), EPSILON));
            CHECK_THAT(std::get<1>(high_flow[object][layer]), Catch::Matchers::WithinAbs(std::get<1>(standard[object][layer]), EPSILON));
            CHECK(std::get<2>(high_flow[object][layer]) == std::get<2>(standard[object][layer]));
        }
    }
    // The plan is a real one: the cube of tool head 2 prints in runs, so some of its layers are empty.
    const auto empty_layers = std::count_if(standard[1].begin(), standard[1].end(), [](const auto &layer) { return !std::get<2>(layer); });
    CHECK(empty_layers > 0);
}

} // namespace

TEST_CASE("A shipped U1 plate with one High Flow tool head prints that head alone with the High Flow values", "[HighFlow][Profiles]")
{
    // Tool head 2 carries the High Flow nozzle; heads 1 and 2 print the same filament preset.
    auto               bundle = std::make_unique<PresetBundle>();
    DynamicPrintConfig config = u1_project_config(*bundle, {nvtStandard, nvtHighFlow, nvtStandard, nvtStandard});
    // Extrusion lengths are read as relative ones below.
    REQUIRE(config.opt_bool("use_relative_e_distances"));
    // The auxiliary fan is part of what differs between the columns.
    REQUIRE(config.opt_bool("auxiliary_fan"));

    const std::string gcode = two_head_gcode(config);
    std::map<int, ShippedToolFacts> facts = shipped_tool_facts(gcode, config.option<ConfigOptionFloats>("filament_diameter")->get_at(0));
    const ShippedToolFacts &standard  = facts[0];
    const ShippedToolFacts &high_flow = facts[1];

    SECTION("nozzle temperature") {
        const int standard_temperature  = shipped_column<ConfigOptionInts>(*bundle, "nozzle_temperature", nvtStandard);
        const int high_flow_temperature = shipped_column<ConfigOptionInts>(*bundle, "nozzle_temperature", nvtHighFlow);
        REQUIRE(standard_temperature != high_flow_temperature); // else the preset cannot tell the columns apart
        CHECK(standard.print_temperature == standard_temperature);
        CHECK(high_flow.print_temperature == high_flow_temperature);
    }
    SECTION("maximum volumetric speed") {
        const double standard_cap  = shipped_column<ConfigOptionFloats>(*bundle, "filament_max_volumetric_speed", nvtStandard);
        const double high_flow_cap = shipped_column<ConfigOptionFloats>(*bundle, "filament_max_volumetric_speed", nvtHighFlow);
        REQUIRE(high_flow_cap > standard_cap * 1.2);
        // Each head runs into the cap of its own column: the wall speeds of the process ask for more.
        REQUIRE(standard.max_wall_flow > 0.);
        CHECK(standard.max_wall_flow < standard_cap * 1.05);
        CHECK(high_flow.max_wall_flow > standard_cap * 1.2);
        CHECK(high_flow.max_wall_flow < high_flow_cap * 1.05);
    }
    SECTION("pressure advance") {
        // The Standard column of the preset leaves pressure advance to the firmware, the High Flow
        // column sets its own value.
        REQUIRE_FALSE(shipped_column<ConfigOptionBools>(*bundle, "enable_pressure_advance", nvtStandard));
        REQUIRE(shipped_column<ConfigOptionBools>(*bundle, "enable_pressure_advance", nvtHighFlow));
        CHECK(standard.pressure_advances.empty());
        REQUIRE(high_flow.pressure_advances.size() == 1);
        CHECK_THAT(*high_flow.pressure_advances.begin(),
                   Catch::Matchers::WithinAbs(shipped_column<ConfigOptionFloats>(*bundle, "pressure_advance", nvtHighFlow), 1e-4));
    }
    SECTION("auxiliary fan") {
        const int standard_fan  = shipped_column<ConfigOptionInts>(*bundle, "additional_cooling_fan_speed", nvtStandard);
        const int high_flow_fan = shipped_column<ConfigOptionInts>(*bundle, "additional_cooling_fan_speed", nvtHighFlow);
        REQUIRE(standard_fan != high_flow_fan);
        auto fan_command = [](int percent) { return int(percent * 255. / 100. + 0.5); };
        CHECK(standard.auxiliary_fan_speeds == std::set<int>{fan_command(standard_fan)});
        CHECK(high_flow.auxiliary_fan_speeds == std::set<int>{fan_command(high_flow_fan)});
    }
}

TEST_CASE("A shipped U1 plate plans its per-extruder layer heights the same with a High Flow tool head", "[HighFlow][Profiles][MultiNozzleLayerHeight]")
{
    // Object layers of 0.1 mm; tool head 2 prefers 0.2 mm, i.e. runs of two object layers.
    check_layer_plan_unchanged(U1_PLATE_0_4, 0.1, 0.2);
}

// ---------------------------------------------------------------------------------------------
// High Flow on a 0.6 mm tool head: shipped High Flow values cover 0.4 mm only, so the 0.6 mm
// column is a fixture (tests/snapmaker_high_flow_fixture.hpp). Prints only on the High Flow head.
// ---------------------------------------------------------------------------------------------

namespace {

const char *const MATTE_0_6 = "Snapmaker PLA Matte @U1 0.6 nozzle";

// The 0.4 plate with a 0.6 mm nozzle on tool head 2. Slots 1 and 2 hold the same 0.6 mm preset
// (slot 2 as "filament follows nozzle" fills it; slot 1 so that every difference between T0 and
// T1 comes from the flow type of head 2).
const U1Plate             MIXED_PLATE{U1_MACHINE, U1_PROCESS, {MATTE_0_6, MATTE_0_6, "Snapmaker PLA SnapSpeed @U1", "Snapmaker PETG HF"}};
const std::vector<double> MIXED_DIAMETERS{0.4, 0.6, 0.4, 0.4};

// The High Flow column of the fixture against the shipped Standard column: 220 C against 215,
// a 36 mm3/s ceiling against 20, the auxiliary fan at 100 % against 80.
constexpr int    FIXTURE_HIGH_FLOW_TEMPERATURE = 220;
constexpr double FIXTURE_HIGH_FLOW_MAX_FLOW    = 36.;
constexpr int    FIXTURE_HIGH_FLOW_AUX_FAN     = 100;

void matte_0_6_gets_high_flow_column(PresetBundle &bundle)
{
    filament_gets_high_flow_column(bundle, MATTE_0_6,
                                   {{"nozzle_temperature", std::to_string(FIXTURE_HIGH_FLOW_TEMPERATURE)},
                                    {"nozzle_temperature_initial_layer", std::to_string(FIXTURE_HIGH_FLOW_TEMPERATURE)},
                                    {"filament_max_volumetric_speed", "36"},
                                    {"additional_cooling_fan_speed", std::to_string(FIXTURE_HIGH_FLOW_AUX_FAN)}});
}

} // namespace

TEST_CASE("A U1 plate with a 0.6 mm High Flow tool head prints that head alone with the High Flow column of its 0.6 mm filament", "[HighFlow][Profiles]")
{
    auto               bundle = std::make_unique<PresetBundle>();
    DynamicPrintConfig config = u1_plate_config(*bundle, MIXED_PLATE, {nvtStandard, nvtHighFlow, nvtStandard, nvtStandard}, MIXED_DIAMETERS,
                                                matte_0_6_gets_high_flow_column);
    config.set_key_value("extruder_layer_height", new ConfigOptionFloats(std::vector<double>(HEADS, 0.)));
    REQUIRE(config.opt_bool("use_relative_e_distances"));
    REQUIRE(config.opt_bool("auxiliary_fan"));
    // The shipped Standard column and the fixture's High Flow column.
    const int    standard_temperature  = preset_column<ConfigOptionInts>(*bundle, MATTE_0_6, "nozzle_temperature", nvtStandard);
    const int    high_flow_temperature = preset_column<ConfigOptionInts>(*bundle, MATTE_0_6, "nozzle_temperature", nvtHighFlow);
    const double standard_cap          = preset_column<ConfigOptionFloats>(*bundle, MATTE_0_6, "filament_max_volumetric_speed", nvtStandard);
    const double high_flow_cap         = preset_column<ConfigOptionFloats>(*bundle, MATTE_0_6, "filament_max_volumetric_speed", nvtHighFlow);
    const int    standard_fan          = preset_column<ConfigOptionInts>(*bundle, MATTE_0_6, "additional_cooling_fan_speed", nvtStandard);
    const int    high_flow_fan         = preset_column<ConfigOptionInts>(*bundle, MATTE_0_6, "additional_cooling_fan_speed", nvtHighFlow);
    CHECK(standard_temperature == 215);
    CHECK(high_flow_temperature == FIXTURE_HIGH_FLOW_TEMPERATURE);
    CHECK(preset_column<ConfigOptionInts>(*bundle, MATTE_0_6, "nozzle_temperature_initial_layer", nvtStandard) == 215);
    CHECK(preset_column<ConfigOptionInts>(*bundle, MATTE_0_6, "nozzle_temperature_initial_layer", nvtHighFlow) == FIXTURE_HIGH_FLOW_TEMPERATURE);
    CHECK_THAT(standard_cap, Catch::Matchers::WithinAbs(20., 1e-9));
    CHECK_THAT(high_flow_cap, Catch::Matchers::WithinAbs(FIXTURE_HIGH_FLOW_MAX_FLOW, 1e-9));
    CHECK(standard_fan == 80);
    CHECK(high_flow_fan == FIXTURE_HIGH_FLOW_AUX_FAN);
    // A key the fixture does not name holds the shipped value in both columns.
    CHECK(preset_column<ConfigOptionBools>(*bundle, MATTE_0_6, "enable_pressure_advance", nvtHighFlow) ==
          preset_column<ConfigOptionBools>(*bundle, MATTE_0_6, "enable_pressure_advance", nvtStandard));
    CHECK_THAT(preset_column<ConfigOptionFloats>(*bundle, MATTE_0_6, "pressure_advance", nvtHighFlow),
               Catch::Matchers::WithinAbs(preset_column<ConfigOptionFloats>(*bundle, MATTE_0_6, "pressure_advance", nvtStandard), 1e-9));

    const std::string               gcode = two_head_gcode(config);
    std::map<int, ShippedToolFacts> facts = shipped_tool_facts(gcode, config.option<ConfigOptionFloats>("filament_diameter")->get_at(0));
    const ShippedToolFacts         &standard  = facts[0];
    const ShippedToolFacts         &high_flow = facts[1];
    CHECK(standard.print_temperature == standard_temperature);
    CHECK(high_flow.print_temperature == high_flow_temperature);
    // The 0.20mm Standard process asks for more than either ceiling on the 0.6 mm nozzle
    // (300 / 600 mm/s inner walls); the 0.4 mm head stays below the Standard ceiling.
    REQUIRE(standard.max_wall_flow > 0.);
    CHECK(standard.max_wall_flow < standard_cap * 1.05);
    CHECK(high_flow.max_wall_flow > standard_cap * 1.2);
    CHECK(high_flow.max_wall_flow < high_flow_cap * 1.05);
    auto fan_command = [](int percent) { return int(percent * 255. / 100. + 0.5); };
    CHECK(standard.auxiliary_fan_speeds == std::set<int>{fan_command(standard_fan)});
    CHECK(high_flow.auxiliary_fan_speeds == std::set<int>{fan_command(high_flow_fan)});
    // Pressure advance is the same in both columns, so both tool heads are sent the same.
    CHECK(standard.pressure_advances == high_flow.pressure_advances);
}
