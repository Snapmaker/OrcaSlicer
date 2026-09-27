#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PerHeadProcess.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// Snapmaker Orca: process values per tool head, checked on sliced G-code: which slot a feature
// reads its speed from and which head's speed the prime tower uses. Column composition is tested
// in tests/libslic3r/test_per_head_process.cpp.

namespace {

constexpr size_t HEADS = 4;
const char *const STANDARD = "Direct Drive Standard";

// Four tool heads with 0.4 mm nozzles, filament i on tool head i, one process column per head
// (print_extruder_id 1..4). No flow variants, cooling slowdown or volumetric cap, so a G-code
// feed rate equals the process speed of the slot that was read.
DynamicPrintConfig four_head_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("layer_height",               new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("enable_prime_tower",         new ConfigOptionBool(false));
    config.set_key_value("enable_support",             new ConfigOptionBool(false));
    config.set_key_value("use_relative_e_distances",   new ConfigOptionBool(false));
    config.set_key_value("skirt_loops",                new ConfigOptionInt(0));
    config.set_key_value("gcode_flavor",               new ConfigOptionEnum<GCodeFlavor>(gcfKlipper));

    // Printer.
    config.set_key_value("nozzle_diameter",       new ConfigOptionFloats(std::vector<double>(HEADS, 0.4)));
    config.set_key_value("extruder_layer_height", new ConfigOptionFloats(std::vector<double>(HEADS, 0.)));
    config.set_key_value("min_layer_height",      new ConfigOptionFloats(std::vector<double>(HEADS, 0.07)));
    config.set_key_value("max_layer_height",      new ConfigOptionFloats(std::vector<double>(HEADS, 0.3)));
    config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values       = std::vector<int>(HEADS, int(etDirectDrive));
    config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = std::vector<int>(HEADS, int(nvtStandard));
    config.set_key_value("extruder_variant_list", new ConfigOptionStrings(std::vector<std::string>(HEADS, STANDARD)));
    config.set_key_value("retraction_length",     new ConfigOptionFloats(std::vector<double>(HEADS, 1.5)));
    config.set_key_value("z_hop",                 new ConfigOptionFloats(std::vector<double>(HEADS, 0.)));

    // Process: one column per tool head.
    config.set_key_value("print_extruder_id",      new ConfigOptionInts({1, 2, 3, 4}));
    config.set_key_value("print_extruder_variant", new ConfigOptionStrings(std::vector<std::string>(HEADS, STANDARD)));
    for (const std::string &key : print_options_with_variant) {
        if (key == "print_extruder_id" || key == "print_extruder_variant")
            continue;
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        REQUIRE(option != nullptr);
        option->resize(HEADS);
    }

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
    config.set_key_value("nozzle_temperature",               new ConfigOptionInts(std::vector<int>(HEADS, 215)));
    config.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts(std::vector<int>(HEADS, 215)));
    config.set_key_value("filament_max_volumetric_speed",    new ConfigOptionFloats(std::vector<double>(HEADS, 200.)));
    // The cooling slowdown would rewrite the feed rates under test.
    config.set_key_value("slow_down_for_layer_cooling", new ConfigOptionBools(std::vector<unsigned char>(HEADS, 0)));
    config.set_key_value("enable_pressure_advance",     new ConfigOptionBools(std::vector<unsigned char>(HEADS, 0)));
    config.set_key_value("pressure_advance",            new ConfigOptionFloats(std::vector<double>(HEADS, 0.02)));
    return config;
}

// One value per tool head for a process key.
void set_head_values(DynamicPrintConfig &config, const std::string &key, const std::vector<double> &values)
{
    REQUIRE(values.size() == HEADS);
    auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
    REQUIRE(option != nullptr);
    const std::unique_ptr<ConfigOption> parsed(option->clone());
    for (size_t head = 0; head < HEADS; ++head) {
        REQUIRE(parsed->deserialize(float_to_string_decimal_point(values[head])));
        option->set_at(parsed.get(), head, 0);
    }
}

// The first "<word><number>" of the command part of a G-code line, -1 when absent.
double word_value(const std::string &line, char word)
{
    const std::string command = line.substr(0, line.find(';'));
    for (size_t pos = command.find(word); pos != std::string::npos; pos = command.find(word, pos + 1))
        if (pos > 0 && command[pos - 1] == ' ' && pos + 1 < command.size())
            return std::atof(command.c_str() + pos + 1);
    return -1.;
}

