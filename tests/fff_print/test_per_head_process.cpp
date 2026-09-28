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
#include "libslic3r/NozzleFilamentPresets.hpp"
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
// (a value set for a tool head on the Speed page lives in the edited process preset). `overrides`
// replaces the per-object overrides (cube i on tool head i by default). With `slice_plate` false
// the plate is applied but neither validated nor sliced: the caller reads Print::validate itself.
std::unique_ptr<OwnerPlateSlice> owner_plate_slice(const std::function<void(PresetBundle &)> &before_compose = {},
                                                   const std::vector<std::vector<ConfigBase::SetDeserializeItem>> *overrides = nullptr,
                                                   bool slice_plate = true)
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
    bundle.prints.get_edited_preset().config.set_key_value("bridge_line_width", new ConfigOptionFloatsOrPercentsNullable{FloatOrPercent(0., false)});
    // The skin and skeleton line widths of the Locked Zag infill keep their default of 100 % of the
    // nozzle; Print::validate checks them for a Locked Zag region alone (no cube of the plate has
    // one), so the 0.2 mm head is not refused for a field the Strength page shows for that pattern only.
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
    // The solid widths are read as a band (prints_width): no Arachne beads of the narrow-region reroute.
    config.set_key_value("detect_narrow_internal_solid_infill", new ConfigOptionBool(false));
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
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> default_overrides = {
        {{"extruder", "1"}}, {{"extruder", "2"}}, {{"extruder", "3"}}, {{"extruder", "4"}}};
    init_print(std::move(meshes), slice->print, slice->model, config, overrides != nullptr ? overrides : &default_overrides, /*arrange=*/false);
    if (!slice_plate)
        return slice;
    {
        const StringObjectException err = slice->print.validate();
        INFO(err.string << " (" << err.opt_key << ", tool head " << err.tool_head << ")");
        REQUIRE(err.string.empty());
    }
    slice->gcode = Slic3r::Test::gcode(slice->print);
    return slice;
}

// Writes the text of a value ("1.1", "105%") into column `column` of a key of the edited preset.
void set_wide_column_text(DynamicPrintConfig &config, const std::string &key, size_t column, const std::string &value)
{
    auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
    REQUIRE(option != nullptr);
    const std::unique_ptr<ConfigOption> parsed(option->clone());
    REQUIRE(parsed->deserialize(value));
    option->set_at(parsed.get(), column, 0);
}

// A width set for tool head `head` in the edited process preset, as the Quality page writes it
// (widen, the value in the head's columns, the marker).
void set_head_width(PresetBundle &bundle, size_t head, const std::string &key, const std::string &value)
{
    DynamicPrintConfig &process = bundle.prints.get_edited_preset().config;
    PerHeadProcess::widen(process, bundle.printers.get_edited_preset().config);
    const std::vector<int> columns = PerHeadProcess::head_columns(process, head);
    REQUIRE_FALSE(columns.empty());
    set_wide_column_text(process, key, size_t(columns.front()), value);
    PerHeadProcess::set_head_value(process, head, key, columns.front());
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

// ---- A 0.4 mm High Flow tool head on a printer preset of another size ---------------------------
// Tool heads of 0.6, 0.4 (High Flow), 0.6 and 0.6 mm, cube i on tool head i, sliced once on the 0.6 mm
// U1 preset (which declares no High Flow column) and once on the 0.4 mm one.

namespace {

const char *const MATTE_0_4 = "Snapmaker PLA Matte @U1";
const char *const MATTE_0_6 = "Snapmaker PLA Matte @U1 0.6 nozzle";
const std::vector<double> OFFSIZE_DIAMETERS{0.6, 0.4, 0.6, 0.6};

std::unique_ptr<OwnerPlateSlice> offsize_plate_slice(const char *machine, const char *process)
{
    auto slice = std::make_unique<OwnerPlateSlice>();
    PresetBundle &bundle = slice->bundle;
    bundle.load_vendor_configs_from_json(PROFILES_DIR, "Snapmaker", PresetBundle::LoadSystem,
                                         ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr, /*allow_cache=*/false);
    REQUIRE(bundle.printers.select_preset_by_name(machine, true));
    // As the sidebar sets a size (Sidebar::apply_nozzle_diameter): the diameter, the layer height
    // limits and the per-extruder machine values of the machine preset of that size.
    DynamicPrintConfig &printer = bundle.printers.get_edited_preset().config;
    const Preset       *home    = bundle.printers.find_preset(machine, false, true);
    REQUIRE(home != nullptr);
    const double home_size = NozzleFilament::home_nozzle_size(home->config);
    printer.set_key_value("nozzle_diameter", new ConfigOptionFloats(OFFSIZE_DIAMETERS));
    printer.set_key_value("extruder_layer_height", new ConfigOptionFloats(std::vector<double>(HEADS, 0.)));
    for (size_t head = 0; head < HEADS; ++head) {
        if (std::abs(OFFSIZE_DIAMETERS[head] - home_size) < EPSILON)
            continue;
        const Preset *size_preset = NozzleFilament::head_machine_preset(bundle.printers, printer.opt_string("printer_model"), OFFSIZE_DIAMETERS[head]);
        REQUIRE(size_preset != nullptr);
        for (const char *key : {"min_layer_height", "max_layer_height"}) {
            std::vector<double> limits = printer.option<ConfigOptionFloats>(key)->values;
            limits.resize(HEADS, limits.empty() ? 0. : limits.back());
            limits[head] = size_preset->config.option<ConfigOptionFloats>(key)->get_at(head);
            printer.set_key_value(key, new ConfigOptionFloats(limits));
        }
        adopt_extruder_values_from_size_preset(printer, size_preset->config, &home->config, head, nozzle_size_extruder_options());
    }
    REQUIRE(bundle.prints.select_preset_by_name(process, true));
    bundle.filament_presets = {MATTE_0_6, MATTE_0_4, MATTE_0_6, MATTE_0_6};
    REQUIRE(bundle.filaments.select_preset_by_name(bundle.filament_presets.front(), true));
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2, 3, 4};
    bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {int(nvtStandard), int(nvtHighFlow), int(nvtStandard),
                                                                                                  int(nvtStandard)};
    bundle.process_follows_nozzle = true;

    DynamicPrintConfig &config = slice->config;
    config = bundle.full_config_for_print(false, std::nullopt, std::nullopt, &slice->sources);
    config.set_key_value("filament_colour",      new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}));
    config.set_key_value("flush_multiplier",     new ConfigOptionFloats({1.}));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(std::vector<double>(HEADS * HEADS, 0.)));
    config.set_key_value("enable_support",       new ConfigOptionBool(false));
    config.set_key_value("enable_prime_tower",   new ConfigOptionBool(false));
    config.set_key_value("skirt_loops",          new ConfigOptionInt(0));
    config.set_key_value("detect_narrow_internal_solid_infill", new ConfigOptionBool(false));
    // The cooling slowdown and the volumetric ceiling would rewrite the feed rates under test.
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
        cube.scale(Vec3f(1.f, 1.f, 0.25f)); // 5 mm
        cube.translate(40.f + 40.f * float(head), 100.f, 0.f);
        meshes.emplace_back(std::move(cube));
    }
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides = {
        {{"extruder", "1"}}, {{"extruder", "2"}}, {{"extruder", "3"}}, {{"extruder", "4"}}};
    init_print(std::move(meshes), slice->print, slice->model, config, &overrides, /*arrange=*/false);
    {
        const StringObjectException err = slice->print.validate();
        INFO(err.string << " (" << err.opt_key << ", tool head " << err.tool_head << ")");
        REQUIRE(err.string.empty());
    }
    slice->gcode = Slic3r::Test::gcode(slice->print);
    return slice;
}

