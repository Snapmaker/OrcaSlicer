#include <catch2/catch.hpp>

#include <algorithm>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include <libslic3r/ModelArrange.hpp>

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// ORCA: tests for the per-extruder layer height feature ("extruder_layer_height").

template<typename PathFn>
static void for_each_path(const ExtrusionEntityCollection &collection, const PathFn &fn)
{
    for (const ExtrusionEntity *entity : collection.entities) {
        if (auto *sub_collection = dynamic_cast<const ExtrusionEntityCollection *>(entity))
            for_each_path(*sub_collection, fn);
        else if (auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
            for (const ExtrusionPath &path : loop->paths)
                fn(path);
        } else if (auto *multi_path = dynamic_cast<const ExtrusionMultiPath *>(entity)) {
            for (const ExtrusionPath &path : multi_path->paths)
                fn(path);
        } else if (auto *path = dynamic_cast<const ExtrusionPath *>(entity))
            fn(*path);
    }
}

static void collect_path_heights(const ExtrusionEntityCollection &collection, std::vector<float> &heights)
{
    for_each_path(collection, [&heights](const ExtrusionPath &path) { heights.emplace_back(path.height); });
}

static std::vector<float> region_path_heights(const LayerRegion *layerm)
{
    std::vector<float> heights;
    collect_path_heights(layerm->perimeters, heights);
    collect_path_heights(layerm->fills, heights);
    return heights;
}

static void collect_path_role_widths(const ExtrusionEntityCollection &collection, std::vector<std::pair<ExtrusionRole, float>> &widths)
{
    for_each_path(collection, [&widths](const ExtrusionPath &path) { widths.emplace_back(path.role(), path.width); });
}

// Two extruders: a 0.4 mm nozzle printing with the 0.2 mm object layer height and a 0.6 mm nozzle
// with a configurable extruder layer height (0.4 => multiplier 2).
// On this fork's classic multi-tool printers a filament index is the extruder index, so no
// filament map configuration applies.
static DynamicPrintConfig two_extruder_config(double second_extruder_layer_height)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("layer_height",               new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.4));
    config.set_key_value("enable_prime_tower",         new ConfigOptionBool(false));
    config.set_key_value("enable_support",             new ConfigOptionBool(false));

    config.set_key_value("nozzle_diameter",          new ConfigOptionFloats({0.4, 0.6}));
    config.set_key_value("extruder_layer_height",    new ConfigOptionFloats({0., second_extruder_layer_height}));
    config.set_key_value("min_layer_height",         new ConfigOptionFloats({0.07, 0.07}));
    config.set_key_value("max_layer_height",         new ConfigOptionFloats({0.3, 0.45}));
    config.set_key_value("filament_diameter",        new ConfigOptionFloats({1.75, 1.75}));
    config.set_key_value("filament_colour",          new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("filament_type",            new ConfigOptionStrings({"PLA", "PLA"}));
    config.set_key_value("default_filament_colour",  new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("nozzle_temperature",       new ConfigOptionInts({210, 210}));
    config.set_key_value("nozzle_temperature_range_low",  new ConfigOptionInts({190, 190}));
    config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts({240, 240}));
    // flush_volumes_matrix must be filament_count^2 entries.
    config.set_key_value("flush_multiplier",     new ConfigOptionFloat(1.));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats({0, 0, 0, 0}));
    // Print::validate() reports motion-ability diagnostics by overwriting the single warning
    // out-param; raise the machine limit so the default print accelerations do not clobber the
    // layer height warnings under test.
    config.set_key_value("machine_max_acceleration_extruding", new ConfigOptionFloats({100000., 100000.}));
    return config;
}

// One object made of two 20x20 mm cube parts side by side; the second part prints with filament 2.
// The parts are 20 * z_scale mm tall (10 mm by default).
static void init_two_part_print(Print &print, Model &model, const DynamicPrintConfig &config, float z_scale = 0.5f)
{
    TriangleMesh fine_mesh = mesh(TestMesh::cube_20x20x20);
    fine_mesh.scale(Vec3f(1.f, 1.f, z_scale));
    TriangleMesh coarse_mesh = fine_mesh;
    coarse_mesh.translate(30.f, 0.f, 0.f);

    ModelObject *object = model.add_object();
    object->name = "two_part_cube";
    object->add_volume(std::move(fine_mesh));
    ModelVolume *coarse_volume = object->add_volume(std::move(coarse_mesh));
    coarse_volume->config.set("extruder", 2);
    object->add_instance();

    arrange_objects(model, InfiniteBed{}, ArrangeParams{scaled(min_object_distance(config))});
    for (ModelObject *mo : model.objects)
        mo->ensure_on_bed();
    print.apply(model, config);
    print.set_status_silent();
}

