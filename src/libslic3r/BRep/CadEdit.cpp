// Exact B-rep fillet / chamfer / shell on a part's CAD body. See CadEdit.hpp for the design and
// the credit to Orca-Cad (github.com/tommasobbianchi/Orca-Cad, AGPL-3.0), whose
// GeometryEngine::apply_fillet / apply_chamfer and Shell feature these operations follow.

#include "CadEdit.hpp"
#include "CadShape.hpp"
#include "MeshToBRep.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/Format/STEPExport.hpp"
#include "libslic3r/Model.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRep_Tool.hxx>
#include <BinTools.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom_Surface.hxx>
#include <Poly_Triangulation.hxx>
#include <ShapeAnalysis_Surface.hxx>
#include <ShapeFix_Shape.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace Slic3r { namespace BRep {

// ---- CadShape.hpp -----------------------------------------------------------------------------

namespace {

std::string write_shape(const TopoDS_Shape &shape)
{
    std::ostringstream out(std::ios::out | std::ios::binary);
    // No triangulation: it is rebuilt at the precision each use needs, and it would double the size.
    BinTools::Write(shape, out, Standard_False, Standard_False, BinTools_FormatVersion_CURRENT);
    return out.str();
}

std::string occt_message(const Standard_Failure &e)
{
    const char *msg = e.GetMessageString();
    std::string out = e.DynamicType()->Name();
    if (msg != nullptr && *msg != 0)
        out += std::string(": ") + msg;
    return out;
}

double seconds_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

TopoDS_Shape cad_body_shape(const CadBody &body)
{
    TopoDS_Shape shape;
    try {
        std::istringstream in(body.brep, std::ios::in | std::ios::binary);
        BinTools::Read(shape, in);
    } catch (const Standard_Failure &e) {
        throw Slic3r::RuntimeError("the CAD body cannot be read (" + occt_message(e) + ")");
    }
    if (shape.IsNull())
        throw Slic3r::RuntimeError("the CAD body is empty");
    if (!body.shift.isZero()) {
        gp_Trsf t;
        t.SetTranslation(gp_Vec(body.shift.x(), body.shift.y(), body.shift.z()));
        shape = BRepBuilderAPI_Transform(shape, t, Standard_True).Shape();
    }
    return shape;
}

std::shared_ptr<CadBody> make_cad_body(const TopoDS_Shape &shape, const indexed_triangle_set &mesh, CadBodyOrigin origin, int operations)
{
    auto body        = std::make_shared<CadBody>();
    body->brep       = write_shape(shape);
    body->mesh       = mesh_fingerprint(mesh);
    body->origin     = origin;
    body->operations = operations;
    return body;
}

indexed_triangle_set tessellate_cad_shape(const TopoDS_Shape &shape, double linear_deflection, double angular_deflection)
{
    // Triangulated as load_step() does (BRepMesh_IncrementalMesh at the same deflections), and
    // welded the way load_step() welds - through admesh's import repair (TriangleMesh::from_stl),
    // which joins the exactly shared edge nodes and also the nearly coincident ones BRepMesh
    // leaves around a blend's corner patches.
    const indexed_triangle_set its = brep_to_its(shape, linear_deflection, angular_deflection);
    if (its.indices.empty())
        return its;
    stl_file stl;
    stl.stats.type                = inmemory;
    stl.stats.number_of_facets    = uint32_t(its.indices.size());
    stl.stats.original_num_facets = stl.stats.number_of_facets;
    stl_allocate(&stl);
    for (size_t i = 0; i < its.indices.size(); ++i) {
        stl_facet &facet = stl.facet_start[i];
        for (int k = 0; k < 3; ++k)
            facet.vertex[k] = its.vertices[size_t(its.indices[i](k))];
        facet.extra[0] = facet.extra[1] = 0;
        stl_normal normal;
        stl_calculate_normal(normal, &facet);
        stl_normalize_vector(normal);
        facet.normal = normal;
    }
    TriangleMesh mesh;
    mesh.from_stl(stl);
    return std::move(mesh.its);
}

// ---- sourcing ---------------------------------------------------------------------------------

std::shared_ptr<const CadBody> attached_cad_body(const ModelVolume &volume)
{
    const std::shared_ptr<const CadBody> &body = volume.cad_body;
    if (!body || body->brep.empty())
        return nullptr;
    const MeshFingerprint now = mesh_fingerprint(volume.mesh().its);
    Vec3d                 shift;
    if (!body->mesh.matches(now, shift))
        return nullptr;
    if (shift.isZero())
        return body;
    return body->translated(shift, now);
}

StepSourceRef step_source_ref(const ModelVolume &volume)
{
    return {volume.source.input_file, volume.name, volume.source.mesh_offset};
}

bool is_step_file(const std::string &path)
{
    return boost::iends_with(path, ".step") || boost::iends_with(path, ".stp");
}

std::shared_ptr<const CadBody> cad_body_from_step_source(const ModelVolume &volume, std::string *why_not)
{
    return cad_body_from_step_source(step_source_ref(volume), volume.mesh().its, why_not);
}

std::shared_ptr<const CadBody> cad_body_from_step_source(const StepSourceRef &source, const indexed_triangle_set &mesh, std::string *why_not)
{
    if (source.input_file.empty() || !is_step_file(source.input_file)) {
        if (why_not)
            *why_not = "the part was not imported from STEP";
        return nullptr;
    }
    const TopoDS_Shape shape = step_source_brep(source.input_file, source.volume_name, mesh, source.mesh_offset, why_not);
    if (shape.IsNull())
        return nullptr;
    try {
        return make_cad_body(shape, mesh, CadBodyOrigin::StepFile, 0);
    } catch (const Standard_Failure &e) {
        if (why_not)
            *why_not = occt_message(e);
    }
    return nullptr;
}

std::shared_ptr<const CadBody> cad_body_from_mesh(const indexed_triangle_set &its, MeshConversionReport &report)
{
    report           = MeshConversionReport{};
    report.triangles = int(its.indices.size());
    const auto t0    = std::chrono::steady_clock::now();
    if (its.indices.empty()) {
        report.error = "the part has no mesh";
        return nullptr;
    }
    if (report.triangles > ConvertMaxTriangles) {
        report.error = "the part has " + std::to_string(report.triangles) + " triangles; at most " +
                       std::to_string(ConvertMaxTriangles) + " can be converted. Simplify it first.";
        return nullptr;
    }
    try {
        MeshToBRepStats    stats;
        const TopoDS_Shape shape = mesh_to_brep(its, {}, stats);
        if (!stats.is_solid || stats.solids == 0) {
            report.error = "the mesh is not closed (" + std::to_string(stats.boundary_edges) + " open edges, " +
                           std::to_string(stats.nonmanifold_edges) + " non-manifold edges). Repair the part first.";
            return nullptr;
        }
        TopTools_IndexedMapOfShape faces, edges;
        TopExp::MapShapes(shape, TopAbs_FACE, faces);
        TopExp::MapShapes(shape, TopAbs_EDGE, edges);
        report.faces   = faces.Extent();
        report.edges   = edges.Extent();
        auto body      = make_cad_body(shape, its, CadBodyOrigin::ConvertedMesh, 0);
        report.seconds = seconds_since(t0);
        return body;
    } catch (const Standard_Failure &e) {
        report.error = "the conversion failed (" + occt_message(e) + ")";
    } catch (const std::exception &e) {
        report.error = e.what();
    }
    report.seconds = seconds_since(t0);
    return nullptr;
}

// ---- topology ---------------------------------------------------------------------------------

namespace {

// Outward normal of `face` at the middle of `edge`, which bounds it.
bool normal_at_edge(const TopoDS_Face &face, const TopoDS_Edge &edge, gp_Dir &normal)
{
    BRepAdaptor_Surface surface(face, Standard_False);
    gp_Pnt2d            uv;
    bool                have_uv = false;
    Standard_Real       f = 0., l = 0.;
    const Handle(Geom2d_Curve) pcurve = BRep_Tool::CurveOnSurface(edge, face, f, l);
    if (!pcurve.IsNull()) {
        uv      = pcurve->Value(0.5 * (f + l));
        have_uv = true;
    } else {
        BRepAdaptor_Curve curve(edge);
        const gp_Pnt      p = curve.Value(0.5 * (curve.FirstParameter() + curve.LastParameter()));
        TopLoc_Location   loc;
        const Handle(Geom_Surface) s = BRep_Tool::Surface(face, loc);
        if (!s.IsNull()) {
            ShapeAnalysis_Surface sas(s);
            uv      = sas.ValueOfUV(p.Transformed(loc.Transformation().Inverted()), 1e-6);
            have_uv = true;
        }
    }
    if (!have_uv)
        return false;
    gp_Pnt p;
    gp_Vec du, dv;
    surface.D1(uv.X(), uv.Y(), p, du, dv);
    const gp_Vec n = du.Crossed(dv);
    if (n.Magnitude() < 1e-12)
        return false;
    normal = gp_Dir(n);
    if (face.Orientation() == TopAbs_REVERSED)
        normal.Reverse();
    return true;
}

// Unit tangent of `edge` at its end `vertex`, pointing into the edge.
bool tangent_away_from(const TopoDS_Edge &edge, const TopoDS_Vertex &vertex, gp_Vec &tangent)
{
    if (BRep_Tool::Degenerated(edge))
        return false;
    TopoDS_Vertex v1, v2;
    TopExp::Vertices(edge, v1, v2);
    if (v1.IsSame(v2))
        return false; // a closed edge (full circle) has no single "end" here
    BRepAdaptor_Curve   curve(edge);
    const Standard_Real t = BRep_Tool::Parameter(vertex, edge);
    gp_Pnt              p;
    gp_Vec              d;
    curve.D1(t, p, d);
    if (d.Magnitude() < 1e-12)
        return false;
    if (std::abs(t - curve.LastParameter()) < std::abs(t - curve.FirstParameter()))
        d.Reverse();
    tangent = d.Normalized();
    return true;
}

// Faces meeting at an edge closer to tangent than this are a smooth join: not filletable.
constexpr double SmoothJoinDeg = 0.5;
// Two edges continue each other when they leave a vertex in opposite directions within this.
constexpr double TangentChainDeg = 2.;

} // namespace

CadTopology cad_topology(const CadBody &body, double linear_deflection, double angular_deflection)
{
    CadTopology  topo;
    TopoDS_Shape shape = cad_body_shape(body);
    try {
        TopTools_IndexedMapOfShape faces, edges;
        TopExp::MapShapes(shape, TopAbs_FACE, faces);
        TopExp::MapShapes(shape, TopAbs_EDGE, edges);
        topo.num_faces = faces.Extent();
        topo.num_edges = edges.Extent();

        // Hit-testing mesh, one face at a time so every triangle knows its face.
        BRepMesh_IncrementalMesh mesher(shape, linear_deflection, Standard_False, angular_deflection, Standard_True);
        topo.face_planar.assign(size_t(topo.num_faces), 0);
        topo.face_edges.assign(size_t(topo.num_faces), {});
        for (int fi = 1; fi <= topo.num_faces; ++fi) {
            const TopoDS_Face &face = TopoDS::Face(faces(fi));
            topo.face_planar[size_t(fi - 1)] = BRepAdaptor_Surface(face, Standard_False).GetType() == GeomAbs_Plane;
            TopLoc_Location            loc;
            Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
            if (tri.IsNull())
                continue;
            const gp_Trsf trsf   = loc.Transformation();
            const int     offset = int(topo.mesh.vertices.size());
            for (int i = 1; i <= tri->NbNodes(); ++i) {
                const gp_Pnt p = tri->Node(i).Transformed(trsf);
                topo.mesh.vertices.emplace_back(float(p.X()), float(p.Y()), float(p.Z()));
                topo.bbox.merge(Vec3d(p.X(), p.Y(), p.Z()));
            }
            const bool reversed = face.Orientation() == TopAbs_REVERSED;
            for (int i = 1; i <= tri->NbTriangles(); ++i) {
                int a, b, c;
                tri->Triangle(i).Get(a, b, c);
                if (reversed)
                    std::swap(b, c);
                topo.mesh.indices.emplace_back(offset + a - 1, offset + b - 1, offset + c - 1);
                topo.triangle_face.push_back(fi - 1);
            }
        }

        TopTools_IndexedDataMapOfShapeListOfShape edge_faces;
        TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edge_faces);

        topo.edge_polylines.assign(size_t(topo.num_edges), {});
        topo.edge_faces.assign(size_t(topo.num_edges), {-1, -1});
        topo.edge_selectable.assign(size_t(topo.num_edges), 0);
        topo.edge_tangent_neighbours.assign(size_t(topo.num_edges), {});
        const double smooth_cos = std::cos(SmoothJoinDeg * PI / 180.);

        for (int ei = 1; ei <= topo.num_edges; ++ei) {
            const TopoDS_Edge &edge = TopoDS::Edge(edges(ei));
            const size_t       e    = size_t(ei - 1);
            if (BRep_Tool::Degenerated(edge))
                continue;

            // Polyline for drawing and screen-space picking.
            try {
                BRepAdaptor_Curve           curve(edge);
                GCPnts_TangentialDeflection sampler(curve, angular_deflection, linear_deflection);
                for (int i = 1; i <= sampler.NbPoints(); ++i) {
                    const gp_Pnt p = sampler.Value(i);
                    topo.edge_polylines[e].emplace_back(float(p.X()), float(p.Y()), float(p.Z()));
                }
            } catch (const Standard_Failure &) {
                topo.edge_polylines[e].clear();
            }
            if (topo.edge_polylines[e].size() < 2) {
                TopoDS_Vertex v1, v2;
                TopExp::Vertices(edge, v1, v2);
                topo.edge_polylines[e].clear();
                if (!v1.IsNull() && !v2.IsNull()) {
                    const gp_Pnt a = BRep_Tool::Pnt(v1), b = BRep_Tool::Pnt(v2);
                    topo.edge_polylines[e] = {Vec3f(float(a.X()), float(a.Y()), float(a.Z())), Vec3f(float(b.X()), float(b.Y()), float(b.Z()))};
                }
            }

            // The distinct faces it bounds (a seam lists its one face twice).
            std::vector<TopoDS_Face> adjacent;
            if (edge_faces.Contains(edge))
                for (TopTools_ListOfShape::Iterator it(edge_faces.FindFromKey(edge)); it.More(); it.Next()) {
                    const TopoDS_Face &f = TopoDS::Face(it.Value());
                    if (std::none_of(adjacent.begin(), adjacent.end(), [&f](const TopoDS_Face &g) { return g.IsSame(f); }))
                        adjacent.push_back(f);
                }
            for (size_t k = 0; k < adjacent.size() && k < 2; ++k) {
                const int fi = faces.FindIndex(adjacent[k]);
                topo.edge_faces[e][k] = fi - 1;
                if (fi > 0)
                    topo.face_edges[size_t(fi - 1)].push_back(int(e));
            }
            if (adjacent.size() != 2 || BRep_Tool::IsClosed(edge, adjacent[0]) || BRep_Tool::IsClosed(edge, adjacent[1]))
                continue;
            gp_Dir n0, n1;
            if (normal_at_edge(adjacent[0], edge, n0) && normal_at_edge(adjacent[1], edge, n1) && n0.Dot(n1) > smooth_cos)
                continue; // tangent join (e.g. the boundary of an existing fillet)
            topo.edge_selectable[e] = 1;
        }

        // Tangent continuations through shared vertices.
        TopTools_IndexedDataMapOfShapeListOfShape vertex_edges;
        TopExp::MapShapesAndAncestors(shape, TopAbs_VERTEX, TopAbs_EDGE, vertex_edges);
        const double chain_cos = std::cos(TangentChainDeg * PI / 180.);
        for (int vi = 1; vi <= vertex_edges.Extent(); ++vi) {
            const TopoDS_Vertex &vertex = TopoDS::Vertex(vertex_edges.FindKey(vi));
            std::vector<std::pair<int, gp_Vec>> at_vertex;
            for (TopTools_ListOfShape::Iterator it(vertex_edges.FindFromIndex(vi)); it.More(); it.Next()) {
                const TopoDS_Edge &edge = TopoDS::Edge(it.Value());
                const int          e    = edges.FindIndex(edge) - 1;
                if (e < 0 || !topo.edge_selectable[size_t(e)] ||
                    std::any_of(at_vertex.begin(), at_vertex.end(), [e](const auto &p) { return p.first == e; }))
                    continue;
                gp_Vec t;
                if (tangent_away_from(edge, vertex, t))
                    at_vertex.emplace_back(e, t);
            }
            for (size_t a = 0; a < at_vertex.size(); ++a)
                for (size_t b = a + 1; b < at_vertex.size(); ++b)
                    if (at_vertex[a].second.Dot(at_vertex[b].second) < -chain_cos) {
                        topo.edge_tangent_neighbours[size_t(at_vertex[a].first)].push_back(at_vertex[b].first);
                        topo.edge_tangent_neighbours[size_t(at_vertex[b].first)].push_back(at_vertex[a].first);
                    }
        }
    } catch (const Standard_Failure &e) {
        throw Slic3r::RuntimeError("the CAD body's topology cannot be read (" + occt_message(e) + ")");
    }
    return topo;
}

