#include <catch2/catch.hpp>

#include "libslic3r/BRep/MeshToBRep.hpp"
#include "libslic3r/Format/STEP.hpp"
#include "libslic3r/Format/STEPExport.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PartMeshReplace.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_Writer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <chrono>
#include <sstream>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// Per-process unique temp names: tests running in parallel worktrees must not collide.
struct TempFile
{
    boost::filesystem::path path;
    explicit TempFile(const char *ext)
        : path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path(std::string("edge_step_%%%%-%%%%-%%%%") + ext))
    {}
    ~TempFile()
    {
        boost::system::error_code ec;
        boost::filesystem::remove(path, ec);
    }
    std::string str() const { return path.string(); }
};

// Every shape of a STEP file, as load_step would name and split them (no compound split).
std::vector<NamedSolid> reread(const std::string &path)
{
    std::vector<NamedSolid> plain, split;
    REQUIRE(read_step_named_shapes(path.c_str(), plain, split));
    return plain;
}

BRep::ShapeInfo info_of_all(const std::vector<NamedSolid> &shapes)
{
    BRep::ShapeInfo all;
    for (const NamedSolid &ns : shapes) {
        const BRep::ShapeInfo i = BRep::shape_info(ns.solid);
        all.solids += i.solids;
        all.shells += i.shells;
        all.free_shells += i.free_shells;
        all.faces += i.faces;
        all.planar_faces += i.planar_faces;
        all.volume += i.volume;
        all.bbox.merge(i.bbox);
    }
    return all;
}

void require_bbox(const BoundingBoxf3 &a, const BoundingBoxf3 &b, double tol)
{
    for (int i = 0; i < 3; ++i) {
        REQUIRE_THAT(a.min(i), WithinAbs(b.min(i), tol));
        REQUIRE_THAT(a.max(i), WithinAbs(b.max(i), tol));
    }
}

// A cube whose faces are each an n x n grid of quads: many coplanar triangles to merge.
indexed_triangle_set gridded_cube(double size, int n)
{
    indexed_triangle_set its;
    auto add_face = [&](const Vec3d &origin, const Vec3d &u, const Vec3d &v) {
        const int base = int(its.vertices.size());
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i)
                its.vertices.emplace_back((origin + u * (double(i) / n) + v * (double(j) / n)).cast<float>());
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const int a = base + j * (n + 1) + i, b = a + 1, c = a + n + 1, d = c + 1;
                its.indices.emplace_back(a, b, d);
                its.indices.emplace_back(a, d, c);
            }
    };
    const double s = size;
    // Outward normals: u x v points out of the cube.
    add_face(Vec3d(0, 0, 0), Vec3d(0, s, 0), Vec3d(s, 0, 0)); // bottom (-z)
    add_face(Vec3d(0, 0, s), Vec3d(s, 0, 0), Vec3d(0, s, 0)); // top (+z)
    add_face(Vec3d(0, 0, 0), Vec3d(s, 0, 0), Vec3d(0, 0, s)); // front (-y)
    add_face(Vec3d(0, s, 0), Vec3d(0, 0, s), Vec3d(s, 0, 0)); // back (+y)
    add_face(Vec3d(0, 0, 0), Vec3d(0, 0, s), Vec3d(0, s, 0)); // left (-x)
    add_face(Vec3d(s, 0, 0), Vec3d(0, s, 0), Vec3d(0, 0, s)); // right (+x)
    // The grid duplicates the vertices along the cube's edges; mesh_to_brep welds them.
    return its;
}

void write_occt_step(const TopoDS_Shape &shape, const std::string &path)
{
    STEPControl_Writer writer;
    REQUIRE(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone);
    REQUIRE(writer.Write(path.c_str()) == IFSelect_RetDone);
}

// Import a STEP file the way the GUI does (load_step), into a fresh object.
ModelObject *import_step(Model &model, const std::string &path)
{
    bool cancelled = false;
    REQUIRE(load_step(path.c_str(), &model, cancelled));
    REQUIRE(!model.objects.empty());
    ModelObject *object = model.objects.back();
    object->add_instance();
    return object;
}

} // namespace

// ------------------------------------------------------------------------------------------
// Mesh -> B-rep
// ------------------------------------------------------------------------------------------

