// Triangle mesh -> B-rep. The shared-topology construction (vertex cache, unordered-pair edge
// cache with Reversed() for the backward walk, the two-part degeneracy rule, the optional
// ShapeUpgrade_UnifySameDomain coplanar merge) follows Orca-Cad's GeometryEngine::mesh_to_brep
// (github.com/tommasobbianchi/Orca-Cad, AGPL-3.0), a native port of mesh2step. The component
// split, cavity handling, tolerance setup and the helpers below are EdgeSlicer's.

#include "MeshToBRep.hpp"

#include "libslic3r/Exception.hpp"

#include <BRepTools_ReShape.hxx>
#include <BRep_Builder.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Geom_Line.hxx>
#include <Geom_Plane.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Poly_Triangulation.hxx>
#include <ShapeFix_ShapeTolerance.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <cfloat>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace Slic3r { namespace BRep {

namespace {

struct CellHash
{
    size_t operator()(const std::array<long long, 3> &c) const
    {
        size_t h = std::hash<long long>()(c[0]);
        h ^= std::hash<long long>()(c[1]) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<long long>()(c[2]) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

inline uint64_t edge_key(int i, int j)
{
    const uint32_t a = uint32_t(std::min(i, j)), b = uint32_t(std::max(i, j));
    return (uint64_t(a) << 32) | uint64_t(b);
}

struct EdgeUse
{
    int count   = 0;
    int forward = 0; // walked from the lower to the higher vertex index
    int first_triangle = -1;
};

struct UnionFind
{
    std::vector<int> parent;
    explicit UnionFind(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
    int find(int x)
    {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x         = parent[x];
        }
        return x;
    }
    void unite(int a, int b)
    {
        a = find(a);
        b = find(b);
        if (a != b)
            parent[std::max(a, b)] = std::min(a, b);
    }
};

double solid_volume(const TopoDS_Shape &s)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(s, props);
    return props.Mass();
}

int count_faces(const TopoDS_Shape &s)
{
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(s, TopAbs_FACE, faces);
    return faces.Extent();
}

Bnd_Box tight_box(const TopoDS_Shape &s)
{
    Bnd_Box box;
    BRepBndLib::Add(s, box, false);
    return box;
}

bool box_contains(const Bnd_Box &outer, const Bnd_Box &inner)
{
    if (outer.IsVoid() || inner.IsVoid())
        return false;
    double ox0, oy0, oz0, ox1, oy1, oz1, ix0, iy0, iz0, ix1, iy1, iz1;
    outer.Get(ox0, oy0, oz0, ox1, oy1, oz1);
    inner.Get(ix0, iy0, iz0, ix1, iy1, iz1);
    return ix0 >= ox0 && iy0 >= oy0 && iz0 >= oz0 && ix1 <= ox1 && iy1 <= oy1 && iz1 <= oz1;
}

bool is_line(const TopoDS_Edge &edge)
{
    double                   first, last;
    const Handle(Geom_Curve) curve = BRep_Tool::Curve(edge, first, last);
    return !curve.IsNull() && (curve->IsKind(STANDARD_TYPE(Geom_Line)) ||
                               (curve->IsKind(STANDARD_TYPE(Geom_TrimmedCurve)) &&
                                Handle(Geom_TrimmedCurve)::DownCast(curve)->BasisCurve()->IsKind(STANDARD_TYPE(Geom_Line))));
}

// Join chains of straight edges that meet at a vertex nothing else uses, when they are collinear
// and bound the same faces: after the coplanar merge, the side of a merged face that crossed
// several triangles is still made of several edges. Linear in the number of edges.
TopoDS_Shape merge_collinear_edges(const TopoDS_Shape &shape, double tol, double angle)
{
    TopTools_IndexedDataMapOfShapeListOfShape vertex_edges, edge_faces;
    TopExp::MapShapesAndUniqueAncestors(shape, TopAbs_VERTEX, TopAbs_EDGE, vertex_edges);
    TopExp::MapShapesAndUniqueAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edge_faces);
    const double sin_tol = std::sin(std::max(angle, 1e-9));

