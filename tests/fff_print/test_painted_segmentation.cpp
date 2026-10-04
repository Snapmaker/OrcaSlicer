#include <catch2/catch_all.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/ClipperUtils.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// ORCA: painted multi-material segmentation on a printer whose extruders combine layers
// (per-extruder layer heights) or whose regions override the outer-wall filament.

namespace {

// Three filaments at 0.2 mm layers: 1 and 2 print every layer, 3 uses a 0.6 mm nozzle and height
// (runs of three layers). Parameters override the first and third nozzle and height.
DynamicPrintConfig three_filament_config(double third_height = 0.6, double first_height = 0., double first_nozzle = 0.4, double third_nozzle = 0.6)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("layer_height",               new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("enable_prime_tower",         new ConfigOptionBool(false));
    config.set_key_value("enable_support",             new ConfigOptionBool(false));

    config.set_key_value("nozzle_diameter",          new ConfigOptionFloats({first_nozzle, 0.4, third_nozzle}));
    config.set_key_value("extruder_layer_height",    new ConfigOptionFloats({first_height, 0., third_height}));
    config.set_key_value("min_layer_height",         new ConfigOptionFloats({0.07, 0.07, 0.07}));
    config.set_key_value("max_layer_height",         new ConfigOptionFloats({0.45, 0.3, std::max(0.45, third_height)}));
    config.set_key_value("filament_diameter",        new ConfigOptionFloats({1.75, 1.75, 1.75}));
    config.set_key_value("filament_colour",          new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF"}));
    config.set_key_value("filament_type",            new ConfigOptionStrings({"PLA", "PLA", "PLA"}));
    config.set_key_value("default_filament_colour",  new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF"}));
    config.set_key_value("nozzle_temperature",       new ConfigOptionInts({210, 210, 210}));
    config.set_key_value("nozzle_temperature_range_low",  new ConfigOptionInts({190, 190, 190}));
    config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts({240, 240, 240}));
    // Per nozzle: a flush multiplier and a filaments x filaments block.
    config.set_key_value("flush_multiplier",     new ConfigOptionFloats({1., 1., 1.}));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(std::vector<double>(27, 0.)));
    config.set_key_value("machine_max_acceleration_extruding", new ConfigOptionFloats({100000., 100000., 100000.}));
    config.set_key_value("use_relative_e_distances", new ConfigOptionBool(false));
    return config;
}

// Paints every facet of `volume` whose unit normal satisfies `facing` with `filament` (1-based).
template<typename Facing>
void paint_facets(ModelVolume &volume, int filament, const Facing &facing)
{
    TriangleSelector           selector(volume.mesh());
    const indexed_triangle_set &its = volume.mesh().its;
    for (size_t f = 0; f < its.indices.size(); ++ f) {
        const Vec3f &a = its.vertices[its.indices[f](0)], &b = its.vertices[its.indices[f](1)], &c = its.vertices[its.indices[f](2)];
        const Vec3f  n = (b - a).cross(c - a).normalized();
        if (facing(n))
            selector.set_facet(int(f), EnforcerBlockerType(filament));
    }
    REQUIRE(volume.mmu_segmentation_facets.set(selector));
}

// A 20 x 20 x 5 mm block with a 10 mm ledge cantilevered from the top of its x = 20 side, both
// `part_filament`; the ledge top is painted with `paint`, with no geometry below it.
void add_ledged_block(Model &model, int part_filament, int paint, double ledge_thickness = 0.6)
{
    ModelObject *object = model.add_object();
    object->name = "ledged_block";
    ModelVolume *block = object->add_volume(make_cube(20., 20., 5.));
    block->config.set("extruder", part_filament);
    TriangleMesh ledge_mesh = make_cube(10., 20., ledge_thickness);
    ledge_mesh.translate(20.f, 0.f, float(5. - ledge_thickness));
    ModelVolume *ledge = object->add_volume(std::move(ledge_mesh));
    ledge->config.set("extruder", part_filament);
    paint_facets(*ledge, paint, [](const Vec3f &n) { return n.z() > 0.5f; });
    object->add_instance();
}

// This fork's arrangement engine rejects positions outside the (unset) plate even for an
// InfiniteBed; the fixtures are laid out already, so they go to a fixed bed spot.
void place_and_apply(Print &print, Model &model, const DynamicPrintConfig &config)
{
    for (ModelObject *mo : model.objects) {
        mo->center_around_origin();
        mo->translate(120., 120., 0.);
        mo->ensure_on_bed();
    }
    print.apply(model, config);
    print.set_status_silent();
}

// Slices `print` on a worker thread within `budget`; returns false on a hang, leaking the print
// and the worker on purpose (the worker may still run inside it). Slicing exceptions are rethrown.
bool process_within(std::unique_ptr<Print> &print, std::chrono::seconds budget)
{
    auto              finished = std::make_shared<std::promise<void>>();
    std::future<void> done     = finished->get_future();
    Print            *raw      = print.get();
    std::thread worker([raw, finished]() {
        try {
            raw->process();
            finished->set_value();
        } catch (...) {
            finished->set_exception(std::current_exception());
        }
    });
    if (done.wait_for(budget) == std::future_status::ready) {
        worker.join();
        done.get();
        return true;
    }
    worker.detach();
    Print *leaked = print.release();
    (void)leaked;
    return false;
}

// Index of the first printing region whose inner walls print with `filament`, -1 when none.
int region_with_inner_wall_filament(const PrintObject &object, int filament)
{
    for (size_t i = 0; i < object.num_printing_regions(); ++ i)
        if (object.printing_region(i).config().inner_wall_filament_id.value == filament)
            return int(i);
    return -1;
}

double region_area_at(const Layer &layer, int region)
{
    double sum = 0.;
    if (region >= 0 && size_t(region) < layer.region_count())
        for (const Surface &surface : layer.get_region(region)->slices.surfaces)
            sum += unscale<double>(unscale<double>(surface.expolygon.area()));
    return sum;
}

} // namespace