// A value of the Standard or High Flow column of a shipped two-column filament preset.
int filament_column_int(const PresetBundle &bundle, const std::string &preset_name, const std::string &key, NozzleVolumeType flow)
{
    const Preset *preset = bundle.filaments.find_preset(preset_name, false);
    REQUIRE(preset != nullptr);
    const auto *variants = preset->config.option<ConfigOptionStrings>("filament_extruder_variant");
    const auto *option   = preset->config.option<ConfigOptionInts>(key);
    REQUIRE(variants != nullptr);
    REQUIRE(option != nullptr);
    REQUIRE(variants->values == std::vector<std::string>{STANDARD, "Direct Drive High Flow"});
    REQUIRE(option->values.size() == 2);
    return option->values[flow == nvtHighFlow ? 1 : 0];
}

} // namespace

TEST_CASE("A 0.4 mm High Flow tool head on the 0.6 mm printer preset prints the High Flow values of the 0.4 mm presets", "[PerHeadProcess][HighFlow][Profiles][hf_offsize_gcode]")
{
    const std::unique_ptr<OwnerPlateSlice> slice = offsize_plate_slice("Snapmaker U1 (0.6 nozzle)", "0.24mm Standard @Snapmaker U1 (0.6 nozzle)");
    const Preset *standard_020 = slice->bundle.prints.find_preset("0.20mm Standard @Snapmaker U1 (0.4 nozzle)", false);
    REQUIRE(standard_020 != nullptr);
    // The printer preset declares no High Flow column; the machine preset of 0.4 mm does.
    const auto *declared = slice->bundle.printers.get_edited_preset().config.option<ConfigOptionStrings>("extruder_variant_list");
    REQUIRE(declared != nullptr);
    for (const std::string &variants : declared->values)
        REQUIRE(variants.find("High Flow") == std::string::npos);

    // Process: the High Flow rule gives the 0.4 mm head the preset of its size with a High Flow column.
    REQUIRE(slice->sources.size() == HEADS);
    REQUIRE(slice->sources[1].preset != nullptr);
    CHECK(slice->sources[1].preset->name == standard_020->name);
    const auto rates = feature_feedrates(slice->gcode);
    if (rates.count(1) != 1)
        FAIL("no extrusion above the first layer for tool 1: " << gcode_digest(slice->gcode) << "; G-code kept at " << keep_gcode("hf_offsize_gcode", slice->gcode));
    REQUIRE(rates.at(1).count("Outer wall") == 1);
    CHECK(rates.at(1).at("Outer wall") == std::set<int>{500 * 60});
    CHECK(rates.at(1).at("Outer wall") == std::set<int>{preset_feedrate(*standard_020, "outer_wall_speed", nvtHighFlow)});
    REQUIRE(rates.at(1).count("Sparse infill") == 1);
    CHECK(rates.at(1).at("Sparse infill") == std::set<int>{preset_feedrate(*standard_020, "sparse_infill_speed", nvtHighFlow)});

    // Filament: the High Flow column of the 0.4 mm preset on tool head 2, the Standard column elsewhere.
    const int high_flow_temperature = filament_column_int(slice->bundle, MATTE_0_4, "nozzle_temperature", nvtHighFlow);
    REQUIRE(high_flow_temperature != filament_column_int(slice->bundle, MATTE_0_4, "nozzle_temperature", nvtStandard));
    CHECK(slice->print.config().nozzle_temperature.get_at(1) == high_flow_temperature);

    // Filament: the High Flow retraction length of the 0.4 mm filament overrides the machine value.
    const Preset *matte_0_4 = slice->bundle.filaments.find_preset(MATTE_0_4, false);
    REQUIRE(matte_0_4 != nullptr);
    const auto *filament_retraction = matte_0_4->config.option<ConfigOptionFloatsNullable>("filament_retraction_length");
    REQUIRE(filament_retraction != nullptr);
    REQUIRE(filament_retraction->size() == 2);
    REQUIRE(!filament_retraction->is_nil(1));
    CHECK_THAT(slice->print.config().retraction_length.get_at(1), Catch::Matchers::WithinAbs(filament_retraction->get_at(1), 1e-9));

    // Machine: the retraction minimum travel (no filament override) of the 0.4 mm preset (its High
    // Flow column equals its Standard one), not the 0.6 mm preset's value of tool head 1.
    const Preset *machine_0_4 = slice->bundle.printers.find_preset("Snapmaker U1 (0.4 nozzle)", false, true);
    REQUIRE(machine_0_4 != nullptr);
    const auto *filament_travel = matte_0_4->config.option<ConfigOptionFloatsNullable>("filament_retraction_minimum_travel");
    REQUIRE(filament_travel != nullptr);
    REQUIRE(filament_travel->size() == 2);
    REQUIRE(filament_travel->is_nil(1));
    const double travel_0_4 = machine_0_4->config.option<ConfigOptionFloats>("retraction_minimum_travel")->get_at(1);
    REQUIRE(std::abs(travel_0_4 - slice->print.config().retraction_minimum_travel.get_at(0)) > 0.05);
    CHECK_THAT(slice->print.config().retraction_minimum_travel.get_at(1), Catch::Matchers::WithinAbs(travel_0_4, 1e-9));
    // The narrowed printer table names the flow the head prints.
    CHECK(slice->print.config().printer_extruder_variant.values ==
          std::vector<std::string>{STANDARD, "Direct Drive High Flow", STANDARD, STANDARD});
}