static void find_regions(const PrintObject &object, int &fine_region, int &coarse_region)
{
    fine_region = coarse_region = -1;
    for (size_t i = 0; i < object.num_printing_regions(); ++ i) {
        if (object.printing_region(i).config().outer_wall_filament_id.value == 2)
            coarse_region = int(i);
        else
            fine_region = int(i);
    }
}

SCENARIO("Per-extruder layer height combines region layers", "[MultiNozzleLayerHeight]") {
    GIVEN("A two-part object, the second part on a 0.6 mm nozzle with a 0.4 mm extruder layer height") {
        DynamicPrintConfig config = two_extruder_config(0.4);
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("the configuration passes validation and slices as expected") {
            REQUIRE(print.validate().string.empty());
            print.process();

            const PrintObject &object = *print.objects().front();
            int fine_region, coarse_region;
            find_regions(object, fine_region, coarse_region);
            REQUIRE(fine_region >= 0);
            REQUIRE(coarse_region >= 0);
            // 0.4 mm first layer + 48 layers of 0.2 mm = 10 mm object height.
            REQUIRE(object.layer_count() == 49);

            // The coarse region extrudes on the first layer and then only on every 2nd layer,
            // always with 0.4 mm high paths; the layers in between print nothing for it.
            size_t coarse_layers = 0, coarse_bad_heights = 0, coarse_unexpected = 0, coarse_missing = 0;
            // The fine region extrudes on every layer with the base layer heights.
            size_t fine_bad_heights = 0, fine_missing = 0;
            for (size_t idx = 0; idx < object.layer_count(); ++ idx) {
                const Layer *layer = object.get_layer(int(idx));
                const std::vector<float> coarse_heights = region_path_heights(layer->get_region(coarse_region));
                if (idx % 2 == 0) {
                    if (coarse_heights.empty())
                        ++ coarse_missing;
                    else
                        ++ coarse_layers;
                    for (float height : coarse_heights)
                        if (std::abs(height - 0.4) > 1e-3)
                            ++ coarse_bad_heights;
                } else if (! coarse_heights.empty())
                    ++ coarse_unexpected;

                const std::vector<float> fine_heights = region_path_heights(layer->get_region(fine_region));
                if (fine_heights.empty())
                    ++ fine_missing;
                const double fine_expected = idx == 0 ? 0.4 : 0.2;
                for (float height : fine_heights)
                    if (std::abs(height - fine_expected) > 1e-3)
                        ++ fine_bad_heights;
            }
            CHECK(coarse_missing == 0);
            CHECK(coarse_unexpected == 0);
            CHECK(coarse_bad_heights == 0);
            CHECK(coarse_layers == 25);        // layer 0 + the 24 group tops
            CHECK(fine_missing == 0);
            CHECK(fine_bad_heights == 0);
        }
    }

    GIVEN("The same object with extruder_layer_height disabled") {
        DynamicPrintConfig config = two_extruder_config(0.);
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("both regions print on every layer with the base layer heights") {
            REQUIRE(print.validate().string.empty());
            print.process();

            const PrintObject &object = *print.objects().front();
            int fine_region, coarse_region;
            find_regions(object, fine_region, coarse_region);
            REQUIRE(fine_region >= 0);
            REQUIRE(coarse_region >= 0);

            size_t missing = 0, bad_heights = 0;
            for (size_t idx = 0; idx < object.layer_count(); ++ idx) {
                const Layer *layer = object.get_layer(int(idx));
                const double expected = idx == 0 ? 0.4 : 0.2;
                for (int region_id : { fine_region, coarse_region }) {
                    const std::vector<float> heights = region_path_heights(layer->get_region(region_id));
                    if (heights.empty())
                        ++ missing;
                    for (float height : heights)
                        if (std::abs(height - expected) > 1e-3)
                            ++ bad_heights;
                }
            }
            CHECK(missing == 0);
            CHECK(bad_heights == 0);
        }
    }
}

