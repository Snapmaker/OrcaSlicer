// Tests for the "mesh rebuild loses the source-file offset" review item: a
// same-coordinate mesh replacement (simplify, global smooth, local
// subdivision) rebuilt the mesh through set_mesh(indexed_triangle_set),
// whose fresh TriangleMesh has a zero init_shift. reload from disk / replace
// with STL map the painting between the file and the stored coordinates
// through the init shifts (ModelVolume::paint_relink_transform), so a rebuilt
// mesh relinked the painting at a wrong offset whenever the source file's
// center was not the origin. The production fix is
// ModelVolume::set_mesh_preserving_init_shift(); these tests drive the same
// production entry points (save_painting / paint_relink_transform /
// restore_painting) around it and check exact painted-facet recovery plus
// unpainted regions.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

namespace {

// Closed cube built from 12 triangles (two per face), CCW seen from outside.
indexed_triangle_set cube_its()
{
    indexed_triangle_set its;
    its.vertices = { Vec3f(0.f, 0.f, 0.f), Vec3f(1.f, 0.f, 0.f), Vec3f(1.f, 1.f, 0.f), Vec3f(0.f, 1.f, 0.f),
                     Vec3f(0.f, 0.f, 1.f), Vec3f(1.f, 0.f, 1.f), Vec3f(1.f, 1.f, 1.f), Vec3f(0.f, 1.f, 1.f) };
    its.indices  = {
        stl_triangle_vertex_indices(0, 2, 1), stl_triangle_vertex_indices(0, 3, 2), // bottom
        stl_triangle_vertex_indices(4, 5, 6), stl_triangle_vertex_indices(4, 6, 7), // top
        stl_triangle_vertex_indices(0, 1, 5), stl_triangle_vertex_indices(0, 5, 4), // y = 0
        stl_triangle_vertex_indices(1, 2, 6), stl_triangle_vertex_indices(1, 6, 5), // x = 1
        stl_triangle_vertex_indices(2, 3, 7), stl_triangle_vertex_indices(2, 7, 6), // y = 1
        stl_triangle_vertex_indices(3, 0, 4), stl_triangle_vertex_indices(3, 4, 7), // x = 0
    };
    return its;
}

// A "source file" whose center is clearly not the origin.
indexed_triangle_set source_file_its()
{
    indexed_triangle_set its = cube_its();
    for (stl_vertex& v : its.vertices)
        v += Vec3f(100.f, 50.f, 20.f);
    return its;
}

const Vec3d kFileCenter(100.5, 50.5, 20.5);

// Imported volume: add_volume() re-centers the stored mesh, so init_shift and
// source.mesh_offset both record the file's center (in file units).
struct LoadedVolume {
    std::unique_ptr<Model> model;
    ModelVolume*           volume = nullptr;
};

LoadedVolume load_volume(const indexed_triangle_set& file_its)
{
    LoadedVolume loaded;
    loaded.model                 = std::make_unique<Model>();
    ModelObject* obj             = loaded.model->add_object();
    loaded.volume                = obj->add_volume(TriangleMesh(file_its));
    loaded.volume->name          = "part";
    loaded.volume->source.input_file = "part.stl";
    return loaded;
}

// Paints a small brush stroke on the stored mesh. facet_start must be the
// facet the pointer hits (the production gizmo raycasts for it); the
// neighbor walk from it covers the circle area.
void paint_stroke(TriangleSelector& selector, int facet_start, const Vec3f& stored_center, float radius)
{
    auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(
        stored_center, stored_center + 10.f * Vec3f::UnitZ(), radius, TriangleSelector::CursorType::CIRCLE,
        Transform3d::Identity(), TriangleSelector::ClippingPlane());
    selector.select_patch(facet_start, std::move(cursor), EnforcerBlockerType::ENFORCER, Transform3d::Identity(), true);
}

// Paints two asymmetric strokes, one on each of the two adjacent top-face
// triangles (facets 2 and 3 of the cube built by cube_its()), and stores the
// combined painting on the volume. Returns the painted original-facet indices.
std::vector<int> paint_local_strokes(ModelVolume& volume, float scale = 1.f)
{
    TriangleSelector selector(volume.mesh());
    paint_stroke(selector, 2, scale * Vec3f(0.4f, 0.3f, 0.5f), scale * 0.05f); // x > y side of the diagonal
    paint_stroke(selector, 3, scale * Vec3f(0.3f, 0.4f, 0.5f), scale * 0.05f); // x < y side of the diagonal
    volume.supported_facets.set_data(selector.serialize());
    return selector.get_facets_indices_by_state(EnforcerBlockerType::ENFORCER);
}

std::vector<int> painted_facets(const ModelVolume& volume)
{
    TriangleSelector selector(volume.mesh());
    selector.deserialize(volume.supported_facets.get_data(), false);
    return selector.get_facets_indices_by_state(EnforcerBlockerType::ENFORCER);
}

// Mirrors the production reload / replace paint relink: maps the old painting
// onto a freshly loaded mesh of the same source file through the init shifts.
Transform3d relink_and_restore(ModelVolume& old_volume, ModelVolume& new_volume, double unit_scale)
{
    const auto saved = old_volume.save_painting();
    REQUIRE(saved.has_value());
    const Transform3d target_to_source =
        ModelVolume::paint_relink_transform(saved->mesh.get_init_shift(), new_volume.mesh().get_init_shift(), unit_scale);
    new_volume.restore_painting(saved, target_to_source);
    return target_to_source;
}

} // namespace

