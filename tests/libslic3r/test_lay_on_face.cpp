#include <catch2/catch.hpp>

#include "libslic3r/LayOnFace.hpp"
#include "libslic3r/Model.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// Adds a box part spanning `origin` to `origin + size`, in object coordinates.
void add_box(ModelObject &object, const Vec3d &size, const Vec3d &origin = Vec3d::Zero())
{
    TriangleMesh mesh = make_cube(size.x(), size.y(), size.z());
    mesh.translate(origin.cast<float>());
    object.add_volume(std::move(mesh), ModelVolumeType::MODEL_PART, false);
}

ModelObject &add_box_object(Model &model, const Vec3d &size)
{
    ModelObject *object = model.add_object();
    add_box(*object, size);
    object->add_instance();
    return *object;
}

// A 30 x 30 x 2 plate with three 1 mm thick, 20 mm tall ribs along Y. The rib sides facing -X add up
// to more area than the plate's bottom, but only the bottom is a face of the convex hull.
ModelObject &add_ribbed_plate(Model &model)
{
    ModelObject *object = model.add_object();
    add_box(*object, { 30, 30, 2 });
    for (double x : { 5., 14.5, 24. })
        add_box(*object, { 1, 30, 20 }, { x, 0, 2 });
    object->add_instance();
    return *object;
}

std::vector<LayOnFacePlane> instance_planes(const ModelObject &object)
{
    return lay_on_face_planes(object, object.instances.front()->get_matrix_no_offset());
}

void lay_on_largest_face(ModelObject &object)
{
    const std::vector<LayOnFacePlane> planes = instance_planes(object);
    const int                         idx    = find_largest_plane(planes);
    REQUIRE(idx >= 0);
    lay_on_face(object, 0, planes[idx].normal);
}

void check_size(const ModelObject &object, const Vec3d &expected)
{
    const Vec3d size = object.instance_bounding_box(0).size();
    CHECK_THAT(size.x(), WithinAbs(expected.x(), 1e-3));
    CHECK_THAT(size.y(), WithinAbs(expected.y(), 1e-3));
    CHECK_THAT(size.z(), WithinAbs(expected.z(), 1e-3));
}

void check_on_bed(const ModelObject &object) { CHECK_THAT(object.instance_bounding_box(0).min.z(), WithinAbs(0., 1e-3)); }

} // namespace

TEST_CASE("A tilted box is laid on its largest face and dropped onto the bed", "[LayOnFace]")
{
    Model        model;
    ModelObject &box = add_box_object(model, { 40, 20, 10 }); // the 40 x 20 faces are the largest
    box.instances.front()->set_rotation({ 0.3, 0.5, 0.2 });
    box.instances.front()->set_offset({ 0, 0, 50 });
    REQUIRE(box.instance_bounding_box(0).size().z() > 11.);

    const std::vector<LayOnFacePlane> planes = instance_planes(box);
    REQUIRE(planes.size() == 6);
    CHECK_THAT(planes.front().area, WithinAbs(40. * 20., 1e-2));

    lay_on_largest_face(box);
    CHECK_THAT(box.instance_bounding_box(0).size().z(), WithinAbs(10., 1e-3));
    check_on_bed(box);
}

TEST_CASE("A box lying on one of its equally large faces is not flipped", "[LayOnFace]")
{
    // A half turn about X puts the other large face down, so the two cases expect different faces
    // and neither can pass on the order in which the hull lists them.
    // values<double>({ ... }) rather than GENERATE(0., PI): handing this Catch2 2.13 header a const lvalue such as
    // the constexpr PI selects the template populate(U&&) overload, which MSVC compiles into an infinite
    // tail-call loop (the test spun at 100% CPU forever, in CI and locally). Literals and enums are unaffected.
    const double rotation_x = GENERATE(values<double>({ 0., PI }));
    Model        model;
    ModelObject &box = add_box_object(model, { 40, 20, 10 }); // the bottom and top are both 40 x 20
    box.instances.front()->set_rotation({ rotation_x, 0, 0 });
    const Transform3d before = box.instances.front()->get_matrix_no_offset();

    const std::vector<LayOnFacePlane> planes = instance_planes(box);
    const int                         idx    = find_largest_plane(planes);
    REQUIRE(idx >= 0);
    // The face down on the plate is the object's -Z face, or its +Z face after the half turn.
    CHECK_THAT(planes[idx].normal.z(), WithinAbs(rotation_x == 0. ? -1. : 1., 1e-6));

    lay_on_face(box, 0, planes[idx].normal);
    CHECK(box.instances.front()->get_matrix_no_offset().isApprox(before, 1e-9));
}

