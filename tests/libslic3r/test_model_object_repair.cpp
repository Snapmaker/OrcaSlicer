#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <test_utils.hpp>

#include <libslic3r/Exception.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/ModelObjectRepair.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include <stdexcept>

using namespace Slic3r;

namespace {

indexed_triangle_set cube_at(float size, const Vec3f &origin)
{
    indexed_triangle_set its = its_make_cube(size, size, size);
    for (Vec3f &v : its.vertices)
        v += origin;
    return its;
}

indexed_triangle_set open_cube_at(float size, const Vec3f &origin)
{
    indexed_triangle_set its = cube_at(size, origin);
    its.indices.pop_back();
    return its;
}

indexed_triangle_set sheet_at(const Vec3f &origin)
{
    indexed_triangle_set its;
    its.vertices = {origin, origin + Vec3f(10.f, 0.f, 0.f), origin + Vec3f(10.f, 10.f, 0.f), origin + Vec3f(0.f, 10.f, 0.f)};
    its.indices  = {{0, 1, 2}, {0, 2, 3}};
    return its;
}

indexed_triangle_set merged(indexed_triangle_set a, const indexed_triangle_set &b)
{
    its_merge(a, b);
    return a;
}

ModelObject &object_with(Model &model, std::initializer_list<indexed_triangle_set> volumes)
{
    ModelObject *object = model.add_object();
    for (const indexed_triangle_set &its : volumes)
        object->add_volume(TriangleMesh(its));
    object->add_instance();
    return *object;
}

void repair(ModelObject &object, int volume_idx = -1)
{
    repair_model_object(object, volume_idx, [](const char *, int) {}, [] {});
}

size_t open_edges(const ModelVolume &volume) { return its_num_open_edges(volume.mesh().its); }

double total_volume(const ModelObject &object)
{
    double sum = 0.;
    for (const ModelVolume *v : object.volumes)
        sum += its_volume(v->mesh().its);
    return sum;
}

BoundingBoxf3 world_bounding_box(const ModelObject &object)
{
    BoundingBoxf3  bbox;
    const Transform3d instance = object.instances.front()->get_matrix();
    for (const ModelVolume *v : object.volumes)
        bbox.merge(v->mesh().transformed_bounding_box(instance * v->get_matrix()));
    return bbox;
}

} // namespace

TEST_CASE("Repairing a whole object closes every broken volume", "[ModelObjectRepair]") {
    Model        model;
    ModelObject &object = object_with(model, {open_cube_at(10.f, Vec3f::Zero()), open_cube_at(10.f, Vec3f(20.f, 0.f, 0.f))});
    repair(object);
    REQUIRE(object.volumes.size() == 2);
    REQUIRE(open_edges(*object.volumes[0]) == 0);
    REQUIRE(open_edges(*object.volumes[1]) == 0);
    REQUIRE_THAT(total_volume(object), WithinRel(2000., 0.001));
}

TEST_CASE("Repairing one volume leaves the other volumes untouched", "[ModelObjectRepair]") {
    Model        model;
    ModelObject &object   = object_with(model, {open_cube_at(10.f, Vec3f::Zero()), open_cube_at(10.f, Vec3f(20.f, 0.f, 0.f))});
    const auto   untouched = object.volumes[1]->get_mesh_shared_ptr();
    repair(object, 0);
    REQUIRE(open_edges(*object.volumes[0]) == 0);
    REQUIRE(object.volumes[1]->get_mesh_shared_ptr() == untouched);
}

TEST_CASE("Repairing a split volume leaves the volumes after it untouched", "[ModelObjectRepair]") {
    Model        model;
    ModelObject &object = object_with(model, {merged(open_cube_at(10.f, Vec3f::Zero()), open_cube_at(10.f, Vec3f(20.f, 0.f, 0.f))),
                                              open_cube_at(10.f, Vec3f(40.f, 0.f, 0.f))});
    const auto   untouched = object.volumes[1]->get_mesh_shared_ptr();
    repair(object, 0);
    REQUIRE(object.volumes.back()->get_mesh_shared_ptr() == untouched);
    for (size_t i = 0; i + 1 < object.volumes.size(); ++i)
        REQUIRE(open_edges(*object.volumes[i]) == 0);
}

TEST_CASE("Repairing a volume made of separate shells repairs each shell", "[ModelObjectRepair]") {
    Model        model;
    ModelObject &object = object_with(model, {merged(cube_at(10.f, Vec3f::Zero()), open_cube_at(10.f, Vec3f(20.f, 0.f, 0.f)))});
    repair(object);
    for (const ModelVolume *v : object.volumes)
        REQUIRE(open_edges(*v) == 0);
    REQUIRE_THAT(total_volume(object), WithinRel(2000., 0.001));
}

