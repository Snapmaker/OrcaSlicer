#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Print.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LocalZ/LocalZHeight.hpp"
#include "libslic3r/LocalZ/LocalZPassOrder.hpp"
#include "libslic3r/Slicing.hpp"

#include <boost/filesystem.hpp>
#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <set>
#include <tuple>

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
    config.option<ConfigOptionFloats>("min_layer_height")->values.assign(4, 0.04);
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

TriangleMesh cube_with_split_sides(float side, float zone_top, float height)
{
    indexed_triangle_set its;
    for (const float z : {0.f, zone_top, height}) {
        its.vertices.emplace_back(0.f, 0.f, z);
        its.vertices.emplace_back(side, 0.f, z);
        its.vertices.emplace_back(side, side, z);
        its.vertices.emplace_back(0.f, side, z);
    }
    for (const int ring : {0, 4})
        for (int i = 0; i < 4; ++i) {
            const int j = (i + 1) % 4;
            its.indices.emplace_back(ring + i, ring + j, ring + 4 + j);
            its.indices.emplace_back(ring + i, ring + 4 + j, ring + 4 + i);
        }
    its.indices.emplace_back(0, 2, 1);
    its.indices.emplace_back(0, 3, 2);
    its.indices.emplace_back(8, 9, 10);
    its.indices.emplace_back(8, 10, 11);
    return TriangleMesh(std::move(its));
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

TEST_CASE("Local-Z pass ordering handles nearly equal heights in every input order", "[LocalZ][LocalZPlan]")
{
    std::array<SubLayerPlan, 3> passes;
    const std::array<double, 3> heights{0.406410256, 0.406341463, 0.406250000}; // Layer 3 of the three-mix project.
    for (size_t i = 0; i < passes.size(); ++i) {
        passes[i].print_z          = 0.5;
        passes[i].dependency_group = i;
        passes[i].pass_index       = i;
    }
    std::array<size_t, 3> independent_order{2, 1, 0};
    std::array<size_t, 3> isolated_order{2, 1, 0};

    SECTION("Distinct print heights take priority over all tie breakers")
    {
        for (size_t i = 0; i < passes.size(); ++i)
            passes[i].print_z = heights[i];
    }
    SECTION("Equal print heights use dependency keys or the exact start height")
    {
        for (size_t i = 0; i < passes.size(); ++i)
            passes[i].z_lo = heights[i];
        passes[0].dependency_group = passes[2].dependency_group;
        passes[0].dependency_order = 1;
        independent_order          = {1, 2, 0};
    }
    SECTION("Equal print and start heights use the original pass index")
    {
        passes[0].pass_index = 3;
        independent_order    = {0, 1, 2};
        isolated_order       = {1, 2, 0};
    }

    std::array<size_t, 3> input{0, 1, 2};
    do {
        CAPTURE(input);
        auto independent = input;
        auto isolated    = input;
        std::sort(independent.begin(), independent.end(),
                  [&](size_t a, size_t b) { return independent_local_z_pass_less(passes[a], passes[b]); });
        std::sort(isolated.begin(), isolated.end(), [&](size_t a, size_t b) { return isolated_local_z_pass_less(passes[a], passes[b]); });
        CHECK(independent == independent_order);
        CHECK(isolated == isolated_order);
    } while (std::next_permutation(input.begin(), input.end()));
}

TEST_CASE("Local-Z publication rejects invalid interval references", "[LocalZ][LocalZPlan]")
{
    LocalZInterval interval;
    interval.layer_id       = 4;
    interval.sublayer_count = 1;
    SubLayerPlan pass;
    pass.layer_id = 4;
    LocalZPlan valid({interval}, {pass});
    REQUIRE(valid.interval_for_layer(4) != nullptr);
    CHECK(valid.interval_for_layer(0) == nullptr);
    CHECK_THROWS_AS(LocalZPlan({interval}, {}), std::invalid_argument);
    CHECK_THROWS_AS(LocalZPlan({interval, interval}, {pass}), std::invalid_argument);
    pass.layer_id = 0;
    CHECK_THROWS_AS(LocalZPlan({interval}, {pass}), std::invalid_argument);
    interval.independent_layer_height = true;
    interval.sublayer_count           = 0;
    CHECK_NOTHROW(LocalZPlan({interval}, {}));
}

TEST_CASE("Mixed preview retains the GUI recipe calculation", "[MixedFilament][Preview]")
{
    MixedFilamentPreviewSettings settings;
    settings.nozzle_min_layer_heights = {0.04};
    settings.nozzle_max_layer_heights = {0.16};
    settings.nominal_layer_height = 0.2;
    const auto passes             = mixed_filament_preview_pass_heights(settings, 25);
    REQUIRE(passes.size() == 2);
    CHECK(passes[0] == Approx(0.13));
    CHECK(passes[1] == Approx(0.07));
    settings.preferred_a_height = 0.15;
    settings.preferred_b_height = 0.05;
    const auto preferred        = mixed_filament_preview_pass_heights(settings, 25);
    REQUIRE(preferred.size() == 2);
    CHECK(preferred[0] == Approx(0.15));
    CHECK(preferred[1] == Approx(0.05));
    settings.nominal_layer_height = 0;
    CHECK(mixed_filament_preview_pass_heights(settings, 25).empty());
}

TEST_CASE("Mixed preview settings preserve override precedence", "[MixedFilament][Preview]")
{
    DynamicPrintConfig project, print;
    print.set_key_value("dithering_local_z_gradient_layer_height", new ConfigOptionFloat(0.24));
    print.set_key_value("layer_height", new ConfigOptionFloat(0.12));
    project.set_key_value("dithering_local_z_gradient_layer_height", new ConfigOptionFloat(0.30));
    project.set_key_value("dithering_local_z_mode", new ConfigOptionBool(true));
    auto settings = mixed_filament_preview_settings(project, &print);
    CHECK_THAT(settings.gradient_cycle_height, Catch::Matchers::WithinAbs(0.30, 1e-6));
    CHECK(settings.nominal_layer_height == Approx(0.12));
    CHECK(settings.local_z_independent_layer_height);
    project.set_key_value("mixed_color_layer_height_a", new ConfigOptionFloat(0.08));
    settings = mixed_filament_preview_settings(project, &print);
    CHECK_FALSE(settings.local_z_direct_multicolor);
    CHECK_THAT(mixed_filament_preview_settings(DynamicPrintConfig{}, nullptr).gradient_cycle_height, Catch::Matchers::WithinAbs(0.20, 1e-6));
}

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
    config.option<ConfigOptionFloats>("min_layer_height")->values.assign(4, 0.06);
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

TEST_CASE("Unequal mixed footprints close at their own tops in exported walls", "[LocalZ][Remediation][GCode]")
{
    LocalZTestSettings settings;
    bool               connected = false, paint = false, taper = false, fixed_takeover = false, mixed_takeover = false;
    SECTION("Separate islands, full object") {}
    SECTION("Connected step, full object") { connected = true; }
    SECTION("Separate islands, painted only") { paint = true; }
    SECTION("Mixed island ends under an ordinary material") { fixed_takeover = true; }
    SECTION("Mixed island ends under a different mixed recipe") { mixed_takeover = true; }
    SECTION("Connected step capped with a different mixed recipe") { connected = mixed_takeover = true; }
    SECTION("Tapered island, painted only") { paint = taper = true; }

    Model model;
    auto* obj        = model.add_object();
    auto  tall       = make_cube(10., 10., 2.);
    auto  short_mesh = taper ? make_cone(5., .8, PI / 12) : make_cube(10., 10., .4);
    short_mesh.translate(taper ? 25.f : connected ? 10.f : 20.f, taper ? 5.f : 0.f, 0.f);
    for (const auto& mesh : {tall, short_mesh}) {
        auto* volume = obj->add_volume(mesh);
        if (paint) {
            TriangleSelector selector(volume->mesh());
            for (size_t i = 0; i < mesh.its.indices.size(); ++i)
                selector.set_facet(int(i), static_cast<EnforcerBlockerType>(5));
            volume->mmu_segmentation_facets.set(selector);
        }
    }
    if (fixed_takeover || mixed_takeover) {
        auto cap = make_cube(10., 10., 1.6);
        cap.translate(connected ? 10.f : 20.f, 0.f, .4f);
        obj->add_volume(cap)->config.set("extruder", mixed_takeover ? 6 : 3);
    }
    obj->add_instance();
    obj->config.set("extruder", paint ? 3 : 5);
    auto config = local_z_config(false, connected ? 63 : 37);
    if (mixed_takeover)
        config.set("mixed_filament_definitions",
                   config.opt_string("mixed_filament_definitions") + ";3,4,1,1,45,0,g,w,m2,z0,xa0,xb0,d0,o0,u102,cm0");
    config.set("dithering_local_z_whole_objects", !paint);
    if (connected)
        config.option<ConfigOptionFloats>("min_layer_height")->values.assign(4, .05);
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    print.process();
    ExportFile           file;
    GCodeProcessorResult result;
    print.export_gcode(file.path.string(), &result, nullptr);
    std::map<float, float> tall_walls, short_walls;
    for (const auto& move : result.moves) {
        if (move.type != EMoveType::Extrude || move.extrusion_role != erExternalPerimeter)
            continue;
        if (move.position.x() < 5.f)
            tall_walls[move.position.z()] = move.height;
        if (move.position.x() > (connected ? 15.f : 20.f) && !((fixed_takeover || mixed_takeover) && move.extruder_id >= 2))
            short_walls[move.position.z()] = move.height;
    }
    auto check_stack = [](const std::map<float, float>& walls, double top) {
        REQUIRE_FALSE(walls.empty());
        double cursor = 0.;
        for (auto [z, height] : walls) {
            CAPTURE(z, height, top);
            CHECK(z - height == Approx(cursor).margin(.0011));
            cursor = z;
        }
        CHECK(cursor == Approx(top).margin(.0011));
    };
    check_stack(tall_walls, 2.);
    check_stack(short_walls, taper ? .8 : .4);
}

TEST_CASE("Separate painted patches on a connected wall close at their individual heights", "[LocalZ][Remediation][GCode]")
{
    LocalZTestSettings settings;
    Model              model;
    auto*              object = model.add_object();
    object->config.set("extruder", 3);
    auto*            volume = object->add_volume(cube_with_split_sides(10.f, .4f, 2.f));
    TriangleSelector selector(volume->mesh());
    // Paint the lower front face and the entire opposite face with the same mix.
    for (const int face : {0, 1, 4, 5, 12, 13})
        selector.set_facet(face, static_cast<EnforcerBlockerType>(5));
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    object->add_instance();
    auto config = local_z_config(false, 37);
    config.set("mmu_segmented_region_max_width", 1.);
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    print.process();
    ExportFile           file;
    GCodeProcessorResult result;
    print.export_gcode(file.path.string(), &result, nullptr);
    std::array<float, 2> tops{};
    for (size_t i = 1; i < result.moves.size(); ++i) {
        const auto& move = result.moves[i];
        if (move.type != EMoveType::Extrude || move.extrusion_role != erExternalPerimeter || move.extruder_id >= 2)
            continue;
        const auto& from = result.moves[i - 1].position;
        if (std::min(from.x(), move.position.x()) >= 5.f || std::max(from.x(), move.position.x()) <= 5.f)
            continue;
        const size_t side = move.position.y() > 5.f ? 1 : 0;
        tops[side]        = std::max(tops[side], move.position.z());
    }
    CHECK(tops[0] == Approx(.4).margin(.0011));
    CHECK(tops[1] > 1.5f); // The opposite patch continues above the short patch toward the unpainted top face.
}

TEST_CASE("A sloping paint boundary keeps the gradient cadence on the opposite wall", "[LocalZ][Remediation][Gradient][GCode]")
{
    LocalZTestSettings settings;
    auto               config = local_z_config(true);
    config.set("layer_height", .16);
    config.set("dithering_local_z_gradient_layer_height", .20);
    config.set("mmu_segmented_region_max_width", 1.);
    config.option<ConfigOptionFloats>("min_layer_height")->values.assign(4, .08);
    auto opposite_wall = [&](bool paint, bool taper) {
        Model model;
        auto* object = add_mixed_cube(model, 4.);
        auto* volume = object->volumes.front();
        if (taper) {
            auto mesh = volume->mesh().its;
            for (auto& point : mesh.vertices)
                if (point.y() == 0.f && point.z() > 0.f)
                    point.y() += 2.f;
            volume->set_mesh(std::move(mesh));
        }
        if (paint) {
            TriangleSelector selector(volume->mesh());
            selector.set_facet(6, static_cast<EnforcerBlockerType>(3)); // Upper triangle of the Y-min wall.
            volume->mmu_segmentation_facets.set(selector);
        }
        Print print;
        print.set_status_silent();
        print.apply(model, config);
        print.process();
        ExportFile           file;
        GCodeProcessorResult result;
        print.export_gcode(file.path.string(), &result, nullptr);
        std::map<float, std::pair<unsigned int, float>> passes;
        bool                                            found_paint = false;
        for (size_t i = 1; i < result.moves.size(); ++i) {
            const auto& move = result.moves[i];
            if (move.type != EMoveType::Extrude || move.extrusion_role != erExternalPerimeter)
                continue;
            found_paint |= move.extruder_id == 2;
            const auto& from = result.moves[i - 1].position;
            if (from.y() > 9.f && move.position.y() > 9.f && std::min(from.x(), move.position.x()) < 5.f &&
                std::max(from.x(), move.position.x()) > 5.f)
                passes[move.position.z()] = {move.extruder_id, move.height};
        }
        CHECK(found_paint == paint);
        return passes;
    };
    const auto unpainted = opposite_wall(false, false);
    REQUIRE(unpainted.size() > 30);
    CHECK(opposite_wall(true, false) == unpainted);
    CHECK(opposite_wall(true, true) == unpainted);
}

TEST_CASE("Transformed parts of a gradient share the sliced filament at the same height", "[LocalZ][Remediation][Gradient]")
{
    LocalZTestSettings settings;
    Model              model;
    auto*              obj        = add_mixed_cube(model, 2.);
    auto               short_mesh = make_cube(4., 4., 1.);
    short_mesh.translate(20.f, 0.f, .6f);
    obj->add_volume(short_mesh);
    Print print;
    print.set_status_silent();
    print.apply(model, local_z_config(true));
    auto* printed = print.get_object(0);
    printed->slice();
    const auto tall          = Point::new_scale(5., 5.) - printed->center_offset();
    const auto short_part    = Point::new_scale(22., 2.) - printed->center_offset();
    size_t     shared_passes = 0;
    for (const auto& pass : printed->local_z_sublayer_plan()) {
        if (!pass.split_interval || pass.z_lo < .8 || pass.z_hi > 1.4)
            continue;
        for (const auto& masks : pass.painted_masks_by_extruder) {
            auto covers = [&](const Point& p) {
                return std::any_of(masks.begin(), masks.end(), [&](const auto& poly) { return poly.contains(p); });
            };
            CHECK(covers(tall) == covers(short_part));
            shared_passes += covers(tall);
        }
    }
    CHECK(shared_passes > 1);
}

TEST_CASE("Local-Z terminal height fitting reserves or absorbs short remainders", "[LocalZ][LocalZPlan]")
{
    CHECK(local_z_bounded_pass_height(0.068108, 0.070270, 0.04, 0.28) == Approx(0.070270));
    CHECK(local_z_bounded_pass_height(0.1, 0.12, 0.04, 0.28) == Approx(0.08));
    CHECK(local_z_bounded_pass_height(0.28, 0.30, 0.04, 0.28) == Approx(0.26));
    CHECK(local_z_bounded_pass_height(0.06, 0.08, 0.06, 0.28) == Approx(0.08));
    CHECK(local_z_bounded_pass_height(0.04, 0.02, 0.04, 0.28) == 0.0);
    CHECK(local_z_bounded_pass_height(0.04, 0.07, 0.04, 0.05) == 0.0);
    CHECK(local_z_bounded_pass_height(0.06, 0.14, 0.04, 0.12, 0.10) == Approx(0.04));
    CHECK(local_z_bounded_pass_height(0.06, 0.08, 0.04, 0.12, 0.10) == Approx(0.08));
}

TEST_CASE("Local-Z uses participating nozzle limits", "[LocalZ][LocalZPlan][NozzleLimits]")
{
    LocalZTestSettings settings;
    bool               gradient = false;
    SECTION("Static mix") {}
    SECTION("Gradient") { gradient = true; }
    Model model;
    add_mixed_cube(model, 2.13);
    auto                      config = local_z_config(gradient, 70);
    const std::vector<double> minimums{0.04, 0.10, 0.25, 0.25};
    const std::vector<double> maximums{0.12, 0.28, 0.30, 0.30};
    config.option<ConfigOptionFloats>("min_layer_height")->values = minimums;
    config.option<ConfigOptionFloats>("max_layer_height")->values = maximums;
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    auto* object = print.get_object(0);
    object->slice();
    double           cursor            = object->layers().front()->print_z;
    bool             thin_first_nozzle = false;
    std::set<size_t> used;
    for (const auto& pass : object->local_z_sublayer_plan()) {
        if (!pass.split_interval)
            continue;
        CHECK(pass.z_lo == Approx(cursor).margin(1e-6));
        cursor = pass.z_hi;
        for (size_t tool = 0; tool < pass.painted_masks_by_extruder.size(); ++tool) {
            if (pass.painted_masks_by_extruder[tool].empty())
                continue;
            used.insert(tool);
            CHECK(pass.flow_height >= minimums[tool] - 1e-6);
            CHECK(pass.flow_height <= maximums[tool] + 1e-6);
            thin_first_nozzle |= tool == 0 && pass.flow_height < minimums[1] - 1e-6;
        }
    }
    CHECK(used == std::set<size_t>{0, 1});
    CHECK(thin_first_nozzle);
    CHECK(cursor == Approx(object->layers().back()->print_z).margin(1e-6));
}

TEST_CASE("Saved settings with the removed mixed filament height bounds still load", "[MixedFilament][Config]")
{
    for (const char* removed : {"mixed_filament_height_lower_bound", "mixed_filament_height_upper_bound"}) {
        CAPTURE(removed);
        CHECK_FALSE(DynamicPrintConfig::full_print_config().has(removed));
        t_config_option_key key   = removed;
        std::string         value = "0.1";
        PrintConfigDef::handle_legacy(key, value);
        CHECK(key.empty());
    }
}

TEST_CASE("Nozzle limit edits rebuild the cached Local-Z plan", "[LocalZ][LocalZPlan][NozzleLimits]")
{
    LocalZTestSettings settings;
    Model              model;
    add_mixed_cube(model);
    auto  config = local_z_config(false, 20);
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    config = print.full_print_config();
    print.get_object(0)->slice();
    const size_t original_count = print.get_object(0)->local_z_sublayer_plan().size();
    std::string  key            = "min_layer_height";
    double       limit          = 0.08;
    SECTION("Minimum") {}
    SECTION("Maximum")
    {
        key   = "max_layer_height";
        limit = 0.10;
    }
    config.option<ConfigOptionFloats>(key)->values.assign(4, limit);
    print.apply(model, config);
    print.get_object(0)->slice();
    Print fresh;
    fresh.set_status_silent();
    fresh.apply(model, config);
    fresh.get_object(0)->slice();
    const auto& actual   = print.get_object(0)->local_z_sublayer_plan();
    const auto& expected = fresh.get_object(0)->local_z_sublayer_plan();
    CHECK(actual.size() != original_count);
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        CHECK(actual[i].print_z == Approx(expected[i].print_z));
        CHECK(actual[i].flow_height == Approx(expected[i].flow_height));
        CHECK(actual[i].painted_masks_by_extruder == expected[i].painted_masks_by_extruder);
    }
}

