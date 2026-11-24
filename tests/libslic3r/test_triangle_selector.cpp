#include <tbb/task_arena.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

TEST_CASE("Triangle selector round-trips painted states above sixteen", "[TriangleSelector][MMUPaint]")
{
    indexed_triangle_set its;
    its.vertices = {
        Vec3f(0.f, 0.f, 0.f),
        Vec3f(1.f, 0.f, 0.f),
        Vec3f(0.f, 1.f, 0.f),
    };
    its.indices = {
        stl_triangle_vertex_indices(0, 1, 2),
    };

    TriangleMesh     mesh(its);
    TriangleSelector selector(mesh);

    const int painted_state = GENERATE(120, 255, 256, 272, int(MAXIMUM_FILAMENT_NUMBER));
    selector.set_facet(0, static_cast<EnforcerBlockerType>(painted_state));

    auto data = selector.serialize();
    REQUIRE_FALSE(data.triangles_to_split.empty());
    REQUIRE(data.used_states.size() > painted_state);
    CHECK(data.used_states[painted_state]);

    data.reset_used_states();
    data.update_used_states(size_t(data.triangles_to_split.front().bitstream_start_idx));
    CHECK(data.used_states[painted_state]);
    CHECK(TriangleSelector::has_facets(data, static_cast<EnforcerBlockerType>(painted_state)));

    TriangleSelector restored(mesh);
    restored.deserialize(data, true, static_cast<EnforcerBlockerType>(painted_state));
    CHECK(restored.has_facets(static_cast<EnforcerBlockerType>(painted_state)));
}

TEST_CASE("Compact strict painting meshes retain triangle coordinates and T-joints", "[TriangleSelector][PaintPreparation]")
{
    indexed_triangle_set input;
    input.vertices = {Vec3f(0, 0, 0), Vec3f(2, 0, 0), Vec3f(0, 2, 0), Vec3f(2, 2, 0), Vec3f(10, 0, 0), Vec3f(12, 0, 0), Vec3f(10, 2, 0)};
    input.indices  = {Vec3i32(0, 1, 2), Vec3i32(1, 3, 2), Vec3i32(4, 5, 6)};
    TriangleMesh     mesh(input);
    TriangleSelector selector(mesh);
    selector.set_facet(2, EnforcerBlockerType::Extruder3);
    const auto transform = Transform3d::Identity();
    selector.select_patch(0,
                          TriangleSelector::SinglePointCursor::cursor_factory(Vec3f(0.8f, 0.8f, 0.f), Vec3f(0.f, 0.f, 10.f), 0.4f,
                                                                              TriangleSelector::SPHERE, transform,
                                                                              TriangleSelector::ClippingPlane()),
                          EnforcerBlockerType::Extruder1, transform, true);
    const auto       annotation = selector.serialize();
    TriangleSelector restored(mesh);
    restored.deserialize(annotation, false);
    for (int state : {0, 1, 2, 3, 120}) {
        const auto reference = restored.get_facets_strict(EnforcerBlockerType(state));
        const auto compact   = restored.get_facets_strict_compact(EnforcerBlockerType(state));
        REQUIRE(compact.indices.size() == reference.indices.size());
        std::vector<bool> used(compact.vertices.size(), false);
        for (size_t i = 0; i < compact.indices.size(); ++i)
            for (int j = 0; j < 3; ++j) {
                REQUIRE(compact.indices[i](j) >= 0);
                REQUIRE(size_t(compact.indices[i](j)) < compact.vertices.size());
                CHECK(compact.vertices[compact.indices[i](j)] == reference.vertices[reference.indices[i](j)]);
                used[compact.indices[i](j)] = true;
            }
        CHECK(std::all_of(used.begin(), used.end(), [](bool value) { return value; }));
        if (state == 3)
            CHECK(compact.vertices.size() == 3);
        if (state == 2 || state == 120)
            CHECK(compact.vertices.empty());
    }
    CHECK(restored.serialize() == annotation);
}

TEST_CASE("Compact strict meshes preserve serial painted slab projections", "[TriangleSelector][PaintPreparation]")
{
    indexed_triangle_set patch;
    // Keep another live state before this patch, so compaction must renumber
    // the painted vertices as well as discard unrelated storage.
    patch.vertices = {Vec3f(50.f, 0.f, 0.f), Vec3f(51.f, 0.f, 0.f), Vec3f(50.f, 1.f, 1.f)};
    patch.indices.emplace_back(0, 1, 2);
    constexpr int side = 24;
    for (int y = 0; y <= side; ++y)
        for (int x = 0; x <= side; ++x)
            patch.vertices.emplace_back(float(x), float(y), float((x % 4) + 2 * (y % 3)) * 0.125f);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            if ((x + y) % 3 == 0)
                continue; // Open painted patches with holes and touching vertices.
            const int a = 3 + y * (side + 1) + x;
            patch.indices.emplace_back(a, a + 1, a + side + 1);
            patch.indices.emplace_back(a + 1, a + side + 2, a + side + 1);
        }
    TriangleMesh     mesh(patch);
    TriangleSelector selector(mesh);
    for (size_t i = 1; i < patch.indices.size(); ++i)
        selector.set_facet(int(i), EnforcerBlockerType::Extruder1);
    const auto full    = selector.get_facets_strict(EnforcerBlockerType::Extruder1);
    const auto compact = selector.get_facets_strict_compact(EnforcerBlockerType::Extruder1);
    REQUIRE(compact.vertices.size() < full.vertices.size());
    const std::vector<float> heights{0.f, 0.125f, 0.25f, 0.5f, 0.75f, 1.f};
    for (bool mirror : {false, true}) {
        Transform3d transform = Transform3d::Identity();
        if (mirror)
            transform.scale(Vec3d(-1., 1., -1.));
        auto project = [&](const indexed_triangle_set& triangles) {
            std::pair<std::vector<Polygons>, std::vector<Polygons>> out;
            slice_mesh_slabs(triangles, heights, transform, &out.first, &out.second, nullptr, [] {});
            return out;
        };
        // Isolate compaction from the existing scheduler-dependent ordering of
        // slab projection. Do not introduce a new production paint precedence.
        tbb::task_arena serial(1);
        const auto      expected = serial.execute([&] { return project(full); });
        CHECK(serial.execute([&] { return project(compact); }) == expected);
    }
}
