#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/FullSpectrumKSPairResidual.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem.hpp>
#include <fstream>
#include <cmath>
#include <locale>
#include <regex>
#include <set>

using namespace Slic3r;
using Catch::Approx;

TEST_CASE("Tower skips must not suppress required object tool changes", "[ProductPort][LocalZ][GCode]")
{
    auto dynamic_config = DynamicPrintConfig::full_print_config();
    dynamic_config.set_num_extruders(3);
    dynamic_config.set_num_filaments(3);
    dynamic_config.set("enable_prime_tower", true);
    dynamic_config.set("wipe_tower_no_sparse_layers", true);
    dynamic_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.assign(9, 100.);
    PrintConfig config;
    config.apply(dynamic_config, true);

    const std::vector<WipeTower::ToolChangeResult>              priming;
    std::vector<std::vector<WipeTower::ToolChangeResult>>       nominal;
    const std::vector<std::vector<WipeTower::ToolChangeResult>> local_z;
    const std::vector<std::vector<WipeTower::box_coordinates>>  reserves;
    const WipeTower::ToolChangeResult                           final_purge{};
    SECTION("Above the last planned tower layer") {}
    SECTION("Sparse tower layers without a planned tool change")
    {
        WipeTower::ToolChangeResult sparse{};
        sparse.initial_tool = sparse.new_tool = 0;
        sparse.layer_height                   = .2f;
        nominal                               = {{sparse}, {sparse}};
    }
    WipeTowerIntegration tower(config, 0, Vec3d::Zero(), priming, nominal, local_z, reserves, final_purge, 0.f, BoundingBoxf{});
    tower.next_layer();
    tower.next_layer();
    tower.set_is_first_print(false);
    GCode generator;
    generator.apply_print_config(config);
    generator.writer().set_extruders({0, 1, 2});
    generator.writer().set_extruder(2); // Last mixed pass left yellow active.
    generator.writer().set_position(Vec3d(100., 100., 23.05));

    CHECK(tower.is_empty_wipe_tower_gcode(generator, 2, false));
    REQUIRE_FALSE(tower.is_empty_wipe_tower_gcode(generator, 0, false));
    // Once the requested filament is active, the same omitted tower layer
    // must not request another switch or emit extra G-code.
    generator.writer().set_extruder(0);
    CHECK(tower.is_empty_wipe_tower_gcode(generator, 0, false));
    CHECK(tower.tool_change(generator, 0, false).empty());
}

TEST_CASE("Product color library identifies the shipping bundle and its TD values", "[ProductPort][FullSpectrumKS]")
{
    struct RestoreResources
    {
        std::string path = resources_dir();
        ~RestoreResources()
        {
            set_resources_dir(path);
            FilamentColorLibrary::Instance().Reload();
        }
    } restore;
    set_resources_dir(TEST_RESOURCES_DIR);
    auto& library = FilamentColorLibrary::Instance();
    library.Reload();
    FilamentColorInfo bundle;
    REQUIRE(library.FindFilamentById("14170311270", bundle));
    for (const auto& [sku, td] :
         std::vector<std::pair<std::string, double>>{{"34267", 5.5}, {"34268", 5.5}, {"34269", 9.5}, {"34265", 6.5}}) {
        const auto found = std::find_if(bundle.colors.begin(), bundle.colors.end(), [&](const auto& item) { return item.sku == sku; });
        REQUIRE(found != bundle.colors.end());
        CHECK(found->tdValue == Approx(td));
        CHECK(found->fullSpectrumMaterialId == "snapmaker:" + sku);
        const std::string measured = sku == "34267" ? "#008BB3" : sku == "34268" ? "#AD4A76" : sku == "34269" ? "#EBBE00" : "#7B7F80";
        CHECK(found->colorData.PrimaryColor() == measured);
        CHECK_FALSE(found->legacyPrimaryColor.empty());
    }
}

