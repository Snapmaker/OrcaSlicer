// Slicing-level regression tests for the crash / UB / hang guards ported from OrcaSlicer in batch 1A.
#include <catch2/catch.hpp>

#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// Orca #14665: max_layer_height can be shorter than the extruder count (normalization sizes it to
// the filament count under single_extruder_multi_material). calc_max_layer_height() in ToolOrdering
// indexed it per nozzle and read past the end. Shortened directly here to isolate that read.
TEST_CASE("Multi-extruder slice stays in bounds with a short max_layer_height", "[CoreGuards][ToolOrdering]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = { 0.4, 0.4 };
    config.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
    config.option<ConfigOptionStrings>("filament_colour")->values  = { "#FF0000", "#00FF00" };
    config.set_deserialize_strict({ { "max_layer_height", "0.3" } }); // deliberately one entry short
    Print print;
    init_and_process_print({ TestMesh::cube_20x20x20 }, print, config);
    REQUIRE_FALSE(print.objects().front()->layers().empty());
}

namespace {

// A pillar carrying a wide deck, so tree support has something to hold up. Returns the number of
// support layers the generator produced.
size_t slice_overhang_with_tree_support(std::initializer_list<ConfigBase::SetDeserializeItem> overrides)
{
    Print print;
    Model model;
    print.set_status_silent();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", "1" },
        { "support_type", "tree(auto)" },
        { "wall_generator", "classic" },
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
    });
    config.set_deserialize_strict(overrides);

    ModelObject *object = model.add_object();
    object->name        = "pillar_deck";
    TriangleMesh pillar = make_cube(6., 6., 8.);
    pillar.translate(-3.f, -3.f, 0.f);
    object->add_volume(pillar);
    TriangleMesh deck = make_cube(24., 24., 1.6);
    deck.translate(-12.f, -12.f, 8.f);
    object->add_volume(deck);
    object->add_instance();
    object->ensure_on_bed();

    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.process();
    return print.objects().front()->support_layers().size();
}

} // namespace

// Orca #11189: organic tree support with a zero XY distance made calculateAvoidance() divide by a
// zero move step.
TEST_CASE("Organic tree support survives a zero support XY distance", "[CoreGuards][TreeSupport]")
{
    CHECK(slice_overhang_with_tree_support({ { "support_style", "organic" }, { "support_object_xy_distance", "0" } }) > 0);
}

// Orca #11084: Tree Slim spawned its infill with the raw support_base_pattern_spacing, so a zero
// spacing meant a zero line distance.
TEST_CASE("Tree Slim survives a zero support base pattern spacing", "[CoreGuards][TreeSupport]")
{
    CHECK(slice_overhang_with_tree_support({ { "support_style", "tree_slim" }, { "support_base_pattern_spacing", "0" } }) > 0);
}

// Orca #13936: organic tree support with both the XY distance and the top Z gap at zero divided by
// a zero safe movement distance in increase_areas_one_layer().
TEST_CASE("Organic tree support survives zero XY distance and zero top Z gap", "[CoreGuards][TreeSupport]")
{
    CHECK(slice_overhang_with_tree_support({ { "support_style", "organic" },
                                             { "support_object_xy_distance", "0" },
                                             { "support_top_z_distance", "0" },
                                             { "support_bottom_z_distance", "0" } }) > 0);
}
