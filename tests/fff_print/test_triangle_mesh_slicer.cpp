#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_data.hpp" // provides Test::mesh and defines indexed_triangle_set (via TriangleMesh.hpp)
#include "libslic3r/TriangleMeshSlicer.hpp"

using namespace Slic3r;
using Slic3r::Test::TestMesh;

namespace {
using Mode = MeshSlicingParams::SlicingMode;

indexed_triangle_set mesh_of(TestMesh m) { return Test::mesh(m).its; }

// Single-plane helpers: slice_mesh has a single-z overload, slice_mesh_ex takes
// a vector of zs, so wrap it and take the first (only) layer.
Polygons slice(const indexed_triangle_set &m, float z, MeshSlicingParams p = {})
{
    return slice_mesh(m, z, p);
}

ExPolygons slice_ex(const indexed_triangle_set &m, float z, MeshSlicingParamsEx p = {})
{
    return slice_mesh_ex(m, { z }, p).front();
}

bool ccw(const Polygon &p) { return p.area() > 0; }

// ExPolygon::area() returns the area in squared scaled units (not mm^2); convert
// to mm^2 before asserting golden values.
double area_mm2(const ExPolygon &e) { return e.area() * SCALING_FACTOR * SCALING_FACTOR; }

double area_mm2(const ExPolygons &exs)
{
    double a = 0;
    for (const ExPolygon &e : exs) a += area_mm2(e);
    return a;
}
} // namespace

// ---------------------------------------------------------------------------
// ① golden table
// ---------------------------------------------------------------------------

SCENARIO("SliceMesh: cube mid-plane area", "[TriangleMeshSlicer]") {
    GIVEN("a 20x20x20 cube") {
        auto mesh = mesh_of(TestMesh::cube_20x20x20);
        WHEN("sliced at z = 10") {
            ExPolygons slices = slice_ex(mesh, 10.0);
            THEN("one island of 400 mm^2 is produced") {
                REQUIRE(slices.size() == 1);
                REQUIRE_THAT(area_mm2(slices[0]), WithinRel(400.0, 1e-6));
            }
        }
    }
}

SCENARIO("SliceMesh: cube island and hole count", "[TriangleMeshSlicer]") {
    GIVEN("a 20x20x20 cube") {
        auto mesh = mesh_of(TestMesh::cube_20x20x20);
        WHEN("sliced at z = 10") {
            ExPolygons slices = slice_ex(mesh, 10.0);
            THEN("one island with no holes") {
                REQUIRE(slices.size() == 1);
                REQUIRE(slices[0].holes.empty());
            }
        }
    }
}

SCENARIO("SliceMesh: pyramid tapers monotonically", "[TriangleMeshSlicer]") {
    GIVEN("a pyramid") {
        auto mesh = mesh_of(TestMesh::pyramid);
        WHEN("sliced at increasing heights") {
            double a5  = area_mm2(slice_ex(mesh, 5.0));
            double a10 = area_mm2(slice_ex(mesh, 10.0));
            double a15 = area_mm2(slice_ex(mesh, 15.0));
            THEN("the cross-section area decreases monotonically") {
                REQUIRE(a5 > a10);
                REQUIRE(a10 > a15);
            }
        }
    }
}

SCENARIO("SliceMesh: pyramid single island", "[TriangleMeshSlicer]") {
    GIVEN("a pyramid") {
        auto mesh = mesh_of(TestMesh::pyramid);
        WHEN("sliced at z = 10") {
            ExPolygons slices = slice_ex(mesh, 10.0);
            THEN("one island is produced") {
                REQUIRE(slices.size() == 1);
            }
        }
    }
}

SCENARIO("SliceMesh: cube with a hole", "[TriangleMeshSlicer]") {
    GIVEN("a cube with a through hole") {
        auto mesh = mesh_of(TestMesh::cube_with_hole);
        WHEN("sliced at z = 5") {
            ExPolygons slices = slice_ex(mesh, 5.0);
            THEN("one island with one hole is produced") {
                REQUIRE(slices.size() == 1);
                REQUIRE(slices[0].holes.size() == 1);
            }
        }
    }
}