SCENARIO("Per-extruder layer height respects the extruder's minimum layer height", "[MultiNozzleLayerHeight]") {
    GIVEN("A 0.6 mm preferred layer height with a 0.4 mm minimum, on a part height leaving a 1-layer tail") {
        DynamicPrintConfig config = two_extruder_config(0.6);
        config.set_key_value("max_layer_height", new ConfigOptionFloats({0.3, 0.6}));
        config.set_key_value("min_layer_height", new ConfigOptionFloats({0.07, 0.4}));
        Print print;
        Model model;
        // 10.2 mm parts: a 0.4 mm first layer + 49 layers of 0.2 mm. 49 is not divisible by the
        // multiplier 3, so without the minimum the column would end in a single 0.2 mm layer.
        init_two_part_print(print, model, config, 0.51f);
        THEN("no layer of the coarse part above the first prints below 0.4 mm") {
            REQUIRE(print.validate().string.empty());
            print.process();

            const PrintObject &object = *print.objects().front();
            int fine_region, coarse_region;
            find_regions(object, fine_region, coarse_region);
            REQUIRE(coarse_region >= 0);

            size_t below_min = 0, full_runs = 0, forced_runs = 0;
            for (size_t idx = 1; idx < object.layer_count(); ++ idx) {
                const std::vector<float> heights = region_path_heights(object.get_layer(int(idx))->get_region(coarse_region));
                for (float height : heights) {
                    if (height < 0.4 - 1e-3)
                        ++ below_min;
                    else if (std::abs(height - 0.6) < 1e-3)
                        ++ full_runs;
                    else if (std::abs(height - 0.4) < 1e-3)
                        ++ forced_runs;
                }
            }
            CHECK(below_min == 0);
            CHECK(full_runs > 0);
            CHECK(forced_runs > 0);
        }
    }
    GIVEN("A preferred layer height below the extruder's minimum layer height") {
        DynamicPrintConfig config = two_extruder_config(0.4);
        config.set_key_value("min_layer_height", new ConfigOptionFloats({0.07, 0.45}));
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("validation fails") {
            const StringObjectException err = print.validate();
            REQUIRE(! err.string.empty());
            REQUIRE(err.opt_key == "extruder_layer_height");
        }
    }
}

// Shared check: the fine part's walls print on every layer with the base layer heights while
// combined 0.4 mm high infill appears on some layers above the first.
static void check_plain_walls_combined_infill(Print &print)
{
    REQUIRE(print.validate().string.empty());
    print.process();

    const PrintObject &object = *print.objects().front();
    int fine_region, coarse_region;
    find_regions(object, fine_region, coarse_region);
    REQUIRE(fine_region >= 0);

    size_t wall_bad_heights = 0, wall_missing = 0, combined_infill_paths = 0;
    for (size_t idx = 0; idx < object.layer_count(); ++ idx) {
        const LayerRegion *layerm = object.get_layer(int(idx))->get_region(fine_region);
        std::vector<float> wall_heights;
        collect_path_heights(layerm->perimeters, wall_heights);
        if (wall_heights.empty())
            ++ wall_missing;
        const double expected = idx == 0 ? 0.4 : 0.2;
        for (float height : wall_heights)
            if (std::abs(height - expected) > 1e-3)
                ++ wall_bad_heights;
        if (idx > 0) {
            std::vector<float> fill_heights;
            collect_path_heights(layerm->fills, fill_heights);
            for (float height : fill_heights)
                if (std::abs(height - 0.4) < 1e-3)
                    ++ combined_infill_paths;
        }
    }
    CHECK(wall_missing == 0);
    CHECK(wall_bad_heights == 0);
    CHECK(combined_infill_paths > 0);
}

