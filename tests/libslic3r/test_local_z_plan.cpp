#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LocalZ/LocalZHeight.hpp"

#include <boost/filesystem.hpp>
#include <fstream>
#include <set>

using namespace Slic3r;
using Catch::Approx;

namespace {

struct LocalZTestSettings
{
    bool auto_generate = MixedFilamentManager::auto_generate_enabled();
    bool bbl_tags      = GCodeProcessor::s_IsBBLPrinter;
    LocalZTestSettings() { MixedFilamentManager::set_auto_generate_enabled(false); }
    ~LocalZTestSettings()
    {
        MixedFilamentManager::set_auto_generate_enabled(auto_generate);
        GCodeProcessor::s_IsBBLPrinter = bbl_tags;
    }
};

DynamicPrintConfig local_z_config(bool gradient = false, int percent_b = 37)
{
    auto config = DynamicPrintConfig::full_print_config();
    for (const auto& key : config.keys()) {
        if (config.option(key)->type() != coEnums)
            continue;
        auto* option = config.def()->get(key)->create_default_option();
        option->set(config.option(key));
        config.set_key_value(key, option);
    }
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.76, 1.77, 1.78};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    config.set("mixed_filament_definitions", gradient ? "1,2,1,1,50,0,g,w,m0,z2,xa0,xb0,d0,o0,u101,cm3,r1/0.8000/0.2000;"
                                                        "3,4,1,1,50,0,g,w,m0,z2,xa0,xb0,d0,o0,u102,cm3,r1/0.8000/0.2000" :
                                                        "1,2,1,1," + std::to_string(percent_b) + ",0,g,w,m2,z0,xa0,xb0,d0,o0,u101,cm0");
    config.set("dithering_local_z_mode", !gradient);
    config.set("dithering_local_z_whole_objects", false);
    config.set("dithering_local_z_infill", true);
    config.set("layer_height", 0.2);
    config.set("initial_layer_print_height", 0.2);
    config.set("mixed_filament_height_lower_bound", 0.04);
    config.set("enable_prime_tower", false);
    return config;
}

ModelObject* add_mixed_cube(Model& model, double height = 2.0)
{
    auto* object = model.add_object();
    object->add_volume(make_cube(10., 10., height));
    object->add_instance();
    object->config.set("extruder", 5);
    return object;
}

std::set<size_t> planned_tools(const PrintObject& object)
{
    std::set<size_t> tools;
    for (const auto& plan : object.local_z_sublayer_plan())
        for (size_t i = 0; i < plan.painted_masks_by_extruder.size(); ++i)
            if (!plan.painted_masks_by_extruder[i].empty())
                tools.insert(i);
    return tools;
}

struct ExportFile
{
    boost::filesystem::path path = boost::filesystem::temp_directory_path() /
                                   boost::filesystem::unique_path("orca-local-z-%%%%-%%%%.gcode");
    ~ExportFile()
    {
        boost::system::error_code error;
        boost::filesystem::remove(path, error);
    }
};

} // namespace

TEST_CASE("Local-Z raft plans use actual layer IDs and preserve object Z", "[LocalZ][LocalZPlan]")
{
    LocalZTestSettings settings;
    Model              model;
    add_mixed_cube(model);
    auto config = local_z_config();
    config.set("raft_layers", 3);
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    auto* object = print.get_object(0);
    object->slice();
    REQUIRE(object->layers().front()->id() == 3);
    REQUIRE(object->local_z_intervals().size() == object->layer_count());
    for (const Layer* layer : object->layers()) {
        const auto* interval = object->local_z_interval_for_layer(size_t(layer->id()));
        REQUIRE(interval != nullptr);
        CHECK(interval->z_hi == Approx(layer->print_z));
        for (size_t i = 0; i < interval->sublayer_count; ++i) {
            const auto& plan = object->local_z_sublayer_plan()[interval->first_sublayer_idx + i];
            CHECK(plan.layer_id == size_t(layer->id()));
            CHECK(plan.print_z <= layer->print_z + 1e-6);
            CHECK(plan.print_z > layer->print_z - layer->height - 1e-6);
        }
    }
}

