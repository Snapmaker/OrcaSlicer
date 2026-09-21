#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Point.hpp"
#include "libslic3r/Support/TreeSupport.hpp"

using namespace Slic3r;

namespace {
ExPolygon rect_mm(double min_x, double min_y, double w, double h)
{
    ExPolygon out;
    out.contour.points = {
        Point(scale_(min_x), scale_(min_y)),
        Point(scale_(min_x + w), scale_(min_y)),
        Point(scale_(min_x + w), scale_(min_y + h)),
        Point(scale_(min_x), scale_(min_y + h)),
    };
    return out;
}

// Holes are stored clockwise in ExPolygon::holes, same as the real area-group data.
Polygon hole_rect_mm(double min_x, double min_y, double w, double h)
{
    Polygon hole;
    hole.points = {
        Point(scale_(min_x), scale_(min_y)),
        Point(scale_(min_x), scale_(min_y + h)),
        Point(scale_(min_x + w), scale_(min_y + h)),
        Point(scale_(min_x + w), scale_(min_y)),
    };
    return hole;
}
} // namespace

// Regression for support transition layer interference: the per-layer carve pass leaves
// transition strips / roof fragments as holes inside other groups' polygons. The plain
// sub-2mm sliver cleanup erased those carve holes and re-overlapped the groups, so the
// interface fill printed across transition strips at the same print_z.
TEST_CASE("Tree support area group small-hole cleanup", "[TreeSupport]")
{
    SECTION("Carve hole owned by another group is kept")
    {
        ExPolygon interface_poly = rect_mm(0., 0., 10., 10.);
        // A 1 x 0.8mm transition strip carved out of the interface polygon.
        interface_poly.holes.emplace_back(hole_rect_mm(2., 2., 1., 0.8));
        ExPolygon strip = rect_mm(2., 2., 1., 0.8);

        std::vector<const ExPolygon*> groups{&interface_poly, &strip};
        erase_small_area_group_holes(interface_poly, groups);

        REQUIRE(interface_poly.holes.size() == 1);
    }

    SECTION("Sliver hole owned by nobody is erased")
    {
        ExPolygon poly = rect_mm(0., 0., 10., 10.);
        poly.holes.emplace_back(hole_rect_mm(2., 2., 1., 1.));

        std::vector<const ExPolygon*> groups{&poly};
        erase_small_area_group_holes(poly, groups);

        REQUIRE(poly.holes.empty());
    }

    SECTION("Hole only touching another group's boundary is erased")
    {
        ExPolygon poly = rect_mm(0., 0., 10., 10.);
        poly.holes.emplace_back(hole_rect_mm(2., 2., 1., 1.));
        // Neighbor shares the x=3 edge with the hole but does not cover any of it.
        ExPolygon neighbor = rect_mm(3., 2., 1., 1.);

        std::vector<const ExPolygon*> groups{&poly, &neighbor};
        erase_small_area_group_holes(poly, groups);

        REQUIRE(poly.holes.empty());
    }

    SECTION("Large hole is never evaluated")
    {
        ExPolygon poly = rect_mm(0., 0., 10., 10.);
        poly.holes.emplace_back(hole_rect_mm(2., 2., 3., 3.));
        ExPolygon strip = rect_mm(3., 2.5, 1., 1.);

        std::vector<const ExPolygon*> groups{&poly, &strip};
        erase_small_area_group_holes(poly, groups);

        REQUIRE(poly.holes.size() == 1);
    }
}
