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
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include <boost/filesystem.hpp>
#include <fstream>
#include <cmath>
#include <locale>
#include <map>
#include <regex>
#include <set>

using namespace Slic3r;
using Catch::Approx;

TEST_CASE("Typed mixes retain product gradient order and widths", "[ProductPort][Refactor]")
{
    MixedFilamentLegacyRow row;
    row.component_a                = 1;
    row.component_b                = 3;
    row.stable_id                  = 42;
    row.custom                     = true;
    row.ui_mode                    = 3;
    row.gradient_enabled           = true;
    row.gradient_start             = 1.f;
    row.gradient_end               = 0.f;
    row.gradient_component_ids     = "123";
    row.gradient_component_weights = "23/22/55";
    row.gradient_stop_positions    = {0.f, .2f, .5f, .8f, 1.f};
    row.gradient_solid_widths      = {0.f, .03f, 0.f};
    auto definition                = mixed_filament_definition_from_legacy_row(row, 3);
    CHECK(definition.recipe.blend.component_ids() == std::vector<unsigned int>{1, 3, 2});
    auto rebuilt = mixed_filament_legacy_row_from_definition(definition);
    CHECK(rebuilt.gradient_component_ids == row.gradient_component_ids);
    CHECK(rebuilt.gradient_component_weights == row.gradient_component_weights);
    CHECK(rebuilt.gradient_stop_positions == row.gradient_stop_positions);
    CHECK(rebuilt.gradient_solid_widths == row.gradient_solid_widths);
    CHECK(rebuilt.gradient_start == 1.f);
    CHECK(rebuilt.gradient_end == 0.f);
    CHECK(rebuilt.ui_mode == 3);
    CHECK(rebuilt.stable_id == 42);
}

TEST_CASE("Typed manual patterns retain multi-digit physical references", "[ProductPort][Refactor]")
{
    MixedFilamentLegacyRow row;
    row.component_a       = 11;
    row.component_b       = 2;
    row.manual_pattern    = "1[10]2,[12]21";
    const auto definition = mixed_filament_definition_from_legacy_row(row, 12);
    REQUIRE(definition.recipe.manual_pattern);
    CHECK(mixed_filament_manual_pattern_sequence(definition, 12) == std::vector<unsigned int>{11, 10, 2, 12, 2, 11});
    const auto rebuilt = mixed_filament_legacy_row_from_definition(definition);
    CHECK(rebuilt.manual_pattern == row.manual_pattern);
}

TEST_CASE("Typed and legacy edits stay coherent across manager queries", "[ProductPort][Refactor]")
{
    const std::vector<std::string> colors{"#00FF40", "#FFFFFF", "#FFFF59"};
    MixedFilamentManager           manager;
    manager.add_custom_filament(1, 2, 50, colors);
    auto& row          = manager.mixed_filaments().front();
    row.manual_pattern = "12,21";
    CHECK(manager.ordered_perimeter_extruders(4, 3, 0) == std::vector<unsigned int>{1, 2});
    CHECK(manager.ordered_perimeter_extruders(4, 3, 1) == std::vector<unsigned int>{2, 1});
    row.manual_pattern = "21,12";
    CHECK(manager.ordered_perimeter_extruders(4, 3, 0) == std::vector<unsigned int>{2, 1});
    row.enabled = false;
    CHECK(manager.enabled_count() == 0);
    CHECK_FALSE(manager.mixed_filament_definition_from_id(4, 3));
    row.enabled     = true;
    auto definition = manager.mixed_filament_definition_from_id(4, 3);
    REQUIRE(definition);
    definition->visibility.tombstoned = true;
    REQUIRE(manager.set_mixed_filament_definition(0, *definition, colors));
    CHECK(manager.enabled_count() == 0);
    manager.cleanup_deleted_entries();
    CHECK(manager.mixed_filament_count() == 0);
}

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

