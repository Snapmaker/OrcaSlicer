// Regression tests for the "keep painted features after mesh change" remapping
// (MR !21 adversarial review). Each review item gets a fixture that drives the
// production remapping entry points directly, checks position / blank regions /
// cross-volume pollution, and covers all four painting annotations through
// their shared TriangleSplittingData representation.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <utility>

#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshDeal.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;
using namespace Slic3r::Geometry;

namespace {

// Area-weighted centroid of an indexed triangle set (painted leaf tiling).
std::pair<float, Vec3f> painted_area_and_centroid(const indexed_triangle_set& its)
{
    float area_sum = 0.f;
    Vec3f centroid = Vec3f::Zero();
    for (const auto& face : its.indices) {
        const Vec3f& a = its.vertices[face(0)];
        const Vec3f& b = its.vertices[face(1)];
        const Vec3f& c = its.vertices[face(2)];
        const float area = 0.5f * (b - a).cross(c - a).norm();
        area_sum += area;
        centroid += area * (a + b + c) / 3.f;
    }
    if (area_sum > 0.f)
        centroid /= area_sum;
    return { area_sum, centroid };
}

// Paint a circular brush stroke at `center` (world coordinates of the mesh,
// mesh lies in the XY plane facing +Z) with the given radius.
TriangleSelector::TriangleSplittingData paint_brush_stroke(const TriangleMesh& mesh, const Vec3f& center, float radius,
                                                           EnforcerBlockerType state)
{
    TriangleSelector selector(mesh);
    std::unique_ptr<TriangleSelector::Cursor> cursor = TriangleSelector::SinglePointCursor::cursor_factory(
        center, center + 10.f * Vec3f::UnitZ(), radius, TriangleSelector::CursorType::CIRCLE, Transform3d::Identity(),
        TriangleSelector::ClippingPlane());
    selector.select_patch(0, std::move(cursor), state, Transform3d::Identity(), true);
    return selector.serialize();
}

// Decode a remap result onto a TriangleSelector and return the painted facets
// in the target mesh's own coordinate space.
indexed_triangle_set painted_facets(const TriangleMesh& target_mesh, const TriangleSelector::TriangleSplittingData& data,
                                    EnforcerBlockerType state)
{
    TriangleSelector selector(target_mesh);
    selector.deserialize(data, false);
    return selector.get_facets(state);
}

} // namespace

// Regular triangular lattice over a 16 mm right triangle (n x n faces).
static indexed_triangle_set triangular_grid(int n)
{
    indexed_triangle_set its;
    const float s = 16.f / float(n);
    std::map<std::pair<int, int>, int> vertex_ids;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i + j <= n; ++i) {
            vertex_ids[{ i, j }] = int(its.vertices.size());
            its.vertices.emplace_back(float(i) * s, float(j) * s, 0.f);
        }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i + j < n; ++i) {
            const int a = vertex_ids.at({ i, j });
            const int b = vertex_ids.at({ i + 1, j });
            const int c = vertex_ids.at({ i, j + 1 });
            its.indices.emplace_back(a, b, c); // CCW in the XY plane (+Z normal)
            if (i + j + 1 < n) {
                const int d = vertex_ids.at({ i + 1, j + 1 });
                its.indices.emplace_back(b, d, c);
            }
        }
    return its;
}

// Review item 4: a single fully painted source face covering a coplanar regular
// grid. The spatial index already enumerates every candidate target face; each
// candidate must be selected exactly once instead of starting a connected-region
// traversal per candidate (the review measured 14.24 s for 16,384 faces with
// the old per-candidate select_patch traversal). Counting asserts the contract,
// a generous wall-clock ceiling is only a smoke alarm.
TEST_CASE("Paint remap: one painted source face selects each grid face exactly once", "[TriangleSelector][PaintRemap]")
{
    indexed_triangle_set source_its;
    source_its.vertices = { Vec3f(0.f, 0.f, 0.f), Vec3f(16.f, 0.f, 0.f), Vec3f(0.f, 16.f, 0.f) };
    source_its.indices  = { stl_triangle_vertex_indices(0, 1, 2) };
    const TriangleMesh source_mesh(source_its);
    const EnforcerBlockerType state = EnforcerBlockerType::ENFORCER;

    TriangleSelector source_selector(source_mesh);
    source_selector.set_facet(0, state); // one painted leaf: the whole source triangle
    auto painting = source_selector.serialize();
    REQUIRE(TriangleSelector::has_facets(painting, state));

    for (const int n : { 32, 64, 128 }) { // 1,024 / 4,096 / 16,384 target faces
        const indexed_triangle_set target_its = triangular_grid(n);
        const TriangleMesh         target_mesh(target_its);
        REQUIRE(target_its.indices.size() == size_t(n) * size_t(n));

        TriangleSelector::RemapStats stats;
        const auto start = std::chrono::steady_clock::now();
        auto remapped    = TriangleSelector::remap_painting(source_its, painting, target_its, Transform3d::Identity(),
                                                            std::nullopt, &stats);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        REQUIRE_FALSE(remapped.bitstream.empty());
        INFO("n=" << n << " faces=" << target_its.indices.size() << " candidates=" << stats.candidate_faces
                  << " select_calls=" << stats.select_calls << " elapsed=" << elapsed << "s");
        // One painted source leaf; every grid face is a candidate and is selected exactly once.
        CHECK(stats.painted_leaves == 1);
        CHECK(stats.candidate_faces == target_its.indices.size());
        CHECK(stats.select_calls == stats.candidate_faces);
        // Full coverage: the whole 16 mm triangle (128 mm^2) is painted.
        const auto [area, centroid] = painted_area_and_centroid(painted_facets(target_mesh, remapped, state));
        CHECK_THAT(area, WithinAbs(128.f, 0.5f));
        CHECK_THAT(centroid.x(), WithinAbs(16.f / 3.f, 0.5f));
        CHECK_THAT(centroid.y(), WithinAbs(16.f / 3.f, 0.5f));
        // Smoke ceiling: the old per-candidate traversal needed 14.24 s at 16k faces.
        CHECK(elapsed < 5.0);
    }
}