TEST_CASE("Local-Z cached edits match a freshly sliced print", "[LocalZ][LocalZPlan][Invalidation]")
{
    LocalZTestSettings settings;
    Model              model;
    auto*              object      = add_mixed_cube(model);
    bool               gradient    = true;
    bool               assign_part = false;
    std::string        key         = "extruder";
    SECTION("Automatic gradient object assignment") {}
    SECTION("Automatic gradient part assignment") { assign_part = true; }
    SECTION("Static mix assignment with implicit full domain") { gradient = false; }
    SECTION("Sparse infill edit")
    {
        gradient = false;
        key      = "sparse_infill_filament";
    }
    SECTION("Solid infill edit")
    {
        gradient = false;
        key      = "solid_infill_filament";
    }
    auto  config = local_z_config(gradient);
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    config = print.full_print_config();
    print.process();
    REQUIRE_FALSE(print.get_object(0)->local_z_sublayer_plan().empty());

    ModelConfig& assignment = assign_part ? object->volumes.front()->config : object->config;
    const int    target     = key == "extruder" ? (gradient ? 6 : 1) : 2;
    for (int value : {target, key == "extruder" ? 5 : 1}) {
        assignment.set(key, value);
        print.apply(model, config);
        print.process();
        Print fresh;
        fresh.set_status_silent();
        fresh.apply(model, config);
        fresh.process();
        CHECK(planned_tools(*print.get_object(0)) == planned_tools(*fresh.get_object(0)));
        CHECK(print.get_object(0)->local_z_sublayer_plan().size() == fresh.get_object(0)->local_z_sublayer_plan().size());
        const auto& actual   = print.get_object(0)->local_z_sublayer_plan();
        const auto& expected = fresh.get_object(0)->local_z_sublayer_plan();
        REQUIRE(actual.size() == expected.size());
        for (size_t i = 0; i < actual.size(); ++i) {
            CHECK(actual[i].print_z == Approx(expected[i].print_z));
            CHECK(actual[i].flow_height == Approx(expected[i].flow_height));
            CHECK(actual[i].painted_masks_by_extruder == expected[i].painted_masks_by_extruder);
        }
    }
}

TEST_CASE("Local-Z independent intervals exclude purge overrides even with no pass", "[LocalZ][LocalZPlan][WipeTower]")
{
    LocalZTestSettings settings;
    Model              model;
    add_mixed_cube(model);
    auto* other = model.add_object();
    other->add_volume(make_cube(10., 10., 2.));
    other->add_instance()->set_offset(Vec3d(30., 0., 0.));
    other->config.set("extruder", 2);
    auto config = local_z_config(false, 10);
    config.set("mixed_filament_height_lower_bound", 0.06);
    config.set("flush_into_objects", true);
    config.set("enable_prime_tower", true);
    config.set("single_extruder_multi_material", true);
    config.set("purge_in_prime_tower", true);
    config.set("wipe_tower_no_sparse_layers", false);
    config.set("prime_volume", 15.);
    config.set("machine_start_gcode", "G90\nM83\n");
    config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.assign(16, 100.);
    Print print;
    print.is_BBL_printer() = false;
    print.set_status_silent();
    print.apply(model, config);
    print.process();
    ToolOrdering ordering(print, 0);
    size_t       zero_passes = 0, single_passes = 0;
    for (const auto& interval : print.get_object(0)->local_z_intervals()) {
        if (!interval.independent_layer_height)
            continue;
        zero_passes += interval.sublayer_count == 0;
        single_passes += interval.sublayer_count == 1;
        auto& tools = ordering.tools_for_layer(interval.z_hi);
        CHECK(tools.has_local_z_subdivision);
        CHECK(tools.wiping_extrusions().mark_wiping_extrusions(print, 0, 1, 1.f) == Approx(1.f));
        CHECK_FALSE(tools.wiping_extrusions().is_anything_overridden());
        auto& planned_tools = const_cast<LayerTools&>(print.wipe_tower_data().tool_ordering.tools_for_layer(interval.z_hi));
        CHECK_FALSE(planned_tools.wiping_extrusions().is_anything_overridden());
    }
    CHECK(zero_passes > 0);
    CHECK(single_passes > 0);
    ExportFile           output;
    GCodeProcessorResult result;
    print.export_gcode(output.path.string(), &result, nullptr);
    std::ifstream     stream(output.path.string());
    const std::string gcode((std::istreambuf_iterator<char>(stream)), {});
    CHECK(gcode.find("phase-b disabled") == std::string::npos);
    CHECK(gcode.find("local-z phase-b path passes begin") != std::string::npos);
}

