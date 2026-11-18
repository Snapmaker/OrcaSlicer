#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/MixedFilament.hpp"

#include <regex>
#include <string>

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {
// Extract the S value of the first M190 (first-layer bed temperature, emitted after machine_start_gcode).
int first_layer_bed_temp(const std::string& gcode)
{
    std::smatch m;
    if (std::regex_search(gcode, m, std::regex(R"(M190 S(\d+))")))
        return std::stoi(m[1]);
    return -1;
}

// Extract the S value of the M140 emitted at the first-to-second layer transition.
int later_layer_bed_temp(const std::string& gcode)
{
    std::smatch m;
    if (std::regex_search(gcode, m, std::regex(R"(M140 S(\d+))")))
        return std::stoi(m[1]);
    return -1;
}
} // namespace

// Regression: with a single filament the emitted bed temperature is the value of that filament
// (identical to the pre-change behavior, which always used the first printing extruder).
TEST_CASE("Single filament bed temperature is unchanged", "[BedTemperature]")
{
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "curr_bed_type", "High Temp Plate" }, // btPEI -> hot_plate_temp
        { "hot_plate_temp", "45" },
        { "hot_plate_temp_initial_layer", "55" }
    });
    std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
    REQUIRE(first_layer_bed_temp(gcode) == 55);
    REQUIRE(later_layer_bed_temp(gcode) == 45);
}

// Two compatible filaments (PLA-like 65C + TPU-like 35C). The bed temperature shall always take the
// higher value, even if the first printing extruder (wall) is the low-temperature one.
TEST_CASE("Mixed print uses the highest bed temperature", "[BedTemperature]")
{
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    // Filament count is data-driven like a loaded preset (Preset.cpp derives it from
    // filament_diameter); DynamicPrintConfig::set_num_filaments does not resize these vectors.
    config.set_deserialize_strict({
        { "curr_bed_type", "High Temp Plate" },
        { "filament_diameter", "1.75,1.75" }, // two filaments, preset-style
        { "hot_plate_temp", "30,60" },
        { "hot_plate_temp_initial_layer", "35,65" },
        // Wall printed by extruder 1 (0-based 0, low bed temp 35C), infill by extruder 2 (0-based 1, 65C).
        { "wall_filament", 1 },
        { "sparse_infill_filament", 2 },
        { "solid_infill_filament", 2 }
    });
    std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
    REQUIRE(first_layer_bed_temp(gcode) == 65);
    REQUIRE(later_layer_bed_temp(gcode) == 60);
}

TEST_CASE("An unused wall filament does not raise the bed temperature", "[BedTemperature]")
{
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    // Filament count is data-driven like a loaded preset (Preset.cpp derives it from
    // filament_diameter); DynamicPrintConfig::set_num_filaments does not resize these vectors.
    config.set_deserialize_strict({
        { "curr_bed_type", "High Temp Plate" },
        { "filament_diameter", "1.75,1.75" }, // two filaments, preset-style
        { "hot_plate_temp", "30,60" },
        { "hot_plate_temp_initial_layer", "35,65" },
        // Without walls this configuration generates no brim or paths with filament 2.
        { "wall_loops", 0 },
        { "brim_width", 5 },
        { "brim_type", "auto_brim" },
        { "wall_filament", 2 },
        { "sparse_infill_filament", 1 },
        { "solid_infill_filament", 1 }
    });
    std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
    REQUIRE(gcode.find("\nT1") == std::string::npos);
    REQUIRE(first_layer_bed_temp(gcode) == 35);
    REQUIRE(later_layer_bed_temp(gcode) == 30);
}

// Placeholder: the machine_start_gcode single-value placeholder expands to the max bed temperature
// (this is the path used by the Snapmaker U1 start gcode).
TEST_CASE("bed_temperature_initial_layer_single expands to the max", "[BedTemperature]")
{
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    // Filament count is data-driven like a loaded preset (Preset.cpp derives it from
    // filament_diameter); DynamicPrintConfig::set_num_filaments does not resize these vectors.
    config.set_deserialize_strict({
        { "curr_bed_type", "High Temp Plate" },
        { "filament_diameter", "1.75,1.75" }, // two filaments, preset-style
        { "hot_plate_temp", "30,60" },
        { "hot_plate_temp_initial_layer", "35,65" },
        { "wall_filament", 1 },
        { "sparse_infill_filament", 2 },
        { "solid_infill_filament", 2 },
        { "machine_start_gcode", "M190 S{bed_temperature_initial_layer_single}" }
    });
    std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
    // start gcode already sets the temperature, so no additional M190 is emitted by the slicer.
    REQUIRE(gcode.find("M190 S65") != std::string::npos);
}