TEST_CASE("A 0.4 mm High Flow tool head prints the same feed rates on the 0.6 mm and on the 0.4 mm printer preset", "[PerHeadProcess][HighFlow][Profiles][hf_offsize_same]")
{
    const std::unique_ptr<OwnerPlateSlice> on_0_6 = offsize_plate_slice("Snapmaker U1 (0.6 nozzle)", "0.24mm Standard @Snapmaker U1 (0.6 nozzle)");
    const std::unique_ptr<OwnerPlateSlice> on_0_4 = offsize_plate_slice("Snapmaker U1 (0.4 nozzle)", "0.20mm Standard @Snapmaker U1 (0.4 nozzle)");
    const auto rates_0_6 = feature_feedrates(on_0_6->gcode);
    const auto rates_0_4 = feature_feedrates(on_0_4->gcode);
    REQUIRE(rates_0_6.count(1) == 1);
    REQUIRE(rates_0_4.count(1) == 1);
    for (const auto &[feature, rates] : rates_0_6.at(1))
        UNSCOPED_INFO("0.6 mm preset, tool 1, " << feature << ": " << joined(rates));
    for (const auto &[feature, rates] : rates_0_4.at(1))
        UNSCOPED_INFO("0.4 mm preset, tool 1, " << feature << ": " << joined(rates));
    // The features of the High Flow column; outer wall and sparse infill print on every cube.
    REQUIRE(rates_0_6.at(1).count("Outer wall") == 1);
    REQUIRE(rates_0_6.at(1).count("Sparse infill") == 1);
    for (const char *feature : {"Outer wall", "Inner wall", "Sparse infill", "Internal solid infill"}) {
        INFO(feature);
        CHECK(rates_0_6.at(1).count(feature) == rates_0_4.at(1).count(feature));
        if (rates_0_6.at(1).count(feature) == 1 && rates_0_4.at(1).count(feature) == 1)
            CHECK(rates_0_6.at(1).at(feature) == rates_0_4.at(1).at(feature));
    }
    CHECK(on_0_6->print.config().nozzle_temperature.get_at(1) == on_0_4->print.config().nozzle_temperature.get_at(1));
    CHECK_THAT(on_0_6->print.config().retraction_length.get_at(1), Catch::Matchers::WithinAbs(on_0_4->print.config().retraction_length.get_at(1), 1e-9));
    CHECK(on_0_6->print.config().printer_extruder_variant.get_at(1) == on_0_4->print.config().printer_extruder_variant.get_at(1));
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

// ---- Line widths per tool head: the non-uniform column harness ------------------------------------
// Distinct widths written straight into the narrowed table expose a reader of the wrong column in
// the ;WIDTH: tags; equal percents on distinct nozzles expose one resolving against the wrong nozzle.

namespace {

// The widths (mm) in effect for the extruding moves of every feature type, per tool, on the layers
// first_layer..last_layer (1 = the first layer; last_layer 0 = to the end): the ;WIDTH: tag (";
// LINE_WIDTH: " on a BBL printer) GCode::_extrude writes on every width change.
std::map<int, std::map<std::string, std::set<double>>> feature_widths(const std::string &gcode, int first_layer, int last_layer = 0)
{
    std::map<int, std::map<std::string, std::set<double>>> out;
    std::istringstream in(executable_block(gcode));
    std::string        line, feature;
    int                tool  = 0;
    int                layer = 0;
    double             width = -1.;
    while (std::getline(in, line)) {
        if (line.rfind(";LAYER_CHANGE", 0) == 0 || line.rfind("; CHANGE_LAYER", 0) == 0) { ++layer; continue; }
        if (line.rfind(";TYPE:", 0) == 0) { feature = line.substr(6); continue; }
        if (line.rfind("; FEATURE:", 0) == 0) { feature = line.substr(10); continue; }
        if (line.rfind(";WIDTH:", 0) == 0) { width = std::atof(line.c_str() + 7); continue; }
        if (line.rfind("; LINE_WIDTH: ", 0) == 0) { width = std::atof(line.c_str() + 14); continue; }
        if (const int changed_to = tool_change(line); changed_to >= 0) {
            tool = changed_to;
            continue;
        }
        if (line.rfind("G1 ", 0) == 0 && layer >= first_layer && (last_layer == 0 || layer <= last_layer) && width > 0. && !feature.empty() &&
            word_value(line, 'E') > 0. && (word_value(line, 'X') >= 0. || word_value(line, 'Y') >= 0.))
            out[tool][feature].insert(width);
    }
    return out;
}

// One value per tool head for a width key of the narrowed table ("0.42", "105%").
void set_head_widths(DynamicPrintConfig &config, const std::string &key, const std::vector<std::string> &values)
{
    REQUIRE(values.size() == HEADS);
    auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
    REQUIRE(option != nullptr);
    const std::unique_ptr<ConfigOption> parsed(option->clone());
    for (size_t head = 0; head < HEADS; ++head) {
        REQUIRE(parsed->deserialize(values[head]));
        option->set_at(parsed.get(), head, 0);
    }
}

// The width of column `head` of a width key against the nozzle of that head, a 0 falling back to
// the default width of the column (what the flows of that head print).
double expected_width(const DynamicPrintConfig &config, const std::string &key, size_t head)
{
    const double nozzle = config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(head);
    const double width  = config.get_abs_value_at(key, head, nozzle);
    return width > 0. ? width : config.get_abs_value_at("line_width", head, nozzle);
}

bool all_near(const std::set<double> &values, double expected, double tolerance = 0.005)
{
    if (values.empty())
        return false;
    for (double value : values)
        if (std::abs(value - expected) > tolerance)
            return false;
    return true;
}

bool all_near_any(const std::set<double> &values, const std::vector<double> &expected, double tolerance = 0.005)
{
    if (values.empty())
        return false;
    for (double value : values) {
        bool found = false;
        for (double candidate : expected)
            found = found || std::abs(value - candidate) <= tolerance;
        if (!found)
            return false;
    }
    return true;
}

std::string joined(const std::set<double> &values)
{
    std::string out;
    for (double value : values)
        out += (out.empty() ? "" : " ") + float_to_string_decimal_point(value);
    return out;
}

// Solid fills stretch their spacing (Fill::_adjust_solid_spacing), so their ;WIDTH: tags lie between
// the configured width and 1.2 x it; walls and sparse infill print it exactly. The fixtures turn off
// detect_narrow_internal_solid_infill, whose Arachne beads would leave that band.
bool solid_role(const std::string &role)
{
    return role == "Internal solid infill" || role == "Top surface" || role == "Bottom surface";
}

bool prints_width(const std::set<double> &values, const std::string &role, double expected, double tolerance = 0.005)
{
    if (values.empty())
        return false;
    if (!solid_role(role))
        return all_near(values, expected, tolerance);
    const double widest = *values.rbegin();
    return widest >= expected - tolerance && widest <= 1.2 * expected + tolerance;
}

// The four-head table with distinct line widths per tool head and per role. `percent`: equal
// percents on nozzles of 0.4, 0.5, 0.6 and 0.8 mm (a wrong nozzle shows); else absolute widths on
// four 0.4 mm nozzles (a wrong column shows). Classic walls print the configured width exactly.
DynamicPrintConfig width_harness_config(bool percent)
{
    DynamicPrintConfig config = four_head_config();
    config.set_key_value("wall_generator",         new ConfigOptionEnum<PerimeterGeneratorType>(PerimeterGeneratorType::Classic));
    config.set_key_value("detect_thin_wall",       new ConfigOptionBool(false));
    config.set_key_value("only_one_wall_top",      new ConfigOptionBool(false));
    config.set_key_value("gap_fill_target",        new ConfigOptionEnum<GapFillTarget>(gftNowhere));
    // The narrow-region reroute prints Arachne beads of scattered widths (see prints_width).
    config.set_key_value("detect_narrow_internal_solid_infill", new ConfigOptionBool(false));
    config.set_key_value("wall_loops",             new ConfigOptionInt(2));
    config.set_key_value("top_shell_layers",       new ConfigOptionInt(3));
    config.set_key_value("bottom_shell_layers",    new ConfigOptionInt(3));
    config.set_key_value("sparse_infill_density",  new ConfigOptionPercent(20));
    config.set_key_value("sparse_infill_pattern",  new ConfigOptionEnum<InfillPattern>(ipRectilinear));
    config.set_key_value("top_surface_pattern",    new ConfigOptionEnum<InfillPattern>(ipRectilinear));
    config.set_key_value("bottom_surface_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
    if (percent) {
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.5, 0.6, 0.8}));
        set_head_widths(config, "line_width",                       std::vector<std::string>(HEADS, "100%"));
        set_head_widths(config, "initial_layer_line_width",         std::vector<std::string>(HEADS, "130%"));
        set_head_widths(config, "outer_wall_line_width",            std::vector<std::string>(HEADS, "105%"));
        set_head_widths(config, "inner_wall_line_width",            std::vector<std::string>(HEADS, "110%"));
        set_head_widths(config, "sparse_infill_line_width",         std::vector<std::string>(HEADS, "115%"));
        set_head_widths(config, "internal_solid_infill_line_width", std::vector<std::string>(HEADS, "120%"));
        set_head_widths(config, "top_surface_line_width",           std::vector<std::string>(HEADS, "125%"));
        set_head_widths(config, "support_line_width",               std::vector<std::string>(HEADS, "112%"));
    } else {
        set_head_widths(config, "line_width",                       {"0.30", "0.42", "0.64", "0.86"});
        set_head_widths(config, "outer_wall_line_width",            {"0.31", "0.43", "0.65", "0.87"});
        set_head_widths(config, "inner_wall_line_width",            {"0.33", "0.45", "0.67", "0.89"});
        set_head_widths(config, "support_line_width",               {"0.34", "0.46", "0.68", "0.90"});
        set_head_widths(config, "sparse_infill_line_width",         {"0.35", "0.47", "0.69", "0.91"});
        set_head_widths(config, "internal_solid_infill_line_width", {"0.37", "0.49", "0.71", "0.93"});
        set_head_widths(config, "top_surface_line_width",           {"0.39", "0.51", "0.73", "0.95"});
        set_head_widths(config, "initial_layer_line_width",         {"0.41", "0.53", "0.75", "0.97"});
    }
    return config;
}