// Review item 7: candidate vertices were tested only by their barycentric
// projection, so a target face with one corner raised 1 mm off the painted
// source plane still got assigned the whole face. The remap now gates every
// entry (vertex, edge, pointer, whole-face assignment) on the tolerance band
// around the source leaf plane.
TEST_CASE("Paint remap: distance band keeps raised corners and offset planes blank", "[TriangleSelector][PaintRemap]")
{
    indexed_triangle_set source_its;
    source_its.vertices = { Vec3f(0.f, 0.f, 0.f), Vec3f(16.f, 0.f, 0.f), Vec3f(0.f, 16.f, 0.f) };
    source_its.indices  = { stl_triangle_vertex_indices(0, 1, 2) };
    const TriangleMesh source_mesh(source_its);
    const EnforcerBlockerType state = EnforcerBlockerType::Extruder4;

    TriangleSelector source_selector(source_mesh);
    source_selector.set_facet(0, state); // whole source triangle painted
    auto painting = source_selector.serialize();
    REQUIRE(TriangleSelector::has_facets(painting, state));

    // Coplanar coincident face: fully preserved (the whole 128 mm^2).
    {
        const TriangleMesh target_mesh(source_its);
        auto remapped = TriangleSelector::remap_painting(source_its, painting, target_mesh.its, Transform3d::Identity(), std::nullopt);
        const auto [area, centroid] = painted_area_and_centroid(painted_facets(target_mesh, remapped, state));
        CHECK_THAT(area, WithinAbs(128.f, 0.5f));
    }

    // One corner raised 1 mm: the far corner stays blank; only the band area
    // can show painting; no painted vertex may leave the tolerance band.
    {
        indexed_triangle_set raised_its = source_its;
        raised_its.vertices[2].z()      = 1.f;
        const TriangleMesh raised_mesh(raised_its);
        auto remapped = TriangleSelector::remap_painting(source_its, painting, raised_its, Transform3d::Identity(), std::nullopt);
        const indexed_triangle_set painted = painted_facets(raised_mesh, remapped, state);
        REQUIRE_FALSE(painted.indices.empty()); // band-near region still painted
        const auto [area, centroid] = painted_area_and_centroid(painted);
        INFO("raised corner: area " << area << ", centroid " << centroid.transpose());
        CHECK(area < 128.f); // definitely not the whole face
        for (const Vec3f& v : painted.vertices) {
            CHECK(std::abs(v.z()) <= 0.05f); // within the band plus one subdivision step
        }
        // The painted strip hugs the low edge (y == 0) of the triangle: its
        // centroid stays far from the raised corner at (0, 16).
        CHECK_THAT(centroid.y(), WithinAbs(0.f, 0.6f));
        CHECK_THAT(centroid.x(), WithinAbs(8.f, 3.f));
        // Far corner region (within 2 mm of the raised vertex) must be blank.
        for (const Vec3f& v : painted.vertices)
            CHECK(((Vec2f(v.x(), v.y()) - Vec2f(0.f, 16.f)).norm()) > 2.f);
    }

    // A parallel plane 1 mm away: nothing may be painted.
    {
        indexed_triangle_set offset_its = source_its;
        for (Vec3f& v : offset_its.vertices)
            v.z() = 1.f;
        const TriangleMesh offset_mesh(offset_its);
        auto remapped = TriangleSelector::remap_painting(source_its, painting, offset_its, Transform3d::Identity(), std::nullopt);
        CHECK(painted_facets(offset_mesh, remapped, state).indices.empty());
    }

    // Thin wall: two coincident faces with opposite normals; only the source
    // side (+Z) may receive the painting.
    {
        indexed_triangle_set wall_its;
        wall_its.vertices = source_its.vertices;
        wall_its.indices  = { stl_triangle_vertex_indices(0, 1, 2), stl_triangle_vertex_indices(0, 2, 1) };
        const TriangleMesh wall_mesh(wall_its);
        auto remapped = TriangleSelector::remap_painting(source_its, painting, wall_its, Transform3d::Identity(), std::nullopt);
        TriangleSelector selector(wall_mesh);
        selector.deserialize(remapped, false);
        // Face 0 (+Z winding) painted, face 1 (-Z winding) blank.
        const indexed_triangle_set painted0 = painted_facets(wall_mesh, remapped, state);
        CHECK_FALSE(painted0.indices.empty());
        TriangleSelector side_selector(wall_mesh);
        side_selector.deserialize(remapped, false);
        CHECK(side_selector.has_facets(state));
        // The far side of the thin wall must not get painted: rebuild the wall
        // with only the flipped face and confirm the remap stays blank.
        indexed_triangle_set flipped_only;
        flipped_only.vertices = source_its.vertices;
        flipped_only.indices  = { stl_triangle_vertex_indices(0, 2, 1) };
        const TriangleMesh flipped_mesh(flipped_only);
        auto flipped_remap = TriangleSelector::remap_painting(source_its, painting, flipped_only, Transform3d::Identity(), std::nullopt);
        CHECK(painted_facets(flipped_mesh, flipped_remap, state).indices.empty());
    }
}

// 10 x 10 mm square split along its diagonal into two triangles (faces 0 and 1).
static indexed_triangle_set diagonal_square()
{
    indexed_triangle_set its;
    its.vertices = { Vec3f(0.f, 0.f, 0.f), Vec3f(10.f, 0.f, 0.f), Vec3f(10.f, 10.f, 0.f), Vec3f(0.f, 10.f, 0.f) };
    its.indices  = { stl_triangle_vertex_indices(0, 1, 2), stl_triangle_vertex_indices(0, 2, 3) };
    return its;
}

// Review item 8: reloading the SAME mesh (identical vertices, faces and corner
// order, identity transform) used to grow the painted area on every pass
// (review measured 50.0000 -> 50.3899 -> 50.7782 mm^2) because blank sub-leaves
// touching the painted face's shared edge were assigned at the subdivision
// limit. Identical reloads now short-circuit and keep the serialized painting
// unchanged, for all four painting classes.
TEST_CASE("Paint remap: identical reload keeps the painting and the 50 mm^2 half unchanged", "[TriangleSelector][PaintRemap]")
{
    const indexed_triangle_set its         = diagonal_square();
    const TriangleMesh         target_mesh(its);
    const std::vector<EnforcerBlockerType> states = {
        EnforcerBlockerType::ENFORCER, // supports
        EnforcerBlockerType::BLOCKER,  // seams
        EnforcerBlockerType::Extruder3, // multi-material color
        EnforcerBlockerType::Extruder4, // second color (fuzzy uses ENFORCER/BLOCKER only)
    };
    for (const EnforcerBlockerType state : states) {
        INFO("state " << int(state));
        TriangleSelector source_selector(target_mesh);
        source_selector.set_facet(0, state); // paint exactly one half (50 mm^2)
        auto painting = source_selector.serialize();
        REQUIRE(TriangleSelector::has_facets(painting, state));

        TriangleSelector::TriangleSplittingData current = painting;
        for (int round = 0; round < 3; ++round) {
            TriangleSelector::RemapStats stats;
            current = TriangleSelector::remap_painting(its, painting, target_mesh.its, Transform3d::Identity(), std::nullopt, &stats);
            REQUIRE(TriangleSelector::has_facets(current, state));
            CHECK(stats.identity_path);
            // Serialized content must not change across identical reloads.
            CHECK(current.triangles_to_split == painting.triangles_to_split);
            CHECK(current.bitstream == painting.bitstream);
            const auto [area, centroid] = painted_area_and_centroid(painted_facets(target_mesh, current, state));
            CHECK_THAT(area, WithinAbs(50.f, 1e-3f));
            CHECK_THAT(centroid.x(), WithinAbs(20.f / 3.f, 0.01f));
            CHECK_THAT(centroid.y(), WithinAbs(10.f / 3.f, 0.01f));
        }
    }
}