namespace {
DynamicPrintConfig support_bed_config()
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.option<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#00FF00" };
    config.set_deserialize_strict({
        { "curr_bed_type", "Textured PEI Plate" },
        { "filament_diameter", "1.75,1.75" },
        { "textured_plate_temp_initial_layer", "60,80" },
        { "textured_plate_temp", "55,75" },
        { "wall_filament", 1 }, { "sparse_infill_filament", 1 }, { "solid_infill_filament", 1 },
        { "enable_support", 1 }, { "support_filament", 2 }, { "support_interface_filament", 2 },
        { "enable_prime_tower", 0 }, { "skirt_loops", 0 }, { "brim_type", "no_brim" },
        { "machine_start_gcode", "; bed_single={bed_temperature_initial_layer_single}" }
    });
    return config;
}

TriangleMesh supported_cap()
{
    auto mesh = make_cube(10, 10, 10);
    mesh.translate(10, 10, 0);
    auto cap = make_cube(30, 30, 2);
    cap.translate(0, 0, 10);
    mesh.merge(cap);
    return mesh;
}

void check_bed_temperature(const std::string &gcode, int first, int later)
{
    CHECK(first_layer_bed_temp(gcode) == first);
    CHECK(later_layer_bed_temp(gcode) == later);
    CHECK(gcode.find("; bed_single=" + std::to_string(first)) != std::string::npos);
    CHECK(gcode.find("; first_layer_bed_temperature = " + std::to_string(first) + "\n") != std::string::npos);
}

std::vector<std::string> bed_commands(const std::string& gcode)
{
    std::vector<std::string> commands;
    GCodeReader              reader;
    reader.parse_buffer(gcode, [&](GCodeReader&, const GCodeReader::GCodeLine& line) {
        if (line.cmd() == "M140" || line.cmd() == "M190")
            commands.push_back(line.raw().substr(0, line.raw().find(" ;")));
    });
    return commands;
}

void check_second_layer_bed_command(const std::string& gcode, size_t script_start, bool expect_change)
{
    const std::string layer_marker = "\n; bed test layer\n";
    const size_t      first_layer  = gcode.find(layer_marker, script_start);
    REQUIRE(first_layer != std::string::npos);
    const size_t second_layer = gcode.find(layer_marker, first_layer + layer_marker.size());
    REQUIRE(second_layer != std::string::npos);
    const size_t third_layer = gcode.find(layer_marker, second_layer + layer_marker.size());
    REQUIRE(third_layer != std::string::npos);
    CHECK(bed_commands(gcode.substr(first_layer, second_layer - first_layer)).empty());
    const std::vector<std::string> expected = expect_change ? std::vector<std::string>{"M140 S60"} : std::vector<std::string>{};
    CHECK(bed_commands(gcode.substr(second_layer, third_layer - second_layer)) == expected);
}
} // namespace

TEST_CASE("Support without generated paths does not raise the bed temperature", "[BedTemperature]")
{
    auto config = support_bed_config();
    config.set_deserialize_strict({ { "print_sequence", GENERATE("by layer", "by object") } });
    Print print;
    Model model;
    Test::init_print({ TestMesh::cube_20x20x20 }, print, model, config);
    // BBL writes the config header before initializing the G-code writer's extruders.
    print.is_BBL_printer() = GENERATE(false, true);
    const auto gcode = Test::gcode(print);
    REQUIRE(gcode.find("\nT1") == std::string::npos);
    for (const auto *object : print.objects())
        for (const auto *layer : object->support_layers())
            REQUIRE_FALSE(layer->has_extrusions());
    check_bed_temperature(gcode, 60, 55);
}

