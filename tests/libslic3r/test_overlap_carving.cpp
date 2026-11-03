// Overlap-carving switch tests for slices_to_regions().
//
// The developer switch enable_overlap_area_carving (Process > Others > Special
// mode) is a pure runtime switch between two reference behaviors:
//   off (default): upstream legacy "last listed wins" - the later-listed volume
//     carves every earlier overlapping one.
//   on: exactly what 2.4.1 shipped (ddc522ba21) - per-layer smaller XY slice
//     area carves the larger, with a region-id tie-break while the areas stay
//     within 1.5x (the smaller region id, i.e. the earlier-listed one, wins).
//
// Pipeline-level: scenarios are sliced through PrintObject::slice() - the exact
// path slices_to_regions() arbitrates. Slices are raw layer geometry, so region
// areas are exact box areas; only shares (ratios) are asserted.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Layer.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Print.hpp"

#include <algorithm>
#include <memory>

using namespace Slic3r;

namespace {

// Matches production: the GUI enables mixed-filament auto generation during
// AppConfig startup.
static const int _auto_generate_enabler = []() {
    Slic3r::MixedFilamentManager::set_auto_generate_enabled(true);
    return 0;
}();

DynamicPrintConfig carving_print_config(bool area_carving)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set("layer_height", 0.2);
    config.set("initial_layer_print_height", 0.2);
    config.set("outer_wall_line_width", 0.4);
    config.set("enable_prime_tower", false);
    // Four filaments so the per-volume extruders map to distinct print regions.
    // (Direct option assignment: DynamicPrintConfig::set with an initializer list
    // misbinds on this build and throws std::length_error.)
    if (auto* opt = config.option<ConfigOptionStrings>("filament_colour"))
        opt->values = {"#FF0000", "#00FFFF", "#00FF00", "#FFFF00"};
    // num_extruders is derived from filament_diameter.size(); the full config
    // default carries a single entry, which would clamp all extruders to 1.
    if (auto* dia = config.option<ConfigOptionFloats>("filament_diameter"))
        dia->values = {1.75, 1.75, 1.75, 1.75};
    // The developer switch itself (registered in PrintConfig, developer mode only).
    // set_key_value creates the option even though full_print_config() may not
    // carry developer-only keys registered outside its static defaults.
    if (auto* sw = config.option<ConfigOptionBool>("enable_overlap_area_carving"))
        sw->value = area_carving;
    else
        config.set_key_value("enable_overlap_area_carving", new ConfigOptionBool{area_carving});
    return config;
}

// Total slice area per print region at the middle layer, sorted ascending.
// Units follow the raw scaled expolygon area; only ratios are asserted.
std::vector<double> sorted_region_areas(Slic3r::PrintObject *object)
{
    const Slic3r::Layer *layer = object->layers()[object->layers().size() / 2];
    std::vector<double> areas(layer->region_count(), 0.0);
    for (size_t r = 0; r < layer->region_count(); ++r)
        for (const Slic3r::Surface &s : layer->get_region(r)->slices.surfaces)
            areas[r] += s.expolygon.area() / (SCALING_FACTOR * SCALING_FACTOR);
    std::sort(areas.begin(), areas.end());
    return areas;
}

std::unique_ptr<Slic3r::Print> slice_two_volume_object(double detail_x, double detail_y, bool detail_first, bool area_carving_on,
                                      double detail_dx = 0., double detail_dy = 0., double depth = 2.)
{
    Slic3r::Model model;
    Slic3r::ModelObject* object = model.add_object();
    auto add_volume = [&](double x, double y, int extruder, double dx, double dy) {
        Slic3r::ModelVolume* volume = object->add_volume(Slic3r::make_cube(x, y, depth));
        if (dx != 0. || dy != 0.)
            volume->set_offset(volume->get_offset() + Slic3r::Vec3d(dx, dy, 0.));
        volume->config.set("extruder", extruder);
        return volume;
    };
    // Region ids follow the volume list order (first encounter), so the
    // first-listed volume owns the smaller region id - relevant for the
    // 2.4.1 near-tie tie-break.
    if (detail_first) {
        add_volume(detail_x, detail_y, 2, detail_dx, detail_dy);
        add_volume(12., 12., 1, 0., 0.);
    } else {
        add_volume(12., 12., 1, 0., 0.);
        add_volume(detail_x, detail_y, 2, detail_dx, detail_dy);
    }
    object->add_instance();

    auto print = std::make_unique<Slic3r::Print>();
    print->set_status_silent();
    print->apply(model, carving_print_config(area_carving_on));
    REQUIRE(print->objects().size() == 1);
    print->get_object(0)->slice();
    return print;
}

} // namespace

