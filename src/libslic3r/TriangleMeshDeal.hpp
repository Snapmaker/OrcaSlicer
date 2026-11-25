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
    // Why a local subdivision level failed (valid when ok == false): the selected
    // edges were non-manifold, or the region could not be subdivided into valid
    // geometry (degenerate faces, float collapse).
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
    static indexed_triangle_set smooth_triangle_mesh_local_its(const indexed_triangle_set& origin, const std::vector<uint32_t>& facet_indices, bool& ok,
                                                               SubdivisionFailure* failure = nullptr);

    // Verification hook: how many times smooth_triangle_mesh_local_its ran since the
    // last reset (or process start). Lets the UI layer check that an unchanged
    // failing selection is not recomputed on every frame.
    static uint64_t smooth_triangle_mesh_local_its_call_count();
    static void     reset_smooth_triangle_mesh_local_its_call_count();
};
} // namespace Slic3r

#endif // libslic3r_TriangleMeshDeal_hpp_
