#include <catch2/catch.hpp>

#include "libslic3r/BRep/CadBody.hpp"
#include "libslic3r/BRep/CadEdit.hpp"
#include "libslic3r/BRep/CadShape.hpp"
#include "libslic3r/BRep/MeshToBRep.hpp"
#include "libslic3r/Format/STEP.hpp"
#include "libslic3r/Format/STEPExport.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRep_Tool.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_Writer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>

#include <boost/filesystem.hpp>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

struct TempFile
{
    boost::filesystem::path path;
    explicit TempFile(const char *ext)
        : path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path(std::string("edge_cad_%%%%-%%%%-%%%%") + ext))
    {}
    ~TempFile()
    {
        boost::system::error_code ec;
        boost::filesystem::remove(path, ec);
    }
    std::string str() const { return path.string(); }
};

void write_occt_step(const TopoDS_Shape &shape, const std::string &path)
{
    STEPControl_Writer writer;
    REQUIRE(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone);
    REQUIRE(writer.Write(path.c_str()) == IFSelect_RetDone);
}

ModelObject *import_step(Model &model, const std::string &path)
{
    bool cancelled = false;
    REQUIRE(load_step(path.c_str(), &model, cancelled));
    REQUIRE(!model.objects.empty());
    ModelObject *object = model.objects.back();
    object->add_instance();
    return object;
}

BRep::ShapeInfo body_info(const BRep::CadBody &body) { return BRep::shape_info(BRep::cad_body_shape(body), true); }

// Edges whose every polyline point lies on the plane z = `z`.
std::vector<int> edges_at_z(const BRep::CadTopology &topo, double z)
{
    std::vector<int> out;
    for (int e = 0; e < topo.num_edges; ++e) {
        const auto &pl = topo.edge_polylines[size_t(e)];
        if (!pl.empty() && std::all_of(pl.begin(), pl.end(), [z](const Vec3f &p) { return std::abs(double(p.z()) - z) < 1e-4; }))
            out.push_back(e);
    }
    return out;
}

std::vector<int> all_selectable(const BRep::CadTopology &topo)
{
    std::vector<int> out;
    for (int e = 0; e < topo.num_edges; ++e)
        if (topo.edge_selectable[size_t(e)])
            out.push_back(e);
    return out;
}

// Volume of an a x a x a cube with every edge rounded to radius r.
double rounded_cube_volume(double a, double r)
{
    const double c = a - 2. * r;
    return c * c * c + 6. * r * c * c + 3. * PI * r * r * c + 4. / 3. * PI * r * r * r;
}

std::shared_ptr<const BRep::CadBody> body_of(const TopoDS_Shape &shape, BRep::CadBodyOrigin origin = BRep::CadBodyOrigin::StepFile)
{
    const indexed_triangle_set mesh = BRep::tessellate_cad_shape(shape, 0.003, 0.5);
    return BRep::make_cad_body(shape, mesh, origin, 0);
}

} // namespace

