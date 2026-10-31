#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

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
        { "machine_start_gcode", "" }
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
        { "machine_start_gcode",            "" }
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
