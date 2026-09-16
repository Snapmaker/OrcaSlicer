#include <catch2/catch.hpp>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MultiMaterialSegmentation.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <string>
#include <vector>

using namespace Slic3r;

// Tests for top/bottom_color_penetration_layers, migrated from BambuStudio
// commit c782fbb8 (color bleed layers decoupled from shell layers).

namespace {

// Two-filament configuration so that the MMU painting segmentation path is active.
DynamicPrintConfig two_filament_config(int top_pen, int bottom_pen)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",       "0.2" },
        { "first_layer_height", "0.2" },
        { "filament_diameter",  "1.75;1.75" },
        // The segmentation state count derives from filament_colour, so both
        // filament lists must name two filaments or painted state 2 is dropped.
        { "filament_colour",    "#FF0000;#0000FF" },
        { "top_color_penetration_layers",    std::to_string(top_pen) },
        { "bottom_color_penetration_layers", std::to_string(bottom_pen) },
    });
    return config;
}

// Paints every upward-facing facet of the volume with extruder 2 so the top
// surface projection (and only it) carries color for the segmentation.
void paint_top_faces(ModelVolume *volume)
{
    const indexed_triangle_set &its = volume->mesh().its;
    TriangleSelector            selector(volume->mesh());
    for (int facet_idx = 0; facet_idx < int(its.indices.size()); ++facet_idx) {
        const Vec3f &a = its.vertices[its.indices[facet_idx](0)];
        const Vec3f &b = its.vertices[its.indices[facet_idx](1)];
        const Vec3f &c = its.vertices[its.indices[facet_idx](2)];
        Vec3f        n = (b - a).cross(c - a);
        if (n.z() > 0.9f * n.norm())
            selector.set_facet(facet_idx, EnforcerBlockerType(2));
    }
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
}

// Builds a 20mm cube (100 layers at 0.2mm) painted on the top faces and runs
// the full pipeline, then returns the by-extruder segmentation result.
std::vector<std::vector<ExPolygons>> slice_and_segment(int top_pen, int bottom_pen, double height = 20.)
{
    Model model;
    ModelObject *object = model.add_object();
    object->name = "paint-penetration-test.stl";
    ModelVolume *volume = object->add_volume(make_cube(20., 20., height));
    paint_top_faces(volume);
    object->add_instance();
    object->ensure_on_bed();

    Print print;
    print.set_status_silent();
    print.apply(model, two_filament_config(top_pen, bottom_pen));
    REQUIRE(print.objects().size() == 1);
    print.process();
    return multi_material_segmentation_by_painting(*print.objects()[0], [] {});
}

// Highest/lowest layer index in which the given extruder has a non-empty region.
std::pair<int, int> painted_layer_range(const std::vector<std::vector<ExPolygons>> &segmented, size_t extruder)
{
    int lowest = -1, highest = -1;
    REQUIRE(extruder < segmented.size());
    for (size_t layer_idx = 0; layer_idx < segmented[extruder].size(); ++layer_idx)
        if (!segmented[extruder][layer_idx].empty()) {
            if (lowest < 0) lowest = int(layer_idx);
            highest = int(layer_idx);
        }
    return {lowest, highest};
}

double painted_area(const std::vector<ExPolygons> &layers_by_extruder, size_t layer_idx)
{
    double area = 0.;
    for (const ExPolygon &expoly : layers_by_extruder[layer_idx])
        area += expoly.area();
    return area;
}

} // namespace

TEST_CASE("Paint penetration parameter definition", "[PaintPenetration]")
{
    SECTION("Defaults and range")
    {
        const ConfigOptionDef *def = print_config_def.get("top_color_penetration_layers");
        REQUIRE(def != nullptr);
        REQUIRE(def->type == coInt);
        REQUIRE(def->min == 0);
        REQUIRE(dynamic_cast<const ConfigOptionInt *>(def->get_default_value())->value == 5);

        const ConfigOptionDef *def_bottom = print_config_def.get("bottom_color_penetration_layers");
        REQUIRE(def_bottom != nullptr);
        REQUIRE(def_bottom->type == coInt);
        REQUIRE(def_bottom->min == 0);
        REQUIRE(dynamic_cast<const ConfigOptionInt *>(def_bottom->get_default_value())->value == 3);
    }

    SECTION("Round-trip through deserialization and PrintRegionConfig")
    {
        DynamicPrintConfig config;
        config.set_deserialize_strict("top_color_penetration_layers", "9", true);
        config.set_deserialize_strict("bottom_color_penetration_layers", "2", true);
        REQUIRE(config.opt_int("top_color_penetration_layers", 0) == 9);
        REQUIRE(config.opt_int("bottom_color_penetration_layers", 0) == 2);

        PrintRegionConfig region_config;
        region_config.top_color_penetration_layers.value    = 7;
        region_config.bottom_color_penetration_layers.value = 1;
        REQUIRE(region_config.top_color_penetration_layers.value == 7);
        REQUIRE(region_config.bottom_color_penetration_layers.value == 1);
    }
}

