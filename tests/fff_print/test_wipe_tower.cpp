#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/GCodeReader.hpp"

#include "test_data.hpp"

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <iostream>
#include <limits>
#include <string>

using namespace Slic3r;

namespace Slic3r { namespace Test {

// A single cube on extruder 1 of a 2-extruder machine with the wipe tower enabled. The raft
// has one layer and the support/raft base filament is set to extruder 2. A one-layer raft is
// generated as an interface-only support layer that resolves to the object's extruder, so
// the print never changes tools. The raft layer below the object is marked has_wipe_tower
// regardless, which produces a degenerate zero-depth tower without any block;
// WipeTower2::finish_layer_new then builds a zero-height wall polygon and crashes in
// generate_support_wall_new dereferencing an empty union_ result.
static void init_degenerate_raft_print(Print& print)
{
    DynamicPrintConfig config = Test::default_print_config();
    config.set_deserialize_strict({
        { "nozzle_diameter", "0.4,0.4" },
        { "filament_diameter", "1.75,1.75" },
        { "filament_type", "PLA,PLA" },
        { "flush_volumes_matrix", "0,140,140,0" },
        { "enable_prime_tower", true },
        { "raft_layers", 1 },
        { "support_filament", 2 },
        { "machine_start_gcode", "" },
        // Relative-E Marlin flavor for a non-BBL printer requires G92 E0 at each layer
        // (Print::validate), while the wipe tower itself requires relative addressing.
        { "layer_change_gcode", "G92 E0" }
        });

    Model model;
    ModelObject* object = model.add_object();
    object->name = "cube.stl";
    object->add_volume(make_cube(20., 20., 10.));
    object->add_instance();
    object->ensure_on_bed();
    object->config.set("extruder", 1);

    print.set_status_silent();
    print.is_BBL_printer() = false; // make sure the WipeTower2 code path is taken
    print.apply(model, config);
    const StringObjectException validation = print.validate();
    INFO("validate() error: " << validation.string);
    REQUIRE(validation.string.empty());
}

// Two cubes on extruders 2 and 3 of a 3-extruder machine with the wipe tower enabled.
// "filament_adhesiveness_category" is deliberately shorter than the extruder count (the
// default single-entry value behaves the same way on a multi-extruder machine), so
// extruder 3 resolves to a wrapped category that never takes part in a tool change.
// Wipe tower blocks are only created for categories of tools that change, therefore
// extruder 3 has no block of its own category and WipeTower2::finish_layer_new used to
// dereference a null block when finishing a layer with the wall filament (Sentry:
// EXC_BAD_ACCESS / KERN_INVALID_ADDRESS / 0x58).
static void init_multi_extruder_print(Print& print, const std::string& adhesiveness_categories, int wall_filament)
{
    DynamicPrintConfig config = Test::default_print_config();
    config.set_deserialize_strict({
        { "nozzle_diameter",                "0.4,0.4,0.4" },
        { "filament_diameter",              "1.75,1.75,1.75" },
        { "filament_type",                  "PLA,PLA,PLA" },
        { "flush_volumes_matrix",           "0,140,140,140,0,140,140,140,0" },
        { "enable_prime_tower",             true },
        { "filament_adhesiveness_category", adhesiveness_categories },
        { "wipe_tower_filament",            wall_filament },
        { "machine_start_gcode",            "" },
        // Same validation constraint as above: relative-E Marlin needs G92 E0 per layer.
        { "layer_change_gcode",             "G92 E0" }
        });

    Model model;
    for (int i = 0; i < 2; ++i) {
        ModelObject* object = model.add_object();
        object->name = "cube_" + std::to_string(i + 1) + ".stl";
        object->add_volume(make_cube(20., 20., 10.));
        object->add_instance();
        object->instances[0]->set_offset(Vec3d(60. * i, 0., 0.));
        object->ensure_on_bed();
        // Extruder 1 is never used, so tool changes only ever involve extruders 2 and 3.
        object->config.set("extruder", i + 2);
    }

    print.set_status_silent();
    print.is_BBL_printer() = false; // make sure the WipeTower2 code path is taken
    print.apply(model, config);
    const StringObjectException validation = print.validate();
    INFO("validate() error: " << validation.string);
    REQUIRE(validation.string.empty());
}

} } // namespace Slic3r::Test

