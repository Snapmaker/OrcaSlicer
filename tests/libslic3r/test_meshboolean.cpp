#include <catch2/catch.hpp>
#include <test_utils.hpp>

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/Model.hpp>

#include <map>
#include <vector>

using namespace Slic3r;

TEST_CASE("CGAL and TriangleMesh conversions", "[MeshBoolean]") {
    TriangleMesh sphere = make_sphere(1.);
    
    auto cgalmesh_ptr = MeshBoolean::cgal::triangle_mesh_to_cgal(sphere);
    
    REQUIRE(cgalmesh_ptr);
    REQUIRE(! MeshBoolean::cgal::does_self_intersect(*cgalmesh_ptr));
    
    TriangleMesh M = MeshBoolean::cgal::cgal_to_triangle_mesh(*cgalmesh_ptr);
    
    REQUIRE(M.its.vertices.size() == sphere.its.vertices.size());
    REQUIRE(M.its.indices.size() == sphere.its.indices.size());
    
    REQUIRE(M.volume() == Approx(sphere.volume()));
    
    REQUIRE(! MeshBoolean::cgal::does_self_intersect(M));
}

// ----------------------------------------------------------------------------
// Ultra: the Mesh Boolean gizmo's Union produced correct geometry with INVERTED
// normals, while the right-click Mesh boolean on the same parts was fine.
//
// Cause: Manifold validates topology, not orientation. A closed mesh wound the wrong
// way is a valid 2-manifold to it, so it is accepted, carried through the boolean
// unchanged and returned inside-out. The gizmo fed it such a mesh because it
// transformed each volume with TriangleMesh::transform(trafo) - fix_left_handed
// defaulting to FALSE - so a mirrored (negative-determinant) part arrived inverted.
// The right-click path passes true and so never did.
//
// mfd::make_boolean() now normalises both inputs (and re-checks its output) to
// outward-facing, so both paths agree regardless of what the caller hands it.
// ----------------------------------------------------------------------------

// Signed volume in double. Positive = outward-facing normals. Deliberately computed
// here rather than via its_volume() so the assertion does not share an implementation
// with the code under test.
static double signed_volume(const indexed_triangle_set &its)
{
    double v = 0.;
    for (const Vec3i32 &f : its.indices) {
        const Vec3d a = its.vertices[f(0)].cast<double>();
        const Vec3d b = its.vertices[f(1)].cast<double>();
        const Vec3d c = its.vertices[f(2)].cast<double>();
        v += a.dot(b.cross(c));
    }
    return v / 6.;
}

// Every undirected edge used by exactly two faces.
static bool is_closed(const indexed_triangle_set &its)
{
    std::map<std::pair<int, int>, int> uses;
    for (const Vec3i32 &f : its.indices)
        for (int e = 0; e < 3; ++e) {
            int a = f(e), b = f((e + 1) % 3);
            ++uses[{std::min(a, b), std::max(a, b)}];
        }
    for (const auto &kv : uses)
        if (kv.second != 2)
            return false;
    return !its.indices.empty();
}

TEST_CASE("Mesh boolean: mfd union of two overlapping cubes faces outward", "[MeshBoolean]")
{
    // Two 10 mm cubes overlapping by 5 mm on X. its_make_cube() builds from the
    // origin, so translating the second by 5 gives a 15 x 10 x 10 union.
    TriangleMesh a{its_make_cube(10., 10., 10.)};
    TriangleMesh b{its_make_cube(10., 10., 10.)};
    b.translate(5.f, 0.f, 0.f);

    std::vector<TriangleMesh> out;
    REQUIRE(MeshBoolean::mfd::make_boolean(a, b, out, "UNION"));
    REQUIRE(out.size() == 1);

    const indexed_triangle_set &r = out.front().its;
    REQUIRE(!r.indices.empty());
    // The regression: this was NEGATIVE before the fix - correct geometry, inverted
    // normals, which renders inside-out.
    CHECK(signed_volume(r) > 0.);
    CHECK(is_closed(r));
    // And it really is the union, not one of the operands.
    CHECK(signed_volume(r) == Approx(1500.).epsilon(0.02));
}

