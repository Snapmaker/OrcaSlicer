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
    for_each_path(collection, [&heights](const ExtrusionPath &path) {
        // Bridges print with the bridge flow whose height derives from the nozzle,
        // not from the layer height; they are not this feature's concern.
        if (path.role() != erBridgeInfill && path.role() != erInternalBridgeInfill)
            heights.emplace_back(path.height);
    });
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
    // The default G-code flavor rejects relative extruder addressing without a G92 E0 layer-change
    // reset; this suite does not exercise the G-code writer, keep validation quiet.
    config.set_key_value("use_relative_e_distances", new ConfigOptionBool(false));
    // The scenarios build on consistent-mode expectations with a tight drift tolerance; the
    // shipping defaults are fixed mode with a generous tolerance.
    config.option<ConfigOptionEnum<ExtruderLayerHeightMode>>("extruder_layer_height_mode", true)->value = elhmConsistent;
    config.set_key_value("extruder_layer_height_tolerance", new ConfigOptionPercent(10));
    return config;
}

// One object made of two 20x20 mm parts side by side; the second part prints with filament 2.
// The parts are z-scaled by z_scale (cubes are 10 mm tall by default).
static void init_two_part_print(Print &print, Model &model, const DynamicPrintConfig &config, float z_scale = 0.5f,
                                TestMesh coarse_shape = TestMesh::cube_20x20x20)
{
    TriangleMesh fine_mesh = mesh(TestMesh::cube_20x20x20);
    fine_mesh.scale(Vec3f(1.f, 1.f, z_scale));
    TriangleMesh coarse_mesh = mesh(coarse_shape);
    coarse_mesh.scale(Vec3f(1.f, 1.f, z_scale));
    coarse_mesh.translate(30.f, 0.f, 0.f);

    ModelObject *object = model.add_object();
    object->name = "two_part_cube";
    object->add_volume(std::move(fine_mesh));
    ModelVolume *coarse_volume = object->add_volume(std::move(coarse_mesh));
    coarse_volume->config.set("extruder", 2);
    object->add_instance();

    // This fork's arrangement engine rejects positions outside the (unset) plate even for an
    // InfiniteBed; the fixture geometry is already laid out, so place it at a fixed bed spot.
    for (ModelObject *mo : model.objects) {
        mo->center_around_origin();
        mo->translate(120., 120., 0.);
        mo->ensure_on_bed();
    }
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

SCENARIO("Fixed mode always prints the extruder layer height", "[MultiNozzleLayerHeight]") {
    // A 10 mm tall pyramid on the coarse extruder: its outline drifts by 0.2 mm per edge on every
    // 0.2 mm layer, far past the suite's 10 % thick layer tolerance (of the 0.6 mm nozzle), so
    // consistent mode falls back to the object layer height everywhere.
    GIVEN("A pyramid part whose outline drifts past the thick layer tolerance on every layer") {
        DynamicPrintConfig config = two_extruder_config(0.4);
        // Count coarse extrusion heights well below the apex, where slices stay large enough to print.
        auto coarse_height_counts = [](Print &print, size_t &at_pitch, size_t &at_base, size_t &odd_layers) {
            print.process();
            const PrintObject &object = *print.objects().front();
            int fine_region, coarse_region;
            find_regions(object, fine_region, coarse_region);
            REQUIRE(coarse_region >= 0);
            at_pitch = at_base = odd_layers = 0;
            for (size_t idx = 1; idx <= 40; ++ idx) {
                const std::vector<float> heights = region_path_heights(object.get_layer(int(idx))->get_region(coarse_region));
                if (idx % 2 == 1 && ! heights.empty())
                    ++ odd_layers;
                for (float height : heights) {
                    if (std::abs(height - 0.4) < 1e-3)
                        ++ at_pitch;
                    else if (std::abs(height - 0.2) < 1e-3)
                        ++ at_base;
                }
            }
        };
        WHEN("thick layer regions are Consistent") {
            Print print;
            Model model;
            init_two_part_print(print, model, config, 0.25f, TestMesh::pyramid);
            THEN("the drift keeps the coarse part at the object layer height") {
                REQUIRE(print.validate().string.empty());
                size_t at_pitch, at_base, odd_layers;
                coarse_height_counts(print, at_pitch, at_base, odd_layers);
                CHECK(at_pitch == 0);
                CHECK(at_base > 0);
                CHECK(odd_layers == 20);
            }
        }
        WHEN("thick layer regions are Fixed") {
            config.option<ConfigOptionEnum<ExtruderLayerHeightMode>>("extruder_layer_height_mode", true)->value = elhmFixed;
            Print print;
            Model model;
            init_two_part_print(print, model, config, 0.25f, TestMesh::pyramid);
            THEN("every coarse extrusion above the first layer keeps the extruder layer height") {
                REQUIRE(print.validate().string.empty());
                size_t at_pitch, at_base, odd_layers;
                coarse_height_counts(print, at_pitch, at_base, odd_layers);
                CHECK(at_pitch > 0);
                CHECK(at_base == 0);
                CHECK(odd_layers == 0);
            }
        }
    }
}

SCENARIO("Fixed mode keeps top surfaces on combined steps", "[MultiNozzleLayerHeight]") {
    // A shoulder that ends mid-run: the wide cube's exposed ring is combined away with its layer
    // (the run prints only the common shape), so it must reappear as a top surface on the printed
    // layer below it - otherwise the step carries bare sparse infill.
    GIVEN("A wide cube ending mid-run with a narrower cube on top") {
        auto top_area = [](ExtruderLayerHeightMode mode) {
            DynamicPrintConfig config = two_extruder_config(0.4);
            config.option<ConfigOptionEnum<ExtruderLayerHeightMode>>("extruder_layer_height_mode", true)->value = mode;
            Print print;
            Model model;
            TriangleMesh base = mesh(TestMesh::cube_20x20x20);
            base.scale(Vec3f(1.f, 1.f, 0.25f));                  // 5 mm tall: the shoulder ends mid-run
            TriangleMesh boss = mesh(TestMesh::cube_20x20x20);
            boss.scale(Vec3f(0.5f, 0.5f, 0.1f));                 // 10 x 10 x 2 mm on top
            boss.translate(5.f, 5.f, 5.f);
            ModelObject *object = model.add_object();
            object->name = "stepped_cube";
            object->add_volume(std::move(base));
            object->add_volume(std::move(boss));
            object->config.set("extruder", 2);
            object->add_instance();
            // This fork's arrangement engine rejects positions outside the (unset) plate even for
            // an InfiniteBed; place the object at a fixed bed spot like init_two_part_print().
            for (ModelObject *mo : model.objects) {
                mo->center_around_origin();
                mo->translate(120., 120., 0.);
                mo->ensure_on_bed();
            }
            print.apply(model, config);
            print.set_status_silent();
            REQUIRE(print.validate().string.empty());
            print.process();
            const PrintObject &po = *print.objects().front();
            double area = 0.;
            for (size_t idx = 0; idx < po.layer_count(); ++ idx)
                for (int r = 0; r < po.get_layer(int(idx))->region_count(); ++ r)
                    for (const Surface &surface : po.get_layer(int(idx))->get_region(r)->fill_surfaces.surfaces)
                        if (surface.surface_type == stTop)
                            area += unscale<double>(unscale<double>(surface.expolygon.area()));
            return area;
        };
        THEN("the combined print keeps most of the per-layer top surface area") {
            const double per_layer = top_area(elhmConsistent);
            const double combined  = top_area(elhmFixed);
            CAPTURE(per_layer, combined);
            REQUIRE(per_layer > 100.);
            REQUIRE(combined > 0.7 * per_layer);
        }
    }
}

SCENARIO("Fixed mode bridges lids that start inside a run", "[MultiNozzleLayerHeight]") {
    // The runs spanning the lid's first layers print nothing there; it must still bridge.
    GIVEN("A hollow tube capped by a lid whose bottom starts in the middle of a run") {
        DynamicPrintConfig config = two_extruder_config(0.4);
        config.option<ConfigOptionEnum<ExtruderLayerHeightMode>>("extruder_layer_height_mode", true)->value = elhmFixed;
        Print print;
        Model model;
        TriangleMesh tube = mesh(TestMesh::cube_with_hole);   // 20 x 20 x 10 mm, 10 mm hole through z
        tube.scale(Vec3f(1.f, 1.f, 0.5f));                    // 5 mm tall: the lid starts mid-run
        TriangleMesh lid = mesh(TestMesh::cube_20x20x20);
        lid.scale(Vec3f(1.f, 1.f, 0.1f));
        lid.translate(0.f, 0.f, 5.f);
        ModelObject *object = model.add_object();
        object->name = "capped_tube";
        object->add_volume(std::move(tube));
        object->add_volume(std::move(lid));
        object->config.set("extruder", 2);
        object->add_instance();
        // This fork's arrangement engine rejects positions outside the (unset) plate even for
        // an InfiniteBed; place the object at a fixed bed spot like init_two_part_print().
        for (ModelObject *mo : model.objects) {
            mo->center_around_origin();
            mo->translate(120., 120., 0.);
            mo->ensure_on_bed();
        }
        print.apply(model, config);
        print.set_status_silent();
        THEN("the lid interior is classified as an unsupported bottom") {
            REQUIRE(print.validate().string.empty());
            print.process();
            const PrintObject &po = *print.objects().front();
            double bridge_area = 0.;
            for (size_t idx = 0; idx < po.layer_count(); ++ idx)
                for (int r = 0; r < po.get_layer(int(idx))->region_count(); ++ r)
                    for (const Surface &surface : po.get_layer(int(idx))->get_region(r)->fill_surfaces.surfaces)
                        if (surface.surface_type == stBottomBridge)
                            bridge_area += unscale<double>(unscale<double>(surface.expolygon.area()));
            CAPTURE(bridge_area);
            REQUIRE(bridge_area > 40.);
        }
    }
}

SCENARIO("A bottom over another region's combined-away geometry bridges", "[MultiNozzleLayerHeight]") {
    // At a filament boundary the support below a region's bottom can belong to a neighboring
    // region whose run commits only its common shape: that support never prints even though the
    // object's slices still cover the area, so the bottom must classify as an unsupported bridge.
    GIVEN("A coarse slab whose top layer is a pocket rim holding a fine-filament insert") {
        DynamicPrintConfig config = two_extruder_config(0.4);
        config.option<ConfigOptionEnum<ExtruderLayerHeightMode>>("extruder_layer_height_mode", true)->value = elhmFixed;
        Print print;
        Model model;
        // One coarse volume: a full slab plus a one-layer pocket rim on top. The run pairing the
        // slab's top layer with the rim commits only the rim and drops the pocket footprint.
        TriangleMesh slab = mesh(TestMesh::cube_20x20x20);
        slab.scale(Vec3f(1.f, 1.f, 0.07f));                  // 20 x 20 x 1.4 mm
        TriangleMesh rim = mesh(TestMesh::cube_with_hole);   // 20 x 20, 10 mm hole
        rim.scale(Vec3f(1.f, 1.f, 0.02f));                   // one 0.2 mm layer
        rim.translate(0.f, 0.f, 1.4f);
        slab.merge(rim);
        TriangleMesh insert = mesh(TestMesh::cube_20x20x20);
        insert.scale(Vec3f(0.4f, 0.4f, 0.04f));              // 8 x 8 x 0.8 mm in the pocket
        insert.translate(6.f, 6.f, 1.4f);
        ModelObject *object = model.add_object();
        object->name = "pocketed_slab";
        ModelVolume *coarse_volume = object->add_volume(std::move(slab));
        coarse_volume->config.set("extruder", 2);
        object->add_volume(std::move(insert));
        object->add_instance();
        // This fork's arrangement engine rejects positions outside the (unset) plate even for
        // an InfiniteBed; place the object at a fixed bed spot like init_two_part_print().
        for (ModelObject *mo : model.objects) {
            mo->center_around_origin();
            mo->translate(120., 120., 0.);
            mo->ensure_on_bed();
        }
        print.apply(model, config);
        print.set_status_silent();
        THEN("the insert's first layer is classified as an unsupported bottom") {
            REQUIRE(print.validate().string.empty());
            print.process();
            const PrintObject &po = *print.objects().front();
            int fine_region = -1, coarse_region = -1;
            find_regions(po, fine_region, coarse_region);
            REQUIRE(fine_region >= 0);
            double bridge_area = 0.;
            for (size_t idx = 0; idx < po.layer_count(); ++ idx)
                if (const Layer *layer = po.get_layer(int(idx)); fine_region < layer->region_count())
                    for (const Surface &surface : layer->get_region(fine_region)->fill_surfaces.surfaces)
                        if (surface.surface_type == stBottomBridge)
                            bridge_area += unscale<double>(unscale<double>(surface.expolygon.area()));
            CAPTURE(bridge_area);
            REQUIRE(bridge_area > 30.);
        }
    }
}

SCENARIO("A floating insert below its covering run is filled by the run and resumes on top", "[MultiNozzleLayerHeight]") {
    // Geometry whose covering run commits above it would print into thin air before any support
    // exists: its floating layers are dropped, the covering run's pass fills the object volume
    // they occupied, and the region resumes fully supported on top of the pass.
    GIVEN("A hollow tube capped by a pocketed coarse roof with a fine insert starting mid-run") {
        DynamicPrintConfig config = two_extruder_config(0.6);
        config.set_key_value("max_layer_height", new ConfigOptionFloats({0.3, 0.6}));
        config.option<ConfigOptionEnum<ExtruderLayerHeightMode>>("extruder_layer_height_mode", true)->value = elhmFixed;
        Print print;
        Model model;
        // Coarse volume: tube walls, a full roof slab over the cavity, and a one-run pocket rim
        // above it. The run pairs the slab with the rim and drops the slab's pocket footprint.
        TriangleMesh roof = mesh(TestMesh::cube_with_hole);   // 20 x 20, 10 mm hole
        roof.scale(Vec3f(1.f, 1.f, 0.1f));                    // 1 mm tall tube
        TriangleMesh slab = mesh(TestMesh::cube_20x20x20);
        slab.scale(Vec3f(1.f, 1.f, 0.01f));                   // 20 x 20 x 0.2 mm
        slab.translate(0.f, 0.f, 1.f);
        roof.merge(slab);
        const float rim_dims[4][4] = {{0.3f, 1.f, 0.f, 0.f}, {0.3f, 1.f, 14.f, 0.f},
                                      {0.4f, 0.3f, 6.f, 0.f}, {0.4f, 0.3f, 6.f, 14.f}};
        for (const auto &d : rim_dims) {
            TriangleMesh rim = mesh(TestMesh::cube_20x20x20);
            rim.scale(Vec3f(d[0], d[1], 0.02f));              // rim pieces around an 8 x 8 pocket
            rim.translate(d[2], d[3], 1.2f);
            roof.merge(rim);
        }
        TriangleMesh insert = mesh(TestMesh::cube_20x20x20);
        insert.scale(Vec3f(0.4f, 0.4f, 0.04f));               // 8 x 8 x 0.8 mm in the pocket
        insert.translate(6.f, 6.f, 1.2f);
        ModelObject *object = model.add_object();
        object->name = "pocketed_roof";
        ModelVolume *coarse_volume = object->add_volume(std::move(roof));
        coarse_volume->config.set("extruder", 2);
        object->add_volume(std::move(insert));
        object->add_instance();
        // This fork's arrangement engine rejects positions outside the (unset) plate even for
        // an InfiniteBed; place the object at a fixed bed spot like init_two_part_print().
        for (ModelObject *mo : model.objects) {
            mo->center_around_origin();
            mo->translate(120., 120., 0.);
            mo->ensure_on_bed();
        }
        print.apply(model, config);
        print.set_status_silent();
        THEN("the covering run fills the floating layers and the insert resumes on top") {
            REQUIRE(print.validate().string.empty());
            print.process();
            const PrintObject &po = *print.objects().front();
            int fine_region = -1, coarse_region = -1;
            find_regions(po, fine_region, coarse_region);
            REQUIRE(fine_region >= 0);
            REQUIRE(coarse_region >= 0);
            double floating_area = 0., filled_bridge_area = 0., resumed_area = 0.;
            for (size_t idx = 0; idx < po.layer_count(); ++ idx) {
                const Layer *layer = po.get_layer(int(idx));
                for (int r = 0; r < layer->region_count(); ++ r)
                    for (const Surface &surface : layer->get_region(r)->fill_surfaces.surfaces) {
                        const double area = unscale<double>(unscale<double>(surface.expolygon.area()));
                        if (r == fine_region && layer->print_z < 1.7)
                            floating_area += area;
                        else if (r == coarse_region && std::abs(layer->print_z - 1.6) < EPSILON && surface.surface_type == stBottomBridge)
                            filled_bridge_area += area;
                        else if (r == fine_region && std::abs(layer->print_z - 1.8) < EPSILON)
                            resumed_area += area;
                    }
            }
            CAPTURE(floating_area, filled_bridge_area, resumed_area);
            REQUIRE(floating_area < 0.1);       // nothing of the insert prints in the air
            REQUIRE(filled_bridge_area > 60.);  // the coarse pass bridges rim and pocket alike
            REQUIRE(resumed_area > 25.);        // the insert continues on top of the pass
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
        THEN("validation warns instead of rejecting: the minimum is a soft profile limit") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            REQUIRE(warning.string.find("minimum layer") != std::string::npos);
            REQUIRE(warning.opt_key == "extruder_layer_height");
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

    GIVEN("A part whose 100% density solid infill prints with the preferred-height filament") {
        DynamicPrintConfig config = two_extruder_config(0.4);
        // At 100% density the combined infill is internal solid infill printed with the INTERNAL
        // SOLID filament (PrintRegion::extruder()), so that filament's preference must decide the
        // combined height.
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
        // filament 1 ("Default", no preferred height): the explicit preference sets the part's
        // pitch and the no-preference filaments follow it instead of vetoing it.
        config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(2));
        // Keep the combined-region line width checks out of the way, this test targets heights.
        config.set_key_value("line_width",             new ConfigOptionFloatOrPercent(0.5, false));
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("the part combines to the outer wall filament's height, warning about limits") {
            // Filament 1 prints the pitch above its max_layer_height (0.3 < 0.4): warned, not vetoed.
            // This fork's validate() appends warnings to a single StringObjectException out-param.
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            REQUIRE(warning.string.find("maximum layer") != std::string::npos);
            REQUIRE(warning.opt_key == "extruder_layer_height");

            print.process();
            // Both parts combine now; the first part's region is identified by its top surface
            // filament staying on 1. Its walls print 0.4 mm on every 2nd layer.
            const PrintObject &object = *print.objects().front();
            int fine_region = -1;
            for (size_t i = 0; i < object.num_printing_regions(); ++ i)
                if (object.printing_region(i).config().top_surface_filament_id.value == 1)
                    fine_region = int(i);
            REQUIRE(fine_region >= 0);
            size_t combined_layers = 0, bad_heights = 0, unexpected = 0;
            for (size_t idx = 0; idx < object.layer_count(); ++ idx) {
                std::vector<float> heights;
                collect_path_heights(object.get_layer(int(idx))->get_region(fine_region)->perimeters, heights);
                if (idx % 2 == 0) {
                    if (! heights.empty())
                        ++ combined_layers;
                    for (float height : heights)
                        if (std::abs(height - 0.4) > 1e-3)
                            ++ bad_heights;
                } else if (! heights.empty())
                    ++ unexpected;
            }
            CHECK(combined_layers > 20);
            CHECK(bad_heights == 0);
            CHECK(unexpected == 0);
        }
    }

    GIVEN("Feature filaments with disagreeing preferred layer heights and no wall preference") {
        // Top surfaces on filament 2 (prefers 0.4 mm) and bottom surfaces on filament 3 (prefers
        // 0.6 mm): the features cannot agree on one pitch, so the part keeps the object layer
        // height and validation warns that not all preferences can be honored.
        DynamicPrintConfig config = two_extruder_config(0.4);
        config.set_key_value("nozzle_diameter",       new ConfigOptionFloats({0.4, 0.6, 0.6}));
        config.set_key_value("extruder_layer_height", new ConfigOptionFloats({0., 0.4, 0.6}));
        config.set_key_value("min_layer_height",      new ConfigOptionFloats({0.07, 0.07, 0.07}));
        config.set_key_value("max_layer_height",      new ConfigOptionFloats({0.3, 0.6, 0.6}));
        config.set_key_value("filament_diameter",     new ConfigOptionFloats({1.75, 1.75, 1.75}));
        config.set_key_value("filament_colour",       new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF"}));
        config.set_key_value("filament_type",         new ConfigOptionStrings({"PLA", "PLA", "PLA"}));
        config.set_key_value("default_filament_colour", new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF"}));
        config.set_key_value("nozzle_temperature",    new ConfigOptionInts({210, 210, 210}));
        config.set_key_value("nozzle_temperature_range_low",  new ConfigOptionInts({190, 190, 190}));
        config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts({240, 240, 240}));
        config.set_key_value("flush_volumes_matrix",  new ConfigOptionFloats(std::vector<double>(9, 0.)));
        config.set_key_value("machine_max_acceleration_extruding", new ConfigOptionFloats({100000., 100000., 100000.}));
        config.set_key_value("top_surface_filament_id",    new ConfigOptionInt(2));
        config.set_key_value("bottom_surface_filament_id", new ConfigOptionInt(3));
        // Keep the combined-region line width checks out of the way, this test targets heights.
        config.set_key_value("line_width",            new ConfigOptionFloatOrPercent(0.5, false));
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("the part keeps the object layer height and validation warns") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            REQUIRE(warning.string.find("cannot all be honored") != std::string::npos);

            print.process();
            const PrintObject &object = *print.objects().front();
            int fine_region, coarse_region;
            find_regions(object, fine_region, coarse_region);
            REQUIRE(fine_region >= 0);
            size_t missing = 0, bad_heights = 0;
            for (size_t idx = 0; idx < object.layer_count(); ++ idx) {
                std::vector<float> heights;
                collect_path_heights(object.get_layer(int(idx))->get_region(fine_region)->perimeters, heights);
                if (heights.empty())
                    ++ missing;
                const double expected = idx == 0 ? 0.4 : 0.2;
                for (float height : heights)
                    if (std::abs(height - expected) > 1e-3)
                        ++ bad_heights;
            }
            CHECK(missing == 0);
            CHECK(bad_heights == 0);
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
            // Notes on the expected widths:
            // - Bottom surface paths resolve their percent width against filament 1's 0.4 mm nozzle.
            // - Internal solid infill proper resolves against filament 2's 0.6 mm nozzle - but the
            //   solid paths ADJACENT to top/bottom shells print with the surface's filament by
            //   design (Fill.cpp), so a filament-1-derived width band among erSolidInfill paths is
            //   correct, and individual lines may be spacing-adapted below the nominal width.
            // Assert each band exists where it must and nothing exceeds its own band's ceiling.
            size_t bottom_paths = 0, bottom_in_band = 0, solid_paths = 0, solid_in_band = 0, solid_above_band = 0;
            for (size_t idx = 0; idx < object.layer_count(); ++ idx) {
                std::vector<std::pair<ExtrusionRole, float>> widths;
                collect_path_role_widths(object.get_layer(int(idx))->get_region(fine_region)->fills, widths);
                for (const std::pair<ExtrusionRole, float> &role_width : widths) {
                    const bool bottom = role_width.first == erBottomSurface;
                    if (! bottom && role_width.first != erSolidInfill)
                        continue;
                    ++ (bottom ? bottom_paths : solid_paths);
                    const double expected = (idx == 0 ? 1.25 : 1.05) * (bottom ? 0.4 : 0.6);
                    if (role_width.second >= expected - 1e-3 && role_width.second <= expected * 1.2 + 1e-3)
                        ++ (bottom ? bottom_in_band : solid_in_band);
                    if (! bottom && role_width.second > expected * 1.2 + 1e-3)
                        ++ solid_above_band;
                }
            }
            CHECK(bottom_paths > 0);
            CHECK(bottom_in_band * 4 > bottom_paths * 3);
            CHECK(solid_paths > 0);
            CHECK(solid_in_band > 0);
            CHECK(solid_above_band == 0);
        }
    }
}

SCENARIO("Combined infill is limited by the printing nozzle only", "[MultiNozzleLayerHeight]") {
    GIVEN("Infill combining to a preferred height above the filament's maximum layer height") {
        // Infill on filament 2: preferred layer height 0.6 exceeds its max_layer_height 0.45.
        // The maximum is a soft profile limit: the explicit preference wins (with stock profiles
        // the maximum would otherwise silently veto every legal preference), only the physical
        // 0.6 mm nozzle bore caps the combining, and validation warns about the exceeded maximum.
        DynamicPrintConfig config = two_extruder_config(0.6);
        config.set_key_value("max_layer_height",           new ConfigOptionFloats({0.3, 0.45}));
        config.set_key_value("sparse_infill_filament_id",  new ConfigOptionInt(2));
        config.set_key_value("internal_solid_filament_id", new ConfigOptionInt(2));
        config.set_key_value("sparse_infill_density",      new ConfigOptionPercent(15));
        Print print;
        Model model;
        // A single part: filament 2 prints only infill (the derived 0.6 mm feature pitch is vetoed
        // by the walls' physical 0.4 mm nozzle, so the part itself stays at the object layer height).
        TriangleMesh cube = mesh(TestMesh::cube_20x20x20);
        cube.scale(Vec3f(1.f, 1.f, 0.5f));
        ModelObject *object_model = model.add_object();
        object_model->name = "single_cube";
        object_model->add_volume(std::move(cube));
        object_model->add_instance();
        for (ModelObject *mo : model.objects) {
            mo->center_around_origin();
            mo->translate(120., 120., 0.);
            mo->ensure_on_bed();
        }
        print.apply(model, config);
        print.set_status_silent();
        THEN("infill combines to the full preferred height and validation warns about the maximum") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            REQUIRE(warning.string.find("maximum layer") != std::string::npos);
            print.process();

            const PrintObject &object = *print.objects().front();
            size_t over_preferred = 0, full_height = 0;
            for (size_t idx = 1; idx < object.layer_count(); ++ idx)
                for (const LayerRegion *layerm : object.get_layer(int(idx))->regions()) {
                    std::vector<float> heights;
                    collect_path_heights(layerm->fills, heights);
                    for (float height : heights) {
                        if (height > 0.6 + 1e-3)
                            ++ over_preferred;
                        else if (std::abs(height - 0.6) < 1e-3)
                            ++ full_height;
                    }
                }
            CHECK(over_preferred == 0);
            CHECK(full_height > 0);
        }
    }

    GIVEN("Walls combining to a pitch above a feature filament's maximum layer height") {
        // Both wall filaments map to filament 2 at a 0.4 mm pitch while top/bottom/solid features
        // stay on filament 1 whose max_layer_height is only 0.3: the explicit wall preference
        // wins - the part combines to 0.4 mm and validation warns about the exceeded maximum
        // (only a physically too small nozzle vetoes the pitch).
        DynamicPrintConfig config = two_extruder_config(0.4);
        config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(2));
        config.set_key_value("inner_wall_filament_id", new ConfigOptionInt(2));
        config.set_key_value("max_layer_height",       new ConfigOptionFloats({0.3, 0.45}));
        // Keep the combined-region line width checks out of the way, this test targets heights.
        config.set_key_value("line_width",             new ConfigOptionFloatOrPercent(0.5, false));
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("validation warns about the maximum and the part prints the walls' pitch") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            REQUIRE(warning.string.find("maximum layer") != std::string::npos);

            print.process();
            // Both parts' walls print with filament 2, so the parts are told apart by their
            // top surface filament; the first part now combines like the second.
            const PrintObject &object = *print.objects().front();
            int fine_region = -1;
            for (size_t i = 0; i < object.num_printing_regions(); ++ i)
                if (object.printing_region(i).config().top_surface_filament_id.value == 1)
                    fine_region = int(i);
            REQUIRE(fine_region >= 0);
            size_t combined_layers = 0, wall_bad_heights = 0, unexpected = 0;
            for (size_t idx = 0; idx < object.layer_count(); ++ idx) {
                std::vector<float> heights;
                collect_path_heights(object.get_layer(int(idx))->get_region(fine_region)->perimeters, heights);
                if (idx % 2 == 0) {
                    if (! heights.empty())
                        ++ combined_layers;
                    for (float height : heights)
                        if (std::abs(height - 0.4) > 1e-3)
                            ++ wall_bad_heights;
                } else if (! heights.empty())
                    ++ unexpected;
            }
            CHECK(combined_layers > 20);
            CHECK(wall_bad_heights == 0);
            CHECK(unexpected == 0);
        }
    }

    GIVEN("Internal solid infill on the coarse filament while the walls stay on Default") {
        // Only internal_solid_filament_id points at filament 2 (preferred
        // layer height 0.4) and no wall filament carries a preference. The feature filament's
        // preference must derive the part's pitch instead of being silently ignored; areas falling
        // back to the object layer height print below filament 2's 0.3 mm minimum, which warns.
        DynamicPrintConfig config = two_extruder_config(0.4);
        config.set_key_value("internal_solid_filament_id", new ConfigOptionInt(2));
        config.set_key_value("min_layer_height",           new ConfigOptionFloats({0.07, 0.3}));
        // Keep the combined-region line width checks out of the way, this test targets heights.
        config.set_key_value("line_width",                 new ConfigOptionFloatOrPercent(0.5, false));
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("the feature filament's preference drives the part's pitch, warning about the minimum") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            REQUIRE(warning.string.find("minimum layer") != std::string::npos);

            print.process();
            // The first part's walls have no preference of their own, yet the part prints 0.4 mm
            // layers on every 2nd layer because its internal solid infill filament asks for them.
            const PrintObject &object = *print.objects().front();
            int fine_region, coarse_region;
            find_regions(object, fine_region, coarse_region);
            REQUIRE(fine_region >= 0);
            size_t combined_layers = 0, bad_heights = 0, unexpected = 0;
            for (size_t idx = 0; idx < object.layer_count(); ++ idx) {
                std::vector<float> heights;
                collect_path_heights(object.get_layer(int(idx))->get_region(fine_region)->perimeters, heights);
                if (idx % 2 == 0) {
                    if (! heights.empty())
                        ++ combined_layers;
                    for (float height : heights)
                        if (std::abs(height - 0.4) > 1e-3)
                            ++ bad_heights;
                } else if (! heights.empty())
                    ++ unexpected;
            }
            CHECK(combined_layers > 20);
            CHECK(bad_heights == 0);
            CHECK(unexpected == 0);
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
        // 0.6 is a multiple of 0.2 and fits the 0.6 mm nozzle; it exceeds max_layer_height 0.45,
        // but that is a soft profile limit - the explicit preference prints and validation warns
        // (with stock profiles the maximum would otherwise reject every legal preference).
        DynamicPrintConfig config = two_extruder_config(0.6);
        Print print;
        Model model;
        init_two_part_print(print, model, config);
        THEN("validation warns instead of failing") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            REQUIRE(warning.string.find("maximum layer") != std::string::npos);
            REQUIRE(warning.opt_key == "extruder_layer_height");
        }
    }
}

