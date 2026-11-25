#include "TriangleMeshDeal.hpp"

#include <array>
#include <igl/loop.h>
#include <assert.h>
#include <atomic>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <utility>

namespace {
// Counts every smooth_triangle_mesh_local_its call; see the accessors on
// TriangleMeshDeal.
std::atomic<uint64_t> s_local_its_call_count{0};
} // namespace

namespace Slic3r {
TriangleMesh TriangleMeshDeal::smooth_triangle_mesh(const TriangleMesh& mesh, bool& ok)
{
    if (mesh.its.vertices.empty() || mesh.its.indices.empty())
    {
        ok = false;
        return TriangleMesh();
    }

    using namespace igl;
    typedef Eigen::Matrix<float, Eigen::Dynamic, 3, Eigen::DontAlign | Eigen::RowMajor> RowMatrixX3f;
    typedef Eigen::Matrix<int, Eigen::Dynamic, 3, Eigen::DontAlign | Eigen::RowMajor>   RowMatrixX3i;

    indexed_triangle_set input = mesh.its;

    // Drop duplicate faces first: igl::loop() assumes a manifold mesh, and one repeated
    // face makes every of its edges non-manifold (4 incident faces), which derails the
    // edge pairing inside the subdivision until it runs out of bounds. No menu check
    // stops such input - repeated faces keep open_edges at 0, because the
    // opposite-direction edge pairs still match.
    {
        std::set<std::array<int, 3>>             seen_faces;
        std::vector<stl_triangle_vertex_indices> unique_faces;
        unique_faces.reserve(input.indices.size());
        for (const auto& face : input.indices) {
            // Sort the three indexes so the key matches a face regardless of rotation
            // and winding: both kinds of repeat make the edges non-manifold for igl::loop().
            std::array<int, 3> key = {face(0), face(1), face(2)};
            std::sort(key.begin(), key.end());
            if (seen_faces.insert(key).second)
                unique_faces.push_back(face);
        }
        input.indices = std::move(unique_faces);
    }

    // igl::loop() requires a compact mesh: igl::adjacency_list() sizes its output by
    // F.maxCoeff() + 1 while loop() iterates over V.rows(), so an unreferenced trailing
    // vertex (max face index < vertex count - 1) makes loop() read past the adjacency
    // list. Drop unreferenced vertices (and reindex the faces) on a local copy first.
    its_compactify_vertices(input);
    if (input.vertices.empty() || input.indices.empty()) {
        ok = false;
        return TriangleMesh();
    }

    // Reject meshes with non-manifold edges: an edge shared by more than two faces
    // cannot be paired inside igl::loop() and the subdivision crashes. Open edges
    // (a single face) are allowed: loop() handles boundary vertices explicitly.
    {
        std::map<std::pair<int, int>, int> edge_face_count;
        for (const auto& face : input.indices) {
            const int v[3] = {face(0), face(1), face(2)};
            for (int j = 0; j < 3; ++j) {
                const int a = std::min(v[j], v[(j + 1) % 3]);
                const int b = std::max(v[j], v[(j + 1) % 3]);
                if (++edge_face_count[{a, b}] > 2)
                {
                    ok = false;
                    return TriangleMesh();
                }
            }
        }
    }

    const Eigen::Map<const RowMatrixX3f> OV(input.vertices[0].data(), input.vertices.size(), 3);
    const Eigen::Map<const RowMatrixX3i> OF(input.indices[0].data(), input.indices.size(), 3);
    Eigen::MatrixX3f                     V;
    Eigen::MatrixX3i                     F;

    // igl::loop takes Eigen::PlainObjectBase parameters, which Eigen::Map is not,
    // so materialize the mapped views into plain matrices (this copies the data).
    // orca Eigen version 5.0.1  snap Eigen version 3.3.7
    RowMatrixX3f OV_mat = OV;
    RowMatrixX3i OF_mat = OF;

    ok = true;
    loop(OV_mat, OF_mat, V, F);

    indexed_triangle_set its;

    auto vert_num = V.rows();
    auto face_num = F.rows();
    its.vertices.reserve(vert_num);
    its.indices.reserve(face_num);

    for (int i = 0; i < vert_num; i++)
    {
        // stl_vertex is a 3x1 column vector, V.row(i) a 1x3 row expression.
        its.vertices.push_back(V.row(i).transpose());
    }

    for (int i = 0; i < face_num; i++)
    {
        // Same as above for the face indices.
        its.indices.push_back(F.row(i).transpose());
    }

    TriangleMesh result_mesh(its);
    return result_mesh;
    
}

// Face neighbours computed from the face list alone, pairing the faces that share an
// undirected edge (sorted vertex index pair). Works on indices only, so no vertex
// coordinates have to be provided or copied to build the neighbourhood.
static std::vector<Vec3i32> face_neighbors_from_faces(const std::vector<stl_triangle_vertex_indices>& faces)
{
    std::vector<Vec3i32>                             out(faces.size(), Vec3i32(-1, -1, -1));
    std::map<std::pair<uint32_t, uint32_t>, Vec2i32> edge_first_face;
    for (int i = 0; i < int(faces.size()); ++i) {
        const stl_triangle_vertex_indices& tri = faces[i];
        for (int j = 0; j < 3; ++j) {
            const uint32_t a = uint32_t(tri[j]);
            const uint32_t b = uint32_t(tri[(j + 1) % 3]);
            const auto     key    = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
            const auto     result = edge_first_face.emplace(key, Vec2i32(i, j));
            if (result.second)
                continue;
            const Vec2i32 first = result.first->second;
            if (first.x() >= 0) {
                // Pair the two faces; a third face sharing the same edge stays unpaired.
                out[size_t(first.x())][first.y()] = i;
                out[size_t(i)][j]                 = first.x();
                result.first->second              = Vec2i32(-1, -1);
            }
        }
    }
    return out;
}

// Loop subdivision variant for local smoothing. ALL selected faces are passed in and
// take part in the edge pairing, the boundary marking and the vertex 1-rings, but only
// the faces flagged in subdivide_face are actually split 1-to-4 - the others (the
// boundary faces of the selection) are left for the caller's stitching step. Rules:
//  - original vertices lying on the patch boundary (an edge with no second selected
//    face) keep their coordinates unchanged (weight 1 on themselves),
//  - vertices referenced by faces OUTSIDE the selection (the vertex_boundary_extra
//    flag) are treated like boundary vertices and stay fixed: moving them would
//    deform the untouched part of the mesh,
//  - a new vertex inserted on such a boundary edge is placed at the exact midpoint of
//    the two endpoints, so the subdivided boundary stays on the original edge; the
//    same midpoint rule applies to any interior edge whose both endpoints are boundary
//    vertices, so a new point never leaves the boundary polyline,
//  - edges shared by an interior and a boundary face are interior edges: they get the
//    standard Loop edge point, with the opposite vertex taken from either side,
//  - interior original vertices follow the standard Loop weights over their 1-ring of
//    ALL selected faces (neighbours of the boundary faces included).
//  - in linear mode (linear == true) no vertex is repositioned and every new edge
//    vertex is the exact midpoint of its endpoints: the construction of a valid
//    selection is then valid by construction and serves as the fallback when the
//    smooth result fails the child-face validation.
// Original vertex indices are preserved, new vertices are appended after them.
// If edge_new_vertex_out is given it receives the map from the undirected edge (sorted
// vertex pair) to the index of the vertex the subdivision inserted on that edge, so the
// caller can look shared edge points up in O(log E) instead of scanning the vertex array.
static indexed_triangle_set loop_subdivide_fixed_boundary(const std::vector<stl_vertex>&                    vertices,
                                                          const std::vector<stl_triangle_vertex_indices>&  faces,
                                                          const std::vector<char>&                         subdivide_face,
                                                          std::map<std::pair<uint32_t, uint32_t>, uint32_t>* edge_new_vertex_out = nullptr,
                                                          const std::vector<char>*                         vertex_boundary_extra = nullptr,
                                                          bool                                             linear = false,
                                                          const std::set<std::pair<uint32_t, uint32_t>>*   force_midpoint_edges = nullptr)
{
    const size_t vertex_count = vertices.size();
    const size_t face_count   = faces.size();

    indexed_triangle_set out;
    // The only copy of the vertex array: the result owns it and appends the new points.
    out.vertices = vertices;
    if (vertex_count == 0 || face_count == 0)
        return out;

    // Edge pairing over ALL selected faces: an edge with no second selected face is a
    // true patch-boundary edge, everything else (including edges shared with boundary
    // faces that are not split here) is an interior edge.
    const std::vector<Vec3i32> ff_index = face_neighbors_from_faces(faces);

    // One new vertex per undirected edge of a split face, appended after the originals.
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> edge_new_vertex;
    // 1-ring neighbours and boundary flag of the original vertices.
    std::vector<std::set<uint32_t>> vertex_neighbors(vertex_count);
    std::vector<char>               vertex_on_boundary(vertex_count, 0);

    for (size_t face = 0; face < face_count; ++face)
    {
        const stl_triangle_vertex_indices& tri = faces[face];
        for (int j = 0; j < 3; ++j)
        {
            const uint32_t a = tri[j];
            const uint32_t b = tri[(j + 1) % 3];
            vertex_neighbors[a].insert(b);
            vertex_neighbors[b].insert(a);
            if (ff_index[face][j] < 0) {
                vertex_on_boundary[a] = 1;
                vertex_on_boundary[b] = 1;
            }
        }
    }
    if (vertex_boundary_extra != nullptr) {
        // Vertices referenced by faces outside the selection stay fixed as well.
        for (size_t v = 0; v < vertex_count; ++v)
            vertex_on_boundary[v] |= (*vertex_boundary_extra)[v];
    }

    for (size_t face = 0; face < face_count; ++face) {
        if (! subdivide_face[face])
            continue; // boundary face of the selection: stitched by the caller
        const stl_triangle_vertex_indices& tri = faces[face];
        std::array<int, 3>                 mid;
        for (int j = 0; j < 3; ++j) {
            const uint32_t a = tri[j];
            const uint32_t b = tri[(j + 1) % 3];
            const auto edge_key = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
            const auto result   = edge_new_vertex.emplace(edge_key, uint32_t(out.vertices.size()));
            if (result.second) {
                if (linear || ff_index[face][j] < 0 || (vertex_on_boundary[a] && vertex_on_boundary[b])
                    || (force_midpoint_edges != nullptr && force_midpoint_edges->count(edge_key) > 0))
                    {
                    // Boundary edge: exact midpoint of the two endpoints.
                    const stl_vertex& va = vertices[a];
                    const stl_vertex& vb = vertices[b];
                    out.vertices.push_back(0.5f * (va + vb));
                } else {
                    // Interior edge: standard Loop edge rule 3/8*(a+b) + 1/8*(c+d), with
                    // c and d the vertices opposite the edge in its two incident faces.
                    // The neighbour face may be a boundary face that is not split here,
                    // it still contributes its opposite vertex.
                    const uint32_t                     c    = tri[(j + 2) % 3];
                    const stl_triangle_vertex_indices& ntri = faces[size_t(ff_index[face][j])];
                    uint32_t                           d    = ntri[0];
                    if (d == a || d == b) {
                        d = ntri[1];
                        if (d == a || d == b)
                            d = ntri[2];
                    }
                    const stl_vertex& va = vertices[a];
                    const stl_vertex& vb = vertices[b];
                    const stl_vertex& vc = vertices[c];
                    const stl_vertex& vd = vertices[d];
                    out.vertices.push_back(3.f / 8.f * (va + vb) + 1.f / 8.f * (vc + vd));
                }
            }
            mid[j] = int(result.first->second);
        }
        // 1-to-4 face split: mid[j] is the vertex created on the edge
        // (tri[j], tri[(j+1)%3]); the winding of every sub-face matches the input face.
        out.indices.emplace_back(tri[0], mid[0], mid[2]);
        out.indices.emplace_back(tri[1], mid[1], mid[0]);
        out.indices.emplace_back(mid[0], mid[1], mid[2]);
        out.indices.emplace_back(mid[1], tri[2], mid[2]);
    }

    // Reposition the original vertices: boundary vertices stay fixed, interior
    // vertices follow the standard Loop weights. Skipped in linear mode.
    //
    // The Loop position can fall outside the local star of the vertex around a
    // concave boundary (a fan filling a notch) and flip the incident faces, so the
    // displacement is clamped: the oriented area of an incident face is linear in
    // the displacement (only one corner moves at a time), which gives the largest
    // safe fraction in closed form. Each vertex is clamped against the positions of
    // the other corners AFTER their own clamps were applied.
    if (! linear) {
        std::vector<std::vector<int>> vertex_faces(vertex_count);
        for (size_t face = 0; face < face_count; ++face) {
            const stl_triangle_vertex_indices& tri = faces[face];
            vertex_faces[size_t(tri[0])].push_back(int(face));
            vertex_faces[size_t(tri[1])].push_back(int(face));
            vertex_faces[size_t(tri[2])].push_back(int(face));
        }

        for (size_t v = 0; v < vertex_count; ++v) {
            if (vertex_on_boundary[v])
                continue;
            const size_t n = vertex_neighbors[v].size();
            if (n == 0)
                continue;
            const float beta = (n == 3) ? (3.f / 16.f) : (3.f / (8.f * float(n)));
            stl_vertex    sum = stl_vertex::Zero();
            for (uint32_t nb : vertex_neighbors[v])
                sum += vertices[nb];
            const stl_vertex loop_pos = (1.f - float(n) * beta) * vertices[v] + beta * sum;
            const stl_vertex orig     = out.vertices[v];

            // Largest displacement fraction that keeps every incident face oriented the
            // way it is now (0 = the vertex must not move at all).
            double t = 1.;
            for (int f : vertex_faces[v]) {
                const stl_triangle_vertex_indices& tri = faces[size_t(f)];
                Vec3d corner[3], moved[3];
                for (int i = 0; i < 3; ++i) {
                    corner[i] = out.vertices[size_t(tri[i])].cast<double>();
                    moved[i]  = (size_t(tri[i]) == v) ? loop_pos.cast<double>() : corner[i];
                }
                const Vec3d   n  = (corner[1] - corner[0]).cross(corner[2] - corner[0]);
                const double  a0 = n.squaredNorm(); // current oriented area along its own normal, > 0
                const double  a1 = n.dot((moved[1] - moved[0]).cross(moved[2] - moved[0]));
                if (a0 <= 0.) {
                    // Degenerate incident face: any move is unsafe, keep the vertex put.
                    t = 0.;
                    break;
                }
                if (a1 < 0.) {
                    // a(t) = a0 + t * (a1 - a0); a(t) > 0 <=> t < a0 / (a0 - a1).
                    // Shave off a margin so the sliver stays strictly positive.
                    t = std::min(t, a0 / (a0 - a1) * 0.9999);
                }
            }
            out.vertices[v] = (orig + (loop_pos - orig) * float(t)).eval();
        }
    }

    if (edge_new_vertex_out != nullptr)
        *edge_new_vertex_out = std::move(edge_new_vertex);

    return out;
}


// Validates the generated child faces of one subdivision level against their direct
// parent faces: every child must keep the parent's orientation and a strictly
// positive area relative to the parent's scale. The parent normal and scale are
// taken from the INPUT vertices of this level (parent_vertices), the child cross
// products from the final smoothed float coordinates: a parent that flipped during
// the smoothing would otherwise be measured against its own flipped normal, and
// children flipped together with it would pass. The check also catches Loop edge
// points leaving a concave parent, exact degenerates (a smoothed vertex landing
// exactly on an edge) and faces collapsed by float rounding. Untouched faces are
// not checked: they are the input verbatim.
static bool validate_region_children(const std::vector<stl_vertex>& parent_vertices,
                                     const indexed_triangle_set&     its,
                                     size_t                         region_faces,
                                     const std::vector<int>&        child_parent,
                                     const std::vector<stl_triangle_vertex_indices>& parents,
                                     std::vector<int>*              failed_parents_out = nullptr)
{
    // Minimum child area as a fraction of the parent's longest edge squared.
    // Deliberately tiny: it only has to reject flips (negative), exact degenerates
    // (zero) and float-collapsed faces, not thin-but-legitimate ones.
    // Full scan (no early exit): the caller wants ALL failing parents so it can
    // downgrade them in one round instead of one per rebuild.
    constexpr double MinChildAreaRatio = 1e-9;

    bool all_valid = true;
    for (size_t i = 0; i < region_faces; ++i) {
        const stl_triangle_vertex_indices& p    = parents[size_t(child_parent[i])];
        const Vec3d p0 = parent_vertices[size_t(p[0])].cast<double>();
        const Vec3d p1 = parent_vertices[size_t(p[1])].cast<double>();
        const Vec3d p2 = parent_vertices[size_t(p[2])].cast<double>();
        const double L2 = std::max({(p1 - p0).squaredNorm(), (p2 - p1).squaredNorm(), (p0 - p2).squaredNorm()});
        if (! (L2 > 0.) || ! std::isfinite(L2)) {
            // the parent itself is degenerate: children cannot be valid
            all_valid = false;
            if (failed_parents_out != nullptr)
                failed_parents_out->push_back(child_parent[i]);
            continue;
        }
        const Vec3d n = (p1 - p0).cross(p2 - p0);
        const double n_norm = n.norm();
        if (! (n_norm > 0.) || ! std::isfinite(n_norm)) {
            all_valid = false;
            if (failed_parents_out != nullptr)
                failed_parents_out->push_back(child_parent[i]);
            continue;
        }
        const Vec3d nhat = n / n_norm;

        const stl_triangle_vertex_indices& c    = its.indices[i];
        const Vec3d c0 = its.vertices[size_t(c[0])].cast<double>();
        const Vec3d c1 = its.vertices[size_t(c[1])].cast<double>();
        const Vec3d c2 = its.vertices[size_t(c[2])].cast<double>();
        const double proj = nhat.dot((c1 - c0).cross(c2 - c0));
        if (! std::isfinite(proj) || ! (proj > MinChildAreaRatio * L2)) {
            all_valid = false;
            if (failed_parents_out != nullptr)
                failed_parents_out->push_back(child_parent[i]);
        }
    }
    return all_valid;
}

// Diagnostic hooks: vertex triples (indices into the level's INPUT vertex array)
// of the round-1 failing parents and of the finally downgraded faces. Populated
// by smooth_triangle_mesh_local_its for the test-side STL export.
static std::vector<stl_triangle_vertex_indices> s_round1_failing;
static std::vector<stl_triangle_vertex_indices> s_final_downgraded;

indexed_triangle_set TriangleMeshDeal::smooth_triangle_mesh_local_its(const indexed_triangle_set& origin, const std::vector<uint32_t>& facet_indices, bool& ok, SubdivisionFailure* failure) {
    s_local_its_call_count.fetch_add(1, std::memory_order_relaxed);
    if (failure != nullptr)
        *failure = SubdivisionFailure::None;
    s_round1_failing.clear();
    s_final_downgraded.clear();

    // Only the selected faces take part in the subdivision; the patch BOUNDARY,
    // however, is detected against the full mesh (see the neighborhood pass below):
    // a paired edge that still has outside faces, or a vertex referenced by
    // unselected faces, is degraded to boundary treatment so the untouched geometry
    // keeps its exact coordinates.
    std::vector<char> is_selected(origin.indices.size(), 0);
    for (uint32_t facet : facet_indices)
        if (facet < origin.indices.size())
            is_selected[facet] = 1;

    // Zero-area selected faces (collinear or coincident vertices, e.g. the 11
    // slivers shipped inside 3DBenchy) pass through untouched instead of failing
    // the whole region: their children would be zero-area in ANY construction —
    // smooth, linear, anything — so validate_region_children can never accept
    // them, and subdividing a zero-area face carries no geometry anyway.
    // Unselecting them here routes every downstream pass (boundary detection,
    // stitching, the validator) through the existing untouched-face path; the
    // only side effect is that edges shared with a real selected face degrade to
    // conservative boundary treatment, same as any other unselected neighbor.
    for (size_t i = 0; i < origin.indices.size(); ++i) {
        if (! is_selected[i])
            continue;
        const stl_triangle_vertex_indices& f = origin.indices[i];
        const Vec3d p0 = origin.vertices[size_t(f(0))].cast<double>();
        const Vec3d p1 = origin.vertices[size_t(f(1))].cast<double>();
        const Vec3d p2 = origin.vertices[size_t(f(2))].cast<double>();
        const double n_norm = (p1 - p0).cross(p2 - p0).norm();
        if (! (n_norm > 0.) || ! std::isfinite(n_norm))
            is_selected[i] = 0;
    }

    // Drop duplicate faces inside the selection (either winding): two copies share
    // every edge four times and derail the pairing.
    std::vector<stl_triangle_vertex_indices> selected_faces;
    selected_faces.reserve(facet_indices.size());
    {
        std::set<std::array<int, 3>> seen_faces;
        for (size_t i = 0; i < origin.indices.size(); ++i) {
            if (!is_selected[i])
                continue;
            const stl_triangle_vertex_indices& face = origin.indices[i];
            std::array<int, 3>                 key  = {face(0), face(1), face(2)};
            std::sort(key.begin(), key.end());
            if (seen_faces.insert(key).second)
                selected_faces.push_back(face);
        }
    }

    // Reject edges shared by more than two selected faces: such an edge cannot be
    // paired and would corrupt the boundary stitching below. Boundary edges (a
    // single selected face) are fine.
    {
        std::map<std::pair<int, int>, int> edge_face_count;
        for (const auto& face : selected_faces) {
            const int v[3] = {face(0), face(1), face(2)};
            for (int j = 0; j < 3; ++j) {
                const int a = std::min(v[j], v[(j + 1) % 3]);
                const int b = std::max(v[j], v[(j + 1) % 3]);
                if (++edge_face_count[{a, b}] > 2)
                {
                    ok = false;
                    if (failure != nullptr)
                        *failure = SubdivisionFailure::NonManifold;
                    return indexed_triangle_set();
                }
            }
        }
    }

    const std::vector<stl_vertex>& vertices = origin.vertices;

    auto selected_ff_index = face_neighbors_from_faces(selected_faces);

    // The patch boundary must respect the FULL mesh, not just the selection, or the
    // subdivision tears the untouched geometry that still references these edges and
    // vertices. Building the full-mesh adjacency would cost O(F_total) map
    // insertions, so instead record the selection's edges and scan the UNSELECTED
    // faces once: a selected edge hit by an outside face (total incident faces > 2)
    // is non-manifold and is degraded to a patch boundary below.

    // Selected edge key -> number of incident faces OUTSIDE the selection; edges
    // that no unselected face touches are absent or zero.
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> selected_edge_outside_hits;
    for (const auto& face : selected_faces) {
        const uint32_t v[3] = {uint32_t(face(0)), uint32_t(face(1)), uint32_t(face(2))};
        for (int j = 0; j < 3; ++j) {
            const uint32_t a = std::min(v[j], v[(j + 1) % 3]);
            const uint32_t b = std::max(v[j], v[(j + 1) % 3]);
            selected_edge_outside_hits[{a, b}] = 0;
        }
    }

    // Any vertex referenced by a face OUTSIDE the selection stays fixed: moving it
    // would deform the untouched geometry (e.g. two meshes touching at one vertex).
    // The same single pass over the unselected faces also counts the edge hits.
    std::vector<char> vertex_touched_by_unselected(vertices.size(), 0);
    for (size_t i = 0; i < origin.indices.size(); ++i) {
        if (is_selected[i])
            continue;
        const stl_triangle_vertex_indices& face = origin.indices[i];
        vertex_touched_by_unselected[size_t(face(0))] = 1;
        vertex_touched_by_unselected[size_t(face(1))] = 1;
        vertex_touched_by_unselected[size_t(face(2))] = 1;
        const uint32_t v[3] = {uint32_t(face(0)), uint32_t(face(1)), uint32_t(face(2))};
        for (int j = 0; j < 3; ++j) {
            const uint32_t a = std::min(v[j], v[(j + 1) % 3]);
            const uint32_t b = std::max(v[j], v[(j + 1) % 3]);
            const auto it = selected_edge_outside_hits.find(std::make_pair(a, b));
            if (it != selected_edge_outside_hits.end())
                ++it->second;
        }
    }

    // A paired edge with outside incident faces (non-manifold input, e.g. a flap on
    // a tetrahedron edge) would be replaced by a Loop point off the original edge
    // line, tearing the flap. Clear the pairing on both sides (each slot is checked
    // against the same key set): the boundary rules then keep the endpoints fixed
    // and the shared edge unsplit.
    for (size_t face = 0; face < selected_faces.size(); ++face) {
        const stl_triangle_vertex_indices& tri = selected_faces[face];
        for (int j = 0; j < 3; ++j) {
            if (selected_ff_index[face][j] < 0)
                continue;
            const uint32_t a = uint32_t(tri[j]);
            const uint32_t b = uint32_t(tri[(j + 1) % 3]);
            const auto key = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
            const auto it  = selected_edge_outside_hits.find(key);
            if (it != selected_edge_outside_hits.end() && it->second > 0)
                selected_ff_index[face][j] = -1;
        }
    }

    // Per selected face, the slots of its inner edges (edges shared with another
    // selected face): all three slots = interior face, fewer = boundary face.
    std::vector<std::vector<uint32_t>> bound_face_index(selected_faces.size());
    for (size_t i = 0; i < selected_ff_index.size(); i++) {
        for (int j = 0; j < 3; j++) {
            if (selected_ff_index[i][j] != -1)
                bound_face_index[i].push_back(uint32_t(j));
        }
    }

    // Pairs of boundary faces sharing an inner edge both drop it from their list, so the
    // two sides keep the shared edge unsplit and stay consistent.
    for (size_t face = 0; face < bound_face_index.size(); ++face) {
        std::vector<uint32_t>& bound_index = bound_face_index[face];
        if (bound_index.size() == 3)
            continue; // interior face
        for (int i = int(bound_index.size()) - 1; i >= 0; i--) {
            const auto neigh_face = selected_ff_index[face][bound_index[i]];
            if (neigh_face >= 0 && bound_face_index[size_t(neigh_face)].size() < 3)
            {
                std::vector<uint32_t>& neigh_bound = bound_face_index[size_t(neigh_face)];
                bound_index.erase(bound_index.begin() + i);
                for (size_t j = 0; j < neigh_bound.size(); j++)
                {
                    if (selected_ff_index[size_t(neigh_face)][neigh_bound[j]] == int32_t(face))
                    {
                        neigh_bound.erase(neigh_bound.begin() + j);
                        break;
                    }
                }
            }
        }
    }

    // Only the faces whose three edges are all inner are split 1-to-4; the boundary
    // faces are stitched below on the shared (possibly repositioned) vertex array.
    std::vector<char> subdivide_face(selected_faces.size(), 0);
    for (size_t i = 0; i < selected_faces.size(); ++i)
        subdivide_face[i] = bound_face_index[i].size() == 3 ? 1 : 0;

    // Build the region (split + stitched children) with the smooth Loop
    // configuration and validate every child against its direct parent. One
    // repair round: the failing parents' three edges fall back to exact midpoints
    // and their vertices are pinned, then the level is rebuilt and re-validated.
    // If the second detection still finds problems (a fundamentally concave
    // region whose normals keep flipping), stop processing and ship the INITIAL
    // Loop result unchanged: reprocessing such a region further mostly freezes
    // midpoint patches over it, which visually hurts more than the few invalid
    // children it would remove.
    indexed_triangle_set its;
    std::vector<int>     child_parent; // direct parent (selected face index) of every region child
    std::set<std::pair<uint32_t, uint32_t>> forced_midpoint_edges; // downgraded edges → exact midpoints
    std::vector<char>    pinned_vertices(vertices.size(), 0);      // downgraded vertices → never repositioned
    std::vector<int>     failed_parents;
    bool                 level_valid = false;
    indexed_triangle_set initial_loop_result; // round-0 build, shipped when the repair round fails
    for (int attempt = 0; attempt < 2 && ! level_valid; ++attempt) {
        child_parent.clear();
        failed_parents.clear();

        std::vector<char> effective_pins(vertex_touched_by_unselected);
        for (size_t v = 0; v < effective_pins.size(); ++v)
            effective_pins[v] |= pinned_vertices[v];

        // Edge -> index of the vertex inserted on that edge; fresh per attempt, so
        // the two attempts share no vertex indices.
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> edge_new_vertex;
        its = loop_subdivide_fixed_boundary(vertices, selected_faces, subdivide_face, &edge_new_vertex, &effective_pins, false,
                                            &forced_midpoint_edges);

        // The split children were emitted first, four per split face in face order:
        // record their parents for the validation below.
        for (size_t f = 0; f < selected_faces.size(); ++f)
            if (subdivide_face[f])
                for (int k = 0; k < 4; ++k)
                    child_parent.push_back(int(f));

    // Midpoints manually created for inner edges that never entered the subdivided patch
    // (both incident faces are boundary faces); cached so that boundary faces sharing such
    // an edge reuse the same vertex instead of duplicating it. The endpoints may have
    // been repositioned by the subdivision, so read them from its.vertices.
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> created_edge_midpoint;
    // Index of the vertex sitting at the midpoint of the edge (u, v): either the one the
    // subdivision already inserted on it, or a newly created one.
    auto edge_midpoint_vertex = [&](int u, int v) -> int {
        const auto key = u < v ? std::make_pair(uint32_t(u), uint32_t(v)) : std::make_pair(uint32_t(v), uint32_t(u));
        const auto it  = edge_new_vertex.find(key);
        if (it != edge_new_vertex.end())
            return int(it->second);
        const auto cit = created_edge_midpoint.find(key);
        if (cit != created_edge_midpoint.end())
            return int(cit->second);
        const stl_vertex& vu = its.vertices[size_t(u)];
        const stl_vertex& vv = its.vertices[size_t(v)];
        its.vertices.push_back(0.5f * (vu + vv));
        const int idx = int(its.vertices.size()) - 1;
        created_edge_midpoint.emplace(key, uint32_t(idx));
        return idx;
    };

    for (size_t face = 0; face < bound_face_index.size(); ++face)
    {
        const std::vector<uint32_t>&      bound_index = bound_face_index[face];
        if (bound_index.size() == 3)
            continue; // interior face, already subdivided as part of the patch
        const stl_triangle_vertex_indices& tri = selected_faces[face];
        if (bound_index.size() == 0)
        {
            // Average the UPDATED positions: the vertices may have been moved by the
            // smoothing pass (e.g. the center of a fully selected fan), and the
            // centroid of the ORIGINAL face could fall outside the moved face,
            // flipping the children.
            stl_vertex mid_point(stl_vertex::Zero());
            for (int i = 0; i < 3; i++)
            {
                mid_point += its.vertices[size_t(tri[i])];
            }
            mid_point /= 3;

            its.vertices.push_back(mid_point);
            uint32_t mid = its.vertices.size() - 1;
            its.indices.emplace_back(tri[0], tri[1], mid);
            its.indices.emplace_back(tri[1], tri[2], mid);
            its.indices.emplace_back(tri[2], tri[0], mid);
            child_parent.insert(child_parent.end(), 3, int(face));
        }
        else if (bound_index.size() == 1)
        {
            // One edge shared with a subdivided face: bisect the triangle from the
            // opposite vertex to the midpoint of that edge.
            const int e   = int(bound_index[0]);
            const int a   = tri[e];
            const int b   = tri[(e + 1) % 3];
            const int c   = tri[(e + 2) % 3];
            const int mid = edge_midpoint_vertex(a, b);

            its.indices.emplace_back(a, mid, c);
            its.indices.emplace_back(mid, b, c);
            child_parent.insert(child_parent.end(), 2, int(face));
        }
        else // bound_index.size() == 2: two inner edges, one boundary edge
        {
            // Cut off the corner at the vertex shared by the two inner edges by
            // connecting their midpoints, then split the remaining quad along the
            // shorter of its two diagonals to avoid thin triangles: three triangles.
            const int e0 = int(bound_index[0]);
            const int e1 = int(bound_index[1]);
            // Order the two slots so that the in-edge ends at the shared vertex s and the
            // out-edge starts at it, i.e. (in_slot + 1) % 3 == out_slot.
            const int in_slot  = (e0 + 1) % 3 == e1 ? e0 : e1;
            const int out_slot = (e0 + 1) % 3 == e1 ? e1 : e0;
            const int s        = tri[out_slot];
            const int i_a      = tri[in_slot];
            const int o_a      = tri[(out_slot + 1) % 3];

            const int m_in  = edge_midpoint_vertex(i_a, s);
            const int m_out = edge_midpoint_vertex(s, o_a);

            its.indices.emplace_back(m_in, s, m_out); // corner triangle at the shared vertex

            // Split the remaining quad (i_a, m_in, m_out, o_a) along the shorter diagonal.
            const double d_m_in_o_a  = (its.vertices[size_t(m_in)] - its.vertices[size_t(o_a)]).squaredNorm();
            const double d_m_out_i_a = (its.vertices[size_t(m_out)] - its.vertices[size_t(i_a)]).squaredNorm();
            if (d_m_in_o_a <= d_m_out_i_a) {
                its.indices.emplace_back(i_a, m_in, o_a);
                its.indices.emplace_back(m_in, m_out, o_a);
            } else {
                its.indices.emplace_back(i_a, m_in, m_out);
                its.indices.emplace_back(i_a, m_out, o_a);
            }
            child_parent.insert(child_parent.end(), 3, int(face));
        }
    }

    // Every region child must keep its direct parent's orientation and a minimum
    // area (see validate_region_children). The first round's failure schedules
    // exactly one repair round; a second failure ends the processing.
    if (validate_region_children(vertices, its, child_parent.size(), child_parent, selected_faces, &failed_parents)) {
        level_valid = true;
        break;
    }
    if (attempt == 0) {
        // Diagnostic hook: the parents that failed the pure-smooth round (one
        // entry per parent, not per failing child).
        std::set<int> seen;
        for (int pi : failed_parents)
            if (seen.insert(pi).second)
                s_round1_failing.push_back(selected_faces[size_t(pi)]);
        // Keep the initial build for the ship-as-is fallback and schedule the
        // single repair round.
        initial_loop_result = std::move(its);
        for (int pi : failed_parents) {
            const stl_triangle_vertex_indices& tri = selected_faces[size_t(pi)];
            for (int j = 0; j < 3; ++j) {
                const uint32_t a = tri[j];
                const uint32_t b = tri[(j + 1) % 3];
                forced_midpoint_edges.insert(a < b ? std::make_pair(a, b) : std::make_pair(b, a));
                pinned_vertices[size_t(a)] = 1;
                pinned_vertices[size_t(b)] = 1;
            }
        }
    }
    }

    if (! level_valid) {
        // Two detections both found problems: ship the initial Loop result.
        its = std::move(initial_loop_result);
    }

    ok = true;

    // Diagnostic hook: the faces the downgrade finally landed on (any of their
    // three edges carries a forced midpoint). Empty when the initial result
    // shipped - no downgrade applies to that output.
    if (level_valid) {
        for (size_t f = 0; f < selected_faces.size(); ++f) {
            const stl_triangle_vertex_indices& tri = selected_faces[f];
            for (int j = 0; j < 3; ++j) {
                const uint32_t a = tri[j];
                const uint32_t b = tri[(j + 1) % 3];
                if (forced_midpoint_edges.count(a < b ? std::make_pair(a, b) : std::make_pair(b, a)) > 0) {
                    s_final_downgraded.push_back(tri);
                    break;
                }
            }
        }
    }

    // Finally append the untouched part of the original mesh: every face outside the
    // selection. Its vertex indices are still valid in its.vertices, where the original
    // vertices kept their indices and the ones untouched by the patch their coordinates.
    for (size_t i = 0; i < origin.indices.size(); ++i) {
        if (is_selected[i])
            continue;
        its.indices.push_back(origin.indices[i]);
    }

    return its;

}

TriangleMesh TriangleMeshDeal::smooth_triangle_mesh_local(const TriangleMesh& origin_mesh, const std::vector<uint32_t>& facet_indices, bool& ok)
{
    // Build the full stats (neighbour graph, volume, bounding box, ...) only here,
    // when the result is actually applied, not during previewing.
    return TriangleMesh(smooth_triangle_mesh_local_its(origin_mesh.its, facet_indices, ok));
}

uint64_t TriangleMeshDeal::smooth_triangle_mesh_local_its_call_count()
{
    return s_local_its_call_count.load(std::memory_order_relaxed);
}

void TriangleMeshDeal::reset_smooth_triangle_mesh_local_its_call_count()
{
    s_local_its_call_count.store(0, std::memory_order_relaxed);
}

std::vector<stl_triangle_vertex_indices> TriangleMeshDeal::diag_round1_failing_faces()
{
    return s_round1_failing;
}

std::vector<stl_triangle_vertex_indices> TriangleMeshDeal::diag_final_downgraded_faces()
{
    return s_final_downgraded;
}

} // namespace Slic3r