TEST_CASE("Local-Z prime towers emit every planned layer and extrusion", "[ProductPort][LocalZ][GCode][WipeTower]")
{
    double layer_height = .20;
    bool   gradient     = false;
    SECTION("Independent mixes at standard layer height") {}
    SECTION("Independent mixes with single-pass intervals") { layer_height = .08; }
    SECTION("Gradients with single-pass intervals")
    {
        layer_height = .08;
        gradient     = true;
    }

    const std::vector<std::string> colors{"#00C3FF", "#F54399", "#FAE727", "#9199A4"};
    MixedFilamentManager           manager;
    manager.add_custom_filament(1, 2, 65, colors);
    manager.mixed_filaments().back().gradient_component_ids     = "12";
    manager.mixed_filaments().back().gradient_component_weights = "35/65";
    manager.add_custom_filament(1, 3, 69, colors);
    manager.mixed_filaments().back().gradient_component_ids     = "123";
    manager.mixed_filaments().back().gradient_component_weights = "13/18/69";
    for (auto& row : manager.mixed_filaments()) {
        row.distribution_mode = int(MixedFilament::LayerCycle);
        row.gradient_enabled  = gradient;
    }

    auto config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    config.set("layer_height", layer_height);
    config.set("initial_layer_print_height", .20);
    config.set("mixed_filament_height_lower_bound", .06);
    config.set("dithering_local_z_mode", true);
    config.set("dithering_local_z_infill", true);
    config.set("enable_prime_tower", true);
    config.set("wipe_tower_no_sparse_layers", false);
    config.set("single_extruder_multi_material", false);
    config.set("prime_volume", 15.);
    config.set("machine_start_gcode", "G90\nM83\n");
    config.option<ConfigOptionStrings>("filament_colour")->values  = colors;
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75, 1.75};
    config.option<ConfigOptionFloats>("max_layer_height")->values  = {.30, .30, .30, .30};
    config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.assign(16, 100.);
    // Production G-code export requires the enum dictionaries used by preset loading.
    for (const auto& key : config.keys()) {
        if (config.option(key)->type() != coEnums)
            continue;
        auto* option = config.def()->get(key)->create_default_option();
        option->set(config.option(key));
        config.set_key_value(key, option);
    }

    Model model;
    auto* object = model.add_object();
    object->name = "prime-tower-mixed-pass-regression";
    object->config.set("extruder", 4);
    auto*            volume = object->add_volume(make_cube(8., 8., 5.));
    TriangleSelector selector(volume->mesh());
    const auto&      mesh = volume->mesh().its;
    for (size_t facet = 0; facet < mesh.indices.size(); ++facet) {
        const auto& triangle = mesh.indices[facet];
        const float center_x = (mesh.vertices[triangle[0]].x() + mesh.vertices[triangle[1]].x() + mesh.vertices[triangle[2]].x()) / 3.f;
        selector.set_facet(int(facet), static_cast<EnforcerBlockerType>(center_x < 4.f ? 5 : 6));
    }
    volume->mmu_segmentation_facets.set(selector);
    object->add_instance()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();

    Print print;
    print.is_BBL_printer() = false;
    print.set_status_silent();
    print.apply(model, config);
    print.process();
    const auto& tower = print.wipe_tower_data();
    REQUIRE(tower.tool_changes.size() > 10);
    REQUIRE(tower.local_z_tool_changes.size() == tower.tool_changes.size());
    if (layer_height < .1) {
        const auto& intervals = print.get_object(0)->local_z_intervals();
        REQUIRE(std::any_of(intervals.begin(), intervals.end(), [](const LocalZInterval& interval) {
            return interval.independent_layer_height && interval.sublayer_count == 1;
        }));
    }

    struct TemporaryGCode
    {
        boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("tower-%%%%-%%%%.gcode");
        ~TemporaryGCode()
        {
            boost::system::error_code ec;
            boost::filesystem::remove(path, ec);
        }
    } temporary;
    GCodeProcessorResult result;
    print.export_gcode(temporary.path.string(), &result, nullptr);
    // Compare the actual exported moves with every nominal and Local-Z tower
    // section. Checking height alone misses skipped walls on partly printed layers.
    auto                  z_key = [](double z) { return int(std::lround(z * 1000.)); };
    std::map<int, double> actual_lengths;
    for (size_t idx = 1; idx < result.moves.size(); ++idx) {
        const auto& move = result.moves[idx];
        if (move.type == EMoveType::Extrude && move.extrusion_role == erWipeTower)
            actual_lengths[z_key(move.position.z())] += (move.position - result.moves[idx - 1].position).head<2>().norm();
    }
    for (size_t layer_idx = 0; layer_idx < tower.tool_changes.size(); ++layer_idx) {
        const auto& nominal = tower.tool_changes[layer_idx];
        REQUIRE_FALSE(nominal.empty());
        const auto& local_z         = tower.local_z_tool_changes[layer_idx];
        double      expected_length = 0.;
        for (const auto* changes : {&nominal, &local_z}) {
            for (const auto& change : *changes) {
                // Upstream now stores nozzle-unload extrusion separately from the tower preview paths.
                if (!change.nozzle_change_result.gcode.empty()) {
                    GCodeReader reader;
                    reader.apply_config(config);
                    reader.x() = change.nozzle_change_result.start_pos.x();
                    reader.y() = change.nozzle_change_result.start_pos.y();
                    reader.parse_buffer(change.nozzle_change_result.gcode, [&](GCodeReader& state, const GCodeReader::GCodeLine& line) {
                        if (line.extruding(state))
                            expected_length += line.dist_XY(state);
                    });
                }
                for (size_t idx = 1; idx < change.extrusions.size(); ++idx) {
                    if (change.extrusions[idx].width > 0.f)
                        expected_length += (change.extrusions[idx].pos - change.extrusions[idx - 1].pos).norm();
                }
            }
        }
        CAPTURE(layer_idx, nominal.front().print_z, expected_length);
        REQUIRE(expected_length > 0.);
        const auto actual = actual_lengths.find(z_key(nominal.front().print_z));
        REQUIRE(actual != actual_lengths.end());
        // Tower coordinates are rounded to 0.001 mm when exported.
        CHECK(actual->second == Approx(expected_length).margin(.5));
    }
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
    MixedFilamentManager           manager;
    manager.add_custom_filament(1, 2, 65, colors);
    manager.mixed_filaments().back().gradient_component_ids     = "12";
    manager.mixed_filaments().back().gradient_component_weights = "35/65";
    manager.add_custom_filament(1, 3, 69, colors);
    manager.mixed_filaments().back().gradient_component_ids     = "123";
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
    config.option<ConfigOptionStrings>("filament_colour")->values  = colors;
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75, 1.75};
    config.option<ConfigOptionFloats>("max_layer_height")->values  = {.30, .30, .30, .30};

    Model model;
    auto* object = model.add_object();
    object->config.set("extruder", 4);
    auto*            volume = object->add_volume(make_cube(8., 8., 5.));
    TriangleSelector selector(volume->mesh());
    const auto&      mesh = volume->mesh().its;
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

