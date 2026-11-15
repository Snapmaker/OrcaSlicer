#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

#include <string>

using namespace Slic3r;

namespace {

std::string repeated(const std::string &value, size_t count)
{
    std::string out;
    for (size_t i = 0; i < count; ++i)
        out += (i == 0 ? "" : ",") + value;
    return out;
}

// One object made of two parts on a 17-filament machine: a 20 x 20 x 10 mm base on filament 1 and a
// 10 x 10 x 5 mm inlay in its upper half on `inlay_filament`. The lower half of the object is printed
// with filament 1 alone, the upper half with both filaments.
void init_inlay_print(Print &print, int inlay_filament)
{
    const size_t num_filaments = 17;

    DynamicPrintConfig config = Test::default_print_config();
    config.set_deserialize_strict({
        { "nozzle_diameter",      repeated("0.4", num_filaments) },
        { "filament_diameter",    repeated("1.75", num_filaments) },
        { "filament_type",        repeated("PLA", num_filaments) },
        { "flush_volumes_matrix", repeated("140", num_filaments * num_filaments) },
        { "enable_prime_tower",   false },
        { "machine_start_gcode",  "" },
        // Relative-E Marlin flavor for a non-BBL printer requires G92 E0 at each layer (Print::validate).
        { "layer_change_gcode",   "G92 E0" }
        });

    Model model;
    ModelObject *object = model.add_object();
    object->name = "inlay.stl";

    ModelVolume *base = object->add_volume(make_cube(20., 20., 10.));
    base->config.set("extruder", 1);

    TriangleMesh inlay_mesh = make_cube(10., 10., 5.);
    inlay_mesh.translate(5.f, 5.f, 5.f);
    ModelVolume *inlay = object->add_volume(std::move(inlay_mesh));
    inlay->config.set("extruder", inlay_filament);

    object->add_instance();
    object->ensure_on_bed();

    print.set_status_silent();
    print.is_BBL_printer() = false;
    print.apply(model, config);
    const StringObjectException validation = print.validate();
    INFO("validate() error: " << validation.string);
    REQUIRE(validation.string.empty());
}

// Slices the inlay print and counts the tool changes to the inlay filament.
size_t inlay_tool_changes(int inlay_filament)
{
    Print print;
    init_inlay_print(print, inlay_filament);
    const std::string gcode = Test::gcode(print);

    const std::string tool_change = "\nT" + std::to_string(inlay_filament - 1) + "\n";
    size_t            count       = 0;
    for (size_t pos = gcode.find(tool_change); pos != std::string::npos; pos = gcode.find(tool_change, pos + 1))
        ++count;
    return count;
}

} // namespace

// The tool order of a layer is cached under a key built from the layer's filaments and the filament
// that was active before it. The two halves of the key used to overlap from filament 17 on, so a layer
// printing filaments 1 and 17 after filament 1 took the cached order of a layer printing filament 1
// alone, and filament 17 was never printed.
SCENARIO("Tool ordering keeps a filament above 16 that joins a layer of filament 1", "[ToolOrdering]")
{
    GIVEN("17 filaments and a base on filament 1 with an inlay in its upper half")
    {
        WHEN("the inlay is on filament 16")
        {
            const size_t tool_changes_16 = inlay_tool_changes(16);

            THEN("filament 16 is selected for the inlay") { REQUIRE(tool_changes_16 > 0); }

            AND_WHEN("the inlay is on filament 17")
            {
                const size_t tool_changes_17 = inlay_tool_changes(17);

                THEN("filament 17 is selected as often as filament 16 was") { REQUIRE(tool_changes_17 == tool_changes_16); }
            }
        }
    }
}