// The tool a line changes to: "T1", or "T1 ; change extruder" (GCodeWriter::full_gcode_comment is
// on in the tests); -1 for any other line.
int tool_change(const std::string &line)
{
    if (line.size() < 2 || line[0] != 'T' || !std::isdigit(static_cast<unsigned char>(line[1])))
        return -1;
    const size_t end = line.find_first_not_of("0123456789", 1);
    if (end != std::string::npos && line[end] != ' ' && line[end] != ';' && line[end] != '\t' && line[end] != '\r')
        return -1;
    return std::atoi(line.c_str() + 1);
}

// The same table with one process column shared by every tool head (print_extruder_id [1]).
void single_column(DynamicPrintConfig &config)
{
    config.set_key_value("print_extruder_id",      new ConfigOptionInts({1}));
    config.set_key_value("print_extruder_variant", new ConfigOptionStrings({STANDARD}));
    for (const std::string &key : print_options_with_variant) {
        if (key == "print_extruder_id" || key == "print_extruder_variant")
            continue;
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        REQUIRE(option != nullptr);
        option->resize(1);
    }
}

void with_prime_tower(DynamicPrintConfig &config)
{
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
    config.set_key_value("wipe_tower_x", new ConfigOptionFloats({20.})); // clear of the cubes, inside the 200 x 200 test bed
    config.set_key_value("wipe_tower_y", new ConfigOptionFloats({20.}));
    config.set_key_value("wipe_tower_no_sparse_layers", new ConfigOptionBool(false));
    // The prime tower needs relative extruder addressing, which in turn needs the E reset.
    config.set_key_value("use_relative_e_distances", new ConfigOptionBool(true));
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0"));
}