TEST_CASE("Gradient samples respect each component minimum", "[MixedFilament][NozzleLimits]")
{
    MixedFilament row;
    row.component_a      = 1;
    row.component_b      = 2;
    row.gradient_enabled = true;
    const auto sample    = sample_mixed_gradient_local_z(row, 2, 0.1, 0.03, 0.20, {0.04, 0.10}, {0.3, 0.3});
    CHECK(sample.height_a == Approx(0.10));
    CHECK(sample.height_b == Approx(0.10));
    const auto reversed = sample_mixed_gradient_local_z(row, 2, 0.9, 0.03, 0.20, {0.04, 0.10}, {0.3, 0.3});
    CHECK(reversed.height_a == Approx(0.04));
    CHECK(reversed.height_b == Approx(0.16));
}

TEST_CASE("Local-Z shares the slicer's automatic nozzle limits", "[LocalZ][NozzleLimits]")
{
    auto config                                                   = local_z_config();
    config.option<ConfigOptionFloats>("min_layer_height")->values = {0.0, 0.04, 0.1, 0.1};
    config.option<ConfigOptionFloats>("max_layer_height")->values = {0.0, 0.28, 0.3, 0.3};
    Print print;
    Model model;
    add_mixed_cube(model);
    print.apply(model, config);
    CHECK(Slicing::min_layer_height_from_nozzle(print.config(), 1) == Approx(0.07));
    CHECK(Slicing::max_layer_height_from_nozzle(print.config(), 1) == Approx(0.30));
    CHECK(Slicing::min_layer_height_from_nozzle(print.config(), 2) == Approx(0.04));
    CHECK(Slicing::min_layer_height_from_nozzle(config, 1) == Approx(0.07));
}