TEST_CASE("Mixed filament typed definition resolves legacy manual pattern tokens", "[MixedFilament][Refactor]")
{
    MixedFilamentLegacyRow row;
    row.component_a                = 4;
    row.component_b                = 2;
    row.stable_id                  = 123;
    row.custom                     = true;
    row.origin_auto                = false;
    row.mix_b_percent              = 50;
    row.ratio_a                    = 3;
    row.ratio_b                    = 1;
    row.manual_pattern             = MixedFilamentManager::normalize_manual_pattern("12,31");
    row.local_z_max_sublayers      = 5;
    row.component_a_surface_offset = 0.02f;
    row.component_b_surface_offset = -0.01f;
    row.display_color              = "#123456";

    const MixedFilamentDefinition definition = mixed_filament_definition_from_legacy_row(row, 9);
    CHECK(definition.identity.stable_id == 123);
    CHECK(definition.source.kind == MixedFilamentSourceKind::Custom);
    CHECK(definition.recipe.kind == MixedFilamentRecipeKind::ManualPattern);
    REQUIRE(definition.recipe.blend.components.size() == 3);
    CHECK(definition.recipe.blend.components[0].filament.id == 4);
    CHECK(definition.recipe.blend.components[0].percent == 50);
    CHECK(definition.recipe.blend.components[1].filament.id == 2);
    CHECK(definition.recipe.blend.components[1].percent == 25);
    CHECK(definition.recipe.blend.components[2].filament.id == 3);
    CHECK(definition.recipe.blend.components[2].percent == 25);
    CHECK(definition.behavior.layer_cadence.component_a_layers == 3);
    CHECK(definition.behavior.layer_cadence.component_b_layers == 1);
    CHECK(definition.behavior.local_z.max_sublayers == 5);
    CHECK(definition.behavior.surface_bias.component_a_offset_mm == Approx(0.02f));
    CHECK(definition.behavior.surface_bias.component_b_offset_mm == Approx(-0.01f));
    CHECK(definition.presentation.display_color == "#123456");

    REQUIRE(definition.recipe.manual_pattern);
    REQUIRE(definition.recipe.manual_pattern->groups.size() == 2);
    REQUIRE(definition.recipe.manual_pattern->groups[0].size() == 2);
    REQUIRE(definition.recipe.manual_pattern->groups[1].size() == 2);
    CHECK(definition.recipe.manual_pattern->groups[0][0].id == 4);
    CHECK(definition.recipe.manual_pattern->groups[0][1].id == 2);
    CHECK(definition.recipe.manual_pattern->groups[1][0].id == 3);
    CHECK(definition.recipe.manual_pattern->groups[1][1].id == 4);

    CHECK(mixed_filament_manual_pattern_sequence(definition, 9) == std::vector<unsigned int>{4, 2, 3, 4});
    CHECK(mixed_filament_manual_pattern_preview_sequence(definition, 9, 2) == std::vector<unsigned int>{4, 3, 2, 4});

    const MixedFilamentLegacyRow rebuilt = mixed_filament_legacy_row_from_definition(definition);
    CHECK(rebuilt.component_a == 4);
    CHECK(rebuilt.component_b == 2);
    CHECK(rebuilt.manual_pattern == "12,31");
    CHECK(rebuilt.mix_b_percent == 25);
    CHECK(rebuilt.gradient_component_ids.empty());
    CHECK(rebuilt.gradient_component_weights.empty());
}