TEST_CASE("Overlap carving switch selects between legacy and 2.4.1 arbitration", "[overlap_carving]")
{
    using Catch::Approx;

    // Developer switch semantics, exercised in both states per scenario:
    //   off (default) : upstream legacy, last-listed-wins for every overlap
    //   on            : 2.4.1 per-layer smaller-area rule everywhere, with the
    //                   region-id tie-break inside the 1.5x band
    for (bool area_carving_on : {false, true}) {
        DYNAMIC_SECTION("fully enclosed volume, overlap area carving " << (area_carving_on ? "on" : "off")) {
            // 4x4x2 detail (extruder 2, listed first) fully inside a 12x12x2
            // base (extruder 1): area ratio 9.
            // Switch off: legacy last-listed-wins, the detail yields.
            // Switch on: per-layer smaller-area arbitration keeps the smaller
            // detail region (its interior slices are invisible on the body).
            auto print = slice_two_volume_object(4., 4., /*detail_first=*/true, area_carving_on);
            const std::vector<double> areas = sorted_region_areas(print->get_object(0));
            REQUIRE(areas.size() == 2);
            const double total = areas[0] + areas[1];
            if (area_carving_on) {
                CHECK(areas[0] / total == Approx(16.0 / 144.0).margin(0.01));  // smaller detail region wins
                CHECK(areas[1] / total == Approx(128.0 / 144.0).margin(0.01)); // base carved around it
            } else {
                CHECK(areas[0] / total == Approx(0.0).margin(0.01));   // detail yielded
                CHECK(areas[1] / total == Approx(1.0).margin(0.01));   // body keeps its surface
            }
        }

        DYNAMIC_SECTION("seal-style detail protruding from a later base, overlap area carving " << (area_carving_on ? "on" : "off")) {
            // 4x4x4 detail (extruder 2, listed first) 72% inside a 12x12x4 base
            // (extruder 1), protruding on +X: the TS20261028000011 seal color
            // detail vs surrounding body scenario.
            auto print = slice_two_volume_object(4., 4., /*detail_first=*/true, area_carving_on,
                                                 /*detail_dx=*/9.12, /*detail_dy=*/0., /*depth=*/4.);
            const std::vector<double> areas = sorted_region_areas(print->get_object(0));
            REQUIRE(areas.size() == 2);
            const double total = areas[0] + areas[1];
            if (area_carving_on) {
                // The protruding detail is the smaller region: it wins its full slice.
                CHECK(areas[0] / total == Approx(16.0 / 144.0).margin(0.01));  // detail keeps its full slice
                CHECK(areas[1] / total == Approx(128.0 / 144.0).margin(0.01)); // base is carved around it
            } else {
                // Legacy last-listed-wins: the later base carves the overlap,
                // leaving the detail its protruding part only (16 - 16*0.72 = 4.48).
                CHECK(areas[0] / total == Approx(4.48 / 144.0).margin(0.01));
                CHECK(areas[1] / total == Approx(139.52 / 144.0).margin(0.01));
            }
        }

        DYNAMIC_SECTION("peg partially embedded in an earlier ring, overlap area carving " << (area_carving_on ? "on" : "off")) {
            // 20x20x2 ring (extruder 2, listed first) and a larger 30x30x2 peg
            // (extruder 1, listed later) shifted by (4,4): the TS20261028000011
            // peg scenario. Ring is the smaller region (400 vs 900, ratio 2.25).
            Slic3r::Model model;
            Slic3r::ModelObject* object = model.add_object();
            Slic3r::ModelVolume* ring = object->add_volume(Slic3r::make_cube(20., 20., 2.));
            ring->config.set("extruder", 2);
            Slic3r::ModelVolume* peg = object->add_volume(Slic3r::make_cube(30., 30., 2.));
            peg->set_offset(peg->get_offset() + Slic3r::Vec3d(4., 4., 0.));
            peg->config.set("extruder", 1);
            object->add_instance();

            Slic3r::Print print;
            print.set_status_silent();
            print.apply(model, carving_print_config(area_carving_on));
            REQUIRE(print.objects().size() == 1);
            print.get_object(0)->slice();

            const std::vector<double> areas = sorted_region_areas(print.get_object(0));
            REQUIRE(areas.size() == 2);
            const double total = areas[0] + areas[1];
            if (area_carving_on) {
                // 2.4.1: the smaller earlier ring wins its full slice.
                CHECK(areas[0] / total == Approx(400.0 / 1044.0).margin(0.01));
                CHECK(areas[1] / total == Approx(644.0 / 1044.0).margin(0.01)); // peg carved where the ring is smaller
            } else {
                // Legacy last-listed-wins: the later peg carves the ring.
                CHECK(areas[0] / total == Approx(144.0 / 1044.0).margin(0.01));  // ring yields
                CHECK(areas[1] / total == Approx(900.0 / 1044.0).margin(0.01)); // peg keeps its full slice
            }
        }

        DYNAMIC_SECTION("mere crossing hands the overlap to the smaller region under 2.4.1, overlap area carving " << (area_carving_on ? "on" : "off")) {
            // 20x20x2 ring (extruder 2, listed first) and a larger 30x30x2 peg
            // (extruder 1, listed later) shifted by (12,12): a mere 64mm2
            // crossing. There is no containment gate under the 2.4.1 rule, so
            // even this crossing goes to the smaller region when the switch is
            // on - reproducing 2.4.1's own behavior verbatim, plaque-style
            // regressions included.
            Slic3r::Model model;
            Slic3r::ModelObject* object = model.add_object();
            Slic3r::ModelVolume* ring = object->add_volume(Slic3r::make_cube(20., 20., 2.));
            ring->config.set("extruder", 2);
            Slic3r::ModelVolume* peg = object->add_volume(Slic3r::make_cube(30., 30., 2.));
            peg->set_offset(peg->get_offset() + Slic3r::Vec3d(12., 12., 0.));
            peg->config.set("extruder", 1);
            object->add_instance();

            Slic3r::Print print;
            print.set_status_silent();
            print.apply(model, carving_print_config(area_carving_on));
            REQUIRE(print.objects().size() == 1);
            print.get_object(0)->slice();

            const std::vector<double> areas = sorted_region_areas(print.get_object(0));
            REQUIRE(areas.size() == 2);
            const double total = areas[0] + areas[1];
            if (area_carving_on) {
                // 2.4.1: ring 400 < peg 900, ratio 2.25 - ring keeps its full slice.
                CHECK(areas[0] / total == Approx(400.0 / 1236.0).margin(0.01));
                CHECK(areas[1] / total == Approx(836.0 / 1236.0).margin(0.01));
            } else {
                // Legacy last-listed-wins: the later peg carves the overlap.
                CHECK(areas[0] / total == Approx(336.0 / 1236.0).margin(0.01));  // earlier yields the overlap
                CHECK(areas[1] / total == Approx(900.0 / 1236.0).margin(0.01)); // later keeps its full slice
            }
        }

        DYNAMIC_SECTION("near-tie ratio 1.44 resolves by region id under 2.4.1, overlap area carving " << (area_carving_on ? "on" : "off")) {
            // 12x12x2 base (extruder 1, listed FIRST = smaller region id) and a
            // fully enclosed 10x10x2 detail (extruder 2, listed later). Area
            // ratio 144/100 = 1.44 stays inside the 1.5x band, so 2.4.1 hands
            // the overlap to the smaller region id: the earlier-listed base
            // wins even though it is the bigger region - 2.4.1's own tie-break
            // quirk (the plaque-text band), reproduced verbatim.
            auto print = slice_two_volume_object(10., 10., /*detail_first=*/false, area_carving_on);
            const std::vector<double> areas = sorted_region_areas(print->get_object(0));
            REQUIRE(areas.size() == 2);
            const double total = areas[0] + areas[1];
            if (area_carving_on) {
                // Tie-break: smaller region id (= earlier base) wins.
                CHECK(areas[0] / total == Approx(0.0).margin(0.01));   // later detail fully yields
                CHECK(areas[1] / total == Approx(1.0).margin(0.01));   // earlier base keeps its surface
            } else {
                // Legacy last-listed-wins: the later detail carves the base.
                CHECK(areas[0] / total == Approx(44.0 / 144.0).margin(0.01));
                CHECK(areas[1] / total == Approx(100.0 / 144.0).margin(0.01));
            }
        }
    }
}

