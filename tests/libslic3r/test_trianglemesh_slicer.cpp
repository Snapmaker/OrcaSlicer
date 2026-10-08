#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>

#include <tbb/global_control.h>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

using namespace Slic3r;

// The slab slicer collects each slab's intersection lines from a parallel loop over the facets.
// Its loops, and therefore the projected polygons, are derived from the order of those lines, so
// the order has to be canonical or the same mesh projects to different polygons run to run.
// The single-threaded projection is the reference; every multi-threaded run must reproduce it
// exactly, vertex order included.
TEST_CASE("Slab slicing projects the same polygons whatever the thread schedule", "[TriangleMeshSlicer]")
{
    // A dense sphere, tilted so no facet is axis aligned: thousands of upward and downward
    // facing facets spread over every slab.
    indexed_triangle_set mesh = its_make_sphere(10., 0.05);
    Transform3d trafo = Transform3d::Identity();
    trafo.rotate(Eigen::AngleAxisd(0.37, Vec3d(0.3, 0.5, 1.).normalized()));
    trafo.translate(Vec3d(1., 2., 0.));
    std::vector<float> zs;
    for (float z = -9.7f; z < 9.7f; z += 0.2f)
        zs.emplace_back(z);

    auto project = [&mesh, &trafo, &zs]() {
        std::vector<Polygons> top, bottom;
        slice_mesh_slabs(mesh, zs, trafo, &top, &bottom, nullptr, []{});
        return std::make_pair(std::move(top), std::move(bottom));
    };

    std::pair<std::vector<Polygons>, std::vector<Polygons>> reference;
    {
        tbb::global_control single_thread(tbb::global_control::max_allowed_parallelism, 1);
        reference = project();
    }
    REQUIRE(reference.first.size() == zs.size());
    REQUIRE(std::any_of(reference.first.begin(), reference.first.end(), [](const Polygons &p) { return !p.empty(); }));
    REQUIRE(std::any_of(reference.second.begin(), reference.second.end(), [](const Polygons &p) { return !p.empty(); }));

    for (int run = 0; run < 3; ++run) {
        DYNAMIC_SECTION("multi-threaded run " << run)
        {
            auto parallel = project();
            CHECK(parallel.first == reference.first);
            CHECK(parallel.second == reference.second);
        }
    }
}

namespace {

// A vertical prism over a convex polygon. Every side face is a planar quad split into two triangles,
// so a slicing plane that crosses the shared diagonal adds a vertex that is not part of the contour.
indexed_triangle_set make_triangulated_prism(const std::vector<Vec2d> &outline, double height)
{
    indexed_triangle_set its;
    const int n = int(outline.size());
    for (const Vec2d &p : outline)
        its.vertices.emplace_back(float(p.x()), float(p.y()), 0.f);
    for (const Vec2d &p : outline)
        its.vertices.emplace_back(float(p.x()), float(p.y()), float(height));
    for (int i = 0; i < n; ++ i) {
        const int j = (i + 1) % n;
        its.indices.emplace_back(i, j, n + j);
        its.indices.emplace_back(i, n + j, n + i);
    }
    for (int i = 1; i + 1 < n; ++ i) {
        its.indices.emplace_back(0, i + 1, i);
        its.indices.emplace_back(n, n + i, n + i + 1);
    }
    return its;
}

// Number of vertices of the single contour sliced at each height, or -1 if there is not exactly one contour.
std::vector<int> contour_vertex_counts(const indexed_triangle_set &its, const std::vector<float> &zs, double rotation)
{
    MeshSlicingParams params;
    params.trafo = Transform3d::Identity();
    params.trafo.rotate(Eigen::AngleAxisd(rotation, Vec3d::UnitZ()));
    std::vector<Polygons> layers = slice_mesh(its, zs, params);
    std::vector<int> out;
    for (const Polygons &polygons : layers)
        out.emplace_back(polygons.size() == 1 ? int(polygons.front().size()) : -1);
    return out;
}

} // namespace

// A planar face made of several triangles must not leave the slicing plane's crossing of their shared
// edge in the contour (Orca #15316, #15366): the contour of a plain box is four points at any height,
// whatever the rotation, so contour simplification no longer depends on the slice height.
TEST_CASE("Slicing keeps no vertices where a slice plane crosses the diagonal of a planar quad", "[TriangleMeshSlicer]")
{
    const indexed_triangle_set box = make_triangulated_prism({ { 0., 0. }, { 30., 0. }, { 30., 20. }, { 0., 20. } }, 14.);
    std::vector<float> zs;
    for (float z = 0.37f; z < 13.9f; z += 0.41f)
        zs.emplace_back(z);
    for (const double rotation : { 0., 0.401, 1.1, 2.9 }) {
        CAPTURE(rotation);
        const std::vector<int> counts = contour_vertex_counts(box, zs, rotation);
        for (size_t i = 0; i < zs.size(); ++ i) {
            CAPTURE(zs[i]);
            CHECK(counts[i] == 4);
        }
    }
}

// Only junctions between coplanar triangles are dropped: a genuine, very shallow corner (about 3 degrees)
// stays in the contour (Orca #15364), unlike with a generic collinear-point cleanup.
TEST_CASE("Slicing preserves a shallow corner between two non-coplanar faces", "[TriangleMeshSlicer]")
{
    const indexed_triangle_set prism = make_triangulated_prism({ { 0., 0. }, { 20., 0. }, { 20., 10. }, { 10., 10.5 }, { 0., 10. } }, 10.);
    std::vector<float> zs;
    for (float z = 0.37f; z < 9.9f; z += 0.41f)
        zs.emplace_back(z);
    for (const double rotation : { 0., 0.401, 1.1 }) {
        CAPTURE(rotation);
        const std::vector<int> counts = contour_vertex_counts(prism, zs, rotation);
        for (size_t i = 0; i < zs.size(); ++ i) {
            CAPTURE(zs[i]);
            CHECK(counts[i] == 5);
        }
    }
}