TEST_CASE("Paint relinks to the same file after a same-coordinate mesh rebuild", "[PaintReload]")
{
    const indexed_triangle_set file_its = source_file_its();

    LoadedVolume loaded = load_volume(file_its);
    REQUIRE(loaded.volume->mesh().get_init_shift().isApprox(kFileCenter));
    const std::vector<int> original_painted = paint_local_strokes(*loaded.volume);
    REQUIRE(original_painted.size() >= 2);
    REQUIRE_FALSE(original_painted.empty());

    // Same-coordinate rebuild, mirroring the simplify write-back path: the
    // result ITS lives in the volume's stored coordinates and the painting is
    // restored with the identity transform.
    const auto saved = loaded.volume->save_painting();
    REQUIRE(saved.has_value());
    loaded.volume->set_mesh_preserving_init_shift(indexed_triangle_set(loaded.volume->mesh().its));
    loaded.volume->restore_painting(saved, Transform3d::Identity(), false, /*transfer_by_projection=*/true);

    // The stored geometry and the file-to-stored relationship survive the rebuild.
    CHECK(loaded.volume->mesh().get_init_shift().isApprox(kFileCenter));

    // A same-file reload relinks the painting at the exact same faces: the
    // relative transform is the identity and nothing is painted elsewhere.
    LoadedVolume        reloaded = load_volume(file_its);
    const Transform3d relink    = relink_and_restore(*loaded.volume, *reloaded.volume, 1.0);
    CHECK(relink.isApprox(Transform3d::Identity()));
    CHECK(painted_facets(*reloaded.volume) == original_painted);
}

TEST_CASE("Plain set_mesh loses the offset and misrelinks the painting (old behavior)", "[PaintReload]")
{
    const indexed_triangle_set file_its = source_file_its();

    LoadedVolume loaded = load_volume(file_its);
    const std::vector<int> original_painted = paint_local_strokes(*loaded.volume);
    REQUIRE_FALSE(original_painted.empty());

    // The pre-fix rebuild path: a fresh TriangleMesh with a zero init_shift.
    const auto saved = loaded.volume->save_painting();
    REQUIRE(saved.has_value());
    loaded.volume->set_mesh(indexed_triangle_set(loaded.volume->mesh().its));
    loaded.volume->restore_painting(saved, Transform3d::Identity(), false, /*transfer_by_projection=*/true);
    CHECK(loaded.volume->mesh().get_init_shift().isApprox(Vec3d::Zero()));

    // The same-file reload then maps the painting through a bogus +file-center
    // translation instead of the identity.
    LoadedVolume reloaded = load_volume(file_its);
    const Transform3d relink = relink_and_restore(*loaded.volume, *reloaded.volume, 1.0);
    CHECK_FALSE(relink.isApprox(Transform3d::Identity()));
    CHECK(relink.translation().isApprox(kFileCenter));
}