// Four extruders with different nozzles, mirroring a Snapmaker U1 customized to 0.2/0.4/0.6/0.8 mm
// nozzles where every extruder carries a preferred layer height (4 * the 0.12 mm object layer
// height on the largest). On such a machine no whole-part pitch is possible - the 0.2 mm nozzle
// prints the part's default features - so the per-feature combining paths must serve instead.
static DynamicPrintConfig four_nozzle_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("layer_height",               new ConfigOptionFloat(0.12));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.12));
    config.set_key_value("enable_prime_tower",         new ConfigOptionBool(false));
    config.set_key_value("enable_support",             new ConfigOptionBool(false));

    config.set_key_value("nozzle_diameter",          new ConfigOptionFloats({0.2, 0.4, 0.6, 0.8}));
    config.set_key_value("extruder_layer_height",    new ConfigOptionFloats({0.12, 0.24, 0.36, 0.48}));
    config.set_key_value("min_layer_height",         new ConfigOptionFloats({0.08, 0.08, 0.14, 0.16}));
    config.set_key_value("max_layer_height",         new ConfigOptionFloats({0.16, 0.32, 0.48, 0.64}));
    config.set_key_value("filament_diameter",        new ConfigOptionFloats({1.75, 1.75, 1.75, 1.75}));
    config.set_key_value("filament_colour",          new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}));
    config.set_key_value("filament_type",            new ConfigOptionStrings({"ABS", "ABS", "ABS", "ABS"}));
    config.set_key_value("default_filament_colour",  new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}));
    config.set_key_value("nozzle_temperature",       new ConfigOptionInts({240, 240, 240, 240}));
    config.set_key_value("nozzle_temperature_range_low",  new ConfigOptionInts({220, 220, 220, 220}));
    config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts({270, 270, 270, 270}));
    config.set_key_value("flush_multiplier",     new ConfigOptionFloat(1.));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(std::vector<double>(16, 0.)));
    config.set_key_value("machine_max_acceleration_extruding", new ConfigOptionFloats({100000., 100000.}));
    config.set_key_value("use_relative_e_distances", new ConfigOptionBool(false));
    return config;
}

