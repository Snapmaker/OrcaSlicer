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
