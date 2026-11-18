#include <catch2/catch_test_macros.hpp>

#include <set>

#include "libslic3r/Triangulation.hpp"

using namespace Slic3r;

namespace {

Point pt(double x_mm, double y_mm) { return Point::new_scale(x_mm, y_mm); }

// Twice the signed area of triangle (a, b, c). Positive = counter-clockwise.
double signed_area2(const Point &a, const Point &b, const Point &c)
{
    double ax = a.x(), ay = a.y(), bx = b.x(), by = b.y(), cx = c.x(), cy = c.y();
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

// Every vertex index referenced by at least one triangle.
std::set<uint32_t> used_vertices(const Triangulation::Indices &tris)
{
    std::set<uint32_t> used;
    for (const auto &t : tris)
        for (int i = 0; i < 3; ++i)
            used.insert(static_cast<uint32_t>(t[i]));
    return used;
}

// Verify the four triangulation properties: exact triangle count, no degenerate
// triangle, consistent winding, and full vertex coverage.
void check_triangulation(const Points &points, const Triangulation::Indices &tris, size_t expected)
{
    INFO("triangles " << tris.size() << " vs expected " << expected);
    REQUIRE(tris.size() == expected);

    int sign = 0;
    for (const auto &t : tris) {
        double a = signed_area2(points[t[0]], points[t[1]], points[t[2]]);
        INFO("triangle [" << t[0] << "," << t[1] << "," << t[2] << "] area2 " << a);
        REQUIRE(a != 0.0); // no zero-area triangle
        int s = a > 0 ? 1 : -1;
        if (sign == 0) sign = s;
        REQUIRE(s == sign); // consistent winding
    }

    auto used = used_vertices(tris);
    for (size_t i = 0; i < points.size(); ++i)
        REQUIRE(used.count(uint32_t(i)) == 1); // vertex coverage
}

} // namespace

SCENARIO("Triangulation of a triangle", "[Triangulation]") {
    GIVEN("a single triangle") {
        Polygon tri{ pt(0.0, 0.0), pt(10.0, 0.0), pt(0.0, 10.0) };

        WHEN("triangulated") {
            Triangulation::Indices tris = Triangulation::triangulate(tri);

            THEN("one triangle is produced (V - 2 = 1)") {
                check_triangulation(tri.points, tris, 1);
            }
        }
    }
}

SCENARIO("Triangulation of a square", "[Triangulation]") {
    GIVEN("a counter-clockwise square") {
        Polygon square{ pt(0.0, 0.0), pt(10.0, 0.0), pt(10.0, 10.0), pt(0.0, 10.0) };

        WHEN("triangulated") {
            Triangulation::Indices tris = Triangulation::triangulate(square);

            THEN("two triangles are produced (V - 2 = 2)") {
                check_triangulation(square.points, tris, 2);
            }
        }
    }
}

SCENARIO("Triangulation of a concave polygon", "[Triangulation]") {
    GIVEN("an L-shaped polygon") {
        Polygon lshape{ pt(0.0, 0.0), pt(10.0, 0.0), pt(10.0, 4.0),
                        pt(4.0, 4.0), pt(4.0, 10.0), pt(0.0, 10.0) };

        WHEN("triangulated") {
            Triangulation::Indices tris = Triangulation::triangulate(lshape);

            THEN("four triangles are produced (V - 2 = 4)") {
                check_triangulation(lshape.points, tris, 4);
            }
        }
    }
}

SCENARIO("Triangulation of a polygon with a hole", "[Triangulation]") {
    GIVEN("a square with a square hole") {
        Polygon contour{ pt(0.0, 0.0), pt(10.0, 0.0), pt(10.0, 10.0), pt(0.0, 10.0) };
        Polygon hole{ pt(2.0, 2.0), pt(2.0, 8.0), pt(8.0, 8.0), pt(8.0, 2.0) }; // CW
        ExPolygon expoly(contour, hole);

        // Indices are into contour points first, then hole points.
        Points all = contour.points;
        all.insert(all.end(), hole.points.begin(), hole.points.end());

        WHEN("triangulated") {
            Triangulation::Indices tris = Triangulation::triangulate(expoly);

            THEN("eight triangles are produced (V - 2 + 2·H = 8)") {
                check_triangulation(all, tris, 8);
            }
        }
    }
}

SCENARIO("Triangulation of multiple polygons", "[Triangulation]") {
    GIVEN("two disjoint squares") {
        Polygon a{ pt(0.0, 0.0), pt(10.0, 0.0), pt(10.0, 10.0), pt(0.0, 10.0) };
        Polygon b{ pt(20.0, 0.0), pt(30.0, 0.0), pt(30.0, 10.0), pt(20.0, 10.0) };
        Polygons polygons{ a, b };

        Points all = a.points;
        all.insert(all.end(), b.points.begin(), b.points.end());

        WHEN("triangulated") {
            Triangulation::Indices tris = Triangulation::triangulate(polygons);

            THEN("four triangles are produced (Σ(V_i - 2) = 2 + 2)") {
                check_triangulation(all, tris, 4);
            }
        }
    }
}

SCENARIO("Triangulation requires counter-clockwise contours", "[Triangulation]") {
    GIVEN("a clockwise square") {
        Polygon square{ pt(0.0, 0.0), pt(0.0, 10.0), pt(10.0, 10.0), pt(10.0, 0.0) }; // CW

        WHEN("triangulated") {
            Triangulation::Indices tris = Triangulation::triangulate(square);

            THEN("no triangles are produced") {
                // The flood fill marks faces by constrained-edge direction, which
                // assumes CCW contours (interior on the left). A CW contour flips
                // the inside/outside test, so the interior is marked as outside.
                REQUIRE(tris.empty());
            }
        }
    }
}

SCENARIO("Triangulation of multiple expolygons", "[Triangulation]") {
    GIVEN("two disjoint squares") {
        ExPolygon a{ pt(0.0, 0.0), pt(10.0, 0.0), pt(10.0, 10.0), pt(0.0, 10.0) };
        ExPolygon b{ pt(20.0, 0.0), pt(30.0, 0.0), pt(30.0, 10.0), pt(20.0, 10.0) };
        ExPolygons expolygons{ a, b };

        Points all = a.contour.points;
        all.insert(all.end(), b.contour.points.begin(), b.contour.points.end());

        WHEN("triangulated") {
            Triangulation::Indices tris = Triangulation::triangulate(expolygons);

            THEN("four triangles are produced (Σ(Vᵢ - 2) = 2 + 2)") {
                check_triangulation(all, tris, 4);
            }
        }
    }
}

SCENARIO("Triangulation deduplicates shared vertices across expolygons", "[Triangulation]") {
    GIVEN("two squares touching at a corner") {
        ExPolygon a{ pt(0.0, 0.0), pt(10.0, 0.0), pt(10.0, 10.0), pt(0.0, 10.0) };
        ExPolygon b{ pt(10.0, 10.0), pt(20.0, 10.0), pt(20.0, 20.0), pt(10.0, 20.0) }; // shares (10,10)
        ExPolygons expolygons{ a, b };

        // to_points flattens a.contour then b.contour: (10,10) appears at indices 2 and 4.
        Points all = a.contour.points;
        all.insert(all.end(), b.contour.points.begin(), b.contour.points.end());

        WHEN("triangulated") {
            Triangulation::Indices tris = Triangulation::triangulate(expolygons);

            THEN("four triangles are produced despite the duplicated vertex") {
                REQUIRE(tris.size() == 4);
            }
            THEN("no zero-area triangle and consistent winding") {
                int sign = 0;
                for (const auto &t : tris) {
                    double ar = signed_area2(all[t[0]], all[t[1]], all[t[2]]);
                    REQUIRE(ar != 0.0);
                    int s = ar > 0 ? 1 : -1;
                    if (sign == 0) sign = s;
                    REQUIRE(s == sign);
                }
            }
        }
    }
}
