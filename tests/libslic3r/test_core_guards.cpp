// Regression tests for the crash / UB guards ported from OrcaSlicer in batch 1A.
#include <catch2/catch.hpp>

// MultiMaterialSegmentation.hpp declares boost::polygon traits for ColoredLine, so its
// geometry/boost dependencies must be included first.
#include <boost/polygon/polygon.hpp>
#include "libslic3r/Line.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/MultiMaterialSegmentation.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/Arachne/SkeletalTrapezoidation.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

using namespace Slic3r;
using namespace Slic3r::Arachne;

namespace {
// Exposes the protected static interpolate() for a focused unit test.
struct InterpolateProbe : SkeletalTrapezoidation {
    using SkeletalTrapezoidation::interpolate;
};
} // anonymous namespace

// Orca #14656: interpolate() indexes the merged beading with an index derived from `left`. The
// merged beading follows the thicker of left/right, so when the thicker side has fewer insets the
// index ran past its end.
TEST_CASE("Beading interpolation tolerates a thicker side with fewer insets", "[CoreGuards][Arachne]")
{
    using Beading = BeadingStrategy::Beading;

    const coord_t w = scaled<coord_t>(0.42);
    Beading left;
    left.total_thickness = scaled<coord_t>(1.0);
    left.bead_widths = { w, w, w, w };
    left.toolpath_locations = { scaled<coord_t>(0.1), scaled<coord_t>(0.3), scaled<coord_t>(0.5), scaled<coord_t>(0.7) };
    left.left_over = 0;

    // Thicker side (right) has fewer insets, so the merged beading holds only 2 toolpath locations.
    Beading right;
    right.total_thickness = scaled<coord_t>(2.0);
    right.bead_widths = { w, w };
    right.toolpath_locations = { scaled<coord_t>(0.1), scaled<coord_t>(0.3) };
    right.left_over = 0;

    // Just past left's location [2] (0.5), so the derived index is 2, past the end of the 2-inset merged beading.
    const coord_t switching_radius = scaled<coord_t>(0.6);

    Beading result;
    REQUIRE_NOTHROW(result = InterpolateProbe::interpolate(left, 0.5, right, switching_radius));

    // With the guard the adjustment is skipped, so the result is the plain interpolation.
    const Beading expected = InterpolateProbe::interpolate(left, 0.5, right);
    REQUIRE(result.toolpath_locations.size() == expected.toolpath_locations.size());
    REQUIRE(result.bead_widths.size() == expected.bead_widths.size());
    for (size_t i = 0; i < expected.toolpath_locations.size(); ++i) {
        CHECK(result.toolpath_locations[i] == expected.toolpath_locations[i]);
        CHECK(result.bead_widths[i] == expected.bead_widths[i]);
    }
}

// Orca #14455: the outer-wall line width used by multi-material segmentation was read straight from
// the region config (0 = "auto" produced zero spacings) and against nozzle 0 instead of the nozzle
// the outer wall prints with.
TEST_CASE("Multi-material segmentation resolves the outer-wall line width", "[CoreGuards][MultiMaterialSegmentation]")
{
    struct Case
    {
        std::string         description;
        double              outer_value;
        bool                outer_percent;
        double              line_value;
        bool                line_percent;
        std::vector<double> nozzle_diameters;
        int                 wall_filament;
        int                 outer_wall_filament;
        double              expected;
    };

    auto c = GENERATE(values<Case>({
        {"absolute outer-wall width is used as-is",      0.6, false, 0.42, false, {0.4},      1, 0, 0.6},
        {"percent outer-wall width uses the nozzle",     120, true,  0.42, false, {0.5},      1, 0, 0.6},
        {"zero outer-wall width uses the line width",    0,   false, 0.5,  false, {0.4},      1, 0, 0.5},
        {"zero outer-wall width uses a percent line",    0,   false, 100,  true,  {0.5},      1, 0, 0.5},
        {"zero width falls back to auto",                0,   false, 0,    false, {0.4},      1, 0, Flow::auto_extrusion_width(frExternalPerimeter, 0.4f)},
        {"the auto fallback scales with the nozzle",     0,   false, 0,    false, {0.6},      1, 0, Flow::auto_extrusion_width(frExternalPerimeter, 0.6f)},
        {"a percent width uses the wall's nozzle",       120, true,  0.42, false, {0.4, 0.8}, 2, 0, 0.96},
        {"a percent width uses the outer wall's nozzle", 120, true,  0.42, false, {0.4, 0.8}, 2, 1, 0.48},
        {"the auto width uses the wall's nozzle",        0,   false, 0,    false, {0.4, 0.8}, 2, 0, Flow::auto_extrusion_width(frExternalPerimeter, 0.8f)},
        {"an absolute width ignores the nozzle",         0.6, false, 0.42, false, {0.4, 0.8}, 2, 0, 0.6},
        {"a zero percent width uses the line width",     0,   true,  0.5,  false, {0.4},      1, 0, 0.5},
        {"an unset filament id uses the first nozzle",   0,   false, 0,    false, {0.4, 0.8}, 0, 0, Flow::auto_extrusion_width(frExternalPerimeter, 0.4f)},
        {"an out-of-range filament id uses nozzle 1",    0,   false, 0,    false, {0.4, 0.8}, 5, 0, Flow::auto_extrusion_width(frExternalPerimeter, 0.4f)},
    }));

    DYNAMIC_SECTION(c.description)
    {
        PrintConfig print_config;
        print_config.nozzle_diameter.values   = c.nozzle_diameters;
        print_config.filament_diameter.values = std::vector<double>(c.nozzle_diameters.size(), 1.75);

        PrintObjectConfig object_config;
        object_config.line_width = ConfigOptionFloatOrPercent(c.line_value, c.line_percent);

        PrintRegionConfig region_config;
        region_config.outer_wall_line_width     = ConfigOptionFloatOrPercent(c.outer_value, c.outer_percent);
        region_config.wall_filament.value       = c.wall_filament;
        region_config.outer_wall_filament.value = c.outer_wall_filament;

        REQUIRE(resolve_outer_wall_line_width(region_config, object_config, print_config) == Approx(c.expected).margin(1e-6));
    }
}