TEST_CASE("Mixed filament manual pattern aggregate keeps legacy primary pair", "[MixedFilament][Refactor]")
{
    MixedFilamentLegacyRow row;
    row.component_a    = 1;
    row.component_b    = 2;
    row.mix_b_percent  = 50;
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("2,12");

    const MixedFilamentDefinition definition = mixed_filament_definition_from_legacy_row(row, 2);
    REQUIRE(definition.recipe.manual_pattern);
    const MixedFilamentPrimaryPairView pair = definition.recipe.blend.primary_pair_or(0, 0);
    CHECK(pair.component_a.id == 1);
    CHECK(pair.component_b.id == 2);
    CHECK(definition.recipe.blend.component_ids(2) == std::vector<unsigned int>{1, 2});
    CHECK(definition.recipe.blend.component_percents(2) == std::vector<int>{33, 67});

    const MixedFilamentLegacyRow rebuilt = mixed_filament_legacy_row_from_definition(definition);
    CHECK(rebuilt.component_a == 1);
    CHECK(rebuilt.component_b == 2);
    CHECK(rebuilt.manual_pattern == "2,12");
}

TEST_CASE("Mixed filament typed definition exposes weighted blend components and weights", "[MixedFilament][Refactor]")
{
    MixedFilamentLegacyRow row;
    row.component_a                = 1;
    row.component_b                = 2;
    row.stable_id                  = 456;
    row.custom                     = true;
    row.gradient_component_ids     = "312";
    row.gradient_component_weights = "50/25/25";
    row.distribution_mode          = int(MixedFilamentLegacyRow::LayerCycle);

    const MixedFilamentDefinition definition = mixed_filament_definition_from_legacy_row(row, 9);
    CHECK(definition.recipe.kind == MixedFilamentRecipeKind::WeightedBlend);
    CHECK(definition.behavior.distribution == MixedFilamentDistributionMode::LayerCycle);

    REQUIRE(definition.recipe.blend.components.size() == 3);
    CHECK(definition.recipe.blend.components[0].filament.id == 1);
    CHECK(definition.recipe.blend.components[0].percent == 25);
    CHECK(definition.recipe.blend.components[1].filament.id == 2);
    CHECK(definition.recipe.blend.components[1].percent == 25);
    CHECK(definition.recipe.blend.components[2].filament.id == 3);
    CHECK(definition.recipe.blend.components[2].percent == 50);
    CHECK(definition.recipe.blend.component_ids(9) == std::vector<unsigned int>{1, 2, 3});
    CHECK(definition.recipe.blend.component_percents(9) == std::vector<int>{25, 25, 50});
    CHECK(definition.recipe.blend.component_ids(2) == std::vector<unsigned int>{1, 2});
    CHECK(definition.recipe.blend.component_percents(2) == std::vector<int>{25, 25});

    const MixedFilamentLegacyRow rebuilt = mixed_filament_legacy_row_from_definition(definition);
    CHECK(rebuilt.gradient_component_ids == "123");
    CHECK(rebuilt.gradient_component_weights == "25/25/50");
    CHECK(rebuilt.manual_pattern.empty());
}

TEST_CASE("Mixed filament legacy gradient decoding filters unavailable physical components", "[MixedFilament][Refactor]")
{
    MixedFilamentLegacyRow row;
    row.component_a                = 1;
    row.component_b                = 2;
    row.mix_b_percent              = 50;
    row.gradient_component_ids     = "1235";
    row.gradient_component_weights = "10/20/30/40";
    row.distribution_mode          = int(MixedFilamentLegacyRow::LayerCycle);

    MixedFilamentDefinition definition = mixed_filament_definition_from_legacy_row(row, 3);
    CHECK(definition.recipe.blend.component_ids(3) == std::vector<unsigned int>{1, 2, 3});
    CHECK(definition.recipe.blend.component_percents(3) == std::vector<int>{17, 33, 50});
    CHECK(definition.behavior.distribution == MixedFilamentDistributionMode::LayerCycle);

    row.gradient_component_ids     = "125";
    row.gradient_component_weights = "10/20/70";
    definition                     = mixed_filament_definition_from_legacy_row(row, 2);
    CHECK(definition.recipe.blend.component_ids(2) == std::vector<unsigned int>{1, 2});
    CHECK(definition.behavior.distribution == MixedFilamentDistributionMode::LayerCycle);

    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("125");
    definition         = mixed_filament_definition_from_legacy_row(row, 2);
    CHECK(mixed_filament_manual_pattern_sequence(definition, 2) == std::vector<unsigned int>{1, 2});
}