// Two 20 mm cubes next to each other, 2 mm tall (10 layers of 0.2 mm), with the given per-object
// overrides; by default the first on tool head 1, the second on tool head 2.
std::string two_head_gcode(const DynamicPrintConfig &config,
                           const std::vector<std::vector<ConfigBase::SetDeserializeItem>> &overrides = {{{"extruder", "1"}}, {{"extruder", "2"}}})
{
    TriangleMesh first = mesh(TestMesh::cube_20x20x20);
    first.scale(Vec3f(1.f, 1.f, 0.1f));
    first.translate(100.f, 100.f, 0.f);
    TriangleMesh second = mesh(TestMesh::cube_20x20x20);
    second.scale(Vec3f(1.f, 1.f, 0.1f));
    second.translate(140.f, 100.f, 0.f);
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
    return Slic3r::Test::gcode(print);
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

// The tool changes on the prime tower: for every "; CP TOOLCHANGE WIPE" block outside the priming
// lines, the tool that was changed in, the layer (1 = the first) and the feed rate of the block's
// first extruding move.
struct WipeBlock {
    int tool;
    int layer;
    int first_feedrate;
};
std::vector<WipeBlock> wipe_blocks(const std::string &gcode)
{
    std::vector<WipeBlock> out;
    std::istringstream     in(executable_block(gcode));
    std::string            line;
    int                    tool = 0, layer = 0;
    double                 feedrate = 0.;
    bool                   priming = false, in_wipe = false;
    while (std::getline(in, line)) {
        if (line.rfind("; CP PRIMING START", 0) == 0) { priming = true; continue; }
        if (line.rfind("; CP PRIMING END", 0) == 0) { priming = false; continue; }
        if (line.rfind(";LAYER_CHANGE", 0) == 0 || line.rfind("; CHANGE_LAYER", 0) == 0) { ++layer; continue; }
        if (const int changed_to = tool_change(line); changed_to >= 0) {
            tool = changed_to;
            continue;
        }
        if (line.rfind("; CP TOOLCHANGE WIPE", 0) == 0) { in_wipe = !priming; continue; }
        if (line.rfind("; CP TOOLCHANGE END", 0) == 0) { in_wipe = false; continue; }
        if (line.rfind("G1 ", 0) == 0) {
            const double f = word_value(line, 'F');
            if (f > 0.)
                feedrate = f;
            if (in_wipe && word_value(line, 'E') > 0.) {
                out.push_back({tool, layer, int(std::lround(feedrate))});
                in_wipe = false;
            }
        }
    }
    return out;
}

// The feed rate of the first purge line of WipeTower2::toolchange_Wipe: 0.33 of the target, which
// is the first layer speed on the first layer, else the sparse infill speed capped by the maximum
// purge speed. Computed in the tower's float arithmetic.
int first_wipe_feedrate(double speed, bool first_layer, double max_purge_speed)
{
    const float target = first_layer ? float(speed) * 60.f : std::min(float(max_purge_speed) * 60.f, float(speed) * 60.f);
    return int(std::floor(0.33f * target + 0.5f));
}


// The feed rates (mm/min) of the extruding moves of every feature type above the first layer, per
// tool. The first layer prints every wall at initial_layer_speed and every fill at
// initial_layer_infill_speed, which is not what the role speeds under test say.
std::map<int, std::map<std::string, std::set<int>>> feature_feedrates(const std::string &gcode)
{
    std::map<int, std::map<std::string, std::set<int>>> out;
    std::istringstream in(executable_block(gcode));
    std::string        line, feature;
    int                tool     = 0;
    int                layer    = 0;
    double             feedrate = 0.;
    while (std::getline(in, line)) {
        if (line.rfind(";LAYER_CHANGE", 0) == 0 || line.rfind("; CHANGE_LAYER", 0) == 0) { ++layer; continue; }
        if (line.rfind(";TYPE:", 0) == 0) {
            feature = line.substr(6);
            continue;
        }
        if (line.rfind("; FEATURE:", 0) == 0) {
            feature = line.substr(10);
            continue;
        }
        if (const int changed_to = tool_change(line); changed_to >= 0) {
            tool = changed_to;
            continue;
        }
        if (line.rfind("G1 ", 0) == 0) {
            const double f = word_value(line, 'F');
            if (f > 0.)
                feedrate = f;
            const bool moves = word_value(line, 'X') >= 0. || word_value(line, 'Y') >= 0.;
            if (layer > 1 && moves && word_value(line, 'E') > 0. && !feature.empty())
                out[tool][feature].insert(int(std::lround(feedrate)));
        }
    }
    return out;
}

std::string joined(const std::set<int> &values)
{
    std::string out;
    for (int value : values)
        out += (out.empty() ? "" : " ") + std::to_string(value);
    return out;
}

// A 20 mm cube on tool head 1 (filament 1) with a modifier that carries the given overrides over
// the left half (x < 10) of its upper half (z > 10): every layer above 10 mm has two regions with
// an area of their own.
void init_cube_with_modifier(Print &print, Model &model, const DynamicPrintConfig &config,
                             const std::vector<std::pair<std::string, std::string>> &modifier_values)
{
    std::vector<TriangleMesh> meshes;
    meshes.emplace_back(mesh(TestMesh::cube_20x20x20));
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides = {{{"extruder", "1"}}};
    init_print(std::move(meshes), print, model, config, &overrides, /*arrange=*/false);

    ModelObject *object = model.objects.front();
    TriangleMesh box    = make_cube(12., 24., 10.);
    box.translate(-2.f, -2.f, 10.f);
    ModelVolume *modifier = object->add_volume(std::move(box), ModelVolumeType::PARAMETER_MODIFIER);
    {
        DynamicPrintConfig overrides;
        for (const auto &[key, value] : modifier_values)
            overrides.set_deserialize_strict(key, value);
        modifier->config.apply(overrides);
    }
    // The narrowed table of the first apply is the config of the print; narrowing it again
    // changes nothing.
    print.apply(model, print.full_print_config());
    {
        const StringObjectException err = print.validate();
        INFO(err.string);
        REQUIRE(err.string.empty());
    }
}

} // namespace