SCENARIO("Per-extruder layer height honors feature filaments", "[MultiNozzleLayerHeight]") {
    GIVEN("A part whose sparse infill uses the filament with a 0.4 mm preferred layer height") {
        DynamicPrintConfig config = two_extruder_config(0.4);
        // Both parts print their sparse infill with filament 2; the first part's walls stay on
        // filament 1 with no preferred layer height.
        config.set_key_value("sparse_infill_filament_id", new ConfigOptionInt(2));
        config.set_key_value("sparse_infill_density",     new ConfigOptionPercent(15));
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("the first part prints per-layer walls with sparse infill combined to 0.4 mm") {
            check_plain_walls_combined_infill(print);
        }
    }

    GIVEN("A part whose internal solid infill uses the 0.4 mm filament at 100% infill density") {
        DynamicPrintConfig config = two_extruder_config(0.4);
        // At 100% density the combined infill is internal solid infill: the preference of ITS
        // filament must decide the combined height, not the sparse infill filament's.
        config.set_key_value("internal_solid_filament_id", new ConfigOptionInt(2));
        config.set_key_value("sparse_infill_density",      new ConfigOptionPercent(100));
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("the first part prints per-layer walls with solid infill combined to 0.4 mm") {
            check_plain_walls_combined_infill(print);
        }
    }

    GIVEN("A part whose outer walls use a filament with a different preferred layer height") {
        DynamicPrintConfig config = two_extruder_config(0.4);
        // The first part's outer walls print with filament 2 while its inner walls stay on
        // filament 1: walls print together, so the part keeps the object layer height and the
        // unhonored preference is warned about.
        config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(2));
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("validation passes with a warning instead of an error") {
            // This fork's validate() appends warnings to a single StringObjectException out-param.
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            REQUIRE(! warning.string.empty());
            REQUIRE(warning.opt_key == "extruder_layer_height");
        }
    }
}

SCENARIO("Fill line width follows the filament that prints the surface", "[MultiNozzleLayerHeight]") {
    GIVEN("Internal solid infill mapped to the 0.6 mm filament, bottom surfaces staying on the 0.4 mm filament") {
        DynamicPrintConfig config = two_extruder_config(0.);
        config.set_key_value("internal_solid_filament_id",       new ConfigOptionInt(2));
        config.set_key_value("initial_layer_line_width",         new ConfigOptionFloatOrPercent(125., true));
        config.set_key_value("internal_solid_infill_line_width", new ConfigOptionFloatOrPercent(105., true));
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("bottom surface widths resolve against their own filament's nozzle") {
            REQUIRE(print.validate().string.empty());
            print.process();

            const PrintObject &object = *print.objects().front();
            int fine_region, coarse_region;
            find_regions(object, fine_region, coarse_region);
            REQUIRE(fine_region >= 0);

            // Bottom surfaces print with filament 1, so their percent line width resolves against
            // its 0.4 mm nozzle, while internal solid infill (filament 2) resolves against 0.6 mm.
            // Solid fills may stretch line spacing up to 20% to fit a region evenly, so accept
            // widths in [nominal, 1.2 * nominal].
            size_t bottom_paths = 0, bottom_bad_widths = 0, solid_paths = 0, solid_bad_widths = 0;
            for (size_t idx = 0; idx < object.layer_count(); ++ idx) {
                std::vector<std::pair<ExtrusionRole, float>> widths;
                collect_path_role_widths(object.get_layer(int(idx))->get_region(fine_region)->fills, widths);
                for (const std::pair<ExtrusionRole, float> &role_width : widths) {
                    const bool bottom = role_width.first == erBottomSurface;
                    if (! bottom && role_width.first != erSolidInfill)
                        continue;
                    ++ (bottom ? bottom_paths : solid_paths);
                    const double expected = (idx == 0 ? 1.25 : 1.05) * (bottom ? 0.4 : 0.6);
                    if (role_width.second < expected - 1e-3 || role_width.second > expected * 1.2 + 1e-3)
                        ++ (bottom ? bottom_bad_widths : solid_bad_widths);
                }
            }
            CHECK(bottom_paths > 0);
            CHECK(bottom_bad_widths == 0);
            CHECK(solid_paths > 0);
            CHECK(solid_bad_widths == 0);
        }
    }
}

