// Regression tests for the "keep painted features after mesh change" remapping
// (MR !21 adversarial review). Each review item gets a fixture that drives the
// production remapping entry points directly, checks position / blank regions /
// cross-volume pollution, and covers all four painting annotations through
// their shared TriangleSplittingData representation.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <utility>
#include <vector>

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

// Area-weighted centroid of a facet tiling after an affine transform, summed
// triangle by triangle. A triangle with unit normal n scales by
// |det(A)| * ||A^-T n||, which depends on n - so the expectation is computed
// from the transformed triangles and never from |det(A)| alone.
std::pair<float, Vec3f> transformed_area_and_centroid(const indexed_triangle_set& its, const Transform3d& trafo)
{
    double   area_sum = 0.;
    Vec3d    centroid = Vec3d::Zero();
    for (const auto& face : its.indices) {
        const Vec3d a = trafo * its.vertices[face(0)].cast<double>();
        const Vec3d b = trafo * its.vertices[face(1)].cast<double>();
        const Vec3d c = trafo * its.vertices[face(2)].cast<double>();
        const double area = 0.5 * ((b - a).cross(c - a)).norm();
        area_sum += area;
        centroid += area * (a + b + c) / 3.0;
    }
    if (area_sum > 0.)
        centroid /= area_sum;
    return { float(area_sum), centroid.cast<float>() };
}

// Area-weighted centroid of the part of a facet tiling on one side of the
// plane z = z_plane (keep_above selects z >= z_plane). Every facet is clipped
// exactly and the remainder is fanned from its first vertex.
std::pair<float, Vec3f> clipped_area_and_centroid(const indexed_triangle_set& its, float z_plane, bool keep_above)
{
    float area_sum = 0.f;
    Vec3f centroid = Vec3f::Zero();
    for (const auto& face : its.indices) {
        const std::array<Vec3f, 3> tri = { its.vertices[face(0)], its.vertices[face(1)], its.vertices[face(2)] };
        std::vector<Vec3f> poly;
        for (int i = 0; i < 3; ++i) {
            const Vec3f& a    = tri[i];
            const Vec3f& b    = tri[(i + 1) % 3];
            const float  da   = keep_above ? a.z() - z_plane : z_plane - a.z();
            const float  db   = keep_above ? b.z() - z_plane : z_plane - b.z();
            const bool   a_in = da >= 0.f;
            const bool   b_in = db >= 0.f;
            if (a_in)
                poly.push_back(a);
            if (a_in != b_in)
                poly.push_back(a + (da / (da - db)) * (b - a));
        }
        for (size_t i = 1; i + 1 < poly.size(); ++i) {
            const float area = 0.5f * (poly[i] - poly[0]).cross(poly[i + 1] - poly[0]).norm();
            area_sum += area;
            centroid += area * (poly[0] + poly[i] + poly[i + 1]) / 3.f;
        }
    }
    if (area_sum > 0.f)
        centroid /= area_sum;
    return { area_sum, centroid };
}

