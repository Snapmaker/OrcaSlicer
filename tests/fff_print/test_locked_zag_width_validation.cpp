#include <catch2/catch.hpp>

#include <string>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// skin_infill_line_width / skeleton_infill_line_width default to 100% of the nozzle. On a 0.2 mm nozzle
// sliced at 0.20 mm layers that is a 0.2 mm width, not above the layer height, so Print::validate()
// refused every such preset that does not set them with "Too small line width" (CLI return code -51:
// Creality CR-10 SE / Ender-3 V3 KE / V3 SE 0.2, Elegoo Neptune 4 Max 0.2, ...). Only Locked Zag
// infill prints with those widths, so only Locked Zag regions are held to them.

namespace {

DynamicPrintConfig fine_nozzle_config(const char *sparse_pattern)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "nozzle_diameter",                  "0.2" },
        { "layer_height",                     "0.2" },
        { "initial_layer_print_height",       "0.2" },
        { "line_width",                       "0.25" },
        { "initial_layer_line_width",         "0.25" },
        { "inner_wall_line_width",            "0.25" },
        { "outer_wall_line_width",            "0.25" },
        { "sparse_infill_line_width",         "0.25" },
        { "internal_solid_infill_line_width", "0.25" },
        { "top_surface_line_width",           "0.25" },
        { "support_line_width",               "0.25" },
        { "sparse_infill_pattern",            sparse_pattern },
        { "enable_support",                   "0" },
        { "skirt_loops",                      "0" },
        { "brim_type",                        "no_brim" },
        // full_print_config() has relative E, which validate() refuses without a per-layer reset.
        { "layer_change_gcode",               "G92 E0" },
    });
    return config;
}

StringObjectException validate(const DynamicPrintConfig &config)
{
    Model model = Test::model("cube", Test::mesh(TestMesh::cube_20x20x20));
    for (ModelObject *object : model.objects) {
        object->instances.front()->set_offset(Vec3d(100., 100., 0.));
        object->ensure_on_bed();
    }
    Print print;
    for (ModelObject *object : model.objects)
        print.auto_assign_extruders(object);
    print.apply(model, config);
    return print.validate();
}

} // namespace

TEST_CASE("Skin and skeleton widths left at their default do not block a 0.2 nozzle at 0.20 mm layers", "[Print][Validate][LockedZag]")
{
    const char *pattern = GENERATE("grid", "crosshatch", "gyroid", "rectilinear");
    CAPTURE(pattern);
    const StringObjectException err = validate(fine_nozzle_config(pattern));
    INFO(err.string << " (" << err.opt_key << ")");
    CHECK(err.string.empty());
}

TEST_CASE("Locked Zag still validates its skin and skeleton widths", "[Print][Validate][LockedZag]")
{
    DynamicPrintConfig config = fine_nozzle_config("lockedzag");
    SECTION("the 100% default is not wider than the 0.20 mm layer") {
        const StringObjectException err = validate(config);
        CHECK(err.string == "Too small line width");
        CHECK((err.opt_key == "skin_infill_line_width" || err.opt_key == "skeleton_infill_line_width"));
    }
    SECTION("explicit widths above the layer height pass") {
        config.set_deserialize_strict({ { "skin_infill_line_width", "0.25" }, { "skeleton_infill_line_width", "0.25" } });
        const StringObjectException err = validate(config);
        INFO(err.string << " (" << err.opt_key << ")");
        CHECK(err.string.empty());
    }
}
