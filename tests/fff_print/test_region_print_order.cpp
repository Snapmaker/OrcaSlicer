#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

constexpr double DOT_SIZE      = 4.;
constexpr double DOT_PITCH     = 12.;
constexpr double CLUSTER_SHIFT = 60.;
constexpr int    DOTS_PER_SIDE = 3;
constexpr int    NUM_CLUSTERS  = 4;
constexpr int    NUM_DOTS      = NUM_CLUSTERS * DOTS_PER_SIDE * DOTS_PER_SIDE;
constexpr int    DOTS_TOOL     = 1;
// Longer than any move to the next dot of a cluster, shorter than any move to another cluster.
constexpr double LONG_MOVE = 20.;
// Going through the clusters one by one only needs one long move per cluster. Some margin for a dot left behind.
constexpr int MAX_LONG_MOVES = NUM_CLUSTERS + 2;

struct Wall
{
    int         layer;
    int         tool;
    double      x;
    double      y;
    std::string type;
};

// The dots of four clusters far apart, in a sequence that keeps switching between clusters. Their coordinates are interleaved too,
// so sorting them by X or Y keeps switching as well.
static std::vector<Vec2d> dot_positions()
{
    std::vector<Vec2d> dots;
    for (int row = 0; row < DOTS_PER_SIDE; ++row)
        for (int column = 0; column < DOTS_PER_SIDE; ++column)
            for (int cluster = 0; cluster < NUM_CLUSTERS; ++cluster) {
                const int right = cluster % 2;
                const int top   = cluster / 2;
                dots.emplace_back(right * CLUSTER_SHIFT + top * DOT_PITCH / 2. + column * DOT_PITCH,
                                  top * CLUSTER_SHIFT + right * DOT_PITCH / 2. + row * DOT_PITCH);
            }
    return dots;
}

// One connected plate of the first filament, with dots of the second filament. Their head prints one region, spread over the layer.
static std::string slice_scattered_dots()
{
    DynamicPrintConfig config = Test::default_print_config();
    config.set_deserialize_strict({
        { "nozzle_diameter", "0.4,0.4" },
        { "filament_diameter", "1.75,1.75" },
        { "layer_height", 0.2 },
        { "initial_layer_print_height", 0.2 },
        { "skirt_loops", 0 },
        { "brim_type", "no_brim" },
        { "enable_prime_tower", false },
        { "machine_start_gcode", "" },
        { "machine_end_gcode", "" },
    });

    Model        model;
    ModelObject* object = model.add_object();
    object->name        = "dots.stl";

    constexpr double PLATE_MARGIN = 5.;
    constexpr double PLATE_SIZE   = 110.;
    TriangleMesh     plate        = make_cube(PLATE_SIZE, PLATE_SIZE, 1.);
    plate.translate(float(-PLATE_MARGIN), float(-PLATE_MARGIN), 0.f);
    object->add_volume(std::move(plate), ModelVolumeType::MODEL_PART, false)->config.set("extruder", 1);
    for (const Vec2d& dot : dot_positions()) {
        TriangleMesh mesh = make_cube(DOT_SIZE, DOT_SIZE, 1.);
        mesh.translate(float(dot.x()), float(dot.y()), 0.f);
        object->add_volume(std::move(mesh), ModelVolumeType::MODEL_PART, false)->config.set("extruder", 2);
    }
    object->add_instance();
    object->center_around_origin();
    object->instances.front()->set_offset(Vec3d(100., 100., object->instances.front()->get_offset(Z)));
    object->ensure_on_bed();

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    print.process();
    return Test::gcode(print);
}

// The wall extrusions of the head that prints the dots.
static std::vector<Wall> parse_dot_walls(const std::string& gcode)
{
    std::vector<Wall>  walls;
    std::istringstream lines(gcode);
    std::string        line;
    std::string        type;
    int                layer = -1;
    int                tool  = 0;
    double             x = 0., y = 0.;
    auto               value = [](const std::string& text, char axis, double& out) {
        const size_t pos = text.find(std::string(" ") + axis);
        if (pos == std::string::npos)
            return;
        out = std::atof(text.c_str() + pos + 2);
    };
    while (std::getline(lines, line)) {
        if (line.rfind(";LAYER_CHANGE", 0) == 0) {
            ++layer;
        } else if (line.rfind(";TYPE:", 0) == 0) {
            type = line.substr(6);
        } else if (line.size() >= 2 && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1]))) {
            tool = std::atoi(line.c_str() + 1);
        } else if (line.rfind("G1 ", 0) == 0) {
            const std::string code = line.substr(0, line.find(';'));
            double            nx = x, ny = y, e = 0.;
            value(code, 'X', nx);
            value(code, 'Y', ny);
            value(code, 'E', e);
            if (tool == DOTS_TOOL && e > 0. && (nx != x || ny != y) && (type == "Inner wall" || type == "Outer wall"))
                walls.push_back({layer, tool, (x + nx) / 2., (y + ny) / 2., type});
            x = nx;
            y = ny;
        }
    }
    return walls;
}

} // namespace

TEST_CASE("Patches of one region are printed in a short path", "[GCode][RegionOrder]")
{
    const std::vector<Wall> walls = parse_dot_walls(slice_scattered_dots());
    REQUIRE(!walls.empty());

    std::map<int, int> long_moves;
    for (size_t i = 1; i < walls.size(); ++i)
        if (walls[i].layer == walls[i - 1].layer) {
            int& moves = long_moves[walls[i].layer];
            if (std::hypot(walls[i].x - walls[i - 1].x, walls[i].y - walls[i - 1].y) > LONG_MOVE)
                ++moves;
        }
    REQUIRE(!long_moves.empty());
    for (const auto& [layer, moves] : long_moves) {
        CAPTURE(layer);
        CHECK(moves <= MAX_LONG_MOVES);
    }
}

TEST_CASE("Reordering patches keeps the wall sequence of every patch", "[GCode][RegionOrder]")
{
    const std::vector<Wall> walls = parse_dot_walls(slice_scattered_dots());
    REQUIRE(!walls.empty());

    // Every dot gets its inner walls and then its outer wall, without leaving the dot in between.
    std::map<int, std::vector<std::vector<std::string>>> sequences;
    for (size_t i = 0; i < walls.size(); ++i) {
        std::vector<std::vector<std::string>>& layer_sequences = sequences[walls[i].layer];
        if (layer_sequences.empty() || std::hypot(walls[i].x - walls[i - 1].x, walls[i].y - walls[i - 1].y) > DOT_PITCH - DOT_SIZE)
            layer_sequences.emplace_back();
        layer_sequences.back().push_back(walls[i].type);
    }
    for (const auto& [layer, layer_sequences] : sequences) {
        CAPTURE(layer);
        CHECK(layer_sequences.size() == NUM_DOTS);
        for (const std::vector<std::string>& sequence : layer_sequences) {
            CHECK(sequence.front() == "Inner wall");
            CHECK(sequence.back() == "Outer wall");
            CHECK(std::is_sorted(sequence.begin(), sequence.end()));
        }
    }
}