TEST_CASE("Mixed filament manager accepts typed definitions at the boundary", "[MixedFilament][Refactor]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};

    MixedFilamentDefinition definition;
    definition.recipe.kind  = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend = MixedFilamentWeightedBlend{
        {{MixedFilamentPhysicalRef{1}, 50}, {MixedFilamentPhysicalRef{2}, 25}, {MixedFilamentPhysicalRef{3}, 25}}};
    definition.behavior.distribution = MixedFilamentDistributionMode::LayerCycle;

    MixedFilamentManager mgr;
    REQUIRE(mgr.add_custom_filament_definition(definition, colors));
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    const std::vector<MixedFilamentDefinition> definitions = mgr.mixed_filament_definitions(colors.size());
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().source.kind == MixedFilamentSourceKind::Custom);
    CHECK(definitions.front().identity.stable_id != 0);
    CHECK(definitions.front().recipe.blend.component_ids(colors.size()) == std::vector<unsigned int>{1, 2, 3});

    MixedFilamentDefinition edited = definitions.front();
    edited.recipe.blend.components = {{MixedFilamentPhysicalRef{1}, 35}, {MixedFilamentPhysicalRef{2}, 65}};
    edited.recipe.kind             = MixedFilamentRecipeKind::WeightedBlend;
    edited.behavior.distribution   = MixedFilamentDistributionMode::Simple;
    REQUIRE(mgr.set_mixed_filament_definition(0, edited, colors));

    const auto roundtrip = mgr.mixed_filament_definition_from_id(4, colors.size());
    REQUIRE(roundtrip);
    CHECK(roundtrip->identity.stable_id == definitions.front().identity.stable_id);
    CHECK(roundtrip->recipe.kind == MixedFilamentRecipeKind::WeightedBlend);
    CHECK(roundtrip->recipe.blend.is_pair());
    CHECK(roundtrip->recipe.blend.primary_pair_or().component_b_percent == 65);
    CHECK(roundtrip->recipe.blend.component_ids(colors.size()) == std::vector<unsigned int>{1, 2});
}

namespace {
// Pre-refactor project syntax: compact and slash-delimited IDs, old mode flags,
// stable IDs, gradient stops/widths, manual perimeter groups and unavailable rows.
const std::string legacy_compat_recipes = "1,2,1,1,50,0,g1/2/12,w33/33/34,m0,u101,cm2;"
                                          "1,2,1,1,50,0,g123,w0/0/100,m2,u102;"
                                          "1,12,1,1,50,0,g1/3/12,w20/30/50,m0,u103,cm3,r1/1.0000/0.0000,p0/0.2/0.5/0.8/1,v0/0.03/0;"
                                          "1,2,1,1,50,0,g,w,m0,u104,cm1,1[12]2,21[10];"
                                          "1,2,0,1,25,0,m2,u105;"
                                          "2,3,0,1,75,0,m2,d1,u106";

void check_legacy_compat_recipes(const MixedFilamentManager& manager)
{
    const auto& rows = manager.mixed_filament_legacy_rows();
    REQUIRE(rows.size() == 6);
    for (size_t i = 0; i < rows.size(); ++i)
        CHECK(rows[i].stable_id == 101 + i);
    CHECK(rows[0].gradient_component_ids == "1/2/12");
    CHECK(rows[0].gradient_component_weights == "33/33/34");
    CHECK(rows[0].mix_b_percent == 50);
    CHECK(rows[1].distribution_mode == int(MixedFilament::Simple));
    CHECK(rows[1].mix_b_percent == 50);
    CHECK(rows[1].gradient_component_ids == "123");
    CHECK(rows[1].gradient_component_weights == "0/0/100");
    CHECK(rows[2].gradient_enabled);
    CHECK(rows[2].gradient_component_ids == "1/3/12");
    CHECK(rows[2].gradient_component_weights == "20/30/50");
    REQUIRE(rows[2].gradient_stop_positions.size() == 5);
    CHECK(rows[2].gradient_stop_positions[1] == Approx(.2));
    REQUIRE(rows[2].gradient_solid_widths.size() == 3);
    CHECK(rows[2].gradient_solid_widths[1] == Approx(.03));
    CHECK(rows[2].gradient_start == 1.f);
    CHECK(rows[2].gradient_end == 0.f);
    CHECK(rows[3].manual_pattern == "1[12]2,21[10]");
    CHECK_FALSE(rows[4].enabled);
    CHECK_FALSE(rows[4].deleted);
    CHECK_FALSE(rows[5].enabled);
    CHECK(rows[5].deleted);
    CHECK(manager.filament_id_from_stable_id(101, 12) == std::optional<unsigned int>(13));
    CHECK(manager.filament_id_from_stable_id(104, 12) == std::optional<unsigned int>(16));
    CHECK_FALSE(manager.filament_id_from_stable_id(105, 12));
}
} // namespace

