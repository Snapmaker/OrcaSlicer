#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/ClipperZUtils.hpp"

using namespace Slic3r;

SCENARIO("Converted Perl tests", "[Polygon]") {
    GIVEN("ccw_square") {
        Polygon ccw_square{ { 100, 100 }, { 200, 100 }, { 200, 200 }, { 100, 200 } };
        Polygon cw_square(ccw_square);
        cw_square.reverse();

        THEN("ccw_square is valid") {
            REQUIRE(ccw_square.is_valid());
        }
        THEN("cw_square is valid") {
            REQUIRE(cw_square.is_valid());
        }
        THEN("ccw_square.area") {
            REQUIRE(ccw_square.area() == 100 * 100);
        }
        THEN("cw_square.area") {
            REQUIRE(cw_square.area() == - 100 * 100);
        }
        THEN("ccw_square.centroid") {
            REQUIRE(ccw_square.centroid() == Point { 150, 150 });
        }
        THEN("cw_square.centroid") {
            REQUIRE(cw_square.centroid() == Point { 150, 150 });
        }
        THEN("ccw_square.contains_point(150, 150)") {
            REQUIRE(ccw_square.contains({ 150, 150 }));
        }
        THEN("cw_square.contains_point(150, 150)") {
            REQUIRE(cw_square.contains({ 150, 150 }));
        }
        THEN("conversion to lines") {
            REQUIRE(ccw_square.lines() == Lines{
                { { 100, 100 }, { 200, 100 } },
                { { 200, 100 }, { 200, 200 } },
                { { 200, 200 }, { 100, 200 } },
                { { 100, 200 }, { 100, 100 } } });
        }
        THEN("split_at_first_point") {
            REQUIRE(ccw_square.split_at_first_point() == Polyline { ccw_square[0], ccw_square[1], ccw_square[2], ccw_square[3], ccw_square[0] });
        }
        THEN("split_at_index(2)") {
            REQUIRE(ccw_square.split_at_index(2) == Polyline { ccw_square[2], ccw_square[3], ccw_square[0], ccw_square[1], ccw_square[2] });
        }
        THEN("split_at_vertex(ccw_square[2])") {
            REQUIRE(ccw_square.split_at_vertex(ccw_square[2]) == Polyline { ccw_square[2], ccw_square[3], ccw_square[0], ccw_square[1], ccw_square[2] });
        }
        THEN("is_counter_clockwise") {
            REQUIRE(ccw_square.is_counter_clockwise());
        }
        THEN("! is_counter_clockwise") {
            REQUIRE(! cw_square.is_counter_clockwise());
        }
        THEN("make_counter_clockwise") {
            cw_square.make_counter_clockwise();
            REQUIRE(cw_square.is_counter_clockwise());
        }
        THEN("make_counter_clockwise^2") {
            cw_square.make_counter_clockwise();
            cw_square.make_counter_clockwise();
            REQUIRE(cw_square.is_counter_clockwise());
        }
        THEN("first_point") {
            REQUIRE(&ccw_square.first_point() == &ccw_square.points.front());
        }
    }
    GIVEN("Triangulating hexagon") {
        Polygon hexagon{ { 100, 0 } };
        for (size_t i = 1; i < 6; ++ i) {
            Point p = hexagon.points.front();
            p.rotate(PI / 3 * i);
            hexagon.points.emplace_back(p);
        }
        Polygons triangles;
        hexagon.triangulate_convex(&triangles);
        THEN("right number of triangles") {
            REQUIRE(triangles.size() == 4);
        }
        THEN("all triangles are ccw") {
            auto it = std::find_if(triangles.begin(), triangles.end(), [](const Polygon &tri) { return tri.is_clockwise(); });
            REQUIRE(it == triangles.end());
        }
    }
    GIVEN("General triangle") {
        Polygon polygon { { 50000000, 100000000 }, { 300000000, 102000000 }, { 50000000, 104000000 } };
        Line    line { { 175992032, 102000000 }, { 47983964, 102000000 } };
        Point   intersection;
        bool    has_intersection = polygon.intersection(line, &intersection);
        THEN("Intersection with line") {
            REQUIRE(has_intersection);
            REQUIRE(intersection == Point { 50000000, 102000000 });
        }
    }
}

TEST_CASE("Centroid of Trapezoid must be inside", "[Polygon][Utils]")
{
    Slic3r::Polygon trapezoid {
        { 4702134, 1124765853 },
        { -4702134, 1124765853 },
        { -9404268, 1049531706 },
        { 9404268, 1049531706 },
    };
    Point centroid = trapezoid.centroid();
    CHECK(trapezoid.contains(centroid));
}

