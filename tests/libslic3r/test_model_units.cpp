#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// Builds a Model holding one object made of a single box, the same way load_stl() does for one STL file.
static Model make_single_box_model(const char *name, const Vec3d &size, const Vec3d &min_corner)
{
    TriangleMesh mesh = make_cube(size.x(), size.y(), size.z());
    mesh.translate(min_corner.cast<float>());
    Model model;
    model.add_object(name, name, std::move(mesh));
    return model;
}

// Mirrors the per-file steps of Plater::priv::load_files() for a geometry file:
// preferred-orientation rotation (which centers the object), unit conversion, centering.
static void load_like_plater(Model &model, bool convert_from_meters)
{
    for (ModelObject *obj : model.objects)
        obj->rotate(0., Axis::Z);
    if (convert_from_meters) {
        REQUIRE(model.looks_like_saved_in_meters());
        model.convert_from_meters(true);
    }
    for (ModelObject *obj : model.objects)
        obj->center_around_origin(false);
}

static Vec3d volume_center(const ModelObject &object, size_t idx)
{
    const ModelVolume &v = *object.volumes[idx];
    return v.mesh().transformed_bounding_box(v.get_matrix()).center();
}

SCENARIO("Files converted from meters keep their relative position when merged into one object", "[Model][units]")
{
    GIVEN("two STL bodies saved in meters, the second sitting on top of the first and off its center") {
        // 0.16 m x 0.14 m x 0.01 m base plate and a 0.05 m x 0.03 m x 0.002 m relief on top of it.
        const Vec3d base_size{0.16, 0.14, 0.01};
        const Vec3d base_min{0.0, 0.0, 0.0};
        const Vec3d relief_size{0.05, 0.03, 0.002};
        const Vec3d relief_min{0.02, 0.09, 0.01};

        auto run = [&](bool convert) {
            Model base   = make_single_box_model("base.stl", base_size, base_min);
            Model relief = make_single_box_model("relief.stl", relief_size, relief_min);
            load_like_plater(base, convert);
            load_like_plater(relief, convert);

            Model merged;
            merged.add_object(*base.objects.front());
            merged.add_object(*relief.objects.front());
            merged.convert_multipart_object(4);
            REQUIRE(merged.objects.size() == 1);
            REQUIRE(merged.objects.front()->volumes.size() == 2);
            return volume_center(*merged.objects.front(), 1) - volume_center(*merged.objects.front(), 0);
        };

        const Vec3d original_delta = (relief_min + 0.5 * relief_size) - (base_min + 0.5 * base_size);

        WHEN("the files are loaded without unit conversion") {
            const Vec3d delta = run(false);
            THEN("the parts keep their relative position") {
                REQUIRE_THAT(delta.x(), WithinAbs(original_delta.x(), 1e-6));
                REQUIRE_THAT(delta.y(), WithinAbs(original_delta.y(), 1e-6));
                REQUIRE_THAT(delta.z(), WithinAbs(original_delta.z(), 1e-6));
            }
        }

        WHEN("both files are converted from meters to millimeters") {
            const Vec3d delta    = run(true);
            const Vec3d expected = 1000. * original_delta;
            THEN("the relative position is scaled together with the geometry") {
                REQUIRE_THAT(delta.x(), WithinAbs(expected.x(), 1e-3));
                REQUIRE_THAT(delta.y(), WithinAbs(expected.y(), 1e-3));
                REQUIRE_THAT(delta.z(), WithinAbs(expected.z(), 1e-3));
            }
        }
    }
}