TEST_CASE("CAD body of a STEP-imported cube takes an exact fillet, stacks and exports exactly", "[CadEdit]")
{
    TempFile source(".step");
    write_occt_step(BRepPrimAPI_MakeBox(gp_Pnt(0., 0., 0.), 20., 20., 20.).Shape(), source.str());
    Model        model;
    ModelObject *object = import_step(model, source.str());
    REQUIRE(object->volumes.size() == 1);
    ModelVolume *volume = object->volumes.front();
    object->instances.front()->set_offset(Vec3d(50., 40., 10.));

    // Nothing attached yet: the body comes from the source file.
    CHECK(BRep::attached_cad_body(*volume) == nullptr);
    std::string why;
    const auto  body = BRep::cad_body_from_step_source(*volume, &why);
    INFO(why);
    REQUIRE(body);
    CHECK(body->origin == BRep::CadBodyOrigin::StepFile);

    const BRep::CadTopology topo = BRep::cad_topology(*body, 0.05, 0.3);
    CHECK(topo.num_faces == 6);
    CHECK(topo.num_edges == 12);
    CHECK(all_selectable(topo).size() == 12);
    CHECK(std::count(topo.face_planar.begin(), topo.face_planar.end(), 1) == 6);
    // In the volume's mesh frame: the importer centred the cube.
    CHECK_THAT(topo.bbox.min.z(), WithinAbs(-10., 1e-4));
    CHECK_THAT(topo.bbox.max.z(), WithinAbs(10., 1e-4));
    // Every triangle knows its face; a face's selectable edges are its four.
    CHECK(topo.triangle_face.size() == topo.mesh.indices.size());
    CHECK(BRep::face_selectable_edges(topo, 0).size() == 4);

    SECTION("fillet every edge, apply, export")
    {
        const size_t facets_before = volume->mesh().facets_count();
        const BRep::CadOpResult r = BRep::fillet_edges(*body, BRep::EdgeFeature::Fillet, 2., all_selectable(topo));
        INFO(r.error);
        REQUIRE(r.ok());
        CHECK(r.faces_before == 6);
        CHECK(r.faces_after == 6 + 12 + 8);
        CHECK_THAT(r.volume_before, WithinRel(8000., 1e-9));
        CHECK_THAT(r.volume_after, WithinRel(rounded_cube_volume(20., 2.), 1e-6));
        REQUIRE(r.body);
        CHECK(r.body->operations == 1);
        CHECK(body_info(*r.body).valid);

        // The tessellation is closed, as fine as STEP import's, and the size did not change.
        const TriangleMesh mesh(r.mesh);
        CHECK(mesh.stats().open_edges == 0);
        CHECK_THAT(double(its_volume(r.mesh)), WithinRel(rounded_cube_volume(20., 2.), 2e-3));
        const Vec3d size = mesh.bounding_box().size();
        CHECK_THAT(size.x(), WithinAbs(20., 1e-3));
        CHECK_THAT(size.z(), WithinAbs(20., 1e-3));

        const Transform3d volume_matrix = volume->get_matrix();
        const std::string name          = volume->name;
        CHECK_FALSE(BRep::apply_cad_result(*volume, r)); // nothing painted
        CHECK(volume->mesh().facets_count() > facets_before);
        CHECK(volume->get_matrix().isApprox(volume_matrix));
        CHECK(volume->name == name);
        CHECK(volume->source.input_file == source.str());

        // The body travels with the volume now; STEP export writes it, exactly.
        const auto attached = BRep::attached_cad_body(*volume);
        REQUIRE(attached);
        CHECK(body_info(*attached).faces == 26);

        TempFile               out(".step");
        StepExportReport       report;
        REQUIRE(store_step(out.str(), model, {}, report));
        CHECK(report.exact_parts == 1);
        CHECK(report.mesh_parts == 0);
        std::vector<NamedSolid> plain, split;
        REQUIRE(read_step_named_shapes(out.str().c_str(), plain, split));
        REQUIRE(plain.size() == 1);
        const BRep::ShapeInfo exported = BRep::shape_info(plain.front().solid, true);
        CHECK(exported.valid);
        CHECK(exported.faces == 26);
        CHECK_THAT(exported.volume, WithinRel(rounded_cube_volume(20., 2.), 1e-6));
        // Placed where the part is shown.
        const BoundingBoxf3 shown = object->volume_mesh_in_world(0, 0).bounding_box();
        CHECK_THAT(exported.bbox.min.x(), WithinAbs(shown.min.x(), 1e-3));
        CHECK_THAT(exported.bbox.max.y(), WithinAbs(shown.max.y(), 1e-3));
        CHECK_THAT(exported.bbox.max.z(), WithinAbs(shown.max.z(), 1e-3));
    }

    SECTION("operations stack: fillet the top, then chamfer the bottom of the result")
    {
        const std::vector<int> top = edges_at_z(topo, 10.);
        REQUIRE(top.size() == 4);
        const BRep::CadOpResult r1 = BRep::fillet_edges(*body, BRep::EdgeFeature::Fillet, 3., top);
        INFO(r1.error);
        REQUIRE(r1.ok());
        BRep::apply_cad_result(*volume, r1);

        const auto body1 = BRep::attached_cad_body(*volume);
        REQUIRE(body1);
        const BRep::CadTopology topo1 = BRep::cad_topology(*body1, 0.05, 0.3);
        // The fillets' boundaries are tangent joins: not offered for picking.
        CHECK(all_selectable(topo1).size() < size_t(topo1.num_edges));
        const std::vector<int> bottom = edges_at_z(topo1, -10.);
        REQUIRE(bottom.size() == 4);
        const BRep::CadOpResult r2 = BRep::fillet_edges(*body1, BRep::EdgeFeature::Chamfer, 1., bottom);
        INFO(r2.error);
        REQUIRE(r2.ok());
        CHECK(r2.body->operations == 2);
        CHECK(r2.faces_after == r2.faces_before + 4);
        // Four 45 degree chamfers of 1 mm along 20 mm edges, overlapping at the corners.
        CHECK(r2.volume_after < r2.volume_before);
        CHECK(r2.volume_after > r2.volume_before - 4. * 0.5 * 20.);
        BRep::apply_cad_result(*volume, r2);
        const auto body2 = BRep::attached_cad_body(*volume);
        REQUIRE(body2);
        CHECK(body_info(*body2).valid);
    }

    SECTION("a radius too large fails with a reason and changes nothing")
    {
        const std::string          brep_before = body->brep;
        const indexed_triangle_set mesh_before = volume->mesh().its;
        const BRep::CadOpResult    r           = BRep::fillet_edges(*body, BRep::EdgeFeature::Fillet, 15., all_selectable(topo));
        CHECK_FALSE(r.ok());
        CHECK_FALSE(r.error.empty());
        CHECK(r.body == nullptr);
        CHECK(body->brep == brep_before);
        CHECK_FALSE(BRep::apply_cad_result(*volume, r));
        CHECK(volume->mesh().its.vertices == mesh_before.vertices);
        CHECK(volume->mesh().its.indices == mesh_before.indices);
        CHECK(volume->cad_body == nullptr);
    }

    SECTION("bad input is refused before OCCT sees it")
    {
        CHECK(BRep::fillet_edges(*body, BRep::EdgeFeature::Fillet, 1., {}).status == BRep::CadOpStatus::NothingSelected);
        CHECK(BRep::fillet_edges(*body, BRep::EdgeFeature::Fillet, 0., {0}).status == BRep::CadOpStatus::BadSize);
        CHECK(BRep::fillet_edges(*body, BRep::EdgeFeature::Chamfer, 1., {99}).status == BRep::CadOpStatus::BadIndex);
    }
}