// One 20x20x10 mm cube.
static void init_cube_print(Print &print, Model &model, const DynamicPrintConfig &config)
{
    TriangleMesh cube = mesh(TestMesh::cube_20x20x20);
    cube.scale(Vec3f(1.f, 1.f, 0.5f));
    ModelObject *object = model.add_object();
    object->name = "cube";
    object->add_volume(std::move(cube));
    object->add_instance();
    for (ModelObject *mo : model.objects) {
        mo->center_around_origin();
        mo->translate(120., 120., 0.);
        mo->ensure_on_bed();
    }
    print.apply(model, config);
    print.set_status_silent();
}

SCENARIO("Walls combine to their filament's pitch when the part cannot follow", "[MultiNozzleLayerHeight]") {
    GIVEN("Both walls on the 0.8 mm nozzle filament preferring 0.48 mm, the rest on the 0.2 mm nozzle") {
        DynamicPrintConfig config = four_nozzle_config();
        config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(4));
        config.set_key_value("inner_wall_filament_id", new ConfigOptionInt(4));
        Print print;
        Model model;
        init_cube_print(print, model, config);
        THEN("walls print once per 4 layers at 0.48 mm while the fills keep 0.12 mm") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            // The whole-part pitch is impossible (0.2 mm nozzle prints the fills), but the walls
            // combine on their own - no "parts print with the object layer height" fallback.
            CHECK(warning.string.find("too small to extrude") == std::string::npos);
            print.process();

            const PrintObject &object = *print.objects().front();
            size_t tall_wall_layers = 0, plain_wall_layers = 0, wall_bad_heights = 0, fill_bad_heights = 0;
            for (size_t idx = 0; idx < object.layer_count(); ++ idx) {
                const LayerRegion *layerm = object.get_layer(int(idx))->get_region(0);
                std::vector<float> wall_heights;
                collect_path_heights(layerm->perimeters, wall_heights);
                bool tall = false, plain = false;
                for (float height : wall_heights) {
                    if (std::abs(height - 0.48) < 1e-3)
                        tall = true;
                    else if (std::abs(height - 0.12) < 1e-3 || std::abs(height - 0.24) < 1e-3)
                        // The first layer and the run capping the object top stay finer.
                        plain = true;
                    else
                        ++ wall_bad_heights;
                }
                if (tall) ++ tall_wall_layers;
                if (plain) ++ plain_wall_layers;
                std::vector<float> fill_heights;
                collect_path_heights(layerm->fills, fill_heights);
                for (float height : fill_heights)
                    if (std::abs(height - 0.12) > 1e-3)
                        ++ fill_bad_heights;
            }
            // 83 layers: layer 0 plain, 20 full runs of 4, a 2-layer cap.
            CHECK(tall_wall_layers >= 15);
            CHECK(plain_wall_layers <= 4);
            CHECK(wall_bad_heights == 0);
            CHECK(fill_bad_heights == 0);
        }
    }
}