SCENARIO("Combined infill respects the printing extruder's layer height limits", "[MultiNozzleLayerHeight]") {
    GIVEN("Infill combining to a preferred height above the filament's maximum layer height") {
        // Infill on filament 2: preferred layer height 0.6 exceeds its max_layer_height 0.45,
        // so combining must stop at 0.4 mm groups instead of building 0.6 mm ones (3 x 0.2).
        DynamicPrintConfig config = two_extruder_config(0.6);
        config.set_key_value("max_layer_height",           new ConfigOptionFloats({0.3, 0.45}));
        config.set_key_value("sparse_infill_filament_id",  new ConfigOptionInt(2));
        config.set_key_value("internal_solid_filament_id", new ConfigOptionInt(2));
        config.set_key_value("sparse_infill_density",      new ConfigOptionPercent(15));
        Print print;
        Model model;
        // A single part: filament 2 prints only infill, so its preference skips the strict wall checks.
        TriangleMesh cube = mesh(TestMesh::cube_20x20x20);
        cube.scale(Vec3f(1.f, 1.f, 0.5f));
        ModelObject *object_model = model.add_object();
        object_model->name = "single_cube";
        object_model->add_volume(std::move(cube));
        object_model->add_instance();
        arrange_objects(model, InfiniteBed{}, ArrangeParams{scaled(min_object_distance(config))});
        for (ModelObject *mo : model.objects)
            mo->ensure_on_bed();
        print.apply(model, config);
        print.set_status_silent();
        THEN("no combined infill group exceeds the maximum layer height") {
            REQUIRE(print.validate().string.empty());
            print.process();

            const PrintObject &object = *print.objects().front();
            size_t over_max = 0, combined = 0;
            for (size_t idx = 1; idx < object.layer_count(); ++ idx)
                for (const LayerRegion *layerm : object.get_layer(int(idx))->regions()) {
                    std::vector<float> heights;
                    collect_path_heights(layerm->fills, heights);
                    for (float height : heights) {
                        if (height > 0.45 + 1e-3)
                            ++ over_max;
                        else if (height > 0.2 + 1e-3)
                            ++ combined;
                    }
                }
            CHECK(over_max == 0);
            CHECK(combined > 0);
        }
    }

    GIVEN("Walls combining to a pitch a feature filament of the part cannot print") {
        // Both wall filaments map to filament 2 at a 0.4 mm pitch, but top/bottom/solid features
        // stay on filament 1 whose max_layer_height (0.3) cannot print that pitch: the part
        // falls back to the object layer height with a warning, like disagreeing wall filaments.
        DynamicPrintConfig config = two_extruder_config(0.4);
        config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(2));
        config.set_key_value("inner_wall_filament_id", new ConfigOptionInt(2));
        config.set_key_value("max_layer_height",       new ConfigOptionFloats({0.3, 0.45}));
        // Keep the line width checks out of the way, this test targets the height fallback.
        config.set_key_value("line_width",             new ConfigOptionFloatOrPercent(0.5, false));
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("validation warns and the part prints with the object layer height") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            REQUIRE(warning.string.find("cannot print that height") != std::string::npos);

            print.process();
            // Only the first part falls back; the second is all filament 2 and keeps the pitch.
            // Both parts' walls print with filament 2, so the parts are told apart by their
            // top surface filament.
            const PrintObject &object = *print.objects().front();
            int fine_region = -1;
            for (size_t i = 0; i < object.num_printing_regions(); ++ i)
                if (object.printing_region(i).config().top_surface_filament_id.value == 1)
                    fine_region = int(i);
            REQUIRE(fine_region >= 0);
            size_t wall_bad_heights = 0;
            for (size_t idx = 0; idx < object.layer_count(); ++ idx) {
                const double expected = idx == 0 ? 0.4 : 0.2;
                std::vector<float> heights;
                collect_path_heights(object.get_layer(int(idx))->get_region(fine_region)->perimeters, heights);
                for (float height : heights)
                    if (std::abs(height - expected) > 1e-3)
                        ++ wall_bad_heights;
            }
            CHECK(wall_bad_heights == 0);
        }
    }

    GIVEN("A feature filament whose minimum layer height is above the pitch it prints at") {
        // Internal solid infill on filament 2 (minimum layer height 0.3) prints with the 0.2 mm
        // object layer height while filament 2's preference drives infill combining elsewhere.
        DynamicPrintConfig config = two_extruder_config(0.4);
        config.set_key_value("internal_solid_filament_id", new ConfigOptionInt(2));
        config.set_key_value("min_layer_height",           new ConfigOptionFloats({0.07, 0.3}));
        // Keep the line width checks out of the way, this test targets the height warning.
        config.set_key_value("line_width",                 new ConfigOptionFloatOrPercent(0.5, false));
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("validation warns about printing below the minimum layer height") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            REQUIRE(warning.string.find("minimum layer") != std::string::npos);
        }
    }
}