TEST_CASE("CAD body converted from a mesh cube takes an exact chamfer", "[CadEdit]")
{
    const indexed_triangle_set   cube = its_make_cube(20., 20., 20.);
    BRep::MeshConversionReport   report;
    const auto                   body = BRep::cad_body_from_mesh(cube, report);
    INFO(report.error);
    REQUIRE(body);
    CHECK(body->origin == BRep::CadBodyOrigin::ConvertedMesh);
    CHECK(report.triangles == 12);
    CHECK(report.faces == 6);
    CHECK(report.edges == 12);

    const BRep::CadTopology topo = BRep::cad_topology(*body, 0.05, 0.3);
    REQUIRE(all_selectable(topo).size() == 12);

    SECTION("chamfer")
    {
        const BRep::CadOpResult r = BRep::fillet_edges(*body, BRep::EdgeFeature::Chamfer, 1., all_selectable(topo));
        INFO(r.error);
        REQUIRE(r.ok());
        CHECK(r.faces_after == 6 + 12 + 8);
        // Each edge loses a 0.5 mm^2 prism, minus what the corners share; the corners lose more.
        CHECK(r.volume_after < 8000. - 12. * 0.5 * 18.);
        CHECK(r.volume_after > 8000. - 12. * 0.5 * 20. - 8.);
        CHECK(body_info(*r.body).valid);
        CHECK(TriangleMesh(r.mesh).stats().open_edges == 0);
    }
    SECTION("fillet")
    {
        const BRep::CadOpResult r = BRep::fillet_edges(*body, BRep::EdgeFeature::Fillet, 2., all_selectable(topo));
        INFO(r.error);
        REQUIRE(r.ok());
        CHECK_THAT(r.volume_after, WithinRel(rounded_cube_volume(20., 2.), 1e-6));
        CHECK(TriangleMesh(r.mesh).stats().open_edges == 0);
    }
    SECTION("the radius too large fails")
    {
        const BRep::CadOpResult r = BRep::fillet_edges(*body, BRep::EdgeFeature::Fillet, 12., all_selectable(topo));
        CHECK_FALSE(r.ok());
        CHECK_FALSE(r.error.empty());
    }
}

