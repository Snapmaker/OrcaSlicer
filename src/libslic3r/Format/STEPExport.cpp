// STEP export. The overall shape of the writer (compound the bodies at their placed
// transforms, STEPControl AsIs transfer) follows Orca-Cad's CadDocument::export_step
// (github.com/tommasobbianchi/Orca-Cad, AGPL-3.0); the XCAF assembly with names and colours,
// the per-part mesh -> B-rep conversion and the exact source-STEP recovery are EdgeSlicer's.

#include "STEPExport.hpp"
#include "STEP.hpp"

#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <APIHeaderSection_MakeHeader.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <Standard_Failure.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Trsf.hxx>

#include <chrono>
#include <cmath>
#include <map>
#include <memory>

namespace Slic3r {

namespace {

bool is_step_path(const std::string &path)
{
    return boost::iends_with(path, ".step") || boost::iends_with(path, ".stp");
}

BoundingBoxf3 tight_bbox(const TopoDS_Shape &shape)
{
    Bnd_Box box;
    BRepBndLib::AddOptimal(shape, box, Standard_False, Standard_False);
    if (box.IsVoid())
        return {};
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    return BoundingBoxf3(Vec3d(x0, y0, z0), Vec3d(x1, y1, z1));
}

double solids_volume(const TopoDS_Shape &shape)
{
    double volume = 0.;
    for (TopExp_Explorer ex(shape, TopAbs_SOLID); ex.More(); ex.Next()) {
        GProp_GProps props;
        BRepGProp::VolumeProperties(ex.Current(), props);
        volume += props.Mass();
    }
    return volume;
}

double shape_area(const TopoDS_Shape &shape)
{
    GProp_GProps props;
    BRepGProp::SurfaceProperties(shape, props);
    return props.Mass();
}

// `inner` lies inside `outer` grown by `slack` on every side.
bool bbox_inside(const BoundingBoxf3 &inner, const BoundingBoxf3 &outer, double slack)
{
    return (inner.min - outer.min).minCoeff() >= -slack && (outer.max - inner.max).minCoeff() >= -slack;
}

// Is `mesh` (in mesh coordinates, i.e. STEP coordinates minus `mesh_offset`) still the
// tessellation of `shape`? Every mesh vertex must lie on the shape's surface and every
// triangle close to it, with matching extents and volume. That rejects a part that was cut,
// simplified, remeshed, sculpted, booleaned or had a transform baked into its mesh.
bool mesh_matches_shape(const TopoDS_Shape &shape, const indexed_triangle_set &mesh, const Vec3d &mesh_offset, std::string &why)
{
    if (mesh.indices.empty()) {
        why = "the part has no mesh";
        return false;
    }
    BoundingBoxf3 mesh_bb;
    for (const stl_vertex &v : mesh.vertices)
        mesh_bb.merge(v.cast<double>() + mesh_offset);
    const BoundingBoxf3 shape_bb = tight_bbox(shape);
    if (!shape_bb.defined) {
        why = "the STEP shape is empty";
        return false;
    }
    const double diag = shape_bb.size().norm();
    if (!bbox_inside(mesh_bb, shape_bb, 0.01 + 1e-5 * diag)) {
        why = "the part's mesh reaches outside the STEP shape";
        return false;
    }
    if (!bbox_inside(shape_bb, mesh_bb, 0.05 + 0.02 * shape_bb.size().maxCoeff())) {
        why = "the part's mesh is smaller than the STEP shape";
        return false;
    }

    // A fine triangulation of the exact shape stands in for its surface.
    const double               lin  = std::clamp(diag * 5e-4, 0.002, 0.02);
    const indexed_triangle_set fine = BRep::brep_to_its(shape, lin, 0.2);
    if (fine.indices.empty()) {
        why = "the STEP shape could not be triangulated";
        return false;
    }
    const AABBMesh surface(fine);
    const double   tol_v = lin + 0.01 + 1e-5 * diag;
    for (const stl_vertex &v : mesh.vertices)
        if (surface.squared_distance(v.cast<double>() + mesh_offset) > tol_v * tol_v) {
            why = "the part's mesh is no longer on the STEP surface";
            return false;
        }
    double max_sag = 0.;
    for (const stl_triangle_vertex_indices &t : mesh.indices) {
        const Vec3d  a = mesh.vertices[t(0)].cast<double>(), b = mesh.vertices[t(1)].cast<double>(), c = mesh.vertices[t(2)].cast<double>();
        const double longest = std::max({(b - a).norm(), (c - b).norm(), (a - c).norm()});
        const double d       = std::sqrt(std::max(0., surface.squared_distance((a + b + c) / 3. + mesh_offset)));
        // A chord triangle sags by L^2 / 8R, far below 0.3 L; a cut cap or a patch spans the inside.
        if (d > tol_v + 0.3 * longest) {
            why = "the part's mesh has faces away from the STEP surface";
            return false;
        }
        max_sag = std::max(max_sag, d);
    }
    const double shape_vol = solids_volume(shape);
    if (shape_vol > 0.) {
        const double mesh_vol  = std::abs(double(its_volume(mesh)));
        const double allowance = 0.002 * shape_vol + 1.5 * shape_area(shape) * (max_sag + tol_v);
        if (std::abs(mesh_vol - shape_vol) > allowance) {
            why = "the part's volume differs from the STEP shape";
            return false;
        }
    }
    return true;
}

struct SourceFile
{
    bool                    ok = false;
    std::vector<NamedSolid> plain, split;
};

// Parsed source STEP files for one export call, so a multi-part file is read once.
struct SourceCache
{
    std::map<std::string, std::unique_ptr<SourceFile>> files;