TEST_CASE("An unused support interface does not raise the bed temperature", "[BedTemperature]")
{
    auto config = support_bed_config();
    config.set_num_extruders(3);
    config.set_deserialize_strict({
        { "filament_diameter", "1.75,1.75,1.75" },
        { "textured_plate_temp_initial_layer", "60,80,95" }, { "textured_plate_temp", "55,75,90" },
        { "support_interface_filament", 3 },
        { "support_interface_top_layers", 0 }, { "support_interface_bottom_layers", 0 },
        { "print_sequence", GENERATE("by layer", "by object") }
    });
    const auto gcode = Test::slice({ supported_cap() }, config);
    REQUIRE(gcode.find("\nT1") != std::string::npos);
    REQUIRE(gcode.find("\nT2") == std::string::npos);
    check_bed_temperature(gcode, 80, 75);
}

TEST_CASE("Support interface above the first layer does not raise the bed temperature", "[BedTemperature]")
{
    auto config = support_bed_config();
    config.set_deserialize_strict({
        { "support_filament", 1 }, { "print_sequence", GENERATE("by layer", "by object") }
    });
    Print print;
    Model model;
    Test::init_print({ supported_cap() }, print, model, config);
    const auto gcode = Test::gcode(print);
    REQUIRE(gcode.find("\nT1") != std::string::npos);
    const auto ordering = ToolOrdering(*print.objects().front(), unsigned(-1));
    REQUIRE(ordering.front().extruders == std::vector<unsigned int>{ 0 });
    check_bed_temperature(gcode, 60, 55);
}

TEST_CASE("Generated support and raft filaments still determine bed temperature", "[BedTemperature]")
{
    auto config = support_bed_config();
    config.set_deserialize_strict({ { "print_sequence", GENERATE("by layer", "by object") } });
    const bool raft = GENERATE(false, true);
    if (raft)
        config.set_deserialize_strict({ { "enable_support", 0 }, { "raft_layers", 3 } });
    const auto gcode = Test::slice({ raft ? make_cube(20, 20, 20) : supported_cap() }, config);
    REQUIRE(gcode.find("\nT1") != std::string::npos);
    check_bed_temperature(gcode, 80, 75);
}

TEST_CASE("Generated brims retain their filament bed temperature", "[BedTemperature]")
{
    auto config = support_bed_config();
    config.set_deserialize_strict({
        { "enable_support", 0 }, { "wall_filament", 2 },
        { "brim_type", "outer_only" }, { "brim_width", 5 }, { "wall_loops", 1 },
        { "gcode_comments", true },
        { "print_sequence", GENERATE("by layer", "by object") }
    });
    Print print;
    Model model;
    Test::init_print({ TestMesh::cube_20x20x20 }, print, model, config);
    const auto gcode = Test::gcode(print);
    bool brim_generated = false;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.extruding(self) && line.dist_XY(self) > 0 && line.comment().find("brim") != std::string_view::npos)
            brim_generated = true;
    });
    REQUIRE(brim_generated);
    check_bed_temperature(gcode, 80, 75);
}

TEST_CASE("Sequential bed temperature covers every object's first layer", "[BedTemperature]")
{
    auto config = support_bed_config();
    config.set_deserialize_strict({ { "enable_support", 0 }, { "print_sequence", "by object" } });
    Print print;
    Model model;
    Test::init_print({ TestMesh::cube_20x20x20, TestMesh::cube_20x20x20 }, print, model, config);
    for (const char *key : { "wall_filament", "sparse_infill_filament", "solid_infill_filament" })
        model.objects.back()->config.set_key_value(key, new ConfigOptionInt(2));
    print.apply(model, config);
    const auto gcode = Test::gcode(print);
    REQUIRE(gcode.find("\nT1") != std::string::npos);
    check_bed_temperature(gcode, 80, 75);
    // Reusing the print must rebuild the export's set rather than retain filament 2.
    for (const char *key : { "wall_filament", "sparse_infill_filament", "solid_infill_filament" })
        model.objects.back()->config.set_key_value(key, new ConfigOptionInt(1));
    print.apply(model, config);
    check_bed_temperature(Test::gcode(print), 60, 55);
}

TEST_CASE("Mixed filament bed temperature follows its actual first-layer component", "[BedTemperature]")
{
    auto config = support_bed_config();
    MixedFilamentManager manager;
    manager.add_custom_filament(2, 1, 50, { "#FF0000", "#00FF00" });
    config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    config.set_deserialize_strict({
        { "enable_support", 0 }, { "wall_filament", 3 }, { "sparse_infill_filament", 3 }, { "solid_infill_filament", 3 },
        { "print_sequence", "by layer" }
    });
    Print print;
    Model model;
    Test::init_print({ TestMesh::cube_20x20x20 }, print, model, config);
    const auto gcode = Test::gcode(print);
    REQUIRE(gcode.find("\nT0") != std::string::npos);
    REQUIRE(gcode.find("\nT1") != std::string::npos);
    const auto ordering = ToolOrdering(*print.objects().front(), unsigned(-1));
    REQUIRE(ordering.front().extruders == std::vector<unsigned int>{ 1 });
    check_bed_temperature(gcode, 80, 75);
}