SCENARIO("Top surfaces combine to their filament's pitch by absorbing the shells below", "[MultiNozzleLayerHeight]") {
    GIVEN("Top surfaces on the 0.8 mm nozzle filament preferring 0.48 mm, the rest on the 0.2 mm nozzle") {
        DynamicPrintConfig config = four_nozzle_config();
        config.set_key_value("top_surface_filament_id", new ConfigOptionInt(4));
        config.set_key_value("top_shell_layers",        new ConfigOptionInt(9));
        config.set_key_value("bottom_shell_layers",     new ConfigOptionInt(7));
        Print print;
        Model model;
        init_cube_print(print, model, config);
        THEN("the topmost surface prints once at 0.48 mm while everything else keeps 0.12 mm") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            print.process();

            const PrintObject &object = *print.objects().front();
            size_t tall_fill_paths = 0, wall_bad_heights = 0;
            for (size_t idx = 0; idx < object.layer_count(); ++ idx) {
                const LayerRegion *layerm = object.get_layer(int(idx))->get_region(0);
                std::vector<float> fill_heights;
                collect_path_heights(layerm->fills, fill_heights);
                for (float height : fill_heights)
                    if (std::abs(height - 0.48) < 1e-3) {
                        ++ tall_fill_paths;
                        // Only the topmost layer may carry the absorbed pass.
                        CHECK(idx == object.layer_count() - 1);
                    }
                std::vector<float> wall_heights;
                collect_path_heights(layerm->perimeters, wall_heights);
                for (float height : wall_heights)
                    if (std::abs(height - 0.12) > 1e-3)
                        ++ wall_bad_heights;
            }
            CHECK(tall_fill_paths > 0);
            CHECK(wall_bad_heights == 0);
        }
    }
}