struct WidthHarnessSlice
{
    Print       print;
    Model       model;
    std::string gcode;
};

// Four 20 x 20 x 5 mm cubes, cube i on tool head i with the given extra per-object overrides.
std::unique_ptr<WidthHarnessSlice> width_harness_slice(const DynamicPrintConfig &config, const std::vector<std::vector<ConfigBase::SetDeserializeItem>> &extra = {})
{
    std::vector<TriangleMesh> meshes;
    std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides;
    for (size_t head = 0; head < HEADS; ++head) {
        TriangleMesh cube = mesh(TestMesh::cube_20x20x20);
        cube.scale(Vec3f(1.f, 1.f, 0.25f)); // 5 mm, 25 layers of 0.2 mm
        cube.translate(40.f + 40.f * float(head), 100.f, 0.f);
        meshes.emplace_back(std::move(cube));
        std::vector<ConfigBase::SetDeserializeItem> items = {{"extruder", std::to_string(head + 1)}};
        if (head < extra.size())
            items.insert(items.end(), extra[head].begin(), extra[head].end());
        overrides.emplace_back(std::move(items));
    }
    auto slice = std::make_unique<WidthHarnessSlice>();
    init_print(std::move(meshes), slice->print, slice->model, config, &overrides, /*arrange=*/false);
    {
        const StringObjectException err = slice->print.validate();
        INFO(err.string << " (" << err.opt_key << ", tool head " << err.tool_head << ")");
        REQUIRE(err.string.empty());
    }
    slice->gcode = Slic3r::Test::gcode(slice->print);
    return slice;
}

const std::vector<std::pair<const char *, const char *>> &role_width_keys()
{
    static const std::vector<std::pair<const char *, const char *>> keys = {
        {"Outer wall", "outer_wall_line_width"},
        {"Inner wall", "inner_wall_line_width"},
        {"Sparse infill", "sparse_infill_line_width"},
        {"Internal solid infill", "internal_solid_infill_line_width"},
        {"Top surface", "top_surface_line_width"},
        {"Bottom surface", "internal_solid_infill_line_width"},
    };
    return keys;
}

} // namespace

TEST_CASE("The width column of a filament is its nozzle index, the first column beyond the nozzles", "[PerHeadProcess][PerHeadWidth][columns]")
{
    PrintConfig config;
    config.apply(four_head_config(), true);
    REQUIRE(config.nozzle_diameter.values.size() == HEADS);
    CHECK(Print::width_slot(config, 0) == size_t(0));
    CHECK(Print::width_slot(config, 1) == size_t(0));
    CHECK(Print::width_slot(config, 3) == size_t(2));
    CHECK(Print::width_slot(config, 4) == size_t(3));
    CHECK(Print::width_slot(config, 5) == size_t(0));
    CHECK(Print::width_slot(config, 17) == size_t(0));
}

TEST_CASE("Every role of a tool head prints the line width of that head's column, resolved against its nozzle", "[PerHeadProcess][PerHeadWidth][columns]")
{
    const bool percent = GENERATE(false, true);
    CAPTURE(percent);
    const DynamicPrintConfig config = width_harness_config(percent);
    const std::unique_ptr<WidthHarnessSlice> slice = width_harness_slice(config);

    // Above the first layer: every role at the head's own column.
    const auto widths = feature_widths(slice->gcode, 2);
    if (widths.size() != HEADS)
        FAIL("tools with extrusions above the first layer: " << widths.size() << ": " << gcode_digest(slice->gcode) << "; G-code kept at "
             << keep_gcode("width_columns", slice->gcode));
    for (size_t head = 0; head < HEADS; ++head) {
        const int tool = int(head);
        REQUIRE(widths.count(tool) == 1);
        for (const auto &[role, key] : role_width_keys()) {
            const auto found = widths.at(tool).find(role);
            if (found == widths.at(tool).end()) {
                // A cube's bottom surface is its first layer alone (checked below); every other
                // role prints above it.
                CHECK(std::string(role) == "Bottom surface");
                continue;
            }
            const double expected = expected_width(config, key, head);
            INFO("tool " << tool << ", " << role << ": widths " << joined(found->second) << ", expected " << expected << " (" << key << " column " << head << ")");
            CHECK(prints_width(found->second, role, expected));
        }
        // No other head's outer wall width appears on this tool.
        for (size_t other = 0; other < HEADS; ++other)
            if (other != head) {
                INFO("tool " << tool << " against the outer wall width of tool head " << other + 1);
                CHECK_FALSE(all_near(widths.at(tool).at("Outer wall"), expected_width(config, "outer_wall_line_width", other), 0.001));
            }
    }

    // The first layer: every role at the head's first layer column.
    const auto first = feature_widths(slice->gcode, 1, 1);
    for (size_t head = 0; head < HEADS; ++head) {
        const int tool = int(head);
        REQUIRE(first.count(tool) == 1);
        const double expected = expected_width(config, "initial_layer_line_width", head);
        for (const auto &[role, values] : first.at(tool)) {
            INFO("tool " << tool << ", first layer " << role << ": widths " << joined(values) << ", expected " << expected);
            CHECK(prints_width(values, role, expected));
        }
    }
}