// Review item 8 (general path): the same geometry with a changed face order and
// permuted corners must not paint the blank neighbor through their shared edge.
TEST_CASE("Paint remap: blank neighbor stays blank through remeshed shared edge", "[TriangleSelector][PaintRemap]")
{
    const indexed_triangle_set source_its = diagonal_square();
    const TriangleMesh         source_mesh(source_its);
    const EnforcerBlockerType  state      = EnforcerBlockerType::ENFORCER;

    TriangleSelector source_selector(source_mesh);
    source_selector.set_facet(0, state); // paint exactly one half
    auto painting = source_selector.serialize();

    // Target: same square, faces reordered and corners rotated within the faces.
    indexed_triangle_set permuted;
    permuted.vertices = source_its.vertices;
    permuted.indices  = { stl_triangle_vertex_indices(2, 3, 0),   // blank half first, rotated corners
                          stl_triangle_vertex_indices(1, 2, 0) }; // painted half, rotated corners
    const TriangleMesh permuted_mesh(permuted);

    auto remapped = TriangleSelector::remap_painting(source_its, painting, permuted, Transform3d::Identity(), std::nullopt);
    REQUIRE_FALSE(remapped.bitstream.empty());
    const auto [area, centroid] = painted_area_and_centroid(painted_facets(permuted_mesh, remapped, state));
    INFO("permuted target: area " << area << ", centroid " << centroid.transpose());
    INFO("permuted target: area " << area << ", centroid " << centroid.transpose());
    // The painted half survives; its area must not grow into the blank half
    // (any painting on the blank half would push the area above 50 mm^2 and
    // shift the centroid towards its corner (0, 10)).
    CHECK_THAT(area, WithinAbs(50.f, 0.05f));
    CHECK_THAT(centroid.x(), WithinAbs(20.f / 3.f, 0.05f));
    CHECK_THAT(centroid.y(), WithinAbs(10.f / 3.f, 0.05f));
}

// Axis-aligned box [x0, x1] x [0, y1] x [0, z1] with consistent outward normals;
// the x = x0 end cap is faces 8 and 9.
static indexed_triangle_set box_mesh(float x0, float x1, float y1 = 10.f, float z1 = 10.f)
{
    indexed_triangle_set its;
    its.vertices = { Vec3f(x0, 0.f, 0.f),   Vec3f(x1, 0.f, 0.f),   Vec3f(x1, y1, 0.f),  Vec3f(x0, y1, 0.f),
                     Vec3f(x0, 0.f, z1),    Vec3f(x1, 0.f, z1),    Vec3f(x1, y1, z1),   Vec3f(x0, y1, z1) };
    its.indices  = { stl_triangle_vertex_indices(0, 2, 1), stl_triangle_vertex_indices(0, 3, 2), // bottom (-Z)
                     stl_triangle_vertex_indices(4, 5, 6), stl_triangle_vertex_indices(4, 6, 7), // top (+Z)
                     stl_triangle_vertex_indices(0, 5, 4), stl_triangle_vertex_indices(0, 1, 5), // front (-Y)
                     stl_triangle_vertex_indices(3, 6, 2), stl_triangle_vertex_indices(3, 7, 6), // back (+Y)
                     stl_triangle_vertex_indices(0, 7, 3), stl_triangle_vertex_indices(0, 4, 7), // left cap (-X)
                     stl_triangle_vertex_indices(1, 6, 5), stl_triangle_vertex_indices(1, 2, 6) }; // right cap (+X)
    return its;
}

// Review item 6: after replacing a [0,20] model with the same file origin
// extended to [0,40], the painting was detached from the x=0 end face because
// the old mesh was given the NEW mesh's centering offset (init shift) while the
// target mesh was shifted again. The relative transform now derives from the
// two init shifts (and the unit conversion) only; the old mesh is never
// re-shifted.
TEST_CASE("Paint relink: one-sided extension keeps the painting on the shared end face", "[TriangleSelector][PaintRemap][Model]")
{
    const EnforcerBlockerType state = EnforcerBlockerType::ENFORCER;

    // Old volume: file coords [0,20], init shift = bbox center (10, 5, 5).
    const indexed_triangle_set old_file = box_mesh(0.f, 20.f);
    const Vec3d                shift_old(10., 5., 5.);
    TriangleMesh               old_mesh(old_file);
    old_mesh.translate(-shift_old.cast<float>());
    old_mesh.set_init_shift(shift_old);

    Model        model;
    ModelObject* obj = model.add_object();
    ModelVolume* old_volume = obj->add_volume(old_mesh, /*modify_to_center_geometry=*/false);
    obj->add_instance();

    TriangleSelector paint_selector(old_volume->mesh());
    paint_selector.set_facet(8, state);
    paint_selector.set_facet(9, state);
    old_volume->mmu_segmentation_facets.set(paint_selector);
    REQUIRE(old_volume->is_any_painted());

    auto saved_painting = old_volume->save_painting();
    REQUIRE(saved_painting.has_value());

    // New volume: same file origin extended to [0,40], init shift (20, 5, 5).
    const indexed_triangle_set new_file = box_mesh(0.f, 40.f);
    const Vec3d                shift_new(20., 5., 5.);
    TriangleMesh               new_mesh(new_file);
    new_mesh.translate(-shift_new.cast<float>());
    new_mesh.set_init_shift(shift_new);

    ModelVolume* new_volume = obj->add_volume(new_mesh, /*modify_to_center_geometry=*/false);

    // The caller-side relink used by replace / reload from disk.
    const Transform3d target_to_source = ModelVolume::paint_relink_transform(
        saved_painting->mesh.get_init_shift(), new_volume->mesh().get_init_shift(), 1.0);
    new_volume->restore_painting(saved_painting, target_to_source);

    const indexed_triangle_set painted = new_volume->mmu_segmentation_facets.get_facets(*new_volume, state);
    REQUIRE_FALSE(painted.indices.empty());
    const auto [area, centroid] = painted_area_and_centroid(painted);
    INFO("replaced box: area " << area << ", centroid " << centroid.transpose());
    // The x = 0 cap lives at stored x = -20 in the new volume; it keeps the
    // painting (100 mm^2), and nothing else gets painted.
    CHECK_THAT(area, WithinAbs(100.f, 1.f));
    for (const Vec3f& v : painted.vertices)
        CHECK_THAT(v.x(), WithinAbs(-20.f, 1e-3f));
    // The new x = 40 end cap (stored x = +20) stays blank.
    for (const Vec3f& v : painted.vertices)
        CHECK(v.x() < 0.f);
}

// Review item 6 (unit conversion): the same replacement with the volume loaded
// from inches - the stored coordinates are scaled by 25.4 while the init shifts
// stay in file units, so the relink translation must be scaled as well.
TEST_CASE("Paint relink: unit conversion scales the relink translation", "[TriangleSelector][PaintRemap][Model]")
{
    const EnforcerBlockerType state = EnforcerBlockerType::BLOCKER;
    constexpr double          unit  = 25.4;

    const indexed_triangle_set old_file = box_mesh(0.f, 20.f);
    const Vec3d                shift_old(10., 5., 5.);
    TriangleMesh               old_mesh(old_file);
    old_mesh.translate(-shift_old.cast<float>());
    old_mesh.set_init_shift(shift_old);
    old_mesh.scale(float(unit)); // convert_from_imperial_units scales the stored mesh

    Model        model;
    ModelObject* obj = model.add_object();
    ModelVolume* old_volume = obj->add_volume(old_mesh, /*modify_to_center_geometry=*/false);
    obj->add_instance();

    TriangleSelector paint_selector(old_volume->mesh());
    paint_selector.set_facet(8, state);
    paint_selector.set_facet(9, state);
    old_volume->mmu_segmentation_facets.set(paint_selector);

    auto saved_painting = old_volume->save_painting();
    REQUIRE(saved_painting.has_value());

    const indexed_triangle_set new_file = box_mesh(0.f, 40.f);
    const Vec3d                shift_new(20., 5., 5.);
    TriangleMesh               new_mesh(new_file);
    new_mesh.translate(-shift_new.cast<float>());
    new_mesh.set_init_shift(shift_new);
    new_mesh.scale(float(unit));

    ModelVolume* new_volume = obj->add_volume(new_mesh, /*modify_to_center_geometry=*/false);

    const Transform3d target_to_source = ModelVolume::paint_relink_transform(
        saved_painting->mesh.get_init_shift(), new_volume->mesh().get_init_shift(), unit);
    new_volume->restore_painting(saved_painting, target_to_source);

    const indexed_triangle_set painted = new_volume->mmu_segmentation_facets.get_facets(*new_volume, state);
    REQUIRE_FALSE(painted.indices.empty());
    const auto [area, centroid] = painted_area_and_centroid(painted);
    INFO("imperial replaced box: area " << area << ", centroid " << centroid.transpose());
    CHECK_THAT(area, WithinAbs(100.f * float(unit) * float(unit), float(unit)));
    for (const Vec3f& v : painted.vertices)
        CHECK_THAT(v.x(), WithinAbs(-20.f * float(unit), 0.05f));
}