SCENARIO("Fill collections dispatch to the filament their flow was computed for", "[MultiNozzleLayerHeight]") {
    GIVEN("A region with distinct per-feature filaments") {
        PrintRegionConfig region_config;
        region_config.outer_wall_filament_id.value     = 1;
        region_config.inner_wall_filament_id.value     = 1;
        region_config.sparse_infill_filament_id.value  = 4;
        region_config.internal_solid_filament_id.value = 3;
        region_config.top_surface_filament_id.value    = 1;
        region_config.bottom_surface_filament_id.value = 1;
        const PrintRegion region(region_config, region_config.hash(), 0);
        LayerTools layer_tools(0.);

        auto collection_extruder = [&](std::initializer_list<ExtrusionRole> roles) {
            ExtrusionEntityCollection eec;
            for (ExtrusionRole role : roles)
                eec.entities.push_back(new ExtrusionPath(role));
            return layer_tools.extruder(eec, region);
        };

        THEN("top and bottom surfaces keep their filament when gap fill is mixed in") {
            CHECK(collection_extruder({erTopSolidInfill})            == 0); // 0 based filament 1
            CHECK(collection_extruder({erTopSolidInfill, erGapFill}) == 0);
            CHECK(collection_extruder({erBottomSurface, erGapFill})  == 0);
            CHECK(collection_extruder({erSolidInfill, erGapFill})    == 2); // filament 3
        }
        THEN("external bridges print with the bottom surface filament, internal ones stay solid") {
            CHECK(collection_extruder({erBridgeInfill})         == 0); // bottom filament 1
            CHECK(collection_extruder({erInternalBridgeInfill}) == 2); // internal solid filament 3
        }
        THEN("gap fill with no sibling surface prints with the outer wall filament") {
            PrintRegionConfig gap_region_config = region_config;
            gap_region_config.outer_wall_filament_id.value = 2;
            const PrintRegion gap_region(gap_region_config, gap_region_config.hash(), 0);
            ExtrusionEntityCollection eec;
            eec.entities.push_back(new ExtrusionPath(erGapFill));
            CHECK(layer_tools.extruder(eec, gap_region) == 1); // 0 based outer wall filament 2
        }
    }
}

SCENARIO("Support nozzle diameter restricts support printing", "[MultiNozzleLayerHeight]") {
    // A raft makes the object require support handling without any overhang geometry: the raft
    // layers below the object print as support-only layers.
    auto raft_config = [](double support_nozzle_diameter) {
        DynamicPrintConfig config = two_extruder_config(0.);
        config.set_key_value("raft_layers",             new ConfigOptionInt(2));
        config.option<ConfigOptionEnum<SupportType>>("support_type", true)->value = stNormalAuto;
        config.set_key_value("support_nozzle_diameter", new ConfigOptionFloat(support_nozzle_diameter));
        config.set_key_value("support_line_width",      new ConfigOptionFloatOrPercent(105., true));
        return config;
    };

    GIVEN("A raft restricted to the 0.6 mm nozzle while the default filament prints with 0.4 mm") {
        DynamicPrintConfig config = raft_config(0.6);
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("support flows, layer height limits and tool ordering follow the 0.6 mm filament") {
            REQUIRE(print.validate().string.empty());

            const PrintObject &object = *print.objects().front();
            // Support / raft flows must resolve width against the restricted nozzle, not against
            // extruder 1 that the "default" support filament falls back to.
            REQUIRE(double(support_material_flow(&object).width()) == Approx(1.05 * 0.6).margin(1e-4));
            REQUIRE(double(support_material_interface_flow(&object).width()) == Approx(1.05 * 0.6).margin(1e-4));

            // Support layer height limits follow the restricted nozzle (filament 2: max 0.45).
            PrintConfig print_config;
            print_config.apply(config, true);
            PrintObjectConfig object_config;
            object_config.apply(config, true);
            const SlicingParameters params = SlicingParameters::create_from_config(
                print_config, object_config, 10., std::vector<unsigned int>{0, 1}, Vec3d(1., 1., 1.));
            REQUIRE(params.max_suport_layer_height == Approx(0.45).margin(1e-6));

            // The raft layers below the object print with filament 2 only.
            print.process();
            ToolOrdering tool_ordering(print, (unsigned int)-1, false);
            size_t support_only_layers = 0, wrong_extruders = 0;
            for (const LayerTools &layer_tools : tool_ordering.layer_tools())
                if (layer_tools.has_support && ! layer_tools.has_object) {
                    ++ support_only_layers;
                    for (unsigned int extruder_id : layer_tools.extruders)
                        if (extruder_id != 1) // 0 based: filament 2 prints with the 0.6 mm nozzle
                            ++ wrong_extruders;
                }
            CHECK(support_only_layers > 0);
            CHECK(wrong_extruders == 0);
        }
    }

    GIVEN("A support nozzle diameter no extruder has") {
        DynamicPrintConfig config = raft_config(0.5);
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("validation fails") {
            const StringObjectException err = print.validate();
            REQUIRE(! err.string.empty());
            REQUIRE(err.opt_key == "support_nozzle_diameter");
        }
    }

    GIVEN("A support filament printing with a different nozzle than the support nozzle diameter") {
        DynamicPrintConfig config = raft_config(0.6);
        config.set_key_value("support_filament", new ConfigOptionInt(1));
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("validation fails") {
            const StringObjectException err = print.validate();
            REQUIRE(! err.string.empty());
            REQUIRE(err.opt_key == "support_filament");
        }
    }
}