TEST_CASE("Mesh to CAD body conversion refuses what it cannot fillet", "[CadEdit]")
{
    SECTION("an open mesh")
    {
        indexed_triangle_set cube = its_make_cube(20., 20., 20.);
        cube.indices.pop_back();
        BRep::MeshConversionReport report;
        CHECK(BRep::cad_body_from_mesh(cube, report) == nullptr);
        CHECK_FALSE(report.error.empty());
    }
    SECTION("a mesh above the triangle limit, without trying")
    {
        const indexed_triangle_set sphere = its_make_sphere(10., PI / 200.);
        REQUIRE(int(sphere.indices.size()) > BRep::ConvertMaxTriangles);
        BRep::MeshConversionReport report;
        CHECK(BRep::cad_body_from_mesh(sphere, report) == nullptr);
        CHECK_FALSE(report.error.empty());
        CHECK(report.seconds < 1.);
    }
}

TEST_CASE("An attached CAD body follows a translated mesh and drops an edited one", "[CadEdit]")
{
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(TriangleMesh(its_make_cube(20., 20., 20.)));
    object->add_instance();
    // add_volume() centred the mesh; convert what the volume has.
    BRep::MeshConversionReport report;
    auto                       body = BRep::cad_body_from_mesh(volume->mesh().its, report);
    REQUIRE(body);
    volume->cad_body = body;
    REQUIRE(BRep::attached_cad_body(*volume) == body);

    SECTION("translated in place, as center_geometry_after_creation() does")
    {
        TriangleMesh mesh = volume->mesh();
        mesh.translate(3.f, -2.f, 7.f);
        volume->set_mesh(std::move(mesh));
        const auto moved = BRep::attached_cad_body(*volume);
        REQUIRE(moved);
        CHECK(moved->shift.isApprox(Vec3d(3., -2., 7.), 1e-5));
        const BRep::ShapeInfo info = body_info(*moved);
        CHECK_THAT(info.bbox.min.x(), WithinAbs(-7., 1e-4));
        CHECK_THAT(info.bbox.max.z(), WithinAbs(17., 1e-4));
    }
    SECTION("edited")
    {
        indexed_triangle_set its = volume->mesh().its;
        its.vertices.front() += Vec3f(0.f, 0.f, 0.5f);
        volume->set_mesh(TriangleMesh(its));
        CHECK(BRep::attached_cad_body(*volume) == nullptr);
    }
    SECTION("replaced by a different mesh with the same counts")
    {
        volume->set_mesh(TriangleMesh(its_make_cube(20., 20., 21.)));
        CHECK(BRep::attached_cad_body(*volume) == nullptr);
    }
    SECTION("a copy of the volume keeps it; a new mesh does not")
    {
        ModelObject *copy = model.add_object(*object);
        CHECK(copy->volumes.front()->cad_body == body);
        ModelVolume *split = object->add_volume(*volume, TriangleMesh(its_make_cube(5., 5., 5.)));
        CHECK(split->cad_body == nullptr);
    }
}

