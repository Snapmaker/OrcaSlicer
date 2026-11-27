#pragma once

#ifndef libslic3r_TriangleMeshDeal_hpp_
#define libslic3r_TriangleMeshDeal_hpp_

#include "TriangleMesh.hpp"

#include <utility>
#include <vector>

namespace Slic3r {
class TriangleMeshDeal
{
public:
    // Why a local subdivision level failed (valid when ok == false): an edge of
    // the selection is shared by more than two selected faces (NonManifold).
    // DegenerateGeometry is NOT a rejection: the two-strike policy ships the
    // initial Loop result, and the GUI records this reason when the shipped
    // children turned out fully degenerate (the next level's grow then finds
    // nothing subdividable).
    enum class SubdivisionFailure {
        None,
        NonManifold,
        DegenerateGeometry,
    };

    // Loop-subdivides every face once. When `out_origins` is given, it receives
    // one entry per output face: (original input face index, child ordinal 0..3
    // in the igl::loop() output layout). The ordinal identifies the child region
    // in the parent face's barycentric domain, which paint remapping uses to
    // transfer per-face painting trees onto the four children. Duplicate input
    // faces are dropped (keeping the first occurrence) and unreferenced vertices
    // are compactified; the mapping therefore refers to the ORIGINAL input faces,
    // not the preprocessed ones. When two duplicated faces carry different
    // paintings, the first occurrence deterministically wins.
    static TriangleMesh smooth_triangle_mesh(const TriangleMesh& mesh, bool& ok, std::vector<std::pair<int, int>>* out_origins = nullptr);

    static TriangleMesh smooth_triangle_mesh_local(const TriangleMesh& mesh, const std::vector<uint32_t>& facet_indices, bool& ok);

    // Same subdivision as smooth_triangle_mesh_local, but returns the bare triangle
    // set without building TriangleMesh stats (neighbour graph, volume, bounding box,
    // patch count). Use it for previewing, where the stats would be recomputed and
    // thrown away on every step; construct the TriangleMesh only when applying.
    // region_face_count (optional) receives how many faces at the FRONT of the
    // output are the subdivided region - the selection for the next level. The
    // caller cannot infer this by subtraction: the zero-area filter moves painted
    // faces into the untouched tail, so the tail size is not "input faces minus
    // painted faces". 0 on failure.
    static indexed_triangle_set smooth_triangle_mesh_local_its(const indexed_triangle_set& origin, const std::vector<uint32_t>& facet_indices, bool& ok,
                                                               SubdivisionFailure* failure = nullptr, size_t* region_face_count = nullptr);

    // Conservative upper bound of the face count produced by `level` local
    // subdivision steps over `painted_faces` of a mesh with `total_faces` faces:
    // only the selected faces subdivide (an interior face splits 1-to-4, a
    // boundary face into 2 or 3 children, and the children of one step become the
    // selection of the next), while every unselected face passes through
    // unchanged. The bound is exact for level <= 0 or an empty selection. It is
    // meant to be evaluated BEFORE running the subdivision: it only multiplies
    // face counts and builds no mesh. Overflow-safe: returns false and leaves
    // `out` untouched when the result would not fit in a size_t, in which case
    // the caller should treat the size as unbounded.
    static bool estimated_local_subdivision_face_count(size_t total_faces, size_t painted_faces, int level, size_t &out);

    // True when the estimated face count after `level` local subdivision steps
    // exceeds `face_budget` (or when the estimate itself overflows).
    static bool local_subdivision_exceeds_face_budget(size_t total_faces, size_t painted_faces, int level, size_t face_budget);

    // Verification hook: how many times smooth_triangle_mesh_local_its ran since the
    // last reset (or process start). Lets the UI layer check that an unchanged
    // failing selection is not recomputed on every frame.
    static uint64_t smooth_triangle_mesh_local_its_call_count();
    static void     reset_smooth_triangle_mesh_local_its_call_count();

    // Diagnostic hooks for the test-side STL export: vertex triples (indices into
    // the subdivision INPUT vertex array) of the parents that failed the pure
    // smooth round, and of the faces the downgrade finally landed on. Empty after
    // a call that needs no downgrade.
    static std::vector<stl_triangle_vertex_indices> diag_round1_failing_faces();
    static std::vector<stl_triangle_vertex_indices> diag_final_downgraded_faces();
};
} // namespace Slic3r

#endif // libslic3r_TriangleMeshDeal_hpp_
