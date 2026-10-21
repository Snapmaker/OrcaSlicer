#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

#include <string>

using namespace Slic3r;

namespace Slic3r { namespace Test {

// Two cubes on extruders 2 and 3 of a 3-extruder machine with the wipe tower enabled.
// "filament_adhesiveness_category" is deliberately shorter than the extruder count (the
// default single-entry value behaves the same way on a multi-extruder machine), so
// extruder 3 resolves to a wrapped category that never takes part in a tool change.
// Wipe tower blocks are only created for categories of tools that change, therefore
// extruder 3 has no block of its own category and WipeTower2::finish_layer_new used to
// dereference a null block when finishing a layer with the wall filament (Sentry:
// EXC_BAD_ACCESS / KERN_INVALID_ADDRESS / 0x58).
static void init_wipe_tower_print(Print& print, const std::string& adhesiveness_categories, int wall_filament)
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
    print.validate();
}

} } // namespace Slic3r::Test

SCENARIO("Wipe tower wall filament without a block of its own category", "[WipeTower]")
{
    GIVEN("3 extruders, objects on extruders 2/3, adhesiveness categories for 2 extruders only")
    {
        Print print;
        Test::init_wipe_tower_print(print, "50,60", 3);

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
        Test::init_wipe_tower_print(print, "50,60,70", 3);

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