TEST_CASE("Static typed blends preserve all supported legacy filament IDs", "[ProductPort][Refactor][Persistence]")
{
    for (unsigned int last : {9u, 12u, unsigned(MixedFilamentManager::kMaxPhysicalFilaments)}) {
        CAPTURE(last);
        MixedFilamentLegacyRow row;
        row.component_a                = 1;
        row.component_b                = 2;
        row.distribution_mode          = int(MixedFilament::LayerCycle);
        row.gradient_component_ids     = MixedFilamentManager::encode_gradient_component_ids({1, 2, last});
        row.gradient_component_weights = "20/30/50";
        const auto typed               = mixed_filament_definition_from_legacy_row(row, last);
        const auto rebuilt             = mixed_filament_legacy_row_from_definition(typed);
        CHECK(rebuilt.gradient_component_ids == row.gradient_component_ids);
        CHECK(rebuilt.gradient_component_weights == row.gradient_component_weights);
        CHECK(rebuilt.mix_b_percent == row.mix_b_percent);
        CHECK_FALSE(rebuilt.gradient_enabled);
        auto edited                    = typed;
        edited.recipe.blend.components = {{{1}, 35}, {{2}, 65}};
        CHECK(mixed_filament_legacy_row_from_definition(edited).mix_b_percent == 65);
        if (last == 12) {
            const std::vector<std::string> colors(12, "#808080");
            MixedFilamentManager           manager;
            REQUIRE(manager.add_custom_filament_definition(typed, colors));
            auto changed                    = manager.mixed_filament_definitions().front();
            changed.recipe.blend.components = {{{1}, 35}, {{2}, 65}};
            REQUIRE(manager.set_mixed_filament_definition(0, changed, colors));
            CHECK(manager.mixed_filament_legacy_rows().front().mix_b_percent == 65);
            changed                         = manager.mixed_filament_definitions().front();
            changed.recipe.blend.components = {{{1}, 70}, {{2}, 30}};
            REQUIRE(manager.set_mixed_filament_definition(0, changed, colors));
            CHECK(manager.mixed_filament_legacy_rows().front().mix_b_percent == 30);
        }
    }
}

TEST_CASE("Simple mixes retain their pair cadence with inactive multi-color weights", "[ProductPort][Refactor][Persistence]")
{
    const std::vector<std::string> colors(3, "#808080");
    std::string                    saved         = "1,2,1,1,50,0,g123,w0/0/100,m2,u102";
    int                            gradient_mode = 0;
    SECTION("Layer-count cadence") {}
    SECTION("Height-weighted cadence") { gradient_mode = 1; }
    for (int generation = 0; generation < 3; ++generation) {
        CAPTURE(generation);
        MixedFilamentManager manager;
        manager.load_custom_entries(saved, colors);
        manager.apply_gradient_settings(gradient_mode, .04f, .16f, false);
        const auto& row = manager.mixed_filament_legacy_rows().front();
        REQUIRE(row.mix_b_percent == 50);
        for (int layer = 0; layer < 8; ++layer)
            CHECK(manager.resolve(4, 3, layer, .075f + .1f * layer, .1f) == unsigned(layer % 2 + 1));
        saved = manager.serialize_custom_entries();
        CHECK(saved.find("g123,w0/0/100,m2") != std::string::npos);
    }
}

TEST_CASE("Old mixed recipe strings survive repeated typed manager save and load", "[ProductPort][Refactor][Persistence]")
{
    const std::vector<std::string> colors(12, "#808080");
    std::string                    saved = legacy_compat_recipes;
    for (int generation = 0; generation < 3; ++generation) {
        CAPTURE(generation);
        MixedFilamentManager manager;
        manager.load_custom_entries(saved, colors);
        manager.apply_gradient_settings(0, .04f, .16f, false);
        check_legacy_compat_recipes(manager);
        CHECK(manager.resolve(14, 12, 0) == 1);
        CHECK(manager.resolve(14, 12, 1) == 2);
        const auto next = manager.serialize_custom_entries();
        if (generation > 0)
            CHECK(next == saved);
        saved = next;
    }
}

