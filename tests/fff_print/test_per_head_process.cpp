#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_helpers.hpp"

#include <boost/filesystem.hpp>

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

// A slice of two_head_slice with the Print behind it, kept for the message of a failed check.
struct TwoHeadSlice
{
    Print       print;
    Model       model;
    std::string gcode;
};

// Two 20 mm cubes next to each other, 2 mm tall (10 layers of 0.2 mm), with the given per-object
// overrides; by default the first on tool head 1, the second on tool head 2.
std::unique_ptr<TwoHeadSlice> two_head_slice(const DynamicPrintConfig &config,
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
    auto slice = std::make_unique<TwoHeadSlice>();
    init_print(std::move(meshes), slice->print, slice->model, config, &overrides, /*arrange=*/false);
    {
        const StringObjectException err = slice->print.validate();
        INFO(err.string);
        REQUIRE(err.string.empty());
    }
    slice->gcode = Slic3r::Test::gcode(slice->print);
    return slice;
}

std::string two_head_gcode(const DynamicPrintConfig &config,
                           const std::vector<std::vector<ConfigBase::SetDeserializeItem>> &overrides = {{{"extruder", "1"}}, {{"extruder", "2"}}})
{
    return two_head_slice(config, overrides)->gcode;
}

// The Print state behind a slice, for a failure message: per-object layer and extrusion counts,
// the tool ordering's layers, and the counts of a rebuilt ordering. Tells whether the slice or the
// tool ordering lost the layers.
std::string print_digest(const Print &print)
{
    std::string out;
    for (const PrintObject *object : print.objects()) {
        size_t with_slices = 0, with_extrusions = 0;
        const Layer *first_empty = nullptr;
        for (const Layer *layer : object->layers()) {
            if (!layer->lslices.empty())
                ++with_slices;
            if (layer->has_extrusions())
                ++with_extrusions;
            else if (first_empty == nullptr && layer->id() > 0)
                first_empty = layer;
        }
        out += (out.empty() ? "object " : "; object ") + object->model_object()->name + ": layers " + std::to_string(object->layers().size()) +
               ", with slices " + std::to_string(with_slices) + ", with extrusions " + std::to_string(with_extrusions);
        if (first_empty != nullptr) {
            out += ", first without an extrusion above the first: layer " + std::to_string(first_empty->id() + 1) + " (z " +
                   float_to_string_decimal_point(first_empty->print_z) + ") lslices " + std::to_string(first_empty->lslices.size()) + ", regions";
            for (const LayerRegion *layerm : first_empty->regions())
                out += " slices=" + std::to_string(layerm->slices.surfaces.size()) + " perimeters=" + std::to_string(layerm->perimeters.entities.size()) +
                       " fills=" + std::to_string(layerm->fills.entities.size());
        }
    }
    const std::vector<LayerTools> &layer_tools = print.tool_ordering().layer_tools();
    size_t      with_extruders = 0, with_object = 0, with_tower = 0;
    std::string layers;
    for (const LayerTools &lt : layer_tools) {
        // has_object is set where the extruders are collected, for a region with perimeters or
        // fills, and nothing later clears it: a layer with an object and no extruder lost them
        // after the collection; a layer without one had nothing to collect from.
        if (lt.has_object)
            ++with_object;
        if (lt.has_wipe_tower)
            ++with_tower;
        if (lt.extruders.empty())
            continue;
        ++with_extruders;
        if (with_extruders > 4)
            continue;
        layers += " " + float_to_string_decimal_point(lt.print_z) + ":";
        for (size_t i = 0; i < lt.extruders.size(); ++i)
            layers += (i == 0 ? "T" : ",T") + std::to_string(lt.extruders[i]);
    }
    out += "; tool ordering: layers " + std::to_string(layer_tools.size()) + ", with extruders " + std::to_string(with_extruders) +
           ", with an object " + std::to_string(with_object) + ", with a tower slab " + std::to_string(with_tower);
    if (!layers.empty())
        out += " (the first:" + layers + ")";
    // An ordering rebuilt from the finished Print as Print::_make_wipe_tower does, counted after
    // the constructor and after sort_and_build_data. A full rebuild next to a short print ordering
    // means the layers were read before they held their extrusions.
    {
        auto layers_with_extruders = [](const std::vector<LayerTools> &tools) {
            return std::count_if(tools.begin(), tools.end(), [](const LayerTools &lt) { return !lt.extruders.empty(); });
        };
        try {
            const bool   priming = print.wipe_tower_type() == WipeTowerType::Type2;
            ToolOrdering rebuilt(print, (unsigned int) -1, priming);
            const auto   collected = layers_with_extruders(rebuilt.layer_tools());
            rebuilt.sort_and_build_data(print, (unsigned int) -1, priming);
            out += "; rebuilt from the finished Print: layers with extruders " + std::to_string(collected) + " collected, " +
                   std::to_string(layers_with_extruders(rebuilt.layer_tools())) + " sorted";
        } catch (const std::exception &ex) {
            out += std::string("; rebuilt from the finished Print: threw ") + ex.what();
        }
    }
    return out;
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

// What feature_feedrates saw, for the message of a failed check: the layers counted, every tool
// change with the layer it was read on, and per tool the extruding moves above the first layer,
// split into those under a feature label and those before any label.
std::string gcode_digest(const std::string &gcode)
{
    std::istringstream in(executable_block(gcode));
    std::string        line, feature, changes;
    std::map<int, int> labelled, unlabelled;
    int                tool  = 0;
    int                layer = 0;
    while (std::getline(in, line)) {
        if (line.rfind(";LAYER_CHANGE", 0) == 0 || line.rfind("; CHANGE_LAYER", 0) == 0) { ++layer; continue; }
        if (line.rfind(";TYPE:", 0) == 0) { feature = line.substr(6); continue; }
        if (line.rfind("; FEATURE:", 0) == 0) { feature = line.substr(10); continue; }
        if (const int changed_to = tool_change(line); changed_to >= 0) {
            tool = changed_to;
            changes += (changes.empty() ? "" : " ") + std::to_string(layer) + ":T" + std::to_string(tool);
            continue;
        }
        if (line.rfind("G1 ", 0) == 0 && layer > 1 && word_value(line, 'E') > 0. && (word_value(line, 'X') >= 0. || word_value(line, 'Y') >= 0.))
            ++(feature.empty() ? unlabelled : labelled)[tool];
    }
    std::string out = "layers " + std::to_string(layer) + "; tool changes (layer:tool) " + changes + "; extruding moves above layer 1 under a feature label:";
    for (const auto &[t, n] : labelled)
        out += " T" + std::to_string(t) + "=" + std::to_string(n);
    out += "; before any label:";
    for (const auto &[t, n] : unlabelled)
        out += " T" + std::to_string(t) + "=" + std::to_string(n);
    return out;
}

// Keeps the G-code of a failed slice check in the system temp directory for inspection and
// answers its path (for the failure message). Nothing removes the file.
std::string keep_gcode(const std::string &name, const std::string &gcode)
{
    const boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path(name + "-%%%%%%%%.gcode");
    std::ofstream out(path.string(), std::ios::binary);
    out << gcode;
    return path.string();
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

// An unindexed vector variable in custom G-code (travel_speed in the U1's change_filament_gcode)
// reads the slot of the current filament's tool head; filament_map is 1-based.
TEST_CASE("A custom G-code's unindexed vector variable reads the tool head of the current filament", "[PerHeadProcess][PerHeadOverride][pho_placeholder_head]")
{
    DynamicPrintConfig config = four_head_config();
    const std::vector<double> travel_speed = {500., 400., 300., 200.};
    set_head_values(config, "travel_speed", travel_speed);
    // The block is evaluated for the filament changed in; the writer's T command follows it.
    config.set_key_value("change_filament_gcode", new ConfigOptionString("; TOOLCHANGE_TRAVEL {travel_speed}\n"));

    std::istringstream in(executable_block(two_head_gcode(config)));
    std::string        line;
    double             pending = -1.;
    size_t             blocks  = 0;
    std::set<int>      tools;
    while (std::getline(in, line)) {
        if (line.rfind("; TOOLCHANGE_TRAVEL ", 0) == 0) {
            pending = std::atof(line.c_str() + 20);
            continue;
        }
        if (const int changed_to = tool_change(line); changed_to >= 0 && pending >= 0.) {
            REQUIRE(changed_to >= 0);
            REQUIRE(changed_to < int(HEADS));
            INFO("tool " << changed_to << " changed in with travel speed " << pending);
            CHECK_THAT(pending, Catch::Matchers::WithinAbs(travel_speed[size_t(changed_to)], 1e-6));
            tools.insert(changed_to);
            ++blocks;
            pending = -1.;
        }
    }
    // Both tool heads of the plate are changed in at least once.
    CHECK(blocks >= 2);
    CHECK(tools == std::set<int>{0, 1});
}

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

// The per-type layout of flow_columns_config(): one column per (tool head x flow type), ids
// 1,1,2,2,3,3,4,4, Standard then High Flow per head, so that every one of the 8 slots of the
// narrowing is an exact match and a value can be given to one slot alone.
void per_type_process(DynamicPrintConfig &config)
{
    std::vector<int>         ids;
    std::vector<std::string> variants;
    for (size_t head = 0; head < HEADS; ++head)
        for (const char *variant : {STANDARD, HIGH_FLOW}) {
            ids.emplace_back(int(head) + 1);
            variants.emplace_back(variant);
        }
    config.set_key_value("print_extruder_id",      new ConfigOptionInts(ids));
    config.set_key_value("print_extruder_variant", new ConfigOptionStrings(variants));
    for (const std::string &key : print_options_with_variant) {
        if (key == "print_extruder_id" || key == "print_extruder_variant")
            continue;
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        REQUIRE(option != nullptr);
        option->resize(HEADS * 2);
    }
}

// One value per slot (8) for a process key.
void set_slot_values(DynamicPrintConfig &config, const std::string &key, const std::vector<double> &values)
{
    REQUIRE(values.size() == HEADS * 2);
    auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
    REQUIRE(option != nullptr);
    const std::unique_ptr<ConfigOption> parsed(option->clone());
    for (size_t slot = 0; slot < values.size(); ++slot) {
        REQUIRE(parsed->deserialize(float_to_string_decimal_point(values[slot])));
        option->set_at(parsed.get(), slot, 0);
    }
}

// The feed rates (mm/min) of the non-extruding XY moves per tool: above the first layer outside
// the prime tower blocks (the writer's travels), and the maximum inside the tower blocks (the
// tower's travels; the tool changes inside a block, a move belongs to the tool active at it).
struct TravelFacts
{
    std::map<int, std::set<int>> writer_travels;
    std::map<int, int>           tower_max_travel;
};
TravelFacts travel_facts(const std::string &gcode)
{
    TravelFacts        out;
    std::istringstream in(executable_block(gcode));
    std::string        line;
    int                tool     = 0;
    int                layer    = 0;
    double             feedrate = 0.;
    bool               in_tower = false;
    while (std::getline(in, line)) {
        if (line.rfind(";LAYER_CHANGE", 0) == 0 || line.rfind("; CHANGE_LAYER", 0) == 0) { ++layer; continue; }
        if (line.rfind("; CP TOOLCHANGE START", 0) == 0) { in_tower = true; continue; }
        if (line.rfind("; CP TOOLCHANGE END", 0) == 0) { in_tower = false; continue; }
        if (const int changed_to = tool_change(line); changed_to >= 0) {
            tool = changed_to;
            continue;
        }
        if (line.rfind("G1 ", 0) != 0)
            continue;
        const double f = word_value(line, 'F');
        if (f > 0.)
            feedrate = f;
        const bool moves    = word_value(line, 'X') >= 0. || word_value(line, 'Y') >= 0.;
        const bool extrudes = word_value(line, 'E') >= 0.;
        if (!moves || extrudes || f <= 0.)
            continue;
        if (in_tower)
            out.tower_max_travel[tool] = std::max(out.tower_max_travel[tool], int(std::lround(feedrate)));
        else if (layer > 1)
            out.writer_travels[tool].insert(int(std::lround(feedrate)));
    }
    return out;
}

} // namespace

// On a table with one slot per (tool head x flow type), travel and first-layer speeds are read
// from the tool head's slot, not at the tool head's index (another slot there).
TEST_CASE("The writer and the prime tower read the travel and first-layer speeds in the slot of the tool head", "[PerHeadProcess][PerHeadOverride][pho_slot_travel]")
{
    DynamicPrintConfig config = flow_columns_config();
    per_type_process(config);
    with_prime_tower(config);
    // Slots: (1,S) (1,HF) (2,S) (2,HF) (3,S) (3,HF) (4,S) (4,HF); tool head 3 is on High Flow.
    const std::vector<double> travel_speed        = {500., 510., 400., 410., 300., 310., 200., 210.};
    const std::vector<double> initial_layer_speed = {50., 51., 40., 41., 30., 31., 20., 21.};
    set_slot_values(config, "travel_speed", travel_speed);
    set_slot_values(config, "initial_layer_speed", initial_layer_speed);
    // No first-layer travel factor and no z travel speed of their own: every travel reads travel_speed.
    for (const char *key : {"initial_layer_travel_speed"}) {
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        const std::unique_ptr<ConfigOption> parsed(option->clone());
        REQUIRE(parsed->deserialize("100%"));
        for (size_t slot = 0; slot < HEADS * 2; ++slot)
            option->set_at(parsed.get(), slot, 0);
    }
    set_slot_values(config, "travel_speed_z", std::vector<double>(HEADS * 2, 0.));
    const double max_purge_speed = config.opt_float("wipe_tower_max_purge_speed");

    // The first cube on tool head 2 (Standard, slot 2), the second on tool head 3 (High Flow, slot 5).
    const std::string gcode = two_head_gcode(config, {{{"extruder", "2"}}, {{"extruder", "3"}}});
    const std::map<int, size_t> slot_of_tool = {{1, 2}, {2, 5}};
    const TravelFacts facts = travel_facts(gcode);

    SECTION("the writer's travels of each tool run at the travel speed of its slot and at no other slot's") {
        for (const auto &[tool, slot] : slot_of_tool) {
            REQUIRE(facts.writer_travels.count(tool) == 1);
            const std::set<int> &rates = facts.writer_travels.at(tool);
            INFO("tool " << tool << " travel feed rates: " << joined(rates));
            CHECK(rates.count(int(std::lround(travel_speed[slot] * 60.))) == 1);
            for (size_t other = 0; other < travel_speed.size(); ++other)
                if (other != slot)
                    CHECK(rates.count(int(std::lround(travel_speed[other] * 60.))) == 0);
        }
    }

    SECTION("the prime tower travels at the travel speed of the slot of the tool it runs") {
        for (const auto &[tool, slot] : slot_of_tool) {
            REQUIRE(facts.tower_max_travel.count(tool) == 1);
            INFO("tool " << tool << " fastest travel on the tower: " << facts.tower_max_travel.at(tool));
            CHECK(facts.tower_max_travel.at(tool) == int(std::lround(travel_speed[slot] * 60.)));
        }
    }

    SECTION("the prime tower purges a tool on the first layer at the first layer speed of its slot") {
        const std::vector<WipeBlock> blocks = wipe_blocks(gcode);
        bool first_layer_seen = false;
        for (const WipeBlock &block : blocks) {
            if (block.layer > 1 || slot_of_tool.count(block.tool) == 0)
                continue;
            first_layer_seen = true;
            INFO("tool " << block.tool << " on layer " << block.layer);
            CHECK(block.first_feedrate == first_wipe_feedrate(initial_layer_speed[slot_of_tool.at(block.tool)], true, max_purge_speed));
        }
        CHECK(first_layer_seen);
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

    const std::unique_ptr<TwoHeadSlice> slice = two_head_slice(config);
    const std::string                  &gcode = slice->gcode;
    const auto                          rates = feature_feedrates(gcode);
    // Under CPU load the G-code can hold the first layer alone. The details go in the FAIL message:
    // Catch2 3 can drop earlier INFO scopes before the failure reports.
    if (rates.count(0) != 1 || rates.count(1) != 1)
        FAIL("no extrusion above the first layer for tool " << (rates.count(0) != 1 ? 0 : 1) << ": " << gcode_digest(gcode) << "; "
             << print_digest(slice->print) << "; G-code kept at " << keep_gcode("phs_gcode_head2", gcode));
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

namespace {

// The wide layout of a single Standard column on the four-head printer: a shared column with id 0
// and one column per tool head (ids 0,1,2,3,4), every key five wide with the shared value.
void wide_single_flow(DynamicPrintConfig &config)
{
    config.set_key_value("print_extruder_id",      new ConfigOptionInts({0, 1, 2, 3, 4}));
    config.set_key_value("print_extruder_variant", new ConfigOptionStrings(std::vector<std::string>(HEADS + 1, STANDARD)));
    for (const std::string &key : print_options_with_variant) {
        if (key == "print_extruder_id" || key == "print_extruder_variant")
            continue;
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        REQUIRE(option != nullptr);
        option->resize(1);
        option->resize(HEADS + 1);
    }
    config.set_key_value(PerHeadProcess::override_key, new ConfigOptionStrings(std::vector<std::string>(HEADS + 1, std::string())));
}

void set_wide_column(DynamicPrintConfig &config, const std::string &key, size_t column, double value)
{
    auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
    REQUIRE(option != nullptr);
    const std::unique_ptr<ConfigOption> parsed(option->clone());
    REQUIRE(parsed->deserialize(float_to_string_decimal_point(value)));
    option->set_at(parsed.get(), column, 0);
}

} // namespace

// A value set for one tool head reaches that head's G-code and no other head's.
TEST_CASE("A value set for one tool head reaches its G-code alone, under the identity map and under a manual map", "[PerHeadProcess][PerHeadOverride][pho_gcode_head_value]")
{
    DynamicPrintConfig config = four_head_config();
    for (const char *key : {"machine_max_acceleration_extruding", "machine_max_acceleration_x", "machine_max_acceleration_y"})
        config.set_key_value(key, new ConfigOptionFloats(std::vector<double>(HEADS * 2, 20000.)));
    wide_single_flow(config);
    for (size_t column = 0; column <= HEADS; ++column) {
        set_wide_column(config, "outer_wall_speed", column, 200.);
        set_wide_column(config, "outer_wall_acceleration", column, 5000.);
        set_wide_column(config, "inner_wall_speed", column, 200.);
        set_wide_column(config, "default_acceleration", column, 10000.);
    }
    DynamicPrintConfig without = config;
    // Tool head 2 (column 2): outer wall 30 mm/s at 1000 mm/s2, set on the Speed page.
    set_wide_column(config, "outer_wall_speed", 2, 30.);
    set_wide_column(config, "outer_wall_acceleration", 2, 1000.);
    PerHeadProcess::set_head_value(config, 1, "outer_wall_speed", 2);
    PerHeadProcess::set_head_value(config, 1, "outer_wall_acceleration", 2);
    REQUIRE(PerHeadProcess::head_override_keys(config, 1) == std::vector<std::string>{"outer_wall_acceleration", "outer_wall_speed"});

    SECTION("identity map: tool head 2 prints its own outer wall, tool head 1 the shared one, byte for byte the run without the value") {
        const std::string gcode         = two_head_gcode(config);
        const auto        rates         = feature_feedrates(gcode);
        const auto        accelerations = outer_wall_accelerations(gcode);
        REQUIRE(rates.count(0) == 1);
        REQUIRE(rates.count(1) == 1);
        CHECK(rates.at(0).at("Outer wall") == std::set<int>{200 * 60});
        CHECK(rates.at(1).at("Outer wall") == std::set<int>{30 * 60});
        CHECK(accelerations.at(0) == std::set<int>{5000});
        CHECK(accelerations.at(1) == std::set<int>{1000});
        // The head without a value prints exactly as without the value; the head with it differs.
        const auto reference = feature_feedrates(two_head_gcode(without));
        CHECK(rates.at(0) == reference.at(0));
        CHECK(rates.at(1) != reference.at(1));
        CHECK(reference.at(1).at("Outer wall") == std::set<int>{200 * 60});
        // The marker reaches the header and the transient keys do not.
        CHECK(gcode.find("; print_extruder_override") != std::string::npos);
        CHECK(gcode.find("print_extruder_source_flow") == std::string::npos);
        CHECK(gcode.find("print_extruder_flow_count") == std::string::npos);
    }

    SECTION("a manual map with filament 1 on tool head 2: the value follows the tool head, not the filament") {
        config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(fmmManual));
        config.set_key_value("filament_map", new ConfigOptionInts({2, 1, 3, 4}));
        const std::string gcode = two_head_gcode(config);
        const auto        rates = feature_feedrates(gcode);
        REQUIRE(rates.count(0) == 1);
        REQUIRE(rates.count(1) == 1);
        // T0 is filament 1, printed by tool head 2: the head's value; T1 (filament 2) on tool head 1: the shared value.
        CHECK(rates.at(0).at("Outer wall") == std::set<int>{30 * 60});
        CHECK(rates.at(1).at("Outer wall") == std::set<int>{200 * 60});
        const auto accelerations = outer_wall_accelerations(gcode);
        CHECK(accelerations.at(0) == std::set<int>{1000});
        CHECK(accelerations.at(1) == std::set<int>{5000});
    }
}

// An override of a part on a wide preset is read by its width.
TEST_CASE("An override of a part on a preset with values per tool head is read once, by flow or slot by slot", "[PerHeadProcess][PerHeadOverride][pho_override_flow_space]")
{
    // The per-type printer (8 slots, tool head 3 on High Flow) with the flow-only child widened and
    // tool head 2 set to 60 mm/s walls.
    DynamicPrintConfig config = flow_columns_config();
    PerHeadProcess::widen(config, config);
    REQUIRE(config.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{0, 0, 1, 1, 2, 2, 3, 3, 4, 4});
    for (const char *key : {"outer_wall_speed", "inner_wall_speed"}) {
        set_wide_column(config, key, 4, 60.);
        PerHeadProcess::set_head_value(config, 1, key, 4);
    }

    SECTION("plain table: the head value, a one-value override, and a flow-only override by the flow of each head") {
        {
            const auto [head_2, head_3] = wall_feedrates_with_override(config, "");
            CHECK(head_2 == std::set<int>{60 * 60});
            CHECK(head_3 == std::set<int>{500 * 60});
        }
        {
            const auto [head_2, head_3] = wall_feedrates_with_override(config, "90");
            CHECK(head_2 == std::set<int>{90 * 60});
            CHECK(head_3 == std::set<int>{90 * 60});
        }
        {
            const auto [head_2, head_3] = wall_feedrates_with_override(config, "90,400");
            CHECK(head_2 == std::set<int>{90 * 60});
            CHECK(head_3 == std::set<int>{400 * 60});
        }
    }

    SECTION("composed table: the head value beats the source of its size and the flow-only override is read by flow") {
        auto source = std::make_unique<Preset>(Preset::TYPE_PRINT, "0.12mm Standard @Test (0.2 nozzle)");
        source->config.option<ConfigOptionInts>("print_extruder_id", true)->values         = {1};
        source->config.option<ConfigOptionStrings>("print_extruder_variant", true)->values = {STANDARD};
        source->config.option<ConfigOptionFloatsNullable>("outer_wall_speed", true)->values = {70.};
        source->config.option<ConfigOptionFloatsNullable>("inner_wall_speed", true)->values = {70.};
        std::vector<PerHeadProcess::Source> sources(HEADS);
        for (size_t head = 0; head < HEADS; ++head)
            sources[head].head = head;
        sources[1].preset  = source.get();
        sources[1].derived = true;
        sources[1].composed_keys.assign(PerHeadProcess::composed_keys().begin(), PerHeadProcess::composed_keys().end());
        REQUIRE(PerHeadProcess::compose(config, {}, sources));
        REQUIRE(config.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{1, 1, 2, 2, 3, 3, 4, 4});
        // The source columns are the head columns of the wide layout; their flow positions Standard / High Flow.
        CHECK(config.option<ConfigOptionInts>(PerHeadProcess::source_column_key)->values == std::vector<int>{2, 3, 4, 5, 6, 7, 8, 9});
        CHECK(config.option<ConfigOptionInts>(PerHeadProcess::source_flow_key)->values == std::vector<int>{0, 1, 0, 1, 0, 1, 0, 1});
        CHECK(config.opt_int(PerHeadProcess::flow_count_key) == 2);
        {
            const auto [head_2, head_3] = wall_feedrates_with_override(config, "");
            CHECK(head_2 == std::set<int>{60 * 60});
            CHECK(head_3 == std::set<int>{500 * 60});
        }
        {
            const auto [head_2, head_3] = wall_feedrates_with_override(config, "90,400");
            CHECK(head_2 == std::set<int>{90 * 60});
            CHECK(head_3 == std::set<int>{400 * 60});
        }
    }
}

// ---- A plate of four nozzle sizes, sliced -------------------------------------------------------
// Tool heads of 0.2, 0.4 (High Flow), 0.6 and 0.8 mm on the U1 presets under 0.20mm High Quality,
// composed by full_config_for_print; cube i on tool head i. No cooling slowdown or volumetric cap.

namespace {

const char *const OWNER_MACHINE  = "Snapmaker U1 (0.4 nozzle)";
const char *const OWNER_PROCESS  = "0.20mm High Quality @Snapmaker U1 (0.4 nozzle)";
const char *const OWNER_FILAMENT = "Snapmaker PLA Matte @U1";

struct OwnerPlateSlice
{
    PresetBundle                        bundle;
    std::vector<PerHeadProcess::Source> sources;
    DynamicPrintConfig                  config;
    Print                               print;
    Model                               model;
    std::string                         gcode;
};

// `before_compose` edits the bundle once the presets are selected, before the table is composed
// (a value set for a tool head on the Speed page lives in the edited process preset).
std::unique_ptr<OwnerPlateSlice> owner_plate_slice(const std::function<void(PresetBundle &)> &before_compose = {})
{
    auto slice = std::make_unique<OwnerPlateSlice>();
    PresetBundle &bundle = slice->bundle;
    bundle.load_vendor_configs_from_json(PROFILES_DIR, "Snapmaker", PresetBundle::LoadSystem,
                                         ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr, /*allow_cache=*/false);
    REQUIRE(bundle.printers.select_preset_by_name(OWNER_MACHINE, true));
    // As the sidebar sets the sizes: on the edited printer preset; no preferred layer heights.
    bundle.printers.get_edited_preset().config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.2, 0.4, 0.6, 0.8}));
    bundle.printers.get_edited_preset().config.set_key_value("extruder_layer_height", new ConfigOptionFloats(std::vector<double>(HEADS, 0.)));
    REQUIRE(bundle.prints.select_preset_by_name(OWNER_PROCESS, true));
    // The 0.25 mm first layer exceeds the 0.2 mm nozzle, which Print::validate refuses; lowered to
    // the layer height.
    bundle.prints.get_edited_preset().config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    // A 100 % bridge width at the 0.2 mm head equals the layer height, which Print::validate
    // refuses ("Line width too small"); 0 uses the internal solid infill width. No cube bridges.
    bundle.prints.get_edited_preset().config.set_key_value("bridge_line_width", new ConfigOptionFloatOrPercent(0., false));
    // Locked Zag skin / skeleton widths also default to 100 % and are validated for every pattern,
    // refusing the 0.2 mm head; 0 (auto) passes and only Locked Zag uses them.
    bundle.prints.get_edited_preset().config.set_key_value("skin_infill_line_width",     new ConfigOptionFloatOrPercent(0., false));
    bundle.prints.get_edited_preset().config.set_key_value("skeleton_infill_line_width", new ConfigOptionFloatOrPercent(0., false));
    bundle.filament_presets = std::vector<std::string>(HEADS, OWNER_FILAMENT);
    REQUIRE(bundle.filaments.select_preset_by_name(OWNER_FILAMENT, true));
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2, 3, 4};
    bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {int(nvtStandard), int(nvtHighFlow), int(nvtStandard),
                                                                                                  int(nvtStandard)};
    bundle.process_follows_nozzle = true;
    if (before_compose)
        before_compose(bundle);

    DynamicPrintConfig &config = slice->config;
    config = bundle.full_config_for_print(false, std::nullopt, std::nullopt, &slice->sources);
    // The application sizes the colours and the flush volumes with the filament list; nothing is
    // flushed here.
    config.set_key_value("filament_colour",      new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}));
    config.set_key_value("flush_multiplier",     new ConfigOptionFloats({1.}));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(std::vector<double>(HEADS * HEADS, 0.)));
    config.set_key_value("enable_support",       new ConfigOptionBool(false));
    config.set_key_value("enable_prime_tower",   new ConfigOptionBool(false));
    config.set_key_value("skirt_loops",          new ConfigOptionInt(0));
    // The cooling slowdown and the volumetric ceiling of the filament would rewrite the feed rates
    // under test; every column of both is replaced, the width kept.
    {
        auto *slowdown = config.option<ConfigOptionBools>("slow_down_for_layer_cooling");
        REQUIRE(slowdown != nullptr);
        slowdown->values.assign(slowdown->values.size(), 0);
        auto *ceiling = dynamic_cast<ConfigOptionVectorBase *>(config.option("filament_max_volumetric_speed"));
        REQUIRE(ceiling != nullptr);
        std::string values;
        for (size_t column = 0; column < ceiling->size(); ++column)
            values += (values.empty() ? "" : ",") + std::string("200");
        REQUIRE(ceiling->deserialize(values));
    }

    std::vector<TriangleMesh> meshes;
    for (size_t head = 0; head < HEADS; ++head) {
        TriangleMesh cube = mesh(TestMesh::cube_20x20x20);
        cube.scale(Vec3f(1.f, 1.f, 0.25f)); // 5 mm, 25 layers of 0.2 mm
        cube.translate(40.f + 40.f * float(head), 100.f, 0.f);
        meshes.emplace_back(std::move(cube));
    }
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides = {
        {{"extruder", "1"}}, {{"extruder", "2"}}, {{"extruder", "3"}}, {{"extruder", "4"}}};
    init_print(std::move(meshes), slice->print, slice->model, config, &overrides, /*arrange=*/false);
    {
        const StringObjectException err = slice->print.validate();
        INFO(err.string << " (" << err.opt_key << ")");
        REQUIRE(err.string.empty());
    }
    slice->gcode = Slic3r::Test::gcode(slice->print);
    return slice;
}

