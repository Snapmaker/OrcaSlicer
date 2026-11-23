#pragma once

#ifndef libslic3r_TriangleMeshDeal_hpp_
#define libslic3r_TriangleMeshDeal_hpp_

#include "TriangleMesh.hpp"

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

    static TriangleMesh smooth_triangle_mesh(const TriangleMesh& mesh, bool& ok);

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