// This test currently only covers remove_collinear_points.
// All remaining tests are to be ported from xs/t/06_polygon.t

Slic3r::Points collinear_circle({
    Slic3r::Point::new_scale(0, 0), // 3 collinear points at beginning
    Slic3r::Point::new_scale(10, 0),
    Slic3r::Point::new_scale(20, 0),
    Slic3r::Point::new_scale(30, 10),
    Slic3r::Point::new_scale(40, 20), // 2 collinear points
    Slic3r::Point::new_scale(40, 30),
    Slic3r::Point::new_scale(30, 40), // 3 collinear points
    Slic3r::Point::new_scale(20, 40),
    Slic3r::Point::new_scale(10, 40),
    Slic3r::Point::new_scale(-10, 20),
    Slic3r::Point::new_scale(-20, 10),
    Slic3r::Point::new_scale(-20, 0), // 3 collinear points at end
    Slic3r::Point::new_scale(-10, 0),
    Slic3r::Point::new_scale(-5, 0)
});

SCENARIO("Remove collinear points from Polygon", "[Polygon]") {
    GIVEN("Polygon with collinear points"){
        Slic3r::Polygon p(collinear_circle);
        WHEN("collinear points are removed") {
            remove_collinear(p);
            THEN("Leading collinear points are removed") {
                REQUIRE(p.points.front() == Slic3r::Point::new_scale(20, 0));
            }
            THEN("Trailing collinear points are removed") {
                REQUIRE(p.points.back() == Slic3r::Point::new_scale(-20, 0));
            }
            THEN("Number of remaining points is correct") {
                REQUIRE(p.points.size() == 7);
            }
        }
    }
}