// The speed of a process key in the column of `preset` that serves `flow` (the column whose
// variant names the flow; the single column of a one-column preset), as a feed rate in mm/min.
int preset_feedrate(const Preset &preset, const std::string &key, NozzleVolumeType flow)
{
    const auto *variants = preset.config.option<ConfigOptionStrings>("print_extruder_variant");
    const auto *option   = dynamic_cast<const ConfigOptionVectorBase *>(preset.config.option(key));
    REQUIRE(variants != nullptr);
    REQUIRE(option != nullptr);
    size_t column = 0;
    for (size_t i = 0; i < variants->values.size(); ++i)
        if (variants->values[i] == (flow == nvtHighFlow ? "Direct Drive High Flow" : STANDARD))
            column = i;
    const std::vector<std::string> values = option->vserialize();
    REQUIRE_FALSE(values.empty());
    return int(std::lround(std::atof(values[column < values.size() ? column : 0].c_str()) * 60.));
}

} // namespace

TEST_CASE("On a plate of four nozzle sizes the 0.6 and 0.8 mm tool heads print their outer walls at the speed of the process preset of their size", "[PerHeadProcess][Profiles][hs_mixed_plate_gcode]")
{
    const std::unique_ptr<OwnerPlateSlice> slice = owner_plate_slice();
    REQUIRE(slice->sources.size() == HEADS);
    for (size_t head : {size_t(2), size_t(3)}) {
        REQUIRE(slice->sources[head].derived);
        REQUIRE(slice->sources[head].preset != nullptr);
    }
    // The plate is High Quality, which neither size has: the Standard presets nearest to its 0.20 mm.
    CHECK(slice->sources[2].preset->name == "0.18mm Standard @Snapmaker U1 (0.6 nozzle)");
    CHECK(slice->sources[3].preset->name == "0.24mm Standard @Snapmaker U1 (0.8 nozzle)");

    const auto rates = feature_feedrates(slice->gcode);
    if (rates.count(2) != 1 || rates.count(3) != 1)
        FAIL("no extrusion above the first layer for tool " << (rates.count(2) != 1 ? 2 : 3) << ": " << gcode_digest(slice->gcode) << "; "
             << print_digest(slice->print) << "; G-code kept at " << keep_gcode("hs_mixed_plate_gcode", slice->gcode));
    for (size_t head : {size_t(2), size_t(3)}) {
        const Preset &source = *slice->sources[head].preset;
        INFO("tool " << head << " (" << source.name << ")");
        REQUIRE(rates.at(int(head)).count("Outer wall") == 1);
        INFO("outer wall: " << joined(rates.at(int(head)).at("Outer wall")));
        CHECK(rates.at(int(head)).at("Outer wall") == std::set<int>{preset_feedrate(source, "outer_wall_speed", nvtStandard)});
    }
    // The 0.8 mm head does not print the selected preset's outer wall speed.
    const Preset &selected = slice->bundle.prints.get_selected_preset();
    CHECK(rates.at(3).at("Outer wall") != std::set<int>{preset_feedrate(selected, "outer_wall_speed", nvtStandard)});
}

