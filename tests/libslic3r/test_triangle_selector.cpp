#include <catch2/catch_test_macros.hpp>

#include "libslic3r/TriangleMesh.hpp"
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

    TriangleMesh mesh(its);
    TriangleSelector selector(mesh);

    constexpr int painted_state = 120;
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

// #1279: the paint gizmo's Remap leaves state 0 (never painted) out of the map, so those faces keep
// following their part's filament while painted faces move to the destination filament.
TEST_CASE("Triangle selector remap moves painted faces and keeps unpainted ones", "[TriangleSelector][MMUPaint]")
{
    indexed_triangle_set its;
    its.vertices = {
        Vec3f(0.f, 0.f, 0.f),
        Vec3f(1.f, 0.f, 0.f),
        Vec3f(1.f, 1.f, 0.f),
        Vec3f(0.f, 1.f, 0.f),
    };
    its.indices = {
        stl_triangle_vertex_indices(0, 1, 2),
        stl_triangle_vertex_indices(0, 2, 3),
    };

    TriangleMesh     mesh(its);
    TriangleSelector selector(mesh);
    selector.set_facet(0, EnforcerBlockerType::Extruder1);

    EnforcerBlockerStateMap state_map;
    for (size_t i = 0; i < state_map.size(); ++i)
        state_map[i] = static_cast<EnforcerBlockerType>(i);
    state_map[size_t(EnforcerBlockerType::Extruder1)] = EnforcerBlockerType::Extruder3;

    selector.remap_triangle_state(state_map);

    CHECK(selector.get_facets_strict(EnforcerBlockerType::Extruder3).indices.size() == 1);
    CHECK(selector.get_facets_strict(EnforcerBlockerType::Extruder1).indices.empty());
    CHECK(selector.get_facets_strict(EnforcerBlockerType::NONE).indices.size() == 1);
}