TEST_CASE("mesh_to_brep turns a closed cube into a solid with six planar faces", "[MeshToBRep]")
{
    BRep::MeshToBRepStats stats;
    const TopoDS_Shape    shape = BRep::mesh_to_brep(its_make_cube(20., 30., 40.), {}, stats);
    const BRep::ShapeInfo info  = BRep::shape_info(shape, true);
    CHECK(stats.watertight);
    CHECK(stats.is_solid);
    CHECK(stats.faces_before_merge == 12);
    CHECK(info.valid);
    CHECK(info.solids == 1);
    CHECK(info.faces == 6);
    CHECK(info.planar_faces == 6);
    CHECK(info.edges == 12);
    CHECK_THAT(info.volume, WithinRel(24000., 1e-9));
    require_bbox(info.bbox, BoundingBoxf3(Vec3d::Zero(), Vec3d(20., 30., 40.)), 1e-6);
}

TEST_CASE("mesh_to_brep without merging keeps one face per triangle", "[MeshToBRep]")
{
    BRep::MeshToBRepParams params;
    params.merge_angle_deg = 0.;
    const BRep::ShapeInfo info = BRep::shape_info(BRep::mesh_to_brep(its_make_cube(10., 10., 10.), params), true);
    CHECK(info.valid);
    CHECK(info.solids == 1);
    CHECK(info.faces == 12);
    CHECK_THAT(info.volume, WithinRel(1000., 1e-9));
}

TEST_CASE("mesh_to_brep merges a tessellated cylinder's caps and wall quads", "[MeshToBRep]")
{
    const indexed_triangle_set its = its_make_cylinder(10., 20., 2. * PI / 36.);
    const BRep::ShapeInfo      info = BRep::shape_info(BRep::mesh_to_brep(its), true);
    CHECK(info.valid);
    CHECK(info.solids == 1);
    CHECK(info.faces == 36 + 2);
    CHECK(info.planar_faces == info.faces);
    CHECK(info.edges == 3 * 36); // 36 vertical edges, 36 around each cap
    CHECK_THAT(info.volume, WithinRel(double(its_volume(its)), 1e-5));
}

TEST_CASE("mesh_to_brep returns an open mesh as a shell with a warning", "[MeshToBRep]")
{
    indexed_triangle_set its = its_make_cube(10., 10., 10.);
    its.indices.pop_back(); // punch a triangular hole
    BRep::MeshToBRepStats stats;
    const TopoDS_Shape    shape = BRep::mesh_to_brep(its, {}, stats);
    const BRep::ShapeInfo info  = BRep::shape_info(shape);
    CHECK_FALSE(stats.watertight);
    CHECK_FALSE(stats.is_solid);
    CHECK(stats.boundary_edges == 3);
    CHECK(stats.open_shells == 1);
    CHECK_FALSE(stats.warnings.empty());
    CHECK(shape.ShapeType() == TopAbs_SHELL);
    CHECK(info.solids == 0);
    CHECK(info.free_shells == 1);
    CHECK(info.volume == 0.);
}

TEST_CASE("mesh_to_brep builds a solid per closed component and keeps cavities", "[MeshToBRep]")
{
    SECTION("two separate cubes are two solids")
    {
        indexed_triangle_set its   = its_make_cube(10., 10., 10.);
        indexed_triangle_set other = its_make_cube(5., 5., 5.);
        for (stl_vertex &v : other.vertices)
            v.x() += 20.f;
        its_merge(its, other);
        BRep::MeshToBRepStats stats;
        const BRep::ShapeInfo info = BRep::shape_info(BRep::mesh_to_brep(its, {}, stats), true);
        CHECK(stats.solids == 2);
        CHECK(info.valid);
        CHECK(info.solids == 2);
        CHECK(info.faces == 12);
        CHECK_THAT(info.volume, WithinRel(1125., 1e-9));
    }
    SECTION("an inward-facing cube inside a cube is a cavity")
    {
        indexed_triangle_set its  = its_make_cube(20., 20., 20.);
        indexed_triangle_set hole = its_make_cube(10., 10., 10.);
        for (stl_vertex &v : hole.vertices)
            v += Vec3f(5.f, 5.f, 5.f);
        its_flip_triangles(hole);
        its_merge(its, hole);
        BRep::MeshToBRepStats stats;
        const BRep::ShapeInfo info = BRep::shape_info(BRep::mesh_to_brep(its, {}, stats), true);
        CHECK(stats.cavities == 1);
        CHECK(info.valid);
        CHECK(info.solids == 1);
        CHECK(info.shells == 2);
        CHECK_THAT(info.volume, WithinRel(8000. - 1000., 1e-9));
    }
    SECTION("an inside-out cube on its own is flipped")
    {
        indexed_triangle_set its = its_make_cube(10., 10., 10.);
        its_flip_triangles(its);
        BRep::MeshToBRepStats stats;
        const BRep::ShapeInfo info = BRep::shape_info(BRep::mesh_to_brep(its, {}, stats), true);
        CHECK(stats.inverted_solids == 1);
        CHECK(info.valid);
        CHECK_THAT(info.volume, WithinRel(1000., 1e-9));
    }
}