// Orca #13450: an empty ExtrusionLoop (reachable from the ZAA / ContourZ pass) crashed as_polyline()
// through Polygon::split_at_first_point() on an empty polygon.
TEST_CASE("Empty extrusion loop and polygon split cleanly", "[CoreGuards][ExtrusionLoop]")
{
    ExtrusionLoop loop;
    CHECK(loop.as_polyline().empty());
    Polylines polylines;
    loop.collect_polylines(polylines);
    CHECK(polylines.empty());

    Polygon empty;
    CHECK(empty.split_at_index(0).empty());
}

// Orca #15399: BoundingBoxWrapper::centroid() lacked parentheses and returned min + max / 2.
TEST_CASE("AABB tree bounding box wrapper centroid is the box centre", "[CoreGuards][AABBIndirect]")
{
    const BoundingBox bbox(Point(1000, 2000), Point(3000, 6000));
    const AABBTreeIndirect::BoundingBoxWrapper wrapper(0, bbox);
    // The wrapper inflates the box symmetrically, so the centre is unchanged.
    CHECK(wrapper.centroid() == Point(2000, 4000));
}

// Orca #11476: cut_mesh() leaked one heap Vec3f per vertex lying on the cutting plane. The leak is
// not observable from a test; this pins that a cut still produces the two halves after the map moved
// to values.
TEST_CASE("cut_mesh splits a cube", "[CoreGuards][Cut]")
{
    const indexed_triangle_set cube = make_cube(10., 10., 10.).its;
    for (float z : { 2.5f, 5.f }) {
        indexed_triangle_set upper, lower;
        cut_mesh(cube, z, &upper, &lower, true);
        INFO("cut at z = " << z);
        CHECK(its_volume(upper) == Approx(10. * 10. * (10. - z)).margin(1e-3));
        CHECK(its_volume(lower) == Approx(10. * 10. * z).margin(1e-3));
    }
}

// Orca #15877: ConfigOptionVector::resize() passed a reference to values.front() to
// std::vector::resize(), which may reallocate before it copies the value.
TEST_CASE("ConfigOptionVector resize duplicates the first value across a reallocation", "[CoreGuards][Config]")
{
    ConfigOptionStrings strings(std::vector<std::string>{ std::string(200, 'x') });
    strings.values.shrink_to_fit();
    strings.resize(64);
    REQUIRE(strings.values.size() == 64);
    for (const std::string &s : strings.values)
        CHECK(s == std::string(200, 'x'));

    ConfigOptionFloats floats(std::vector<double>{ 1.5 });
    floats.resize(1000);
    REQUIRE(floats.values.size() == 1000);
    CHECK(floats.values.back() == 1.5);

    // apply_override() with a nullable rhs is the second resize site.
    ConfigOptionFloats         dst(std::vector<double>{ 2.0 });
    ConfigOptionFloatsNullable src(std::vector<double>(500, 3.0));
    dst.apply_override(&src);
    REQUIRE(dst.values.size() == 500);
    CHECK(dst.values.back() == 3.0);
}

// Orca #12406: GCodeWriter::set_extruders() took max_element() of an empty id list (a calibration
// pattern with nothing to print).
TEST_CASE("GCodeWriter::set_extruders accepts an empty extruder list", "[CoreGuards][GCodeWriter]")
{
    GCodeWriter writer;
    writer.set_extruders({ 0, 1 });
    CHECK(writer.multiple_extruders);
    REQUIRE_NOTHROW(writer.set_extruders({}));
    CHECK_FALSE(writer.multiple_extruders);
    CHECK(writer.extruders().empty());
    CHECK(writer.extruder() == nullptr);
}
