// Tests for the cheap pre-computation face-count estimate of the local
// subdivision preview (MR !21 review item 2). The estimate must be evaluable
// WITHOUT running the subdivision, be a conservative upper bound of what
// smooth_triangle_mesh_local_its actually produces (selected faces plus the
// untouched surroundings, including the boundary coordination), be exact in
// the trivial cases, and never overflow. The shared budget decision must flip
// exactly at the threshold.

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <vector>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshDeal.hpp"

using namespace Slic3r;

namespace {

// Closed cube built from 12 triangles (two per face), CCW seen from outside:
// every edge is shared by exactly two faces, so all faces are interior.
indexed_triangle_set cube_its()
{
    indexed_triangle_set its;
    its.vertices = { Vec3f(0.f, 0.f, 0.f), Vec3f(1.f, 0.f, 0.f), Vec3f(1.f, 1.f, 0.f), Vec3f(0.f, 1.f, 0.f),
                     Vec3f(0.f, 0.f, 1.f), Vec3f(1.f, 0.f, 1.f), Vec3f(1.f, 1.f, 1.f), Vec3f(0.f, 1.f, 1.f) };
    its.indices  = {
        stl_triangle_vertex_indices(0, 2, 1), stl_triangle_vertex_indices(0, 3, 2), // bottom
        stl_triangle_vertex_indices(4, 5, 6), stl_triangle_vertex_indices(4, 6, 7), // top
        stl_triangle_vertex_indices(0, 1, 5), stl_triangle_vertex_indices(0, 5, 4), // y = 0
        stl_triangle_vertex_indices(1, 2, 6), stl_triangle_vertex_indices(1, 6, 5), // x = 1
        stl_triangle_vertex_indices(2, 3, 7), stl_triangle_vertex_indices(2, 7, 6), // y = 1
        stl_triangle_vertex_indices(3, 0, 4), stl_triangle_vertex_indices(3, 4, 7), // x = 0
    };
    return its;
}

// Closed regular tetrahedron: 4 faces, all interior.
indexed_triangle_set tetra_its()
{
    indexed_triangle_set its;
    its.vertices = { Vec3f(0.f, 0.f, 0.f), Vec3f(1.f, 0.f, 0.f), Vec3f(0.f, 1.f, 0.f), Vec3f(0.f, 0.f, 1.f) };
    its.indices  = {
        stl_triangle_vertex_indices(0, 2, 1),
        stl_triangle_vertex_indices(0, 1, 3),
        stl_triangle_vertex_indices(1, 2, 3),
        stl_triangle_vertex_indices(2, 0, 3),
    };
    return its;
}

// Mirrors the production preview growth loop: subdivides the region (front of
// the face list) level by level and returns the final face count.
size_t subdivide_region_levels(const indexed_triangle_set& origin, const std::vector<uint32_t>& selection, int levels)
{
    indexed_triangle_set  current = origin;
    std::vector<uint32_t> region  = selection;
    for (int step = 0; step < levels; ++step) {
        bool                 ok = false;
        indexed_triangle_set next = TriangleMeshDeal::smooth_triangle_mesh_local_its(current, region, ok);
        REQUIRE(ok);
        const size_t unselected   = current.indices.size() - region.size();
        const size_t region_faces = next.indices.size() - unselected;
        region.assign(region_faces, 0);
        for (size_t i = 0; i < region_faces; ++i)
            region[i] = uint32_t(i);
        current = std::move(next);
    }
    return current.indices.size();
}

} // namespace

TEST_CASE("Local subdivision budget estimate is exact for the trivial cases", "[TriangleMeshDeal][LocalSubdivision]")
{
    size_t out = 0;
    // No subdivision requested: the result is the input, exactly.
    CHECK(TriangleMeshDeal::estimated_local_subdivision_face_count(123, 7, 0, out));
    CHECK(out == 123);
    CHECK(TriangleMeshDeal::estimated_local_subdivision_face_count(123, 7, -2, out));
    CHECK(out == 123);
    // Nothing selected: unselected faces pass through untouched, exactly.
    CHECK(TriangleMeshDeal::estimated_local_subdivision_face_count(123, 0, 3, out));
    CHECK(out == 123);
}

TEST_CASE("Local subdivision budget estimate grows by at most 4^level per painted face", "[TriangleMeshDeal][LocalSubdivision]")
{
    size_t out = 0;
    // One painted face: 4^level - 1 additional faces at most.
    CHECK(TriangleMeshDeal::estimated_local_subdivision_face_count(1000, 1, 1, out));
    CHECK(out == 1003);
    CHECK(TriangleMeshDeal::estimated_local_subdivision_face_count(1000, 1, 2, out));
    CHECK(out == 1015);
    CHECK(TriangleMeshDeal::estimated_local_subdivision_face_count(1000, 1, 3, out));
    CHECK(out == 1063);
    // Linear in the painted count.
    CHECK(TriangleMeshDeal::estimated_local_subdivision_face_count(1000, 5, 3, out));
    CHECK(out == 1000 + 5 * 63);
}