TEST_CASE("point_in_polygon handles large coordinates without int64 overflow", "[Polygon]")
{
    auto check_inside = [](const Polygon &poly, const Point &pt) {
        CHECK(contains(poly, pt, true));
        CHECK(contains(poly, pt, false));
    };
    auto check_boundary = [](const Polygon &poly, const Point &pt) {
        CHECK(contains(poly, pt, true));
        CHECK_FALSE(contains(poly, pt, false));
    };
    auto check_outside = [](const Polygon &poly, const Point &pt) {
        CHECK_FALSE(contains(poly, pt, true));
        CHECK_FALSE(contains(poly, pt, false));
    };

    SECTION("3.3e9 scale triangle (cross product > INT64_MAX)") {
        constexpr coord_t S = 3300000000LL;
        constexpr coord_t H = S / 2;
        const Polygon ccw_tri{ { S, 0 }, { 0, S }, { 0, 0 } };
        Polygon       cw_tri(ccw_tri);
        cw_tri.reverse();

        for (const Polygon &tri : { ccw_tri, cw_tri }) {
            // Strictly inside (including points where raw int64 cross product overflows)
            check_inside(tri, { 100000000, 100000000 });
            check_inside(tri, { 1000000000, 1000000000 });
            check_inside(tri, { H - 1, H });

            // Vertices
            check_boundary(tri, { S, 0 });
            check_boundary(tri, { 0, S });
            check_boundary(tri, { 0, 0 });

            // Horizontal, vertical, and diagonal edges
            check_boundary(tri, { H, 0 });
            check_boundary(tri, { 0, H });
            check_boundary(tri, { H, H });
            check_boundary(tri, { 1000000000, S - 1000000000 });

            // Strictly outside and collinear extensions outside edges
            check_outside(tri, { H + 1, H });
            check_outside(tri, { S, S });
            check_outside(tri, { -1, 100000000 });
            check_outside(tri, { 100000000, -1 });
            check_outside(tri, { -100000000, 0 });
            check_outside(tri, { S + 100000000, 0 });
            check_outside(tri, { 0, -100000000 });
            check_outside(tri, { 0, S + 100000000 });
            check_outside(tri, { S + 100000000, -100000000 });
            check_outside(tri, { -100000000, S + 100000000 });
        }
    }

    SECTION("InfiniteBed +/-2^50 scale polygons") {
        constexpr coord_t R = coord_t(1) << 50;

        // Diamond with diagonal edges spanning +/-2^50
        const Polygon ccw_diamond{ { R, 0 }, { 0, R }, { -R, 0 }, { 0, -R } };
        Polygon       cw_diamond(ccw_diamond);
        cw_diamond.reverse();

        for (const Polygon &diamond : { ccw_diamond, cw_diamond }) {
            check_inside(diamond, { 0, 0 });
            check_inside(diamond, { R / 2 - 1, R / 2 });
            check_inside(diamond, { -R / 2 + 1, -R / 2 });
            check_inside(diamond, { R - 1, 0 });
            check_inside(diamond, { 0, -R + 1 });

            check_boundary(diamond, { R, 0 });
            check_boundary(diamond, { 0, R });
            check_boundary(diamond, { -R, 0 });
            check_boundary(diamond, { 0, -R });
            check_boundary(diamond, { R / 2, R / 2 });
            check_boundary(diamond, { -R / 2, R / 2 });
            check_boundary(diamond, { -R / 2, -R / 2 });
            check_boundary(diamond, { R / 2, -R / 2 });

            check_outside(diamond, { R / 2 + 1, R / 2 });
            check_outside(diamond, { -R / 2 - 1, R / 2 });
            check_outside(diamond, { -R / 2 - 1, -R / 2 });
            check_outside(diamond, { R / 2 + 1, -R / 2 });
            check_outside(diamond, { R, 1 });
            check_outside(diamond, { -R, -1 });
            check_outside(diamond, { R + 1, -1 });
            check_outside(diamond, { -1, R + 1 });
        }

        // Near-collinear diagonal at 2^50 scale (determinant = +/-1 at {1, 1}, sub-ULP for double)
        const Polygon ccw_skew{ { 0, 0 }, { R, R - 1 }, { 0, R } };
        Polygon       cw_skew(ccw_skew);
        cw_skew.reverse();

        for (const Polygon &tri : { ccw_skew, cw_skew }) {
            check_inside(tri, { 1, 1 });
            check_inside(tri, { R / 2, R / 2 });
            check_boundary(tri, { 0, R / 2 });
            check_outside(tri, { R / 2 + 1, R / 2 });
            check_outside(tri, { 2, 1 });
            check_outside(tri, { -1, R / 2 });
        }

        // Axis-aligned InfiniteBed box with a large diamond hole
        const Polygon  box{ { -R, -R }, { R, -R }, { R, R }, { -R, R } };
        const Polygon  hole{ { R / 2, 0 }, { 0, -R / 2 }, { -R / 2, 0 }, { 0, R / 2 } };
        const Polygons with_hole{ box, hole };

        check_inside(box, { 0, 0 });
        check_inside(box, { R - 1, R - 1 });
        check_boundary(box, { -R, -R });
        check_boundary(box, { 0, R });
        check_boundary(box, { R, 0 });
        check_outside(box, { R + 1, 0 });
        check_outside(box, { 0, -R - 1 });

        CHECK_FALSE(contains(with_hole, { 0, 0 }, true));
        CHECK_FALSE(contains(with_hole, { 0, 0 }, false));
        CHECK(contains(with_hole, { R / 4, R / 4 }, true));
        CHECK_FALSE(contains(with_hole, { R / 4, R / 4 }, false));
        CHECK(contains(with_hole, { R - 1, R - 1 }, true));
        CHECK(contains(with_hole, { R - 1, R - 1 }, false));
    }
}

TEST_CASE("Polygon area and winding are invariant under large coordinates (1e9)", "[Polygon]")
{
    const coord_t offset_xy = 1000000000;
    auto make_sq = [&](coord_t size, bool cw) {
        Polygon p({ { offset_xy, offset_xy },
                    { offset_xy + size, offset_xy },
                    { offset_xy + size, offset_xy + size },
                    { offset_xy, offset_xy + size } });
        if (cw)
            p.reverse();
        return p;
    };

    const Polygon ccw10 = make_sq(10, false);
    const Polygon cw10  = make_sq(10, true);
    const Polygon ccw2  = make_sq(2, false);
    const Polygon cw2   = make_sq(2, true);

    CHECK(ccw10.area() == 100.);
    CHECK(ccw10.is_counter_clockwise());
    CHECK_FALSE(ccw10.is_clockwise());

    CHECK(cw10.area() == -100.);
    CHECK_FALSE(cw10.is_counter_clockwise());
    CHECK(cw10.is_clockwise());

    CHECK(ccw2.area() == 4.);
    CHECK(ccw2.is_counter_clockwise());

    CHECK(cw2.area() == -4.);
    CHECK(cw2.is_clockwise());

    ClipperZUtils::ZPath z_ccw2 = ClipperZUtils::to_zpath<false>(ccw2.points, 0);
    ClipperZUtils::ZPath z_cw2  = ClipperZUtils::to_zpath<false>(cw2.points, 0);
    CHECK(ClipperZUtils::area(z_ccw2) == 4.);
    CHECK(ClipperZUtils::area(z_cw2) == -4.);
}