// Layer::is_perimeter_compatible and the infill role speed read the slot of the tool head that
// prints the filament, not the slot of the next filament.
TEST_CASE("Region merging and the infill speed compare the slot of the tool head that prints", "[PerHeadProcess][head_lookup]")
{
    // Tool head 1 prints at 200 / 270, tool head 2 at 60 / 100; the modifier on tool head 1's cube
    // asks for 60 / 100. Read from tool head 2's slot, both regions would match and merge into one
    // LayerRegion; read from tool head 1's slot, each region keeps its own perimeters and speeds.
    DynamicPrintConfig config = four_head_config();
    set_head_values(config, "outer_wall_speed",    {200., 60., 200., 200.});
    set_head_values(config, "inner_wall_speed",    {200., 60., 200., 200.});
    set_head_values(config, "sparse_infill_speed", {270., 100., 270., 270.});
    set_head_values(config, "gap_infill_speed",    {200., 60., 200., 200.});
    config.set_key_value("sparse_infill_density", new ConfigOptionPercent(20.));
    config.set_key_value("wall_loops",            new ConfigOptionInt(2));

    Print print;
    Model model;
    init_cube_with_modifier(print, model, config, {{"outer_wall_speed", "60"}, {"inner_wall_speed", "60"}, {"sparse_infill_speed", "100"}, {"gap_infill_speed", "60"}});
    const std::string gcode = Slic3r::Test::gcode(print);

    SECTION("both regions of a layer inside the modifier generate their own perimeters") {
        REQUIRE(print.objects().size() == 1);
        const PrintObject *object = print.objects().front();
        size_t layers_with_two_perimeter_regions = 0;
        for (const Layer *layer : object->layers()) {
            if (layer->print_z < 10. + EPSILON || layer->regions().size() != 2)
                continue;
            if (!layer->regions()[0]->perimeters.empty() && !layer->regions()[1]->perimeters.empty())
                ++layers_with_two_perimeter_regions;
        }
        // The merged case leaves the second region without perimeters on every layer.
        CHECK(layers_with_two_perimeter_regions > 0);
    }

    SECTION("the walls and the infill of tool head 1 run at the base speeds and at the modifier's") {
        const auto rates = feature_feedrates(gcode);
        REQUIRE(rates.count(0) == 1);
        const auto &tool = rates.at(0);
        REQUIRE(tool.count("Outer wall") == 1);
        REQUIRE(tool.count("Sparse infill") == 1);
        INFO("outer wall feed rates: " << joined(tool.at("Outer wall")));
        INFO("sparse infill feed rates: " << joined(tool.at("Sparse infill")));
        CHECK(tool.at("Outer wall").count(200 * 60) == 1);
        CHECK(tool.at("Outer wall").count(60 * 60) == 1);
        CHECK(tool.at("Sparse infill").count(270 * 60) == 1);
        CHECK(tool.at("Sparse infill").count(100 * 60) == 1);
        // No other tool prints.
        CHECK(rates.size() == 1);
    }
}

