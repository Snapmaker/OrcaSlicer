#include <catch2/catch_all.hpp>
#include "test_utils.hpp"

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/MeshBoolean.hpp>

using namespace Slic3r;

TEST_CASE("CGAL and TriangleMesh conversions", "[MeshBoolean]") {
    TriangleMesh sphere = make_sphere(1.);
    
    auto cgalmesh_ptr = MeshBoolean::cgal::triangle_mesh_to_cgal(sphere);
    
    REQUIRE(cgalmesh_ptr);
    REQUIRE(! MeshBoolean::cgal::does_self_intersect(*cgalmesh_ptr));
    
    TriangleMesh M = MeshBoolean::cgal::cgal_to_triangle_mesh(*cgalmesh_ptr);
    
    REQUIRE(M.its.vertices.size() == sphere.its.vertices.size());
    REQUIRE(M.its.indices.size() == sphere.its.indices.size());
    
    REQUIRE(M.volume() == Catch::Approx(sphere.volume()));
    
    REQUIRE(! MeshBoolean::cgal::does_self_intersect(M));
}

// The repair behind "Fix Model": a mesh with a missing facet comes back closed at its old volume.
// Hidden; run with the [MeshRepair] tag.
TEST_CASE("CGAL repair closes a mesh with a missing facet", "[.][MeshRepair]") {
    auto check_repair = [](const indexed_triangle_set &closed_its) {
        TriangleMesh closed(closed_its);
        REQUIRE(closed.stats().manifold());
        const float closed_volume = closed.volume();

        indexed_triangle_set open_its = closed_its;
        open_its.indices.pop_back();
        TriangleMesh mesh(open_its);
        REQUIRE(mesh.stats().open_edges > 0);

        std::string error;
        REQUIRE(MeshBoolean::cgal::repair(mesh, nullptr, &error));
        CHECK(error.empty());
        CHECK(mesh.stats().open_edges == 0);
        CHECK(its_num_open_edges(mesh.its) == 0);
        CHECK(mesh.volume() == Catch::Approx(closed_volume).epsilon(0.01));
    };

    SECTION("20 mm cube") {
        check_repair(its_make_cube(20., 20., 20.));
    }
    SECTION("Prusa.stl from the test data") {
        TriangleMesh loaded;
        const std::string path = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(loaded.ReadSTLFile(path.c_str()));
        check_repair(loaded.its);
    }
}