TEST_CASE("Tangent chains and smooth joins are read from the B-rep", "[CadEdit]")
{
    // A box with ONE vertical edge rounded: the top outline is line - arc - line (tangent), the
    // other corners are sharp, and the arc's side boundaries are tangent joins.
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0., 0., 0.), 20., 20., 10.).Shape();
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(box, TopAbs_EDGE, edges);
    BRepFilletAPI_MakeFillet mk(box);
    for (int i = 1; i <= edges.Extent(); ++i) {
        TopoDS_Vertex a, b;
        TopExp::Vertices(TopoDS::Edge(edges(i)), a, b);
        const gp_Pnt pa = BRep_Tool::Pnt(a), pb = BRep_Tool::Pnt(b);
        if (std::abs(pa.X() - 20.) < 1e-9 && std::abs(pb.X() - 20.) < 1e-9 && std::abs(pa.Y() - 20.) < 1e-9 && std::abs(pb.Y() - 20.) < 1e-9)
            mk.Add(4., TopoDS::Edge(edges(i)));
    }
    mk.Build();
    REQUIRE(mk.IsDone());
    const auto              body = body_of(mk.Shape());
    const BRep::CadTopology topo = BRep::cad_topology(*body, 0.05, 0.3);
    CHECK(topo.num_faces == 7);
    // 12 box edges - 1 filleted + 2 arcs + 2 tangent side joins = 15; the 2 joins are not selectable.
    CHECK(topo.num_edges == 15);
    CHECK(all_selectable(topo).size() == 13);

    const std::vector<int> top = edges_at_z(topo, 10.);
    REQUIRE(top.size() == 5);
    // The arc: the top edge that is not straight.
    int arc = -1;
    for (int e : top)
        if (topo.edge_polylines[size_t(e)].size() > 2)
            arc = e;
    REQUIRE(arc >= 0);
    CHECK(BRep::tangent_chain(topo, arc).size() == 3);
    // A straight edge between two sharp corners is a chain of one.
    for (int e : top)
        if (e != arc && std::find(topo.edge_tangent_neighbours[size_t(arc)].begin(), topo.edge_tangent_neighbours[size_t(arc)].end(), e) ==
                            topo.edge_tangent_neighbours[size_t(arc)].end())
            CHECK(BRep::tangent_chain(topo, e).size() == 1);

    // Fillet the whole top chain in one go: OCCT blends along it.
    const BRep::CadOpResult r = BRep::fillet_edges(*body, BRep::EdgeFeature::Fillet, 1., BRep::tangent_chain(topo, arc));
    INFO(r.error);
    CHECK(r.ok());
}

TEST_CASE("Shell hollows a solid to a wall, opening the picked face", "[CadEdit]")
{
    const auto              body = body_of(BRepPrimAPI_MakeBox(gp_Pnt(0., 0., 0.), 20., 20., 20.).Shape());
    const BRep::CadTopology topo = BRep::cad_topology(*body, 0.05, 0.3);
    // The top face: the face whose edges are all at z = 20.
    int top_face = -1;
    for (int f = 0; f < topo.num_faces; ++f) {
        const std::vector<int> fe = BRep::face_selectable_edges(topo, f);
        const std::vector<int> at = edges_at_z(topo, 20.);
        if (fe.size() == 4 && std::all_of(fe.begin(), fe.end(), [&at](int e) { return std::find(at.begin(), at.end(), e) != at.end(); }))
            top_face = f;
    }
    REQUIRE(top_face >= 0);

    SECTION("a 2 mm wall")
    {
        const BRep::CadOpResult r = BRep::shell_solid(*body, {top_face}, 2.);
        INFO(r.error);
        REQUIRE(r.ok());
        CHECK_THAT(r.volume_after, WithinRel(8000. - 16. * 16. * 18., 1e-6));
        CHECK(r.faces_after == 11);
        CHECK(body_info(*r.body).valid);
        CHECK(TriangleMesh(r.mesh).stats().open_edges == 0);
    }
    SECTION("no open face, or a wall too thick, is refused")
    {
        CHECK(BRep::shell_solid(*body, {}, 2.).status == BRep::CadOpStatus::NothingSelected);
        const BRep::CadOpResult r = BRep::shell_solid(*body, {top_face}, 15.);
        CHECK_FALSE(r.ok());
        CHECK_FALSE(r.error.empty());
    }
}

TEST_CASE("CAD body blob round trip", "[CadEdit]")
{
    auto body = std::make_shared<BRep::CadBody>(*body_of(BRepPrimAPI_MakeBox(10., 20., 30.).Shape(), BRep::CadBodyOrigin::ConvertedMesh));
    body->operations = 3;
    body->shift      = Vec3d(1., 2., 3.);
    const std::string blob = body->to_blob();
    const auto        back = BRep::CadBody::from_blob(blob);
    REQUIRE(back);
    CHECK(back->brep == body->brep);
    CHECK(back->operations == 3);
    CHECK(back->origin == BRep::CadBodyOrigin::ConvertedMesh);
    CHECK(back->shift == body->shift);
    Vec3d shift;
    CHECK(back->mesh.matches(body->mesh, shift));
    CHECK(shift.isZero());
    CHECK(BRep::CadBody::from_blob(blob.substr(0, blob.size() - 1)) == nullptr);
    CHECK(BRep::CadBody::from_blob("not a body") == nullptr);
}

