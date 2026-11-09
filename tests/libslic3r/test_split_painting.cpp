#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Format/STL.hpp"

using namespace Slic3r;

// Build one volume whose mesh consists of two disconnected cubes, so that
// ModelVolume::split() splits it into two parts. Face indices 0..11 belong to the
// first cube, 12..23 to the second one (its_merge appends the second mesh).
static ModelVolume *make_two_patch_volume(Model &model)
{
    TriangleMesh mesh_a = make_cube(20., 20., 20.);
    TriangleMesh mesh_b = make_cube(20., 20., 20.);
    mesh_b.translate(100., 0., 0.);
    its_merge(mesh_a.its, mesh_b.its);

    ModelObject *object = model.add_object();
    object->add_instance();
    ModelVolume *volume = object->add_volume(std::move(mesh_a));
    volume->name = "TwoCubes";
    return volume;
}

TEST_CASE("Split carries the mmu painting over to the split parts", "[SplitPainting]")
{
    Model model;
    ModelVolume *volume = make_two_patch_volume(model);
    REQUIRE(volume->mesh().its.indices.size() == 24);

    // Paint one face of the first cube (extruder 2) and one face of the second
    // cube (extruder 3). Whole-face states serialize as a single digit.
    volume->mmu_segmentation_facets.set_triangle_from_string(1, "2");
    volume->mmu_segmentation_facets.set_triangle_from_string(13, "3");

    const size_t parts = volume->split(16);
    REQUIRE(parts == 2);
    REQUIRE(model.objects[0]->volumes.size() == 2);

    ModelVolume *part1 = model.objects[0]->volumes[0];
    ModelVolume *part2 = model.objects[0]->volumes[1];
    REQUIRE(part1->mesh().its.indices.size() == 12);
    REQUIRE(part2->mesh().its.indices.size() == 12);

    // The face order inside each part is ascending in the source face ids, so
    // source face 1 stays index 1 of part 1 and source face 13 becomes index 1
    // of part 2 (12 - 12).
    REQUIRE(part1->mmu_segmentation_facets.get_triangle_as_string(1) == "2");
    REQUIRE(part2->mmu_segmentation_facets.get_triangle_as_string(1) == "3");

    // No other face is painted on either part.
    for (int i = 0; i < 12; ++ i) {
        if (i != 1)
            CHECK(part1->mmu_segmentation_facets.get_triangle_as_string(i).empty());
        if (i != 1)
            CHECK(part2->mmu_segmentation_facets.get_triangle_as_string(i).empty());
    }
}

TEST_CASE("Split without painting keeps the previous behavior", "[SplitPainting]")
{
    Model model;
    ModelVolume *volume = make_two_patch_volume(model);

    const size_t parts = volume->split(16);
    REQUIRE(parts == 2);

    for (const ModelVolume *part : model.objects[0]->volumes) {
        for (int i = 0; i < int(part->mesh().its.indices.size()); ++ i)
            CHECK(part->mmu_segmentation_facets.get_triangle_as_string(i).empty());
    }
}