TEST_CASE("Faces are chosen from the orientation left by an earlier part rotation", "[LayOnFace]")
{
    Model        model;
    ModelObject &box = add_box_object(model, { 40, 20, 10 });
    box.rotate(PI / 2., X); // what --rotate-x 90 does: rotates the parts, not the instance
    check_size(box, { 40, 10, 20 });

    SECTION("the largest face") {
        lay_on_largest_face(box);
        check_size(box, { 40, 20, 10 });
        check_on_bed(box);
    }

    SECTION("the face pointing along +X") {
        const std::vector<LayOnFacePlane> planes = instance_planes(box);
        const int                         idx    = find_plane_by_normal(planes, { 1, 0, 0 });
        REQUIRE(idx >= 0);
        CHECK_THAT(planes[idx].normal.x(), WithinAbs(1., 1e-6));
        lay_on_face(box, 0, planes[idx].normal);
        check_size(box, { 20, 10, 40 });
        check_on_bed(box);
    }
}

TEST_CASE("Objects are laid on their own faces independently", "[LayOnFace]")
{
    Model model;
    // Standing on end through its instance rotation.
    ModelObject &standing = add_box_object(model, { 40, 20, 10 });
    standing.instances.front()->set_rotation({ 0, PI / 2., 0 });
    // Standing on edge through a part rotation, lifted above the bed.
    ModelObject &on_edge = add_box_object(model, { 30, 20, 5 });
    on_edge.rotate(PI / 2., X);
    on_edge.instances.front()->set_offset({ 100, 0, 30 });
    check_size(standing, { 10, 20, 40 });
    check_size(on_edge, { 30, 5, 20 });

    for (ModelObject *object : model.objects)
        lay_on_largest_face(*object);

    check_size(standing, { 40, 20, 10 });
    check_on_bed(standing);
    check_size(on_edge, { 30, 20, 5 });
    check_on_bed(on_edge);
}

TEST_CASE("A part rests on its largest hull face even when parallel inner faces add up to more area", "[LayOnFace]")
{
    Model        model;
    ModelObject &plate = add_ribbed_plate(model);

    double area_facing_minus_x = 0.;
    for (const ModelVolume *volume : plate.volumes) {
        const indexed_triangle_set &its = volume->mesh().its;
        for (const Vec3i32 &face : its.indices) {
            const Vec3d cross = (its.vertices[face[1]] - its.vertices[face[0]]).cast<double>().cross(
                (its.vertices[face[2]] - its.vertices[face[0]]).cast<double>());
            if (cross.normalized().x() < -0.999)
                area_facing_minus_x += 0.5 * cross.norm();
        }
    }
    // Summing triangle area per normal would pick a rib side over the 900 mm² bottom.
    REQUIRE(area_facing_minus_x > 30. * 30.);

    plate.instances.front()->set_rotation({ 0, PI / 2., 0 }); // stand the plate on its side
    check_size(plate, { 22, 30, 30 });

    const std::vector<LayOnFacePlane> planes = instance_planes(plate);
    const int                         idx    = find_largest_plane(planes);
    REQUIRE(idx >= 0);
    CHECK_THAT(planes[idx].area, WithinAbs(30. * 30., 1e-2));
    CHECK_THAT(planes[idx].normal.z(), WithinAbs(-1., 1e-6));

    lay_on_face(plate, 0, planes[idx].normal);
    check_size(plate, { 30, 30, 22 });
    check_on_bed(plate);
}

TEST_CASE("Faces are selected in object coordinates whatever the instance rotation", "[LayOnFace]")
{
    Model        model;
    ModelObject &plate = add_ribbed_plate(model);
    plate.instances.front()->set_rotation({ 0, 0, PI / 2. });
    const Transform3d                 instance_matrix = plate.instances.front()->get_matrix_no_offset();
    const std::vector<LayOnFacePlane> planes          = lay_on_face_planes(plate, instance_matrix);
    REQUIRE_FALSE(planes.empty());

    // Every face center, as --inspect-mesh reports it, selects its own face.
    for (size_t i = 0; i < planes.size(); ++i)
        CHECK(find_plane_at_point(planes, instance_matrix, planes[i].center, 0.01) == int(i));

    const int bottom = find_plane_at_point(planes, instance_matrix, { 15, 15, 0 }, 0.01);
    REQUIRE(bottom >= 0);
    CHECK_THAT(planes[bottom].normal.z(), WithinAbs(-1., 1e-6));
    CHECK(find_plane_by_normal(planes, { 0, 0, -1 }) == bottom);
    // Above the bottom plane, and on a rib side that lies inside the hull.
    CHECK(find_plane_at_point(planes, instance_matrix, { 15, 15, 0.5 }, 0.01) == -1);
    CHECK(find_plane_at_point(planes, instance_matrix, { 14.5, 15, 12 }, 0.01) == -1);
}

TEST_CASE("A part too small to rest on offers no faces", "[LayOnFace]")
{
    Model model;
    CHECK(instance_planes(add_box_object(model, { 2, 2, 2 })).empty()); // every face is 4 mm², under the 5 mm² minimum
}