// WipeTower2 prints each tool's lines with the speeds of the tool head that holds the filament.
TEST_CASE("The prime tower runs each tool at the speeds of its own tool head", "[PerHeadProcess][tower]")
{
    SECTION("slots that all hold one value give the G-code of a single process column") {
        DynamicPrintConfig per_head = four_head_config();
        with_prime_tower(per_head);
        set_head_values(per_head, "initial_layer_speed", {50., 50., 50., 50.});
        set_head_values(per_head, "sparse_infill_speed", {80., 80., 80., 80.});
        set_head_values(per_head, "inner_wall_speed",    {150., 150., 150., 150.});
        DynamicPrintConfig one_column = per_head;
        single_column(one_column);

        const std::string before = executable_block(two_head_gcode(one_column));
        const std::string after  = executable_block(two_head_gcode(per_head));
        REQUIRE(before.find("\nT0") != std::string::npos);
        REQUIRE(before.find("\nT1") != std::string::npos);
        REQUIRE(before.find("; CP TOOLCHANGE WIPE") != std::string::npos);
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

    SECTION("a tool changed in purges at the first layer and infill speed of its own tool head") {
        // Tool head 1: 50 mm/s on the first layer, 80 above; tool head 2: 40 and 50. Both tools
        // are changed in on the tower, on the first layer and above it.
        DynamicPrintConfig config = four_head_config();
        with_prime_tower(config);
        const std::vector<double> initial_layer_speed = {50., 40., 50., 50.};
        const std::vector<double> sparse_infill_speed = {80., 50., 80., 80.};
        set_head_values(config, "initial_layer_speed", initial_layer_speed);
        set_head_values(config, "sparse_infill_speed", sparse_infill_speed);
        const double max_purge_speed = config.opt_float("wipe_tower_max_purge_speed");

        const std::vector<WipeBlock> blocks = wipe_blocks(two_head_gcode(config));
        REQUIRE(blocks.size() >= 4);
        std::set<int> tools_changed_in, layers;
        for (const WipeBlock &block : blocks) {
            tools_changed_in.insert(block.tool);
            layers.insert(block.layer);
            REQUIRE(block.tool >= 0);
            REQUIRE(block.tool < int(HEADS));
            const bool first_layer = block.layer <= 1;
            const int  expected    = first_wipe_feedrate(first_layer ? initial_layer_speed[size_t(block.tool)] : sparse_infill_speed[size_t(block.tool)],
                                                      first_layer, max_purge_speed);
            INFO("tool " << block.tool << " on layer " << block.layer);
            CHECK(block.first_feedrate == expected);
        }
        // Both tools purge, on the first layer and above it.
        CHECK(tools_changed_in == std::set<int>{0, 1});
        CHECK(layers.count(1) == 1);
        CHECK(layers.size() > 1);
    }
}

namespace {

const char *const HIGH_FLOW = "Direct Drive High Flow";

// The four-head printer declaring Standard and High Flow for every tool head with nozzle stats
// (one slot per head x flow type, 8 slots), tool head 3 on High Flow; the process in the flow-only
// layout of 0.20mm Standard (Standard 200, High Flow 500 for the walls).
DynamicPrintConfig flow_columns_config()
{
    DynamicPrintConfig config = four_head_config();
    config.set_key_value("extruder_variant_list", new ConfigOptionStrings(std::vector<std::string>(HEADS, std::string(STANDARD) + "," + HIGH_FLOW)));
    config.set_key_value("extruder_nozzle_stats", new ConfigOptionStrings(std::vector<std::string>(HEADS, "Standard#1|High Flow#1")));
    config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {int(nvtStandard), int(nvtStandard), int(nvtHighFlow), int(nvtStandard)};
    // Machine values one per column, both columns of a head equal.
    config.set_key_value("retraction_length", new ConfigOptionFloats(std::vector<double>(HEADS * 2, 1.5)));
    config.set_key_value("z_hop",             new ConfigOptionFloats(std::vector<double>(HEADS * 2, 0.)));
    config.set_key_value("print_extruder_id",      new ConfigOptionInts({1, 1}));
    config.set_key_value("print_extruder_variant", new ConfigOptionStrings({STANDARD, HIGH_FLOW}));
    for (const std::string &key : print_options_with_variant) {
        if (key == "print_extruder_id" || key == "print_extruder_variant")
            continue;
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        REQUIRE(option != nullptr);
        option->resize(2);
    }
    for (const char *key : {"outer_wall_speed", "inner_wall_speed"}) {
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        const std::unique_ptr<ConfigOption> parsed(option->clone());
        REQUIRE(parsed->deserialize("200,500"));
        option->set(parsed.get());
    }
    return config;
}

// A single-column process preset of another nozzle size: 60 mm/s walls.
std::unique_ptr<Preset> wall_60_source()
{
    auto preset = std::make_unique<Preset>(Preset::TYPE_PRINT, "0.12mm Standard @Test (0.2 nozzle)");
    preset->config.option<ConfigOptionInts>("print_extruder_id", true)->values         = {1};
    preset->config.option<ConfigOptionStrings>("print_extruder_variant", true)->values = {STANDARD};
    preset->config.option<ConfigOptionFloatsNullable>("outer_wall_speed", true)->values = {60.};
    preset->config.option<ConfigOptionFloatsNullable>("inner_wall_speed", true)->values = {60.};
    return preset;
}

// The table composed with tool head 2 (0-based 1) deriving from `source`.
void compose_head_2(DynamicPrintConfig &config, const Preset &source)
{
    std::vector<PerHeadProcess::Source> sources(HEADS);
    for (size_t head = 0; head < HEADS; ++head)
        sources[head].head = head;
    sources[1].preset  = &source;
    sources[1].derived = true;
    sources[1].reason  = PerHeadProcess::Reason::Derived;
    sources[1].composed_keys.assign(PerHeadProcess::composed_keys().begin(), PerHeadProcess::composed_keys().end());
    REQUIRE(PerHeadProcess::compose(config, {}, sources));
    REQUIRE(config.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{1, 1, 2, 2, 3, 3, 4, 4});
    REQUIRE(config.option<ConfigOptionInts>(PerHeadProcess::source_column_key)->values == std::vector<int>{0, 1, 0, 1, 0, 1, 0, 1});
}

// The outer wall feed rates of tool head 2 (Standard, derived) and tool head 3 (High Flow) with
// the given outer wall override on both cubes.
std::pair<std::set<int>, std::set<int>> wall_feedrates_with_override(const DynamicPrintConfig &config, const std::string &override_value)
{
    std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides = {{{"extruder", "2"}}, {{"extruder", "3"}}};
    if (!override_value.empty())
        for (auto &object : overrides) {
            object.push_back({"outer_wall_speed", override_value});
            object.push_back({"inner_wall_speed", override_value});
        }
    const auto rates = feature_feedrates(two_head_gcode(config, overrides));
    std::set<int> head_2, head_3;
    if (rates.count(1) == 1 && rates.at(1).count("Outer wall") == 1)
        head_2 = rates.at(1).at("Outer wall");
    if (rates.count(2) == 1 && rates.at(2).count("Outer wall") == 1)
        head_3 = rates.at(2).at("Outer wall");
    return {head_2, head_3};
}

} // namespace

// An override on a part is absolute on every tool head that prints it and is read by its width
// on a table composed per tool head.
TEST_CASE("An override on a part is read by its width on a table composed per tool head", "[PerHeadProcess][phs_override_width]")
{
    const std::unique_ptr<Preset> source = wall_60_source();
    DynamicPrintConfig            config = flow_columns_config();
    compose_head_2(config, *source);

    SECTION("without an override tool head 2 prints the derived speed and tool head 3 the selected preset's High Flow column") {
        const auto [head_2, head_3] = wall_feedrates_with_override(config, "");
        CHECK(head_2 == std::set<int>{60 * 60});
        CHECK(head_3 == std::set<int>{500 * 60});
    }
    SECTION("a one-value override applies to every slot and beats the derived value") {
        const auto [head_2, head_3] = wall_feedrates_with_override(config, "90");
        CHECK(head_2 == std::set<int>{90 * 60});
        CHECK(head_3 == std::set<int>{90 * 60});
    }
    SECTION("an override as wide as the selected preset is read through its column: Standard on tool head 2, High Flow on tool head 3") {
        const auto [head_2, head_3] = wall_feedrates_with_override(config, "90,400");
        CHECK(head_2 == std::set<int>{90 * 60});
        CHECK(head_3 == std::set<int>{400 * 60});
    }
    SECTION("an override as wide as the slot table is read slot by slot") {
        // Slots: (1,S) (1,HF) (2,S) (2,HF) (3,S) (3,HF) (4,S) (4,HF); tool head 2 Standard is slot 3, tool head 3 High Flow slot 6.
        const auto [head_2, head_3] = wall_feedrates_with_override(config, "10,20,90,30,40,400,50,70");
        CHECK(head_2 == std::set<int>{90 * 60});
        CHECK(head_3 == std::set<int>{400 * 60});
    }
    SECTION("a two-value override under a single-column selected preset gives every tool head its first value") {
        DynamicPrintConfig one_column = flow_columns_config();
        one_column.set_key_value("print_extruder_id",      new ConfigOptionInts({1}));
        one_column.set_key_value("print_extruder_variant", new ConfigOptionStrings({STANDARD}));
        for (const std::string &key : print_options_with_variant) {
            if (key == "print_extruder_id" || key == "print_extruder_variant")
                continue;
            auto *option = dynamic_cast<ConfigOptionVectorBase *>(one_column.option(key));
            REQUIRE(option != nullptr);
            option->resize(1);
        }
        std::vector<PerHeadProcess::Source> sources(HEADS);
        for (size_t head = 0; head < HEADS; ++head)
            sources[head].head = head;
        sources[1].preset  = source.get();
        sources[1].derived = true;
        sources[1].composed_keys.assign(PerHeadProcess::composed_keys().begin(), PerHeadProcess::composed_keys().end());
        REQUIRE(PerHeadProcess::compose(one_column, {}, sources));
        REQUIRE(one_column.option<ConfigOptionInts>(PerHeadProcess::source_column_key)->values == std::vector<int>(HEADS * 2, 0));
        const auto [head_2, head_3] = wall_feedrates_with_override(one_column, "90,400");
        CHECK(head_2 == std::set<int>{90 * 60});
        CHECK(head_3 == std::set<int>{90 * 60});
    }
}

namespace {

// The acceleration (SET_VELOCITY_LIMIT ACCEL=, Klipper) in force at the extruding moves of the
// outer walls above the first layer (which runs at initial_layer_acceleration), per tool.
std::map<int, std::set<int>> outer_wall_accelerations(const std::string &gcode)
{
    std::map<int, std::set<int>> out;
    std::istringstream in(executable_block(gcode));
    std::string        line, feature;
    int                tool  = 0;
    int                layer = 0;
    int                accel = 0;
    const std::string  prefix = "SET_VELOCITY_LIMIT ACCEL=";
    while (std::getline(in, line)) {
        if (line.rfind(prefix, 0) == 0) {
            accel = std::atoi(line.c_str() + prefix.size());
            continue;
        }
        if (line.rfind(";LAYER_CHANGE", 0) == 0 || line.rfind("; CHANGE_LAYER", 0) == 0) { ++layer; continue; }
        if (line.rfind(";TYPE:", 0) == 0) { feature = line.substr(6); continue; }
        if (line.rfind("; FEATURE:", 0) == 0) { feature = line.substr(10); continue; }
        if (const int changed_to = tool_change(line); changed_to >= 0) {
            tool = changed_to;
            continue;
        }
        if (layer > 1 && line.rfind("G1 ", 0) == 0 && feature == "Outer wall" && word_value(line, 'E') > 0. && (word_value(line, 'X') >= 0. || word_value(line, 'Y') >= 0.))
            out[tool].insert(accel);
    }
    return out;
}

// A 0.2 mm nozzle process preset: walls 60 / 150, sparse 100, gap fill 50,
// accelerations 4000 / 2000, first layer 40.
std::unique_ptr<Preset> small_nozzle_source()
{
    auto preset = std::make_unique<Preset>(Preset::TYPE_PRINT, "0.10mm High Quality @Test (0.2 nozzle)");
    DynamicPrintConfig &config = preset->config;
    config.option<ConfigOptionInts>("print_extruder_id", true)->values         = {1};
    config.option<ConfigOptionStrings>("print_extruder_variant", true)->values = {STANDARD};
    config.option<ConfigOptionFloatsNullable>("outer_wall_speed", true)->values        = {60.};
    config.option<ConfigOptionFloatsNullable>("inner_wall_speed", true)->values        = {150.};
    config.option<ConfigOptionFloatsNullable>("sparse_infill_speed", true)->values     = {100.};
    config.option<ConfigOptionFloatsNullable>("gap_infill_speed", true)->values        = {50.};
    config.option<ConfigOptionFloatsNullable>("default_acceleration", true)->values    = {4000.};
    config.option<ConfigOptionFloatsNullable>("outer_wall_acceleration", true)->values = {2000.};
    config.option<ConfigOptionFloatsNullable>("initial_layer_speed", true)->values     = {40.};
    return preset;
}

} // namespace

// A mixed plate sliced end to end: tool head 1 has a 0.4 mm nozzle, tool head 2 a 0.2 mm nozzle
// with a filament capped at 1.6 mm3/s and the 0.2 mm process values composed onto its column.
TEST_CASE("A tool head of another nozzle size prints with the speeds and accelerations of the process preset of its size", "[PerHeadProcess][phs_gcode_head2]")
{
    DynamicPrintConfig config = four_head_config();
    with_prime_tower(config);
    config.set_key_value("layer_height",               new ConfigOptionFloat(0.1));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.1));
    config.set_key_value("nozzle_diameter",            new ConfigOptionFloats({0.4, 0.2, 0.4, 0.4}));
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats({200., 1.6, 200., 200.}));
    // Machine limits far above the process accelerations (the Klipper writer clamps to them).
    for (const char *key : {"machine_max_acceleration_extruding", "machine_max_acceleration_x", "machine_max_acceleration_y"})
        config.set_key_value(key, new ConfigOptionFloats(std::vector<double>(HEADS * 2, 20000.)));
    set_head_values(config, "outer_wall_speed",         {200., 200., 200., 200.});
    set_head_values(config, "inner_wall_speed",         {200., 200., 200., 200.});
    set_head_values(config, "sparse_infill_speed",      {270., 270., 270., 270.});
    set_head_values(config, "gap_infill_speed",         {250., 250., 250., 250.});
    set_head_values(config, "default_acceleration",     {10000., 10000., 10000., 10000.});
    set_head_values(config, "outer_wall_acceleration",  {5000., 5000., 5000., 5000.});
    set_head_values(config, "initial_layer_speed",      {50., 50., 50., 50.});

    const std::unique_ptr<Preset>       source  = small_nozzle_source();
    std::vector<PerHeadProcess::Source> sources(HEADS);
    for (size_t head = 0; head < HEADS; ++head)
        sources[head].head = head;
    sources[1].preset  = source.get();
    sources[1].derived = true;
    sources[1].reason  = PerHeadProcess::Reason::Derived;
    sources[1].composed_keys.assign(PerHeadProcess::composed_keys().begin(), PerHeadProcess::composed_keys().end());
    REQUIRE(PerHeadProcess::compose(config, {}, sources));
    REQUIRE(config.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{1, 2, 3, 4});

    const std::string gcode = two_head_gcode(config);
    const auto        rates = feature_feedrates(gcode);
    REQUIRE(rates.count(0) == 1);
    REQUIRE(rates.count(1) == 1);

    SECTION("the outer walls run at the speed of each tool head's preset: 200 mm/s and 60 mm/s (below the 1.6 mm3/s cap at 0.1 mm)") {
        REQUIRE(rates.at(0).count("Outer wall") == 1);
        REQUIRE(rates.at(1).count("Outer wall") == 1);
        INFO("tool 1 outer wall: " << joined(rates.at(1).at("Outer wall")));
        CHECK(rates.at(0).at("Outer wall") == std::set<int>{200 * 60});
        CHECK(rates.at(1).at("Outer wall") == std::set<int>{60 * 60});
    }
    SECTION("the sparse infill of the 0.2 mm tool head asks for 100 mm/s and is held by the cap of its filament") {
        REQUIRE(rates.at(1).count("Sparse infill") == 1);
        INFO("tool 1 sparse infill: " << joined(rates.at(1).at("Sparse infill")));
        for (int feedrate : rates.at(1).at("Sparse infill")) {
            CHECK(feedrate < 100 * 60);
            CHECK(feedrate > 60 * 60);
        }
        CHECK(rates.at(0).at("Sparse infill") == std::set<int>{270 * 60});
    }
    SECTION("the outer wall acceleration is set per tool head: SET_VELOCITY_LIMIT ACCEL=5000 and 2000") {
        const auto accelerations = outer_wall_accelerations(gcode);
        REQUIRE(accelerations.count(0) == 1);
        REQUIRE(accelerations.count(1) == 1);
        CHECK(accelerations.at(0) == std::set<int>{5000});
        CHECK(accelerations.at(1) == std::set<int>{2000});
    }
    SECTION("the prime tower purges each tool at its own first layer speed") {
        const std::vector<WipeBlock> blocks = wipe_blocks(gcode);
        REQUIRE_FALSE(blocks.empty());
        bool first_layer_seen = false;
        for (const WipeBlock &block : blocks) {
            if (block.layer > 1)
                continue;
            first_layer_seen = true;
            INFO("tool " << block.tool << " on layer " << block.layer);
            CHECK(block.first_feedrate == first_wipe_feedrate(block.tool == 1 ? 40. : 50., true, config.opt_float("wipe_tower_max_purge_speed")));
        }
        CHECK(first_layer_seen);
    }
    SECTION("the header carries the record and not the transient source column key") {
        CHECK(gcode.find("print_extruder_source_column") == std::string::npos);
        CHECK(gcode.find("; extruder_process_preset") != std::string::npos);
    }
}