SCENARIO("A painted colour no region prints outer walls with does not hang the segmentation", "[PaintedSegmentation][Segmentation][MultiNozzleLayerHeight][Regression]") {
    // Outer walls overridden to filament 1, a thin ledge top painted with run colour 3: the colour's
    // outer-wall width is zero and its shell rows run out of geometry under the ledge, which must
    // not hang the inset relaxation of pitch_shell_row.
    GIVEN("a ledged block with an outer-wall override and its ledge top painted with a run colour") {
        auto slices_within_budget = [](DynamicPrintConfig config) {
            config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(1));
            Model model;
            auto  print = std::make_unique<Print>();
            add_ledged_block(model, 2, 3);
            place_and_apply(*print, model, config);
            REQUIRE(process_within(print, std::chrono::seconds(180)));
            const PrintObject &object  = *print->objects().front();
            const int          painted = region_with_inner_wall_filament(object, 3);
            REQUIRE(painted >= 0);
            const Layer &top = *object.layers().back();
            CAPTURE(top.print_z, object.printing_region(size_t(painted)).config().outer_wall_filament_id.value);
            CHECK(object.printing_region(size_t(painted)).config().outer_wall_filament_id.value == 1);
            CHECK(region_area_at(top, painted) > 50.);   // the ledge's painted top (10 x 20 mm) reached its region
        };
        WHEN("the outer-wall filament prints every layer") {
            THEN("the slicing finishes and the painted face reaches its region") {
                slices_within_budget(three_filament_config());
            }
        }
        WHEN("the outer-wall filament shares the painted filament's pitch") {
            THEN("the slicing finishes and the painted face reaches its region") {
                slices_within_budget(three_filament_config(0.6, 0.6, 0.6));
            }
        }
    }
}

