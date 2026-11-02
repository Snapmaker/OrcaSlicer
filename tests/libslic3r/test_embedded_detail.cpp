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
    SECTION("Cube fully enclosed in a larger cube yields to it (hidden internals lose)") {
        // Seal case: color patches fully hidden inside the body shell must not
        // carve the body - the enclosing part (blue body) keeps its surface.
        auto earlier = cube(4., 4., 4.);
        auto current = cube(12., 12., 12.);
        REQUIRE_FALSE(earlier_is_embedded_detail(earlier.its, current.its));
    }

    SECTION("Detail protruding from a larger cube (72% contained) is an embedded detail") {
        // The seal ticket scenario: a small color detail whose volume mostly lives
        // inside another part but which pokes out - per-layer 2D coverage dips well
        // below 90%, yet the 3D containment stays high (regression guard for the
        // TS20261028000011 follow-up).
        auto earlier = translated(cube(4., 4., 4.), 9.12, 0., 0.);
        auto current = cube(12., 12., 12.);
        REQUIRE(earlier_is_embedded_detail(earlier.its, current.its));
    }

    SECTION("Band lower edge: containment at the 0.4 boundary still flips") {
        // overlap = (12-9.44)*4*4 / 64 = 0.40
        auto earlier = translated(cube(4., 4., 4.), 9.44, 0., 0.);
        auto current = cube(12., 12., 12.);
        REQUIRE(earlier_is_embedded_detail(earlier.its, current.its));
    }

    SECTION("Partially overlapping cubes keep the upstream last-listed-wins semantics") {
        // Overlap is ~2.5% of the earlier cube (peg crossing a wall).
        auto earlier = cube(20., 20., 2.);
        auto current = translated(cube(30., 30., 2.), 12., 12., 0.);
        REQUIRE_FALSE(earlier_is_embedded_detail(earlier.its, current.its));
    }

    SECTION("Current smaller than the earlier one is never an embedded detail") {
        auto earlier = cube(12., 12., 12.);
        auto current = cube(4., 4., 4.);
        REQUIRE_FALSE(earlier_is_embedded_detail(earlier.its, current.its));
    }

    SECTION("Disjoint cubes are never an embedded detail") {
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
    //   on            : protruding embedded details win (partial band),
    //                   fully hidden volumes still yield to the body
    for (bool containment_on : {false, true}) {
        DYNAMIC_SECTION("fully enclosed volume, containment carving " << (containment_on ? "on" : "off")) {
            // 4x4x2 detail (extruder 2, listed first) fully inside a 12x12x2
            // base (extruder 1): 3D containment is 1.0 (>= FULL), so the hidden
            // inner volume yields to the body in both switch states.
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
            CHECK(areas[0] / total == Approx(0.0).margin(0.01));   // detail yielded
            CHECK(areas[1] / total == Approx(1.0).margin(0.01));   // body keeps its surface
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
                CHECK(areas[0] / total == Approx(400.0 / 1044.0).margin(0.01));  // ring keeps its full slice
                CHECK(areas[1] / total == Approx(644.0 / 1044.0).margin(0.01)); // peg is carved
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