TEST_CASE("A surface printed by the filament of another tool head takes that head's width column", "[PerHeadProcess][PerHeadWidth][columns]")
{
    const DynamicPrintConfig config = width_harness_config(false);
    // Cube 1 prints its bottom surfaces with filament 3 (tool head 3), cube 2 its top surface with
    // filament 4 (tool head 4).
    const std::unique_ptr<WidthHarnessSlice> slice = width_harness_slice(config, {{{"bottom_surface_filament_id", "3"}}, {{"top_surface_filament_id", "4"}}});
    // A cube's bottom surface is its first layer: tool head 3 prints it at its own first layer
    // width (the first layer width of a head covers every role of the layer).
    const auto first = feature_widths(slice->gcode, 1, 1);
    REQUIRE(first.count(2) == 1);
    REQUIRE(first.at(2).count("Bottom surface") == 1);
    {
        const double expected = expected_width(config, "initial_layer_line_width", 2);
        INFO("tool 2 bottom surfaces: " << joined(first.at(2).at("Bottom surface")) << ", expected " << expected);
        CHECK(prints_width(first.at(2).at("Bottom surface"), "Bottom surface", expected));
    }
    const auto widths = feature_widths(slice->gcode, 2);
    REQUIRE(widths.count(3) == 1);
    REQUIRE(widths.at(3).count("Top surface") == 1);
    {
        const double expected = expected_width(config, "top_surface_line_width", 3);
        INFO("tool 3 top surfaces: " << joined(widths.at(3).at("Top surface")) << ", expected " << expected);
        CHECK(prints_width(widths.at(3).at("Top surface"), "Top surface", expected));
    }
    // Tool head 1 prints no bottom surface of its own any more, tool head 2 no top surface.
    CHECK((first.count(0) == 0 || first.at(0).count("Bottom surface") == 0));
    CHECK((widths.count(1) == 0 || widths.at(1).count("Top surface") == 0));
}

TEST_CASE("The raft of an object prints the support widths of the tool head of its support filament", "[PerHeadProcess][PerHeadWidth][columns]")
{
    DynamicPrintConfig config = width_harness_config(false);
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("raft_layers",    new ConfigOptionInt(2));
    std::vector<std::vector<ConfigBase::SetDeserializeItem>> extra;
    for (size_t head = 0; head < HEADS; ++head)
        extra.push_back({{"support_filament", std::to_string(head + 1)}, {"support_interface_filament", std::to_string(head + 1)}});
    const std::unique_ptr<WidthHarnessSlice> slice = width_harness_slice(config, extra);
    const auto widths = feature_widths(slice->gcode, 1);
    for (size_t head = 0; head < HEADS; ++head) {
        const int tool = int(head);
        REQUIRE(widths.count(tool) == 1);
        std::set<double> support;
        for (const char *role : {"Support", "Support interface"})
            if (const auto found = widths.at(tool).find(role); found != widths.at(tool).end())
                support.insert(found->second.begin(), found->second.end());
        // The raft's first layer prints the head's first layer width, its other layers the head's
        // support width (support_material_1st_layer_flow, support_material_flow, the interface flow).
        const std::vector<double> expected = {expected_width(config, "support_line_width", head), expected_width(config, "initial_layer_line_width", head)};
        INFO("tool " << tool << " raft widths: " << joined(support) << ", expected " << expected[0] << " or " << expected[1]);
        CHECK(all_near_any(support, expected));
    }
}

// A support material pins the default support filament (PrintObject::resolved_default_support_filament);
// the support widths follow that filament's tool head, not head 1.
TEST_CASE("With a support material the raft prints the support widths of the tool head that material resolves to", "[PerHeadProcess][PerHeadWidth][columns]")
{
    DynamicPrintConfig config = width_harness_config(false);
    config.set_key_value("enable_support",             new ConfigOptionBool(true));
    config.set_key_value("raft_layers",                new ConfigOptionInt(2));
    config.set_key_value("filament_type",              new ConfigOptionStrings({"PLA", "PLA", "PETG", "PLA"}));
    config.set_key_value("support_base_material",      new ConfigOptionString("PETG"));
    config.set_key_value("support_interface_material", new ConfigOptionString("PETG"));
    std::vector<std::vector<ConfigBase::SetDeserializeItem>> extra(HEADS, {{"support_filament", "0"}, {"support_interface_filament", "0"}});
    const std::unique_ptr<WidthHarnessSlice> slice = width_harness_slice(config, extra);
    for (const PrintObject *object : slice->print.objects()) {
        CHECK(object->resolved_default_support_filament(false) == 3u);
        CHECK(support_head(object, 0, false) == size_t(2));
        CHECK(support_head(object, 0, true) == size_t(2));
    }
    const auto widths = feature_widths(slice->gcode, 1);
    std::set<double> support;
    std::set<int>    tools;
    for (const auto &[tool, features] : widths)
        for (const char *role : {"Support", "Support interface"})
            if (const auto found = features.find(role); found != features.end()) {
                support.insert(found->second.begin(), found->second.end());
                tools.insert(tool);
            }
    const std::vector<double> expected = {expected_width(config, "support_line_width", 2), expected_width(config, "initial_layer_line_width", 2)};
    INFO("raft widths: " << joined(support) << ", expected " << expected[0] << " or " << expected[1]);
    CHECK(all_near_any(support, expected));
    // Tool head 3 prints every raft.
    CHECK(tools == std::set<int>{2});
}


// ---- Line widths per tool head: Print::validate per head --------------------------

TEST_CASE("The owner plate validates without the Locked Zag widths being lowered", "[PerHeadProcess][PerHeadWidth][Profiles][validate_head]")
{
    const std::unique_ptr<OwnerPlateSlice> slice = owner_plate_slice({}, nullptr, /*slice_plate=*/false);
    // (a) The skin and skeleton widths keep their default of 100 % of the nozzle (0.2 mm at the 0.2
    // mm head, equal to the layer height): checked for a Locked Zag region alone, the plate passes.
    CHECK(slice->config.opt<ConfigOptionFloatOrPercent>("skin_infill_line_width")->value == 100.);
    CHECK(slice->config.opt<ConfigOptionFloatOrPercent>("skeleton_infill_line_width")->value == 100.);
    const StringObjectException err = slice->print.validate();
    INFO(err.string << " (" << err.opt_key << ", tool head " << err.tool_head << ")");
    CHECK(err.string.empty());
    CHECK(err.tool_head == -1);
}

TEST_CASE("A line width too wide for one tool head is refused naming that head, and accepted when set for a wide head alone", "[PerHeadProcess][PerHeadWidth][Profiles][validate_head]")
{
    // (b) 1.1 mm under All tool heads: five times the 0.2 mm nozzle is 1.0 mm.
    {
        const std::unique_ptr<OwnerPlateSlice> slice = owner_plate_slice(
            [](PresetBundle &bundle) {
                bundle.prints.get_edited_preset().config.set_key_value("outer_wall_line_width", new ConfigOptionFloatsOrPercentsNullable{FloatOrPercent(1.1, false)});
            },
            nullptr, /*slice_plate=*/false);
        const StringObjectException err = slice->print.validate();
        INFO(err.string << " (" << err.opt_key << ", tool head " << err.tool_head << ")");
        REQUIRE_FALSE(err.string.empty());
        CHECK(err.opt_key == "outer_wall_line_width");
        CHECK(err.tool_head == 0);
        CHECK(err.string.find("extruder 1") != std::string::npos);
        CHECK(err.string.find("too large") != std::string::npos);
    }
    // Set for tool head 4 (0.8 mm nozzle) alone: accepted, the other heads keep the preset's percent.
    {
        const std::unique_ptr<OwnerPlateSlice> slice = owner_plate_slice(
            [](PresetBundle &bundle) { set_head_width(bundle, 3, "outer_wall_line_width", "1.1"); }, nullptr, /*slice_plate=*/false);
        const StringObjectException err = slice->print.validate();
        INFO(err.string << " (" << err.opt_key << ", tool head " << err.tool_head << ")");
        CHECK(err.string.empty());
    }
}