TEST_CASE("Layer-aligned mixed previews use nozzle minima", "[MixedFilament][Preview][NozzleLimits]")
{
    MixedFilament row;
    row.component_a   = 1;
    row.component_b   = 2;
    row.mix_b_percent = 10;
    MixedFilamentPreviewSettings preview;
    preview.local_z_mode                     = true;
    preview.local_z_independent_layer_height = false;
    preview.nozzle_min_layer_heights         = {0.04, 0.10};
    CHECK(mixed_filament_effective_local_z_preview_mix_b_percent(row, preview) == 50);
    preview.nozzle_min_layer_heights = {0.04, 0.04};
    CHECK(mixed_filament_effective_local_z_preview_mix_b_percent(row, preview) == 20);
}

TEST_CASE("Local-Z prints a mixed region too short for the nozzle limits and warns", "[LocalZ][LocalZPlan][NozzleLimits]")
{
    LocalZTestSettings settings;
    Model              model;
    // Two layers: the first is never subdivided, so the mixed domain is one 0.2 mm layer,
    // which is below the 0.25 mm minimum of every nozzle.
    add_mixed_cube(model, 0.4);
    auto config = local_z_config(false, 37);
    config.option<ConfigOptionFloats>("min_layer_height")->values.assign(4, 0.25);
    config.option<ConfigOptionFloats>("max_layer_height")->values.assign(4, 0.30);
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    auto* object = print.get_object(0);
    REQUIRE_NOTHROW(object->slice());

    size_t mixed_passes = 0;
    for (const auto& pass : object->local_z_sublayer_plan()) {
        if (!pass.split_interval)
            continue;
        ++mixed_passes;
        CHECK_THAT(pass.flow_height, Catch::Matchers::WithinAbs(0.2, 1e-6));
        const auto tools = std::count_if(pass.painted_masks_by_extruder.begin(), pass.painted_masks_by_extruder.end(),
                                         [](const ExPolygons& masks) { return !masks.empty(); });
        CHECK(tools == 1);
    }
    CHECK(mixed_passes == 1);

    const auto warnings = object->step_state_with_warnings(posSlice).warnings;
    const bool warned   = std::any_of(warnings.begin(), warnings.end(), [](const PrintStateBase::Warning& warning) {
        return warning.message.find("nozzle layer height limits") != std::string::npos;
    });
    CHECK(warned);
}

