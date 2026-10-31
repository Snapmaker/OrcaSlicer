#include <catch2/catch_test_macros.hpp>

#include "libslic3r/EmbeddedDetailCarving.hpp"

using namespace Slic3r;

namespace {

ExPolygon square(coord_t cx, coord_t cy, coord_t size)
{
    ExPolygon out;
    const coord_t h = size / 2;
    out.contour = Polygon({
        Point(cx - h, cy - h),
        Point(cx + h, cy - h),
        Point(cx + h, cy + h),
        Point(cx - h, cy + h),
    });
    return out;
}

ExPolygons make(const ExPolygon &p)
{
    ExPolygons out;
    out.emplace_back(p);
    return out;
}

} // namespace

TEST_CASE("Embedded detail carving decisions", "[embedded_detail]")
{
    SECTION("Earlier region fully covered by a much larger current region is an embedded detail") {
        // Embossed text / color patch imported before its base plate.
        auto earlier = make(square(0, 0, 1000000));
        auto current = make(square(0, 0, 4000000));
        REQUIRE(earlier_is_embedded_detail(earlier, current));
    }

    SECTION("Partially overlapping regions keep the upstream last-listed-wins semantics") {
        // Overlap is ~10% of the earlier region (peg in a notch / text over a mountain).
        auto earlier = make(square(0, 0, 1000000));
        auto current = make(square(1200000, 0, 1500000));
        REQUIRE_FALSE(earlier_is_embedded_detail(earlier, current));
    }

    SECTION("Equally sized fully overlapping regions are not treated as embedded") {
        // The current region must be strictly larger to embed the earlier one.
        auto earlier = make(square(0, 0, 1000000));
        auto current = make(square(0, 0, 1000000));
        REQUIRE_FALSE(earlier_is_embedded_detail(earlier, current));
    }

    SECTION("Current region smaller than the earlier one is never an embedded detail") {
        auto earlier = make(square(0, 0, 4000000));
        auto current = make(square(0, 0, 1000000));
        REQUIRE_FALSE(earlier_is_embedded_detail(earlier, current));
    }

    SECTION("Empty earlier region is never an embedded detail") {
        ExPolygons earlier;
        auto current = make(square(0, 0, 4000000));
        REQUIRE_FALSE(earlier_is_embedded_detail(earlier, current));
    }
}