TEST_CASE("Paint penetration geometry", "[PaintPenetration]")
{
    // Top face of a 20mm cube at 0.2mm layers -> surface layer 99.
    SECTION("N=1 keeps color on the surface layer only")
    {
        auto segmented = slice_and_segment(1, 1);
        auto [lowest, highest] = painted_layer_range(segmented, 2);
        REQUIRE(highest == 99);
        REQUIRE(lowest >= 98); // tolerate one layer of projection boundary effect
    }

    SECTION("N=3 penetrates two layers below the surface layer")
    {
        auto segmented_1 = slice_and_segment(1, 1);
        auto segmented_3 = slice_and_segment(3, 1);
        auto [lowest_1, highest_1] = painted_layer_range(segmented_1, 2);
        auto [lowest_3, highest_3] = painted_layer_range(segmented_3, 2);
        REQUIRE(highest_3 == 99);
        REQUIRE(lowest_3 == lowest_1 - 2);
    }

    SECTION("Penetration layers shrink inwards monotonically")
    {
        auto segmented = slice_and_segment(3, 1);
        auto [lowest, highest] = painted_layer_range(segmented, 2);
        REQUIRE(highest - lowest >= 2);
        REQUIRE(painted_area(segmented[2], size_t(lowest)) < painted_area(segmented[2], size_t(highest)));
    }

    SECTION("N exceeding the total layer count is clamped safely")
    {
        auto segmented = slice_and_segment(1000, 1000);
        auto [lowest, highest] = painted_layer_range(segmented, 2);
        REQUIRE(lowest >= 0);
        REQUIRE(highest <= 99);
    }

    SECTION("N=0 behaves like N=1 (surface projection stays alive)")
    {
        auto segmented_0 = slice_and_segment(0, 0);
        auto segmented_1 = slice_and_segment(1, 1);
        REQUIRE(segmented_0.size() == segmented_1.size());
        auto [lowest_0, highest_0] = painted_layer_range(segmented_0, 2);
        auto [lowest_1, highest_1] = painted_layer_range(segmented_1, 2);
        REQUIRE(highest_0 == highest_1);
        REQUIRE(lowest_0 == lowest_1);
        for (size_t layer_idx = 0; layer_idx < segmented_0[2].size(); ++layer_idx)
            REQUIRE(std::abs(painted_area(segmented_0[2], layer_idx) - painted_area(segmented_1[2], layer_idx)) < 1e-6);
    }
}

TEST_CASE("Paint penetration validation", "[PaintPenetration][Print]")
{
    SECTION("Penetration exceeding the total layer count is an error with a jump-to key")
    {
        // 0.6mm cube at 0.2mm layers -> 3 layers total, penetration 5 must fail.
        Model model;
        ModelObject *object = model.add_object();
        object->name = "tiny-painted.stl";
        ModelVolume *volume = object->add_volume(make_cube(20., 20., 0.6));
        paint_top_faces(volume);
        object->add_instance();
        object->ensure_on_bed();

        Print print;
        print.set_status_silent();
        print.apply(model, two_filament_config(5, 5));
        REQUIRE(print.objects().size() == 1);
        auto error = print.validate();
        REQUIRE(error.string.find("paint penetration") != std::string::npos);
        REQUIRE(error.opt_key == "top_color_penetration_layers");
        REQUIRE(error.object != nullptr);
    }

    SECTION("Single-filament prints never trigger the penetration error")
    {
        Model model;
        ModelObject *object = model.add_object();
        object->name = "tiny-single.stl";
        ModelVolume *volume = object->add_volume(make_cube(20., 20., 0.6));
        paint_top_faces(volume);
        object->add_instance();
        object->ensure_on_bed();

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "layer_height",       "0.2" },
            { "first_layer_height", "0.2" },
            { "top_color_penetration_layers",    "5" },
            { "bottom_color_penetration_layers", "5" },
        });
        Print print;
        print.set_status_silent();
        print.apply(model, config);
        auto error = print.validate();
        REQUIRE(error.string.find("paint penetration") == std::string::npos);
    }
}

TEST_CASE("Single-filament slicing is unaffected by penetration parameters", "[PaintPenetration][Print]")
{
    Model model;
    ModelObject *object = model.add_object();
    object->name = "single-filament-painted.stl";
    ModelVolume *volume = object->add_volume(make_cube(20., 20., 20.));
    paint_top_faces(volume);
    object->add_instance();
    object->ensure_on_bed();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",       "0.2" },
        { "first_layer_height", "0.2" },
        { "top_color_penetration_layers",    "5" },
        { "bottom_color_penetration_layers", "3" },
    });
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    print.process();
    REQUIRE(print.objects()[0]->layers().size() == 100);
}
