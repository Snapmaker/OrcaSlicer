#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "libslic3r/Tesselate.hpp"
#include "libslic3r/ExPolygon.hpp"

using namespace Slic3r;

namespace {

Point pt(double x_mm, double y_mm) { return Point::new_scale(x_mm, y_mm); }

// Twice the signed area of triangle (a, b, c). Positive = counter-clockwise.
// Templated so the same helper covers Vec2d / Vec2f / Vec3d outputs.
template <typename V>
double signed_area2(const V &a, const V &b, const V &c)
{
    return (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
}

// Verify flat-vertex output: exactly expected_tris * 3 vertices, every triple
// forms a non-degenerate triangle, and all triangles are wound the same way.
// GLU may emit GL_TRIANGLES / TRIANGLE_FAN / TRIANGLE_STRIP; GluTessWrapper
// normalises all three to independent triangles, so the output must still be
// consistently wound — this assertion is what pins that normalisation.
template <typename V>
void check_flat_triangles(const std::vector<V> &verts, size_t expected_tris)
{
    INFO("vertices " << verts.size() << " vs expected " << expected_tris * 3);
    REQUIRE(verts.size() == expected_tris * 3);

    int sign = 0;
    for (size_t i = 0; i + 2 < verts.size(); i += 3) {
        double a = signed_area2(verts[i], verts[i + 1], verts[i + 2]);
        INFO("triangle " << i / 3 << " area2 " << a);
        REQUIRE(a != 0.0); // no zero-area triangle
        int s = a > 0 ? 1 : -1;
        if (sign == 0) sign = s;
        REQUIRE(s == sign); // consistent winding
    }
}

// Sign of the first triangle's twice-signed-area (+1 = CCW, -1 = CW).
template <typename V>
int first_winding(const std::vector<V> &verts)
{
    REQUIRE(verts.size() >= 3);
    double a = signed_area2(verts[0], verts[1], verts[2]);
    return a > 0 ? 1 : -1;
}

// --- Scope: geometry fixtures (golden triangle counts via Euler) ---
// A triangulation of a polygon with V vertices and H holes has V + 2H - 2
// triangles. Hand-derived counts below all follow this formula:
//   square (V=4)            -> 4 - 2 = 2
//   L-shape (V=6)           -> 6 - 2 = 4
//   square + 1 hole (V=8,H=1) -> 8 + 2 - 2 = 8
//   square + 2 holes (V=12,H=2)-> 12 + 4 - 2 = 14
//   two squares (V=4+4)     -> 2 + 2 = 4
Polygon ccw_square() { return Polygon{ pt(0, 0), pt(10, 0), pt(10, 10), pt(0, 10) }; }
Polygon cw_square()  { return Polygon{ pt(0, 0), pt(0, 10), pt(10, 10), pt(10, 0) }; }

} // namespace

// --- Scope: single-polygon 2d ---

SCENARIO("Tesselate a convex square to 2D vertices", "[Tesselate]") {
    GIVEN("a counter-clockwise square") {
        ExPolygon expoly(ccw_square());

        WHEN("tessellated to 2D double vertices") {
            std::vector<Vec2d> verts = triangulate_expolygon_2d(expoly, NORMALS_UP);

            THEN("two non-degenerate, consistently wound triangles are produced") {
                check_flat_triangles(verts, 2);
                // CCW contour + NORMALS_UP -> GLU output keeps CCW (positive).
                REQUIRE(first_winding(verts) == 1);
            }
        }
    }
}

SCENARIO("Tesselate a concave polygon (fan path)", "[Tesselate]") {
    GIVEN("an L-shaped concave polygon") {
        Polygon lshape{ pt(0, 0), pt(10, 0), pt(10, 4),
                        pt(4, 4), pt(4, 10), pt(0, 10) };
        ExPolygon expoly(lshape);

        WHEN("tessellated to 2D vertices") {
            std::vector<Vec2d> verts = triangulate_expolygon_2d(expoly, NORMALS_UP);

            THEN("four triangles are produced (V - 2 = 4)") {
                // Concave input forces GLU onto the triangle-fan path; the
                // wrapper must still emit consistent independent triangles.
                check_flat_triangles(verts, 4);
            }
        }
    }
}

SCENARIO("Tesselate a polygon with a hole (strip + combine path)", "[Tesselate]") {
    GIVEN("a counter-clockwise square with a clockwise square hole") {
        Polygon hole{ pt(2, 2), pt(2, 8), pt(8, 8), pt(8, 2) }; // CW
        ExPolygon expoly(ccw_square(), hole);

        WHEN("tessellated to 2D vertices") {
            std::vector<Vec2d> verts = triangulate_expolygon_2d(expoly, NORMALS_UP);

            THEN("eight triangles are produced (V + 2H - 2 = 8)") {
                // A hole forces GLU onto the triangle-strip path and exercises
                // the m_num_points == 4 flip toggle in GluTessWrapper::tessVertex.
                check_flat_triangles(verts, 8);
            }
        }
    }
}

SCENARIO("Tesselate a polygon with two holes", "[Tesselate]") {
    GIVEN("a square with two non-overlapping clockwise square holes") {
        Polygon hole1{ pt(2, 2), pt(2, 4), pt(4, 4), pt(4, 2) }; // CW
        Polygon hole2{ pt(6, 6), pt(6, 8), pt(8, 8), pt(8, 6) }; // CW
        ExPolygon expoly(ccw_square(), hole1);
        expoly.holes.push_back(hole2);

        WHEN("tessellated to 2D vertices") {
            std::vector<Vec2d> verts = triangulate_expolygon_2d(expoly, NORMALS_UP);

            THEN("fourteen triangles are produced (V + 2H - 2 = 14)") {
                check_flat_triangles(verts, 14);
            }
        }
    }
}

// --- Scope: flip / winding ---

SCENARIO("flip reverses triangle winding", "[Tesselate]") {
    GIVEN("a counter-clockwise square") {
        ExPolygon expoly(ccw_square());

        WHEN("tessellated with NORMALS_UP and NORMALS_DOWN") {
            std::vector<Vec2d> up   = triangulate_expolygon_2d(expoly, NORMALS_UP);
            std::vector<Vec2d> down = triangulate_expolygon_2d(expoly, NORMALS_DOWN);

            THEN("both are valid, and DOWN is wound oppositely to UP") {
                check_flat_triangles(up, 2);
                check_flat_triangles(down, 2);
                // flip=false keeps CCW (positive); flip=true reverses to CW.
                REQUIRE(first_winding(up) == 1);
                REQUIRE(first_winding(down) == -1);
                REQUIRE(first_winding(up) == -first_winding(down));
            }
        }
    }
}

SCENARIO("Winding follows the input contour orientation", "[Tesselate]") {
    GIVEN("a clockwise square") {
        ExPolygon expoly(cw_square());

        WHEN("tessellated with NORMALS_UP") {
            std::vector<Vec2d> verts = triangulate_expolygon_2d(expoly, NORMALS_UP);

            THEN("triangles are wound clockwise (opposite of a CCW input)") {
                check_flat_triangles(verts, 2);
                // Unlike Triangulation, GLU has no CCW-only restriction: output
                // winding simply follows the input contour orientation.
                REQUIRE(first_winding(verts) == -1);
            }
        }
    }
}

// --- Scope: 2f / 3d variants ---

SCENARIO("Tesselate to 2D float vertices", "[Tesselate]") {
    GIVEN("a counter-clockwise square") {
        ExPolygon expoly(ccw_square());

        WHEN("tessellated to 2D float vertices") {
            std::vector<Vec2f> verts = triangulate_expolygon_2f(expoly, NORMALS_UP);

            THEN("two non-degenerate, consistently wound triangles are produced") {
                check_flat_triangles(verts, 2);
            }
        }
    }
}

SCENARIO("3D tesselation lifts every vertex to z", "[Tesselate]") {
    GIVEN("a counter-clockwise square") {
        ExPolygon expoly(ccw_square());

        WHEN("tessellated to 3D vertices with z = 5.0") {
            std::vector<Vec3d> verts = triangulate_expolygon_3d(expoly, 5.0);

            THEN("two triangles are produced, every vertex at z == 5") {
                check_flat_triangles(verts, 2);
                for (const Vec3d &v : verts)
                    REQUIRE(v.z() == 5.0); // m_z is copied verbatim into each vertex
            }
        }
    }
}

// --- Scope: batch (multiple ExPolygons) ---

SCENARIO("Batch tesselation sums triangles across polygons", "[Tesselate]") {
    GIVEN("two disjoint squares") {
        Polygon b{ pt(20, 0), pt(30, 0), pt(30, 10), pt(20, 10) };
        ExPolygons polys{ ExPolygon(ccw_square()), ExPolygon(b) };

        WHEN("tessellated via each batch entry point") {
            std::vector<Vec2d> v2d = triangulate_expolygons_2d(polys, NORMALS_UP);
            std::vector<Vec2f> v2f = triangulate_expolygons_2f(polys, NORMALS_UP);
            std::vector<Vec3d> v3d = triangulate_expolygons_3d(polys, 0.0);

            THEN("four triangles are produced in every variant (2 + 2)") {
                check_flat_triangles(v2d, 4);
                check_flat_triangles(v2f, 4);
                check_flat_triangles(v3d, 4);
            }
        }
    }
}

// --- Scope: degenerate / empty input ---

SCENARIO("Empty and degenerate input produce empty output", "[Tesselate]") {
    GIVEN("an empty ExPolygon") {
        WHEN("tessellated") {
            THEN("empty output is produced without crashing") {
                std::vector<Vec2d> verts;
                REQUIRE_NOTHROW(verts = triangulate_expolygon_2d(ExPolygon{}));
                REQUIRE(verts.empty());
            }
        }
    }

    GIVEN("a two-point polygon (too few vertices to triangulate)") {
        Polygon two{ pt(0, 0), pt(10, 0) };

        WHEN("tessellated") {
            THEN("empty output is produced without crashing") {
                std::vector<Vec2d> verts;
                REQUIRE_NOTHROW(verts = triangulate_expolygon_2d(ExPolygon(two)));
                REQUIRE(verts.empty());
            }
        }
    }
}