TEST_CASE("mesh_to_brep output takes an exact OCCT fillet (ModelingAlgorithms is linked)", "[MeshToBRep]")
{
    const TopoDS_Shape       cube = BRep::mesh_to_brep(its_make_cube(20., 20., 20.));
    BRepFilletAPI_MakeFillet fillet(cube);
    for (TopExp_Explorer ex(cube, TopAbs_EDGE); ex.More(); ex.Next())
        fillet.Add(2., TopoDS::Edge(ex.Current()));
    fillet.Build();
    REQUIRE(fillet.IsDone());
    const BRep::ShapeInfo info = BRep::shape_info(fillet.Shape(), true);
    CHECK(info.valid);
    CHECK(info.solids == 1);
    CHECK(info.faces == 6 + 12 + 8); // faces, edge blends, corner blends
    // 12 edges lose (4 - pi) r^2 * (20 - 2r) each, 8 corners lose (8 - 4/3 pi - 3 (4 - pi)) r^3 / 8 ... check the order of magnitude only.
    CHECK(info.volume < 8000.);
    CHECK(info.volume > 8000. - 12. * (4. - PI) * 4. * 20.);
}

TEST_CASE("mesh_to_brep on large meshes completes in reasonable time", "[MeshToBRep][timing]")
{
    SECTION("a 120k-triangle gridded cube merges back to six faces")
    {
        const indexed_triangle_set its = gridded_cube(50., 100);
        REQUIRE(its.indices.size() == 120000);
        BRep::MeshToBRepStats stats;
        const auto            t0   = std::chrono::steady_clock::now();
        const TopoDS_Shape    shape = BRep::mesh_to_brep(its, {}, stats);
        const double          secs  = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const BRep::ShapeInfo info  = BRep::shape_info(shape, true);
        WARN("gridded cube 120000 triangles: build " << stats.seconds_build << " s, merge " << stats.seconds_merge << " s, total " << secs
                                                   << " s, faces " << stats.faces_before_merge << " -> " << info.faces);
        CHECK(stats.is_solid);
        CHECK(info.valid);
        CHECK(info.faces == 6);
        CHECK(info.edges == 12); // the 100 grid segments along each side are joined into one edge
        CHECK_THAT(info.volume, WithinRel(125000., 1e-6));
        CHECK(secs < 120.);
    }
}

// Heavy: run explicitly with "[.timing]" (about 40 s here); the gridded cube above covers the
// merge in the regular suite.
TEST_CASE("mesh_to_brep and store_step on a 130k-triangle sphere", "[.timing]")
{
    {
        const indexed_triangle_set its = its_make_sphere(40., PI / 180.);
        BRep::MeshToBRepStats stats;
        const auto            t0   = std::chrono::steady_clock::now();
        const TopoDS_Shape    shape = BRep::mesh_to_brep(its, {}, stats);
        const double          secs  = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        WARN("sphere " << its.indices.size() << " triangles: build " << stats.seconds_build << " s, merge " << stats.seconds_merge
                       << " s, total " << secs << " s, faces " << stats.faces_before_merge << " -> " << stats.faces_final);
        CHECK(stats.is_solid);
        CHECK_THAT(stats.volume, WithinRel(double(its_volume(its)), 1e-4)); // its_volume sums in float
        CHECK(secs < 120.);
        // Writing it out is part of the cost a user sees.
        TempFile step(".step");
        Model    model;
        ModelObject *object = model.add_object();
        object->name        = "sphere";
        object->add_volume(TriangleMesh(its), ModelVolumeType::MODEL_PART, false);
        object->add_instance();
        StepExportReport report;
        const auto       t1 = std::chrono::steady_clock::now();
        REQUIRE(store_step(step.str(), model, {}, report));
        const double export_secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
        WARN("sphere store_step " << export_secs << " s, " << boost::filesystem::file_size(step.path) / (1024 * 1024) << " MiB");
        CHECK(export_secs < 180.);
    }
}

// ------------------------------------------------------------------------------------------
// STEP export
// ------------------------------------------------------------------------------------------