// Review item 1: global smooth (one Loop subdivision) used to clear ALL painting
// because Loop moves vertices and normals and the intersection-based remap found
// no coincident faces. The painting is now carried over the subdivision in the
// parent faces' barycentric domain, using the (source face, child ordinal)
// origins returned by smooth_triangle_mesh().
TEST_CASE("Paint remap: smooth keeps a fully painted cube painted on all 48 faces", "[TriangleSelector][PaintRemap][Smooth]")
{
    const indexed_triangle_set cube = box_mesh(0.f, 20.f, 20.f, 20.f);
    const TriangleMesh         mesh(cube);
    const EnforcerBlockerType  state = EnforcerBlockerType::ENFORCER;

    TriangleSelector source_selector(mesh);
    for (int f = 0; f < int(cube.indices.size()); ++f)
        source_selector.set_facet(f, state);
    auto painting = source_selector.serialize();
    REQUIRE(TriangleSelector::has_facets(painting, state));

    bool ok = false;
    std::vector<std::pair<int, int>> origins;
    const TriangleMesh smoothed = TriangleMeshDeal::smooth_triangle_mesh(mesh, ok, &origins);
    REQUIRE(ok);
    REQUIRE(smoothed.its.indices.size() == 48);
    REQUIRE(origins.size() == 48);
    for (const auto& [parent, child] : origins) {
        CHECK(parent >= 0);
        CHECK(parent < int(cube.indices.size()));
        CHECK((child >= 0 && child < 4));
    }

    auto remapped = TriangleSelector::remap_painting_smooth(cube, painting, smoothed.its, origins, std::nullopt);
    REQUIRE(TriangleSelector::has_facets(remapped, state));

    // All 48 faces inherit the color: the painted area equals the smoothed
    // cube's total surface area.
    const auto [area, centroid] = painted_area_and_centroid(painted_facets(smoothed, remapped, state));
    float total_area = 0.f;
    for (const auto& face : smoothed.its.indices) {
        const Vec3f& a = smoothed.its.vertices[face(0)];
        const Vec3f& b = smoothed.its.vertices[face(1)];
        const Vec3f& c = smoothed.its.vertices[face(2)];
        total_area += 0.5f * (b - a).cross(c - a).norm();
    }
    INFO("smooth full cube: painted " << area << " of " << total_area);
    CHECK_THAT(area, WithinAbs(total_area, total_area * 0.01f));
}

// Review item 1: painting only one parent face must be inherited by exactly its
// four children, nothing else.
TEST_CASE("Paint remap: smooth inherits a single painted parent to its four children only", "[TriangleSelector][PaintRemap][Smooth]")
{
    const indexed_triangle_set cube = box_mesh(0.f, 20.f, 20.f, 20.f);
    const TriangleMesh         mesh(cube);
    const EnforcerBlockerType  state = EnforcerBlockerType::Extruder3;

    TriangleSelector source_selector(mesh);
    source_selector.set_facet(2, state); // one triangle of the top face (z = 10)
    source_selector.set_facet(3, state); // its partner
    auto painting = source_selector.serialize();

    bool ok = false;
    std::vector<std::pair<int, int>> origins;
    const TriangleMesh smoothed = TriangleMeshDeal::smooth_triangle_mesh(mesh, ok, &origins);
    REQUIRE(ok);
    REQUIRE(smoothed.its.indices.size() == 48);

    auto remapped = TriangleSelector::remap_painting_smooth(cube, painting, smoothed.its, origins, std::nullopt);
    const indexed_triangle_set painted = painted_facets(smoothed, remapped, state);
    REQUIRE_FALSE(painted.indices.empty());
    const auto [area, centroid] = painted_area_and_centroid(painted);
    INFO("smooth one face: area " << area << ", centroid " << centroid.transpose());
    // The smoothed cube surface is 6 equal faces; the painted area is one of them.
    float total_area = 0.f;
    for (const auto& face : smoothed.its.indices) {
        const Vec3f& a = smoothed.its.vertices[face(0)];
        const Vec3f& b = smoothed.its.vertices[face(1)];
        const Vec3f& c = smoothed.its.vertices[face(2)];
        total_area += 0.5f * (b - a).cross(c - a).norm();
    }
    CHECK_THAT(area, WithinAbs(total_area / 6.f, total_area * 0.01f));
    // The painted region is the TOP face: every painted vertex sits well above
    // the cube's middle plane.
    for (const Vec3f& v : painted.vertices)
        CHECK(v.z() > 5.f);
}

// Review item 1: an asymmetric stroke crossing the child-region boundaries plus
// an erased hole keeps its position and continuity; blanks never expand. The
// fixture is a FLAT two-triangle sheet: Loop smoothing keeps a planar sheet
// planar, so the painted 3D area is directly comparable to the pre-smoothing
// painting (on a cube the corners round off and the surface shrinks, which
// would make such an area comparison meaningless).
TEST_CASE("Paint remap: smooth keeps a crossing stroke and an erased hole", "[TriangleSelector][PaintRemap][Smooth]")
{
    indexed_triangle_set sheet;
    sheet.vertices = { Vec3f(0.f, 0.f, 0.f), Vec3f(20.f, 0.f, 0.f), Vec3f(20.f, 20.f, 0.f), Vec3f(0.f, 20.f, 0.f) };
    sheet.indices  = { stl_triangle_vertex_indices(0, 1, 2), stl_triangle_vertex_indices(0, 2, 3) };
    const TriangleMesh mesh(sheet);
    const EnforcerBlockerType state = EnforcerBlockerType::Extruder4;

    TriangleSelector source_selector(mesh);
    // Asymmetric stroke at (11, 9): it crosses the diagonal between the two
    // source triangles and the medial lines of both parents' child regions.
    auto stroke = TriangleSelector::SinglePointCursor::cursor_factory(
        Vec3f(11.f, 9.f, 0.f), Vec3f(11.f, 9.f, 10.f), 2.0f, TriangleSelector::CursorType::CIRCLE,
        Transform3d::Identity(), TriangleSelector::ClippingPlane());
    source_selector.select_patch(0, std::move(stroke), state, Transform3d::Identity(), true);
    // Erase a hole inside the stroke.
    auto hole = TriangleSelector::SinglePointCursor::cursor_factory(
        Vec3f(12.f, 10.f, 0.f), Vec3f(12.f, 10.f, 10.f), 0.5f, TriangleSelector::CursorType::CIRCLE,
        Transform3d::Identity(), TriangleSelector::ClippingPlane());
    source_selector.select_patch(0, std::move(hole), EnforcerBlockerType::NONE, Transform3d::Identity(), true);
    auto painting = source_selector.serialize();
    REQUIRE(TriangleSelector::has_facets(painting, state));

    // Baseline: the painted area before smoothing (stroke minus hole).
    const auto [baseline_area, baseline_centroid] = painted_area_and_centroid(source_selector.get_facets(state));
    REQUIRE(baseline_area > 5.f);
    INFO("baseline stroke-hole area " << baseline_area);

    bool ok = false;
    std::vector<std::pair<int, int>> origins;
    const TriangleMesh smoothed = TriangleMeshDeal::smooth_triangle_mesh(mesh, ok, &origins);
    REQUIRE(ok);
    REQUIRE(smoothed.its.indices.size() == 8);
    REQUIRE(origins.size() == 8);

    auto remapped = TriangleSelector::remap_painting_smooth(sheet, painting, smoothed.its, origins, std::nullopt);
    const indexed_triangle_set painted = painted_facets(smoothed, remapped, state);
    REQUIRE_FALSE(painted.indices.empty());
    const auto [area, centroid] = painted_area_and_centroid(painted);
    INFO("smooth stroke: area " << area << " (baseline " << baseline_area << "), centroid " << centroid.transpose());
    // Continuity across the child boundaries: no gap may eat the stroke.
    CHECK_THAT(area, WithinAbs(baseline_area, baseline_area * 0.15f));
    // Blanks never expand: the remapped area does not grow beyond the source.
    CHECK(area <= baseline_area * 1.05f);
    // The hole stays a hole: nothing near its center (the brush resolution on
    // the source and the remap's own subdivision eat ~0.15 mm from the rim).
    for (const Vec3f& v : painted.vertices)
        CHECK((Vec2f(v.x(), v.y()) - Vec2f(12.f, 10.f)).norm() > 0.3f);
}