// High Flow rule of PerHeadProcess::head_sources: 0.20mm High Quality has no High Flow column, so
// the High Flow head prints the High Flow column of 0.20mm Standard.
TEST_CASE("On a plate of four nozzle sizes the High Flow tool head prints at the High Flow speeds of the preset of its size that has them", "[PerHeadProcess][Profiles][hs_high_flow_gcode]")
{
    const std::unique_ptr<OwnerPlateSlice> slice = owner_plate_slice();
    const Preset *standard_020 = slice->bundle.prints.find_preset("0.20mm Standard @Snapmaker U1 (0.4 nozzle)", false);
    REQUIRE(standard_020 != nullptr);
    REQUIRE(slice->sources.size() == HEADS);
    CHECK(slice->sources[1].derived);

    const auto rates = feature_feedrates(slice->gcode);
    if (rates.size() != HEADS)
        FAIL("tools with extrusions above the first layer: " << rates.size() << ": " << gcode_digest(slice->gcode) << "; G-code kept at "
             << keep_gcode("hs_high_flow_gcode", slice->gcode));
    // The feed rates of every tool, for the record: outer wall and sparse infill.
    for (const auto &[tool, features] : rates) {
        const auto outer  = features.find("Outer wall");
        const auto sparse = features.find("Sparse infill");
        UNSCOPED_INFO("tool " << tool << ": outer wall " << (outer == features.end() ? std::string("-") : joined(outer->second)) << ", sparse infill "
                              << (sparse == features.end() ? std::string("-") : joined(sparse->second)));
    }
    REQUIRE(rates.at(1).count("Outer wall") == 1);
    CHECK(rates.at(1).at("Outer wall") == std::set<int>{preset_feedrate(*standard_020, "outer_wall_speed", nvtHighFlow)});
    REQUIRE(rates.at(1).count("Sparse infill") == 1);
    CHECK(rates.at(1).at("Sparse infill") == std::set<int>{preset_feedrate(*standard_020, "sparse_infill_speed", nvtHighFlow)});
}