SCENARIO("An area painted with the outer-wall override filament is printed once", "[PaintedSegmentation][Segmentation][Regression]") {
    // Part filament 2, outer walls overridden to 1, one side painted with 1: the painted region
    // (inner walls and infill follow the paint) takes the area and the parent must drop it.
    GIVEN("a cube with an outer-wall override and one side painted with the override filament") {
        DynamicPrintConfig config = three_filament_config();
        config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(1));
        Model model;
        Print print;
        ModelObject *object = model.add_object();
        object->name = "painted_cube";
        ModelVolume *cube = object->add_volume(make_cube(20., 20., 10.));
        cube->config.set("extruder", 2);
        paint_facets(*cube, 1, [](const Vec3f &n) { return n.x() > 0.5f; });
        object->add_instance();
        place_and_apply(print, model, config);
        THEN("the regions of every layer add up to its slice and the painted region holds the painted side") {
            print.process();
            const PrintObject &po      = *print.objects().front();
            const int          painted = region_with_inner_wall_filament(po, 1);
            REQUIRE(painted >= 0);
            CHECK(po.printing_region(size_t(painted)).config().outer_wall_filament_id.value == 1);
            double painted_total = 0., worst_excess = 0.;
            for (const Layer *layer : po.layers()) {
                const double slice   = unscale<double>(unscale<double>(area(layer->lslices)));
                double       regions = 0.;
                for (size_t r = 0; r < layer->region_count(); ++ r)
                    regions += region_area_at(*layer, int(r));
                painted_total += region_area_at(*layer, painted);
                worst_excess = std::max(worst_excess, regions - slice);
                CAPTURE(layer->print_z, slice, regions);
                CHECK(regions <= slice * 1.02 + 0.5);
                CHECK(regions >= slice * 0.98 - 0.5);
            }
            CAPTURE(worst_excess);
            CHECK(painted_total > 100.);   // the painted side region is there, once
        }
    }
}

TEST_CASE("A painted colour whose region cannot follow its filament's pitch gets the stock shell depth", "[PaintedSegmentation][Segmentation][MultiNozzleLayerHeight][Regression]")
{
    // Top face painted with filament 3 (0.6 mm pitch), top surfaces fixed to filament 1 (0.4 mm
    // nozzle): the region prints every layer, so its shell is the configured two rows, not the
    // 2N-1 = 5 rows of a run colour. The paint penetration sets the painted depth.
    DynamicPrintConfig config = three_filament_config();
    config.set_key_value("top_surface_filament_id",      new ConfigOptionInt(1));
    config.set_key_value("top_shell_layers",             new ConfigOptionInt(2));
    config.set_key_value("top_color_penetration_layers", new ConfigOptionInt(2));
    Model model;
    Print print;
    ModelObject *object = model.add_object();
    object->name = "capped_cube";
    ModelVolume *cube = object->add_volume(make_cube(20., 20., 10.));
    cube->config.set("extruder", 2);
    paint_facets(*cube, 3, [](const Vec3f &n) { return n.z() > 0.5f; });
    object->add_instance();
    place_and_apply(print, model, config);
    print.process();
    const PrintObject &po      = *print.objects().front();
    const int          painted = region_with_inner_wall_filament(po, 3);
    REQUIRE(painted >= 0);
    CHECK(po.printing_region(size_t(painted)).config().outer_wall_filament_id.value == 3);
    CHECK(po.region_layer_height_multiplier(po.printing_region(size_t(painted))) == 1u);
    // 10 mm at 0.2 mm: 50 layers, the painted face on the last one.
    REQUIRE(po.layer_count() == size_t(50));
    std::vector<double> areas;
    for (size_t idx = 44; idx < 50; ++ idx)
        areas.emplace_back(region_area_at(*po.get_layer(int(idx)), painted));
    CAPTURE(areas);
    CHECK(areas[5] > 300.);   // the painted top face itself
    CHECK(areas[4] > 100.);   // the one shell row under it
    for (size_t i = 0; i < 4; ++ i)
        CHECK(areas[i] < 0.1); // rows 44..47: no deep run shell
}