TEST_CASE("STEP export of a mesh cube re-imports as six planar faces in world coordinates", "[StepExport]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "cube";
    ModelVolume *volume = object->add_volume(make_cube(20., 20., 20.), ModelVolumeType::MODEL_PART, false);
    volume->name        = "cube";
    object->add_instance()->set_offset(Vec3d(100., 50., 0.));

    TempFile         step(".step");
    StepExportReport report;
    REQUIRE(store_step(step.str(), model, {}, report));
    CHECK(report.error.empty());
    CHECK(report.parts == 1);
    CHECK(report.mesh_parts == 1);
    CHECK(report.open_parts == 0);

    const std::vector<NamedSolid> shapes = reread(step.str());
    REQUIRE(shapes.size() == 1);
    CHECK(shapes.front().name == "cube");
    const BRep::ShapeInfo info = BRep::shape_info(shapes.front().solid, true);
    CHECK(info.valid);
    CHECK(info.solids == 1);
    CHECK(info.faces == 6);
    CHECK(info.planar_faces == 6);
    CHECK_THAT(info.volume, WithinRel(8000., 1e-6));
    require_bbox(info.bbox, BoundingBoxf3(Vec3d(100., 50., 0.), Vec3d(120., 70., 20.)), 1e-3);

    // And through the regular importer.
    Model reimported;
    bool  cancelled = false;
    REQUIRE(load_step(step.str().c_str(), &reimported, cancelled));
    REQUIRE(reimported.objects.size() == 1);
    REQUIRE(reimported.objects.front()->volumes.size() == 1);
    CHECK_THAT(double(its_volume(reimported.objects.front()->volumes.front()->mesh().its)), WithinRel(8000., 1e-5));
}

TEST_CASE("STEP export of a tessellated cylinder keeps its facets as planar faces", "[StepExport]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "cyl";
    TriangleMesh cyl = make_cylinder(10., 20., 2. * PI / 48.);
    object->add_volume(TriangleMesh(cyl), ModelVolumeType::MODEL_PART, false);
    object->add_instance();

    TempFile         step(".stp");
    StepExportReport report;
    REQUIRE(store_step(step.str(), model, {}, report));
    const std::vector<NamedSolid> shapes = reread(step.str());
    REQUIRE(shapes.size() == 1);
    const BRep::ShapeInfo info = BRep::shape_info(shapes.front().solid, true);
    CHECK(info.valid);
    CHECK(info.solids == 1);
    CHECK(info.faces == 48 + 2);
    CHECK_THAT(info.volume, WithinRel(double(cyl.volume()), 1e-5));
    require_bbox(info.bbox, cyl.bounding_box(), 1e-3);
}

TEST_CASE("STEP export of a multi-part object writes an assembly with named, coloured parts", "[StepExport]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "bracket";
    ModelVolume *base   = object->add_volume(make_cube(40., 20., 5.), ModelVolumeType::MODEL_PART, false);
    base->name          = "base";
    base->config.set("extruder", 1);
    ModelVolume *peg    = object->add_volume(make_cylinder(3., 15., 2. * PI / 24.), ModelVolumeType::MODEL_PART, false);
    peg->name           = "peg";
    peg->set_offset(Vec3d(20., 10., 5.));
    peg->config.set("extruder", 2);
    // A modifier and a negative volume must not be exported.
    ModelVolume *mod = object->add_volume(make_cube(5., 5., 5.), ModelVolumeType::PARAMETER_MODIFIER, false);
    mod->name        = "modifier";
    ModelVolume *neg = object->add_volume(make_cube(2., 2., 2.), ModelVolumeType::NEGATIVE_VOLUME, false);
    neg->name        = "hole";
    object->add_instance()->set_offset(Vec3d(10., 10., 0.));

    // A second object with two instances, to get a root assembly and instance names.
    ModelObject *other = model.add_object();
    other->name        = "block";
    other->add_volume(make_cube(10., 10., 10.), ModelVolumeType::MODEL_PART, false)->name = "block";
    other->add_instance()->set_offset(Vec3d(100., 0., 0.));
    other->add_instance()->set_offset(Vec3d(150., 0., 0.));

    StepExportParams params;
    // Not OCCT's predefined colours (those are written as DRAUGHTING_PRE_DEFINED_COLOUR).
    params.extruder_colours = {"#E07020", "#2070E0"};
    TempFile         step(".step");
    StepExportReport report;
    REQUIRE(store_step(step.str(), model, params, report));
    CHECK(report.objects == 3);
    CHECK(report.parts == 4);
    CHECK(report.skipped_negative == 1);

    const std::vector<NamedSolid> shapes = reread(step.str());
    REQUIRE(shapes.size() == 4);
    std::vector<std::string> names;
    for (const NamedSolid &ns : shapes)
        names.push_back(ns.name);
    CHECK(std::count(names.begin(), names.end(), "base") == 1);
    CHECK(std::count(names.begin(), names.end(), "peg") == 1);
    // A single-part object is one shape named after the object, per instance.
    CHECK(std::count(names.begin(), names.end(), "block #1") == 1);
    CHECK(std::count(names.begin(), names.end(), "block #2") == 1);

    const BRep::ShapeInfo all = info_of_all(shapes);
    CHECK(all.solids == 4);
    const double peg_volume = its_volume(its_make_cylinder(3., 15., 2. * PI / 24.));
    CHECK_THAT(all.volume, WithinRel(40. * 20. * 5. + peg_volume + 2. * 1000., 1e-5));
    CHECK(all.faces == 6 + (24 + 2) + 6 + 6);
    require_bbox(all.bbox, BoundingBoxf3(Vec3d(10., 0., 0.), Vec3d(160., 30., 20.)), 1e-3);

    // Colours: one style per coloured part.
    boost::nowide::ifstream in(step.str());
    std::stringstream       text;
    text << in.rdbuf();
    const std::string s = text.str();
    CHECK(s.find("COLOUR_RGB") != std::string::npos);
    CHECK(s.find("EdgeSlicer") != std::string::npos);
}