    auto point  = [](const TopoDS_Shape &v) { return BRep_Tool::Pnt(TopoDS::Vertex(v)); };
    auto first  = [](const TopoDS_Shape &e) { return TopExp::FirstVertex(TopoDS::Edge(e.Oriented(TopAbs_FORWARD))); };
    auto last   = [](const TopoDS_Shape &e) { return TopExp::LastVertex(TopoDS::Edge(e.Oriented(TopAbs_FORWARD))); };
    auto other  = [&](const TopoDS_Shape &e, const TopoDS_Shape &v) -> TopoDS_Vertex {
        const TopoDS_Vertex a = first(e);
        return a.IsSame(v) ? last(e) : a;
    };
    auto same_faces = [&](const TopoDS_Shape &e1, const TopoDS_Shape &e2) {
        const TopTools_ListOfShape &f1 = edge_faces.FindFromKey(e1), &f2 = edge_faces.FindFromKey(e2);
        if (f1.Extent() != f2.Extent())
            return false;
        for (TopTools_ListIteratorOfListOfShape i(f1); i.More(); i.Next()) {
            bool found = false;
            for (TopTools_ListIteratorOfListOfShape j(f2); j.More() && !found; j.Next())
                found = i.Value().IsSame(j.Value());
            if (!found)
                return false;
        }
        return true;
    };

    // A vertex can go when exactly two straight edges meet there, bounding the same faces, in line.
    std::vector<char> removable(vertex_edges.Extent() + 1, 0);
    for (int i = 1; i <= vertex_edges.Extent(); ++i) {
        const TopTools_ListOfShape &edges = vertex_edges(i);
        if (edges.Extent() != 2)
            continue;
        const TopoDS_Shape &e1 = edges.First(), &e2 = edges.Last();
        if (e1.IsSame(e2) || !is_line(TopoDS::Edge(e1)) || !is_line(TopoDS::Edge(e2)) || !same_faces(e1, e2))
            continue;
        const TopoDS_Shape &v = vertex_edges.FindKey(i);
        const gp_Pnt        p = point(v), a = point(other(e1, v)), b = point(other(e2, v));
        const gp_Vec        d1(a, p), d2(p, b);
        if (d1.Magnitude() < tol || d2.Magnitude() < tol)
            continue;
        if (d1.Dot(d2) > 0. && d1.Crossed(d2).Magnitude() <= sin_tol * d1.Magnitude() * d2.Magnitude())
            removable[i] = 1;
    }

    BRepTools_ReShape          reshape;
    TopTools_IndexedMapOfShape visited;
    int                        merged_chains = 0;
    for (int ie = 1; ie <= edge_faces.Extent(); ++ie) {
        const TopoDS_Shape &start = edge_faces.FindKey(ie);
        if (visited.Contains(start))
            continue;
        visited.Add(start);
        auto is_removable = [&](const TopoDS_Shape &v) { const int idx = vertex_edges.FindIndex(v); return idx > 0 && removable[idx]; };
        // Walk from `start` through removable vertices in both directions.
        std::deque<TopoDS_Shape>  edges{start};
        std::deque<TopoDS_Vertex> verts{first(start), last(start)};
        bool                      loop = false;
        for (int dir = 0; dir < 2 && !loop; ++dir) {
            for (;;) {
                const TopoDS_Vertex end = dir == 0 ? verts.back() : verts.front();
                if (!is_removable(end))
                    break;
                const TopoDS_Shape &cur  = dir == 0 ? edges.back() : edges.front();
                const TopTools_ListOfShape &at = vertex_edges.FindFromKey(end);
                const TopoDS_Shape &next = at.First().IsSame(cur) ? at.Last() : at.First();
                if (next.IsSame(start)) {
                    loop = true;
                    break;
                }
                visited.Add(next);
                if (dir == 0) {
                    edges.push_back(next);
                    verts.push_back(other(next, end));
                } else {
                    edges.push_front(next);
                    verts.push_front(other(next, end));
                }
            }
        }
        if (loop || edges.size() < 2)
            continue;
        const TopoDS_Edge joined = BRepBuilderAPI_MakeEdge(verts.front(), verts.back()).Edge();
        const TopoDS_Shape head  = edges.front().Oriented(TopAbs_FORWARD);
        // The replacement must run the way the edge it replaces runs.
        reshape.Replace(head, first(head).IsSame(verts.front()) ? TopoDS_Shape(joined) : joined.Reversed());
        for (size_t k = 1; k < edges.size(); ++k)
            reshape.Remove(edges[k].Oriented(TopAbs_FORWARD));
        ++merged_chains;
    }
    return merged_chains > 0 ? reshape.Apply(shape) : shape;
}

} // namespace