TEST_CASE("Local-Z whole-object masks follow compensated outer walls", "[LocalZ][LocalZPlan][GCode]")
{
    LocalZTestSettings settings;
    double             compensation = 0.0;
    SECTION("Uncompensated") {}
    SECTION("Expanded") { compensation = 0.6; }
    SECTION("Contracted") { compensation = -0.6; }
    Model model;
    add_mixed_cube(model);
    auto config = local_z_config();
    config.set("xy_contour_compensation", compensation);
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    print.process();
    ExportFile           output;
    GCodeProcessorResult result;
    print.export_gcode(output.path.string(), &result, nullptr);
    size_t local_walls = 0;
    for (const auto& move : result.moves) {
        if (move.type != EMoveType::Extrude || move.extrusion_role != erExternalPerimeter || move.position.z() <= 0.21f)
            continue;
        ++local_walls;
        CHECK(std::abs(move.height - 0.2f) > 1e-4);
    }
    CHECK(local_walls > 0);
}

TEST_CASE("Local-Z terminal passes meet height limits and cover the entire mixed domain", "[LocalZ][LocalZPlan][Ratios]")
{
    LocalZTestSettings settings;
    bool               gradient = false;
    SECTION("Static cadence") {}
    SECTION("Automatic gradient") { gradient = true; }
    for (double object_height : {0.28, 0.4, 1.2, 2.0, 2.13}) {
        for (int percent : {10, 37, 50, 63, 90}) {
            CAPTURE(object_height, percent, gradient);
            Model model;
            add_mixed_cube(model, object_height);
            auto  config = local_z_config(gradient, percent);
            Print print;
            print.set_status_silent();
            print.apply(model, config);
            auto* object = print.get_object(0);
            object->slice();
            double cursor = object->layers().front()->print_z;
            size_t passes = 0;
            for (const auto& plan : object->local_z_sublayer_plan()) {
                if (!plan.split_interval)
                    continue;
                CHECK(plan.flow_height >= 0.04 - 1e-6);
                CHECK(plan.flow_height <= 0.3 + 1e-6);
                CHECK(plan.z_lo == Approx(cursor).margin(1e-6));
                CHECK(plan.z_hi - plan.z_lo == Approx(plan.flow_height).margin(1e-6));
                cursor = plan.z_hi;
                ++passes;
            }
            if (object->layer_count() > 1)
                CHECK(passes > 0);
            CHECK(cursor == Approx(object->layers().back()->print_z).margin(1e-6));
        }
    }
}

TEST_CASE("Local-Z terminal height fitting reserves or absorbs short remainders", "[LocalZ][LocalZPlan]")
{
    CHECK(local_z_bounded_pass_height(0.068108, 0.070270, 0.04, 0.28) == Approx(0.070270));
    CHECK(local_z_bounded_pass_height(0.1, 0.12, 0.04, 0.28) == Approx(0.08));
    CHECK(local_z_bounded_pass_height(0.28, 0.30, 0.04, 0.28) == Approx(0.26));
    CHECK(local_z_bounded_pass_height(0.06, 0.08, 0.06, 0.28) == Approx(0.08));
    CHECK(local_z_bounded_pass_height(0.04, 0.02, 0.04, 0.28) == 0.0);
    CHECK(local_z_bounded_pass_height(0.04, 0.07, 0.04, 0.05) == 0.0);
}