TEST_CASE("The bridge width and the Locked Zag widths are refused at the tool head whose nozzle they do not fit", "[PerHeadProcess][PerHeadWidth][Profiles][validate_head]")
{
    // (c) The default bridge width of 100 % equals the 0.2 mm layer height at the 0.2 mm head.
    {
        const std::unique_ptr<OwnerPlateSlice> slice = owner_plate_slice(
            [](PresetBundle &bundle) {
                bundle.prints.get_edited_preset().config.set_key_value("bridge_line_width", new ConfigOptionFloatsOrPercentsNullable{FloatOrPercent(100., true)});
            },
            nullptr, /*slice_plate=*/false);
        const StringObjectException err = slice->print.validate();
        INFO(err.string << " (" << err.opt_key << ", tool head " << err.tool_head << ")");
        REQUIRE_FALSE(err.string.empty());
        CHECK(err.opt_key == "bridge_line_width");
        CHECK(err.tool_head == 0);
        CHECK(err.string.find("extruder 1") != std::string::npos);
    }
    // (d) A Locked Zag region: the skin width of 100 % equals the layer height at the 0.2 mm head.
    {
        const std::unique_ptr<OwnerPlateSlice> slice = owner_plate_slice(
            [](PresetBundle &bundle) {
                bundle.prints.get_edited_preset().config.set_key_value("sparse_infill_pattern", new ConfigOptionEnum<InfillPattern>(ipLockedZag));
            },
            nullptr, /*slice_plate=*/false);
        const StringObjectException err = slice->print.validate();
        INFO(err.string << " (" << err.opt_key << ", tool head " << err.tool_head << ")");
        REQUIRE_FALSE(err.string.empty());
        CHECK(err.opt_key == "skin_infill_line_width");
        CHECK(err.tool_head == 0);
    }
}

TEST_CASE("The config validator checks each width column against the nozzle of its tool head", "[PerHeadProcess][PerHeadWidth][Profiles][validate_head]")
{
    // (e) The composed table has one column per tool head (print_extruder_id 1..4): a 0.7 mm bridge
    // width for the 0.8 mm head alone passes, the same under every head is refused for the 0.2 mm one.
    const std::unique_ptr<OwnerPlateSlice> slice = owner_plate_slice({}, nullptr, /*slice_plate=*/false);
    DynamicPrintConfig config = slice->config;
    const auto *ids = config.option<ConfigOptionInts>("print_extruder_id");
    REQUIRE(ids != nullptr);
    REQUIRE(ids->values == std::vector<int>{1, 2, 3, 4});
    config.set_deserialize_strict("bridge_line_width", "0,0,0,0.7");
    CHECK(config.validate().count("bridge_line_width") == 0);
    config.set_deserialize_strict("bridge_line_width", "0.7,0.7,0.7,0.7");
    CHECK(config.validate().count("bridge_line_width") == 1);
    // An outer wall of 1.1 mm: too large for the 0.2 mm head, fine for the 0.8 mm one.
    config.set_deserialize_strict("bridge_line_width", "0,0,0,0");
    config.set_deserialize_strict("outer_wall_line_width", "105%,105%,105%,1.1");
    CHECK(config.validate().count("outer_wall_line_width") == 0);
    config.set_deserialize_strict("outer_wall_line_width", "1.1,105%,105%,105%");
    CHECK(config.validate().count("outer_wall_line_width") == 1);
}

TEST_CASE("An external bridge printed by the bottom surface filament is checked at that filament's tool head", "[PerHeadProcess][PerHeadWidth][Profiles][validate_head]")
{
    // (f) Cube 1 prints on the 0.4 mm head with its bottom surfaces, and so its external bridges, on
    // the 0.2 mm head: a 0.3 mm bridge width fits every role's nozzle and not the bottom surface's.
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides = {
        {{"extruder", "2"}, {"bottom_surface_filament_id", "1"}}, {{"extruder", "2"}}, {{"extruder", "3"}}, {{"extruder", "4"}}};
    const std::unique_ptr<OwnerPlateSlice> slice = owner_plate_slice(
        [](PresetBundle &bundle) {
            bundle.prints.get_edited_preset().config.set_key_value("bridge_line_width", new ConfigOptionFloatsOrPercentsNullable{FloatOrPercent(0.3, false)});
        },
        &overrides, /*slice_plate=*/false);
    const StringObjectException err = slice->print.validate();
    INFO(err.string << " (" << err.opt_key << ", tool head " << err.tool_head << ")");
    REQUIRE_FALSE(err.string.empty());
    CHECK(err.opt_key == "bridge_line_width");
    CHECK(err.tool_head == 0);
    CHECK(err.string.find("extruder 1") != std::string::npos);
}

TEST_CASE("Line widths set per tool head are refused on a Bambu printer", "[PerHeadProcess][PerHeadWidth][Profiles][validate_head]")
{
    // (g) The same plate with a width set for tool head 4 passes on a Snapmaker printer and is
    // refused with the printer flagged as a Bambu one, where nothing composes or edits per head.
    const std::unique_ptr<OwnerPlateSlice> slice = owner_plate_slice(
        [](PresetBundle &bundle) { set_head_width(bundle, 3, "outer_wall_line_width", "0.9"); }, nullptr, /*slice_plate=*/false);
    {
        const StringObjectException err = slice->print.validate();
        INFO(err.string << " (" << err.opt_key << ", tool head " << err.tool_head << ")");
        CHECK(err.string.empty());
    }
    slice->print.is_BBL_printer() = true;
    const StringObjectException err = slice->print.validate();
    INFO(err.string << " (" << err.opt_key << ", tool head " << err.tool_head << ")");
    REQUIRE_FALSE(err.string.empty());
    // The composed plate gives every head the widths of the preset of its size, so every one of the
    // nine keys differs between the columns; the refusal names the first key it meets.
    CHECK(PerHeadProcess::flow_independent_keys().count(err.opt_key) == 1);
    CHECK(err.string.find("not supported on this printer") != std::string::npos);
}