std::vector<int> tangent_chain(const CadTopology &topo, int edge)
{
    std::vector<int> out;
    if (edge < 0 || edge >= topo.num_edges || !topo.edge_selectable[size_t(edge)])
        return out;
    std::vector<uint8_t> seen(size_t(topo.num_edges), 0);
    std::vector<int>     stack{edge};
    seen[size_t(edge)] = 1;
    while (!stack.empty()) {
        const int e = stack.back();
        stack.pop_back();
        out.push_back(e);
        for (int n : topo.edge_tangent_neighbours[size_t(e)])
            if (!seen[size_t(n)]) {
                seen[size_t(n)] = 1;
                stack.push_back(n);
            }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<int> face_selectable_edges(const CadTopology &topo, int face)
{
    std::vector<int> out;
    if (face < 0 || face >= topo.num_faces)
        return out;
    for (int e : topo.face_edges[size_t(face)])
        if (topo.edge_selectable[size_t(e)])
            out.push_back(e);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ---- operations -------------------------------------------------------------------------------

namespace {

CadOpResult fail(CadOpStatus status, std::string why)
{
    CadOpResult r;
    r.status = status;
    r.error  = std::move(why);
    return r;
}

// Validate, tessellate and wrap an operation's result.
void finish(TopoDS_Shape result, const CadBody &input, const TessellationParams &tess, CadOpResult &r)
{
    if (result.IsNull()) {
        r.status = CadOpStatus::InvalidResult;
        r.error  = "the operation produced no shape";
        return;
    }
    if (!BRepCheck_Analyzer(result).IsValid()) {
        // A blend can come out with tolerances a little off; let ShapeFix try once.
        ShapeFix_Shape fix(result);
        fix.Perform();
        result = fix.Shape();
        if (result.IsNull() || !BRepCheck_Analyzer(result).IsValid()) {
            r.status = CadOpStatus::InvalidResult;
            r.error  = "the result is not a valid solid";
            return;
        }
    }
    const ShapeInfo info = shape_info(result, false);
    if (info.solids == 0 || info.free_shells > 0 || !(info.volume > 0.)) {
        r.status = CadOpStatus::InvalidResult;
        r.error  = "the result is not a closed solid";
        return;
    }
    r.faces_after  = info.faces;
    r.volume_after = info.volume;
    r.mesh         = tessellate_cad_shape(result, tess.linear_deflection, tess.angular_deflection);
    if (r.mesh.indices.empty()) {
        r.status = CadOpStatus::InvalidResult;
        r.error  = "the result could not be triangulated";
        return;
    }
    r.body   = make_cad_body(result, r.mesh, input.origin, input.operations + 1);
    r.status = CadOpStatus::Ok;
}

} // namespace

CadOpResult fillet_edges(const CadBody &body, EdgeFeature feature, double size, const std::vector<int> &edges_in, const TessellationParams &tess)
{
    const auto  t0   = std::chrono::steady_clock::now();
    const char *what = feature == EdgeFeature::Fillet ? "fillet" : "chamfer";
    if (edges_in.empty())
        return fail(CadOpStatus::NothingSelected, "no edge is selected");
    if (!(size > 0.) || !std::isfinite(size))
        return fail(CadOpStatus::BadSize, feature == EdgeFeature::Fillet ? "the radius must be greater than zero"
                                                                         : "the distance must be greater than zero");
    std::vector<int> edges = edges_in;
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

    CadOpResult r;
    try {
        const TopoDS_Shape shape  = cad_body_shape(body);
        const ShapeInfo    before = shape_info(shape, false);
        r.faces_before            = before.faces;
        r.volume_before           = before.volume;

        TopTools_IndexedMapOfShape emap;
        TopExp::MapShapes(shape, TopAbs_EDGE, emap);
        TopoDS_Shape result;
        if (feature == EdgeFeature::Fillet) {
            BRepFilletAPI_MakeFillet mk(shape);
            for (int e : edges) {
                if (e < 0 || e >= emap.Extent())
                    return fail(CadOpStatus::BadIndex, "an edge index is out of range");
                mk.Add(size, TopoDS::Edge(emap(e + 1)));
            }
            mk.Build();
            if (!mk.IsDone()) {
                r.status = CadOpStatus::Failed;
                r.error  = "the fillet could not be built. The radius is probably too large for the faces next to the "
                           "selected edges, or an edge ends where a fillet cannot be blended. Try a smaller radius or fewer edges.";
                BOOST_LOG_TRIVIAL(info) << "CAD fillet failed: " << mk.NbFaultyContours() << " faulty contours, "
                                        << mk.NbFaultyVertices() << " faulty vertices, radius " << size;
                r.seconds = seconds_since(t0);
                return r;
            }
            result = mk.Shape();
        } else {
            BRepFilletAPI_MakeChamfer mk(shape);
            for (int e : edges) {
                if (e < 0 || e >= emap.Extent())
                    return fail(CadOpStatus::BadIndex, "an edge index is out of range");
                mk.Add(size, TopoDS::Edge(emap(e + 1)));
            }
            mk.Build();
            if (!mk.IsDone()) {
                r.status = CadOpStatus::Failed;
                r.error  = "the chamfer could not be built. The distance is probably too large for the faces next to the "
                           "selected edges. Try a smaller distance or fewer edges.";
                r.seconds = seconds_since(t0);
                return r;
            }
            result = mk.Shape();
        }
        finish(result, body, tess, r);
    } catch (const Standard_Failure &e) {
        r.status = CadOpStatus::Failed;
        r.error  = std::string("the ") + what + " could not be built (" + occt_message(e) + "). Try a smaller size or fewer edges.";
    } catch (const std::exception &e) {
        r.status = CadOpStatus::Failed;
        r.error  = e.what();
    }
    r.seconds = seconds_since(t0);
    BOOST_LOG_TRIVIAL(info) << "CAD " << what << " of " << edges.size() << " edges, size " << size << ": "
                            << (r.ok() ? "ok" : r.error) << ", faces " << r.faces_before << " -> " << r.faces_after
                            << ", " << r.mesh.indices.size() << " triangles, " << r.seconds << " s";
    return r;
}

CadOpResult shell_solid(const CadBody &body, const std::vector<int> &faces_in, double thickness, const TessellationParams &tess)
{
    const auto t0 = std::chrono::steady_clock::now();
    if (faces_in.empty())
        return fail(CadOpStatus::NothingSelected, "pick at least one face to leave open");
    if (!(thickness > 0.) || !std::isfinite(thickness))
        return fail(CadOpStatus::BadSize, "the wall thickness must be greater than zero");
    std::vector<int> faces = faces_in;
    std::sort(faces.begin(), faces.end());
    faces.erase(std::unique(faces.begin(), faces.end()), faces.end());

    CadOpResult r;
    try {
        const TopoDS_Shape shape  = cad_body_shape(body);
        const ShapeInfo    before = shape_info(shape, false);
        r.faces_before            = before.faces;
        r.volume_before           = before.volume;
        if (before.solids != 1)
            return fail(CadOpStatus::Failed, "shell works on a single solid, and this part has " + std::to_string(before.solids));

        TopTools_IndexedMapOfShape fmap;
        TopExp::MapShapes(shape, TopAbs_FACE, fmap);
        TopTools_ListOfShape open;
        for (int f : faces) {
            if (f < 0 || f >= fmap.Extent())
                return fail(CadOpStatus::BadIndex, "a face index is out of range");
            open.Append(fmap(f + 1));
        }
        // A negative offset shells inward, so the outside of the part keeps its size.
        BRepOffsetAPI_MakeThickSolid mk;
        mk.MakeThickSolidByJoin(shape, open, -thickness, 1.e-3);
        mk.Build();
        if (!mk.IsDone()) {
            r.status  = CadOpStatus::Failed;
            r.error   = "the shell could not be built. The wall is probably too thick for the part, or a face cannot be "
                        "offset by that much. Try a thinner wall.";
            r.seconds = seconds_since(t0);
            return r;
        }
        finish(mk.Shape(), body, tess, r);
        if (r.ok() && !(r.volume_after < r.volume_before)) {
            r      = fail(CadOpStatus::InvalidResult, "the shell did not remove any material");
            r.body = nullptr;
        }
    } catch (const Standard_Failure &e) {
        r.status = CadOpStatus::Failed;
        r.error  = "the shell could not be built (" + occt_message(e) + "). Try a thinner wall.";
    } catch (const std::exception &e) {
        r.status = CadOpStatus::Failed;
        r.error  = e.what();
    }
    r.seconds = seconds_since(t0);
    BOOST_LOG_TRIVIAL(info) << "CAD shell of " << faces.size() << " open faces, thickness " << thickness << ": "
                            << (r.ok() ? "ok" : r.error) << ", " << r.seconds << " s";
    return r;
}

bool apply_cad_result(ModelVolume &volume, const CadOpResult &result)
{
    if (!result.ok() || !result.body || result.mesh.indices.empty())
        return false;
    const bool had_paint = !volume.supported_facets.empty() || !volume.seam_facets.empty() ||
                           !volume.mmu_segmentation_facets.empty() || !volume.fuzzy_skin_facets.empty();
    volume.supported_facets.reset();
    volume.seam_facets.reset();
    volume.mmu_segmentation_facets.reset();
    volume.fuzzy_skin_facets.reset();

    volume.set_mesh(TriangleMesh(result.mesh));
    volume.calculate_convex_hull();
    volume.invalidate_convex_hull_2d();
    volume.set_new_unique_id();

    auto body        = std::make_shared<CadBody>(*result.body);
    body->mesh       = mesh_fingerprint(volume.mesh().its);
    volume.cad_body  = std::move(body);
    return had_paint;
}

}} // namespace Slic3r::BRep