// Review item 1: duplicate input faces resolve to the FIRST occurrence, and an
// unreferenced vertex (compaction) does not break the origins.
TEST_CASE("Paint remap: smooth deduplicates faces with first-occurrence painting and compacts vertices", "[TriangleSelector][PaintRemap][Smooth]")
{
    indexed_triangle_set cube = box_mesh(0.f, 20.f, 20.f, 20.f);
    // Unreferenced vertex: dropped by the compaction inside smooth_triangle_mesh.
    cube.vertices.emplace_back(99.f, 99.f, 99.f);
    // Duplicate of face 4 (same corner set), appended at the end.
    const auto& f4 = cube.indices[4];
    cube.indices.push_back(stl_triangle_vertex_indices(f4(1), f4(2), f4(0))); // rotated copy, same set
    const TriangleMesh mesh(cube);

    const EnforcerBlockerType keep_state = EnforcerBlockerType::ENFORCER;
    const EnforcerBlockerType dup_state  = EnforcerBlockerType::Extruder3;

    TriangleSelector source_selector(mesh);
    source_selector.set_facet(4, keep_state);                      // first occurrence
    source_selector.set_facet(int(cube.indices.size()) - 1, dup_state); // dropped duplicate
    auto painting = source_selector.serialize();

    bool ok = false;
    std::vector<std::pair<int, int>> origins;
    const TriangleMesh smoothed = TriangleMeshDeal::smooth_triangle_mesh(mesh, ok, &origins);
    REQUIRE(ok);
    REQUIRE(smoothed.its.indices.size() == 48);
    REQUIRE(origins.size() == 48);
    // Every origin refers to a face of the ORIGINAL input (12 real faces).
    for (const auto& [parent, child] : origins)
        CHECK(parent < int(cube.indices.size()));

    auto remapped = TriangleSelector::remap_painting_smooth(cube, painting, smoothed.its, origins, std::nullopt);
    CHECK(TriangleSelector::has_facets(remapped, keep_state));
    // The dropped duplicate's painting must not leak into the output.
    CHECK_FALSE(TriangleSelector::has_facets(remapped, dup_state));
}

// Review item 5: cutting used to restore EVERY source snapshot onto EVERY
// output entity, so spatially overlapping parts stained each other. Plane cuts
// now remember which source volume each output entity was cut from, and only
// that source's painting is restored; newly generated faces (the cut section)
// have no painting source.
TEST_CASE("Paint remap: plane cut restores each part from its own source volume only", "[TriangleSelector][PaintRemap][Cut]")
{
    const EnforcerBlockerType state = EnforcerBlockerType::ENFORCER;

    // A: [0,12]^3, B: [6,18] x [0,12]^2 - overlapping [6,12] in X.
    Model        model;
    ModelObject* obj = model.add_object();
    obj->add_volume(TriangleMesh(box_mesh(0.f, 12.f, 12.f, 12.f)), /*modify_to_center_geometry=*/false);
    obj->add_volume(TriangleMesh(box_mesh(6.f, 18.f, 12.f, 12.f)), /*modify_to_center_geometry=*/false);
    obj->add_instance();

    // Paint ONLY A's top face (z = 12, faces 2 and 3 of the first volume).
    TriangleSelector paint_selector(obj->volumes[0]->mesh());
    paint_selector.set_facet(2, state);
    paint_selector.set_facet(3, state);
    obj->volumes[0]->mmu_segmentation_facets.set(paint_selector);
    REQUIRE(obj->volumes[0]->is_any_painted());
    REQUIRE_FALSE(obj->volumes[1]->is_any_painted());

    const ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                                                ModelObjectCutAttribute::KeepAsParts | ModelObjectCutAttribute::KeepPaint;
    Cut cut(obj, 0, Geometry::translation_transform(Vec3d(0., 0., 6.)), attributes);
    const ModelObjectPtrs& results = cut.perform_with_plane();
    REQUIRE(results.size() == 1);
    const ModelObject* cut_object = results.front();
    REQUIRE(cut_object->volumes.size() == 4); // A upper, A lower, B upper, B lower

    const auto painted_of = [&](int idx, EnforcerBlockerType st) {
        return painted_area_and_centroid(cut_object->volumes[idx]->mmu_segmentation_facets.get_facets(*cut_object->volumes[idx], st));
    };

    // A's upper part keeps the top face (12 x 12 = 144 mm^2).
    const auto [a_upper_area, a_upper_centroid] = painted_of(0, state);
    INFO("A upper: area " << a_upper_area << ", centroid " << a_upper_centroid.transpose());
    CHECK_THAT(a_upper_area, WithinAbs(144.f, 1.f));
    CHECK(a_upper_centroid.z() > 0.f); // stored coords: the volume was re-centered by add_volume()
    // A's lower part, and BOTH parts of B, must stay completely blank.
    for (const int idx : { 1, 2, 3 }) {
        const indexed_triangle_set painted = cut_object->volumes[idx]->mmu_segmentation_facets.get_facets(*cut_object->volumes[idx], state);
        INFO("volume " << idx << " painted faces: " << painted.indices.size());
        CHECK(painted.indices.empty());
    }
}