SCENARIO("Internal solid infill combines to its filament's pitch", "[MultiNozzleLayerHeight]") {
    GIVEN("Disagreeing wall preferences with the internal solid infill on the 0.8 mm nozzle filament") {
        // The wall filaments' explicit preferences disagree (0.12 vs 0.24 mm), so the part keeps
        // the object layer height and the walls split. The internal solid infill used to fall
        // through every combining pass here, printing 0.12 mm layers on the 0.8 mm nozzle - below
        // its own 0.16 mm minimum layer height.
        DynamicPrintConfig config = four_nozzle_config();
        config.set_key_value("outer_wall_filament_id",     new ConfigOptionInt(1));
        config.set_key_value("inner_wall_filament_id",     new ConfigOptionInt(2));
        config.set_key_value("sparse_infill_filament_id",  new ConfigOptionInt(3));
        config.set_key_value("internal_solid_filament_id", new ConfigOptionInt(4));
        config.set_key_value("top_surface_filament_id",    new ConfigOptionInt(4));
        config.set_key_value("bottom_surface_filament_id", new ConfigOptionInt(4));
        config.set_key_value("top_shell_layers",           new ConfigOptionInt(4));
        config.set_key_value("bottom_shell_layers",        new ConfigOptionInt(5));
        config.set_key_value("sparse_infill_density",      new ConfigOptionPercent(15));
        Print print;
        Model model;
        init_cube_print(print, model, config);
        THEN("the solid interior prints 0.48 mm groups and never below the extruder's minimum") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            // The bottom surfaces (also filament 4) keep printing the object layer height: warned.
            REQUIRE(warning.string.find("minimum layer") != std::string::npos);
            print.process();

            const PrintObject &object = *print.objects().front();
            // The one legitimate below-minimum leftover: the single solid layer an internal
            // bridge rests on. It is tied to its own layer (sparse below, the bridge above), so
            // no combining can lift it to the minimum.
            auto seats_internal_bridge = [&object](size_t idx) {
                if (idx + 1 >= object.layer_count())
                    return false;
                const Surfaces &above = object.get_layer(int(idx + 1))->get_region(0)->fill_surfaces.surfaces;
                return std::any_of(above.begin(), above.end(),
                    [](const Surface &surface) { return surface.surface_type == stInternalBridge; });
            };
            size_t tall_solid_paths = 0, below_min_paths = 0;
            std::vector<size_t> below_min_layers;
            for (size_t idx = 1; idx < object.layer_count(); ++ idx)
                for_each_path(object.get_layer(int(idx))->get_region(0)->fills, [&](const ExtrusionPath &path) {
                    if (path.role() != erSolidInfill)
                        return;
                    if (path.height > 0.48f - 1e-3f)
                        ++ tall_solid_paths;
                    else if (path.height < 0.16f - 1e-3f && ! seats_internal_bridge(idx)) {
                        ++ below_min_paths;
                        if (below_min_layers.empty() || below_min_layers.back() != idx)
                            below_min_layers.push_back(idx);
                    }
                });
            CAPTURE(below_min_layers);
            CHECK(tall_solid_paths > 0);
            CHECK(below_min_paths == 0);
        }
    }
}

