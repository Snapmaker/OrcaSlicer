#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

// The arrange clearance that ArrangePolygon still calls brim_width is meant for support material.
// "Enable support" is on in most global presets, so before this a flat print - one that generates no
// support at all - was still spaced 6 mm (24 mm with tree support) from its neighbours, and Fill bed's
// gap floor blamed a brim for it. The clearance now follows whether support would actually attach.
// get_instance_arrange_poly is the entry point Arrange, Fill bed and the Fill bed dialog all use.

namespace {

DynamicPrintConfig config_with_support(SupportType type, int threshold_deg = 30)
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_key_value("enable_support", new ConfigOptionBool(true));
    cfg.set_key_value("support_type", new ConfigOptionEnum<SupportType>(type));
    cfg.set_key_value("support_threshold_angle", new ConfigOptionInt(threshold_deg));
    return cfg;
}

ModelInstance *single_instance(Model &model, const indexed_triangle_set &its)
{
    ModelObject *obj = model.add_object();
    obj->name = "part";
    obj->add_volume(TriangleMesh(its));
    return obj->add_instance();
}

// A slab held up by a pillar: the slab's underside at z = 10 faces straight down with air below it.
indexed_triangle_set make_table()
{
    indexed_triangle_set pillar = its_make_cube(4., 4., 10.);
    indexed_triangle_set slab   = its_make_cube(20., 20., 5.);
    for (Vec3f &v : pillar.vertices) v += Vec3f(8.f, 8.f, 0.f);
    for (Vec3f &v : slab.vertices) v.z() += 10.f;
    its_merge(pillar, slab);
    return pillar;
}

} // namespace

TEST_CASE("Arrange clearance: a flat print with support enabled gets no support clearance", "[ArrangeClearance]")
{
    Model model;
    ModelInstance *inst = single_instance(model, its_make_cube(20., 20., 10.));
    // The cube's only down-facing facets are its base on the bed: nothing for support to attach to.
    CHECK_FALSE(instance_may_get_support(*inst, stNormalAuto, 30));
    CHECK_FALSE(instance_may_get_support(*inst, stTreeAuto, 30));
    CHECK(get_instance_arrange_poly(inst, config_with_support(stNormalAuto)).brim_width == Approx(1.0));
    CHECK(get_instance_arrange_poly(inst, config_with_support(stTreeAuto)).brim_width == Approx(1.0));
    CHECK_FALSE(get_instance_arrange_poly(inst, config_with_support(stTreeAuto)).has_tree_support);
}

TEST_CASE("Arrange clearance: an overhang keeps the normal and tree support clearances", "[ArrangeClearance]")
{
    Model model;
    ModelInstance *inst = single_instance(model, make_table());
    CHECK(instance_may_get_support(*inst, stNormalAuto, 30));
    CHECK(instance_may_get_support(*inst, stTreeAuto, 30));
    CHECK(get_instance_arrange_poly(inst, config_with_support(stNormalAuto)).brim_width == Approx(6.0));
    const auto tree = get_instance_arrange_poly(inst, config_with_support(stTreeAuto));
    CHECK(tree.brim_width == Approx(24.0));
    CHECK(tree.has_tree_support);
}

TEST_CASE("Arrange clearance: the threshold angle decides, in world space", "[ArrangeClearance]")
{
    // A cube tipped 45 deg about X has two faces at 45 deg from the horizontal, one of them facing
    // down. Below a 30 deg threshold that slope needs no support; at 60 deg it does.
    Model model;
    ModelInstance *inst = single_instance(model, its_make_cube(10., 10., 10.));
    inst->set_rotation(Vec3d(M_PI / 4.0, 0., 0.));
    CHECK_FALSE(instance_may_get_support(*inst, stNormalAuto, 30));
    CHECK(instance_may_get_support(*inst, stNormalAuto, 60));
    CHECK(get_instance_arrange_poly(inst, config_with_support(stNormalAuto, 30)).brim_width == Approx(1.0));
    CHECK(get_instance_arrange_poly(inst, config_with_support(stNormalAuto, 60)).brim_width == Approx(6.0));
    // Threshold 0 is "auto" in the UI and stands in for the slicer's 30 deg default.
    CHECK_FALSE(instance_may_get_support(*inst, stNormalAuto, 0));
}

TEST_CASE("Arrange clearance: manual support kinds need a painted enforcer", "[ArrangeClearance]")
{
    Model model;
    ModelInstance *inst = single_instance(model, make_table());
    // Same overhang as above, but the manual kinds only support what the user painted - nothing yet.
    CHECK_FALSE(instance_may_get_support(*inst, stNormal, 30));
    CHECK_FALSE(instance_may_get_support(*inst, stTree, 30));
    CHECK(get_instance_arrange_poly(inst, config_with_support(stNormal)).brim_width == Approx(1.0));
    CHECK(get_instance_arrange_poly(inst, config_with_support(stTree)).brim_width == Approx(1.0));
}

TEST_CASE("Arrange clearance: support disabled stays at the flat clearance", "[ArrangeClearance]")
{
    Model model;
    ModelInstance *inst = single_instance(model, make_table());
    DynamicPrintConfig cfg = config_with_support(stNormalAuto);
    cfg.set_key_value("enable_support", new ConfigOptionBool(false));
    CHECK(get_instance_arrange_poly(inst, cfg).brim_width == Approx(1.0));
}