// Review item 5 (colors + merged path): two volumes painted with different
// colors keep their own color after a contour cut, whose merge path rebases the
// per-face painting data of each part onto the merged mesh.
TEST_CASE("Paint remap: contour cut keeps each part's own color through the merge", "[TriangleSelector][PaintRemap][Cut]")
{
    const EnforcerBlockerType color_a = EnforcerBlockerType::Extruder3;
    const EnforcerBlockerType color_b = EnforcerBlockerType::Extruder4;

    Model        model;
    ModelObject* obj = model.add_object();
    obj->add_volume(TriangleMesh(box_mesh(0.f, 12.f, 12.f, 12.f)), /*modify_to_center_geometry=*/false);
    obj->add_volume(TriangleMesh(box_mesh(6.f, 18.f, 12.f, 12.f)), /*modify_to_center_geometry=*/false);
    obj->add_instance();

    TriangleSelector paint_a(obj->volumes[0]->mesh());
    paint_a.set_facet(2, color_a);
    paint_a.set_facet(3, color_a);
    obj->volumes[0]->mmu_segmentation_facets.set(paint_a);

    TriangleSelector paint_b(obj->volumes[1]->mesh());
    paint_b.set_facet(2, color_b);
    paint_b.set_facet(3, color_b);
    obj->volumes[1]->mmu_segmentation_facets.set(paint_b);

    const ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                                                ModelObjectCutAttribute::KeepPaint;
    std::vector<Cut::Part> parts = { { true, false }, { true, false } };
    Cut cut(obj, 0, Geometry::translation_transform(Vec3d(0., 0., 6.)), attributes);
    const ModelObjectPtrs& results = cut.perform_by_contour(obj, parts, 0);
    REQUIRE(results.size() == 1);
    const ModelObject* cut_object = results.front();
    REQUIRE_FALSE(cut_object->volumes.empty());

    const ModelVolume* merged = cut_object->volumes.front();
    // A's color survives on the merged mesh, at A's top position (x ~ [0,12]).
    const auto [area_a, centroid_a] = painted_area_and_centroid(merged->mmu_segmentation_facets.get_facets(*merged, color_a));
    INFO("merged A color: area " << area_a << ", centroid " << centroid_a.transpose());
    CHECK_THAT(area_a, WithinAbs(144.f, 1.f));
    CHECK(centroid_a.z() > 0.f);
    // B's color survives at B's top position (x ~ [6,18]), and neither color
    // bleeds onto the other volume's region. The merged volume was re-centered
    // by add_volume(), so the absolute positions are in the stored mesh's
    // coordinates - only the separation and the areas are asserted.
    const auto [area_b, centroid_b] = painted_area_and_centroid(merged->mmu_segmentation_facets.get_facets(*merged, color_b));
    INFO("merged B color: area " << area_b << ", centroid " << centroid_b.transpose());
    CHECK_THAT(area_b, WithinAbs(144.f, 1.f));
    CHECK(centroid_b.z() > 0.f);
    CHECK(centroid_a.x() < centroid_b.x());
    CHECK_THAT(centroid_b.x() - centroid_a.x(), WithinAbs(6.f, 0.5f));
}

// Review item 5: a mirrored instance must keep the painting on its own part
// (the composite instance*volume transform is handled once, on the remap side).
TEST_CASE("Paint remap: plane cut with a mirrored instance keeps the painting on its part", "[TriangleSelector][PaintRemap][Cut]")
{
    const EnforcerBlockerType state = EnforcerBlockerType::BLOCKER;

    Model        model;
    ModelObject* obj = model.add_object();
    obj->add_volume(TriangleMesh(box_mesh(0.f, 12.f, 12.f, 12.f)), /*modify_to_center_geometry=*/false);
    obj->add_volume(TriangleMesh(box_mesh(6.f, 18.f, 12.f, 12.f)), /*modify_to_center_geometry=*/false);
    obj->add_instance();
    obj->instances.front()->set_transformation(Geometry::Transformation(Geometry::scale_transform(Vec3d(-1., 1., 1.))));

    TriangleSelector paint_selector(obj->volumes[0]->mesh());
    paint_selector.set_facet(2, state);
    paint_selector.set_facet(3, state);
    obj->volumes[0]->mmu_segmentation_facets.set(paint_selector);

    const ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                                                ModelObjectCutAttribute::KeepAsParts | ModelObjectCutAttribute::KeepPaint;
    Cut cut(obj, 0, Geometry::translation_transform(Vec3d(0., 0., 6.)), attributes);
    const ModelObjectPtrs& results = cut.perform_with_plane();
    REQUIRE(results.size() == 1);
    const ModelObject* cut_object = results.front();
    REQUIRE(cut_object->volumes.size() == 4);

    const auto [a_upper_area, a_upper_centroid] =
        painted_area_and_centroid(cut_object->volumes[0]->mmu_segmentation_facets.get_facets(*cut_object->volumes[0], state));
    INFO("mirrored A upper: area " << a_upper_area << ", centroid " << a_upper_centroid.transpose());
    CHECK_THAT(a_upper_area, WithinAbs(144.f, 1.f));
    for (const int idx : { 1, 2, 3 }) {
        const indexed_triangle_set painted = cut_object->volumes[idx]->mmu_segmentation_facets.get_facets(*cut_object->volumes[idx], state);
        CHECK(painted.indices.empty());
    }
}


// Review item 9: the Windows repair worker used to write each repaired mesh
// back per volume and remap the painting afterwards, while the UI kept
// accepting cancel - a cancel (or a failure in a later volume) then returned
// "canceled" with some volumes already written back. The commit is now
// two-phase (prepare all painting remaps with cancel checks, final cancel
// check, then apply everything together), driven by the SAME production helper
// the worker calls.
namespace {

struct VolumeSnapshot {
    indexed_triangle_set                        its;
    TriangleSelector::TriangleSplittingData     painting;
    const TriangleMesh*                         hull = nullptr; // convex hull object identity
    ObjectID                                    id;
};

VolumeSnapshot snapshot_volume(const ModelVolume* volume)
{
    return { volume->mesh().its, volume->mmu_segmentation_facets.get_data(),
             volume->get_convex_hull_shared_ptr().get(), volume->id() };
}

// The commit is two-phase: any cancel or failure BEFORE the final check must
// leave the mesh, the painting, the convex hull (same object identity, i.e.
// no recompute happened) and the volume id completely untouched.
bool volume_unchanged(const ModelVolume* volume, const VolumeSnapshot& snapshot)
{
    return volume->mesh().its.vertices == snapshot.its.vertices && volume->mesh().its.indices == snapshot.its.indices &&
           volume->mmu_segmentation_facets.get_data().bitstream == snapshot.painting.bitstream &&
           volume->mmu_segmentation_facets.get_data().triangles_to_split == snapshot.painting.triangles_to_split &&
           volume->get_convex_hull_shared_ptr().get() == snapshot.hull && volume->id() == snapshot.id;
}

// The commit phase must be non-throwing BY CONTRACT: everything that allocates
// or computes (mesh wrapping, convex hull, painting remap) runs in the prepare
// phase before the real volumes are touched. This compile-time check encodes
// that guarantee - if any commit step regains an allocation or a computation,
// this static_assert breaks the build.
static_assert(noexcept(std::declval<ModelVolume&>().commit_prepared_repair(
    std::declval<std::shared_ptr<const TriangleMesh>>(), std::declval<std::shared_ptr<const TriangleMesh>>(),
    std::declval<std::array<std::optional<TriangleSelector::TriangleSplittingData>, 4>&&>())),
    "ModelVolume::commit_prepared_repair must stay noexcept: all fallible work belongs to the prepare phase");

ModelObject* make_two_painted_boxes(Model& model)
{
    ModelObject* obj = model.add_object();
    obj->add_volume(TriangleMesh(box_mesh(0.f, 12.f, 12.f, 12.f)), /*modify_to_center_geometry=*/false);
    obj->add_volume(TriangleMesh(box_mesh(6.f, 18.f, 12.f, 12.f)), /*modify_to_center_geometry=*/false);
    obj->add_instance();
    const EnforcerBlockerType state = EnforcerBlockerType::ENFORCER;
    for (ModelVolume* volume : obj->volumes) {
        TriangleSelector selector(volume->mesh());
        selector.set_facet(2, state);
        selector.set_facet(3, state);
        volume->mmu_segmentation_facets.set(selector);
    }
    return obj;
}

} // namespace

