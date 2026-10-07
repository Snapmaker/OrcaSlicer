// Paint remapping onto a replaced mesh (core of OrcaSlicer #13472 "Keep painting after cut",
// merge 1c0d0b89cc). Texture displacement subdivides/remeshes a part before baking and relies on
// ModelVolume::save_painting() / restore_painting() to keep support, seam, colour and fuzzy-skin
// paint on the new triangles.
#include <catch2/catch.hpp>

#include <map>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

namespace {

// Midpoint subdivision: every triangle becomes four, shared edges share their midpoint.
indexed_triangle_set subdivide_once(const indexed_triangle_set &in)
{
    indexed_triangle_set out;
    out.vertices = in.vertices;
    std::map<std::pair<int, int>, int> mid;
    auto midpoint = [&](int a, int b) {
        const std::pair<int, int> key{std::min(a, b), std::max(a, b)};
        auto it = mid.find(key);
        if (it != mid.end())
            return it->second;
        const int idx = int(out.vertices.size());
        out.vertices.emplace_back(0.5f * (in.vertices[a] + in.vertices[b]));
        mid.emplace(key, idx);
        return idx;
    };
    for (const Vec3i32 &f : in.indices) {
        const int a = f[0], b = f[1], c = f[2];
        const int ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
        out.indices.emplace_back(a, ab, ca);
        out.indices.emplace_back(ab, b, bc);
        out.indices.emplace_back(ca, bc, c);
        out.indices.emplace_back(ab, bc, ca);
    }
    return out;
}

double area_of(const indexed_triangle_set &its)
{
    double a = 0.;
    for (const Vec3i32 &f : its.indices)
        a += 0.5 * double((its.vertices[f[1]] - its.vertices[f[0]]).cross(its.vertices[f[2]] - its.vertices[f[0]]).norm());
    return a;
}

bool all_on_top(const indexed_triangle_set &its, float top_z)
{
    for (const Vec3i32 &f : its.indices)
        for (int i = 0; i < 3; ++i)
            if (std::abs(its.vertices[f[i]].z() - top_z) > 1e-3f)
                return false;
    return true;
}

// Paints every facet whose normal points up with `state`.
TriangleSelector::TriangleSplittingData paint_top(const TriangleMesh &mesh, EnforcerBlockerType state)
{
    TriangleSelector sel(mesh);
    for (int i = 0; i < int(mesh.its.indices.size()); ++i)
        if (its_face_normal(mesh.its, i).z() > 0.9f)
            sel.set_facet(i, state);
    return sel.serialize();
}

} // namespace

TEST_CASE("remap_painting keeps a painted face on a subdivided copy of the mesh", "[PaintRemap]")
{
    const TriangleMesh src = make_cube(20., 20., 20.);
    const auto         data = paint_top(src, EnforcerBlockerType::ENFORCER);
    REQUIRE_FALSE(data.bitstream.empty());

    const indexed_triangle_set dst_its = subdivide_once(subdivide_once(src.its));
    REQUIRE(dst_its.indices.size() == src.its.indices.size() * 16);

    const auto remapped = TriangleSelector::remap_painting(src.its, data, dst_its, Transform3d::Identity(), {});
    REQUIRE_FALSE(remapped.bitstream.empty());

    const TriangleMesh dst(dst_its);
    TriangleSelector   dsel(dst);
    dsel.deserialize(remapped, false);
    const indexed_triangle_set painted = dsel.get_facets(EnforcerBlockerType::ENFORCER);
    CHECK(all_on_top(painted, 20.f));
    CHECK(area_of(painted) == Approx(400.).epsilon(0.01));
    CHECK(dsel.get_facets(EnforcerBlockerType::BLOCKER).indices.empty());
}

TEST_CASE("remap_painting with existing paint keeps both", "[PaintRemap]")
{
    const TriangleMesh src = make_cube(20., 20., 20.);
    const auto         top = paint_top(src, EnforcerBlockerType::ENFORCER);

    // Existing paint on the target: the bottom face blocked.
    const TriangleMesh dst(subdivide_once(src.its));
    TriangleSelector   existing(dst);
    for (int i = 0; i < int(dst.its.indices.size()); ++i)
        if (its_face_normal(dst.its, i).z() < -0.9f)
            existing.set_facet(i, EnforcerBlockerType::BLOCKER);
    const auto existing_data = existing.serialize();

    const auto merged = TriangleSelector::remap_painting(src.its, top, dst.its, Transform3d::Identity(), std::cref(existing_data));
    TriangleSelector dsel(dst);
    dsel.deserialize(merged, false);
    CHECK(area_of(dsel.get_facets(EnforcerBlockerType::ENFORCER)) == Approx(400.).epsilon(0.01));
    CHECK(area_of(dsel.get_facets(EnforcerBlockerType::BLOCKER)) == Approx(400.).epsilon(0.01));
}

TEST_CASE("ModelVolume save_painting / restore_painting across set_mesh", "[PaintRemap]")
{
    Model        model;
    ModelObject *obj = model.add_object();
    ModelVolume *vol = obj->add_volume(make_cube(20., 20., 20.));

    SECTION("unpainted volume saves nothing") {
        CHECK_FALSE(vol->save_painting().has_value());
    }

    SECTION("support and colour paint survive a subdivision") {
        vol->supported_facets.set_data(paint_top(vol->mesh(), EnforcerBlockerType::ENFORCER));
        vol->mmu_segmentation_facets.set_data(paint_top(vol->mesh(), EnforcerBlockerType::Extruder2));
        REQUIRE(vol->is_any_painted());

        const auto saved = vol->save_painting();
        REQUIRE(saved.has_value());

        vol->set_mesh(TriangleMesh(subdivide_once(vol->mesh().its)));
        vol->restore_painting(saved);

        const indexed_triangle_set sup = vol->supported_facets.get_facets(*vol, EnforcerBlockerType::ENFORCER);
        CHECK(area_of(sup) == Approx(400.).epsilon(0.01));
        const indexed_triangle_set mmu = vol->mmu_segmentation_facets.get_facets(*vol, EnforcerBlockerType::Extruder2);
        CHECK(area_of(mmu) == Approx(400.).epsilon(0.01));
        CHECK_FALSE(vol->is_seam_painted());
        CHECK_FALSE(vol->is_fuzzy_skin_painted());
    }

    SECTION("restore without saved paint clears stale paint") {
        vol->seam_facets.set_data(paint_top(vol->mesh(), EnforcerBlockerType::ENFORCER));
        REQUIRE(vol->is_seam_painted());
        vol->restore_painting(std::nullopt);
        CHECK_FALSE(vol->is_any_painted());
    }
}