TEST_CASE("STEP export flips nothing inside out under a mirroring transform", "[StepExport]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "mirrored";
    ModelVolume *volume = object->add_volume(make_cube(10., 20., 30.), ModelVolumeType::MODEL_PART, false);
    volume->set_mirror(Vec3d(-1., 1., 1.));
    object->add_instance()->set_rotation(Vec3d(0., 0., PI / 6.));

    TempFile         step(".step");
    StepExportReport report;
    REQUIRE(store_step(step.str(), model, {}, report));
    const BRep::ShapeInfo info = info_of_all(reread(step.str()));
    CHECK(info.solids == 1);
    CHECK_THAT(info.volume, WithinRel(6000., 1e-6));
}

TEST_CASE("STEP export of an open mesh writes a surface and warns", "[StepExport]")
{
    Model                model;
    ModelObject         *object = model.add_object();
    object->name                = "open box";
    indexed_triangle_set its    = its_make_cube(10., 10., 10.);
    its.indices.resize(its.indices.size() - 2); // drop one side
    object->add_volume(TriangleMesh(its), ModelVolumeType::MODEL_PART, false);
    object->add_instance();

    TempFile         step(".step");
    StepExportReport report;
    REQUIRE(store_step(step.str(), model, {}, report));
    CHECK(report.open_parts == 1);
    CHECK_FALSE(report.warnings.empty());
    const BRep::ShapeInfo info = info_of_all(reread(step.str()));
    CHECK(info.solids == 0);
    CHECK(info.faces == 5);
}

TEST_CASE("STEP export leaves out a degenerate part but exports the rest", "[StepExport]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "mixed";
    object->add_volume(make_cube(10., 10., 10.), ModelVolumeType::MODEL_PART, false)->name = "good";
    indexed_triangle_set flat;
    flat.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(0.f, 0.f, 0.f), Vec3f(0.f, 0.f, 0.f)};
    flat.indices  = {Vec3i32(0, 1, 2)};
    object->add_volume(TriangleMesh(flat), ModelVolumeType::MODEL_PART, false)->name = "dust";
    object->add_instance();

    TempFile         step(".step");
    StepExportReport report;
    REQUIRE(store_step(step.str(), model, {}, report));
    CHECK(report.parts == 1);
    REQUIRE_FALSE(report.warnings.empty());
    CHECK(report.warnings.front().find("dust") != std::string::npos);
    CHECK(info_of_all(reread(step.str())).solids == 1);
}