TEST_CASE("The interlocking depth alternates per run of a colour that combines layers", "[PaintedSegmentation][Segmentation][MultiNozzleLayerHeight][Regression]")
{
    // +x side painted with filament 3 (0.8 mm nozzle, runs of four 0.2 mm layers), cut to a 2 mm ribbon
    // and on alternate runs to the 1 mm interlocking depth. A ribbon of width w in the side's
    // triangle holds about w * (20 - w) mm2: 19 for the depth, 36 for the full width.
    DynamicPrintConfig config = three_filament_config(0.8, 0., 0.4, 0.8);
    config.set_key_value("mmu_segmented_region_max_width",          new ConfigOptionFloat(2.0));
    config.set_key_value("mmu_segmented_region_interlocking_depth", new ConfigOptionFloat(1.0));
    Model model;
    Print print;
    ModelObject *object = model.add_object();
    object->name = "interlocked_cube";
    ModelVolume *cube = object->add_volume(make_cube(20., 20., 10.));
    paint_facets(*cube, 3, [](const Vec3f &n) { return n.x() > 0.5f; });
    object->add_instance();
    place_and_apply(print, model, config);
    print.process();
    const PrintObject &po      = *print.objects().front();
    const int          painted = region_with_inner_wall_filament(po, 3);
    REQUIRE(painted >= 0);
    REQUIRE(po.region_layer_height_multiplier(po.printing_region(size_t(painted))) == 4u);
    // The runs sit on the ladder from layer 1: rows 1-4 form run 0, rows 5-8 run 1, ... and the
    // run's top row holds its committed shape. Even runs are cut to the depth, odd ones to the width.
    std::vector<double> narrow, wide;
    for (size_t idx = PrintObject::first_combined_layer_idx; idx < po.layer_count(); ++ idx) {
        const LayerRegion *layerm = po.get_layer(int(idx))->get_region(painted);
        if (layerm->combined_layer_count() != 4)
            continue;
        const size_t run = (idx - PrintObject::first_combined_layer_idx) / 4;
        (run % 2 == 0 ? narrow : wide).emplace_back(region_area_at(*po.get_layer(int(idx)), painted));
    }
    CAPTURE(narrow, wide);
    REQUIRE(narrow.size() >= 3);
    REQUIRE(wide.size() >= 3);
    for (double a : narrow) {
        CHECK(a > 8.);
        CHECK(a < 27.);
    }
    for (double a : wide) {
        CHECK(a > 28.);
        CHECK(a < 48.);
    }
}

TEST_CASE("Interlocking beams are rejected while a region combines layers", "[PaintedSegmentation][Segmentation][MultiNozzleLayerHeight]")
{
    // The beams are carved per object layer before the runs are built; a run keeps only the shape
    // common to its layers, so a combined region's beam cells vanish while the holes for them stay
    // in the other region. Validation rejects the combination instead of slicing a defective part.
    DynamicPrintConfig config = three_filament_config();
    config.set_key_value("interlocking_beam", new ConfigOptionBool(true));
    Model model;
    Print print;
    ModelObject *object = model.add_object();
    object->name = "beamed_parts";
    object->add_volume(make_cube(20., 20., 10.));
    TriangleMesh coarse_mesh = make_cube(20., 20., 10.);
    coarse_mesh.translate(20.f, 0.f, 0.f);
    ModelVolume *coarse = object->add_volume(std::move(coarse_mesh));
    coarse->config.set("extruder", 3);
    object->add_instance();
    place_and_apply(print, model, config);
    const std::string error = print.validate().string;
    CAPTURE(error);
    CHECK(error.find("interlocking beams") != std::string::npos);
}