TEST_CASE("Repairing keeps the object in place", "[ModelObjectRepair]") {
    Model        model;
    ModelObject &object = object_with(model, {merged(open_cube_at(10.f, Vec3f::Zero()), open_cube_at(10.f, Vec3f(20.f, 0.f, 0.f))),
                                              cube_at(10.f, Vec3f(0.f, 20.f, 0.f))});
    object.volumes[0]->set_offset(Vec3d(3., -7., 11.));
    object.instances.front()->set_offset(Vec3d(100., 50., 0.));
    const BoundingBoxf3 before = world_bounding_box(object);
    repair(object);
    const BoundingBoxf3 after = world_bounding_box(object);
    REQUIRE(after.min.isApprox(before.min, 1e-5));
    REQUIRE(after.max.isApprox(before.max, 1e-5));
}

TEST_CASE("Repairing drops parts that enclose no volume", "[ModelObjectRepair]") {
    Model        model;
    ModelObject &object = object_with(model, {merged(open_cube_at(10.f, Vec3f::Zero()), sheet_at(Vec3f(20.f, 0.f, 0.f)))});
    repair(object);
    REQUIRE(object.volumes.size() == 1);
    REQUIRE(open_edges(*object.volumes.front()) == 0);
    REQUIRE_THAT(total_volume(object), WithinRel(1000., 0.001));
}

TEST_CASE("Repairing after dropping a flat first volume still repairs the next volume", "[ModelObjectRepair]") {
    Model        model;
    ModelObject &object = object_with(model, {sheet_at(Vec3f::Zero()), open_cube_at(10.f, Vec3f(20.f, 0.f, 0.f))});
    repair(object);
    REQUIRE(object.volumes.size() == 1);
    REQUIRE(open_edges(*object.volumes.front()) == 0);
}

TEST_CASE("Repairing never leaves an object without volumes", "[ModelObjectRepair]") {
    Model        model;
    ModelObject &object = object_with(model, {sheet_at(Vec3f::Zero())});
    REQUIRE_THROWS(repair(object));
    REQUIRE(object.volumes.size() == 1);
}

TEST_CASE("Repairing an object made only of flat parts reports failure without changing it", "[ModelObjectRepair]") {
    // Side by side, and stacked: together the stacked sheets span a box and have a non-zero signed volume.
    const Vec3f second = GENERATE(Vec3f(20.f, 0.f, 0.f), Vec3f(0.f, 0.f, 5.f));
    Model        model;
    ModelObject &object    = object_with(model, {merged(sheet_at(Vec3f::Zero()), sheet_at(second))});
    const auto   unchanged = object.volumes.front()->get_mesh_shared_ptr();
    REQUIRE_THROWS_AS(repair(object), Slic3r::RuntimeError);
    REQUIRE(object.volumes.size() == 1);
    REQUIRE(object.volumes.front()->get_mesh_shared_ptr() == unchanged);
}

TEST_CASE("Repairing a volume that does not exist is rejected", "[ModelObjectRepair]") {
    Model        model;
    ModelObject &object = object_with(model, {open_cube_at(10.f, Vec3f::Zero())});
    REQUIRE_THROWS_AS(repair(object, 1), Slic3r::InvalidArgument);
    REQUIRE_THROWS_AS(repair(object, -2), Slic3r::InvalidArgument);
    REQUIRE(open_edges(*object.volumes.front()) > 0);
}

TEST_CASE("Repairing stops when cancelled", "[ModelObjectRepair]") {
    Model        model;
    ModelObject &object = object_with(model, {open_cube_at(10.f, Vec3f::Zero())});
    struct Cancelled {};
    REQUIRE_THROWS_AS(repair_model_object(object, -1, [](const char *, int) {}, [] { throw Cancelled(); }), Cancelled);
    REQUIRE(open_edges(*object.volumes.front()) > 0);
}

TEST_CASE("Repairing an object made only of flat parts stops when cancelled", "[ModelObjectRepair]") {
    Model        model;
    ModelObject &object = object_with(model, {merged(sheet_at(Vec3f::Zero()), sheet_at(Vec3f(0.f, 0.f, 5.f)))});
    struct Cancelled {};
    REQUIRE_THROWS_AS(repair_model_object(object, -1, [](const char *, int) {}, [] { throw Cancelled(); }), Cancelled);
}

TEST_CASE("Repairing reports progress from 0 to 100 percent", "[ModelObjectRepair]") {
    Model        model;
    ModelObject &object = object_with(model, {open_cube_at(10.f, Vec3f::Zero()), sheet_at(Vec3f(20.f, 0.f, 0.f)),
                                              merged(open_cube_at(10.f, Vec3f(40.f, 0.f, 0.f)), open_cube_at(10.f, Vec3f(60.f, 0.f, 0.f)))});
    std::vector<int> reported;
    repair_model_object(object, -1, [&reported](const char *, int percent) { reported.push_back(percent); }, [] {});
    REQUIRE_FALSE(reported.empty());
    for (int percent : reported) {
        REQUIRE(percent >= 0);
        REQUIRE(percent <= 100);
    }
    REQUIRE(reported.back() == 100);
}
