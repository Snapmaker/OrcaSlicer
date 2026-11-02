#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/EmbeddedDetailCarving.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

namespace {

TriangleMesh cube(double x, double y, double z) { return make_cube(x, y, z); }

TriangleMesh translated(TriangleMesh m, double dx, double dy, double dz)
{
    for (auto &v : m.its.vertices) {
        v.x() += float(dx);
        v.y() += float(dy);
        v.z() += float(dz);
    }
    return m;
}

} // namespace

TEST_CASE("Embedded detail containment decisions", "[embedded_detail]")
{
    SECTION("Cube fully enclosed in a larger cube is an embedded pair") {
        // Seal case: color details fully or nearly fully inside the body are
        // embedded - per-layer smaller-area arbitration decides them.
        auto earlier = cube(4., 4., 4.);
        auto current = cube(12., 12., 12.);
        REQUIRE(earlier_is_embedded_detail(earlier.its, current.its));
    }

    SECTION("Detail protruding from a larger cube (72% contained) is an embedded pair") {
        // The seal ticket scenario: a small color detail whose volume mostly lives
        // inside another part but which pokes out - per-layer 2D coverage dips well
        // below 90%, yet the 3D containment stays high (regression guard for the
        // TS20261028000011 follow-up).
        auto earlier = translated(cube(4., 4., 4.), 9.12, 0., 0.);
        auto current = cube(12., 12., 12.);
        REQUIRE(earlier_is_embedded_detail(earlier.its, current.its));
    }

    SECTION("Containment just above the gate (0.62) is an embedded pair") {
        // overlap = (12-9.52)*4*4 / 64 = 0.62
        auto earlier = translated(cube(4., 4., 4.), 9.52, 0., 0.);
        auto current = cube(12., 12., 12.);
        REQUIRE(earlier_is_embedded_detail(earlier.its, current.its));
    }

    SECTION("Containment just below the gate (0.52) keeps upstream semantics") {
        // overlap = (12-9.92)*4*4 / 64 = 0.52 - e.g. the plaque relief case (0.45)
        // must stay a crossing so the later-listed text keeps winning.
        auto earlier = translated(cube(4., 4., 4.), 9.92, 0., 0.);
        auto current = cube(12., 12., 12.);
        REQUIRE_FALSE(earlier_is_embedded_detail(earlier.its, current.its));
    }

    SECTION("Partially overlapping cubes keep the upstream last-listed-wins semantics") {
        // Overlap is ~2.5% of the earlier cube (peg crossing a wall).
        auto earlier = cube(20., 20., 2.);
        auto current = translated(cube(30., 30., 2.), 12., 12., 0.);
        REQUIRE_FALSE(earlier_is_embedded_detail(earlier.its, current.its));
    }

    SECTION("Current smaller than the earlier one is never an embedded pair") {
        auto earlier = cube(12., 12., 12.);
        auto current = cube(4., 4., 4.);
        REQUIRE_FALSE(earlier_is_embedded_detail(earlier.its, current.its));
    }

    SECTION("Disjoint cubes are never an embedded pair") {
        auto earlier = cube(4., 4., 4.);
        auto current = translated(cube(12., 12., 12.), 50., 0., 0.);
        REQUIRE_FALSE(earlier_is_embedded_detail(earlier.its, current.its));
    }
}

// Pipeline-level check: two overlapping volumes with different extruders inside one
// object, sliced through PrintObject::slice() - the exact path slices_to_regions()
// arbitrates. Slices are raw layer geometry, so region areas are exact box areas.
// The developer switch (enable_volume_containment_carving, a developer-mode
// PrintConfig option) is exercised in both states per scenario.
#include "libslic3r/Layer.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Print.hpp"

#include <algorithm>

namespace {

// Matches production: the GUI enables mixed-filament auto generation during
// AppConfig startup.
static const int _auto_generate_enabler = []() {
    Slic3r::MixedFilamentManager::set_auto_generate_enabled(true);
    return 0;
}();

DynamicPrintConfig carving_print_config(bool containment_carving)
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
    if (auto* sw = config.option<ConfigOptionBool>("enable_volume_containment_carving"))
        sw->value = containment_carving;
    // set_key_value creates the option even though full_print_config() does not
    // carry developer-only keys registered outside its static defaults.
    return config;
}