TEST_CASE("STEP export of a part imported from STEP writes the exact B-rep", "[StepExport]")
{
    // An exact cylinder with a box-shaped notch: 3 cylinder faces + the notch's faces.
    const TopoDS_Shape cylinder = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(5., 5., 0.), gp_Dir(0., 0., 1.)), 10., 20.).Shape();
    const TopoDS_Shape notch    = BRepPrimAPI_MakeBox(gp_Pnt(12., 0., 10.), 10., 10., 20.).Shape();
    BRepAlgoAPI_Cut    cut(cylinder, notch);
    REQUIRE(cut.IsDone());
    const TopoDS_Shape    source      = cut.Shape();
    const BRep::ShapeInfo source_info = BRep::shape_info(source, true);
    REQUIRE(source_info.valid);

    TempFile source_step(".step");
    write_occt_step(source, source_step.str());

    Model        model;
    ModelObject *object = import_step(model, source_step.str());
    REQUIRE(object->volumes.size() == 1);
    // Rotated, uniformly scaled and moved, like a user would.
    object->instances.front()->set_offset(Vec3d(80., 60., 0.));
    object->instances.front()->set_rotation(Vec3d(0., 0., PI / 4.));
    object->instances.front()->set_scaling_factor(Vec3d(2., 2., 2.));

    SECTION("the exact shape is recovered and placed")
    {
        std::string        why;
        const TopoDS_Shape exact = step_source_brep(*object->volumes.front(), &why);
        INFO(why);
        REQUIRE_FALSE(exact.IsNull());

        TempFile         step(".step");
        StepExportReport report;
        REQUIRE(store_step(step.str(), model, {}, report));
        CHECK(report.exact_parts == 1);
        CHECK(report.mesh_parts == 0);
        const std::vector<NamedSolid> shapes = reread(step.str());
        REQUIRE(shapes.size() == 1);
        const BRep::ShapeInfo info = BRep::shape_info(shapes.front().solid, true);
        CHECK(info.valid);
        CHECK(info.solids == 1);
        CHECK(info.faces == source_info.faces);                            // not a triangle soup
        CHECK(info.planar_faces == source_info.planar_faces);
        CHECK_THAT(info.volume, WithinRel(8. * source_info.volume, 1e-6)); // exact, scaled by 2^3
        // The exported solid sits where the object is shown.
        const TriangleMesh world = object->volume_mesh_in_world(0, 0);
        require_bbox(info.bbox, world.bounding_box(), 0.05);
    }
    SECTION("an edited mesh falls back to the mesh, with the reason")
    {
        ModelVolume         *volume = object->volumes.front();
        indexed_triangle_set its    = volume->mesh().its;
        for (stl_vertex &v : its.vertices)
            v.z() *= 0.9f; // "baked" non-uniform scale
        volume->set_mesh(TriangleMesh(its));

        std::string why;
        CHECK(step_source_brep(*volume, &why).IsNull());
        CHECK_FALSE(why.empty());

        TempFile         step(".step");
        StepExportReport report;
        REQUIRE(store_step(step.str(), model, {}, report));
        CHECK(report.exact_parts == 0);
        CHECK(report.mesh_parts == 1);
        CHECK_FALSE(report.warnings.empty());
    }
    SECTION("a missing source file falls back to the mesh")
    {
        object->volumes.front()->source.input_file = source_step.str() + ".missing.step";
        TempFile         step(".step");
        StepExportReport report;
        REQUIRE(store_step(step.str(), model, {}, report));
        CHECK(report.exact_parts == 0);
        CHECK(report.mesh_parts == 1);
        const BRep::ShapeInfo info = info_of_all(reread(step.str()));
        CHECK(info.solids == 1);
        CHECK_THAT(info.volume, WithinRel(8. * double(its_volume(object->volumes.front()->mesh().its)), 1e-5));
    }
}

TEST_CASE("STEP import still tessellates into one volume per solid", "[StepExport][StepImport]")
{
    // Pins load_step's behaviour (unchanged by the exporter): an assembly of two solids comes
    // in as one object with two named volumes, each a closed mesh of the right size.
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0., 0., 0.), 20., 10., 5.).Shape();
    TempFile           box_step(".step");
    write_occt_step(box, box_step.str());

    Model        model;
    ModelObject *object = import_step(model, box_step.str());
    REQUIRE(object->volumes.size() == 1);
    const TriangleMesh &mesh = object->volumes.front()->mesh();
    CHECK(mesh.facets_count() == 12);
    CHECK_THAT(double(its_volume(mesh.its)), WithinRel(1000., 1e-6));
    CHECK(object->volumes.front()->source.input_file == box_step.str());
    // The importer centres the mesh and remembers the shift.
    CHECK_THAT(object->volumes.front()->source.mesh_offset.x(), WithinAbs(10., 1e-4));
    CHECK_THAT(object->volumes.front()->source.mesh_offset.y(), WithinAbs(5., 1e-4));
    CHECK_THAT(object->volumes.front()->source.mesh_offset.z(), WithinAbs(2.5, 1e-4));

    const TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(10., 30.).Shape();
    TempFile           cyl_step(".step");
    write_occt_step(cyl, cyl_step.str());
    Model        cyl_model;
    ModelObject *cyl_object = import_step(cyl_model, cyl_step.str());
    REQUIRE(cyl_object->volumes.size() == 1);
    const TriangleMesh &cyl_mesh = cyl_object->volumes.front()->mesh();
    CHECK(cyl_mesh.stats().open_edges == 0);
    CHECK_THAT(double(its_volume(cyl_mesh.its)), WithinRel(PI * 100. * 30., 2e-3));
    const Vec3d size = cyl_mesh.bounding_box().size();
    CHECK_THAT(size.z(), WithinAbs(30., 1e-4));
    CHECK_THAT(size.x(), WithinAbs(20., 0.01));
}