TEST_CASE("Prime tower planning does not add unused support to bed temperature", "[BedTemperature]")
{
    auto config = support_bed_config();
    const bool mixed_first_layer = GENERATE(false, true);
    config.set_deserialize_strict({
        { "enable_prime_tower", 1 }, { "nozzle_diameter", "0.4,0.4" },
        { "filament_type", "PLA,PLA" }, { "flush_volumes_matrix", "0,140,140,0" },
        { "layer_change_gcode", "G92 E0" }, { "wall_filament", mixed_first_layer ? 2 : 1 }
    });
    Print print;
    Model model;
    Test::init_print({ TestMesh::cube_20x20x20 }, print, model, config);
    const auto gcode = Test::gcode(print);
    REQUIRE((gcode.find("\nT1") != std::string::npos) == mixed_first_layer);
    if (mixed_first_layer)
        REQUIRE_FALSE(print.wipe_tower_data().tool_changes.empty());
    check_bed_temperature(gcode, mixed_first_layer ? 80 : 60, mixed_first_layer ? 75 : 55);
}

TEST_CASE("Start G-code bed commands expand from actual first-layer filaments", "[BedTemperature]")
{
    auto config = support_bed_config();
    config.set_deserialize_strict({
        { "machine_start_gcode", "M140 S{bed_temperature_initial_layer_single}\nM190 S{bed_temperature_initial_layer_single}" }
    });
    const auto gcode = Test::slice({ TestMesh::cube_20x20x20 }, config);
    CHECK(gcode.find("\nM140 S60\n") != std::string::npos);
    CHECK(gcode.find("\nM190 S60\n") != std::string::npos);
    CHECK(gcode.find("\nM140 S55") != std::string::npos);
    CHECK(gcode.find("M190 S80") == std::string::npos);
}

TEST_CASE("Custom start bed temperature is used for the second-layer transition", "[BedTemperature]")
{
    const auto [script, expect_change] = GENERATE(table<std::string, bool>({{"M190 S80", true},
                                                                            {"M140 S80", true},
                                                                            {"M140 S90\nM190 S60", false},
                                                                            {"M140 S60\nM190 R80", true},
                                                                            {"M190 S60.5", true},
                                                                            {"M140 S0", true}}));
    CAPTURE(script);
    auto config = support_bed_config();
    config.set_deserialize_strict(
        {{"textured_plate_temp", "60,75"}, {"machine_start_gcode", script}, {"before_layer_change_gcode", "; bed test layer"}});
    const auto   gcode        = Test::slice({TestMesh::cube_20x20x20}, config);
    const size_t script_start = gcode.find("\n" + script + "\n");
    REQUIRE(script_start != std::string::npos);
    const auto first_layer = gcode.find("\n; bed test layer\n", script_start);
    REQUIRE(first_layer != std::string::npos);
    CHECK(bed_commands(gcode.substr(0, first_layer)) == bed_commands(script));
    check_second_layer_bed_command(gcode, script_start, expect_change);
}

TEST_CASE("Custom between-object bed temperature is used for the next second-layer transition", "[BedTemperature]")
{
    const int script_temp = GENERATE(60, 80);
    CAPTURE(script_temp);
    auto              config         = support_bed_config();
    const std::string switch_marker  = "; bed test object switch";
    const std::string switch_command = "M140 S" + std::to_string(script_temp);
    config.set_deserialize_strict({{"enable_support", 0},
                                   {"print_sequence", "by object"},
                                   {"textured_plate_temp", "60,75"},
                                   {"before_layer_change_gcode", "; bed test layer"},
                                   {"printing_by_object_gcode", switch_marker + "\n" + switch_command}});
    Print print;
    Model model;
    Test::init_print({TestMesh::cube_20x20x20, TestMesh::cube_20x20x20}, print, model, config);
    const auto gcode        = Test::gcode(print);
    const auto switch_start = gcode.find("\n" + switch_marker + "\n" + switch_command + "\n");
    REQUIRE(switch_start != std::string::npos);
    CHECK(bed_commands(gcode.substr(0, switch_start)) == std::vector<std::string>{"M190 S60"});
    check_second_layer_bed_command(gcode, switch_start, script_temp != 60);
}