TEST_CASE("An absolute width changed under All tool heads warns for the tool heads whose nozzle it does not suit", "[PerHeadProcess][PerHeadWidth][Profiles][validate_head]")
{
    // (h) 0.42 mm under All: below the 0.6 and 0.8 mm nozzles, above twice the 0.2 mm one, fine for 0.4.
    const std::unique_ptr<OwnerPlateSlice> slice = owner_plate_slice(
        [](PresetBundle &bundle) {
            bundle.prints.get_edited_preset().config.set_key_value("outer_wall_line_width", new ConfigOptionFloatsOrPercentsNullable{FloatOrPercent(0.42, false)});
        },
        nullptr, /*slice_plate=*/false);
    std::vector<StringObjectException> warnings;
    const StringObjectException        err = slice->print.validate(&warnings);
    INFO(err.string << " (" << err.opt_key << ", tool head " << err.tool_head << ")");
    CHECK(err.string.empty());
    std::set<int> warned;
    for (const StringObjectException &w : warnings)
        if (w.opt_key == "outer_wall_line_width") {
            CHECK(w.is_warning);
            CHECK(w.string.find("changed under All extruders") != std::string::npos);
            warned.insert(w.tool_head);
        }
    CHECK(warned == std::set<int>{0, 2, 3});
    // Set for tool head 4 (0.8 mm) itself: no warning for that head.
    const std::unique_ptr<OwnerPlateSlice> set_for_head = owner_plate_slice(
        [](PresetBundle &bundle) { set_head_width(bundle, 3, "outer_wall_line_width", "0.42"); }, nullptr, /*slice_plate=*/false);
    warnings.clear();
    CHECK(set_for_head->print.validate(&warnings).string.empty());
    for (const StringObjectException &w : warnings)
        if (w.opt_key == "outer_wall_line_width")
            CHECK(w.tool_head != 3);
}


// ---- Line widths per tool head: the composed widths in the G-code --------------

namespace {

// The width a preset's Standard shared column gives a key against `nozzle`, a 0 falling back to
// its default line width.
double preset_width(const Preset &preset, const std::string &key, double nozzle)
{
    const size_t column = size_t(PerHeadProcess::shared_column(preset.config, nvtStandard));
    const auto  *widths = preset.config.option<ConfigOptionFloatsOrPercentsNullable>(key);
    REQUIRE(widths != nullptr);
    const ConfigOptionFloatOrPercent width = Flow::width_at(*widths, column);
    if (width.value > 0.)
        return width.get_abs_value(nozzle);
    const auto *defaults = preset.config.option<ConfigOptionFloatsOrPercentsNullable>("line_width");
    REQUIRE(defaults != nullptr);
    return Flow::width_at(*defaults, column).get_abs_value(nozzle);
}

} // namespace

TEST_CASE("On a plate of four nozzle sizes every tool head prints the line widths of the preset of its size", "[PerHeadProcess][PerHeadWidth][Profiles][hs_mixed_plate_widths]")
{
    const std::unique_ptr<OwnerPlateSlice> slice = owner_plate_slice();
    REQUIRE(slice->sources.size() == HEADS);
    const std::vector<double> nozzles = {0.2, 0.4, 0.6, 0.8};
    const Preset             &selected = slice->bundle.prints.get_selected_preset();
    // The width source of every head: the selected preset for the High Flow home-size head 2, the
    // preset of the size for the others.
    std::vector<const Preset *> width_sources(HEADS, &selected);
    for (size_t head : {size_t(0), size_t(2), size_t(3)}) {
        REQUIRE(PerHeadProcess::width_source(slice->sources[head]) != nullptr);
        width_sources[head] = PerHeadProcess::width_source(slice->sources[head]);
    }
    CHECK(PerHeadProcess::width_source(slice->sources[1]) == nullptr);

    const auto widths = feature_widths(slice->gcode, 2);
    if (widths.size() != HEADS)
        FAIL("tools with extrusions above the first layer: " << widths.size() << ": " << gcode_digest(slice->gcode) << "; G-code kept at "
             << keep_gcode("hs_mixed_plate_widths", slice->gcode));
    const std::vector<std::pair<const char *, const char *>> roles = {
        {"Sparse infill", "sparse_infill_line_width"}, {"Internal solid infill", "internal_solid_infill_line_width"}, {"Top surface", "top_surface_line_width"}};
    for (size_t head = 0; head < HEADS; ++head) {
        const int tool = int(head);
        for (const auto &[role, key] : roles) {
            REQUIRE(widths.at(tool).count(role) == 1);
            const double expected = preset_width(*width_sources[head], key, nozzles[head]);
            INFO("tool " << tool << " (" << width_sources[head]->name << "), " << role << ": " << joined(widths.at(tool).at(role)) << ", expected " << expected);
            CHECK(prints_width(widths.at(tool).at(role), role, expected));
        }
    }
    // Expected: 110 % of 0.2, the selected preset's 112.5 % / 105 % / 105 %
    // of 0.4, 103.33 % of 0.6, 102.5 % of 0.8.
    CHECK(all_near(widths.at(0).at("Sparse infill"), 0.22));
    CHECK(prints_width(widths.at(0).at("Top surface"), "Top surface", 0.22));
    CHECK(all_near(widths.at(1).at("Sparse infill"), 0.45));
    CHECK(prints_width(widths.at(1).at("Internal solid infill"), "Internal solid infill", 0.42));
    CHECK(prints_width(widths.at(1).at("Top surface"), "Top surface", 0.42));
    CHECK(all_near(widths.at(2).at("Sparse infill"), 0.62));
    CHECK(all_near(widths.at(3).at("Sparse infill"), 0.82));
    // The first layer: 125 % of 0.2 on tool head 1, 102.5 % of 0.8 on tool head 4 (1.00 before).
    const auto first = feature_widths(slice->gcode, 1, 1);
    REQUIRE(first.count(0) == 1);
    REQUIRE(first.count(3) == 1);
    for (const auto &[role, values] : first.at(0)) {
        INFO("tool 0 first layer " << role << ": " << joined(values));
        CHECK(prints_width(values, role, 0.25));
    }
    for (const auto &[role, values] : first.at(3)) {
        INFO("tool 3 first layer " << role << ": " << joined(values));
        CHECK(prints_width(values, role, 0.82));
    }
}

TEST_CASE("On a plate of four nozzle sizes a sparse infill width set for the 0.8 mm tool head reaches its G-code alone", "[PerHeadProcess][PerHeadWidth][Profiles][pho_gcode_head_width]")
{
    const std::unique_ptr<OwnerPlateSlice> slice = owner_plate_slice([](PresetBundle &bundle) { set_head_width(bundle, 3, "sparse_infill_line_width", "0.9"); });
    REQUIRE(slice->sources.size() == HEADS);
    CHECK(slice->sources[3].overridden_keys == std::vector<std::string>{"sparse_infill_line_width"});
    const auto widths = feature_widths(slice->gcode, 2);
    if (widths.size() != HEADS)
        FAIL("tools with extrusions above the first layer: " << widths.size() << ": " << gcode_digest(slice->gcode) << "; G-code kept at "
             << keep_gcode("pho_gcode_head_width", slice->gcode));
    REQUIRE(widths.at(3).count("Sparse infill") == 1);
    INFO("tool 3 sparse infill: " << joined(widths.at(3).at("Sparse infill")));
    CHECK(all_near(widths.at(3).at("Sparse infill"), 0.9));
    for (int tool : {0, 1, 2}) {
        REQUIRE(widths.at(tool).count("Sparse infill") == 1);
        INFO("tool " << tool << " sparse infill: " << joined(widths.at(tool).at("Sparse infill")));
        CHECK_FALSE(all_near(widths.at(tool).at("Sparse infill"), 0.9, 0.02));
    }
    // The other widths of tool head 4 still come from the preset of its size.
    REQUIRE(widths.at(3).count("Top surface") == 1);
    CHECK(prints_width(widths.at(3).at("Top surface"), "Top surface", 0.82));
}