// Regression test for the user-reported "switch has no effect": on the real
// seal ticket model (TS20261028000011) the two switch states must disagree.
// Mid layer of the real 3mf, volume list order: white details (extruder 3)
// first, blue body (extruder 1), ..., cream bulk (extruder 4) listed last.
//   switch on  (2.4.1 rule): the blue body owns the body silhouette (smaller
//     region on most carved layers).
//   switch off (legacy):     the later cream bulk paints over everything else.
// Skipped when the model file is not present on this machine.
#include "libslic3r/Format/bbs_3mf.hpp"
#include <boost/filesystem/operations.hpp>

TEST_CASE("Seal regression model ownership follows the carving switch", "[overlap_carving][seal3mf]")
{
    const std::string seal_path = "C:/tmp/e2o/seal_regression_ascii.3mf";
    if (!boost::filesystem::exists(seal_path)) {
        WARN("seal_regression.3mf not present on this machine; skipping");
        return;
    }
    Slic3r::Model model;
    Slic3r::DynamicPrintConfig config;
    Slic3r::ConfigSubstitutionContext ctxt{Slic3r::ForwardCompatibilitySubstitutionRule::Disable};
    Slic3r::PlateDataPtrs plate_data;
    std::vector<Slic3r::Preset*> project_presets;
    bool is_bbl_3mf = false;
    Slic3r::Semver file_version;
    REQUIRE(Slic3r::load_bbs_3mf(seal_path.c_str(), &config, &ctxt, &model, &plate_data, &project_presets,
        &is_bbl_3mf, &file_version, nullptr, Slic3r::LoadStrategy::LoadModel));
    REQUIRE(model.objects.size() == 1);

    for (bool area_carving_on : {false, true}) {
        DYNAMIC_SECTION("seal 3mf, overlap area carving " << (area_carving_on ? "on" : "off")) {
            Slic3r::Print print;
            print.set_status_silent();
            if (auto *sw = config.option<Slic3r::ConfigOptionBool>("enable_overlap_area_carving"))
                sw->value = area_carving_on;
            else
                config.set_key_value("enable_overlap_area_carving", new Slic3r::ConfigOptionBool{area_carving_on});
            print.apply(model, config);
            REQUIRE(print.objects().size() == 1);
            print.get_object(0)->slice();

            const Slic3r::Layer *layer = print.get_object(0)->layers()[print.get_object(0)->layers().size() / 2];
            double total = 0.0;
            std::vector<double> region_areas(layer->region_count(), 0.0);
            std::vector<int> region_extruders(layer->region_count(), 0);
            for (size_t r = 0; r < layer->region_count(); ++r) {
                region_extruders[r] = int(layer->get_region(r)->region().extruder(Slic3r::frExternalPerimeter));
                for (const Slic3r::Surface &s : layer->get_region(r)->slices.surfaces)
                    region_areas[r] += s.expolygon.area() / (SCALING_FACTOR * SCALING_FACTOR);
                total += region_areas[r];
            }
            for (size_t r = 0; r < layer->region_count(); ++r)
                INFO("region " << r << " extruder=" << region_extruders[r] << " area=" << region_areas[r] << " share=" << region_areas[r] / total);
            size_t biggest = 0;
            for (size_t r = 1; r < region_areas.size(); ++r)
                if (region_areas[r] > region_areas[biggest]) biggest = r;
            // The switch must decide who owns the body silhouette: the blue
            // body (extruder 1) under the 2.4.1 rule, the later cream bulk
            // (extruder 4) under the legacy rule. If both states agreed, this
            // is exactly the user-reported "switch has no effect" bug.
            CHECK(region_extruders[biggest] == (area_carving_on ? 1 : 4));
        }
    }
}