TEST_CASE("Current-tool raft uses the tool inherited by sequential export", "[BedTemperature]")
{
    auto config = support_bed_config();
    const bool sequential = GENERATE(false, true);
    config.set_deserialize_strict({
        { "enable_support", 0 }, { "raft_layers", 3 },
        { "support_filament", 0 }, { "support_interface_filament", 0 },
        { "wall_loops", 0 }, { "sparse_infill_filament", 2 }, { "solid_infill_filament", 2 },
        { "print_sequence", sequential ? "by object" : "by layer" }
    });
    const auto gcode = Test::slice({ TestMesh::cube_20x20x20 }, config);
    int tool = -1, first_extruding_tool = -1;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (!line.cmd().empty() && line.cmd().front() == 'T')
            tool = std::stoi(std::string(line.cmd().substr(1)));
        if (first_extruding_tool < 0 && line.extruding(self) && line.dist_XY(self) > 0)
            first_extruding_tool = tool;
    });
    REQUIRE(first_extruding_tool == (sequential ? 1 : 0));
    check_bed_temperature(gcode, sequential ? 80 : 60, sequential ? 75 : 55);
}

TEST_CASE("Bed temperature follows current-tool raft after sequential clearance reorders objects", "[BedTemperature][SequentialClearance]")
{
    auto config = support_bed_config();
    config.set_deserialize_strict({{"enable_support", 0},
                                   {"print_sequence", "by object"},
                                   {"textured_plate_temp_initial_layer", "80,60"},
                                   {"textured_plate_temp", "75,55"},
                                   {"support_filament", 0},
                                   {"support_interface_filament", 0},
                                   {"extruder_clearance_height_to_rod", 27.5},
                                   {"extruder_clearance_height_to_lid", 140},
                                   {"extruder_clearance_radius", 72.5},
                                   {"gcode_label_objects", true}});
    Print print;
    Model model;
    Test::init_print({make_cube(40, 40, 35), make_cube(40, 40, 10)}, print, model, config);
    auto* tall         = model.objects[0];
    auto* short_object = model.objects[1];
    tall->name         = "bed_test_tall";
    short_object->name = "bed_test_short";
    tall->instances.front()->set_offset(Vec3d(60, 130, 0));
    short_object->instances.front()->set_offset(Vec3d(190, 130, 0));
    tall->config.set_key_value("raft_layers", new ConfigOptionInt(3));
    for (const char* key : {"wall_filament", "sparse_infill_filament", "solid_infill_filament"})
        short_object->config.set_key_value(key, new ConfigOptionInt(2));
    print.apply(model, config);

    REQUIRE(Print::sequential_print_clearance_valid(print).string.empty());
    const auto order = sort_object_instances_by_model_order(print);
    REQUIRE(order.size() == 2);
    REQUIRE(order.front()->print_object->model_object() == print.model().objects[1]);

    const auto gcode      = Test::gcode(print);
    const auto tall_start = gcode.find("; printing object bed_test_tall");
    const auto short_end  = gcode.rfind("; stop printing object bed_test_short");
    REQUIRE(tall_start != std::string::npos);
    REQUIRE(short_end != std::string::npos);
    REQUIRE(short_end < tall_start);

    int              tool = -1;
    std::vector<int> tall_tools;
    bool             tall_started = false;
    GCodeReader      reader;
    reader.parse_buffer(gcode, [&](GCodeReader& state, const GCodeReader::GCodeLine& line) {
        if (line.cmd() == "T0")
            tool = 0;
        else if (line.cmd() == "T1")
            tool = 1;
        if (line.raw().find("; printing object bed_test_tall") == 0)
            tall_started = true;
        if (tall_started && line.extruding(state) && line.dist_XY(state) > 0 && (tall_tools.empty() || tall_tools.back() != tool))
            tall_tools.push_back(tool);
    });
    CHECK(tall_tools == std::vector<int>{1, 0});
    CHECK(bed_commands(gcode) == std::vector<std::string>{"M190 S60", "M140 S55", "M140 S60", "M140 S55"});
    check_bed_temperature(gcode, 60, 55);
}