TEST_CASE("Paint relinks to the same file without any rebuild", "[PaintReload]")
{
    const indexed_triangle_set file_its = source_file_its();

    LoadedVolume loaded = load_volume(file_its);
    const std::vector<int> original_painted = paint_local_strokes(*loaded.volume);
    REQUIRE_FALSE(original_painted.empty());

    // The pre-existing direct-reload path must stay exact.
    LoadedVolume        reloaded = load_volume(file_its);
    const Transform3d relink    = relink_and_restore(*loaded.volume, *reloaded.volume, 1.0);
    CHECK(relink.isApprox(Transform3d::Identity()));
    CHECK(painted_facets(*reloaded.volume) == original_painted);
}

TEST_CASE("Unit conversion keeps the relink in file units", "[PaintReload]")
{
    const indexed_triangle_set file_its = source_file_its();

    LoadedVolume loaded = load_volume(file_its);
    // The source file is in inches: conversion scales the stored mesh to mm but
    // leaves the shifts in file (inch) units.
    loaded.volume->convert_from_imperial_units();
    CHECK(loaded.volume->mesh().get_init_shift().isApprox(kFileCenter));
    CHECK(loaded.volume->source.is_converted_from_inches);

    const float   scale            = 25.4f;
    const std::vector<int> original_painted = paint_local_strokes(*loaded.volume, scale);
    REQUIRE_FALSE(original_painted.empty());

    // Rebuild in stored (mm) coordinates, then reload the same inches file.
    const auto saved = loaded.volume->save_painting();
    REQUIRE(saved.has_value());
    loaded.volume->set_mesh_preserving_init_shift(indexed_triangle_set(loaded.volume->mesh().its));
    loaded.volume->restore_painting(saved, Transform3d::Identity(), false, /*transfer_by_projection=*/true);

    LoadedVolume reloaded = load_volume(file_its);
    reloaded.volume->convert_from_imperial_units();
    const Transform3d relink = relink_and_restore(*loaded.volume, *reloaded.volume, 25.4);
    CHECK(relink.isApprox(Transform3d::Identity()));
    CHECK(painted_facets(*reloaded.volume) == original_painted);
}

TEST_CASE("Placement changes do not disturb the reload relink", "[PaintReload]")
{
    const indexed_triangle_set file_its = source_file_its();

    LoadedVolume loaded = load_volume(file_its);
    const std::vector<int> original_painted = paint_local_strokes(*loaded.volume);
    REQUIRE_FALSE(original_painted.empty());

    // Rotation, non-uniform scaling and mirroring act on the placement only;
    // the painting lives in the stored mesh space.
    loaded.volume->set_rotation(Vec3d(0., 0., 0.7));
    loaded.volume->set_scaling_factor(Vec3d(2.0, 1.0, 0.5));
    loaded.volume->set_mirror(Vec3d(-1., 1., 1.));
    CHECK(loaded.volume->mesh().get_init_shift().isApprox(kFileCenter));

    const auto saved = loaded.volume->save_painting();
    REQUIRE(saved.has_value());
    loaded.volume->set_mesh_preserving_init_shift(indexed_triangle_set(loaded.volume->mesh().its));
    loaded.volume->restore_painting(saved, Transform3d::Identity(), false, /*transfer_by_projection=*/true);

    LoadedVolume        reloaded = load_volume(file_its);
    const Transform3d relink    = relink_and_restore(*loaded.volume, *reloaded.volume, 1.0);
    CHECK(relink.isApprox(Transform3d::Identity()));
    CHECK(painted_facets(*reloaded.volume) == original_painted);
}