namespace {

DynamicPrintConfig packed_std_hf_temp_config()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    cfg.option<ConfigOptionInts>("filament_flow_step_size", true)->values = {2, 1};
    cfg.option<ConfigOptionStrings>("filament_flow_support", true)->values =
        {FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW, FLOW_MODE_STANDARD};
    cfg.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values = {int(fvtHighFlow), int(fvtStandard)};
    cfg.option<ConfigOptionInts>("nozzle_temperature")->values = {190, 230, 200};
    cfg.option<ConfigOptionInts>("nozzle_temperature_initial_layer")->values = {195, 235, 205};
    cfg.option<ConfigOptionStrings>("filament_type")->values = {"PLA", "PETG"};
    return cfg;
}

} // namespace

TEST_CASE("Arrange print temps follow the selected flow variant per filament", "[ArrangeClearance][FilamentTabIndex]")
{
    Model model;
    ModelInstance *f0 = single_instance(model, its_make_cube(10., 10., 10.));
    const DynamicPrintConfig cfg = packed_std_hf_temp_config();

    const auto f0_poly = get_instance_arrange_poly(f0, cfg);
    REQUIRE(f0_poly.extrude_ids.front() == 1);
    REQUIRE(f0_poly.print_temp == 230);
    REQUIRE(f0_poly.first_print_temp == 235);
    REQUIRE(cfg.opt_int("nozzle_temperature", 0) == 190);

    Model model_f1;
    ModelObject *obj = model_f1.add_object();
    obj->name = "f1";
    obj->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    obj->config.set_key_value("extruder", new ConfigOptionInt(2));
    ModelInstance *f1 = obj->add_instance();
    const auto f1_poly = get_instance_arrange_poly(f1, cfg);
    REQUIRE(f1_poly.extrude_ids.front() == 2);
    REQUIRE(f1_poly.print_temp == 200);
    REQUIRE(f1_poly.first_print_temp == 205);
}

TEST_CASE("setExtruderParams reads the active flow variant nozzle temp", "[ArrangeClearance][FilamentTabIndex]")
{
    const DynamicPrintConfig cfg = packed_std_hf_temp_config();
    Model::setExtruderParams(cfg, 2);
    REQUIRE(Model::extruderParamsMap.at(1).heatEndTemp == 230);
    REQUIRE(Model::extruderParamsMap.at(2).heatEndTemp == 200);
    REQUIRE(cfg.opt_int("nozzle_temperature", 0) == 190);
}

TEST_CASE("Arrange bed and vitrify temps use the guarded filament id", "[ArrangeClearance][FilamentTabIndex]")
{
    DynamicPrintConfig cfg = packed_std_hf_temp_config();
    cfg.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(btPEI));
    cfg.option<ConfigOptionInts>("hot_plate_temp")->values = {50, 60};
    cfg.option<ConfigOptionInts>("hot_plate_temp_initial_layer")->values = {55, 65};
    cfg.option<ConfigOptionInts>("temperature_vitrification")->values = {40, 80};

    Model model;
    ModelInstance *f0 = single_instance(model, its_make_cube(10., 10., 10.));
    const auto f0_poly = get_instance_arrange_poly(f0, cfg);
    REQUIRE_FALSE(f0_poly.extrude_ids.empty());
    REQUIRE(f0_poly.bed_temp == 50);
    REQUIRE(f0_poly.first_bed_temp == 55);
    REQUIRE(f0_poly.vitrify_temp == 40);

    Model model_f1;
    ModelObject *obj = model_f1.add_object();
    obj->name = "f1";
    obj->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    obj->config.set_key_value("extruder", new ConfigOptionInt(2));
    ModelInstance *f1 = obj->add_instance();
    const auto f1_poly = get_instance_arrange_poly(f1, cfg);
    REQUIRE(f1_poly.extrude_ids.front() == 2);
    REQUIRE(f1_poly.bed_temp == 60);
    REQUIRE(f1_poly.first_bed_temp == 65);
    REQUIRE(f1_poly.vitrify_temp == 80);
}

TEST_CASE("Arrange and setExtruderParams stay on index 0 without packed variants", "[ArrangeClearance][FilamentTabIndex]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    cfg.option<ConfigOptionInts>("nozzle_temperature")->values = {210, 200};
    cfg.option<ConfigOptionInts>("nozzle_temperature_initial_layer")->values = {215, 205};
    cfg.option<ConfigOptionStrings>("filament_type")->values = {"PLA", "PETG"};

    Model model;
    ModelInstance *inst = single_instance(model, its_make_cube(10., 10., 10.));
    const auto poly = get_instance_arrange_poly(inst, cfg);
    REQUIRE(poly.print_temp == 210);
    REQUIRE(poly.first_print_temp == 215);

    Model::setExtruderParams(cfg, 2);
    REQUIRE(Model::extruderParamsMap.at(1).heatEndTemp == 210);
    REQUIRE(Model::extruderParamsMap.at(2).heatEndTemp == 200);
}