SCENARIO("Wipe tower with a single cube on extruder 1 and the raft base on extruder 2", "[WipeTower]")
{
    GIVEN("2 extruders, one cube on extruder 1, wipe tower enabled, 1 raft layer printed with extruder 2")
    {
        Print print;
        Test::init_degenerate_raft_print(print);

        WHEN("the print is sliced")
        {
            THEN("the degenerate single-material tower does not crash the slicing")
            {
                REQUIRE_NOTHROW(print.process());
            }
            THEN("gcode is generated for the raft and the object without tool changes")
            {
                const std::string gcode = Test::gcode(print);
                REQUIRE(gcode.find(";TYPE:Support interface") != std::string::npos);
                REQUIRE(gcode.find("CP TOOLCHANGE") == std::string::npos);
            }
        }
    }
}

SCENARIO("Wipe tower wall filament without a block of its own category", "[WipeTower]")
{
    GIVEN("3 extruders, objects on extruders 2/3, adhesiveness categories for 2 extruders only")
    {
        Print print;
        Test::init_multi_extruder_print(print, "50,60", 3);

        WHEN("the print is sliced")
        {
            THEN("the layer finish does not dereference a null wipe tower block")
            {
                REQUIRE_NOTHROW(print.process());
            }
            THEN("wipe tower gcode is still generated")
            {
                const std::string gcode = Test::gcode(print);
                REQUIRE(gcode.find("WIPE_TOWER_START") != std::string::npos);
            }
        }
    }
}

SCENARIO("Wipe tower with fully defined adhesiveness categories", "[WipeTower]")
{
    GIVEN("the same print with one category per extruder")
    {
        Print print;
        Test::init_multi_extruder_print(print, "50,60,70", 3);

        WHEN("the print is sliced")
        {
            THEN("slicing succeeds") { REQUIRE_NOTHROW(print.process()); }

            THEN("wipe tower gcode with tool changes is generated")
            {
                const std::string gcode = Test::gcode(print);
                REQUIRE(gcode.find("WIPE_TOWER_START") != std::string::npos);
                REQUIRE(gcode.find("CP TOOLCHANGE START") != std::string::npos);
            }
        }
    }
}