TEST_CASE("Local subdivision budget estimate never overflows", "[TriangleMeshDeal][LocalSubdivision]")
{
    size_t out = 0;
    const size_t max = std::numeric_limits<size_t>::max();
    CHECK_FALSE(TriangleMeshDeal::estimated_local_subdivision_face_count(max, 1, 1, out));
    CHECK_FALSE(TriangleMeshDeal::estimated_local_subdivision_face_count(max - 2, 1, 1, out));
    CHECK_FALSE(TriangleMeshDeal::estimated_local_subdivision_face_count(0, max, 1, out));
    CHECK_FALSE(TriangleMeshDeal::estimated_local_subdivision_face_count(max, max, 3, out));
    // The 4^level growth itself must not wrap at absurd levels.
    CHECK_FALSE(TriangleMeshDeal::estimated_local_subdivision_face_count(0, 1, 1000, out));
    // Representable sums still succeed.
    CHECK(TriangleMeshDeal::estimated_local_subdivision_face_count(max - 1000, 1, 1, out));
    CHECK(out == max - 997);
}

TEST_CASE("Estimate bounds the real local subdivision output", "[TriangleMeshDeal][LocalSubdivision]")
{
    // A closed tetrahedron: all faces interior, one step splits every face
    // 1-to-4, so the bound is attained exactly.
    {
        const indexed_triangle_set its   = tetra_its();
        const size_t               total = its.indices.size();
        const std::vector<uint32_t> all{ 0, 1, 2, 3 };
        size_t                      bound = 0;
        REQUIRE(TriangleMeshDeal::estimated_local_subdivision_face_count(total, total, 1, bound));
        CHECK(bound == total * 4);
        CHECK(subdivide_region_levels(its, all, 1) == bound);
    }

    // A closed cube, fully selected: exact again for every level (all faces
    // stay interior), which also checks the 4^level factor across steps.
    {
        const indexed_triangle_set its   = cube_its();
        const size_t               total = its.indices.size();
        std::vector<uint32_t>      all(total);
        for (size_t i = 0; i < total; ++i)
            all[i] = uint32_t(i);
        for (const int level : { 1, 2, 3 }) {
            size_t bound = 0;
            REQUIRE(TriangleMeshDeal::estimated_local_subdivision_face_count(total, total, level, bound));
            CHECK(bound == total * (size_t{ 1 } << (2 * level)));
            CHECK(subdivide_region_levels(its, all, level) == bound);
        }
    }

    // Partial selection: the boundary coordination produces 2-3 children per
    // boundary face, so the actual count stays strictly below the 4x bound
    // while never dropping below 2 children per selected face. The unselected
    // faces pass through untouched. Both claims are checked against the real
    // algorithm over two levels, mirroring the production growth loop.
    {
        const indexed_triangle_set its   = cube_its();
        const size_t               total = its.indices.size();
        const std::vector<uint32_t> some{ 0, 1, 4 };
        bool                        ok      = false;
        indexed_triangle_set        subdiv  = TriangleMeshDeal::smooth_triangle_mesh_local_its(its, some, ok);
        REQUIRE(ok);
        const size_t unselected = total - some.size();
        CHECK(subdiv.indices.size() >= unselected + 2 * some.size());
        CHECK(subdiv.indices.size() < unselected + 4 * some.size());
        size_t bound = 0;
        REQUIRE(TriangleMeshDeal::estimated_local_subdivision_face_count(total, some.size(), 1, bound));
        CHECK(subdiv.indices.size() <= bound);
        // Two levels: the region children become the next selection; the bound
        // keeps holding.
        REQUIRE(TriangleMeshDeal::estimated_local_subdivision_face_count(total, some.size(), 2, bound));
        CHECK(subdivide_region_levels(its, some, 2) <= bound);
    }
}

TEST_CASE("Budget decision flips exactly at the threshold", "[TriangleMeshDeal][LocalSubdivision]")
{
    // The same value the gizmo passes as LocalSubdivisionFaceBudget.
    const size_t budget = 1000000;
    // Level 3: every painted face costs at most 63 extra faces.
    CHECK_FALSE(TriangleMeshDeal::local_subdivision_exceeds_face_budget(999937, 1, 3, budget)); // exactly 1,000,000
    CHECK(TriangleMeshDeal::local_subdivision_exceeds_face_budget(999938, 1, 3, budget));       // 1,000,001
    CHECK_FALSE(TriangleMeshDeal::local_subdivision_exceeds_face_budget(0, 15873, 3, budget));  // 999,999
    CHECK(TriangleMeshDeal::local_subdivision_exceeds_face_budget(0, 15874, 3, budget));        // 1,000,062
    // The review scenario: ~50k interior faces at level 3 cost several million.
    CHECK(TriangleMeshDeal::local_subdivision_exceeds_face_budget(0, 50000, 3, budget));
    // Normal previews keep computing without asking.
    CHECK_FALSE(TriangleMeshDeal::local_subdivision_exceeds_face_budget(400000, 9000, 3, budget)); // 967,000
    // An empty selection estimates exactly the untouched mesh (nothing grows):
    // the production gate short-circuits before the dialog for painted == 0,
    // while the pure decision reflects the face count of the unchanged result.
    CHECK_FALSE(TriangleMeshDeal::local_subdivision_exceeds_face_budget(999999, 0, 3, budget));
    CHECK(TriangleMeshDeal::local_subdivision_exceeds_face_budget(1000001, 0, 3, budget));
    // Overflow counts as exceeding - never as within budget.
    CHECK(TriangleMeshDeal::local_subdivision_exceeds_face_budget(std::numeric_limits<size_t>::max(), 1, 100, budget));
    CHECK(TriangleMeshDeal::local_subdivision_exceeds_face_budget(std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max(), 3, budget));
}
