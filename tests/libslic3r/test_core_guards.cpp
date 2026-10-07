// Regression tests for the crash / UB guards ported from OrcaSlicer in batch 1A.
#include <catch2/catch.hpp>

#include "libslic3r/Arachne/SkeletalTrapezoidation.hpp"

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