// "Add settings" seeds a line width override with the value the item prints (PerHeadProcess::
// override_seed): 102.5 % on the head-4 cube slices unchanged; the selected preset's 112.5 %
// widens head 4's sparse infill to 0.90 mm.
TEST_CASE("A sparse infill width added to the 0.8 mm cube with the value it prints leaves its G-code unchanged", "[PerHeadProcess][PerHeadWidth][Profiles][phw_override_seed]")
{
    const auto plate = [](const char *head4_width) {
        const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides = {
            {{"extruder", "1"}}, {{"extruder", "2"}}, {{"extruder", "3"}}, {{"extruder", "4"}, {"sparse_infill_line_width", head4_width}}};
        return owner_plate_slice({}, &overrides);
    };
    const std::unique_ptr<OwnerPlateSlice> seeded = plate("102.5%");
    REQUIRE(seeded->sources.size() == HEADS);
    const auto widths = feature_widths(seeded->gcode, 2);
    if (widths.size() != HEADS)
        FAIL("tools with extrusions above the first layer: " << widths.size() << ": " << gcode_digest(seeded->gcode) << "; G-code kept at "
             << keep_gcode("phw_override_seed", seeded->gcode));
    REQUIRE(widths.at(3).count("Sparse infill") == 1);
    INFO("tool 3 sparse infill with the seed: " << joined(widths.at(3).at("Sparse infill")));
    CHECK(all_near(widths.at(3).at("Sparse infill"), 0.82));
    // The other cubes print as before.
    REQUIRE(widths.at(0).count("Sparse infill") == 1);
    REQUIRE(widths.at(2).count("Sparse infill") == 1);
    CHECK(all_near(widths.at(0).at("Sparse infill"), 0.22));
    CHECK(all_near(widths.at(2).at("Sparse infill"), 0.62));

    // The red case: the selected preset's value as the override prints 112.5 % of 0.8.
    const std::unique_ptr<OwnerPlateSlice> cloned = plate("112.5%");
    const auto                             widened = feature_widths(cloned->gcode, 2);
    if (widened.size() != HEADS)
        FAIL("tools with extrusions above the first layer: " << widened.size() << ": " << gcode_digest(cloned->gcode) << "; G-code kept at "
             << keep_gcode("phw_override_seed_clone", cloned->gcode));
    REQUIRE(widened.at(3).count("Sparse infill") == 1);
    INFO("tool 3 sparse infill with the clone: " << joined(widened.at(3).at("Sparse infill")));
    CHECK(all_near(widened.at(3).at("Sparse infill"), 0.90));
}

// ---- Line widths per tool head: an all-0.4 plate under the High Flow rule ----

namespace {

// The text of column `column` of a key ("112.5%", "0.42").
std::string column_text(const DynamicPrintConfig &config, const std::string &key, size_t column)
{
    const auto *option = dynamic_cast<const ConfigOptionVectorBase *>(config.option(key));
    REQUIRE(option != nullptr);
    const std::vector<std::string> values = option->vserialize();
    REQUIRE(column < values.size());
    return values[column];
}

} // namespace

// On an all-0.4 plate the High Flow rule re-picks a High Flow head's speeds (0.20mm Standard), but its
// widths stay the selected preset's (width_source null for a home-size head): 0.45 mm top surfaces
// on every tool, also with the Standard speeds of the flow toggle.
TEST_CASE("On an all-0.4 plate a High Flow tool head under a preset without High Flow speeds keeps that preset's line widths", "[PerHeadProcess][PerHeadWidth][Profiles][hs_all_04_widths]")
{
    const bool standard_speeds = GENERATE(false, true);
    CAPTURE(standard_speeds);
    const char *const STD_024_04 = "0.24mm Standard @Snapmaker U1 (0.4 nozzle)";
    const std::unique_ptr<OwnerPlateSlice> slice = owner_plate_slice([standard_speeds, STD_024_04](PresetBundle &bundle) {
        bundle.printers.get_edited_preset().config.set_key_value("nozzle_diameter", new ConfigOptionFloats(std::vector<double>(HEADS, 0.4)));
        REQUIRE(bundle.prints.select_preset_by_name(STD_024_04, true));
        if (standard_speeds)
            PerHeadProcess::set_chosen_flow(bundle, 1, nvtStandard);
    });
    const Preset &selected = slice->bundle.prints.get_selected_preset();
    REQUIRE(selected.name == STD_024_04);
    REQUIRE(slice->sources.size() == HEADS);
    if (standard_speeds) {
        CHECK(slice->sources[1].flow_chosen);
        CHECK_FALSE(slice->sources[1].derived);
    } else {
        CHECK(slice->sources[1].reason == PerHeadProcess::Reason::HighFlow);
        REQUIRE(slice->sources[1].derived);
        REQUIRE(slice->sources[1].preset != nullptr);
        CHECK(slice->sources[1].preset->name == "0.20mm Standard @Snapmaker U1 (0.4 nozzle)");
    }
    for (size_t head = 0; head < HEADS; ++head) {
        INFO("tool head " << head + 1);
        CHECK(PerHeadProcess::width_source(slice->sources[head]) == nullptr);
    }
    // Every column of the nine keys holds the selected preset's value.
    const size_t shared = size_t(PerHeadProcess::shared_column(selected.config, nvtStandard));
    for (const std::string &key : PerHeadProcess::flow_independent_keys()) {
        const auto *composed = dynamic_cast<const ConfigOptionVectorBase *>(slice->config.option(key));
        REQUIRE(composed != nullptr);
        const std::vector<std::string> values = composed->vserialize();
        const std::string              wanted = column_text(selected.config, key, shared);
        REQUIRE_FALSE(values.empty());
        for (size_t column = 0; column < values.size(); ++column) {
            INFO(key << " column " << column << ": " << values[column] << ", the selected preset's " << wanted);
            CHECK(values[column] == wanted);
        }
    }
    CHECK(column_text(slice->config, "top_surface_line_width", 1) == "112.5%");
    // Every tool prints 0.45 mm top surfaces and 0.45 mm sparse infill (112.5 % of 0.4).
    const auto widths = feature_widths(slice->gcode, 2);
    if (widths.size() != HEADS)
        FAIL("tools with extrusions above the first layer: " << widths.size() << ": " << gcode_digest(slice->gcode) << "; G-code kept at "
             << keep_gcode("hs_all_04_widths", slice->gcode));
    for (size_t head = 0; head < HEADS; ++head) {
        const int tool = int(head);
        REQUIRE(widths.at(tool).count("Top surface") == 1);
        INFO("tool " << tool << " top surface: " << joined(widths.at(tool).at("Top surface")));
        CHECK(prints_width(widths.at(tool).at("Top surface"), "Top surface", 0.45));
        REQUIRE(widths.at(tool).count("Sparse infill") == 1);
        INFO("tool " << tool << " sparse infill: " << joined(widths.at(tool).at("Sparse infill")));
        CHECK(all_near(widths.at(tool).at("Sparse infill"), 0.45));
    }
}
