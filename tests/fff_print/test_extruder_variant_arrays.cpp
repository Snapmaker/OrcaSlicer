#include <catch2/catch.hpp>

#include <string>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// Upstream Orca's Custom vendor (02.04.00.07) ships a Klipper toolchanger, MyToolChanger, written in
// Bambu's per-extruder-variant layout: five extruders, each listing three possible variants in
// extruder_variant_list, 15-entry printer_extruder_variant / printer_extruder_id / retraction arrays and
// 30-entry machine limits. Sliced in EdgeSlicer it ran the CLI up to 16 GB or crashed it: the variant
// list made it look like a Bambu dual-nozzle grouping machine, and the grouping read the per-extruder
// arrays it never sizes (extruder_max_nozzle_count, extruder_type: one entry each) past their end, then
// built that many nozzles out of the garbage. These cases slice minimal synthetic presets of that shape.

namespace {

std::vector<std::string> repeat(const std::string &value, size_t n) { return std::vector<std::string>(n, value); }

// A toolchanger of `extruders` tools in the per-variant layout, every per-extruder array the slicer
// itself does not resize left as a preset would leave it.
DynamicPrintConfig variant_layout_config(size_t extruders, const std::string &variant_list, size_t filaments)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(unsigned(extruders));
    config.set_num_filaments(unsigned(filaments));
    config.option<ConfigOptionFloats>("nozzle_diameter")->values = std::vector<double>(extruders, 0.4);
    config.option<ConfigOptionFloats>("filament_diameter")->values = std::vector<double>(filaments, 1.75);
    config.option<ConfigOptionStrings>("filament_colour")->values  = repeat("#FF0000", filaments);
    config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = repeat(variant_list, extruders);

    // The per-variant arrays: one entry per (extruder, variant) pair, the machine limits twice that.
    std::vector<std::string> variants;
    std::vector<int>         ids;
    for (size_t e = 0; e < extruders; ++e)
        for (const char *v : { "Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Extra High Flow" }) {
            variants.emplace_back(v);
            ids.emplace_back(int(e + 1));
        }
    config.option<ConfigOptionStrings>("printer_extruder_variant", true)->values = variants;
    config.option<ConfigOptionInts>("printer_extruder_id", true)->values         = ids;
    config.option<ConfigOptionFloats>("machine_max_speed_x", true)->values       = std::vector<double>(2 * variants.size(), 500.);
    config.option<ConfigOptionFloats>("machine_max_acceleration_y", true)->values = std::vector<double>(2 * variants.size(), 20000.);

    config.option<ConfigOptionBool>("enable_prime_tower")->value = true;
    config.option<ConfigOptionFloats>("wipe_tower_x")->values    = { 15. };
    config.option<ConfigOptionFloats>("wipe_tower_y")->values    = { 15. };
    config.set_deserialize_strict({ { "brim_type", "no_brim" }, { "skirt_loops", "0" }, { "enable_support", "0" } });
    // A toolchanger has a head per filament, not one head fed by several (full_print_config()'s
    // default is the Bambu AMS layout).
    if (extruders > 2)
        config.set_deserialize_strict({ { "single_extruder_multi_material", "0" } });
    return config;
}

// Two 20 mm cubes, the second on filament 2, so the tool ordering has a tool change to plan.
std::string slice_two_cubes(const DynamicPrintConfig &config, Print &print)
{
    Model        model;
    ModelObject *first = model.add_object();
    first->name        = "cube-a.stl";
    first->add_volume(mesh(TestMesh::cube_20x20x20));
    first->add_instance()->set_offset(Vec3d(80., 60., 0.));
    first->ensure_on_bed();
    ModelObject *second = model.add_object();
    second->name        = "cube-b.stl";
    second->add_volume(mesh(TestMesh::cube_20x20x20));
    second->add_instance()->set_offset(Vec3d(120., 60., 0.));
    second->ensure_on_bed();
    second->volumes.front()->config.set("extruder", 2);

    print.apply(model, config);
    return Test::gcode(print);
}

const std::string THREE_VARIANTS = "Direct Drive Standard,Direct Drive High Flow,Direct Drive Extra High Flow";
const std::string TWO_VARIANTS   = "Direct Drive Standard,Direct Drive High Flow";

} // namespace

TEST_CASE("Only one- and two-extruder printers listing several variants are nozzle-grouping machines", "[ExtruderVariant][MultiNozzle]")
{
    int count = 0;
    // X1C-like: one extruder that can take a Standard or a High Flow nozzle.
    DynamicPrintConfig single = variant_layout_config(1, TWO_VARIANTS, 1);
    CHECK(single.support_different_extruders(count));
    CHECK(count == 1);
    // H2D-like.
    DynamicPrintConfig dual = variant_layout_config(2, TWO_VARIANTS, 2);
    CHECK(dual.support_different_extruders(count));
    CHECK(count == 2);
    // MyToolChanger-like: five tools, each listing three variants.
    DynamicPrintConfig toolchanger = variant_layout_config(5, THREE_VARIANTS, 2);
    CHECK_FALSE(toolchanger.support_different_extruders(count));
    CHECK(count == 5);
    // ...which makes it an identical-toolhead printer, filament n on tool n.
    CHECK(is_identical_multi_extruder_printer(toolchanger));
    CHECK(identity_filament_map(toolchanger, 2) == std::vector<int>{ 1, 2 });
}

TEST_CASE("A five-extruder toolchanger in the per-variant layout slices", "[ExtruderVariant][MultiNozzle]")
{
    // On the unfixed build this never returned: the grouping allocated nozzles until memory ran out.
    DynamicPrintConfig config = variant_layout_config(5, THREE_VARIANTS, 2);
    Print              print;
    const std::string  gcode = slice_two_cubes(config, print);
    REQUIRE_FALSE(gcode.empty());
    // Not grouped, so filament 2 stays on tool 2.
    CHECK(print.get_layered_nozzle_group_result() == nullptr);
    CHECK(gcode.find("\nT1") != std::string::npos);
}

TEST_CASE("A two-extruder grouping machine with one-entry per-extruder arrays slices", "[ExtruderVariant][MultiNozzle]")
{
    // A dual-nozzle machine whose preset does not size extruder_max_nozzle_count / extruder_type (only
    // Bambu's fdm_bbl_3dp_002_common does): the grouping must read the missing entries as one
    // Direct Drive nozzle instead of reading past the end of the arrays.
    DynamicPrintConfig config = variant_layout_config(2, TWO_VARIANTS, 2);
    config.set_deserialize_strict({ { "extruder_max_nozzle_count", "1" } });
    int count = 0;
    REQUIRE(config.support_different_extruders(count));

    Print             print;
    const std::string gcode = slice_two_cubes(config, print);
    REQUIRE_FALSE(gcode.empty());
    const auto grouping = print.get_layered_nozzle_group_result();
    REQUIRE(grouping != nullptr);
    for (int extruder : grouping->get_extruder_map(true))
        CHECK((extruder == 0 || extruder == 1));
}