// ------------------------------------------------------------------------------------------
// CAD round trip (the FreeCAD bridge): a part goes out in its own mesh coordinates, comes back
// as STEP and replaces the mesh while the part and the object keep their placement.
// ------------------------------------------------------------------------------------------

namespace {

// An asymmetric part (a plate with a peg off one corner, not touching it), so a rotation or a
// mirror applied in the wrong frame changes the world bounding box.
TriangleMesh asymmetric_part()
{
    indexed_triangle_set its = its_make_cube(30., 12., 5.);
    TriangleMesh         peg = make_cube(4., 4., 8.);
    peg.translate(25.f, 7.f, 6.f);
    its_merge(its, peg.its);
    return TriangleMesh(its);
}

// An off-centre, rotated, non-uniformly scaled instance of a part that is itself moved and rotated
// inside its object, resting on the bed.
ModelObject *placed_object(Model &model, TriangleMesh &&mesh, const std::string &name)
{
    ModelObject *object = model.add_object();
    object->name        = name;
    ModelVolume *volume = object->add_volume(std::move(mesh));
    volume->name        = name;
    volume->set_offset(volume->get_offset() + Vec3d(5., -3., 2.));
    volume->set_rotation(Vec3d(0.1, 0., 0.4));
    ModelInstance *instance = object->add_instance();
    instance->set_offset(Vec3d(123.4, -56.7, 8.));
    instance->set_rotation(Vec3d(0.3, -0.2, 1.1));
    instance->set_scaling_factor(Vec3d(1.5, 0.8, 2.));
    object->ensure_on_bed();
    return object;
}

} // namespace

TEST_CASE("A part sent to a CAD program and back keeps its geometry and placement", "[StepExport][CadBridge]")
{
    Model        model;
    ModelObject *object = placed_object(model, asymmetric_part(), "bracket");
    ModelVolume *volume = object->volumes.front();
    volume->config.set_key_value("wall_loops", new ConfigOptionInt(5));
    const Transform3d  volume_matrix   = volume->get_matrix();
    const Transform3d  instance_matrix = object->instances.front()->get_matrix();
    const TriangleMesh world_before    = object->volume_mesh_in_world(0, 0);
    const ObjectID     id_before       = volume->id();

    // Out: the part on its own, in its mesh coordinates (a mesh part becomes planar faces).
    TempFile         out(".step");
    StepExportReport report;
    REQUIRE(store_step_part(out.str(), *volume, {}, report));
    CHECK(report.error.empty());
    CHECK(report.mesh_parts == 1);
    CHECK(report.exact_parts == 0);
    const BRep::ShapeInfo sent = info_of_all(reread(out.str()));
    CHECK(sent.solids == 2);
    require_bbox(sent.bbox, volume->mesh().bounding_box(), 1e-4);

    SECTION("unchanged, it lands exactly where it was")
    {
        TriangleMesh back;
        std::string  error;
        REQUIRE(load_step_mesh(out.str().c_str(), back, 0.003, 0.5, &error));
        INFO(error);
        require_bbox(back.bounding_box(), volume->mesh().bounding_box(), 1e-4);
        CHECK_THAT(double(its_volume(back.its)), WithinRel(double(its_volume(volume->mesh().its)), 1e-5));

        CHECK_FALSE(replace_part_mesh(*volume, std::move(back)));
        CHECK(volume->id() != id_before);
        CHECK(volume->get_matrix().isApprox(volume_matrix));
        CHECK(object->instances.front()->get_matrix().isApprox(instance_matrix));
        CHECK(volume->name == "bracket");
        CHECK(volume->config.opt_int("wall_loops") == 5);
        require_bbox(object->volume_mesh_in_world(0, 0).bounding_box(), world_before.bounding_box(), 1e-3);
    }
    SECTION("edited in the CAD program (a hole through the plate), it replaces the part in place")
    {
        // What FreeCAD does to the shape, in the same frame: drill through the plate's middle.
        const std::vector<NamedSolid> shapes = reread(out.str());
        REQUIRE(shapes.size() == 1);
        const BoundingBoxf3 bb   = volume->mesh().bounding_box();
        const Vec3d         c    = bb.center();
        const TopoDS_Shape  hole = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(c.x() - 8., c.y() - 2., bb.min.z() - 1.), gp_Dir(0., 0., 1.)), 2., 7.).Shape();
        BRepAlgoAPI_Cut     cut(shapes.front().solid, hole);
        REQUIRE(cut.IsDone());
        TempFile edited(".step");
        write_occt_step(cut.Shape(), edited.str());

        TriangleMesh back;
        REQUIRE(load_step_mesh(edited.str().c_str(), back, 0.003, 0.5));
        const double volume_before = double(its_volume(volume->mesh().its));
        const double drilled       = double(its_volume(back.its));
        CHECK_THAT(drilled, WithinRel(volume_before - PI * 4. * 5., 2e-3));

        // Painted data is per triangle of the old mesh: dropped, and reported.
        volume->seam_facets.set_triangle_from_string(0, "4");
        REQUIRE_FALSE(volume->seam_facets.empty());
        CHECK(replace_part_mesh(*volume, std::move(back)));
        CHECK(volume->seam_facets.empty());

        CHECK(volume->get_matrix().isApprox(volume_matrix));
        CHECK(object->instances.front()->get_matrix().isApprox(instance_matrix));
        CHECK(volume->config.opt_int("wall_loops") == 5);
        // The hole is inside: the outside, and so the world placement, is unchanged.
        require_bbox(object->volume_mesh_in_world(0, 0).bounding_box(), world_before.bounding_box(), 1e-3);
    }
}