// Total slice area per print region at the middle layer, sorted ascending.
std::vector<double> sorted_region_areas_mm2(Slic3r::PrintObject *object)
{
    const Slic3r::Layer *layer = object->layers()[object->layers().size() / 2];
    std::vector<double> areas(layer->region_count(), 0.0);
    for (size_t r = 0; r < layer->region_count(); ++r)
        for (const Slic3r::Surface &s : layer->get_region(r)->slices.surfaces)
            areas[r] += s.expolygon.area() / (SCALING_FACTOR * SCALING_FACTOR);
    std::sort(areas.begin(), areas.end());
    return areas;
}

} // namespace

TEST_CASE("Overlap carving pipeline assigns overlap to the correct region", "[embedded_detail]")
{
    using Catch::Approx;

    // Developer switch semantics, exercised in both states per scenario:
    //   off (default) : upstream last-listed-wins for every overlap
    //   on            : embedded pairs (3D containment >= gate) are carved by
    //                   the smaller per-layer slice area; crossings stay upstream
    for (bool containment_on : {false, true}) {
        DYNAMIC_SECTION("fully enclosed volume, containment carving " << (containment_on ? "on" : "off")) {
            // 4x4x2 detail (extruder 2, listed first) fully inside a 12x12x2
            // base (extruder 1): 3D containment is 1.0 - an embedded pair.
            // Switch off: upstream last-listed-wins, the detail yields.
            // Switch on: per-layer smaller-area arbitration keeps the smaller
            // detail region (its interior slices are invisible on the body).
            Slic3r::Model model;
            Slic3r::ModelObject* object = model.add_object();
            Slic3r::ModelVolume* detail = object->add_volume(Slic3r::make_cube(4., 4., 2.));
            detail->config.set("extruder", 2);
            Slic3r::ModelVolume* base = object->add_volume(Slic3r::make_cube(12., 12., 2.));
            base->config.set("extruder", 1);
            object->add_instance();

            Slic3r::Print print;
            print.set_status_silent();
            print.apply(model, carving_print_config(containment_on));
            REQUIRE(print.objects().size() == 1);
            print.get_object(0)->slice();

            const std::vector<double> areas = sorted_region_areas_mm2(print.get_object(0));
            REQUIRE(areas.size() == 2);
            const double total = areas[0] + areas[1];
            if (containment_on) {
                CHECK(areas[0] / total == Approx(16.0 / 144.0).margin(0.01));  // smaller detail region wins
                CHECK(areas[1] / total == Approx(128.0 / 144.0).margin(0.01)); // base carved around it
            } else {
                CHECK(areas[0] / total == Approx(0.0).margin(0.01));   // detail yielded
                CHECK(areas[1] / total == Approx(1.0).margin(0.01));   // body keeps its surface
            }
        }

        DYNAMIC_SECTION("seal-style detail protruding from a later base, containment carving " << (containment_on ? "on" : "off")) {
            // 4x4x4 detail (extruder 2, listed first) 72% inside a 12x12x4 base
            // (extruder 1), protruding on +X: per-layer 2D coverage is 72%, below
            // the old area threshold - the TS20261028000011 follow-up regression
            // guard (seal color detail vs surrounding body).
            Slic3r::Model model;
            Slic3r::ModelObject* object = model.add_object();
            Slic3r::ModelVolume* detail = object->add_volume(Slic3r::make_cube(4., 4., 4.));
            detail->set_offset(detail->get_offset() + Slic3r::Vec3d(9.12, 0., 0.));
            detail->config.set("extruder", 2);
            Slic3r::ModelVolume* base = object->add_volume(Slic3r::make_cube(12., 12., 4.));
            base->config.set("extruder", 1);
            object->add_instance();

            Slic3r::Print print;
            print.set_status_silent();
            print.apply(model, carving_print_config(containment_on));
            REQUIRE(print.objects().size() == 1);
            print.get_object(0)->slice();

            const std::vector<double> areas = sorted_region_areas_mm2(print.get_object(0));
            REQUIRE(areas.size() == 2);
            const double total = areas[0] + areas[1];
            if (containment_on) {
                // Partial band: the protruding detail wins its full slice.
                CHECK(areas[0] / total == Approx(16.0 / 144.0).margin(0.01));  // detail keeps its full slice
                CHECK(areas[1] / total == Approx(128.0 / 144.0).margin(0.01)); // base is carved around it
            } else {
                // Upstream last-listed-wins: the later base carves the overlap,
                // leaving the detail its protruding part only (16 - 16*0.72 = 4.48 mm2).
                CHECK(areas[0] / total == Approx(4.48 / 144.0).margin(0.01));
                CHECK(areas[1] / total == Approx(139.52 / 144.0).margin(0.01));
            }
        }

        SECTION("Peg partially embedded in an earlier ring") {
            // 20x20x2 ring (extruder 2, listed first) and a larger 30x30x2 peg
            // (extruder 1, listed later) shifted by (4,4): 3D containment of the
            // ring in the peg is ~0.64, inside the partial band - the ring wins.
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
            print.apply(model, carving_print_config(containment_on));
            REQUIRE(print.objects().size() == 1);
            print.get_object(0)->slice();

            const std::vector<double> areas = sorted_region_areas_mm2(print.get_object(0));
            REQUIRE(areas.size() == 2);
            const double total = areas[0] + areas[1];
            if (containment_on) {
                // Partial band: the protruding earlier ring wins its full slice.
                CHECK(areas[0] / total == Approx(400.0 / 1044.0).margin(0.01));  // ring is the smaller region and keeps its full slice
                CHECK(areas[1] / total == Approx(644.0 / 1044.0).margin(0.01)); // peg carved where the ring is smaller
            } else {
                // Upstream last-listed-wins: the later peg carves the ring.
                CHECK(areas[0] / total == Approx(144.0 / 1044.0).margin(0.01));  // ring yields
                CHECK(areas[1] / total == Approx(900.0 / 1044.0).margin(0.01)); // peg keeps its full slice
            }
        }

        SECTION("Mere crossing keeps last-listed-wins in both switch states") {
            // 20x20x2 ring (extruder 2, listed first) and a larger 30x30x2 peg
            // (extruder 1, listed later) shifted by (12,12): 3D containment is
            // only ~0.16, below the partial band - crossing, later wins.
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
            print.apply(model, carving_print_config(containment_on));
            REQUIRE(print.objects().size() == 1);
            print.get_object(0)->slice();

            const std::vector<double> areas = sorted_region_areas_mm2(print.get_object(0));
            REQUIRE(areas.size() == 2);
            const double total = areas[0] + areas[1];
            CHECK(areas[0] / total == Approx(336.0 / 1236.0).margin(0.01));  // earlier yields the overlap
            CHECK(areas[1] / total == Approx(900.0 / 1236.0).margin(0.01)); // later keeps its full slice
        }
    }
}


// Regression diagnostic for the real seal ticket model (TS20261028000011 follow-up):
// with containment carving on, the blue body (extruder 1, volume listed before the
// cream bulk, extruder 4) must own the body silhouette slices. Skipped when the
// model file is not present on this machine.
#include "libslic3r/Format/bbs_3mf.hpp"
#include <boost/filesystem/operations.hpp>

TEST_CASE("Seal regression model ownership with containment carving", "[embedded_detail][seal3mf]")
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

    Slic3r::Print print;
    print.set_status_silent();
    if (auto *sw = config.option<Slic3r::ConfigOptionBool>("enable_volume_containment_carving"))
        sw->value = true;
    else
        config.set_key_value("enable_volume_containment_carving", new Slic3r::ConfigOptionBool{true});
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
    // The blue body (extruder 1) must own the bulk of the body silhouette.
    size_t biggest = 0;
    for (size_t r = 1; r < region_areas.size(); ++r)
        if (region_areas[r] > region_areas[biggest]) biggest = r;
    CHECK(region_extruders[biggest] == 1);
}