TEST_CASE("A raft under a mixed-filament object is printed with a physical filament", "[LocalZ][GCode][ToolOrdering]")
{
    LocalZTestSettings settings;
    bool               whole_objects = false;
    SECTION("Painted zones only") {}
    SECTION("Whole objects") { whole_objects = true; }
    Model model;
    add_mixed_cube(model);
    auto config = local_z_config(false, 33);
    config.set("raft_layers", 2);
    config.set("dithering_local_z_whole_objects", whole_objects);
    const size_t num_physical = config.option<ConfigOptionStrings>("filament_colour")->values.size();
    Print        print;
    print.is_BBL_printer() = false;
    print.set_status_silent();
    print.apply(model, config);
    print.process();

    // The raft has no filament of its own. It must inherit a component of the mix, never the mixed filament itself.
    ToolOrdering ordering(print, (unsigned int) -1, false);
    size_t       raft_layers = 0;
    unsigned int highest_tool = 0;
    for (const LayerTools& layer_tools : ordering) {
        raft_layers += layer_tools.has_support && !layer_tools.has_object;
        for (const unsigned int tool : layer_tools.extruders)
            highest_tool = std::max(highest_tool, tool);
    }
    CHECK(raft_layers == 2);
    CHECK(highest_tool < num_physical);

    ExportFile           output;
    GCodeProcessorResult result;
    print.export_gcode(output.path.string(), &result, nullptr);
    unsigned int highest_printed_tool = 0;
    for (const auto& move : result.moves)
        if (move.type == EMoveType::Extrude)
            highest_printed_tool = std::max(highest_printed_tool, unsigned(move.extruder_id));
    CHECK(highest_printed_tool < num_physical);
}