TopoDS_Shape mesh_to_brep(const indexed_triangle_set &its, const MeshToBRepParams &params, MeshToBRepStats &stats)
{
    using clock = std::chrono::steady_clock;
    const auto t_start = clock::now();

    stats                 = MeshToBRepStats{};
    stats.input_triangles = int(its.indices.size());
    if (its.indices.empty() || its.vertices.empty())
        throw Slic3r::RuntimeError("mesh_to_brep: the mesh has no triangles");

    // 0. Weld distance: at least the float precision of the largest coordinate, or the merge
    //    below would find "coplanar" faces whose shared vertices are off the kept plane.
    float max_abs = 0.f;
    for (const stl_vertex &v : its.vertices)
        max_abs = std::max(max_abs, v.cwiseAbs().maxCoeff());
    const double tol = std::max(params.tolerance > 0. ? params.tolerance : 1e-4, 4. * double(FLT_EPSILON) * double(max_abs));
    stats.tolerance_used = tol;

    // 1. Tolerance-quantized vertex weld. A welded vertex keeps the exact coordinates of its
    //    first occurrence; vertices are grouped by cell, never snapped onto the grid.
    std::unordered_map<std::array<long long, 3>, int, CellHash> cell_to_new;
    cell_to_new.reserve(its.vertices.size());
    std::vector<int>   old_to_new(its.vertices.size(), -1);
    std::vector<Vec3d> verts;
    verts.reserve(its.vertices.size());
    for (size_t i = 0; i < its.vertices.size(); ++i) {
        const Vec3d                    p = its.vertices[i].cast<double>();
        const std::array<long long, 3> cell{(long long) std::llround(p.x() / tol), (long long) std::llround(p.y() / tol),
                                            (long long) std::llround(p.z() / tol)};
        auto ins = cell_to_new.emplace(cell, int(verts.size()));
        if (ins.second)
            verts.push_back(p);
        old_to_new[i] = ins.first->second;
    }

    // 2. Degenerate triangles. Noise is an absolute floor (longest edge below the weld
    //    distance), slivers are scale-independent (area below 1e-9 * longest^2): folding the
    //    two into one "area < tol^2" rule drops legitimate thin CAD slivers and opens the shell.
    std::vector<std::array<int, 3>> tris;
    tris.reserve(its.indices.size());
    for (const stl_triangle_vertex_indices &t : its.indices) {
        if (t(0) < 0 || t(1) < 0 || t(2) < 0 || size_t(t(0)) >= old_to_new.size() || size_t(t(1)) >= old_to_new.size() ||
            size_t(t(2)) >= old_to_new.size()) {
            ++stats.degenerate_collapsed;
            continue;
        }
        const int a = old_to_new[t(0)], b = old_to_new[t(1)], c = old_to_new[t(2)];
        if (a == b || b == c || a == c) {
            ++stats.degenerate_collapsed;
            continue;
        }
        const Vec3d &pa = verts[a], &pb = verts[b], &pc = verts[c];
        const double longest = std::max({(pb - pa).norm(), (pc - pb).norm(), (pa - pc).norm()});
        if (longest < tol) {
            ++stats.degenerate_collapsed;
            continue;
        }
        const double area = 0.5 * (pb - pa).cross(pc - pa).norm();
        if (area < 1e-9 * longest * longest) {
            ++stats.degenerate_sliver;
            continue;
        }
        tris.push_back({a, b, c});
    }
    stats.kept_triangles = int(tris.size());
    if (tris.empty())
        throw Slic3r::RuntimeError("mesh_to_brep: every triangle was degenerate");

    // 3. Edge usage and connected components (triangles sharing an edge).
    std::unordered_map<uint64_t, EdgeUse> edges;
    edges.reserve(tris.size() * 2);
    UnionFind uf(tris.size());
    for (size_t ti = 0; ti < tris.size(); ++ti) {
        const auto &t = tris[ti];
        for (int k = 0; k < 3; ++k) {
            const int i = t[k], j = t[(k + 1) % 3];
            EdgeUse  &e = edges[edge_key(i, j)];
            ++e.count;
            if (i < j)
                ++e.forward;
            if (e.first_triangle < 0)
                e.first_triangle = int(ti);
            else
                uf.unite(e.first_triangle, int(ti));
        }
    }
    stats.unique_edges = int(edges.size());
    std::vector<int> component_of(tris.size());
    std::unordered_map<int, int> root_to_component;
    for (size_t ti = 0; ti < tris.size(); ++ti) {
        const int root = uf.find(int(ti));
        auto      ins  = root_to_component.emplace(root, int(root_to_component.size()));
        component_of[ti] = ins.first->second;
    }
    const int ncomp  = int(root_to_component.size());
    stats.components = ncomp;
    std::vector<char> comp_closed(ncomp, 1);
    for (const auto &kv : edges) {
        const EdgeUse &e    = kv.second;
        const int      comp = component_of[e.first_triangle];
        if (e.count == 1) {
            ++stats.boundary_edges;
            comp_closed[comp] = 0;
        } else if (e.count >= 3) {
            ++stats.nonmanifold_edges;
            comp_closed[comp] = 0;
        } else if (e.forward != 1) {
            ++stats.flipped_edges;
            comp_closed[comp] = 0;
        }
    }
    stats.watertight = stats.boundary_edges == 0 && stats.nonmanifold_edges == 0 && stats.flipped_edges == 0;

    // 4. One planar face per triangle; vertices and edges are shared through the caches, so the
    //    shells are sewn by construction.
    BRep_Builder               builder;
    std::vector<TopoDS_Vertex> vertex_cache(verts.size());
    auto get_vertex = [&](int i) -> const TopoDS_Vertex & {
        if (vertex_cache[i].IsNull()) {
            const Vec3d &p = verts[i];
            builder.MakeVertex(vertex_cache[i], gp_Pnt(p.x(), p.y(), p.z()), tol);
        }
        return vertex_cache[i];
    };
    std::unordered_map<uint64_t, TopoDS_Edge> edge_cache;
    edge_cache.reserve(edges.size());
    auto get_edge = [&](int i, int j) -> TopoDS_Edge {
        const uint64_t key = edge_key(i, j);
        auto           it  = edge_cache.find(key);
        if (it == edge_cache.end()) {
            const int lo = std::min(i, j), hi = std::max(i, j);
            it = edge_cache.emplace(key, BRepBuilderAPI_MakeEdge(get_vertex(lo), get_vertex(hi)).Edge()).first;
        }
        return i < j ? it->second : TopoDS::Edge(it->second.Reversed());
    };

    std::vector<TopoDS_Shell> shells(ncomp);
    for (TopoDS_Shell &s : shells)
        builder.MakeShell(s);
    for (size_t ti = 0; ti < tris.size(); ++ti) {
        const auto &t = tris[ti];
        try {
            const Vec3d &pa = verts[t[0]], &pb = verts[t[1]], &pc = verts[t[2]];
            const Vec3d  n  = (pb - pa).cross(pc - pa).normalized();
            TopoDS_Wire  wire;
            builder.MakeWire(wire);
            builder.Add(wire, get_edge(t[0], t[1]));
            builder.Add(wire, get_edge(t[1], t[2]));
            builder.Add(wire, get_edge(t[2], t[0]));
            wire.Closed(Standard_True);
            TopoDS_Face face;
            builder.MakeFace(face, new Geom_Plane(gp_Pln(gp_Pnt(pa.x(), pa.y(), pa.z()), gp_Dir(n.x(), n.y(), n.z()))), tol);
            builder.Add(face, wire);
            builder.Add(shells[component_of[ti]], face);
        } catch (const Standard_Failure &) {
            ++stats.faces_failed;
            comp_closed[component_of[ti]] = 0;
        }
    }

    // 5. Closed components become solids. An inward-facing closed component is a cavity when
    //    a solid contains it, otherwise an inverted solid that gets flipped.
    struct Piece { TopoDS_Solid solid; double volume; Bnd_Box box; };
    std::vector<Piece>        solids;
    std::vector<TopoDS_Shell> inward;
    std::vector<TopoDS_Shape> open_shells;
    for (int c = 0; c < ncomp; ++c) {
        TopoDS_Shell &shell = shells[c];
        if (!comp_closed[c]) {
            open_shells.push_back(shell);
            continue;
        }
        shell.Closed(Standard_True);
        TopoDS_Solid solid;
        builder.MakeSolid(solid);
        builder.Add(solid, shell);
        const double vol = solid_volume(solid);
        if (vol > 0.)
            solids.push_back({solid, vol, tight_box(solid)});
        else if (vol < 0.)
            inward.push_back(shell);
        else
            open_shells.push_back(shell);
    }
    for (const TopoDS_Shell &cav : inward) {
        const Bnd_Box cav_box = tight_box(cav);
        const gp_Pnt  probe   = BRep_Tool::Pnt(TopoDS::Vertex(TopExp_Explorer(cav, TopAbs_VERTEX).Current()));
        int           best    = -1;
        for (int i = 0; i < int(solids.size()); ++i) {
            if (!box_contains(solids[i].box, cav_box) || (best >= 0 && solids[i].volume >= solids[best].volume))
                continue;
            BRepClass3d_SolidClassifier cls(solids[i].solid, probe, tol);
            if (cls.State() == TopAbs_IN)
                best = i;
        }
        if (best >= 0) {
            builder.Add(solids[best].solid, cav);
            solids[best].volume = solid_volume(solids[best].solid);
            ++stats.cavities;
        } else {
            TopoDS_Solid solid;
            builder.MakeSolid(solid);
            builder.Add(solid, cav.Reversed());
            solids.push_back({solid, solid_volume(solid), cav_box});
            ++stats.inverted_solids;
        }
    }

    stats.solids      = int(solids.size());
    stats.open_shells = int(open_shells.size());
    for (const Piece &p : solids)
        stats.volume += p.volume;
    stats.is_solid = !solids.empty() && open_shells.empty();

    TopoDS_Shape shape;
    if (solids.size() == 1 && open_shells.empty())
        shape = solids.front().solid;
    else if (solids.empty() && open_shells.size() == 1)
        shape = open_shells.front();
    else {
        TopoDS_Compound comp;
        builder.MakeCompound(comp);
        for (const Piece &p : solids)
            builder.Add(comp, p.solid);
        for (const TopoDS_Shape &s : open_shells)
            builder.Add(comp, s);
        shape = comp;
    }
    // Every sub-shape carries the weld distance, so a merged planar face stays valid although
    // its vertices sit on the float grid rather than exactly on the kept plane.
    ShapeFix_ShapeTolerance().SetTolerance(shape, tol);
    stats.faces_before_merge = count_faces(shape);
    stats.seconds_build      = std::chrono::duration<double>(clock::now() - t_start).count();

    // 6. Coplanar merge: one face per triangle is exact, but a CAD user (and a fillet) wants
    //    the cube's six faces, not its twelve triangles.
    if (params.merge_angle_deg > 0.) {
        const auto t_merge = clock::now();
        try {
            // Faces only: UnifySameDomain's edge unification is far worse than linear (134 s for
            // the 52k-triangle Stanford bunny against 1.3 s for the faces), so the collinear
            // edges left on the merged faces' boundaries are joined by merge_collinear_edges().
            ShapeUpgrade_UnifySameDomain unifier(shape, Standard_False, Standard_True, Standard_False);
            unifier.SetLinearTolerance(tol);
            unifier.SetAngularTolerance(params.merge_angle_deg * M_PI / 180.);
            unifier.AllowInternalEdges(Standard_False);
            unifier.Build();
            TopoDS_Shape merged = unifier.Shape();
            if (!merged.IsNull()) {
                merged = merge_collinear_edges(merged, tol, params.merge_angle_deg * M_PI / 180.);
                ShapeFix_ShapeTolerance().SetTolerance(merged, tol);
                shape = merged;
            }
        } catch (const Standard_Failure &) {
            // The merge is cosmetic: keep the exact faceted shape.
            stats.warnings.emplace_back("coplanar faces could not be merged; the faceted shape is kept");
        }
        stats.seconds_merge = std::chrono::duration<double>(clock::now() - t_merge).count();
    }
    stats.faces_final = count_faces(shape);

    if (stats.boundary_edges > 0)
        stats.warnings.emplace_back(std::to_string(stats.boundary_edges) + " open edges: exported as a surface (shell), not a solid");
    if (stats.nonmanifold_edges > 0)
        stats.warnings.emplace_back(std::to_string(stats.nonmanifold_edges) + " non-manifold edges: that part is exported as a surface");
    if (stats.flipped_edges > 0)
        stats.warnings.emplace_back(std::to_string(stats.flipped_edges) + " edges with inconsistent winding: that part is exported as a surface");
    if (stats.inverted_solids > 0)
        stats.warnings.emplace_back(std::to_string(stats.inverted_solids) + " inside-out parts were flipped");
    if (stats.faces_failed > 0)
        stats.warnings.emplace_back(std::to_string(stats.faces_failed) + " triangles could not be converted");
    return shape;
}