SCENARIO("A preference-less fine-nozzle wall filament vetoes the walls-only pitch", "[MultiNozzleLayerHeight]") {
    GIVEN("Outer walls on the 0.8 mm nozzle preferring 0.48 mm, inner walls on a 0.2 mm nozzle with no preference") {
        DynamicPrintConfig config = four_nozzle_config();
        config.set_key_value("extruder_layer_height",  new ConfigOptionFloats({0., 0., 0., 0.48}));
        config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(4));
        config.set_key_value("inner_wall_filament_id", new ConfigOptionInt(1));
        Print print;
        Model model;
        init_cube_print(print, model, config);
        THEN("no wall combines: the 0.2 mm inner-wall nozzle cannot extrude 0.48 mm layers") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            print.process();

            const PrintObject &object = *print.objects().front();
            size_t wall_bad_heights = 0;
            for (size_t idx = 0; idx < object.layer_count(); ++ idx) {
                std::vector<float> wall_heights;
                collect_path_heights(object.get_layer(int(idx))->get_region(0)->perimeters, wall_heights);
                for (float height : wall_heights)
                    if (std::abs(height - 0.12f) > 1e-3f)
                        ++ wall_bad_heights;
            }
            CHECK(wall_bad_heights == 0);
        }
    }
}

SCENARIO("Disagreeing wall preferences meet at the lower height", "[MultiNozzleLayerHeight]") {
    GIVEN("Outer walls prefer 0.48 mm (0.8 mm nozzle) and inner walls prefer 0.36 mm (0.6 mm nozzle)") {
        DynamicPrintConfig config = four_nozzle_config();
        config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(4));
        config.set_key_value("inner_wall_filament_id", new ConfigOptionInt(3));
        Print print;
        Model model;
        init_cube_print(print, model, config);
        THEN("the walls combine to 0.36 mm - the lower preference both nozzles can print") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            print.process();

            const PrintObject &object = *print.objects().front();
            size_t tall_wall_layers = 0, wall_bad_heights = 0, fill_bad_heights = 0;
            for (size_t idx = 0; idx < object.layer_count(); ++ idx) {
                const LayerRegion *layerm = object.get_layer(int(idx))->get_region(0);
                std::vector<float> wall_heights;
                collect_path_heights(layerm->perimeters, wall_heights);
                for (float height : wall_heights) {
                    if (std::abs(height - 0.36f) < 1e-3f)
                        ++ tall_wall_layers;
                    else if (std::abs(height - 0.12f) > 1e-3f && std::abs(height - 0.24f) > 1e-3f)
                        // The first layer and forced / capping runs stay finer.
                        ++ wall_bad_heights;
                }
                std::vector<float> fill_heights;
                collect_path_heights(layerm->fills, fill_heights);
                for (float height : fill_heights)
                    if (std::abs(height - 0.12f) > 1e-3f)
                        ++ fill_bad_heights;
            }
            CHECK(tall_wall_layers >= 15);
            CHECK(wall_bad_heights == 0);
            CHECK(fill_bad_heights == 0);
        }
    }
}