TEST_CASE("Mesh boolean: mfd union with a mirrored operand faces outward", "[MeshBoolean]")
{
    // The other half of the bug: a MIRRORED volume. The gizmo transforms by the
    // volume matrix, and a negative-determinant matrix inverts the winding. Feeding
    // that mesh in unmirrored is what the gizmo did.
    TriangleMesh a{its_make_cube(10., 10., 10.)};

    TriangleMesh b{its_make_cube(10., 10., 10.)};
    // Mirror in X about x = 5, then shift so it still overlaps `a`: determinant -1.
    Transform3d m = Transform3d::Identity();
    m.scale(Vec3d(-1., 1., 1.));
    m.pretranslate(Vec3d(15., 0., 0.));
    REQUIRE(m.matrix().block(0, 0, 3, 3).determinant() < 0.);
    // fix_left_handed deliberately FALSE - this reproduces exactly what the gizmo
    // handed the backend. The backend must cope on its own.
    b.transform(m, false);
    REQUIRE(signed_volume(b.its) < 0.);   // the operand really is inside-out

    std::vector<TriangleMesh> out;
    REQUIRE(MeshBoolean::mfd::make_boolean(a, b, out, "UNION"));
    REQUIRE(out.size() == 1);

    const indexed_triangle_set &r = out.front().its;
    REQUIRE(!r.indices.empty());
    CHECK(signed_volume(r) > 0.);
    CHECK(is_closed(r));
    // Mirrored cube spans x in [5, 15], so the union is again 15 x 10 x 10.
    CHECK(signed_volume(r) == Approx(1500.).epsilon(0.02));
}

// ----------------------------------------------------------------------------
// Mesh Boolean gizmo: the result part jumped (up, down, left, right) after Union /
// Difference / Intersection.
//
// The gizmo booleans the two parts in OBJECT coordinates (each mesh transformed by its
// own ModelVolume::get_matrix()), then hands the result to
// ModelObject::replace_volume_with_object_mesh(). add_volume() centres that mesh and
// gives the volume the matching offset, which is correct - but the gizmo then
// overwrote the offset with the SOURCE part's offset, moving the result by
// (source offset - result bbox centre). That vector depends on where the tool part
// is relative to the source, hence "no obvious pattern".
//
// These tests run the gizmo's exact sequence and require the result to sit where
// the boolean geometry is, and every other part not to move.
// ----------------------------------------------------------------------------

namespace {

Transform3d make_trafo(const Vec3d &offset, const Vec3d &axis, double angle_deg, const Vec3d &scale)
{
    Transform3d t = Transform3d::Identity();
    t.translate(offset);
    t.rotate(Eigen::AngleAxisd(angle_deg * PI / 180., axis.normalized()));
    t.scale(scale);
    return t;
}

BoundingBoxf3 world_bbox(const ModelObject &mo, const ModelVolume &mv)
{
    return mv.mesh().transformed_bounding_box(mo.instances.front()->get_matrix() * mv.get_matrix());
}

void check_same_box(const BoundingBoxf3 &got, const BoundingBoxf3 &expected, double tol = 1e-3)
{
    INFO("got min " << got.min.transpose() << " max " << got.max.transpose());
    INFO("expected min " << expected.min.transpose() << " max " << expected.max.transpose());
    CHECK((got.min - expected.min).norm() < tol);
    CHECK((got.max - expected.max).norm() < tol);
}

// Object with a rotated + scaled + offset instance, and up to three parts:
//   [0] A - rotated, non-uniformly scaled (optionally mirrored), off-centre
//   [1] B - overlaps A, rotated differently
//   [2] C - well away from both, never touched (only when with_bystander)
ModelObject *make_boolean_scene(Model &model, bool mirror_a, bool with_bystander)
{
    ModelObject *mo = model.add_object();
    mo->name = "boolean scene";
    ModelInstance *inst = mo->add_instance();
    inst->set_transformation(Geometry::Transformation(
        make_trafo(Vec3d(92., 117., 6.), Vec3d(0., 0., 1.), 25., Vec3d(1.1, 1.1, 1.1))));

    ModelVolume *a = mo->add_volume(TriangleMesh(its_make_cube(20., 20., 20.)));
    a->name = "A";
    a->set_transformation(Geometry::Transformation(
        make_trafo(Vec3d(-12., 5., 9.), Vec3d(1., 0., 2.), 35., Vec3d(mirror_a ? -1.2 : 1.2, 0.8, 1.0))));

    ModelVolume *b = mo->add_volume(TriangleMesh(its_make_cube(15., 15., 15.)));
    b->name = "B";
    b->set_transformation(Geometry::Transformation(
        make_trafo(Vec3d(-1., 11., 16.), Vec3d(0., 1., 0.), 15., Vec3d(1., 1., 1.))));

    if (with_bystander) {
        ModelVolume *c = mo->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
        c->name = "C";
        c->set_transformation(Geometry::Transformation(
            make_trafo(Vec3d(45., -30., 0.), Vec3d(0., 0., 1.), 10., Vec3d(1., 1., 1.))));
    }
    return mo;
}

// The gizmo's apply path, minus the GUI: boolean in object coordinates, replace the
// source volume, optionally delete the tool. Returns the new volume and fills
// `expected` with the world bbox of the boolean geometry.
ModelVolume *run_gizmo_boolean(ModelObject *mo, size_t src_idx, size_t tool_idx, const std::string &op,
                               const std::string &suffix, bool delete_tool, BoundingBoxf3 &expected, double &expected_volume)
{
    TriangleMesh src = mo->volumes[src_idx]->mesh();
    src.transform(mo->volumes[src_idx]->get_matrix(), true);
    TriangleMesh tool = mo->volumes[tool_idx]->mesh();
    tool.transform(mo->volumes[tool_idx]->get_matrix(), true);

    std::vector<TriangleMesh> out;
    REQUIRE(MeshBoolean::mfd::make_boolean(src, tool, out, op));
    REQUIRE(!out.empty());
    REQUIRE(!out.front().empty());

    expected        = out.front().transformed_bounding_box(mo->instances.front()->get_matrix());
    expected_volume = std::abs(out.front().volume());

    ModelVolume *nv = mo->replace_volume_with_object_mesh(src_idx, TriangleMesh(out.front()), suffix);
    if (delete_tool)
        mo->delete_volume(tool_idx);
    return nv;
}

} // namespace