SCENARIO("A raft keeps the bottom surfaces of combined regions", "[MultiNozzleLayerHeight]") {
    GIVEN("A combined region printing on a raft") {
        DynamicPrintConfig config = two_extruder_config(0.4);
        config.set_key_value("raft_layers", new ConfigOptionInt(2));
        config.option<ConfigOptionEnum<SupportType>>("support_type", true)->value = stNormalAuto;
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("the first layer above the raft prints uncombined and carries the bottom surfaces") {
            REQUIRE(print.validate().string.empty());
            print.process();

            const PrintObject &object = *print.objects().front();
            int fine_region, coarse_region;
            find_regions(object, fine_region, coarse_region);
            REQUIRE(fine_region >= 0);
            REQUIRE(coarse_region >= 0);

            // The first layer above the raft must not be swallowed by a combined group: surface
            // detection can only seed the object's bottom shells there.
            const Layer *first_layer = object.get_layer(0);
            size_t bottom_regions = 0;
            for (int region_id : { fine_region, coarse_region }) {
                const LayerRegion *layerm = first_layer->get_region(region_id);
                const std::vector<float> heights = region_path_heights(layerm);
                CHECK(! heights.empty());
                for (float height : heights)
                    CHECK(double(height) == Approx(0.2).margin(1e-3));
                for (const Surface &surface : layerm->fill_surfaces.surfaces)
                    if (surface.is_bottom()) {
                        ++ bottom_regions;
                        break;
                    }
            }
            CHECK(bottom_regions == 2);

            // Combining still happens above the first object layer.
            size_t combined_layers = 0;
            for (size_t idx = 1; idx < object.layer_count(); ++ idx)
                for (float height : region_path_heights(object.get_layer(int(idx))->get_region(coarse_region)))
                    if (std::abs(height - 0.4) < 1e-3) {
                        ++ combined_layers;
                        break;
                    }
            CHECK(combined_layers > 0);
        }
    }
}

SCENARIO("Per-extruder layer height validation rejects invalid configurations", "[MultiNozzleLayerHeight]") {
    auto expect_error = [](double second_extruder_layer_height) {
        DynamicPrintConfig config = two_extruder_config(second_extruder_layer_height);
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        const StringObjectException err = print.validate();
        REQUIRE(! err.string.empty());
        REQUIRE(err.opt_key == "extruder_layer_height");
    };
    GIVEN("An extruder layer height that is no integer multiple of the object layer height") {
        THEN("validation fails") { expect_error(0.5); }
    }
    GIVEN("An extruder layer height smaller than the object layer height") {
        THEN("validation fails") { expect_error(0.1); }
    }
    GIVEN("An extruder layer height exceeding the nozzle diameter") {
        THEN("validation fails") { expect_error(0.8); }
    }
    GIVEN("An extruder layer height exceeding the extruder's maximum layer height") {
        // 0.6 is a multiple of 0.2 and fits the 0.6 mm nozzle, but exceeds max_layer_height 0.45.
        THEN("validation fails") { expect_error(0.6); }
    }
}