TEST_CASE("Product painting projection tools select only their screen regions", "[ProductPort][TriangleSelector]")
{
    const Vec3f                                 camera(0.f, 0.f, 5.f), target(0.f, 0.f, 0.f);
    const std::array<int, 4>                    viewport{0, 0, 100, 100};
    const TriangleSelector::ClippingPlane       clipping;
    TriangleSelector::RectangleProjectionCursor rectangle(camera, target, Matrix4d::Identity(), viewport, Vec2f(25.f, 25.f),
                                                          Vec2f(75.f, 75.f), Transform3d::Identity(), clipping);
    CHECK(rectangle.is_mesh_point_inside(Vec3f(0.f, 0.f, 0.f)));
    CHECK_FALSE(rectangle.is_mesh_point_inside(Vec3f(.8f, 0.f, 0.f)));
    TriangleSelector::PolygonProjectionCursor polygon(camera, target, Matrix4d::Identity(), viewport,
                                                      {Vec2f(20.f, 80.f), Vec2f(50.f, 20.f), Vec2f(80.f, 80.f)}, Transform3d::Identity(),
                                                      clipping);
    CHECK(polygon.is_mesh_point_inside(Vec3f(0.f, 0.f, 0.f)));
    CHECK_FALSE(polygon.is_mesh_point_inside(Vec3f(.5f, .5f, 0.f)));
}

TEST_CASE("Product gradients preserve endpoints, midpoints and middle-color windows", "[ProductPort][Gradient]")
{
    MixedFilament entry;
    entry.gradient_enabled        = true;
    entry.gradient_component_ids  = "1234";
    entry.gradient_stop_positions = {0.f, .1f, .3f, .5f, .7f, .9f, 1.f};
    CHECK(sample_mixed_gradient(entry, 4, 0.0, .22).component_a == 1);
    CHECK(sample_mixed_gradient(entry, 4, 1.0, .22).component_a == 4);
    auto center = sample_mixed_gradient(entry, 4, .3, .22);
    CHECK(center.component_a == 2);
    CHECK(center.component_b == 2);
    auto midpoint = sample_mixed_gradient(entry, 4, .5, .22);
    CHECK(midpoint.component_a == 2);
    CHECK(midpoint.component_b == 3);
    CHECK(midpoint.mix_b_percent == 50);
    for (int i = 0; i <= 1000; ++i) {
        const auto sample = sample_mixed_gradient(entry, 4, i / 1000.0, .22);
        CHECK(sample.component_a >= 1);
        CHECK(sample.component_b <= 4);
        CHECK(sample.component_b - sample.component_a <= 1);
        CHECK(sample.mix_b_percent >= 0);
        CHECK(sample.mix_b_percent <= 100);
    }
    std::swap(entry.gradient_start, entry.gradient_end);
    CHECK(sample_mixed_gradient(entry, 4, 0.0, .22).component_a == 4);
    CHECK(sample_mixed_gradient(entry, 4, 1.0, .22).component_a == 1);
}

TEST_CASE("Product gradient ratio clamping preserves cycle height and pure endpoints", "[ProductPort][LocalZ]")
{
    auto heights = mixed_filament_local_z_pair_heights(.20, .06, 25);
    CHECK(heights.first == Approx(.14));
    CHECK(heights.second == Approx(.06));
    CHECK(mixed_filament_local_z_pair_heights(.20, .06, 0).first == Approx(.20));
    CHECK(mixed_filament_local_z_pair_heights(.20, .06, 100).second == Approx(.20));
    MixedFilament row;
    row.gradient_enabled = true;
    CHECK(mixed_filament_definition_uses_local_z(row, false));
    row.distribution_mode = int(MixedFilament::SameLayerPointillisme);
    CHECK_FALSE(mixed_filament_definition_uses_local_z(row, true));
    row.distribution_mode = int(MixedFilament::LayerCycle);
    row.enabled           = false;
    CHECK_FALSE(mixed_filament_definition_uses_local_z(row, true));
}