namespace {

// A 30 x 30 x 2 plate with a 10 x 20 x 4 block standing on it, stood on edge by a part rotation.
ModelObject &add_plate_with_tilted_block(Model &model)
{
    ModelObject *object = model.add_object();
    add_box(*object, { 30, 30, 2 });
    add_box(*object, { 10, 20, 4 }, { 10, 5, 2 });
    object->add_instance();
    ModelVolume &block = *object->volumes[1];
    const Vec3d  center = block.mesh().bounding_box().center();
    block.set_transformation(Geometry::Transformation(Geometry::translation_transform(center) *
                                                      Geometry::rotation_transform({ PI / 2., 0, 0 }) *
                                                      Geometry::translation_transform(-center)));
    return *object;
}

BoundingBoxf3 part_bounding_box(const ModelObject &object, size_t volume_idx)
{
    const ModelVolume &volume = *object.volumes[volume_idx];
    return volume.mesh().transformed_bounding_box(object.instances.front()->get_matrix() * volume.get_matrix());
}

void lay_part_on_largest_face(ModelObject &object, size_t volume_idx)
{
    const std::vector<LayOnFacePlane> planes =
        lay_on_face_planes(*object.volumes[volume_idx], object.instances.front()->get_matrix_no_offset());
    const int idx = find_largest_plane(planes);
    REQUIRE(idx >= 0);
    CHECK_THAT(planes[idx].area, WithinAbs(10. * 20., 1e-2)); // the block's own face, not the plate's
    lay_part_on_face(object, 0, volume_idx, planes[idx].normal);
}

} // namespace

TEST_CASE("A single part is laid on its own face while the rest of the object stays put", "[LayOnFace]")
{
    Model        model;
    ModelObject &object = add_plate_with_tilted_block(model);
    const Transform3d plate_before  = object.volumes[0]->get_matrix();
    const Transform3d inst_before   = object.instances.front()->get_matrix_no_offset();
    const Vec3d       center_before = part_bounding_box(object, 1).center();
    const Vec3d       size_before   = part_bounding_box(object, 1).size();
    CHECK_THAT(size_before.z(), WithinAbs(20., 1e-3));

    lay_part_on_largest_face(object, 1);

    const BoundingBoxf3 block = part_bounding_box(object, 1);
    CHECK_THAT(block.size().x(), WithinAbs(10., 1e-3));
    CHECK_THAT(block.size().y(), WithinAbs(20., 1e-3));
    CHECK_THAT(block.size().z(), WithinAbs(4., 1e-3));
    // Turned in place about its own center.
    CHECK_THAT(block.center().x(), WithinAbs(center_before.x(), 1e-3));
    CHECK_THAT(block.center().y(), WithinAbs(center_before.y(), 1e-3));
    // The plate and the instance keep their transformations, and the object still sits on the bed.
    CHECK(object.volumes[0]->get_matrix().isApprox(plate_before, 1e-9));
    CHECK(object.instances.front()->get_matrix_no_offset().isApprox(inst_before, 1e-9));
    check_on_bed(object);
}

TEST_CASE("A part's face ends up pointing down whatever the instance rotation and scale", "[LayOnFace]")
{
    Model        model;
    ModelObject &object = add_plate_with_tilted_block(model);
    object.instances.front()->set_rotation({ PI / 3., PI / 5., PI / 7. });
    object.instances.front()->set_scaling_factor({ 1., 2., 0.5 });
    const Matrix3d inst_linear = object.instances.front()->get_matrix().matrix().block(0, 0, 3, 3);

    ModelVolume &block = *object.volumes[1];
    for (size_t i = 0; i < 6; ++i) {
        // Every face of the block in turn, taken from where the previous one left it.
        const std::vector<LayOnFacePlane> planes = lay_on_face_planes(block, object.instances.front()->get_matrix_no_offset());
        REQUIRE(planes.size() == 6);
        const LayOnFacePlane &plane = planes[i];
        const Matrix3d old_linear = block.get_matrix().matrix().block(0, 0, 3, 3);
        lay_part_on_face(object, 0, 1, plane.normal);
        const Matrix3d new_linear = block.get_matrix().matrix().block(0, 0, 3, 3);

        // Normals map through the inverse transpose: object -> mesh with the old part matrix, then back with the new one.
        const Vec3d mesh_normal  = old_linear.transpose() * plane.normal;
        const Vec3d world_normal = (inst_linear.inverse().transpose() * new_linear.inverse().transpose() * mesh_normal).normalized();
        CHECK_THAT(world_normal.z(), WithinAbs(-1., 1e-6));
        // The part stays rigid: its own matrix is still a rotation (the mesh boxes are unscaled).
        CHECK(new_linear.isUnitary(1e-9));
        check_on_bed(object);
    }
}