enum class ZoneKind { SubdividedMix, WholeLayerMix, SolidColour };

// Slices a box that is printed with a whole-object gradient and has a colour zone painted on the lower part of
// one wall, then checks how the zone and the gradient that takes its place meet. The gradient resumes below the
// top of the nominal layer it takes over from, so it would be printed into the zone: the zone has to give way,
// while the gradient passes keep their height.
// stepped_edge: the zone also gets narrower with every layer, so the gradient covers a short strip of it on every
// layer. There the zone keeps its tip as it is, and the gradient pass that would be printed into the tip stops
// short of it, in a seam of its own.
static void check_local_z_zone_under_gradient(const ZoneKind zone_kind, const bool stepped_edge)
{
    LocalZTestSettings settings;
    const bool         subdivide_mixes = zone_kind == ZoneKind::SubdividedMix;

    // Whole-object gradient of filaments 1 and 2 (mixed filament 4).
    MixedFilamentDefinition gradient_definition;
    gradient_definition.identity.stable_id                  = 737377;
    gradient_definition.source.kind                         = MixedFilamentSourceKind::Custom;
    gradient_definition.recipe.kind                         = MixedFilamentRecipeKind::WeightedBlend;
    gradient_definition.recipe.blend.components             = {{{1}, 50}, {{2}, 50}};
    gradient_definition.behavior.distribution               = MixedFilamentDistributionMode::Simple;
    gradient_definition.behavior.gradient.enabled           = true;
    gradient_definition.behavior.gradient.component_a_start = 0.75f;
    gradient_definition.behavior.gradient.component_a_end   = 0.25f;
    gradient_definition.presentation.display_color          = "#808000";

    // Painted mix: a fixed 69/31 mix of filaments 3 and 2 (mixed filament 5).
    MixedFilamentDefinition zone_definition;
    zone_definition.identity.stable_id         = 737378;
    zone_definition.source.kind                = MixedFilamentSourceKind::Custom;
    zone_definition.recipe.kind                = MixedFilamentRecipeKind::WeightedBlend;
    zone_definition.recipe.blend.components    = {{{3}, 69}, {{2}, 31}};
    zone_definition.behavior.distribution      = MixedFilamentDistributionMode::Simple;
    zone_definition.presentation.display_color = "#4080C0";

    MixedFilamentManager           definitions;
    const std::vector<std::string> colors{"#FF0000", "#00FF00", "#0000FF"};
    REQUIRE(definitions.add_custom_filament_definition(gradient_definition, colors));
    REQUIRE(definitions.add_custom_filament_definition(zone_definition, colors));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    for (const std::string& key : config.keys()) {
        const ConfigOption* option = config.option(key);
        if (option->type() != coEnums)
            continue;
        ConfigOption* replacement = print_config_def.get(key)->create_default_option();
        replacement->set(option);
        config.set_key_value(key, replacement);
    }
    const double min_sublayer_height = 0.06;
    config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.75, 1.75}));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4, 0.4}));
    config.set_key_value("max_layer_height", new ConfigOptionFloats({0.32, 0.32, 0.32}));
    config.set_key_value("line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("initial_layer_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("inner_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("sparse_infill_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("internal_solid_infill_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("top_surface_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("wall_filament", new ConfigOptionInt(4));
    // A 0.30 mm gradient cycle over 0.24 mm layers: the gradient regularly resumes below a layer top.
    config.set_key_value("layer_height", new ConfigOptionFloat(0.24));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.24));
    config.set_key_value("sparse_infill_density", new ConfigOptionPercent(0.0));
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(definitions.serialize_custom_entries()));
    config.option<ConfigOptionFloats>("min_layer_height")->values.assign(3, min_sublayer_height);
    config.set_key_value("mixed_filament_region_collapse", new ConfigOptionBool(true));
    // A painted zone reaches only 1 mm into the object, as thin as it is on a thin-walled model: how long a
    // piece of it is then shows in the length of the wall along the outline alone.
    config.set_key_value("mmu_segmented_region_max_width", new ConfigOptionFloat(1.0));
    config.set_key_value("dithering_local_z_gradient_layer_height", new ConfigOptionFloat(0.30));
    // Either subdivide the painted mix as well, with its own cadence that is independent of the layer
    // height, or leave it to alternate its filaments by nominal layer.
    config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(subdivide_mixes));
    config.set_key_value("dithering_local_z_direct_multicolor", new ConfigOptionBool(subdivide_mixes));
    config.set_key_value("dithering_local_z_independent_layer_height", new ConfigOptionBool(subdivide_mixes));
    config.set_key_value("dithering_local_z_whole_objects", new ConfigOptionBool(false));
    config.set_key_value("dithering_local_z_preserve_first_layer", new ConfigOptionBool(false));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));

    // A 20 x 20 x 6 mm box whose side walls are split at z = 3.1 mm, so that the lower part of the
    // Y-min wall can be painted with a colour zone that stops below the top of the object. The zone's
    // last nominal layer ends at z = 3.12 while the gradient around it has only reached z = 3.00.
    const float  side     = 20.f;
    const float  zone_top = 3.1f;
    const float  height   = 6.f;
    Model        model;
    ModelObject* model_object = model.add_object();
    ModelVolume* model_volume = model_object->add_volume(cube_with_split_sides(side, zone_top, height));
    {
        TriangleSelector          selector(model_volume->mesh());
        const EnforcerBlockerType zone_paint = zone_kind == ZoneKind::SolidColour ? EnforcerBlockerType::Extruder3 :
                                                                                    EnforcerBlockerType::Extruder5;
        // Facets 0 and 1 are the lower part of the Y-min wall. Facet 0 alone is a triangle that gets
        // narrower with height, by about 1.5 mm per layer.
        selector.set_facet(0, zone_paint);
        if (!stepped_edge)
            selector.set_facet(1, zone_paint);
        REQUIRE(model_volume->mmu_segmentation_facets.set(selector));
    }
    model_object->add_instance();
    model_object->ensure_on_bed();

    Print print;
    print.is_BBL_printer() = false;
    print.auto_assign_extruders(model_object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    REQUIRE(print.objects().size() == 1);
    const PrintObject&               print_object = *print.objects().front();
    const std::vector<SubLayerPlan>& plans        = print_object.local_z_sublayer_plan();

    // The zone is everything that is not printed with the gradient (mixed filament 4).
    const int  gradient_filament = 4;
    const auto zone_slices       = [&print_object, gradient_filament](const size_t layer_id) {
        ExPolygons slices;
        for (const LayerRegion* layerm : print_object.get_layer(int(layer_id))->regions())
            if (layerm->region().config().wall_filament.value != gradient_filament)
                append(slices, to_expolygons(layerm->slices.surfaces));
        return slices;
    };

    // It ends below the top of the object.
    size_t zone_top_layer = 0;
    for (size_t layer_id = 0; layer_id < print_object.layer_count(); ++layer_id)
        if (!zone_slices(layer_id).empty())
            zone_top_layer = layer_id;
    REQUIRE(zone_top_layer > 0);
    REQUIRE(zone_top_layer + 1 < print_object.layer_count());

    // The gradient's independent passes carry dependency_group == its mixed row index + 1. The gradient is left
    // alone: none of its passes is split or changes height where it takes over from the zone.
    const size_t gradient_group = 1;
    const auto   pass_masks     = [](const SubLayerPlan& plan) -> const ExPolygons* {
        for (const ExPolygons& masks : plan.painted_masks_by_extruder)
            if (!masks.empty())
                return &masks;
        return nullptr;
    };
    const auto is_gradient_pass = [&pass_masks, gradient_group](const SubLayerPlan& plan) {
        return plan.split_interval && plan.dependency_group == gradient_group && pass_masks(plan) != nullptr;
    };
    std::set<std::pair<size_t, size_t>> gradient_pass_ids;
    for (const SubLayerPlan& plan : plans)
        if (is_gradient_pass(plan))
            CHECK(gradient_pass_ids.emplace(plan.layer_id, plan.dependency_order).second);
    REQUIRE_FALSE(gradient_pass_ids.empty());

    ExportFile           output;
    GCodeProcessorResult processor_result;
    print.export_gcode(output.path.string(), &processor_result, nullptr);

    // No wall is printed into another wall. Every wall bead fills the Z range [z - height, z] along its
    // centreline; two beads of different print planes that share a point overlap when those ranges do.
    // Where a wall is handed over from one pass to another the two parts meet end to end, which shows
    // up as a few points at most. The top of a zone is cut down to the gradient, and at the step of a zone
    // edge the gradient pass stops short of the tip, so nothing overlaps in either case.
    const float                                                         cell_mm = 0.05f;
    std::map<std::pair<int, int>, std::vector<std::pair<float, float>>> beads_by_cell;
    {
        Vec3f previous_position = Vec3f::Zero();
        for (const GCodeProcessorResult::MoveVertex& move : processor_result.moves) {
            const Vec3f from  = previous_position;
            previous_position = move.position;
            if (move.type != EMoveType::Extrude || !is_perimeter(move.extrusion_role) || move.height <= 0.f)
                continue;
            const int steps = std::max(1, int((move.position - from).head<2>().norm() / cell_mm));
            for (int step = 0; step <= steps; ++step) {
                const Vec3f pos = from + (move.position - from) * (float(step) / float(steps));
                beads_by_cell[{int(std::lround(pos.x() / cell_mm)), int(std::lround(pos.y() / cell_mm))}].emplace_back(move.position.z(),
                                                                                                                       move.position.z() -
                                                                                                                           move.height);
            }
        }
    }
    size_t overlapped_cells = 0;
    for (auto& cell_beads : beads_by_cell) {
        std::vector<std::pair<float, float>>& beads = cell_beads.second;
        std::sort(beads.begin(), beads.end());
        bool overlapped = false;
        for (size_t upper = 1; upper < beads.size() && !overlapped; ++upper)
            for (size_t lower = upper; lower-- > 0;) {
                if (beads[upper].first - beads[lower].first < 1e-4f)
                    continue;
                // Sorted by top: once a bead ends below this one's bottom, the ones before it do too.
                overlapped = beads[lower].first - beads[upper].second > 0.01f;
                break;
            }
        if (overlapped)
            ++overlapped_cells;
    }
    const double overlapped_wall_mm = double(overlapped_cells) * double(cell_mm);
    CHECK(overlapped_wall_mm < 1.5);

    if (stepped_edge) {
        // The zone does get narrower layer by layer, so the check above covers a stepped edge.
        REQUIRE(zone_top_layer >= 6);
        const double low_area  = area(union_ex(zone_slices(2)));
        const double high_area = area(union_ex(zone_slices(zone_top_layer - 2)));
        CHECK(high_area < 0.5 * low_area);

        // The zone ends in a point. Where it ends, what is left of it is cut down like any zone top. The layers
        // below are the stepped edge.
        const size_t last_stepped_layer = zone_top_layer - 2;
        const double last_stepped_z     = print_object.get_layer(int(last_stepped_layer))->print_z;

        // No strip is printed as a pass of its own: every pass is planned once, without a shorter copy.
        std::set<std::tuple<size_t, size_t, size_t>> pass_ids;
        for (const SubLayerPlan& plan : plans)
            if (plan.split_interval && plan.layer_id <= last_stepped_layer && pass_masks(plan) != nullptr)
                CHECK(pass_ids.emplace(plan.layer_id, plan.dependency_group, plan.dependency_order).second);

        // The gradient pass that resumes below the top of a tip is planned without the tip, and the layer has a
        // second set of walls for it, generated for the gradient's region without the tip.
        const Point shift            = print_object.instances().front().shift;
        size_t      held_back_passes = 0;
        size_t      passes_with_seam = 0;
        for (const SubLayerPlan& plan : plans) {
            if (!is_gradient_pass(plan) || plan.held_back_masks.empty() || plan.layer_id == 0 || plan.layer_id > last_stepped_layer)
                continue;
            ++held_back_passes;
            CHECK(intersection_ex(*pass_masks(plan), offset_ex(plan.held_back_masks, -float(scale_(0.02)))).empty());
            bool has_alt_walls = false;
            for (const LayerRegion* layerm : print_object.get_layer(int(plan.layer_id))->regions())
                has_alt_walls = has_alt_walls || (layerm->local_z_alt_perimeters && !layerm->local_z_alt_perimeters->empty());
            CHECK(has_alt_walls);

            // The pass ends in a seam where it stops short: its wall turns into the object just before the edge
            // the zone has on the layer below, instead of being cut off on the outer wall line.
            const BoundingBox zone_below  = get_extents(zone_slices(plan.layer_id - 1));
            const double      zone_edge_x = unscale<double>(zone_below.min.x() + shift.x());
            bool              has_seam    = false;
            Vec3f             previous    = Vec3f::Zero();
            for (const GCodeProcessorResult::MoveVertex& move : processor_result.moves) {
                const Vec3f from = previous;
                previous         = move.position;
                if (move.type != EMoveType::Extrude || !is_perimeter(move.extrusion_role) ||
                    std::abs(double(move.position.z()) - plan.print_z) > 1e-3)
                    continue;
                const Vec3f delta = move.position - from;
                if (std::abs(delta.x()) < 0.02f && std::abs(delta.y()) > 0.3f && double(from.x()) > zone_edge_x - 0.5 &&
                    double(from.x()) < zone_edge_x + 0.05)
                    has_seam = true;
            }
            if (has_seam)
                ++passes_with_seam;
        }
        CHECK(held_back_passes >= 3);
        CHECK(passes_with_seam == held_back_passes);

        // No wall changes height on the way: every pass and every layer is printed at one height.
        size_t height_changes_within_a_wall = 0;
        for (size_t move_idx = 1; move_idx < processor_result.moves.size(); ++move_idx) {
            const GCodeProcessorResult::MoveVertex& previous = processor_result.moves[move_idx - 1];
            const GCodeProcessorResult::MoveVertex& move     = processor_result.moves[move_idx];
            if (previous.type == EMoveType::Extrude && move.type == EMoveType::Extrude && is_perimeter(previous.extrusion_role) &&
                is_perimeter(move.extrusion_role) && previous.extruder_id == move.extruder_id &&
                std::abs(previous.position.z() - move.position.z()) < 1e-4f && std::abs(previous.height - move.height) > 1e-3f)
                ++height_changes_within_a_wall;
        }
        CHECK(height_changes_within_a_wall == 0);

        // Filament 3 is only used by the zone. Where the zone is printed by nominal layers, each of its walls
        // is a whole layer high.
        if (zone_kind != ZoneKind::SubdividedMix) {
            const float layer_height       = float(print_object.config().layer_height.value);
            size_t      zone_walls         = 0;
            size_t      reduced_zone_walls = 0;
            for (const GCodeProcessorResult::MoveVertex& move : processor_result.moves)
                if (move.type == EMoveType::Extrude && is_perimeter(move.extrusion_role) && move.extruder_id == 2 &&
                    double(move.position.z()) <= last_stepped_z + 1e-3) {
                    ++zone_walls;
                    if (std::abs(move.height - layer_height) > 1e-3f)
                        ++reduced_zone_walls;
                }
            REQUIRE(zone_walls > 0);
            CHECK(reduced_zone_walls == 0);

            // Its walls are not clipped either: nothing of the zone's region is taken out of the ordinary layer.
            // Each wall is as the perimeter generator made it, and turns inwards where the zone ends. The zone
            // is exactly as it was painted.
            double clipped_zone_area = 0.;
            for (const LocalZInterval& interval : print_object.local_z_intervals())
                if (interval.layer_id <= last_stepped_layer && !interval.managed_masks.empty())
                    clipped_zone_area += area(intersection_ex(interval.managed_masks, zone_slices(interval.layer_id)));
            CHECK(unscale<double>(unscale<double>(clipped_zone_area)) < 0.05);
        }
        return;
    }

    // Footprint of the zone on its last layer, kept clear of its edges.
    const ExPolygons zone_core = offset_ex(union_ex(zone_slices(zone_top_layer)), -float(scale_(0.15)));
    REQUIRE_FALSE(zone_core.empty());

    // The gradient that takes the zone's place resumes inside the zone's last nominal layer: left
    // alone, the zone would be filled up to the layer top and the gradient printed into it.
    double gradient_resume_z = std::numeric_limits<double>::max();
    for (const SubLayerPlan& plan : plans)
        if (plan.layer_id > zone_top_layer && is_gradient_pass(plan)) {
            gradient_resume_z = std::min(gradient_resume_z, plan.print_z - plan.flow_height);
            CHECK(diff_ex(zone_core, *pass_masks(plan)).empty());
        }
    const Layer& zone_layer     = *print_object.get_layer(int(zone_top_layer));
    const double zone_layer_top = zone_layer.print_z;
    REQUIRE(gradient_resume_z < zone_layer_top - EPSILON);
    REQUIRE(gradient_resume_z > zone_layer_top - zone_layer.height + EPSILON);

    // In the G-code, look at the walls printed over the zone's last footprint: the zone's own, up to
    // the top of its last nominal layer, and the gradient's above that. Walls are long straight moves,
    // so look at points along each move rather than at its end.
    const Point shift     = print_object.instances().front().shift;
    const auto  over_zone = [&zone_core, &shift](const Vec3f& from, const Vec3f& to) {
        for (const float t : {0.1f, 0.3f, 0.5f, 0.7f, 0.9f}) {
            const Vec3f pos = from + (to - from) * t;
            const Point point(coord_t(scale_(pos.x())) - shift.x(), coord_t(scale_(pos.y())) - shift.y());
            if (std::any_of(zone_core.begin(), zone_core.end(), [&point](const ExPolygon& zone) { return zone.contains(point); }))
                return true;
        }
        return false;
    };
    size_t walls_of_zone            = 0;
    size_t walls_above_zone         = 0;
    double zone_printed_top         = std::numeric_limits<double>::lowest();
    int    zone_top_extruder        = -1;
    double lowest_bottom_above_zone = std::numeric_limits<double>::max();
    Vec3f  previous_position        = Vec3f::Zero();
    for (const GCodeProcessorResult::MoveVertex& move : processor_result.moves) {
        const Vec3f from  = previous_position;
        previous_position = move.position;
        if (move.type != EMoveType::Extrude || !is_perimeter(move.extrusion_role) || !over_zone(from, move.position))
            continue;
        if (move.position.z() > zone_layer_top + 1e-3) {
            ++walls_above_zone;
            lowest_bottom_above_zone = std::min(lowest_bottom_above_zone, double(move.position.z()) - double(move.height));
        } else {
            ++walls_of_zone;
            if (double(move.position.z()) > zone_printed_top + 1e-4) {
                zone_printed_top  = double(move.position.z());
                zone_top_extruder = int(move.extruder_id);
            }
        }
    }
    REQUIRE(walls_of_zone > 0);
    REQUIRE(walls_above_zone > 0);

    // The zone ends early, where the gradient resumes: nothing is printed into the zone, and no more
    // than a sliver that is not worth printing is missing between the two.
    CHECK(lowest_bottom_above_zone == Approx(gradient_resume_z).margin(1e-3));
    CHECK(zone_printed_top <= lowest_bottom_above_zone + 1e-3);
    CHECK(zone_printed_top >= lowest_bottom_above_zone - 0.5 * min_sublayer_height - 1e-3);

    // A zone printed by nominal layers keeps the filament of its last layer when that layer is cut down.
    if (zone_kind != ZoneKind::SubdividedMix) {
        int zone_layer_filament = 0;
        for (const LayerRegion* layerm : zone_layer.regions())
            if (layerm->region().config().wall_filament.value != gradient_filament && !layerm->slices.empty())
                zone_layer_filament = layerm->region().config().wall_filament.value;
        REQUIRE(zone_layer_filament >= 1);
        REQUIRE(zone_layer_filament <= 3);
        CHECK(zone_top_extruder == zone_layer_filament - 1);
    }
}

TEST_CASE("Local-Z ends a colour zone early under the gradient pass that takes its place", "[PrintObject][MixedFilament][LocalZ][Gradient]")
{
    SECTION("the zone is a mix subdivided into its own Local-Z passes")
    {
        check_local_z_zone_under_gradient(ZoneKind::SubdividedMix, false);
    }
    SECTION("the zone is a mix printed one filament per nominal layer")
    {
        check_local_z_zone_under_gradient(ZoneKind::WholeLayerMix, false);
    }
    SECTION("the zone is a solid colour") { check_local_z_zone_under_gradient(ZoneKind::SolidColour, false); }
}

TEST_CASE("Local-Z stops the gradient pass short of the step of a colour zone edge it would be printed into",
          "[PrintObject][MixedFilament][LocalZ][Gradient]")
{
    SECTION("the zone is a mix subdivided into its own Local-Z passes")
    {
        check_local_z_zone_under_gradient(ZoneKind::SubdividedMix, true);
    }
    SECTION("the zone is a mix printed one filament per nominal layer")
    {
        check_local_z_zone_under_gradient(ZoneKind::WholeLayerMix, true);
    }
    SECTION("the zone is a solid colour") { check_local_z_zone_under_gradient(ZoneKind::SolidColour, true); }
}
