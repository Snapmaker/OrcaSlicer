#pragma once

#ifndef libslic3r_TriangleMeshDeal_hpp_
#define libslic3r_TriangleMeshDeal_hpp_

#include "TriangleMesh.hpp"

namespace Slic3r {
class TriangleMeshDeal
{
public:
    static TriangleMesh smooth_triangle_mesh(const TriangleMesh& mesh, bool& ok);

    static TriangleMesh smooth_triangle_mesh_local(const TriangleMesh& mesh, const std::vector<uint32_t>& facet_indices, bool& ok);

    // Same subdivision as smooth_triangle_mesh_local, but returns the bare triangle
    // set without building TriangleMesh stats (neighbour graph, volume, bounding box,
    // patch count). Use it for previewing, where the stats would be recomputed and
    // thrown away on every step; construct the TriangleMesh only when applying.
    static indexed_triangle_set smooth_triangle_mesh_local_its(const indexed_triangle_set& origin, const std::vector<uint32_t>& facet_indices, bool& ok);
};
} // namespace Slic3r

#endif // libslic3r_TriangleMeshDeal_hpp_
