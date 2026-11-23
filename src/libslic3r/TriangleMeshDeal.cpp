#include "TriangleMeshDeal.hpp"

#include <array>
#include <igl/loop.h>
#include <assert.h>
#include <atomic>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <array>
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
    // opposite-direction edge pairs still match. Keep the first occurrence only.
    {
        std::set<std::array<int, 3>>             seen_faces;
        std::vector<stl_triangle_vertex_indices> unique_faces;
        unique_faces.reserve(input.indices.size());
        for (const auto& face : input.indices) {
            // Fully sort the three vertex indexes: the key then matches a face
            // regardless of rotation AND winding, so both exact repeats and
            // opposite-winding repeats (zero-thickness walls) are caught - either
            // kind makes the edges non-manifold and breaks igl::loop().
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

    // Temporary guard: reject meshes with non-manifold edges - an edge shared by more
    // than two faces cannot be paired inside igl::loop() (it pairs exactly two faces
    // per edge) and the subdivision runs out of bounds and crashes. Duplicate faces
    // were already removed above; this catches every other source, e.g. face fans.
    // Open edges (a single face) are deliberately allowed: loop() handles boundary
    // vertices explicitly.
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

    // Use Map to map the vertices and indicies into Matrixes without requiring a copy.
    const Eigen::Map<const RowMatrixX3f> OV(input.vertices[0].data(), input.vertices.size(), 3);
    const Eigen::Map<const RowMatrixX3i> OF(input.indices[0].data(), input.indices.size(), 3);
    Eigen::MatrixX3f                     V;
    Eigen::MatrixX3i                     F;

    // igl::loop takes Eigen::PlainObjectBase parameters, which Eigen::Map is not,
    // so materialize the mapped views into plain matrices (this copies the data).
    //orca Eigen version 5.0.1  snap Eigen version 3.3.7
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
        // its.vertices holds 3x1 column vectors (stl_vertex), while V.row(i) is a 1x3
        // row expression; transpose keeps the conversion dimensions exact instead of
        // relying on Eigen's runtime-only size check (an eigen_assert in debug builds).
        its.vertices.push_back(V.row(i).transpose());
    }

    for (int i = 0; i < face_num; i++)
    {
        // Same as above: stl_triangle_vertex_indices is a 3x1 column vector.
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
//  - a new vertex inserted on such a boundary edge is placed at the exact midpoint of
//    the two endpoints, so the subdivided boundary stays on the original edge; the
//    same midpoint rule applies to any interior edge whose both endpoints are boundary
//    vertices, so a new point never leaves the boundary polyline,
//  - edges shared by an interior and a boundary face are interior edges: they get the
//    standard Loop edge point, with the opposite vertex taken from either side,
//  - interior original vertices follow the standard Loop weights over their 1-ring of
//    ALL selected faces (neighbours of the boundary faces included).
// Original vertex indices are preserved, new vertices are appended after them.
// If edge_new_vertex_out is given it receives the map from the undirected edge (sorted
// vertex pair) to the index of the vertex the subdivision inserted on that edge, so the
// caller can look shared edge points up in O(log E) instead of scanning the vertex array.
static indexed_triangle_set loop_subdivide_fixed_boundary(const std::vector<stl_vertex>&                    vertices,
                                                          const std::vector<stl_triangle_vertex_indices>&  faces,
                                                          const std::vector<char>&                         subdivide_face,
                                                          std::map<std::pair<uint32_t, uint32_t>, uint32_t>* edge_new_vertex_out = nullptr)
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

    for (size_t face = 0; face < face_count; ++face) {
        if (! subdivide_face[face])
            continue; // boundary face of the selection: not split here, keep verbatim
        const stl_triangle_vertex_indices& tri = faces[face];
        std::array<int, 3>                 mid;
        for (int j = 0; j < 3; ++j) {
            const uint32_t a = tri[j];
            const uint32_t b = tri[(j + 1) % 3];
            const auto edge_key = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
            const auto result   = edge_new_vertex.emplace(edge_key, uint32_t(out.vertices.size()));
            if (result.second) {
                if (ff_index[face][j] < 0 || (vertex_on_boundary[a] && vertex_on_boundary[b]))
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
        // Same 1-to-4 face split as igl::loop, with mid[j] the midpoint of the edge
        // (tri[j], tri[(j+1)%3]); the winding of every sub-face matches the input face.
        out.indices.emplace_back(tri[0], mid[0], mid[2]);
        out.indices.emplace_back(tri[1], mid[1], mid[0]);
        out.indices.emplace_back(mid[0], mid[1], mid[2]);
        out.indices.emplace_back(mid[1], tri[2], mid[2]);
    }

    // Reposition the original vertices: boundary vertices stay fixed, interior vertices
    // follow the standard Loop weights.
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
        out.vertices[v] = (1.f - float(n) * beta) * vertices[v] + beta * sum;
    }

    if (edge_new_vertex_out != nullptr)
        *edge_new_vertex_out = std::move(edge_new_vertex);

    return out;
}


indexed_triangle_set TriangleMeshDeal::smooth_triangle_mesh_local_its(const indexed_triangle_set& origin, const std::vector<uint32_t>& facet_indices, bool& ok) {
    s_local_its_call_count.fetch_add(1, std::memory_order_relaxed);

    // Guards limited to the SELECTION: only the selected faces take part in the
    // subdivision and its edge pairing, so duplicates or non-manifold edges outside the
    // selection cannot break this algorithm - untouched faces are appended verbatim.
    // The vertex array is kept untouched as well: original vertex indexes are preserved.
    std::vector<char> is_selected(origin.indices.size(), 0);
    for (uint32_t facet : facet_indices)
        if (facet < origin.indices.size())
            is_selected[facet] = 1;

    // Drop duplicate faces inside the selection (either winding): two selected copies
    // of one face share every edge four times and derail the pairing; keep one copy.
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

    // Temporary guard: an edge of the selected patch shared by more than two selected
    // faces cannot be paired and corrupts the boundary stitching below. Boundary edges
    // of the patch (a single selected face) are fine - that is the selection outline.
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
                    return indexed_triangle_set();
                }
            }
        }
    }

    // Only the face records of the selection are copied; the vertex coordinates are
    // referenced from the original mesh through indices until the result is built.
    const std::vector<stl_vertex>& vertices = origin.vertices;

    auto selected_ff_index = face_neighbors_from_faces(selected_faces);

    // For every selected face record the slots of its inner edges (edges shared with
    // another selected face), indexed by face: an entry holding all three slots marks
    // an interior face, an entry with fewer than three a boundary face of the selection.
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

    // Every selected face enters the subdivision (for edge pairing, boundary marking
    // and vertex 1-rings), but only the interior faces - those whose three edges are
    // all inner - are actually split 1-to-4. The boundary faces keep their vertices in
    // the (possibly repositioned) shared vertex array and are stitched below.
    std::vector<char> subdivide_face(selected_faces.size(), 0);
    for (size_t i = 0; i < selected_faces.size(); ++i)
        subdivide_face[i] = bound_face_index[i].size() == 3 ? 1 : 0;

    ok = true;

    // Edge -> index of the vertex the subdivision inserted on that edge (the edges of
    // the split faces; the split-face edges shared with boundary faces are included).
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> edge_new_vertex;
    indexed_triangle_set                              its = loop_subdivide_fixed_boundary(vertices, selected_faces, subdivide_face, &edge_new_vertex);

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
            // Average the UPDATED positions: a face can end up here while one of its
            // vertices is interior to the patch and was moved by the smoothing pass
            // (e.g. the center of a fully selected fan, whose inner edges were all
            // dropped pairwise because every neighbour is a boundary face). The
            // centroid of the ORIGINAL face may fall outside the moved face, flipping
            // the child faces and overlapping the neighbours.
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
        }
        else if (bound_index.size() == 1)
        {
            // Exactly one edge (slot bound_index[0]) is shared with a subdivided face:
            // bisect the boundary triangle along the segment joining the opposite vertex
            // and the midpoint of that edge.
            const int e   = int(bound_index[0]);
            const int a   = tri[e];
            const int b   = tri[(e + 1) % 3];
            const int c   = tri[(e + 2) % 3];
            const int mid = edge_midpoint_vertex(a, b);

            its.indices.emplace_back(a, mid, c);
            its.indices.emplace_back(mid, b, c);
        }
        else // bound_index.size() == 2: two inner edges, one boundary edge
        {
            // Cut off the corner at the vertex shared by the two inner edges by connecting
            // their midpoints, then split the remaining quad with the diagonal from the
            // in-edge midpoint to the far endpoint of the other edge: three triangles.
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

            // Split the remaining quad (i_a, m_in, m_out, o_a) along the shorter of its
            // two diagonals to avoid creating thin triangles.
            const double d_m_in_o_a  = (its.vertices[size_t(m_in)] - its.vertices[size_t(o_a)]).squaredNorm();
            const double d_m_out_i_a = (its.vertices[size_t(m_out)] - its.vertices[size_t(i_a)]).squaredNorm();
            if (d_m_in_o_a <= d_m_out_i_a) {
                its.indices.emplace_back(i_a, m_in, o_a);
                its.indices.emplace_back(m_in, m_out, o_a);
            } else {
                its.indices.emplace_back(i_a, m_in, m_out);
                its.indices.emplace_back(i_a, m_out, o_a);
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

} // namespace Slic3r