    const SourceFile &get(const std::string &path)
    {
        auto it = files.find(path);
        if (it == files.end()) {
            auto file = std::make_unique<SourceFile>();
            try {
                file->ok = read_step_named_shapes(path.c_str(), file->plain, file->split);
            } catch (const std::exception &) {
                file->ok = false;
            }
            it = files.emplace(path, std::move(file)).first;
        }
        return *it->second;
    }
};

TopoDS_Shape find_source_brep(const ModelVolume &volume, SourceCache &cache, std::string *why_not)
{
    auto fail = [why_not](const std::string &why) {
        if (why_not)
            *why_not = why;
        return TopoDS_Shape();
    };
    const std::string &path = volume.source.input_file;
    if (path.empty() || !is_step_path(path))
        return fail("the part was not imported from STEP");
    boost::system::error_code ec;
    if (!boost::filesystem::exists(boost::filesystem::path(path), ec))
        return fail("the source STEP file is gone: " + path);
    const SourceFile &file = cache.get(path);
    if (!file.ok)
        return fail("the source STEP file cannot be read: " + path);

    // Same-named shapes first (the import named the volume after its shape), then the rest.
    std::vector<const NamedSolid *> candidates;
    for (int pass = 0; pass < 2; ++pass)
        for (const std::vector<NamedSolid> *list : {&file.plain, &file.split})
            for (const NamedSolid &ns : *list)
                if ((ns.name == volume.name) == (pass == 0))
                    candidates.push_back(&ns);

    const indexed_triangle_set &mesh   = volume.mesh().its;
    const Vec3d                 offset = volume.source.mesh_offset;
    std::string                 why    = "no shape in the source STEP matches the part";
    for (const NamedSolid *ns : candidates) {
        if (ns->solid.IsNull())
            continue;
        try {
            if (mesh_matches_shape(ns->solid, mesh, offset, why)) {
                gp_Trsf to_mesh;
                to_mesh.SetTranslation(gp_Vec(-offset.x(), -offset.y(), -offset.z()));
                return BRepBuilderAPI_Transform(ns->solid, to_mesh, Standard_True).Shape();
            }
        } catch (const Standard_Failure &) {
            why = "OCCT failed while comparing the part with the source STEP";
        }
    }
    return fail(why);
}

// Place a shape with an affine matrix. Similarities (rotation, uniform scale, mirror) keep the
// exact geometry; a non-uniform scale goes through gp_GTrsf, which converts to B-splines.
TopoDS_Shape transform_shape(const TopoDS_Shape &shape, const Transform3d &m)
{
    if (m.isApprox(Transform3d::Identity(), 1e-12))
        return shape;
    const Matrix3d lin = m.linear();
    const double   det = lin.determinant();
    if (std::abs(det) < 1e-12)
        throw Slic3r::RuntimeError("the part's transformation is singular");
    const double   s        = std::cbrt(std::abs(det));
    const Matrix3d r        = lin / s;
    const bool     similar  = (r.transpose() * r - Matrix3d::Identity()).cwiseAbs().maxCoeff() < 1e-7;
    TopoDS_Shape   out;
    if (similar) {
        gp_Trsf t;
        t.SetValues(m(0, 0), m(0, 1), m(0, 2), m(0, 3), m(1, 0), m(1, 1), m(1, 2), m(1, 3), m(2, 0), m(2, 1), m(2, 2), m(2, 3));
        out = BRepBuilderAPI_Transform(shape, t, Standard_True).Shape();
    } else {
        gp_GTrsf g;
        g.SetVectorialPart(gp_Mat(m(0, 0), m(0, 1), m(0, 2), m(1, 0), m(1, 1), m(1, 2), m(2, 0), m(2, 1), m(2, 2)));
        g.SetTranslationPart(gp_XYZ(m(0, 3), m(1, 3), m(2, 3)));
        out = BRepBuilderAPI_GTransform(shape, g, Standard_True).Shape();
        if (det < 0.) {
            // A mirroring non-uniform scale can leave solids inside out.
            for (TopExp_Explorer ex(out, TopAbs_SOLID); ex.More(); ex.Next()) {
                GProp_GProps props;
                BRepGProp::VolumeProperties(ex.Current(), props);
                if (props.Mass() < 0.) {
                    out.Reverse();
                    break;
                }
            }
        }
    }
    return out;
}

bool parse_colour(const std::string &hex, Quantity_Color &colour)
{
    if (hex.size() != 7 || hex[0] != '#')
        return false;
    try {
        const unsigned long v = std::stoul(hex.substr(1), nullptr, 16);
        colour = Quantity_Color(double((v >> 16) & 0xff) / 255., double((v >> 8) & 0xff) / 255., double(v & 0xff) / 255., Quantity_TOC_sRGB);
        return true;
    } catch (...) {
        return false;
    }
}

struct Part
{
    std::string    name;
    TopoDS_Shape   shape;
    bool           has_colour = false;
    Quantity_Color colour;
};

} // namespace

TopoDS_Shape step_source_brep(const ModelVolume &volume, std::string *why_not)
{
    SourceCache cache;
    try {
        return find_source_brep(volume, cache, why_not);
    } catch (const Standard_Failure &) {
        if (why_not)
            *why_not = "OCCT failed while reading the source STEP";
    }
    return TopoDS_Shape();
}

bool store_step(const std::string &path, const std::vector<StepExportItem> &items, const StepExportParams &params, StepExportReport &report)
{
    report          = StepExportReport{};
    const auto t0   = std::chrono::steady_clock::now();
    auto       done = [&](bool ok) {
        report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return ok;
    };

    SourceCache cache;
    // Objects (one per exported instance), each with its parts in world coordinates.
    std::vector<std::pair<std::string, std::vector<Part>>> objects;
    try {
        for (const StepExportItem &item : items) {
            const ModelObject *object = item.object;
            if (object == nullptr)
                continue;
            std::vector<int> instances;
            if (item.instance_idx >= 0 && item.instance_idx < int(object->instances.size()))
                instances.push_back(item.instance_idx);
            else if (item.instance_idx < 0)
                for (int i = 0; i < int(object->instances.size()); ++i)
                    instances.push_back(i);
            for (int inst : instances) {
                std::string object_name = object->name.empty() ? std::string("object") : object->name;
                if (object->instances.size() > 1)
                    object_name += " #" + std::to_string(inst + 1);
                std::vector<Part> parts;
                const Transform3d world = Geometry::translation_transform(params.world_offset) * object->instances[inst]->get_matrix();
                for (size_t vi = 0; vi < object->volumes.size(); ++vi) {
                    const ModelVolume *volume = object->volumes[vi];
                    if (volume->is_negative_volume()) {
                        ++report.skipped_negative;
                        continue;
                    }
                    if (!volume->is_model_part())
                        continue;
                    Part part;
                    part.name = volume->name.empty() ? "part " + std::to_string(vi + 1) : volume->name;
                    const int extruder = volume->extruder_id();
                    if (extruder >= 1 && extruder <= int(params.extruder_colours.size()))
                        part.has_colour = parse_colour(params.extruder_colours[extruder - 1], part.colour);

                    std::string  why_not;
                    TopoDS_Shape exact;
                    if (params.use_source_brep && !volume->source.input_file.empty() && is_step_path(volume->source.input_file))
                        exact = find_source_brep(*volume, cache, &why_not);
                    if (!exact.IsNull()) {
                        part.shape = transform_shape(exact, world * volume->get_matrix());
                        ++report.exact_parts;
                    } else {
                        if (!why_not.empty())
                            report.warnings.emplace_back(part.name + ": exported from its mesh (" + why_not + ")");
                        TriangleMesh mesh = object->volume_mesh_in_world(inst, int(vi));
                        if (!params.world_offset.isZero())
                            mesh.translate(params.world_offset.cast<float>());
                        if (mesh.empty())
                            continue;
                        BRep::MeshToBRepStats stats;
                        try {
                            part.shape = BRep::mesh_to_brep(mesh.its, params.mesh, stats);
                        } catch (const Slic3r::RuntimeError &e) {
                            // Only degenerate triangles: leave this part out, not the whole export.
                            report.warnings.emplace_back(part.name + ": not exported (" + e.what() + ")");
                            continue;
                        }
                        ++report.mesh_parts;
                        if (!stats.is_solid)
                            ++report.open_parts;
                        for (const std::string &w : stats.warnings)
                            report.warnings.emplace_back(part.name + ": " + w);
                        BOOST_LOG_TRIVIAL(info) << "STEP export: " << part.name << " " << stats.kept_triangles << " triangles -> "
                                                << stats.faces_final << " faces, " << stats.solids << " solids, "
                                                << stats.open_shells << " shells, build " << stats.seconds_build << " s, merge "
                                                << stats.seconds_merge << " s";
                    }
                    if (!part.shape.IsNull()) {
                        parts.push_back(std::move(part));
                        ++report.parts;
                    }
                }
                if (!parts.empty())
                    objects.emplace_back(std::move(object_name), std::move(parts));
            }
        }
    } catch (const Standard_Failure &e) {
        report.error = std::string("OCCT failed while building the shapes: ") + (e.GetMessageString() ? e.GetMessageString() : "unknown error");
        return done(false);
    } catch (const std::exception &e) {
        report.error = e.what();
        return done(false);
    }
    if (report.skipped_negative > 0)
        report.warnings.emplace_back(std::to_string(report.skipped_negative) + " negative parts were not exported (they are not subtracted either)");
    if (objects.empty()) {
        report.error = "nothing to export";
        return done(false);
    }
    report.objects = int(objects.size());
    const double seconds_shapes = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();
    Handle(TDocStd_Document) doc;
    app->NewDocument("MDTV-XCAF", doc);
    bool ok = false;
    try {
        Handle(XCAFDoc_ShapeTool) shapes  = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
        Handle(XCAFDoc_ColorTool) colours = XCAFDoc_DocumentTool::ColorTool(doc->Main());
        auto add_part = [&](const Part &part, const std::string &name) {
            const TDF_Label label = shapes->AddShape(part.shape, Standard_False);
            TDataStd_Name::Set(label, TCollection_ExtendedString(name.c_str(), Standard_True));
            if (part.has_colour)
                colours->SetColor(label, part.colour, XCAFDoc_ColorSurf);
            return label;
        };
        TDF_Label root;
        if (objects.size() > 1) {
            root = shapes->NewShape();
            TDataStd_Name::Set(root, TCollection_ExtendedString(params.product_name.c_str(), Standard_True));
        }
        for (const auto &[object_name, parts] : objects) {
            TDF_Label object_label;
            if (parts.size() == 1)
                object_label = add_part(parts.front(), object_name);
            else {
                object_label = shapes->NewShape();
                TDataStd_Name::Set(object_label, TCollection_ExtendedString(object_name.c_str(), Standard_True));
                for (const Part &part : parts) {
                    const TDF_Label comp = shapes->AddComponent(object_label, add_part(part, part.name), TopLoc_Location());
                    TDataStd_Name::Set(comp, TCollection_ExtendedString(part.name.c_str(), Standard_True));
                }
            }
            if (!root.IsNull()) {
                const TDF_Label comp = shapes->AddComponent(root, object_label, TopLoc_Location());
                TDataStd_Name::Set(comp, TCollection_ExtendedString(object_name.c_str(), Standard_True));
            }
        }
        shapes->UpdateAssemblies();

        STEPCAFControl_Writer writer;
        writer.SetColorMode(Standard_True);
        writer.SetNameMode(Standard_True);
        Interface_Static::SetCVal("write.step.unit", "MM");
        Interface_Static::SetCVal("write.step.schema", "AP214IS");
        // No p-curves: CAD systems rebuild them, and for a faceted B-rep they would double the
        // file (the source STEP files we re-export rarely carry them either).
        Interface_Static::SetIVal("write.surfacecurve.mode", 0);
        const auto t_transfer = std::chrono::steady_clock::now();
        if (!writer.Transfer(doc, STEPControl_AsIs))
            report.error = "the shapes could not be transferred to STEP";
        else {
            const auto t_write = std::chrono::steady_clock::now();
            APIHeaderSection_MakeHeader header(writer.ChangeWriter().Model());
            header.SetOriginatingSystem(new TCollection_HAsciiString("EdgeSlicer"));
            header.SetName(new TCollection_HAsciiString(boost::filesystem::path(path).filename().string().c_str()));
            if (writer.Write(path.c_str()) != IFSelect_RetDone)
                report.error = "cannot write " + path;
            else
                ok = true;
            const auto t_end = std::chrono::steady_clock::now();
            BOOST_LOG_TRIVIAL(info) << "STEP export: shapes " << seconds_shapes << " s, transfer "
                                    << std::chrono::duration<double>(t_write - t_transfer).count() << " s, write "
                                    << std::chrono::duration<double>(t_end - t_write).count() << " s";
        }
    } catch (const Standard_Failure &e) {
        report.error = std::string("OCCT failed while writing STEP: ") + (e.GetMessageString() ? e.GetMessageString() : "unknown error");
    }
    app->Close(doc);
    return done(ok);
}

bool store_step(const std::string &path, const Model &model, const StepExportParams &params, StepExportReport &report)
{
    std::vector<StepExportItem> items;
    for (const ModelObject *object : model.objects)
        items.push_back({object, -1});
    return store_step(path, items, params, report);
}

} // namespace Slic3r
