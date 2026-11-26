#include <catch2/catch_test_macros.hpp>
#include <future>
#include "slic3r/GUI/MeshUtils.hpp"

using namespace Slic3r;

TEST_CASE("Deferred picking preserves hits and normals", "[MeshRaycaster]")
{
    const auto mesh = std::make_shared<TriangleMesh>(make_cube(3, 4, 5));
    const GUI::MeshRaycaster picker(mesh);
    const auto copy = picker;

    auto first = std::async(std::launch::async, [&] { return &picker.get_aabb_mesh(); });
    auto second = std::async(std::launch::async, [&] { return &copy.get_aabb_mesh(); });
    REQUIRE(first.get() == second.get());

    const AABBMesh reference(*mesh, true);
    for (const Vec3d& origin : { Vec3d(1, 2, 8), Vec3d(10, 20, 8) }) {
        const Vec3d direction(0, 0, -1);
        const auto expected = reference.query_ray_hits(origin, direction);
        const auto actual = picker.get_aabb_mesh().query_ray_hits(origin, direction);
        REQUIRE(actual.size() == expected.size());
        for (size_t i = 0; i < actual.size(); ++i) {
            CHECK(actual[i].face() == expected[i].face());
            CHECK(actual[i].distance() == expected[i].distance());
            CHECK(actual[i].normal() == expected[i].normal());
        }
    }

    const auto normals = its_face_normals(mesh->its);
    for (size_t i = 0; i < normals.size(); ++i)
        CHECK(picker.get_triangle_normal(i) == normals[i]);

    Vec3f normal;
    CHECK(picker.get_closest_point(Vec3f(1, 2, 8), &normal) == Vec3f(1, 2, 5));
    CHECK(normal == Vec3f(0, 0, 1));
}