TEST_CASE("Product gradient stops survive project serialization", "[ProductPort][Gradient]")
{
    const std::vector<std::string> colors{"#00C3FF", "#F54399", "#FAE727", "#9199A4"};
    MixedFilamentManager           manager;
    manager.add_custom_filament(1, 2, 50, colors);
    auto& entry                     = manager.mixed_filaments().back();
    entry.gradient_enabled          = true;
    entry.gradient_start            = 1.f;
    entry.gradient_end              = 0.f;
    entry.gradient_component_ids    = "1234";
    entry.gradient_stop_positions   = {0.f, .15f, .35f, .5f, .7f, .85f, 1.f};
    entry.gradient_solid_widths     = {0.f, .08f, .16f, 0.f};
    const auto           serialized = manager.serialize_custom_entries();
    MixedFilamentManager restored;
    restored.load_custom_entries(serialized, colors);
    REQUIRE(restored.mixed_filaments().size() == manager.mixed_filaments().size());
    const auto& copy = restored.mixed_filaments().back();
    CHECK(copy.gradient_enabled);
    CHECK(copy.gradient_start == Approx(1.0));
    CHECK(copy.gradient_end == Approx(0.0));
    CHECK(copy.gradient_stop_positions == entry.gradient_stop_positions);
    CHECK(copy.gradient_solid_widths == entry.gradient_solid_widths);
    for (int i = 0; i <= 100; ++i) {
        const auto before = sample_mixed_gradient(entry, 4, i / 100.0, .22);
        const auto after  = sample_mixed_gradient(copy, 4, i / 100.0, .01);
        CHECK(before.component_a == after.component_a);
        CHECK(before.component_b == after.component_b);
        CHECK(before.mix_b_percent == after.mix_b_percent);
    }
}