namespace {

// Store `model` as a project 3MF and load it back, as the application does (see test_3mf.cpp's
// support-group round trip for why the config and the temporary dir are set up this way).
bool project_round_trip(Model &model, Model &loaded)
{
    TempFile                 file(".3mf");
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
    for (const std::string &key : store_config.keys())
        if (const ConfigOption *opt = store_config.option(key); opt != nullptr && opt->type() == coEnums) {
            store_config.erase(key);
            store_config.option(key, true);
        }
    const std::string path = file.str();
    StoreParams       store_params;
    store_params.path     = path.c_str();
    store_params.model    = &model;
    store_params.config   = &store_config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    if (!store_bbs_3mf(store_params))
        return false;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::EnableSilent};
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const bool ok = load_bbs_3mf(path.c_str(), &dst_config, &ctxt, &loaded, &plate_data, &project_presets, &is_bbl_3mf, &file_version,
                                 nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence);
    release_PlateData_list(plate_data);
    return ok;
}

} // namespace

TEST_CASE("A filleted part keeps its exact CAD body through a project 3MF", "[CadEdit][3mf]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "cad_part";
    ModelVolume *volume = object->add_volume(TriangleMesh(its_make_cube(20., 20., 20.)));
    volume->name        = "cube";
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(60., 60., 10.));

    BRep::MeshConversionReport report;
    const auto                 body = BRep::cad_body_from_mesh(volume->mesh().its, report);
    REQUIRE(body);
    const BRep::CadTopology topo = BRep::cad_topology(*body, 0.05, 0.3);
    const BRep::CadOpResult r    = BRep::fillet_edges(*body, BRep::EdgeFeature::Fillet, 2., all_selectable(topo));
    INFO(r.error);
    REQUIRE(r.ok());
    BRep::apply_cad_result(*volume, r);
    REQUIRE(BRep::attached_cad_body(*volume));

    SECTION("stored and restored")
    {
        Model loaded;
        REQUIRE(project_round_trip(model, loaded));
        REQUIRE(loaded.objects.size() == 1);
        REQUIRE(loaded.objects.front()->volumes.size() == 1);
        const ModelVolume *back = loaded.objects.front()->volumes.front();
        const auto         restored = BRep::attached_cad_body(*back);
        REQUIRE(restored);
        CHECK(restored->operations == 1);
        CHECK(restored->origin == BRep::CadBodyOrigin::ConvertedMesh);
        const BRep::ShapeInfo info = body_info(*restored);
        CHECK(info.valid);
        CHECK(info.faces == 26);
        CHECK_THAT(info.volume, WithinRel(rounded_cube_volume(20., 2.), 1e-6));
        // It sits on the reloaded mesh.
        const BoundingBoxf3 mesh_box = back->mesh().bounding_box();
        CHECK_THAT(info.bbox.min.x(), WithinAbs(mesh_box.min.x(), 1e-3));
        CHECK_THAT(info.bbox.max.z(), WithinAbs(mesh_box.max.z(), 1e-3));

        // And the next operation stacks on it.
        const BRep::CadTopology topo2 = BRep::cad_topology(*restored, 0.05, 0.3);
        CHECK(topo2.num_faces == 26);
    }
    SECTION("an edited mesh stores no body")
    {
        indexed_triangle_set its = volume->mesh().its;
        its.vertices.front() += Vec3f(0.f, 0.f, 0.3f);
        volume->set_mesh(TriangleMesh(its));
        Model loaded;
        REQUIRE(project_round_trip(model, loaded));
        REQUIRE(loaded.objects.size() == 1);
        CHECK(loaded.objects.front()->volumes.front()->cad_body == nullptr);
    }
}