// A value set for one tool head on the Speed page beats the preset of its size on that head alone.
TEST_CASE("On a plate of four nozzle sizes an outer wall speed set for the 0.8 mm tool head reaches its G-code alone", "[PerHeadProcess][Profiles][hs_mixed_plate_override]")
{
    const std::unique_ptr<OwnerPlateSlice> slice = owner_plate_slice([](PresetBundle &bundle) {
        // Tool head 4: outer wall 40 mm/s, as the Speed page writes it (widen, the value in the
        // head's columns, the marker).
        DynamicPrintConfig &process = bundle.prints.get_edited_preset().config;
        PerHeadProcess::widen(process, bundle.printers.get_edited_preset().config);
        const std::vector<int> columns = PerHeadProcess::head_columns(process, 3);
        REQUIRE_FALSE(columns.empty());
        set_wide_column(process, "outer_wall_speed", size_t(columns.front()), 40.);
        PerHeadProcess::set_head_value(process, 3, "outer_wall_speed", columns.front());
        REQUIRE(PerHeadProcess::head_override_keys(process, 3) == std::vector<std::string>{"outer_wall_speed"});
    });
    REQUIRE(slice->sources.size() == HEADS);
    REQUIRE(slice->sources[3].derived);
    CHECK(slice->sources[3].overridden_keys == std::vector<std::string>{"outer_wall_speed"});
    CHECK(slice->sources[2].overridden_keys.empty());
    // The widened preset's High Flow column for tool head 2 copies Standard and holds no High Flow
    // values (PerHeadProcess::has_high_flow_values): the head keeps 0.20mm Standard's High Flow column.
    const Preset *standard_020 = slice->bundle.prints.find_preset("0.20mm Standard @Snapmaker U1 (0.4 nozzle)", false);
    REQUIRE(standard_020 != nullptr);
    CHECK(slice->sources[1].reason == PerHeadProcess::Reason::HighFlow);
    REQUIRE(slice->sources[1].preset != nullptr);
    CHECK(slice->sources[1].preset->name == standard_020->name);

    const auto rates = feature_feedrates(slice->gcode);
    if (rates.count(2) != 1 || rates.count(3) != 1)
        FAIL("no extrusion above the first layer for tool " << (rates.count(2) != 1 ? 2 : 3) << ": " << gcode_digest(slice->gcode) << "; "
             << print_digest(slice->print) << "; G-code kept at " << keep_gcode("hs_mixed_plate_override", slice->gcode));
    REQUIRE(rates.at(3).count("Outer wall") == 1);
    CHECK(rates.at(3).at("Outer wall") == std::set<int>{40 * 60});
    // Tool head 2 prints 0.20mm Standard's High Flow outer wall (500 mm/s), not the copied Standard speed.
    REQUIRE(rates.count(1) == 1);
    REQUIRE(rates.at(1).count("Outer wall") == 1);
    CHECK(rates.at(1).at("Outer wall") == std::set<int>{preset_feedrate(*standard_020, "outer_wall_speed", nvtHighFlow)});
    // The 0.6 mm head keeps the outer wall of the preset of its size, the sparse infill of the 0.8 mm head its preset's too.
    REQUIRE(rates.at(2).count("Outer wall") == 1);
    CHECK(rates.at(2).at("Outer wall") == std::set<int>{preset_feedrate(*slice->sources[2].preset, "outer_wall_speed", nvtStandard)});
    REQUIRE(rates.at(3).count("Sparse infill") == 1);
    CHECK(rates.at(3).at("Sparse infill") == std::set<int>{preset_feedrate(*slice->sources[3].preset, "sparse_infill_speed", nvtStandard)});
}