// Heights of the wall extrusions per wall class, classified like the G-code dispatch
// (perimeter_entity_uses_outer_wall_filament()). Bridges keep their nozzle-derived flow height
// and are skipped, like collect_path_heights().
static void collect_wall_class_heights(const ExtrusionEntityCollection &collection,
                                       std::vector<float> &outer_heights, std::vector<float> &inner_heights)
{
    for (const ExtrusionEntity *entity : collection.entities) {
        if (auto *sub = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
            collect_wall_class_heights(*sub, outer_heights, inner_heights);
            continue;
        }
        std::vector<float> &dst = perimeter_entity_uses_outer_wall_filament(*entity) ? outer_heights : inner_heights;
        if (auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
            for (const ExtrusionPath &path : loop->paths)
                if (path.role() != erBridgeInfill && path.role() != erInternalBridgeInfill && path.role() != erOverhangPerimeter)
                    dst.emplace_back(path.height);
        } else if (auto *multi_path = dynamic_cast<const ExtrusionMultiPath *>(entity)) {
            for (const ExtrusionPath &path : multi_path->paths)
                dst.emplace_back(path.height);
        } else if (auto *path = dynamic_cast<const ExtrusionPath *>(entity))
            dst.emplace_back(path->height);
    }
}

// Wall heights of the whole object bucketed per class: `tall` counts the layers where a class
// prints its own pitch (the first expected entry); any height outside the class's expected set
// (the finer entries: fine cadence, first-layer and cap fallbacks) counts as `bad`.
struct WallHeightCounts { size_t outer_tall = 0, inner_tall = 0, bad = 0; };
static WallHeightCounts count_wall_heights(const PrintObject &object,
                                           const std::vector<float> &outer_expected,
                                           const std::vector<float> &inner_expected)
{
    auto tally = [](const std::vector<float> &heights, const std::vector<float> &expected, size_t &tall_layers, size_t &bad) {
        bool tall = false;
        for (float height : heights) {
            if (std::abs(height - expected.front()) < 1e-3f)
                tall = true;
            else if (std::none_of(expected.begin() + 1, expected.end(),
                                  [height](float e) { return std::abs(height - e) < 1e-3f; }))
                ++ bad;
        }
        tall_layers += tall;
    };
    WallHeightCounts counts;
    for (size_t idx = 0; idx < object.layer_count(); ++ idx) {
        std::vector<float> outer_heights, inner_heights;
        collect_wall_class_heights(object.get_layer(int(idx))->get_region(0)->perimeters, outer_heights, inner_heights);
        tally(outer_heights, outer_expected, counts.outer_tall, counts.bad);
        tally(inner_heights, inner_expected, counts.inner_tall, counts.bad);
    }
    return counts;
}