TEST_CASE("Repeated rebuild and reload cycles keep the painting aligned", "[PaintReload]")
{
    const indexed_triangle_set file_its = source_file_its();

    LoadedVolume loaded = load_volume(file_its);
    const std::vector<int> original_painted = paint_local_strokes(*loaded.volume);
    REQUIRE_FALSE(original_painted.empty());

    ModelVolume* current = loaded.volume;
    for (int cycle = 0; cycle < 3; ++cycle) {
        // Rebuild (simplify-like), then reload the same file onto a fresh volume.
        const auto saved = current->save_painting();
        REQUIRE(saved.has_value());
        current->set_mesh_preserving_init_shift(indexed_triangle_set(current->mesh().its));
        current->restore_painting(saved, Transform3d::Identity(), false, /*transfer_by_projection=*/true);
        CHECK(current->mesh().get_init_shift().isApprox(kFileCenter));

        LoadedVolume reloaded = load_volume(file_its);
        const Transform3d relink = relink_and_restore(*current, *reloaded.volume, 1.0);
        CHECK(relink.isApprox(Transform3d::Identity()));
        CHECK(painted_facets(*reloaded.volume) == original_painted);
        loaded = std::move(reloaded);
        current = loaded.volume;
    }
}

TEST_CASE("Rebuild preserves the immediate geometry and the four paint stores", "[PaintReload]")
{
    const indexed_triangle_set file_its = source_file_its();

    LoadedVolume fixed = load_volume(file_its);
    LoadedVolume plain = load_volume(file_its);
    paint_local_strokes(*fixed.volume);
    paint_local_strokes(*plain.volume);

    const auto saved_fixed = fixed.volume->save_painting();
    const auto saved_plain = plain.volume->save_painting();
    REQUIRE(saved_fixed.has_value());
    REQUIRE(saved_plain.has_value());

    fixed.volume->set_mesh_preserving_init_shift(indexed_triangle_set(fixed.volume->mesh().its));
    plain.volume->set_mesh(indexed_triangle_set(plain.volume->mesh().its));
    fixed.volume->restore_painting(saved_fixed, Transform3d::Identity(), false, /*transfer_by_projection=*/true);
    plain.volume->restore_painting(saved_plain, Transform3d::Identity(), false, /*transfer_by_projection=*/true);

    // Identical geometry and identical painting stores: the only difference is
    // the carried init shift.
    CHECK(fixed.volume->mesh().its.vertices == plain.volume->mesh().its.vertices);
    CHECK(fixed.volume->mesh().its.indices == plain.volume->mesh().its.indices);
    CHECK(fixed.volume->supported_facets.get_data() == plain.volume->supported_facets.get_data());
    CHECK(fixed.volume->seam_facets.get_data() == plain.volume->seam_facets.get_data());
    CHECK(fixed.volume->mmu_segmentation_facets.get_data() == plain.volume->mmu_segmentation_facets.get_data());
    CHECK(fixed.volume->fuzzy_skin_facets.get_data() == plain.volume->fuzzy_skin_facets.get_data());
    CHECK(fixed.volume->mesh().get_init_shift().isApprox(kFileCenter));
    CHECK(plain.volume->mesh().get_init_shift().isApprox(Vec3d::Zero()));
}

TEST_CASE("Repair commit preserves the file offset for a subsequent reload", "[PaintReload][ReviewRegression]")
{
    for (bool keep_paint : {false, true}) {
        LoadedVolume loaded = load_volume(source_file_its());
        const auto original = paint_local_strokes(*loaded.volume);
        // Recreate the mesh from geometry only, as after the temporary 3MF
        // round trip in the Windows repair path. Exercise its real commit API.
        std::vector<ModelObject::RepairCommitItem> items;
        items.push_back({loaded.volume, TriangleMesh(indexed_triangle_set(loaded.volume->mesh().its))});
        REQUIRE(items.front().repaired_mesh.get_init_shift().isApprox(Vec3d::Zero()));
        auto result = loaded.volume->get_object()->commit_repaired_meshes_with_painting(
            items, keep_paint, [] { return false; }, nullptr);
        REQUIRE(result == ModelObject::RepairCommitResult::Committed);
        CHECK(loaded.volume->mesh().get_init_shift().isApprox(kFileCenter));
        if (keep_paint)
            CHECK(painted_facets(*loaded.volume) == original);
        else {
            CHECK_FALSE(loaded.volume->is_any_painted());
            paint_local_strokes(*loaded.volume);
        }
        LoadedVolume reloaded = load_volume(source_file_its());
        CHECK(relink_and_restore(*loaded.volume, *reloaded.volume, 1.0).isApprox(Transform3d::Identity()));
        CHECK(painted_facets(*reloaded.volume) == original);
    }
}