// A preset chosen for the 0.6 mm head applies to that head only. Its sparse infill (150 mm/s)
// tells it apart from the automatic 0.18mm Standard (100 mm/s).
TEST_CASE("On a plate of four nozzle sizes a process preset chosen for the 0.6 mm tool head prints on that tool head alone", "[PerHeadProcess][Profiles][hs_chosen_plate_gcode]")
{
    const char *const chosen_name = "0.24mm Standard @Snapmaker U1 (0.6 nozzle)";
    const std::unique_ptr<OwnerPlateSlice> slice = owner_plate_slice([chosen_name](PresetBundle &bundle) {
        PerHeadProcess::set_chosen(bundle, 2, chosen_name);
    });
    REQUIRE(slice->sources.size() == HEADS);
    REQUIRE(slice->sources[2].derived);
    REQUIRE(slice->sources[2].preset != nullptr);
    CHECK(slice->sources[2].chosen_state == PerHeadProcess::ChosenState::Applied);
    CHECK(slice->sources[2].step == PerHeadProcess::Step::Chosen);
    CHECK(slice->sources[2].preset->name == chosen_name);
    REQUIRE(slice->sources[2].automatic != nullptr);
    CHECK(slice->sources[2].automatic->name == "0.18mm Standard @Snapmaker U1 (0.6 nozzle)");
    // The other tool heads follow the rule as without the choice.
    CHECK(slice->sources[3].chosen_state == PerHeadProcess::ChosenState::None);
    CHECK(slice->sources[3].preset->name == "0.24mm Standard @Snapmaker U1 (0.8 nozzle)");
    CHECK(slice->sources[1].reason == PerHeadProcess::Reason::HighFlow);

    const auto rates = feature_feedrates(slice->gcode);
    if (rates.count(2) != 1 || rates.count(3) != 1)
        FAIL("no extrusion above the first layer for tool " << (rates.count(2) != 1 ? 2 : 3) << ": " << gcode_digest(slice->gcode) << "; "
             << print_digest(slice->print) << "; G-code kept at " << keep_gcode("hs_chosen_plate_gcode", slice->gcode));
    const Preset &chosen = *slice->sources[2].preset;
    REQUIRE(rates.at(2).count("Sparse infill") == 1);
    INFO("T2 sparse infill: " << joined(rates.at(2).at("Sparse infill")));
    CHECK(rates.at(2).at("Sparse infill") == std::set<int>{preset_feedrate(chosen, "sparse_infill_speed", nvtStandard)});
    CHECK(rates.at(2).at("Sparse infill") == std::set<int>{150 * 60});
    REQUIRE(rates.at(2).count("Outer wall") == 1);
    CHECK(rates.at(2).at("Outer wall") == std::set<int>{preset_feedrate(chosen, "outer_wall_speed", nvtStandard)});
    // T3 keeps the sparse infill of its automatic preset (100 mm/s), T1 the High Flow outer wall of 0.20mm Standard.
    REQUIRE(rates.at(3).count("Sparse infill") == 1);
    CHECK(rates.at(3).at("Sparse infill") == std::set<int>{preset_feedrate(*slice->sources[3].preset, "sparse_infill_speed", nvtStandard)});
    CHECK(rates.at(3).at("Sparse infill") == std::set<int>{100 * 60});
    REQUIRE(rates.count(1) == 1);
    REQUIRE(rates.at(1).count("Outer wall") == 1);
    CHECK(rates.at(1).at("Outer wall") == std::set<int>{preset_feedrate(*slice->sources[1].preset, "outer_wall_speed", nvtHighFlow)});
    // The record of the plate names the chosen preset for tool head 3.
    PerHeadProcess::record_sources(slice->bundle);
    const auto *record = slice->bundle.project_config.option<ConfigOptionStrings>(PerHeadProcess::record_key);
    REQUIRE(record != nullptr);
    REQUIRE(record->values.size() == HEADS);
    CHECK(record->values[2] == chosen_name);
    CHECK(slice->bundle.project_config.option<ConfigOptionStrings>(PerHeadProcess::choice_key)->values == std::vector<std::string>{"", "", chosen_name});
}