SCENARIO("Split wall layer heights print each wall class at its own pitch", "[MultiNozzleLayerHeight]") {
    GIVEN("Outer walls prefer 0.48 mm and inner walls 0.24 mm - divisible heights split automatically") {
        DynamicPrintConfig config = four_nozzle_config();
        config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(4));
        config.set_key_value("inner_wall_filament_id", new ConfigOptionInt(2));
        Print print;
        Model model;
        init_cube_print(print, model, config);
        THEN("outer walls print once per 4 layers at 0.48 mm, inner walls once per 2 at 0.24 mm") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            // The disagreement is intentional here; no conflict warning.
            CHECK(warning.string.find("prefer different layer heights") == std::string::npos);
            print.process();

            // Outside committed coarse runs the outer walls follow the fine cadence; the first
            // layer and fallback areas keep the object layer height. 83 layers: ~20 coarse runs
            // of 4 and ~40 fine runs of 2 above the first layer.
            const PrintObject      &object = *print.objects().front();
            const WallHeightCounts  counts = count_wall_heights(object, {0.48f, 0.24f, 0.12f}, {0.24f, 0.12f});
            CHECK(counts.outer_tall >= 15);
            CHECK(counts.inner_tall >= 30);
            CHECK(counts.bad == 0);
            size_t fill_bad_heights = 0;
            for (size_t idx = 0; idx < object.layer_count(); ++ idx) {
                std::vector<float> fill_heights;
                collect_path_heights(object.get_layer(int(idx))->get_region(0)->fills, fill_heights);
                for (float height : fill_heights)
                    if (std::abs(height - 0.12f) > 1e-3f)
                        ++ fill_bad_heights;
            }
            CHECK(fill_bad_heights == 0);
        }
    }
    GIVEN("The finer wall class at the object layer height itself") {
        DynamicPrintConfig config = four_nozzle_config();
        config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(4));
        config.set_key_value("inner_wall_filament_id", new ConfigOptionInt(1));
        Print print;
        Model model;
        init_cube_print(print, model, config);
        THEN("inner walls print every 0.12 mm layer while outer walls combine to 0.48 mm") {
            REQUIRE(print.validate().string.empty());
            print.process();

            const WallHeightCounts counts = count_wall_heights(*print.objects().front(), {0.48f, 0.12f}, {0.12f});
            CHECK(counts.outer_tall >= 15);
            CHECK(counts.bad == 0);
        }
    }
    GIVEN("Wall preferences that do not divide evenly (0.48 mm and 0.36 mm), no adjustment") {
        DynamicPrintConfig config = four_nozzle_config();
        config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(4));
        config.set_key_value("inner_wall_filament_id", new ConfigOptionInt(3));
        Print print;
        Model model;
        init_cube_print(print, model, config);
        THEN("the walls fall back to printing together at the lower height, with the conflict warning") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            CHECK(warning.string.find("prefer different layer heights") != std::string::npos);
            print.process();

            const WallHeightCounts counts = count_wall_heights(*print.objects().front(),
                                                               {0.36f, 0.24f, 0.12f}, {0.36f, 0.24f, 0.12f});
            CHECK(counts.outer_tall >= 15);
            CHECK(counts.bad == 0);
        }
    }
}

// Non-divisible wall preferences reconciled by "split_wall_adjust": outer walls prefer 0.48 mm
// (filament 4, multiplier 4) and inner walls 0.36 mm (filament 3, multiplier 3) over 0.12 mm
// object layers. The selected wall class moves to the nearest divisor / multiple of the other
// in the selected direction, hard-bounded by its filament's layer height limits.
static DynamicPrintConfig wall_adjust_config(WallSplitFilament filament, WallSplitDirection direction)
{
    DynamicPrintConfig config = four_nozzle_config();
    config.set_key_value("outer_wall_filament_id",      new ConfigOptionInt(4));
    config.set_key_value("inner_wall_filament_id",      new ConfigOptionInt(3));
    config.set_key_value("split_wall_adjust",           new ConfigOptionBool(true));
    config.set_key_value("split_wall_adjust_filament",  new ConfigOptionEnum<WallSplitFilament>(filament));
    config.set_key_value("split_wall_adjust_direction", new ConfigOptionEnum<WallSplitDirection>(direction));
    return config;
}

SCENARIO("Adjusting a wall layer height reconciles non-divisible wall preferences", "[MultiNozzleLayerHeight]") {
    GIVEN("The inner walls adjusted downwards") {
        DynamicPrintConfig config = wall_adjust_config(wsfInnerWall, wsdDecrease);
        Print print;
        Model model;
        init_cube_print(print, model, config);
        THEN("inner walls print 0.24 mm - the largest divisor of 0.48 mm below 0.36 mm - and the walls split") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            CHECK(warning.string.find("was adjusted") != std::string::npos);
            CHECK(warning.string.find("prefer different layer heights") == std::string::npos);
            print.process();

            // In particular no 0.36 mm: the raw inner preference is off for walls.
            const WallHeightCounts counts = count_wall_heights(*print.objects().front(),
                                                               {0.48f, 0.24f, 0.12f}, {0.24f, 0.12f});
            CHECK(counts.outer_tall >= 15);
            CHECK(counts.inner_tall >= 30);
            CHECK(counts.bad == 0);
        }
    }
    GIVEN("The inner walls adjusted upwards") {
        DynamicPrintConfig config = wall_adjust_config(wsfInnerWall, wsdIncrease);
        Print print;
        Model model;
        init_cube_print(print, model, config);
        THEN("the inner walls land on the outer walls' 0.48 mm and the walls merge there") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            CHECK(warning.string.find("was adjusted") != std::string::npos);
            CHECK(warning.string.find("prefer different layer heights") == std::string::npos);
            print.process();

            // Merged walls never print the raw 0.36 mm inner preference (bad == 0 covers it).
            const WallHeightCounts counts = count_wall_heights(*print.objects().front(),
                                                               {0.48f, 0.24f, 0.12f}, {0.48f, 0.24f, 0.12f});
            CHECK(counts.inner_tall >= 15);
            CHECK(counts.bad == 0);
        }
    }
    GIVEN("The outer walls adjusted upwards, where the next candidate breaks the layer height limit") {
        // The smallest multiple of the inner 0.36 mm above the outer 0.48 mm is 0.72 mm, over
        // filament 4's 0.64 mm maximum layer height - a hard bound for adjustments.
        DynamicPrintConfig config = wall_adjust_config(wsfOuterWall, wsdIncrease);
        Print print;
        Model model;
        init_cube_print(print, model, config);
        THEN("no adjustment happens: the walls merge at the lower height with the conflict warning") {
            StringObjectException warning;
            REQUIRE(print.validate(&warning).string.empty());
            CHECK(warning.string.find("was adjusted") == std::string::npos);
            CHECK(warning.string.find("prefer different layer heights") != std::string::npos);
            print.process();

            const WallHeightCounts counts = count_wall_heights(*print.objects().front(),
                                                               {0.36f, 0.24f, 0.12f}, {0.36f, 0.24f, 0.12f});
            CHECK(counts.outer_tall >= 15);
            CHECK(counts.bad == 0);
        }
    }
}