TEST_CASE("Legacy 3MF recipes and painted assignments survive refactored resaves", "[ProductPort][Refactor][Persistence][3mf]")
{
    struct TemporaryProject
    {
        boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
                                            boost::filesystem::unique_path("mf-compat-%%%%-%%%%");
        TemporaryProject() { boost::filesystem::create_directories(directory); }
        ~TemporaryProject()
        {
            boost::system::error_code ec;
            boost::filesystem::remove_all(directory, ec);
        }
    } temporary;
    const std::vector<std::string> colors(12, "#808080");
    auto                           config = DynamicPrintConfig::full_print_config();
    // Full defaults need enum dictionaries before the production exporter serializes them.
    for (const auto& key : config.keys()) {
        if (config.option(key)->type() != coEnums)
            continue;
        auto* option = config.def()->get(key)->create_default_option();
        option->set(config.option(key));
        config.set_key_value(key, option);
    }
    config.option<ConfigOptionStrings>("filament_colour")->values = colors;
    config.option<ConfigOptionFloats>("filament_diameter")->values.assign(12, 1.75);
    config.set("mixed_filament_definitions", legacy_compat_recipes);
    Model model;
    auto* object = model.add_object();
    object->config.set("extruder", 13);
    auto*            volume = object->add_volume(make_cube(5., 5., 5.));
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType(14));
    volume->mmu_segmentation_facets.set(selector);
    const auto painted_triangle = volume->mmu_segmentation_facets.get_triangle_as_string(0);
    REQUIRE_FALSE(painted_triangle.empty());
    object->add_instance();
    model.set_backup_path((temporary.directory / "export").string());

    for (int generation = 0; generation < 2; ++generation) {
        CAPTURE(generation);
        const auto  path = (temporary.directory / ("project-" + std::to_string(generation) + ".3mf")).string();
        StoreParams params;
        params.path     = path.c_str();
        params.model    = &model;
        params.config   = &config;
        params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
        REQUIRE(store_bbs_3mf(params));
        Model loaded_model;
        loaded_model.set_backup_path((temporary.directory / ("import-" + std::to_string(generation))).string());
        DynamicPrintConfig        loaded_config;
        ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
        struct LoadedExtras
        {
            PlateDataPtrs        plates;
            std::vector<Preset*> presets;
            ~LoadedExtras()
            {
                release_PlateData_list(plates);
                for (auto* preset : presets)
                    delete preset;
            }
        } extras;
        bool   is_project = false;
        Semver version;
        REQUIRE(load_bbs_3mf(path.c_str(), &loaded_config, &substitutions, &loaded_model, &extras.plates, &extras.presets, &is_project,
                             &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence));
        REQUIRE(is_project);
        REQUIRE(loaded_model.objects.size() == 1);
        REQUIRE(loaded_model.objects.front()->volumes.size() == 1);
        CHECK(loaded_model.objects.front()->config.opt_int("extruder") == 13);
        CHECK(loaded_model.objects.front()->volumes.front()->mmu_segmentation_facets.get_triangle_as_string(0) == painted_triangle);
        REQUIRE(loaded_config.option<ConfigOptionString>("mixed_filament_definitions"));
        CHECK(loaded_config.opt_string("mixed_filament_definitions") == config.opt_string("mixed_filament_definitions"));
        MixedFilamentManager manager;
        manager.load_custom_entries(loaded_config.opt_string("mixed_filament_definitions"), colors);
        manager.apply_gradient_settings(0, .04f, .16f, false);
        check_legacy_compat_recipes(manager);
        CHECK(manager.resolve(14, 12, 0) == 1);
        CHECK(manager.resolve(14, 12, 1) == 2);
        // Save the migrated manager through the same old project setting, then reopen.
        config.set("mixed_filament_definitions", manager.serialize_custom_entries());
        model = std::move(loaded_model);
    }
}

TEST_CASE("Gradient solid-zone ramps thicken only the approaching filament", "[ProductPort][Gradient][GradientRamp]")
{
    MixedFilament row;
    row.gradient_enabled        = true;
    row.gradient_component_ids  = "123";
    row.gradient_stop_positions = {0.f, .25f, .5f, .75f, 1.f};
    row.gradient_solid_widths   = {0.f, .03f, 0.f};
    const auto sample           = [&](double t, double maximum = .30) {
        return sample_mixed_gradient_local_z(row, 3, t, .03, .20, .06, {.30, maximum, .30});
    };
    const auto white_height = [](const MixedGradientLocalZSample& value) {
        return value.mix.component_a == 2 ? value.height_a : value.height_b;
    };
    for (double t : {.0, .2, .4, .6, .8, 1.0}) {
        const auto actual   = sample(t);
        const auto original = mixed_filament_local_z_pair_heights(.20, .06, actual.mix.mix_b_percent);
        CHECK(actual.height_a == Approx(original.first));
        CHECK(actual.height_b == Approx(original.second));
    }
    double last_white = .14;
    for (int step = 0; step <= 70; ++step) {
        const double t        = .4125 + .001 * step;
        const auto   entering = sample(t);
        const auto   leaving  = sample(1.0 - t);
        CHECK(entering.mix.component_b == 2);
        CHECK(leaving.mix.component_a == 2);
        CHECK(entering.height_a == Approx(.06));
        CHECK(leaving.height_b == Approx(.06));
        CHECK(white_height(entering) >= last_white - 1e-7);
        CHECK(white_height(entering) == Approx(white_height(leaving)).margin(1e-6));
        CHECK(white_height(entering) <= .28 + 1e-7);
        last_white = white_height(entering);
    }
    CHECK(last_white > .279);
    CHECK(white_height(sample(.5)) == Approx(.28));
    CHECK(sample(.5).height_b == 0.0);
    CHECK(white_height(sample(.4825, .24)) > .239);
    CHECK(white_height(sample(.5, .24)) == Approx(.24));
    // A zero-width color stop is not a solid zone.
    row.gradient_solid_widths[1] = 0.f;
    CHECK(white_height(sample(.5)) == Approx(.20));
    row.gradient_solid_widths[1] = .03f;
    row.gradient_enabled         = false;
    CHECK(white_height(sample(.5)) == Approx(.20));
}