TEST_CASE("Paint commit: cancel or failure keeps every volume and painting untouched", "[TriangleSelector][PaintRemap][Model]")
{
    // A cancel_check that fires on the Nth invocation (in the prepare phase).
    auto make_cancel_at = [](int fire_at) {
        auto counter = std::make_shared<int>(0);
        return [counter, fire_at]() {
            ++*counter;
            return *counter >= fire_at;
        };
    };

    // Cancel while the FIRST volume is being prepared.
    {
        Model model;
        ModelObject* obj = make_two_painted_boxes(model);
        auto snap_a = snapshot_volume(obj->volumes[0]);
        auto snap_b = snapshot_volume(obj->volumes[1]);
        std::vector<ModelObject::RepairCommitItem> items;
        for (ModelVolume* volume : obj->volumes) {
            TriangleMesh repaired = volume->mesh();
            repaired.translate(Vec3f(1.f, 0.f, 0.f));
            items.push_back({ volume, std::move(repaired) });
        }
        const auto result = obj->commit_repaired_meshes_with_painting(items, true, make_cancel_at(1), nullptr);
        CHECK(result == ModelObject::RepairCommitResult::Canceled);
        CHECK(volume_unchanged(obj->volumes[0], snap_a));
        CHECK(volume_unchanged(obj->volumes[1], snap_b));
    }

    // Cancel while the SECOND volume is being prepared (after the first
    // volume's remap data was computed).
    {
        Model model;
        ModelObject* obj = make_two_painted_boxes(model);
        auto snap_a = snapshot_volume(obj->volumes[0]);
        auto snap_b = snapshot_volume(obj->volumes[1]);
        std::vector<ModelObject::RepairCommitItem> items;
        for (ModelVolume* volume : obj->volumes) {
            TriangleMesh repaired = volume->mesh();
            repaired.translate(Vec3f(1.f, 0.f, 0.f));
            items.push_back({ volume, std::move(repaired) });
        }
        const auto result = obj->commit_repaired_meshes_with_painting(items, true, make_cancel_at(6), nullptr);
        CHECK(result == ModelObject::RepairCommitResult::Canceled);
        CHECK(volume_unchanged(obj->volumes[0], snap_a));
        CHECK(volume_unchanged(obj->volumes[1], snap_b));
    }

    // Cancel at the final pre-commit check: everything was prepared, nothing
    // may be written.
    {
        Model model;
        ModelObject* obj = make_two_painted_boxes(model);
        auto snap_a = snapshot_volume(obj->volumes[0]);
        auto snap_b = snapshot_volume(obj->volumes[1]);
        std::vector<ModelObject::RepairCommitItem> items;
        for (ModelVolume* volume : obj->volumes) {
            TriangleMesh repaired = volume->mesh();
            repaired.translate(Vec3f(1.f, 0.f, 0.f));
            items.push_back({ volume, std::move(repaired) });
        }
        // fire on the 11th call: the FINAL pre-commit check (2 volumes x 5 prepare checks + the final one).
        const auto result = obj->commit_repaired_meshes_with_painting(items, true, make_cancel_at(11), nullptr);
        CHECK(volume_unchanged(obj->volumes[0], snap_a));
        CHECK(volume_unchanged(obj->volumes[1], snap_b));
    }

    // The second volume's repair fails: the first volume's prepared result must
    // not be written back either.
    {
        Model model;
        ModelObject* obj = make_two_painted_boxes(model);
        auto snap_a = snapshot_volume(obj->volumes[0]);
        auto snap_b = snapshot_volume(obj->volumes[1]);
        std::vector<ModelObject::RepairCommitItem> items;
        {
            TriangleMesh repaired = obj->volumes[0]->mesh();
            repaired.translate(Vec3f(1.f, 0.f, 0.f));
            items.push_back({ obj->volumes[0], std::move(repaired) });
        }
        items.push_back({ obj->volumes[1], TriangleMesh() }); // failed repair
        std::string error;
        const auto result = obj->commit_repaired_meshes_with_painting(items, true, []() { return false; }, &error);
        CHECK(result == ModelObject::RepairCommitResult::Failed);
        CHECK_FALSE(error.empty());
        CHECK(volume_unchanged(obj->volumes[0], snap_a));
        CHECK(volume_unchanged(obj->volumes[1], snap_b));
    }
}

TEST_CASE("Paint commit: normal completion updates every volume together", "[TriangleSelector][PaintRemap][Model]")
{
    const EnforcerBlockerType state = EnforcerBlockerType::ENFORCER;

    Model model;
    ModelObject* obj = make_two_painted_boxes(model);

    std::vector<ModelObject::RepairCommitItem> items;
    for (ModelVolume* volume : obj->volumes) {
        TriangleMesh repaired = volume->mesh(); // the repair kept the geometry
        items.push_back({ volume, std::move(repaired) });
    }

    const auto result = obj->commit_repaired_meshes_with_painting(items, true, []() { return false; }, nullptr);
    CHECK(result == ModelObject::RepairCommitResult::Committed);

    // Both volumes carry the repaired mesh and the painting followed the mesh.
    for (ModelVolume* volume : obj->volumes) {
        const auto [area, centroid] = painted_area_and_centroid(volume->mmu_segmentation_facets.get_facets(*volume, state));
        INFO("committed volume area " << area << ", centroid " << centroid.transpose());
        CHECK_THAT(area, WithinAbs(144.f, 1.f)); // the painted top face survives the repair
        CHECK(centroid.z() > 0.f);
        // The convex hull was recomputed from the repaired mesh in the PREPARE
        // phase and committed together with the mesh: it must equal the convex
        // hull of the repaired geometry.
        const TriangleMesh expected_hull(volume->mesh().convex_hull_3d());
        CHECK(volume->get_convex_hull().its.vertices == expected_hull.its.vertices);
    }
    // The repaired meshes replaced the original ones.
    CHECK(obj->volumes[0]->mesh().its.vertices == box_mesh(0.f, 12.f, 12.f, 12.f).vertices);
}