TEST_CASE("Product bundle calibration follows IDs and TD through slot reordering", "[ProductPort][FullSpectrumKS]")
{
    MixedFilamentDisplayContext context;
    context.num_physical          = 4;
    context.physical_colors       = {"#00C3FF", "#F54399", "#FAE727", "#9199A4"};
    context.physical_tds          = {5.5, 5.5, 9.5, 6.5};
    context.physical_material_ids = {"snapmaker:34267", "snapmaker:34268", "snapmaker:34269", "snapmaker:34265"};
    context.color_engine          = MixedFilamentColorEngine::FullSpectrumKSPairResidual;
    const auto original           = blend_mixed_components({1, 2, 3, 4}, {15, 25, 35, 25}, context);
    std::reverse(context.physical_colors.begin(), context.physical_colors.end());
    std::reverse(context.physical_tds.begin(), context.physical_tds.end());
    std::reverse(context.physical_material_ids.begin(), context.physical_material_ids.end());
    CHECK(blend_mixed_components({4, 3, 2, 1}, {15, 25, 35, 25}, context) == original);
    context.color_engine = MixedFilamentColorEngine::FilamentMixer;
    CHECK(blend_mixed_components({4, 3, 2, 1}, {15, 25, 35, 25}, context) != original);

    // The slicing manager must receive the same per-slot calibration, even
    // when it regenerates a saved recipe without a GUI display context.
    MixedFilamentManager recipes;
    recipes.add_custom_filament(4, 3, 50, context.physical_colors);
    auto& recipe                      = recipes.mixed_filaments().back();
    recipe.distribution_mode          = int(MixedFilament::LayerCycle);
    recipe.gradient_component_ids     = "4321";
    recipe.gradient_component_weights = "15/25/35/25";
    auto config                       = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    config.option<ConfigOptionFloats>("filament_diameter")->values                   = {1.75, 1.75, 1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values                    = context.physical_colors;
    config.option<ConfigOptionFloats>("filament_transmission_distance")->values      = context.physical_tds;
    config.option<ConfigOptionStrings>("filament_full_spectrum_material_id")->values = context.physical_material_ids;
    config.set("dithering_local_z_mode", true);
    config.set("mixed_filament_calibrated_colors", false); // An old hidden toggle cannot disable automatic pack calibration.
    config.set("mixed_filament_definitions", recipes.serialize_custom_entries());
    Model model;
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    const auto& restored = print.mixed_filament_manager().mixed_filaments();
    const auto found = std::find_if(restored.begin(), restored.end(), [](const auto& row) { return row.gradient_component_ids == "4321"; });
    REQUIRE(found != restored.end());
    CHECK(found->display_color == original);
}

TEST_CASE("Product gradient and direct multicolor modes subdivide assigned objects", "[ProductPort][LocalZ][Slice]")
{
    bool gradient     = true;
    bool independent  = false;
    bool painted      = false;
    bool surface_only = true;
    SECTION("Gradients automatically use independent cadence") {}
    SECTION("Painted gradients use the surface controls") { painted = true; }
    SECTION("Hidden extra painted walls do not apply without surface-only painting")
    {
        painted      = true;
        surface_only = false;
    }
    SECTION("Static SML defaults to direct multicolor") { gradient = false; }
    SECTION("Static SML enables independent heights even with an old disabled setting")
    {
        gradient    = false;
        independent = true;
    }
    const std::vector<std::string> colors{"#00C3FF", "#F54399", "#FAE727", "#9199A4"};
    MixedFilamentManager           manager;
    manager.add_custom_filament(1, 2, 50, colors);
    auto& entry                      = manager.mixed_filaments().back();
    entry.gradient_enabled           = gradient;
    entry.distribution_mode          = int(MixedFilament::LayerCycle);
    entry.gradient_component_weights = independent ? "20/20/60" : "33/33/34";
    entry.gradient_component_ids     = "123";
    entry.gradient_stop_positions    = {0.f, .25f, .5f, .75f, 1.f};
    entry.gradient_solid_widths      = {0.f, .40f, 0.f};
    DynamicPrintConfig config        = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    config.set("layer_height", gradient || independent ? .08 : .20);
    REQUIRE(config.opt_bool("dithering_local_z_direct_multicolor"));
    REQUIRE(config.opt_bool("dithering_local_z_independent_layer_height"));
    if (independent)
        config.set("dithering_local_z_independent_layer_height", false);
    config.set("initial_layer_print_height", .20);
    REQUIRE(config.opt_bool("dithering_local_z_preserve_first_layer"));
    // Legacy projects cannot disable the now-automatic first-layer protection.
    config.set("dithering_local_z_preserve_first_layer", false);
    config.set("dithering_local_z_mode", !gradient);
    config.set("dithering_local_z_whole_objects", false);
    config.set("dithering_local_z_gradient_layer_height", .20);
    config.option<ConfigOptionPercent>("dithering_local_z_gradient_middle_filament_window")->value = 0;

    config.set("mixed_filament_height_lower_bound", .06);
    config.set("fs_surface_paint_only", painted && surface_only);
    config.set("fs_painted_zone_extra_perimeters", painted ? 1 : 0);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = colors;
    // Build enum vectors through the definition factory, as preset loading does.
    // Static defaults do not carry the string-to-enum maps needed for G-code serialization.
    for (const auto& key : config.keys()) {
        if (config.option(key)->type() != coEnums)
            continue;
        auto* initialized = config.def()->get(key)->create_default_option();
        initialized->set(config.option(key));
        config.set_key_value(key, initialized);
    }
    Model model;
    auto* object = model.add_object();
    object->name = "product-gradient-test";
    auto* volume = object->add_volume(make_cube(8., 8., 3.));
    object->config.set("extruder", painted ? 4 : 5);
    if (painted) {
        TriangleSelector selector(volume->mesh());
        for (size_t facet = 0; facet < volume->mesh().its.indices.size(); ++facet)
            selector.set_facet(int(facet), static_cast<EnforcerBlockerType>(5));
        volume->mmu_segmentation_facets.set(selector);
    }
    object->add_instance();
    object->ensure_on_bed();
    auto* ordinary = model.add_object();
    ordinary->name = "unassigned-control";
    ordinary->add_volume(make_cube(8., 8., 3.));
    ordinary->config.set("extruder", 4);
    ordinary->add_instance()->set_offset(Vec3d(20., 0., 0.));
    ordinary->ensure_on_bed();
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 2);
    CHECK(print.config().dithering_local_z_preserve_first_layer.value);
    CHECK(print.full_print_config().opt_bool("dithering_local_z_preserve_first_layer"));
    auto* control = print.get_object(size_t(1));
    control->slice();
    CHECK(control->local_z_sublayer_plan().empty());
    auto* printed = print.get_object(size_t(0));
    if (painted) {
        int most_walls = 0;
        for (const auto& region : printed->all_regions())
            most_walls = std::max(most_walls, region.get().config().wall_loops.value);
        CHECK(most_walls == config.opt_int("wall_loops") + (surface_only ? 1 : 0));
    }
    printed->slice();
    REQUIRE(printed->layer_count() > 5);
    CHECK(printed->layers().front()->height == Approx(.20));
    REQUIRE_FALSE(printed->local_z_sublayer_plan().empty());
    bool found_independent = false;
    for (const auto& interval : printed->local_z_intervals()) {
        if (interval.layer_id == 0)
            CHECK_FALSE(interval.independent_layer_height);
        found_independent |= interval.independent_layer_height;
    }
    CHECK(found_independent);
    for (const auto& pass : printed->local_z_sublayer_plan()) {
        CHECK(pass.flow_height > 0.0);
        CHECK(pass.flow_height <= .30 + 1e-6);
        if (pass.layer_id == 0) {
            CHECK_FALSE(pass.split_interval);
            CHECK(pass.flow_height == Approx(.20));
        }
    }
    if (gradient) {
        bool found_solid_center = false;
        for (const auto& pass : printed->local_z_sublayer_plan()) {
            if (pass.z_lo < 1.3 || pass.z_hi > 1.8)
                continue;
            REQUIRE(pass.painted_masks_by_extruder.size() >= 3);
            CHECK(pass.painted_masks_by_extruder[0].empty());
            CHECK_FALSE(pass.painted_masks_by_extruder[1].empty());
            CHECK(pass.painted_masks_by_extruder[2].empty());
            found_solid_center = true;
        }
        CHECK(found_solid_center);
    }
    print.process();
    const auto           output = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("product-port-%%%%-%%%%.gcode");
    GCodeProcessorResult result;
    const auto           path = print.export_gcode(output.string(), &result, nullptr);
    REQUIRE(boost::filesystem::exists(path));
    std::array<bool, 4> used_filaments{};
    for (const auto& move : result.moves) {
        if (move.type != EMoveType::Extrude)
            continue;
        REQUIRE(move.extruder_id < used_filaments.size());
        used_filaments[move.extruder_id] = true;
        REQUIRE(move.position.allFinite());
        REQUIRE(std::isfinite(move.height));
        REQUIRE(move.height > 0.f);
    }
    CHECK(std::all_of(used_filaments.begin(), used_filaments.end(), [](bool used) { return used; }));
    std::ifstream     stream(path);
    const std::string gcode((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    CHECK(gcode.find("G1") != std::string::npos);
    const std::regex   invalid_coordinate(R"((^|[ \t])[XYZEFRIJ][-+]?(nan|inf)([ \t]|$))", std::regex::icase);
    std::istringstream commands(gcode);
    std::string        line;
    while (std::getline(commands, line)) {
        line = line.substr(0, line.find(';'));
        REQUIRE_FALSE(std::regex_search(line, invalid_coordinate));
    }
    stream.close();
    boost::filesystem::remove(path);
}

TEST_CASE("Gradient solid widths are independent, bounded and default to three percent", "[ProductPort][Gradient]")
{
    MixedFilament row;
    row.gradient_component_ids  = "1234";
    row.gradient_stop_positions = {0.f, .1f, .3f, .5f, .7f, .9f, 1.f};
    auto widths                 = mixed_gradient_solid_half_widths(row, 4);
    REQUIRE(widths.size() == 4);
    CHECK(widths[0] == 0.f);
    CHECK(widths[1] == Approx(.015));
    CHECK(widths[2] == Approx(.015));
    CHECK(widths[3] == 0.f);
    row.gradient_solid_widths = {1.f, .08f, .24f, 1.f};
    CHECK(sample_mixed_gradient(row, 4, .33, .9).component_a == 2);
    CHECK(sample_mixed_gradient(row, 4, .33, .9).component_b == 2);
    CHECK(sample_mixed_gradient(row, 4, .60, .01).component_a == 3);
    CHECK(sample_mixed_gradient(row, 4, .60, .01).component_b == 3);
    CHECK(sample_mixed_gradient(row, 4, .37, .9).component_a != sample_mixed_gradient(row, 4, .37, .9).component_b);
    row.gradient_solid_widths = {0.f, 1.f, -.1f, 0.f};
    widths                    = mixed_gradient_solid_half_widths(row, 4);
    CHECK(widths[1] == Approx(.2));
    CHECK(widths[2] == 0.f);
    const auto defaults = DynamicPrintConfig::full_print_config();
    CHECK(defaults.option<ConfigOptionPercent>("dithering_local_z_gradient_middle_filament_window")->value == Approx(3));
}

TEST_CASE("SML restores direct independent heights for painted two and three filament mixes", "[ProductPort][LocalZ][Slice]")
{
    const std::vector<std::string> colors{"#00FF00", "#FFFFFF", "#FFFF00", "#000000"};
    MixedFilamentManager manager;
    manager.add_custom_filament(1, 2, 65, colors);
    manager.mixed_filaments().back().gradient_component_ids = "12";
    manager.mixed_filaments().back().gradient_component_weights = "35/65";
    manager.add_custom_filament(1, 3, 69, colors);
    manager.mixed_filaments().back().gradient_component_ids = "123";
    manager.mixed_filaments().back().gradient_component_weights = "13/18/69";
    for (auto& row : manager.mixed_filaments())
        row.distribution_mode = int(MixedFilament::LayerCycle);

    auto config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    config.set("layer_height", .12);
    config.set("initial_layer_print_height", .20);
    config.set("mixed_filament_height_lower_bound", .06);
    config.set("dithering_local_z_mode", false);
    config.set("dithering_local_z_direct_multicolor", false);
    config.set("dithering_local_z_independent_layer_height", false);
    config.option<ConfigOptionStrings>("filament_colour")->values = colors;
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75, 1.75};
    config.option<ConfigOptionFloats>("max_layer_height")->values = {.30, .30, .30, .30};

    Model model;
    auto* object = model.add_object();
    object->config.set("extruder", 4);
    auto* volume = object->add_volume(make_cube(8., 8., 5.));
    TriangleSelector selector(volume->mesh());
    const auto& mesh = volume->mesh().its;
    for (size_t facet = 0; facet < mesh.indices.size(); ++facet) {
        const auto& triangle = mesh.indices[facet];
        const float center_x = (mesh.vertices[triangle[0]].x() + mesh.vertices[triangle[1]].x() + mesh.vertices[triangle[2]].x()) / 3.f;
        selector.set_facet(int(facet), static_cast<EnforcerBlockerType>(center_x < 4.f ? 5 : 6));
    }
    volume->mmu_segmentation_facets.set(selector);
    object->add_instance();
    object->ensure_on_bed();

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    print.get_object(0)->slice();
    REQUIRE(print.get_object(0)->local_z_sublayer_plan().empty());

    // Re-enabling SML must recover even if both hidden flags were saved false.
    config.set("dithering_local_z_mode", true);
    print.apply(model, config);
    CHECK(print.config().dithering_local_z_direct_multicolor.value);
    auto* printed = print.get_object(0);
    printed->slice();
    REQUIRE_FALSE(printed->local_z_sublayer_plan().empty());
    std::array<std::array<size_t, 3>, 2> counts{};
    // Yellow needs .31846 mm total per cycle, so the .30 mm printer limit
    // divides it into two .15923 mm passes. The pair needs .06 / .11143 mm.
    const double expected[2][3] = {{.06, .06 * 65 / 35, 0}, {.06, .06 * 18 / 13, .06 * 69 / 13 / 2}};
    for (const auto& interval : printed->local_z_intervals()) {
        if (interval.layer_id > 0 && interval.has_mixed_paint)
            CHECK(interval.independent_layer_height);
    }
    for (const auto& pass : printed->local_z_sublayer_plan()) {
        if (pass.z_lo < .5 || pass.z_hi > 4.5 || pass.dependency_group < 1 || pass.dependency_group > 2)
            continue;
        const size_t row = pass.dependency_group - 1;
        for (size_t component = 0; component < 3; ++component) {
            if (pass.painted_masks_by_extruder[component].empty())
                continue;
            ++counts[row][component];
            CHECK(pass.flow_height == Approx(expected[row][component]).margin(1e-6));
        }
    }
    CHECK(counts[0][0] > 10);
    CHECK(counts[0][1] > 10);
    CHECK(counts[0][2] == 0);
    for (size_t component = 0; component < 3; ++component)
        CHECK(counts[1][component] > 5);
}

TEST_CASE("Gradient design previews retain full transitions at the minimum print cycle", "[ProductPort][Gradient]")
{
    MixedFilament row;
    row.gradient_enabled        = true;
    row.gradient_component_ids  = "123";
    row.gradient_stop_positions = {0.f, .25f, .5f, .75f, 1.f};
    row.gradient_solid_widths   = {0.f, .08f, 0.f};
    MixedFilamentDisplayContext context;
    context.num_physical                           = 3;
    context.color_engine                           = MixedFilamentColorEngine::FullSpectrumKSPairResidual;
    context.preview_settings.mixed_lower_bound     = .06;
    context.preview_settings.gradient_cycle_height = .12;
    bool calibrated                                = false;
    SECTION("Generic green white yellow from the reported preview") { context.physical_colors = {"#00FF00", "#FFFFFF", "#FFFF00"}; }
    SECTION("Calibrated pack retains measured prediction")
    {
        calibrated                    = true;
        context.physical_colors       = {"#008BB3", "#AD4A76", "#EBBE00"};
        context.physical_tds          = {5.5, 5.5, 9.5};
        context.physical_material_ids = {"snapmaker:34267", "snapmaker:34268", "snapmaker:34269"};
    }
    for (double segment_start : {0.0, .5}) {
        std::set<std::string> colors;
        for (int i = 1; i <= 8; ++i) {
            const double t      = segment_start + .05 * i;
            const auto   sample = sample_mixed_gradient(row, 3, t, .08);
            const auto   color  = mixed_gradient_display_color(row, context, t);
            colors.insert(color);
            std::vector<MixedFilamentColorInput> inputs;
            for (const auto& [id, weight] : std::vector<std::pair<unsigned int, int>>{{sample.component_a, 100 - sample.mix_b_percent},
                                                                                      {sample.component_b, sample.mix_b_percent}}) {
                if (weight == 0)
                    continue;
                MixedFilamentColorInput input{context.physical_colors[id - 1], weight};
                if (calibrated) {
                    input.td_mm       = context.physical_tds[id - 1];
                    input.material_id = context.physical_material_ids[id - 1];
                }
                inputs.push_back(input);
            }
            CHECK(color == MixedFilamentManager::blend_color_multi(inputs, calibrated ?
                                                                               MixedFilamentColorEngine::FullSpectrumKSPairResidual :
                                                                               MixedFilamentColorEngine::FilamentMixer));
            auto larger_cycle                                   = context;
            larger_cycle.preview_settings.gradient_cycle_height = .4;
            CHECK(mixed_gradient_display_color(row, larger_cycle, t) == color);
        }
        CHECK(colors.size() >= 7);
    }
    CHECK(mixed_gradient_display_color(row, context, 0.) == context.physical_colors[0]);
    CHECK(mixed_gradient_display_color(row, context, 1.) == context.physical_colors[2]);
    for (double t : {.48, .5, .52})
        CHECK(mixed_gradient_display_color(row, context, t) == context.physical_colors[1]);
    // Display changes do not relax physical minimum-height constraints.
    const auto heights = mixed_filament_local_z_pair_heights(.12, .06, 10);
    CHECK(heights.first == Approx(.06));
    CHECK(heights.second == Approx(.06));
}
