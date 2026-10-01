#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "../fff_print/test_data.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

// "No sparse layers" on a Bambu Lab (WipeTower, append_tcr) prime tower, checked on the exported G-code.
//
// Owner report (H2D / H2C): towers printed with no sparse layers keep failing to stick. Slicing the
// same job in Bambu Studio showed the compacted Z schedule is identical, but in this fork every
// compacted tower layer - the first one on the bed included - had its wall extruded at travel speed:
// the descent back down to the tower after change_filament_gcode was a bare "G1 Z.. F<travel>", and
// the tower's toolchange wall carries no F word of its own, so it inherited F60000. On top of that the
// "auto" brim (-1, shipped by the H2D/H2C process presets) was never resolved, so the tower had no
// brim at all while the preview drew one.

namespace {

struct TowerMove
{
    double z;       // commanded Z of the extrusion
    double layer_z; // object layer the tower was printed on (; Z_HEIGHT)
    double f;       // feedrate in effect (mm/min)
    double x;
    double y;
};

// Every extruding XY move between WIPE_TOWER_START and WIPE_TOWER_END, with the modal Z and F that
// the printer really uses for it (the last Z / F word seen anywhere before it).
std::vector<TowerMove> tower_moves(const std::string &gcode)
{
    std::vector<TowerMove> out;
    std::istringstream     in(gcode);
    std::string            line;
    double                 x = 0, y = 0, z = 0, f = 0, layer_z = 0;
    bool                   in_tower = false;
    bool                   relative_e = false;
    double                 e = 0;
    const std::regex       word("([XYZEF])(-?[0-9]*\\.?[0-9]+)");
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("; Z_HEIGHT:", 0) == 0) {
            layer_z = std::atof(line.c_str() + 11);
            continue;
        }
        if (line.find("; WIPE_TOWER_START") == 0) { in_tower = true; continue; }
        if (line.find("; WIPE_TOWER_END") == 0) { in_tower = false; continue; }
        const std::string code = line.substr(0, line.find(';'));
        if (code.rfind("M83", 0) == 0) { relative_e = true; continue; }
        if (code.rfind("M82", 0) == 0) { relative_e = false; continue; }
        // G2/G3: a rib wall's fillets are arcs when arc fitting is on (I/J are not needed here).
        if (!(code.rfind("G1 ", 0) == 0 || code.rfind("G0 ", 0) == 0 || code.rfind("G2 ", 0) == 0 || code.rfind("G3 ", 0) == 0))
            continue;
        double nx = x, ny = y, de = 0;
        bool   has_e = false;
        for (auto it = std::sregex_iterator(code.begin(), code.end(), word); it != std::sregex_iterator(); ++it) {
            const char   axis = (*it)[1].str()[0];
            const double v    = std::atof((*it)[2].str().c_str());
            switch (axis) {
            case 'X': nx = v; break;
            case 'Y': ny = v; break;
            case 'Z': z = v; break;
            case 'F': f = v; break;
            case 'E': has_e = true; de = relative_e ? v : v - e; if (!relative_e) e = v; break;
            }
        }
        const bool moves_xy = std::abs(nx - x) > 1e-6 || std::abs(ny - y) > 1e-6;
        if (in_tower && has_e && de > 1e-6 && moves_xy)
            out.push_back({z, layer_z, f, nx, ny});
        x = nx;
        y = ny;
    }
    return out;
}

// A 20 mm cube with walls in filament 1 and sparse infill in filament 2 (solid infill in filament 1),
// plus a 0.6 mm pad in filament 2. Layers 1-2 change filament (the pad), layers 3-6 are the cube's
// single-filament bottom shell (sparse tower layers, skipped), every layer above changes filament
// twice. So from layer 7 on the tower is printed well below the object and the nozzle has to come
// back down to it after every toolchange. (The pad is what makes the plate count as two-filament:
// Print::extruders() does not see per-feature filaments, and a one-filament plate drops the tower.)
DynamicPrintConfig no_sparse_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        // A Bambu-style toolchange: lift clear of the print, then switch. The lift is what the
        // compacted tower has to come back down from.
        { "change_filament_gcode", "G1 Z{max_layer_z + 3.0} F1200\nT[next_extruder]\n" },
        { "layer_height", 0.3 },
        { "initial_layer_print_height", 0.3 },
        { "wall_loops", 1 },
        { "wall_filament", 1 },
        { "sparse_infill_filament", 2 },
        { "solid_infill_filament", 1 },
        { "sparse_infill_density", "15%" },
        { "bottom_shell_layers", 6 },
        { "top_shell_layers", 0 },
        { "enable_support", false },
        { "skirt_loops", 0 },
        { "enable_prime_tower", true },
        { "wipe_tower_no_sparse_layers", true },
        { "prime_tower_width", 30 },
        { "prime_tower_brim_width", -1 },
        { "wipe_tower_x", "140" },
        { "wipe_tower_y", "140" },
        { "travel_speed", 500 },
        { "retraction_length", "0.8,0.8" },
    });
    return config;
}