ShapeInfo shape_info(const TopoDS_Shape &shape, bool check_validity)
{
    ShapeInfo info;
    if (shape.IsNull())
        return info;
    TopTools_IndexedMapOfShape solids, shells, faces, edges;
    TopExp::MapShapes(shape, TopAbs_SOLID, solids);
    TopExp::MapShapes(shape, TopAbs_SHELL, shells);
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    info.solids = solids.Extent();
    info.shells = shells.Extent();
    info.faces  = faces.Extent();
    info.edges  = edges.Extent();
    for (TopExp_Explorer ex(shape, TopAbs_SHELL, TopAbs_SOLID); ex.More(); ex.Next())
        ++info.free_shells;
    for (int i = 1; i <= faces.Extent(); ++i)
        if (BRepAdaptor_Surface(TopoDS::Face(faces(i))).GetType() == GeomAbs_Plane)
            ++info.planar_faces;
    for (int i = 1; i <= solids.Extent(); ++i)
        info.volume += solid_volume(solids(i));
    GProp_GProps sprops;
    BRepGProp::SurfaceProperties(shape, sprops);
    info.area = sprops.Mass();
    Bnd_Box box;
    BRepBndLib::AddOptimal(shape, box, Standard_False, Standard_False);
    if (!box.IsVoid()) {
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        info.bbox = BoundingBoxf3(Vec3d(x0, y0, z0), Vec3d(x1, y1, z1));
    }
    if (check_validity)
        info.valid = BRepCheck_Analyzer(shape).IsValid() == Standard_True;
    return info;
}

indexed_triangle_set brep_to_its(const TopoDS_Shape &shape, double linear_deflection, double angular_deflection)
{
    indexed_triangle_set its;
    if (shape.IsNull())
        return its;
    BRepMesh_IncrementalMesh mesher(shape, linear_deflection, Standard_False, angular_deflection, Standard_True);
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face          &face = TopoDS::Face(ex.Current());
        TopLoc_Location             loc;
        Handle(Poly_Triangulation)  tri  = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull())
            continue;
        const gp_Trsf trsf   = loc.Transformation();
        const int     offset = int(its.vertices.size());
        for (int i = 1; i <= tri->NbNodes(); ++i) {
            gp_Pnt p = tri->Node(i);
            p.Transform(trsf);
            its.vertices.emplace_back(float(p.X()), float(p.Y()), float(p.Z()));
        }
        const bool reversed = face.Orientation() == TopAbs_REVERSED;
        for (int i = 1; i <= tri->NbTriangles(); ++i) {
            int a, b, c;
            tri->Triangle(i).Get(a, b, c);
            if (reversed)
                std::swap(b, c);
            its.indices.emplace_back(offset + a - 1, offset + b - 1, offset + c - 1);
        }
    }
    return its;
}

}} // namespace Slic3r::BRep