TEST_CASE("Gradient ramp follows moved stops and per-filament printer limits", "[ProductPort][Gradient][GradientRamp]")
{
    MixedFilament row;
    row.gradient_enabled        = true;
    row.gradient_component_ids  = "1234";
    row.gradient_start          = 0.f; // Reverse the component order, without reversing the stop positions.
    row.gradient_end            = 1.f;
    row.gradient_stop_positions = {0.f, .1f, .3f, .5f, .7f, .9f, 1.f};
    row.gradient_solid_widths   = {0.f, .04f, .08f, 0.f};
    auto first                  = sample_mixed_gradient_local_z(row, 4, .3, .03, .20, .06, {.3, .24, .27, .3});
    auto second                 = sample_mixed_gradient_local_z(row, 4, .7, .03, .20, .06, {.3, .24, .27, .3});
    CHECK(first.mix.component_a == 3);
    CHECK(first.height_a == Approx(.27));
    CHECK(second.mix.component_a == 2);
    CHECK(second.height_a == Approx(.24));
    // With a different cycle setting, the target remains nominal + .08.
    first = sample_mixed_gradient_local_z(row, 4, .3, .03, .12, .04, {.3, .3, .3, .3});
    CHECK(first.height_a == Approx(.20));
}

TEST_CASE("Sliced gradients ramp on both sides of the solid zone within printer limits", "[ProductPort][LocalZ][Slice][GradientRamp]")
{
    double white_limit = .30;
    SECTION("Normal nozzle limit") {}
    SECTION("Lower white nozzle limit") { white_limit = .24; }
    const std::vector<std::string> colors{"#00FF00", "#FFFFFF", "#FFFF00"};
    MixedFilamentManager           manager;
    manager.add_custom_filament(1, 2, 50, colors);
    auto& row                  = manager.mixed_filaments().back();
    row.gradient_enabled       = true;
    row.gradient_component_ids = "123";
    row.gradient_solid_widths  = {0.f, .03f, 0.f};
    auto config                = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    config.set("layer_height", .20);
    config.set("initial_layer_print_height", .20);
    config.set("mixed_filament_height_lower_bound", .06);
    config.set("dithering_local_z_gradient_layer_height", .20);
    config.option<ConfigOptionStrings>("filament_colour")->values  = colors;
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75};
    config.option<ConfigOptionFloats>("max_layer_height")->values  = {.30, white_limit, .30};
    Model model;
    auto* object = model.add_object();
    object->config.set("extruder", 4);
    object->add_volume(make_cube(8., 8., 20.));
    object->add_instance();
    object->ensure_on_bed();
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    auto* printed = print.get_object(0);
    printed->slice();
    REQUIRE_FALSE(printed->local_z_sublayer_plan().empty());
    bool         ramp_before = false, ramp_after = false, solid_target = false;
    const double target = std::min(.28, white_limit);
    for (const auto& pass : printed->local_z_sublayer_plan()) {
        REQUIRE(pass.flow_height > 0.0);
        CHECK(pass.z_hi <= 20.0 + 1e-5);
        if (pass.layer_id == 0) {
            CHECK(pass.flow_height == Approx(.20));
            continue;
        }
        REQUIRE(pass.painted_masks_by_extruder.size() >= 3);
        for (size_t component = 0; component < 3; ++component) {
            if (pass.painted_masks_by_extruder[component].empty())
                continue;
            CHECK(pass.flow_height <= (component == 1 ? white_limit : .30) + 1e-6);
            if (component == 1) {
                const double mid = .5 * (pass.z_lo + pass.z_hi);
                if (mid < 9.8 && pass.flow_height > .20)
                    ramp_before = true;
                if (mid > 10.4 && pass.flow_height > .20)
                    ramp_after = true;
                if (mid > 9.8 && mid < 10.4 && std::abs(pass.flow_height - target) < 1e-6)
                    solid_target = true;
            }
        }
    }
    CHECK(ramp_before);
    CHECK(ramp_after);
    CHECK(solid_target);
}