// Two cubes on extruders 1 and 2 of a 2-extruder machine with the wipe tower enabled and a
// non-zero z_offset. Every layer prints both objects, so a toolchange with tower gcode runs
// right after the deferred layer change: the writer still sits at the previous layer's
// physical Z while nominal_z already names the new layer. The tower used to default its
// Two cubes on extruders 1 and 2 of a 2-extruder machine with the wipe tower enabled and a
// non-zero z_offset. Toolchanges with tower gcode run every layer; with the deferred layer
// change the tower used to default its print height and its tower-exit height to the writer's
// stale physical Z. Both heights must resolve to the logical layer Z instead, in the
// z_offset-shifted gcode frame. (The layer-top toolchange variant of this regression - tower
// ahead of the first extrusion of a layer, as produced by per-layer color changes - needs the
// GUI plate custom-gcode path to build the tower and is covered by the reviewer's exported
// G-code regression on MR !92 instead of this unit-level test.)
SCENARIO("Wipe tower toolchanges print and return at the logical layer height", "[WipeTower][LayerZ]")
{
    GIVEN("2 extruders, one cube per extruder, wipe tower enabled, z_offset 0.1")
    {
        Print print;
        DynamicPrintConfig config = Test::default_print_config();
        config.set_deserialize_strict({
            { "nozzle_diameter",                "0.4,0.4" },
            { "filament_diameter",              "1.75,1.75" },
            { "filament_type",                  "PLA,PLA" },
            { "flush_volumes_matrix",           "0,140,140,0" },
            { "filament_adhesiveness_category", "50,60" },
            { "enable_prime_tower",             true },
            { "layer_height",                   0.2 },
            { "initial_layer_print_height",     0.3 },
            { "z_offset",                       0.1 },
            { "z_hop",                          0. },
            { "machine_start_gcode",            "" },
            // Relative-E Marlin flavor requires G92 E0 at each layer (Print::validate).
            { "layer_change_gcode",             "G92 E0" }
        });

        Model model;
        for (int i = 0; i < 2; ++i) {
            ModelObject* object = model.add_object();
            object->name = "cube_" + std::to_string(i + 1) + ".stl";
            object->add_volume(make_cube(20., 20., 4.));
            object->add_instance();
            object->instances[0]->set_offset(Vec3d(60. * i, 0., 0.));
            object->ensure_on_bed();
            object->config.set("extruder", i + 1);
        }

        print.set_status_silent();
        print.is_BBL_printer() = false; // make sure the WipeTower2 code path is taken
        print.apply(model, config);
        const StringObjectException validation = print.validate();
        INFO("validate() error: " << validation.string);
        REQUIRE(validation.string.empty());

        WHEN("the print is sliced")
        {
            THEN("tower toolchanges extrude at the current layer Z and never dip below it")
            {
                const std::string gcode = Test::gcode(print);
                REQUIRE(gcode.find("CP TOOLCHANGE START") != std::string::npos);

                // Walk the gcode line by line: ";Z:" marks the layer being printed (raw frame,
                // without z_offset), G1 Z sets the modal height, extrusions inherit it. Every
                // extrusion must sit at the current layer's Z (z_offset-shifted frame); a tower
                // printed at the previous layer shows up as an extrusion below the current layer.
                const double z_offset = 0.1;
                double modal_z = -1., current_layer_z = -1.;
                int    extrusions = 0, wrong_height = 0, z_dips = 0;
                double last_extrusion_z = -std::numeric_limits<double>::max();
                std::istringstream lines(gcode);
                std::string line_str;
                while (std::getline(lines, line_str)) {
                    if (line_str.rfind(";Z:", 0) == 0) {
                        current_layer_z = std::atof(line_str.c_str() + 3);
                    } else if (line_str.rfind("G1 ", 0) == 0) {
                        const auto num = [&line_str](const char axis) -> double {
                            const size_t p = line_str.find(axis);
                            if (p == std::string::npos) return std::numeric_limits<double>::quiet_NaN();
                            return std::atof(line_str.c_str() + p + 1);
                        };
                        const double nz = num('Z'), ne = num('E');
                        const double nx = num('X'), ny = num('Y');
                        if (!std::isnan(nz)) modal_z = nz;
                        const bool has_xy = (!std::isnan(nx) && std::abs(nx) > 1e-9) || (!std::isnan(ny) && std::abs(ny) > 1e-9);
                        const bool extruding = !std::isnan(ne) && ne > 1e-9;
                        if (extruding && has_xy && modal_z > 0. && current_layer_z > 0.) {
                            // The first-layer skirt prints at the raw first-layer height, everything
                            // else (objects, tower toolchanges) at the z_offset-shifted layer Z.
                            const double expected = current_layer_z + z_offset;
                            const bool first_layer_skirt = std::abs(current_layer_z - 0.3) < 1e-9 && std::abs(modal_z - 0.3) < 0.05;
                            if (!first_layer_skirt && std::abs(modal_z - expected) > 0.05)
                                ++wrong_height;
                            if (modal_z < last_extrusion_z - 0.05)
                                ++z_dips;
                            last_extrusion_z = std::max(last_extrusion_z, modal_z);
                            ++extrusions;
                        } else if (!extruding && !std::isnan(nz) && nz < last_extrusion_z - 0.05) {
                            // A travel may not sink below the highest extruded Z (the old
                            // tower-exit leg did exactly that).
                            ++z_dips;
                        }
                    }
                }
                CAPTURE(extrusions, wrong_height, z_dips);
                REQUIRE(extrusions > 100);
                REQUIRE(wrong_height == 0);
                REQUIRE(z_dips == 0);
            }
        }
    }
}