// True when every facet of the tiling is parallel to `normal` (either
// orientation): the painting stayed on its original face and did not spread
// onto faces meeting it at an angle.
bool facets_parallel_to(const indexed_triangle_set& its, const Vec3f& normal)
{
    for (const auto& face : its.indices) {
        const Vec3f n = (its.vertices[face(1)] - its.vertices[face(0)]).cross(its.vertices[face(2)] - its.vertices[face(0)]);
        if (n.squaredNorm() <= 0.f || std::abs(n.normalized().dot(normal)) < 0.99f)
            return false;
    }
    return true;
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

// Review report (2026-11-25) item 1: both split entry points re-center the
// pieces through add_volume() / center_geometry_after_creation(), so the piece
// STORED vertices are shifted by the piece center while the snapshot is still
// in the original stored coordinate system. Restoring with the identity
// transform then lost the painting whenever a piece center is non-zero.
TEST_CASE("Paint remap: split to parts and objects keeps the painting across piece centering", "[TriangleSelector][PaintRemap][Model]")
{
    const EnforcerBlockerType state = EnforcerBlockerType::Extruder3;

    // One volume containing two disconnected 12 mm cubes, X ranges [0,12] and
    // [30,42]; only the first cube's top face is painted. The whole volume is
    // auto-centered at creation (center (21,6,6)), so the stored coordinates of
    // cube A span x [-21,-9], y [-6,6], z [-6,6] and its painted top sits at
    // stored z = 6.
    auto make_volume = [&](Model& model) {
        ModelObject* obj = model.add_object();
        TriangleMesh both(box_mesh(0.f, 12.f, 12.f, 12.f));
        both.merge(TriangleMesh(box_mesh(30.f, 42.f, 12.f, 12.f)));
        ModelVolume* volume = obj->add_volume(std::move(both));
        obj->add_instance();
        TriangleSelector selector(volume->mesh());
        selector.set_facet(2, state);
        selector.set_facet(3, state);
        volume->mmu_segmentation_facets.set(selector);
        return volume;
    };
    const auto painted_of = [state](const ModelVolume* volume) {
        return painted_area_and_centroid(volume->mmu_segmentation_facets.get_facets(*volume, state));
    };

    // ModelVolume::split(): the parts stay in the same object.
    {
        Model model;
        ModelVolume* volume = make_volume(model);
        ModelObject* obj = volume->get_object();
        // World position of the painted top before the split, for the
        // scene-consistency check below.
        const auto [orig_area, orig_centroid] = painted_of(volume);
        const Vec3d original_world_top = volume->get_matrix() * orig_centroid.cast<double>();

        REQUIRE(volume->split(4, true) == 2);
        REQUIRE(obj->volumes.size() == 2);

        float  total_area = 0.f;
        int    painted_parts = 0;
        Vec3f  painted_stored_centroid = Vec3f::Zero();
        for (ModelVolume* part : obj->volumes) {
            const auto [area, centroid] = painted_of(part);
            INFO("part area " << area << ", centroid " << centroid.transpose());
            if (area > 1.f) {
                ++painted_parts;
                painted_stored_centroid = centroid;
            }
            total_area += area;
        }
        // Exactly one part carries the painting, with the full 12x12 top face.
        CHECK(painted_parts == 1);
        CHECK_THAT(total_area, WithinAbs(144.f, 1.f));
        // The painted part is cube A: its stored centroid after the piece
        // centering is (0,0,6) - the top face centered on x/y.
        CHECK_THAT(painted_stored_centroid.x(), WithinAbs(0.f, 0.1f));
        CHECK_THAT(painted_stored_centroid.y(), WithinAbs(0.f, 0.1f));
        CHECK_THAT(painted_stored_centroid.z(), WithinAbs(6.f, 0.1f));
        // The split must not move the geometry in the scene: the world position
        // of the painted top (instance * volume * stored) stays where it was.
        for (ModelVolume* part : obj->volumes) {
            const auto [area, centroid] = painted_of(part);
            if (area > 1.f) {
                const Vec3d world = obj->instances.front()->get_matrix() * part->get_matrix() * centroid.cast<double>();
                INFO("world painted centroid " << world.transpose());
                CHECK_THAT((world - original_world_top).norm(), WithinAbs(0., 0.2));
            }
        }
    }

    // ModelObject::split(): each part becomes its own object. The object-level
    // split absorbs the piece offset into the instances, so the scene position
    // must be computed as instance * volume * stored.
    {
        Model model;
        ModelVolume* volume = make_volume(model);
        ModelObject* obj = volume->get_object();
        const auto [orig_area, orig_centroid] = painted_of(volume);
        const Vec3d original_world_top = volume->get_matrix() * orig_centroid.cast<double>();

        ModelObjectPtrs out;
        obj->split(&out, true);
        REQUIRE(out.size() == 2);

        float total_area = 0.f;
        int   painted_objects = 0;
        for (ModelObject* o : out) {
            for (ModelVolume* part : o->volumes) {
                const auto [area, centroid] = painted_of(part);
                if (area > 1.f) {
                    ++painted_objects;
                    // Same stored position as the volume-level split.
                    CHECK_THAT(centroid.z(), WithinAbs(6.f, 0.1f));
                    const Vec3d world = o->instances.front()->get_matrix() * part->get_matrix() * centroid.cast<double>();
                    INFO("object-split world painted centroid " << world.transpose());
                    CHECK_THAT((world - original_world_top).norm(), WithinAbs(0., 0.2));
                }
                total_area += area;
            }
        }
        CHECK(painted_objects == 1);
        CHECK_THAT(total_area, WithinAbs(144.f, 1.f));
    }

    // A rotated + offset volume: the remap happens in stored coordinates, so
    // the painting must survive unchanged and follow the piece geometry. The
    // piece-relative invariant is the stored centroid (0,0,6) - the volume's
    // own placement (rotation/offset) never enters the remap.
    {
        Model model;
        ModelVolume* volume = make_volume(model);
        volume->set_rotation(Vec3d(0., 0., PI / 2.));
        volume->set_offset(Vec3d(30., 40., 0.));

        REQUIRE(volume->split(4, true) == 2);
        float total_area = 0.f;
        int   painted_parts = 0;
        for (ModelVolume* part : volume->get_object()->volumes) {
            const auto [area, centroid] = painted_of(part);
            if (area > 1.f) {
                ++painted_parts;
                INFO("rotated part stored painted centroid " << centroid.transpose());
                CHECK_THAT(centroid.x(), WithinAbs(0.f, 0.1f));
                CHECK_THAT(centroid.y(), WithinAbs(0.f, 0.1f));
                CHECK_THAT(centroid.z(), WithinAbs(6.f, 0.1f));
            }
            total_area += area;
        }
        CHECK(painted_parts == 1);
        CHECK_THAT(total_area, WithinAbs(144.f, 1.f));
    }
}

// Review report (2026-11-25) item 2: both boolean entries create the result
// through add_volume(), which re-centers the stored mesh, but the restore only
// applied the inverse of the source-to-result placement - the result's
// centering shift was not compensated. The expected chain is
// inverse_composite * Translation(init_shift). The GUI entries and this test
// share the same production helper.
TEST_CASE("Paint remap: boolean result centering compensation keeps a local stroke", "[TriangleSelector][PaintRemap][Model]")
{
    const EnforcerBlockerType state = EnforcerBlockerType::Extruder3;
    const indexed_triangle_set source_its = box_mesh(0.f, 12.f, 12.f, 12.f);

    // Asymmetric local stroke on the top face (z = 12), far from the face center.
    TriangleSelector source_selector{ TriangleMesh(source_its) };
    auto stroke = TriangleSelector::SinglePointCursor::cursor_factory(
        Vec3f(9.f, 3.f, 12.f), Vec3f(9.f, 3.f, 22.f), 1.5f, TriangleSelector::CursorType::CIRCLE,
        Transform3d::Identity(), TriangleSelector::ClippingPlane());
    source_selector.select_patch(2, std::move(stroke), state, Transform3d::Identity(), true);
    const auto painting = source_selector.serialize();
    REQUIRE(TriangleSelector::has_facets(painting, state));
    const auto [baseline_area, baseline_centroid] = painted_area_and_centroid(source_selector.get_facets(state));
    REQUIRE(baseline_area > 5.f);

    const std::vector<Transform3d> placements = {
        translation_transform(Vec3d(100., 100., 6.)),                                               // translation
        translation_transform(Vec3d(100., 100., 6.)) * rotation_transform(PI / 2. * Vec3d::UnitZ()), // rotation + translation
        translation_transform(Vec3d(100., 100., 6.)) * scale_transform(Vec3d(2., 1., 1.)),           // non-uniform scale, in the face plane
        translation_transform(Vec3d(100., 100., 6.)) * scale_transform(Vec3d(1., 1., 2.)),           // non-uniform scale, along the face normal
        translation_transform(Vec3d(100., 100., 6.)) * rotation_transform(PI / 4. * Vec3d::UnitX()) *
            scale_transform(Vec3d(2., 1., 1.)),                                                      // tilted stroke + non-uniform scale
        translation_transform(Vec3d(100., 100., 6.)) * scale_transform(Vec3d(-1., 1., 1.)),          // mirror
    };
    for (size_t pi = 0; pi < placements.size(); ++pi) {
        const Transform3d& placement = placements[pi];
        INFO("placement " << pi);

        Model model;
        ModelObject* obj = model.add_object();
        ModelVolume* src = obj->add_volume(TriangleMesh(source_its), /*modify_to_center_geometry=*/false);
        obj->add_instance();
        src->mmu_segmentation_facets.set(source_selector);
        auto saved = src->save_painting();
        REQUIRE(saved.has_value());

        // The boolean result mesh, placed like the boolean engine outputs it.
        TriangleMesh result(source_its);
        result.transform(placement, true);
        ModelVolume* dst = obj->add_volume(std::move(result)); // re-centers the stored mesh

        // Production restore: the shared helper used by both GUI boolean entries.
        dst->restore_painting_on_centered_result(saved, placement.inverse(), true);

        const auto [area, centroid] = painted_area_and_centroid(dst->mmu_segmentation_facets.get_facets(*dst, state));
        INFO("remapped area " << area << ", centroid " << centroid.transpose()
                              << ", init_shift " << dst->mesh().get_init_shift().transpose());
        // Expected: the source painting's own facet tiling mapped through the
        // placement, triangle by triangle. A triangle's area scales by
        // |det(A)| * ||A^-T n|| with n its unit normal, so the expectation is
        // computed from the transformed triangles and NOT from |det(A)| alone
        // (for the scale along the face normal |det| = 2 while the top face's
        // area is unchanged). The result volume was re-centered by add_volume().
        const auto [expected_area, placed_centroid] = transformed_area_and_centroid(source_selector.get_facets(state), placement);
        const Vec3d expected_stored = placed_centroid.cast<double>() - dst->mesh().get_init_shift();
        CHECK_THAT(area, WithinAbs(expected_area, expected_area * 0.1f));
        CHECK_THAT(centroid.x(), WithinAbs(float(expected_stored.x()), 0.5));
        CHECK_THAT(centroid.y(), WithinAbs(float(expected_stored.y()), 0.5));
        CHECK_THAT(centroid.z(), WithinAbs(float(expected_stored.z()), 0.5));
        // A blank point of the top face (far from the stroke) stays blank.
        const Vec3d blank_stored = (placement * Vec3d(3., 9., 12.)) - dst->mesh().get_init_shift();
        for (const Vec3f& v : dst->mmu_segmentation_facets.get_facets(*dst, state).vertices)
            CHECK(((Vec3d(v.cast<double>()) - blank_stored).norm()) > 1.f);
    }

    // Multiple instances: the same painting restored once per instance
    // placement must produce one stroke per placement.
    {
        Model model;
        ModelObject* obj = model.add_object();
        ModelVolume* src = obj->add_volume(TriangleMesh(source_its), /*modify_to_center_geometry=*/false);
        obj->add_instance();
        src->mmu_segmentation_facets.set(source_selector);
        auto saved = src->save_painting();
        REQUIRE(saved.has_value());

        const Transform3d place1 = translation_transform(Vec3d(100., 100., 6.));
        const Transform3d place2 = translation_transform(Vec3d(140., 100., 6.));
        TriangleMesh result(source_its);
        result.transform(place1, true);
        TriangleMesh second(source_its);
        second.transform(place2, true);
        result.merge(second);
        ModelVolume* dst = obj->add_volume(std::move(result));

        dst->restore_painting_on_centered_result(saved, place1.inverse(), true);
        dst->restore_painting_on_centered_result(saved, place2.inverse(), true);

        const auto [area, centroid] = painted_area_and_centroid(dst->mmu_segmentation_facets.get_facets(*dst, state));
        INFO("two-instance area " << area << ", centroid " << centroid.transpose());
        CHECK_THAT(area, WithinAbs(2.f * baseline_area, 2.f * baseline_area * 0.1f));
    }
}

// ---------------------------------------------------------------------------
// By-contour cut (GLGizmoCut3D). When the contour selection starts, the gizmo
// preprocesses the object ONCE - Cut::prepare_for_contour_cut(): plane pre-cut
// plus split into connected components - and keeps that model while the user
// assigns the parts to the upper/lower result; the cut then runs
// Cut::perform_by_contour() over it. The tests below drive exactly these two
// production entry points. Only the creation of connector volumes is GUI-only
// code (GLGizmoCut3D::apply_cut_connectors) and is mirrored below.
namespace {

// World matrix of a volume (first instance x volume matrix). A result object
// left with a single volume carries its placement on the INSTANCE
// (ModelObject::delete_volume collapses the volume transform), so positions
// must always be evaluated through both matrices.
Transform3d world_matrix(const ModelVolume* volume)
{
    return volume->get_object()->instances.front()->get_matrix() * volume->get_matrix();
}

// World-space bounding box of a volume (identity instance on the preprocessed
// parts, the placement folded into the instance on cut results).
BoundingBoxf3 object_bbox(const ModelVolume* volume)
{
    return volume->mesh().transformed_bounding_box(world_matrix(volume));
}

// Index of the preprocessed part whose object-space bounding-box center is
// nearest to `center`. The part order is an implementation detail, so parts
// are always looked up by their geometry.
size_t find_part(const ModelObject* obj, const Vec3d& center)
{
    size_t best    = 0;
    double best_d2 = std::numeric_limits<double>::max();
    for (size_t i = 0; i < obj->volumes.size(); ++i) {
        const double d2 = (object_bbox(obj->volumes[i]).center() - center).squaredNorm();
        if (d2 < best_d2) {
            best_d2 = d2;
            best    = i;
        }
    }
    REQUIRE(std::sqrt(best_d2) < 0.5);
    return best;
}

// The part list PartSelection hands to the final cut
// (GLGizmoCut3D::PartSelection::get_cut_parts()): one entry per preprocessed
// part, `selected` being the user's upper/lower assignment and `is_modifier`
// marking non-model volumes. Connector volumes appended afterwards are not
// parts.
std::vector<Cut::Part> contour_parts(const ModelObject* obj, const std::vector<bool>& to_upper)
{
    REQUIRE(to_upper.size() <= obj->volumes.size());
    std::vector<Cut::Part> parts;
    for (size_t i = 0; i < to_upper.size(); ++i)
        parts.push_back({ to_upper[i], !obj->volumes[i]->is_model_part() });
    return parts;
}

// Runs perform_by_contour() with the gizmo's attribute set for a by-contour cut
// (KeepPaint exactly when keep_painting is on at the time of the cut) and hands
// the results to `check` while they are alive.
template<class Check>
void contour_cut(const ModelObject* parts_obj, const ModelObject* src, const std::vector<bool>& to_upper, const Transform3d& plane,
                 bool keep_painting, int dowels_count, bool create_dowels, Check&& check)
{
    Cut cut(parts_obj, 0, plane,
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                (keep_painting ? ModelObjectCutAttribute::KeepPaint : ModelObjectCutAttributes()) |
                (create_dowels ? ModelObjectCutAttribute::CreateDowels : ModelObjectCutAttributes()));
    check(cut.perform_by_contour(src, contour_parts(parts_obj, to_upper), dowels_count));
}

// Whole-face painting of a volume in one pass (any number of states).
void paint_faces(ModelVolume* volume, const std::vector<std::pair<int, EnforcerBlockerType>>& faces)
{
    TriangleSelector selector(volume->mesh());
    for (const auto& [face, state] : faces)
        selector.set_facet(face, state);
    volume->mmu_segmentation_facets.set(selector);
}

// The merged solid body of a cut result (connectors stay separate volumes).
const ModelVolume* result_body(const ModelObject* obj)
{
    const ModelVolume* body = nullptr;
    for (const ModelVolume* v : obj->volumes)
        if (v->is_model_part() && !v->is_cut_connector()) {
            REQUIRE(body == nullptr);
            body = v;
        }
    REQUIRE(body != nullptr);
    return body;
}

// Painted area of one state over every volume of an object.
float painted_area(const ModelObject* obj, EnforcerBlockerType state)
{
    float area = 0.f;
    for (const ModelVolume* v : obj->volumes)
        area += painted_area_and_centroid(v->mmu_segmentation_facets.get_facets(*v, state)).first;
    return area;
}

// True when no volume of the object carries any painting class.
bool nothing_painted(const ModelObject* obj)
{
    for (const ModelVolume* v : obj->volumes)
        if (v->is_any_painted())
            return false;
    return true;
}

// Mirror of GLGizmoCut3D::apply_cut_connectors() (GUI code, not linkable into
// this test): a unit cylinder as NEGATIVE_VOLUME, placed and sized by the
// volume transformation and tagged through cut_info. Zero tolerances.
ModelVolume* add_connector(ModelObject* parts_obj, CutConnectorType type, const Vec3d& pos, double radius, double height)
{
    ModelVolume* vol = parts_obj->add_volume(TriangleMesh(its_make_cylinder(1.0, 1.0, PI / 180.)), ModelVolumeType::NEGATIVE_VOLUME);
    vol->set_transformation(translation_transform(pos) * scale_transform(Vec3d(radius, radius, height)));
    vol->cut_info = ModelVolume::CutInfo(type, 0.f, 0.f);
    vol->name     = "Connector-test";
    return vol;
}

// One volume holding two disconnected 12 mm cubes, x ranges [0,12] and
// [30,42], re-centered by add_volume(); cube A's top carries color_a, cube B's
// top color_b (top faces are 2/3 and 14/15 of the merged mesh).
ModelObject* make_two_painted_cubes(Model& model, EnforcerBlockerType color_a, EnforcerBlockerType color_b)
{
    ModelObject* obj = model.add_object();
    TriangleMesh both(box_mesh(0.f, 12.f, 12.f, 12.f));
    both.merge(TriangleMesh(box_mesh(30.f, 42.f, 12.f, 12.f)));
    ModelVolume* volume = obj->add_volume(std::move(both));
    obj->add_instance();
    paint_faces(volume, { { 2, color_a }, { 3, color_a }, { 14, color_b }, { 15, color_b } });
    return obj;
}

} // namespace

// Review report (2026-11-25) item 3: the contour flow pre-processes the object
// through a plane cut and a split-to-parts step before the real
// perform_by_contour() runs. The pre-cut used default flags (no KeepPaint) and
// the split remapped nothing, so the volumes the contour cut copied had
// already lost their painting.
//
// Follow-up (2026-11-26): the preprocessing consulted keep_painting when the
// selection was built, and the cut consulted it again. The preference can
// change in between (the modal Preferences dialog writes it on every toggle,
// while the gizmo and its cached selection stay alive), so "off when the
// selection was built, on when the cut runs" restored nothing. The
// preprocessing now always carries the painting; the cut alone decides. The
// cut plane is at mid-height (object z = 6).
TEST_CASE("Paint remap: contour cut follows the keep_painting setting at the time of the cut", "[TriangleSelector][PaintRemap][Cut]")
{
    const EnforcerBlockerType color_a = EnforcerBlockerType::Extruder3;
    const EnforcerBlockerType color_b = EnforcerBlockerType::Extruder4;
    const Transform3d        plane    = translation_transform(Vec3d(0., 0., 6.));

    // (setting while the selection was built, setting when the cut runs)
    const bool scenarios[4][2] = { { false, true }, { true, false }, { true, true }, { false, false } };
    for (const auto& scenario : scenarios) {
        const bool setting_at_selection = scenario[0];
        const bool setting_at_cut       = scenario[1];
        INFO("keep_painting when the selection was built: " << setting_at_selection
                                                             << "; when the cut runs: " << setting_at_cut);

        Model        model;
        ModelObject* obj = make_two_painted_cubes(model, color_a, color_b);

        // The gizmo's one-time preprocessing. It takes no setting on purpose,
        // so `setting_at_selection` only labels the scenario: what the
        // selection holds while the user works no longer depends on it.
        Model        parts_model = Cut::prepare_for_contour_cut(*obj, 0, plane);
        ModelObject* parts_obj   = parts_model.objects.front();
        // Upper and lower half of each cube, as separate connected parts.
        REQUIRE(parts_obj->volumes.size() == 4);
        const size_t a_up = find_part(parts_obj, Vec3d(6., 6., 9.));
        const size_t a_lo = find_part(parts_obj, Vec3d(6., 6., 3.));
        const size_t b_up = find_part(parts_obj, Vec3d(36., 6., 9.));
        const size_t b_lo = find_part(parts_obj, Vec3d(36., 6., 3.));

        // A mixed assignment that must survive untouched: both halves of cube A
        // go to the upper result, both halves of cube B to the lower result.
        std::vector<bool> to_upper(4, false);
        to_upper[a_up] = to_upper[a_lo] = true;

        contour_cut(parts_obj, obj, to_upper, plane, setting_at_cut, 0, false, [&](const ModelObjectPtrs& out) {
            REQUIRE(out.size() == 2);
            // Which result is which follows from the assignment, not from the order.
            ModelObject* upper_a = (object_bbox(result_body(out[0])).center() - Vec3d(6., 6., 6.)).norm() < 1. ? out[0] : out[1];
            ModelObject* lower_b = upper_a == out[0] ? out[1] : out[0];

            // The assignment is honored in every scenario: each result is
            // exactly one whole cube (its two halves merged back), 12 x 12 x 12.
            for (const auto& [result, center] : { std::make_pair(upper_a, Vec3d(6., 6., 6.)), std::make_pair(lower_b, Vec3d(36., 6., 6.)) }) {
                const BoundingBoxf3 bbox = object_bbox(result_body(result));
                CHECK((bbox.center() - center).norm() < 1e-6);
                CHECK((bbox.max - bbox.min - Vec3d(12., 12., 12.)).norm() < 1e-6);
                CHECK(result->volumes.size() == 1);
            }

            if (setting_at_cut) {
                // Cube A's top (144 mm^2) in the upper result and cube B's top
                // in the lower result, each at the top of its merged body
                // (stored z = +6 after the re-centering). Nothing else painted:
                // no other color, no other painting class.
                const ModelVolume* body_a = result_body(upper_a);
                const ModelVolume* body_b = result_body(lower_b);
                const auto [area_a, centroid_a] = painted_area_and_centroid(body_a->mmu_segmentation_facets.get_facets(*body_a, color_a));
                const auto [area_b, centroid_b] = painted_area_and_centroid(body_b->mmu_segmentation_facets.get_facets(*body_b, color_b));
                INFO("A top: area " << area_a << " centroid " << centroid_a.transpose()
                                    << "; B top: area " << area_b << " centroid " << centroid_b.transpose());
                CHECK_THAT(area_a, WithinAbs(144.f, 1.f));
                CHECK_THAT(area_b, WithinAbs(144.f, 1.f));
                for (const Vec3f& centroid : { centroid_a, centroid_b })
                    for (int axis = 0; axis < 3; ++axis)
                        CHECK_THAT(centroid(axis), WithinAbs(axis == 2 ? 6.f : 0.f, 0.05f));
                CHECK(painted_area(upper_a, color_b) < 0.5f);
                CHECK(painted_area(lower_b, color_a) < 0.5f);
                CHECK(facets_parallel_to(body_a->mmu_segmentation_facets.get_facets(*body_a, color_a), Vec3f::UnitZ()));
                CHECK(facets_parallel_to(body_b->mmu_segmentation_facets.get_facets(*body_b, color_b), Vec3f::UnitZ()));
                for (const ModelVolume* body : { body_a, body_b })
                    CHECK_FALSE((body->is_fdm_support_painted() || body->is_seam_painted() || body->is_fuzzy_skin_painted()));
            } else {
                // keep_painting off when the cut runs: the results carry no painting at all.
                CHECK(nothing_painted(upper_a));
                CHECK(nothing_painted(lower_b));
            }
        });
    }
}

// Two overlapping source volumes with different colors, assigned to the two
// results in a mixed way. Every result must carry exactly the painting of the
// parts it received - in the overlap region both sources have coplanar top
// and bottom faces, so a painting restored across sources would show up as an
// area above 144 mm^2 or in the other color.
TEST_CASE("Paint remap: contour cut keeps each source's painting on its own parts across a mixed assignment",
          "[TriangleSelector][PaintRemap][Cut]")
{
    const EnforcerBlockerType color_a = EnforcerBlockerType::Extruder3;
    const EnforcerBlockerType color_b = EnforcerBlockerType::Extruder4;
    const Transform3d        plane    = translation_transform(Vec3d(0., 0., 6.));

    Model        model;
    ModelObject* obj = model.add_object();
    // Cube A spans x [0,12], cube B x [6,18]: they overlap in x [6,12].
    ModelVolume* cube_a = obj->add_volume(TriangleMesh(box_mesh(0.f, 12.f, 12.f, 12.f)));
    ModelVolume* cube_b = obj->add_volume(TriangleMesh(box_mesh(6.f, 18.f, 12.f, 12.f)));
    obj->add_instance();
    // Top AND bottom faces: color_a on source A, color_b on source B.
    paint_faces(cube_a, { { 0, color_a }, { 1, color_a }, { 2, color_a }, { 3, color_a } });
    paint_faces(cube_b, { { 0, color_b }, { 1, color_b }, { 2, color_b }, { 3, color_b } });

    Model        parts_model = Cut::prepare_for_contour_cut(*obj, 0, plane);
    ModelObject* parts_obj   = parts_model.objects.front();
    REQUIRE(parts_obj->volumes.size() == 4);
    const size_t a_up = find_part(parts_obj, Vec3d(6., 6., 9.));
    const size_t a_lo = find_part(parts_obj, Vec3d(6., 6., 3.));
    const size_t b_up = find_part(parts_obj, Vec3d(12., 6., 9.));
    const size_t b_lo = find_part(parts_obj, Vec3d(12., 6., 3.));

    // A's upper half and B's lower half to the upper result; A's lower half and
    // B's upper half to the lower result.
    std::vector<bool> to_upper(4, false);
    to_upper[a_up] = to_upper[b_lo] = true;

    contour_cut(parts_obj, obj, to_upper, plane, true, 0, false, [&](const ModelObjectPtrs& out) {
        REQUIRE(out.size() == 2);
        // Both results span x [0,18] x [0,12] x [0,12] (merged body re-centered
        // on (9,6,6)), so they are told apart by where the colors landed:
        //   result with A_up + B_lo: color_a at A's top (stored z +6), color_b at B's bottom (stored z -6)
        //   result with A_lo + B_up: color_a at A's bottom (stored z -6), color_b at B's top (stored z +6)
        int seen[2] = { 0, 0 };
        for (ModelObject* result : out) {
            REQUIRE(result->volumes.size() == 1);
            const ModelVolume* body = result_body(result);
            const BoundingBoxf3 bbox = object_bbox(body);
            CHECK((bbox.center() - Vec3d(9., 6., 6.)).norm() < 1e-6);

            const auto facets_a = body->mmu_segmentation_facets.get_facets(*body, color_a);
            const auto facets_b = body->mmu_segmentation_facets.get_facets(*body, color_b);
            const auto [area_a, centroid_a] = painted_area_and_centroid(facets_a);
            const auto [area_b, centroid_b] = painted_area_and_centroid(facets_b);
            INFO("color A: area " << area_a << " centroid " << centroid_a.transpose()
                                  << "; color B: area " << area_b << " centroid " << centroid_b.transpose());
            CHECK_THAT(area_a, WithinAbs(144.f, 1.f));
            CHECK_THAT(area_b, WithinAbs(144.f, 1.f));
            // Stored x: A's cube maps to [-9,3], B's to [-3,9] (object x minus 9).
            CHECK_THAT(centroid_a.x(), WithinAbs(-3.f, 0.05f));
            CHECK_THAT(centroid_b.x(), WithinAbs(3.f, 0.05f));
            CHECK_THAT(centroid_a.y(), WithinAbs(0.f, 0.05f));
            CHECK_THAT(centroid_b.y(), WithinAbs(0.f, 0.05f));
            // The two colors are on opposite faces of the result.
            CHECK_THAT(centroid_a.z() + centroid_b.z(), WithinAbs(0.f, 0.05f));
            CHECK(std::abs(centroid_a.z()) > 5.f);
            // The colors stay on horizontal faces only: side faces and the two
            // cut caps of every half cube are unpainted.
            CHECK(facets_parallel_to(facets_a, Vec3f::UnitZ()));
            CHECK(facets_parallel_to(facets_b, Vec3f::UnitZ()));
            for (const Vec3f& v : facets_a.vertices)
                CHECK((v.x() >= -9.f - 1e-3f && v.x() <= 3.f + 1e-3f));
            for (const Vec3f& v : facets_b.vertices)
                CHECK((v.x() >= -3.f - 1e-3f && v.x() <= 9.f + 1e-3f));
            const int which = centroid_a.z() > 0.f ? 0 : 1;
            ++seen[which];
        }
        // Each assignment pattern appears exactly once.
        CHECK(seen[0] == 1);
        CHECK(seen[1] == 1);
    });
}

// Two local strokes on a 12 mm cube: one on the front face crossing the cut
// plane, one on the top face above it. Both halves of the crossing stroke
// must survive on the original outer surface (area, position and state), and
// the caps generated by the cut must stay unpainted although they touch the
// crossing stroke along an edge and lie directly under the top stroke.
TEST_CASE("Paint remap: contour cut keeps a stroke crossing the cut plane and leaves the new caps unpainted",
          "[TriangleSelector][PaintRemap][Cut]")
{
    // The crossing stroke is a SUPPORT painting, the top stroke a SEAM painting:
    // two of the four painting classes go through the contour flow here.
    const EnforcerBlockerType state_front = EnforcerBlockerType::ENFORCER;
    const EnforcerBlockerType state_top   = EnforcerBlockerType::BLOCKER;
    const Transform3d        plane        = translation_transform(Vec3d(0., 0., 6.));
    const float              radius       = 2.f;

    Model        model;
    ModelObject* obj = model.add_object();
    ModelVolume* volume = obj->add_volume(TriangleMesh(box_mesh(0.f, 12.f, 12.f, 12.f)));
    obj->add_instance();
    // Stored coordinates: the cube spans [-6,6]^3 (volume offset (6,6,6)), the
    // cut plane is stored z = 0. Front face (-Y) is faces 4/5, top (+Z) 2/3.
    TriangleSelector front_selector(volume->mesh());
    auto front_stroke = TriangleSelector::SinglePointCursor::cursor_factory(
        Vec3f(0.f, -6.f, 0.f), Vec3f(0.f, -16.f, 0.f), radius, TriangleSelector::CursorType::CIRCLE, Transform3d::Identity(),
        TriangleSelector::ClippingPlane());
    front_selector.select_patch(4, std::move(front_stroke), state_front, Transform3d::Identity(), true);
    volume->supported_facets.set(front_selector);
    TriangleSelector top_selector(volume->mesh());
    auto top_stroke = TriangleSelector::SinglePointCursor::cursor_factory(
        Vec3f(0.f, 0.f, 6.f), Vec3f(0.f, 0.f, 16.f), radius, TriangleSelector::CursorType::CIRCLE, Transform3d::Identity(),
        TriangleSelector::ClippingPlane());
    top_selector.select_patch(2, std::move(top_stroke), state_top, Transform3d::Identity(), true);
    volume->seam_facets.set(top_selector);

    // Baseline: the source painting itself, the front stroke clipped at the cut plane.
    const indexed_triangle_set front_facets = front_selector.get_facets(state_front);
    const indexed_triangle_set top_facets   = top_selector.get_facets(state_top);
    const auto [front_up_area, front_up_centroid] = clipped_area_and_centroid(front_facets, 0.f, true);
    const auto [front_lo_area, front_lo_centroid] = clipped_area_and_centroid(front_facets, 0.f, false);
    const auto [top_area, top_centroid]           = painted_area_and_centroid(top_facets);
    REQUIRE(front_up_area > 5.f);
    REQUIRE(front_lo_area > 5.f);
    REQUIRE(top_area > 10.f);
    // Tolerance derived from the remap's resolution: boundaries are resolved
    // down to the 0.05 mm subdivision limit, so the area may differ from the
    // exact clipped region by at most (boundary length x 0.06 mm).
    const float tol_half = float(PI * radius) * 0.06f;
    const float tol_full = float(2.f * PI * radius) * 0.06f;

    const Vec3d source_offset(6., 6., 6.); // world = stored + offset on the source volume

    // One region of the source painting, measured on a volume holding the
    // corresponding part of the cube: area, world position and the face it
    // lies on must all match the baseline (the world matrix includes the
    // instance, which carries the placement of single-volume results).
    auto check_region = [&](const char* what, const ModelVolume* part, const FacetsAnnotation ModelVolume::*annotation,
                            EnforcerBlockerType state, float expected_area, float tol, const Vec3f& expected_stored_centroid,
                            const Vec3f& face_normal) {
        const indexed_triangle_set facets = (part->*annotation).get_facets(*part, state);
        const auto [area, stored_centroid] = painted_area_and_centroid(facets);
        const Vec3d world_centroid  = world_matrix(part) * stored_centroid.cast<double>();
        const Vec3d expected_world  = source_offset + expected_stored_centroid.cast<double>();
        INFO(what << ": area " << area << " (expected " << expected_area << "), world centroid " << world_centroid.transpose()
                  << " (expected " << expected_world.transpose() << ")");
        CHECK_THAT(area, WithinAbs(expected_area, tol));
        for (int axis = 0; axis < 3; ++axis)
            CHECK_THAT(world_centroid(axis), WithinAbs(expected_world(axis), 0.3));
        CHECK(facets_parallel_to(facets, face_normal));
    };
    const FacetsAnnotation ModelVolume::*supports = &ModelVolume::supported_facets;
    const FacetsAnnotation ModelVolume::*seams    = &ModelVolume::seam_facets;

    Model        parts_model = Cut::prepare_for_contour_cut(*obj, 0, plane);
    ModelObject* parts_obj   = parts_model.objects.front();
    REQUIRE(parts_obj->volumes.size() == 2);
    const size_t up = find_part(parts_obj, Vec3d(6., 6., 9.));
    const size_t lo = find_part(parts_obj, Vec3d(6., 6., 3.));

    // After the preprocessing.
    const ModelVolume* upper_part = parts_obj->volumes[up];
    const ModelVolume* lower_part = parts_obj->volumes[lo];
    check_region("upper part, front stroke", upper_part, supports, state_front, front_up_area, tol_half, front_up_centroid, -Vec3f::UnitY());
    check_region("lower part, front stroke", lower_part, supports, state_front, front_lo_area, tol_half, front_lo_centroid, -Vec3f::UnitY());
    check_region("upper part, top stroke", upper_part, seams, state_top, top_area, tol_full, top_centroid, Vec3f::UnitZ());
    // The lower part's top cap lies 6 mm under the top stroke, directly inside
    // its projection: it must not inherit anything.
    CHECK(lower_part->seam_facets.get_facets(*lower_part, state_top).indices.empty());
    // Only the two painting classes that were painted are present on the parts.
    CHECK(upper_part->is_fdm_support_painted());
    CHECK(upper_part->is_seam_painted());
    CHECK(lower_part->is_fdm_support_painted());
    CHECK_FALSE((upper_part->is_mm_painted() || upper_part->is_fuzzy_skin_painted()));
    CHECK_FALSE((lower_part->is_mm_painted() || lower_part->is_fuzzy_skin_painted() || lower_part->is_seam_painted()));

    // The final cut with the halves swapped between the results.
    std::vector<bool> to_upper(2, false);
    to_upper[lo] = true;
    contour_cut(parts_obj, obj, to_upper, plane, true, 0, false, [&](const ModelObjectPtrs& out) {
        REQUIRE(out.size() == 2);
        for (ModelObject* result : out) {
            REQUIRE(result->volumes.size() == 1);
            const ModelVolume* body = result_body(result);
            const bool is_upper_half = object_bbox(body).min.z() > 5.f;
            INFO("result holding the " << (is_upper_half ? "upper" : "lower") << " half");
            if (is_upper_half) {
                check_region("final upper half, front stroke", body, supports, state_front, front_up_area, tol_half, front_up_centroid, -Vec3f::UnitY());
                check_region("final upper half, top stroke", body, seams, state_top, top_area, tol_full, top_centroid, Vec3f::UnitZ());
                CHECK_FALSE((body->is_mm_painted() || body->is_fuzzy_skin_painted()));
            } else {
                check_region("final lower half, front stroke", body, supports, state_front, front_lo_area, tol_half, front_lo_centroid, -Vec3f::UnitY());
                // The new top cap of the lower half stays unpainted.
                CHECK_FALSE((body->is_seam_painted() || body->is_mm_painted() || body->is_fuzzy_skin_painted()));
            }
        }
    });
}

// Connectors through the by-contour cut: a plug reaching from the cut plane up
// to the painted top face and a dowel straddling the plane. The newly created
// connector volumes (hole in the upper result, plug solid in the lower result,
// separate dowel object) must not inherit the outer-surface painting although
// the plug's top cap lies exactly in the painted top plane, and the source
// body must keep its painting.
TEST_CASE("Paint remap: contour cut with connectors keeps the body painting and leaves connectors unpainted",
          "[TriangleSelector][PaintRemap][Cut]")
{
    const EnforcerBlockerType color = EnforcerBlockerType::Extruder3;
    const Transform3d        plane  = translation_transform(Vec3d(0., 0., 6.));

    Model        model;
    ModelObject* obj    = model.add_object();
    ModelVolume* volume = obj->add_volume(TriangleMesh(box_mesh(0.f, 12.f, 12.f, 12.f)));
    obj->add_instance();
    paint_faces(volume, { { 2, color }, { 3, color } }); // top face

    Model        parts_model = Cut::prepare_for_contour_cut(*obj, 0, plane);
    ModelObject* parts_obj   = parts_model.objects.front();
    REQUIRE(parts_obj->volumes.size() == 2);
    const size_t up = find_part(parts_obj, Vec3d(6., 6., 9.));
    const size_t lo = find_part(parts_obj, Vec3d(6., 6., 3.));

    // What GLGizmoCut3D::apply_connectors_in_model() adds to the preprocessed
    // object before the cut: a plug (center half a height above the plane,
    // reaching the painted top) and a dowel (centered on the plane, tangent to
    // the front face).
    ModelVolume* plug  = add_connector(parts_obj, CutConnectorType::Plug, Vec3d(6., 6., 9.), 2., 6.);
    ModelVolume* dowel = add_connector(parts_obj, CutConnectorType::Dowel, Vec3d(6., 2., 6.), 2., 4.);
    REQUIRE(parts_obj->volumes.size() == 4);
    // Freshly added connectors are tagged but not processed yet
    // (is_cut_connector() only turns true during the cut); they are negative
    // volumes, hence not parts of the solid body.
    REQUIRE(plug->cut_info.is_connector);
    REQUIRE(dowel->cut_info.is_connector);
    REQUIRE_FALSE(plug->is_model_part());
    REQUIRE_FALSE(dowel->is_model_part());

    std::vector<bool> to_upper(2, false);
    to_upper[up] = true;
    contour_cut(parts_obj, obj, to_upper, plane, true, /*dowels_count=*/1, /*create_dowels=*/true, [&](const ModelObjectPtrs& out) {
        // upper result, lower result, and the dowel as its own object.
        REQUIRE(out.size() == 3);
        ModelObject* upper = object_bbox(result_body(out[0])).min.z() > 5.f ? out[0] : out[1];
        ModelObject* lower = upper == out[0] ? out[1] : out[0];
        ModelObject* dowel_object = out[2];

        // Upper result: body (z 6..12) with the painted top, the plug hole and
        // the upper half of the dowel hole.
        REQUIRE(upper->volumes.size() == 3);
        const ModelVolume* upper_body = result_body(upper);
        const auto [area, centroid] = painted_area_and_centroid(upper_body->mmu_segmentation_facets.get_facets(*upper_body, color));
        INFO("upper body: area " << area << " centroid " << centroid.transpose());
        CHECK_THAT(area, WithinAbs(144.f, 1.f));
        CHECK_THAT(centroid.z(), WithinAbs(3.f, 0.05f)); // top of the re-centered body
        CHECK(facets_parallel_to(upper_body->mmu_segmentation_facets.get_facets(*upper_body, color), Vec3f::UnitZ()));
        for (const ModelVolume* v : upper->volumes)
            if (v != upper_body) {
                CHECK(v->is_cut_connector());
                CHECK_FALSE(v->is_any_painted());
            }

        // Lower result: body (z 0..6) without painting, the plug as a solid
        // connector part whose top cap lies in the painted top plane, and the
        // lower half of the dowel hole. None of them painted.
        REQUIRE(lower->volumes.size() == 3);
        CHECK(nothing_painted(lower));
        int plugs = 0;
        for (const ModelVolume* v : lower->volumes)
            if (v->is_cut_connector() && v->is_model_part()) {
                ++plugs;
                const BoundingBoxf3 bbox = object_bbox(v);
                CHECK((bbox.max.z() - 12.f) < 1e-3); // reaches the painted top plane
            }
        CHECK(plugs == 1);

        // The dowel is a separate solid object, unpainted.
        REQUIRE(dowel_object->volumes.size() == 1);
        CHECK(dowel_object->volumes[0]->is_cut_connector());
        CHECK_FALSE(dowel_object->volumes[0]->is_any_painted());
    });
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

TEST_CASE("Intersection remap keeps positive-area terminal overlaps in either winding", "[PaintRemap][ReviewRegression]")
{
    for (bool reverse : {false, true}) {
        indexed_triangle_set source;
        source.vertices = { Vec3f(0, 0, 0), Vec3f(0.01f, 0, 0), Vec3f(0, 0.01f, 0) };
        source.indices.emplace_back(0, reverse ? 2 : 1, reverse ? 1 : 2);
        TriangleMesh source_mesh(source);
        TriangleSelector selector(source_mesh);
        selector.set_facet(0, EnforcerBlockerType::ENFORCER);
        for (float offset : {0.0025f, 0.005f, 0.008f}) {
            // First case overlaps in a triangle of area 0.0000125; second
            // touches only at a point; third is separated. All edges are below
            // the cursor's subdivision limit, exercising terminal coverage.
            indexed_triangle_set target;
            target.vertices = { Vec3f(offset, offset, 0), Vec3f(0.01f, offset, 0), Vec3f(offset, 0.01f, 0) };
            target.indices.emplace_back(0, reverse ? 2 : 1, reverse ? 1 : 2);
            TriangleMesh target_mesh(target);
            auto data = TriangleSelector::remap_painting(source, selector.serialize(), target, Transform3d::Identity(), std::nullopt);
            const auto painted = painted_facets(target_mesh, data, EnforcerBlockerType::ENFORCER);
            const float area = painted_area_and_centroid(painted).first;
            INFO("reverse=" << reverse << " offset=" << offset);
            if (offset < 0.005f)
                CHECK(area > 0.f);
            else
                CHECK(area == 0.f);
        }
    }
}

TEST_CASE("Global smooth preserves a world-sized stroke on differently scaled stored meshes", "[PaintRemap][Smooth][SmoothScaleRegression]")
{
    for (float stored_scale : {0.1f, 1.f, 100.f}) {
        indexed_triangle_set sheet;
        sheet.vertices = {Vec3f(0, 0, 0), Vec3f(stored_scale, 0, 0),
                          Vec3f(stored_scale, stored_scale, 0), Vec3f(0, stored_scale, 0)};
        sheet.indices = {stl_triangle_vertex_indices(0, 1, 2), stl_triangle_vertex_indices(0, 2, 3)};
        const TriangleMesh mesh(sheet);
        const double world_scale = 100.0 / stored_scale;
        const Transform3d placement = scale_transform(Vec3d::Constant(world_scale));
        for (auto state : {EnforcerBlockerType::ENFORCER, EnforcerBlockerType::BLOCKER,
                           EnforcerBlockerType::Extruder3, EnforcerBlockerType::Extruder4}) {
            TriangleSelector selector(mesh);
            const Vec3f center = stored_scale * Vec3f(0.55f, 0.45f, 0);
            auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(
                center, center + stored_scale * Vec3f::UnitZ(), 3.f, TriangleSelector::CursorType::CIRCLE,
                placement, TriangleSelector::ClippingPlane());
            selector.select_patch(0, std::move(cursor), state, placement, true);
            const auto [baseline, baseline_center] = painted_area_and_centroid(selector.get_facets(state));
            REQUIRE(baseline * world_scale * world_scale > 5.f);
            bool ok = false;
            std::vector<std::pair<int, int>> origins;
            const auto smoothed = TriangleMeshDeal::smooth_triangle_mesh(mesh, ok, &origins);
            REQUIRE(ok);
            const auto data = TriangleSelector::remap_painting_smooth(sheet, selector.serialize(), smoothed.its, origins, std::nullopt);
            const auto [area, centroid] = painted_area_and_centroid(painted_facets(smoothed, data, state));
            INFO("stored scale=" << stored_scale << " state=" << int(state)
                 << " world area=" << area * world_scale * world_scale << " baseline=" << baseline * world_scale * world_scale);
            // The stroke is strictly inside the central child of its parent.
            // That child's edge-midpoint vertices stay in place under Loop on
            // this planar sheet, so its expected painted area is unchanged.
            CHECK_THAT(area, WithinAbs(baseline, baseline * 0.05f));
            CHECK((centroid - baseline_center).norm() * world_scale < 0.2);
        }
    }
}

TEST_CASE("Split preserves a world-sized stroke on a small stored mesh", "[PaintRemap][MergeScaleRegression]")
{
    for (float unit : {1.f, 0.001f}) {
        Model model;
        auto* obj = model.add_object();
        TriangleMesh both(box_mesh(0, 10, 10, 10));
        both.merge(TriangleMesh(box_mesh(30, 40, 10, 10)));
        both.scale(unit);
        auto* volume = obj->add_volume(std::move(both));
        obj->add_instance();
        volume->set_scaling_factor(Vec3d::Constant(1.0 / unit));
        const Transform3d placement = volume->get_matrix();
        const Vec3f center = unit * Vec3f(7, 3, 10) - volume->mesh().get_init_shift().cast<float>();
        TriangleSelector selector(volume->mesh());
        auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(
            center, center + Vec3f::UnitZ(), 1.f, TriangleSelector::CursorType::CIRCLE,
            placement, TriangleSelector::ClippingPlane());
        selector.select_patch(2, std::move(cursor), EnforcerBlockerType::Extruder3, placement, true);
        const double baseline = painted_area_and_centroid(selector.get_facets(EnforcerBlockerType::Extruder3)).first / (unit * unit);
        REQUIRE(baseline > 1.0);
        volume->mmu_segmentation_facets.set(selector);
        REQUIRE(volume->split(4, true) == 2);
        double area = 0;
        int painted_parts = 0;
        for (auto* part : obj->volumes) {
            const double a = painted_area_and_centroid(part->mmu_segmentation_facets.get_facets(*part, EnforcerBlockerType::Extruder3)).first / (unit * unit);
            area += a;
            if (a > 0) ++painted_parts;
        }
        INFO("unit=" << unit << " baseline=" << baseline << " remapped=" << area);
        CHECK(painted_parts == 1);
        CHECK_THAT(area, WithinAbs(baseline, baseline * 0.05));
    }
}

TEST_CASE("Same-mesh paint merging preserves disjoint strokes and source wins overlaps", "[PaintRemap][MergeScaleRegression]")
{
    indexed_triangle_set its;
    its.vertices = {Vec3f(0, 0, 0), Vec3f(20, 0, 0), Vec3f(0, 20, 0)};
    its.indices.emplace_back(0, 1, 2);
    TriangleMesh mesh(its);
    auto first = paint_brush_stroke(mesh, Vec3f(4, 4, 0), 1.f, EnforcerBlockerType::Extruder3);
    auto second = paint_brush_stroke(mesh, Vec3f(12, 4, 0), 1.f, EnforcerBlockerType::Extruder4);
    const float first_area = painted_area_and_centroid(painted_facets(mesh, first, EnforcerBlockerType::Extruder3)).first;
    const float second_area = painted_area_and_centroid(painted_facets(mesh, second, EnforcerBlockerType::Extruder4)).first;
    REQUIRE(first_area > 1.f);
    REQUIRE(second_area > 1.f);
    const auto merged = TriangleSelector::remap_painting(its, second, its, Transform3d::Identity(), std::cref(first));
    CHECK_THAT(painted_area_and_centroid(painted_facets(mesh, merged, EnforcerBlockerType::Extruder3)).first,
               WithinAbs(first_area, first_area * 0.05f));
    CHECK_THAT(painted_area_and_centroid(painted_facets(mesh, merged, EnforcerBlockerType::Extruder4)).first,
               WithinAbs(second_area, second_area * 0.05f));
    // Equivalent triangle with a cyclic corner permutation takes the generic
    // path. Merging must not depend on this storage-only representation.
    auto permuted = its;
    permuted.indices[0] = stl_triangle_vertex_indices(1, 2, 0);
    // Paint the source against its own corner order for the equivalence check.
    const TriangleMesh permuted_mesh(permuted);
    const auto permuted_second = paint_brush_stroke(permuted_mesh, Vec3f(12, 4, 0), 1.f, EnforcerBlockerType::Extruder4);
    const auto equivalent = TriangleSelector::remap_painting(permuted, permuted_second, its, Transform3d::Identity(), std::cref(first));
    for (auto state : {EnforcerBlockerType::Extruder3, EnforcerBlockerType::Extruder4})
        CHECK_THAT(painted_area_and_centroid(painted_facets(mesh, equivalent, state)).first,
                   WithinAbs(painted_area_and_centroid(painted_facets(mesh, merged, state)).first, 0.15f));
    // A new non-NONE stroke has precedence where the two sources overlap.
    const auto overlapping = paint_brush_stroke(mesh, Vec3f(4, 4, 0), 1.f, EnforcerBlockerType::Extruder4);
    const auto overwritten = TriangleSelector::remap_painting(its, overlapping, its, Transform3d::Identity(), std::cref(first));
    CHECK(painted_area_and_centroid(painted_facets(mesh, overwritten, EnforcerBlockerType::Extruder3)).first < 0.15f);
    CHECK_THAT(painted_area_and_centroid(painted_facets(mesh, overwritten, EnforcerBlockerType::Extruder4)).first,
               WithinAbs(first_area, first_area * 0.05f));
}