TEST_CASE("Mesh Boolean gizmo: the result stays where the boolean geometry is", "[MeshBoolean]")
{
    struct Case { const char *op; const char *suffix; bool delete_tool; };
    const Case cases[] = {
        {"UNION",        "union",        true },
        {"A_NOT_B",      "difference",   false},
        {"A_NOT_B",      "difference",   true },
        {"INTERSECTION", "intersection", false},
        {"INTERSECTION", "intersection", true },
    };

    for (bool mirror_a : {false, true})
        for (const Case &c : cases) {
            DYNAMIC_SECTION(c.op << (c.delete_tool ? " delete tool" : " keep tool") << (mirror_a ? " mirrored source" : "")) {
                Model        model;
                ModelObject *mo = make_boolean_scene(model, mirror_a, true);
                const BoundingBoxf3 tool_before      = world_bbox(*mo, *mo->volumes[1]);
                const BoundingBoxf3 bystander_before = world_bbox(*mo, *mo->volumes[2]);

                BoundingBoxf3 expected;
                double        expected_volume = 0.;
                ModelVolume  *nv = run_gizmo_boolean(mo, 0, 1, c.op, c.suffix, c.delete_tool, expected, expected_volume);

                // The result replaced the source in slot 0.
                REQUIRE(mo->volumes.front() == nv);
                CHECK(nv->name == std::string("A - ") + c.suffix);
                // Stored centred, like every other volume.
                CHECK(nv->mesh().bounding_box().center().norm() < 1e-3);
                CHECK(std::abs(nv->mesh().volume()) * std::abs(nv->get_matrix().matrix().block<3, 3>(0, 0).determinant()) ==
                      Approx(expected_volume).epsilon(1e-4));

                // The regression: the result moved by (source offset - result centre).
                check_same_box(world_bbox(*mo, *nv), expected);

                // Untouched parts do not move.
                for (const ModelVolume *v : mo->volumes) {
                    if (v->name == "C")
                        check_same_box(world_bbox(*mo, *v), bystander_before);
                    else if (v->name == "B")
                        check_same_box(world_bbox(*mo, *v), tool_before);
                }
                CHECK(mo->volumes.size() == (c.delete_tool ? 2u : 3u));
            }
        }
}

TEST_CASE("Mesh Boolean gizmo: union of a two-part object, the result is the only part left", "[MeshBoolean]")
{
    // Deleting the tool leaves one volume, and ModelObject::delete_volume() then folds
    // the volume transform into the instance. The world position must survive that too.
    for (bool mirror_a : {false, true}) {
        Model        model;
        ModelObject *mo = make_boolean_scene(model, mirror_a, false);
        BoundingBoxf3 expected;
        double        expected_volume = 0.;
        ModelVolume  *nv = run_gizmo_boolean(mo, 0, 1, "UNION", "union", true, expected, expected_volume);
        REQUIRE(mo->volumes.size() == 1);
        REQUIRE(mo->volumes.front() == nv);
        check_same_box(world_bbox(*mo, *nv), expected);
    }
}

TEST_CASE("Mesh Boolean gizmo: the second part as the source", "[MeshBoolean]")
{
    // Source in a later slot than the tool: the result takes the source's slot and the
    // other parts keep theirs.
    Model        model;
    ModelObject *mo = make_boolean_scene(model, false, true);
    const BoundingBoxf3 a_before = world_bbox(*mo, *mo->volumes[0]);
    const BoundingBoxf3 c_before = world_bbox(*mo, *mo->volumes[2]);
    BoundingBoxf3 expected;
    double        expected_volume = 0.;
    ModelVolume  *nv = run_gizmo_boolean(mo, 1, 0, "A_NOT_B", "difference", false, expected, expected_volume);
    REQUIRE(mo->volumes[1] == nv);
    CHECK(mo->volumes[0]->name == "A");
    CHECK(mo->volumes[2]->name == "C");
    check_same_box(world_bbox(*mo, *nv), expected);
    check_same_box(world_bbox(*mo, *mo->volumes[0]), a_before);
    check_same_box(world_bbox(*mo, *mo->volumes[2]), c_before);
}