// Flow toggle (PerHeadProcess::flow_key): the High Flow head set to Standard prints the selected
// preset's Standard column (60 mm/s), not 0.20mm Standard's High Flow column (500 mm/s).
TEST_CASE("On a plate of four nozzle sizes the High Flow tool head set to the Standard speeds prints the selected preset's Standard column", "[PerHeadProcess][Profiles][hs_flow_choice_gcode]")
{
    const std::unique_ptr<OwnerPlateSlice> slice = owner_plate_slice([](PresetBundle &bundle) { PerHeadProcess::set_chosen_flow(bundle, 1, nvtStandard); });
    REQUIRE(slice->sources.size() == HEADS);
    CHECK(slice->sources[1].flow_chosen);
    CHECK(slice->sources[1].flow == nvtStandard);
    CHECK_FALSE(slice->sources[1].derived);
    CHECK(slice->sources[1].reason == PerHeadProcess::Reason::HomeSize);
    for (size_t head : {size_t(0), size_t(2), size_t(3)}) {
        REQUIRE(slice->sources[head].derived);
        REQUIRE(slice->sources[head].preset != nullptr);
        CHECK_FALSE(slice->sources[head].flow_chosen);
    }
    CHECK(slice->sources[2].preset->name == "0.18mm Standard @Snapmaker U1 (0.6 nozzle)");
    CHECK(slice->sources[3].preset->name == "0.24mm Standard @Snapmaker U1 (0.8 nozzle)");

    const auto rates = feature_feedrates(slice->gcode);
    if (rates.size() != HEADS)
        FAIL("tools with extrusions above the first layer: " << rates.size() << ": " << gcode_digest(slice->gcode) << "; G-code kept at "
             << keep_gcode("hs_flow_choice_gcode", slice->gcode));
    // The feed rates of every tool, for the record: outer wall and sparse infill.
    for (const auto &[tool, features] : rates) {
        const auto outer  = features.find("Outer wall");
        const auto sparse = features.find("Sparse infill");
        UNSCOPED_INFO("tool " << tool << ": outer wall " << (outer == features.end() ? std::string("-") : joined(outer->second)) << ", sparse infill "
                              << (sparse == features.end() ? std::string("-") : joined(sparse->second)));
    }
    const Preset &selected = slice->bundle.prints.get_selected_preset();
    REQUIRE(rates.at(1).count("Outer wall") == 1);
    CHECK(rates.at(1).at("Outer wall") == std::set<int>{preset_feedrate(selected, "outer_wall_speed", nvtStandard)});
    CHECK(rates.at(1).at("Outer wall") == std::set<int>{60 * 60});
    REQUIRE(rates.at(1).count("Sparse infill") == 1);
    CHECK(rates.at(1).at("Sparse infill") == std::set<int>{preset_feedrate(selected, "sparse_infill_speed", nvtStandard)});
    // T0, T2 and T3 print the Standard column of the preset of their size, as without the entry.
    for (size_t head : {size_t(0), size_t(2), size_t(3)}) {
        const Preset &source = *slice->sources[head].preset;
        INFO("tool " << head << " (" << source.name << ")");
        REQUIRE(rates.at(int(head)).count("Outer wall") == 1);
        CHECK(rates.at(int(head)).at("Outer wall") == std::set<int>{preset_feedrate(source, "outer_wall_speed", nvtStandard)});
    }
    // The record names the derived heads; a chosen flow is no preset and stays in its own key.
    PerHeadProcess::record_sources(slice->bundle);
    const auto *record = slice->bundle.project_config.option<ConfigOptionStrings>(PerHeadProcess::record_key);
    REQUIRE(record != nullptr);
    REQUIRE(record->values.size() == HEADS);
    CHECK(record->values[1].empty());
    CHECK(record->values[2] == "0.18mm Standard @Snapmaker U1 (0.6 nozzle)");
    CHECK(slice->bundle.project_config.option<ConfigOptionStrings>(PerHeadProcess::flow_key)->values == std::vector<std::string>{"", "Standard"});
}