SCENARIO("SliceMesh: two hollow squares", "[TriangleMeshSlicer]") {
    GIVEN("two hollow squares") {
        auto mesh = mesh_of(TestMesh::two_hollow_squares);
        WHEN("sliced at mid height") {
            ExPolygons slices = slice_ex(mesh, 1.4);
            THEN("two islands, each with one hole") {
                REQUIRE(slices.size() == 2);
                REQUIRE(slices[0].holes.size() == 1);
                REQUIRE(slices[1].holes.size() == 1);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ⑤ contour closure and orientation
// ---------------------------------------------------------------------------

SCENARIO("SliceMesh: outer contour is counter-clockwise", "[TriangleMeshSlicer]") {
    GIVEN("a cube with a hole") {
        auto mesh = mesh_of(TestMesh::cube_with_hole);
        WHEN("sliced at z = 5") {
            ExPolygons slices = slice_ex(mesh, 5.0);
            THEN("the outer contour is CCW") {
                REQUIRE(ccw(slices[0].contour));
            }
        }
    }
}

SCENARIO("SliceMesh: hole contour is clockwise", "[TriangleMeshSlicer]") {
    GIVEN("a cube with a hole") {
        auto mesh = mesh_of(TestMesh::cube_with_hole);
        WHEN("sliced at z = 5") {
            ExPolygons slices = slice_ex(mesh, 5.0);
            THEN("the hole contour is CW") {
                REQUIRE_FALSE(ccw(slices[0].holes[0]));
            }
        }
    }
}

SCENARIO("SliceMesh: contour is closed", "[TriangleMeshSlicer]") {
    GIVEN("a cube") {
        auto mesh = mesh_of(TestMesh::cube_20x20x20);
        WHEN("sliced at z = 10") {
            ExPolygons slices = slice_ex(mesh, 10.0);
            THEN("the contour is a valid loop, implicitly closed") {
                const Polygon &c = slices[0].contour;
                REQUIRE(c.points.size() >= 3);
                // Slic3r polygons are implicitly closed: the first point is NOT repeated.
                REQUIRE(c.points.front() != c.points.back());
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ⑥ slicing modes
// ---------------------------------------------------------------------------

SCENARIO("SliceMesh: Regular mode keeps holes", "[TriangleMeshSlicer]") {
    GIVEN("a cube with a hole") {
        auto mesh = mesh_of(TestMesh::cube_with_hole);
        MeshSlicingParamsEx p;
        p.mode = Mode::Regular;
        WHEN("sliced with Regular mode") {
            ExPolygons slices = slice_ex(mesh, 5.0, p);
            THEN("one island with one hole (pftNonZero)") {
                REQUIRE(slices.size() == 1);
                REQUIRE(slices[0].holes.size() == 1);
            }
        }
    }
}

SCENARIO("SliceMesh: EvenOdd mode keeps holes", "[TriangleMeshSlicer]") {
    GIVEN("a cube with a hole") {
        auto mesh = mesh_of(TestMesh::cube_with_hole);
        MeshSlicingParamsEx p;
        p.mode = Mode::EvenOdd;
        WHEN("sliced with EvenOdd mode") {
            ExPolygons slices = slice_ex(mesh, 5.0, p);
            THEN("one island with one hole (pftEvenOdd)") {
                REQUIRE(slices.size() == 1);
                REQUIRE(slices[0].holes.size() == 1);
            }
        }
    }
}

SCENARIO("SliceMesh: Positive mode orients all contours CCW", "[TriangleMeshSlicer]") {
    GIVEN("a cube with a hole") {
        auto mesh = mesh_of(TestMesh::cube_with_hole);
        MeshSlicingParams p;
        p.mode = Mode::Positive;
        WHEN("sliced with Positive mode (Polygons)") {
            Polygons loops = slice(mesh, 5.0, p);
            THEN("all loops are CCW") {
                REQUIRE(loops.size() >= 2);
                for (const Polygon &loop : loops)
                    REQUIRE(ccw(loop));
            }
        }
    }
}

SCENARIO("SliceMesh: PositiveLargestContour keeps only the largest loop", "[TriangleMeshSlicer]") {
    GIVEN("two hollow squares") {
        auto mesh = mesh_of(TestMesh::two_hollow_squares);
        MeshSlicingParams p;
        p.mode = Mode::PositiveLargestContour;
        WHEN("sliced with PositiveLargestContour mode") {
            Polygons loops = slice(mesh, 1.4, p);
            THEN("only one CCW loop is produced") {
                REQUIRE(loops.size() == 1);
                REQUIRE(ccw(loops[0]));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ⑦ boundary plane open/close
// ---------------------------------------------------------------------------

SCENARIO("SliceMesh: bottom plane is open (empty)", "[TriangleMeshSlicer]") {
    GIVEN("a cube") {
        auto mesh = mesh_of(TestMesh::cube_20x20x20);
        WHEN("sliced exactly at the downward-facing bottom plane z = 0") {
            ExPolygons slices = slice_ex(mesh, 0.0);
            THEN("no slice is produced") {
                REQUIRE(slices.empty());
            }
        }
    }
}

SCENARIO("SliceMesh: top plane is closed (full)", "[TriangleMeshSlicer]") {
    GIVEN("a cube") {
        auto mesh = mesh_of(TestMesh::cube_20x20x20);
        WHEN("sliced exactly at the upward-facing top plane z = 20") {
            ExPolygons slices = slice_ex(mesh, 20.0);
            THEN("one full island of 400 mm^2 is produced") {
                REQUIRE(slices.size() == 1);
                REQUIRE_THAT(area_mm2(slices[0]), WithinRel(400.0, 1e-6));
            }
        }
    }
}

SCENARIO("SliceMesh: interior plane is full", "[TriangleMeshSlicer]") {
    GIVEN("a cube") {
        auto mesh = mesh_of(TestMesh::cube_20x20x20);
        WHEN("sliced at an interior plane z = 10") {
            ExPolygons slices = slice_ex(mesh, 10.0);
            THEN("one full island of 400 mm^2 is produced") {
                REQUIRE(slices.size() == 1);
                REQUIRE_THAT(area_mm2(slices[0]), WithinRel(400.0, 1e-6));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ⑧ closing_radius / extra_offset
// ---------------------------------------------------------------------------

SCENARIO("SliceMesh: extra_offset expands the contour", "[TriangleMeshSlicer]") {
    GIVEN("a cube") {
        auto mesh = mesh_of(TestMesh::cube_20x20x20);
        MeshSlicingParamsEx p;
        p.extra_offset = 0.5;
        WHEN("sliced with extra_offset = 0.5") {
            ExPolygons slices = slice_ex(mesh, 10.0, p);
            THEN("the 20mm square grows to 21mm (441 mm^2)") {
                REQUIRE(slices.size() == 1);
                REQUIRE_THAT(area_mm2(slices[0]), WithinRel(21.0 * 21.0, 1e-3));
            }
        }
    }
}

SCENARIO("SliceMesh: negative extra_offset shrinks the contour", "[TriangleMeshSlicer]") {
    GIVEN("a cube") {
        auto mesh = mesh_of(TestMesh::cube_20x20x20);
        MeshSlicingParamsEx p;
        p.extra_offset = -0.5;
        WHEN("sliced with extra_offset = -0.5") {
            ExPolygons slices = slice_ex(mesh, 10.0, p);
            THEN("the 20mm square shrinks to 19mm (361 mm^2)") {
                REQUIRE(slices.size() == 1);
                REQUIRE_THAT(area_mm2(slices[0]), WithinRel(19.0 * 19.0, 1e-3));
            }
        }
    }
}

SCENARIO("SliceMesh: closing_radius fills a concave notch", "[TriangleMeshSlicer]") {
    GIVEN("a cube with a concave notch") {
        auto mesh = mesh_of(TestMesh::cube_with_concave_hole);
        WHEN("sliced with a closing radius larger than half the notch width") {
            ExPolygons plain  = slice_ex(mesh, 0.0);
            MeshSlicingParamsEx p;
            p.closing_radius = 10.0; // the ~10mm-wide notch needs closing_radius >= 5mm to be bridged
            ExPolygons closed = slice_ex(mesh, 0.0, p);
            THEN("closing fills the notch and increases the area") {
                REQUIRE(!closed.empty());
                REQUIRE(area_mm2(closed) > area_mm2(plain));
            }
        }
    }
}

SCENARIO("SliceMesh: closing_radius >= extra_offset yields a net offset", "[TriangleMeshSlicer]") {
    GIVEN("a cube") {
        auto mesh = mesh_of(TestMesh::cube_20x20x20);
        MeshSlicingParamsEx p;
        p.closing_radius = 0.5;
        p.extra_offset  = 0.3;
        WHEN("sliced with closing_radius = 0.5 and extra_offset = 0.3") {
            ExPolygons slices = slice_ex(mesh, 10.0, p);
            THEN("the net outward offset is +0.3mm (20.6mm square)") {
                REQUIRE(slices.size() == 1);
                REQUIRE_THAT(area_mm2(slices[0]), WithinRel(20.6 * 20.6, 1e-3));
            }
        }
    }
}