std::string slice_bbl(const DynamicPrintConfig &config)
{
    Slic3r::Print print;
    Slic3r::Model model;
    ModelObject  *object = model.add_object();
    object->name         = "cube.stl";
    object->add_volume(Slic3r::Test::mesh(Slic3r::Test::TestMesh::cube_20x20x20));
    object->add_instance()->set_offset(Vec3d(40., 40., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    ModelObject *pad = model.add_object();
    pad->name        = "pad.stl";
    pad->add_volume(make_cube(10., 10., 0.6));
    pad->add_instance()->set_offset(Vec3d(80., 40., 0.));
    pad->ensure_on_bed();
    pad->config.set("extruder", 2);
    print.apply(model, config);
    print.is_BBL_printer() = true;
    const StringObjectException err = print.validate();
    INFO(err.string);
    REQUIRE(err.string.empty());
    return Slic3r::Test::gcode(print);
}

// The checks below for one wall type of the Bambu Lab generator (the rib wall squares the tower
// and adds ribs, with arcs for its fillets).
void check_no_sparse_tower(const std::string &wall)
{
    DynamicPrintConfig config = no_sparse_config();
    config.set_deserialize_strict({ { "wipe_tower_wall_type", wall }, { "enable_arc_fitting", 1 } });
    INFO("wall type " << wall);
    const std::string            gcode  = slice_bbl(config);
    const std::vector<TowerMove> moves  = tower_moves(gcode);
    REQUIRE(!moves.empty());

    const double layer_height = 0.3;
    const double travel_f     = 60. * 500.;

    // The case only means something if the tower really was compacted below the object.
    const bool compacted = std::any_of(moves.begin(), moves.end(),
                                       [](const TowerMove &m) { return m.z < m.layer_z - 1e-3; });
    REQUIRE(compacted);

    SECTION("no tower extrusion runs at travel feedrate")
    {
        // The tower's own speeds top out at 90 mm/s (F5400); the first layer is slower still.
        double worst = 0;
        for (const TowerMove &m : moves)
            worst = std::max(worst, m.f);
        INFO("fastest tower extrusion F" << worst << ", travel F" << travel_f);
        CHECK(worst <= 5400. + 1e-3);
        CHECK(worst < travel_f);
    }

    SECTION("every tower layer sits exactly one layer on top of the last one")
    {
        // Compaction must never leave a gap under a tower layer (printing into air) nor print below it.
        double top = 0;
        for (const TowerMove &m : moves) {
            if (m.z > top + 1e-3) {
                INFO("tower layer at z " << m.z << " (object layer " << m.layer_z << "), tower top was " << top);
                CHECK(m.z <= top + layer_height + 1e-3);
                top = m.z;
            } else {
                INFO("tower extrusion at z " << m.z << " below the tower top " << top);
                CHECK(m.z >= top - 1e-3);
            }
        }
    }

    SECTION("the first tower layer is on the bed and carries the auto brim")
    {
        const double first_z = std::min_element(moves.begin(), moves.end(),
                                                [](const TowerMove &a, const TowerMove &b) { return a.z < b.z; })->z;
        CHECK(std::abs(first_z - 0.3) < 1e-3);
        auto span_at = [&moves](double z) {
            double xmin = 1e9, xmax = -1e9;
            for (const TowerMove &m : moves)
                if (std::abs(m.z - z) < 1e-3) {
                    xmin = std::min(xmin, m.x);
                    xmax = std::max(xmax, m.x);
                }
            return xmax - xmin;
        };
        // The object is 20 mm tall, so the auto brim is 20 / 100 * 8 = 1.6 mm per side: the first
        // layer must reach well past the tower body. Without the brim a rectangle spans exactly 30 mm.
        const double first_span = span_at(first_z);
        INFO("first tower layer spans " << first_span << " mm");
        if (wall == "rectangle")
            CHECK(first_span > 30. + 2.);
        else {
            // A rib tower is squared off: compare with the sixth tower layer, where the brim chamfer
            // (one line narrower per layer) has run out and the ribs have barely tapered.
            const double upper_span = span_at(first_z + 5. * layer_height);
            INFO("sixth tower layer spans " << upper_span << " mm");
            CHECK(upper_span > 0.);
            CHECK(first_span > upper_span + 2.);
        }
    }
}

} // namespace

TEST_CASE("No-sparse Bambu tower: compacted layers print at tower speed on a supported, brimmed base", "[WipeTower][NoSparseLayers]")
{
    for (const char *wall : { "rectangle", "rib" })
        DYNAMIC_SECTION("wall " << wall) { check_no_sparse_tower(wall); }
}

// Owner report (H2D, PR #226 test copy, but main does it too): with "no sparse layers" on, slicing
// stopped with "Conflicts of G-code paths have been found ... (WipeTower <-> Assembly)" although
// the tower stood well clear of every object. The plate's second filament only started far up the
// print, so the tower's first real layer (its first tool change) was hundreds of layers up. The
// brim chamfer counted "layers since the first layer" for the sparse layers below it as a negative
// number and added one brim loop per layer down. Those layers are never printed, but their walls
// went into the fake tower the conflict checker tests against: hundreds of loops spreading across
// the bed. Here the second filament starts at 10 mm and a 1 mm chip stands about 10 mm from the tower.
TEST_CASE("No-sparse Bambu tower: layers below the first tool change get no brim loops", "[WipeTower][NoSparseLayers]")
{
    DynamicPrintConfig config = no_sparse_config();
    config.set_deserialize_strict({ { "wipe_tower_wall_type", "rib" } });

    Slic3r::Print print;
    Slic3r::Model model;
    // Filament 1 up to 10 mm, filament 2 above it: no tool change at all below 10 mm.
    ModelObject *stack = model.add_object();
    stack->name        = "stack.stl";
    ModelVolume *lower = stack->add_volume(make_cube(20., 20., 10.));
    ModelVolume *upper = stack->add_volume(make_cube(20., 20., 10.));
    upper->set_offset(lower->get_offset() + Vec3d(0., 0., 10.));
    lower->config.set("extruder", 1);
    upper->config.set("extruder", 2);
    stack->add_instance()->set_offset(Vec3d(40., 40., 0.));
    stack->ensure_on_bed();
    // A low chip beside the tower (x 140..170), about 10 mm off its left side.
    ModelObject *chip = model.add_object();
    chip->name        = "chip.stl";
    chip->add_volume(make_cube(10., 10., 0.9));
    chip->add_instance()->set_offset(Vec3d(120., 145., 0.));
    chip->ensure_on_bed();
    chip->config.set("extruder", 1);

    print.apply(model, config);
    print.is_BBL_printer() = true;
    const StringObjectException err = print.validate();
    INFO(err.string);
    REQUIRE(err.string.empty());
    const std::string gcode = Slic3r::Test::gcode(print);

    // The checker's view of the tower: no layer reaches further than the tower wall at the bed (the
    // widest, as the ribs taper upwards) plus a brim. The wall is the first outline a layer records;
    // the brim loops follow it. The brim is at most 3 mm (the auto brim here is 1.6 mm).
    const std::map<float, Polylines> &walls = print.get_fake_wipe_tower().outer_wall;
    REQUIRE(walls.size() > 10);
    REQUIRE(!walls.begin()->second.empty());
    BoundingBox first = get_extents(walls.begin()->second.front());
    first.offset(scaled(3.5));
    for (const auto &[z, polylines] : walls) {
        BoundingBox b;
        for (const Polyline &pl : polylines)
            b.merge(get_extents(pl));
        INFO("tower wall at z " << z << ": " << polylines.size() << " polylines, " << unscaled(b.size().x()) << " x "
             << unscaled(b.size().y()) << " mm; allowed " << unscaled(first.size().x()) << " x " << unscaled(first.size().y()));
        CHECK(first.contains(b.min));
        CHECK(first.contains(b.max));
    }

    // ...so the chip does not "conflict" with it.
    const ConflictResultOpt conflict = print.get_conflict_result();
    INFO("conflict: " << (conflict ? conflict->_objName1 + " <-> " + conflict->_objName2 + " at " + std::to_string(conflict->_height) : std::string("none")));
    CHECK(!conflict.has_value());

    // And the printed tower never reaches past its first layer either.
    const std::vector<TowerMove> moves = tower_moves(gcode);
    REQUIRE(!moves.empty());
    const double first_z = std::min_element(moves.begin(), moves.end(), [](const TowerMove &a, const TowerMove &b) { return a.z < b.z; })->z;
    double xmin = 1e9, xmax = -1e9;
    for (const TowerMove &m : moves)
        if (std::abs(m.z - first_z) < 1e-3) {
            xmin = std::min(xmin, m.x);
            xmax = std::max(xmax, m.x);
        }
    for (const TowerMove &m : moves) {
        INFO("tower extrusion at z " << m.z << " x " << m.x << ", first layer spans x " << xmin << ".." << xmax);
        CHECK(m.x >= xmin - 0.01);
        CHECK(m.x <= xmax + 0.01);
    }
}