TEST_CASE("A transformed part survives a world STEP export and re-import", "[StepExport][CadBridge]")
{
    Model               model;
    ModelObject        *object = placed_object(model, asymmetric_part(), "bracket");
    const BoundingBoxf3 world  = object->volume_mesh_in_world(0, 0).bounding_box();

    TempFile         step(".step");
    StepExportReport report;
    REQUIRE(store_step(step.str(), model, {}, report));
    TriangleMesh back;
    REQUIRE(load_step_mesh(step.str().c_str(), back, 0.003, 0.5));
    require_bbox(back.bounding_box(), world, 1e-3);
    // The volume scales with the determinant of the instance's scale (1.5 * 0.8 * 2).
    CHECK_THAT(double(its_volume(back.its)), WithinRel(2.4 * double(its_volume(object->volumes.front()->mesh().its)), 1e-4));
}

TEST_CASE("An unedited STEP import goes to the CAD program exactly, in its mesh frame", "[StepExport][CadBridge]")
{
    const TopoDS_Shape cylinder = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(40., 25., 3.), gp_Dir(0., 0., 1.)), 10., 20.).Shape();
    const TopoDS_Shape notch    = BRepPrimAPI_MakeBox(gp_Pnt(47., 20., 13.), 10., 10., 20.).Shape();
    BRepAlgoAPI_Cut    cut(cylinder, notch);
    REQUIRE(cut.IsDone());
    const BRep::ShapeInfo source_info = BRep::shape_info(cut.Shape(), true);
    TempFile              source(".step");
    write_occt_step(cut.Shape(), source.str());

    Model        model;
    ModelObject *object = import_step(model, source.str());
    REQUIRE(object->volumes.size() == 1);
    // As the GUI does after loading, then placed by the user.
    object->center_around_origin();
    object->instances.front()->set_offset(Vec3d(-30., 70., 0.));
    object->instances.front()->set_rotation(Vec3d(0., 0.5, 2.));
    object->instances.front()->set_scaling_factor(Vec3d(1.2, 1.2, 1.2));
    object->ensure_on_bed();
    ModelVolume        *volume = object->volumes.front();
    const BoundingBoxf3 world  = object->volume_mesh_in_world(0, 0).bounding_box();

    TempFile         out(".step");
    StepExportReport report;
    REQUIRE(store_step_part(out.str(), *volume, {}, report, true));
    CHECK(report.exact_parts == 1);
    const BRep::ShapeInfo sent = info_of_all(reread(out.str()));
    CHECK(sent.faces == source_info.faces); // the B-rep, not its triangles
    CHECK_THAT(sent.volume, WithinRel(source_info.volume, 1e-6));
    // In the part's mesh frame: around the mesh, not where the source file had it.
    require_bbox(sent.bbox, volume->mesh().bounding_box(), 0.05);

    TriangleMesh back;
    REQUIRE(load_step_mesh(out.str().c_str(), back, 0.003, 0.5));
    CHECK_FALSE(replace_part_mesh(*volume, std::move(back)));
    require_bbox(object->volume_mesh_in_world(0, 0).bounding_box(), world, 0.01);

    SECTION("a mesh part has no exact B-rep; exact_only says why and writes nothing")
    {
        Model        other;
        ModelObject *plain = placed_object(other, asymmetric_part(), "plain");
        TempFile     nothing(".step");
        REQUIRE_FALSE(store_step_part(nothing.str(), *plain->volumes.front(), {}, report, true));
        CHECK_FALSE(report.error.empty());
        CHECK_FALSE(boost::filesystem::exists(nothing.path));
    }
}