// Review item 9 (commit boundary): a cancel REQUEST that arrives while the
// commit is already running (i.e. only after the final pre-commit check) must
// not stop the commit: the prepare phase consults the check exactly 11 times
// for two painted volumes (2 x (1 entry + 4 annotations) + 1 final), the 12th
// call would report the cancel but is never made - the commit is already
// moving precomputed data only. The returned status must match what was
// written: Committed with every volume updated.
TEST_CASE("Paint commit: a cancel request racing the commit reports committed", "[TriangleSelector][PaintRemap][Model]")
{
    Model model;
    ModelObject* obj = make_two_painted_boxes(model);
    auto snap_a = snapshot_volume(obj->volumes[0]);
    auto snap_b = snapshot_volume(obj->volumes[1]);

    auto counter = std::make_shared<int>(0);
    auto cancel_requested = std::make_shared<bool>(false);
    // Flip the request right after the 11th consultation (the final pre-commit
    // check): the commit has already started and must not consult it again.
    auto check = [counter, cancel_requested]() {
        ++*counter;
        if (*counter >= 12)
            *cancel_requested = true;
        return *cancel_requested;
    };

    std::vector<ModelObject::RepairCommitItem> items;
    for (ModelVolume* volume : obj->volumes) {
        TriangleMesh repaired = volume->mesh(); // the repair kept the geometry
        items.push_back({ volume, std::move(repaired) });
    }

    const auto result = obj->commit_repaired_meshes_with_painting(items, true, check, nullptr);
    CHECK(result == ModelObject::RepairCommitResult::Committed);
    // The checker was consulted exactly through the prepare phase and the final
    // check (11 calls) and the cancel request only became visible afterwards.
    CHECK(*counter == 11);
    CHECK(*cancel_requested == false);
    // The model matches the reported status: both volumes committed (mesh,
    // hull and id changed), painting preserved.
    CHECK_FALSE(volume_unchanged(obj->volumes[0], snap_a));
    CHECK_FALSE(volume_unchanged(obj->volumes[1], snap_b));
    for (ModelVolume* volume : obj->volumes) {
        const auto [area, centroid] = painted_area_and_centroid(volume->mmu_segmentation_facets.get_facets(*volume, EnforcerBlockerType::ENFORCER));
        CHECK_THAT(area, WithinAbs(144.f, 1.f));
        CHECK(centroid.z() > 0.f);
    }
}
// Review item 3: mirroring (volume mirror, instance mirror, or both) used to
// pre-transform the snapshot mesh with transform(..., true), which swaps face
// corners 1 and 2 for negative determinants while the painting tree is still
// decoded against the old corner order - a local stroke landed on another
// corner (measured (-2, 11) instead of (-11, 2) on this fixture).
// New contract: the snapshot keeps its original corner order and the relative
// transform is applied once, to the target mesh, inside remap_painting().
TEST_CASE("Paint remap: mirrored boolean/cut keeps a local stroke on its corner", "[TriangleSelector][PaintRemap]")
{
    // 16 mm right triangle on the XY plane, stroke near (11, 2).
    indexed_triangle_set its;
    its.vertices = { Vec3f(0.f, 0.f, 0.f), Vec3f(16.f, 0.f, 0.f), Vec3f(0.f, 16.f, 0.f) };
    its.indices  = { stl_triangle_vertex_indices(0, 1, 2) };
    const TriangleMesh mesh(its);
    const EnforcerBlockerType state = EnforcerBlockerType::ENFORCER;

    auto painting = paint_brush_stroke(mesh, Vec3f(11.f, 2.f, 0.f), 1.0f, state);
    REQUIRE(TriangleSelector::has_facets(painting, state));

    // The boolean/cut output mesh: the input transformed by the volume matrix
    // (here an X mirror) with its own handedness fix - its stored winding is
    // positive, which remap_painting() must undo exactly once.
    const Transform3d mirror_x = scale_transform(Vec3d(-1., 1., 1.));
    TriangleMesh      target(its);
    target.transform(mirror_x, true);

    auto remapped = TriangleSelector::remap_painting(mesh.its, painting, target.its, mirror_x, std::nullopt);
    REQUIRE_FALSE(remapped.bitstream.empty());

    const auto [area, centroid] = painted_area_and_centroid(painted_facets(target, remapped, state));
    CHECK(area > 0.f);
    INFO("painted area " << area << ", centroid " << centroid.transpose());
    // The mirrored stroke must sit at (-11, 2), not at the swapped corner (-2, 11).
    CHECK_THAT(centroid.x(), WithinAbs(-11.f, 0.6f));
    CHECK_THAT(centroid.y(), WithinAbs(2.f, 0.6f));
    CHECK_FALSE((centroid.x() > -2.6f && centroid.x() < -1.4f));
}

// Review item 3: volume mirror + instance mirror combined (positive determinant
// of the composite transform) must move the stroke WITH the geometry instead of
// flipping it again: the composite maps (11, 2) to (-11, -2), while a spurious
// corner swap would decode it at (-2, -11).
TEST_CASE("Paint remap: combined volume and instance mirror keeps the stroke position", "[TriangleSelector][PaintRemap]")
{
    indexed_triangle_set its;
    its.vertices = { Vec3f(0.f, 0.f, 0.f), Vec3f(16.f, 0.f, 0.f), Vec3f(0.f, 16.f, 0.f) };
    its.indices  = { stl_triangle_vertex_indices(0, 1, 2) };
    const TriangleMesh mesh(its);
    const EnforcerBlockerType state = EnforcerBlockerType::BLOCKER;

    auto painting = paint_brush_stroke(mesh, Vec3f(11.f, 2.f, 0.f), 1.0f, state);

    // Composite pipeline transform with positive determinant (two mirrors).
    const Transform3d mirror_x  = scale_transform(Vec3d(-1., 1., 1.));
    const Transform3d mirror_y  = scale_transform(Vec3d(1., -1., 1.));
    const Transform3d composite = mirror_x * mirror_y;
    TriangleMesh      target(its);
    target.transform(composite, true);

    auto remapped = TriangleSelector::remap_painting(mesh.its, painting, target.its, composite.inverse(), std::nullopt);
    REQUIRE_FALSE(remapped.bitstream.empty());
    const auto [area, centroid] = painted_area_and_centroid(painted_facets(target, remapped, state));
    CHECK_THAT(centroid.x(), WithinAbs(-11.f, 0.6f));
    CHECK_THAT(centroid.y(), WithinAbs(-2.f, 0.6f));
    CHECK_FALSE((centroid.x() > -2.6f && centroid.x() < -1.4f)); // not the swapped-corner decode
}

// Review item 3 (auxiliary): a double mirror along the same axis is the identity,
// so the stroke must return to its original position.
TEST_CASE("Paint remap: double mirror restores the original stroke position", "[TriangleSelector][PaintRemap]")
{
    indexed_triangle_set its;
    its.vertices = { Vec3f(0.f, 0.f, 0.f), Vec3f(16.f, 0.f, 0.f), Vec3f(0.f, 16.f, 0.f) };
    its.indices  = { stl_triangle_vertex_indices(0, 1, 2) };
    const TriangleMesh mesh(its);
    const EnforcerBlockerType state = EnforcerBlockerType::Extruder3;

    auto painting = paint_brush_stroke(mesh, Vec3f(11.f, 2.f, 0.f), 1.0f, state);

    const Transform3d mirror_x = scale_transform(Vec3d(-1., 1., 1.));
    TriangleMesh      target(its);
    target.transform(mirror_x * mirror_x, true);

    auto remapped = TriangleSelector::remap_painting(mesh.its, painting, target.its, mirror_x * mirror_x, std::nullopt);
    REQUIRE_FALSE(remapped.bitstream.empty());
    const auto [area, centroid] = painted_area_and_centroid(painted_facets(target, remapped, state));
    CHECK_THAT(centroid.x(), WithinAbs(11.f, 0.6f));
    CHECK_THAT(centroid.y(), WithinAbs(2.f, 0.6f));
}
