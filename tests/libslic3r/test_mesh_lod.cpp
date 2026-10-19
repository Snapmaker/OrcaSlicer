// Snapmaker Orca: render LOD, the GUI-less part (libslic3r/MeshLod). Pins the level rule and
// error budgets: select_lod_level(), build_lod_meshes(), estimate_lod_job_bytes(), GUI-side rules.
#include <catch2/catch_all.hpp>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#include "libslic3r/Config.hpp"
#include "libslic3r/MeshLod.hpp"
#include "libslic3r/QuadricEdgeCollapse.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

namespace {

constexpr int VIEWPORT_W = 1000;
constexpr int VIEWPORT_H = 800;

// Orthographic view-projection under which one world unit is one pixel on both axes.
Transform3d pixel_view_proj()
{
    Transform3d t = Transform3d::Identity();
    t.matrix()(0, 0) = 2. / VIEWPORT_W;
    t.matrix()(1, 1) = 2. / VIEWPORT_H;
    return t;
}

BoundingBoxf3 box_of_pixels(double width, double height)
{
    return BoundingBoxf3(Vec3d(-0.5 * width, -0.5 * height, -1.), Vec3d(0.5 * width, 0.5 * height, 1.));
}

// Upstream's rule without the band, so that the result does not depend on the previous level.
LodParams params_without_hysteresis()
{
    LodParams p;
    p.hysteresis = 0.f;
    return p;
}

LodLevel level_of(double width, double height, LodLevel prev, const LodParams &params)
{
    return select_lod_level(box_of_pixels(width, height), pixel_view_proj(), VIEWPORT_W, VIEWPORT_H, prev, params);
}

// Perspective projection looking down -z from the origin (view = identity).
Transform3d perspective_view_proj()
{
    const double f = 1. / std::tan(0.5 * 45. * M_PI / 180.);
    const double z_near = 1., z_far = 1000.;
    Eigen::Matrix4d m = Eigen::Matrix4d::Zero();
    m(0, 0) = f * VIEWPORT_H / VIEWPORT_W;
    m(1, 1) = f;
    m(2, 2) = (z_far + z_near) / (z_near - z_far);
    m(2, 3) = 2. * z_far * z_near / (z_near - z_far);
    m(3, 2) = -1.;
    Transform3d t;
    t.matrix() = m;
    return t;
}

uint64_t hash_of(const indexed_triangle_set &its)
{
    uint64_t h = 1469598103934665603ull; // FNV-1a
    auto feed = [&h](const void *data, size_t bytes) {
        const unsigned char *p = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < bytes; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    for (const stl_vertex &v : its.vertices)
        feed(v.data(), 3 * sizeof(float));
    for (const stl_triangle_vertex_indices &t : its.indices)
        feed(t.data(), 3 * sizeof(int));
    return h;
}

// its_make_sphere yields about 4 pi^2 / fa^2 faces.
indexed_triangle_set sphere_with_sectors(double radius, int sectors) { return its_make_sphere(radius, 2. * M_PI / sectors); }

const LodLevel ALL_LEVELS[] = {LodLevel::High, LodLevel::Middle, LodLevel::Small};

} // namespace

TEST_CASE("An object over the maximum size on either axis is drawn at full detail", "[MeshLod]")
{
    const LodParams params = params_without_hysteresis();
    for (LodLevel prev : ALL_LEVELS) {
        CHECK(level_of(400., 50., prev, params) == LodLevel::High);
        CHECK(level_of(50., 250., prev, params) == LodLevel::High);
        // The thresholds themselves belong to High (upstream compares with >=).
        CHECK(level_of(300., 50., prev, params) == LodLevel::High);
        CHECK(level_of(50., 200., prev, params) == LodLevel::High);
    }
}

TEST_CASE("An object under the minimum size on both axes takes the Small level", "[MeshLod]")
{
    const LodParams params = params_without_hysteresis();
    for (LodLevel prev : ALL_LEVELS) {
        CHECK(level_of(140., 100., prev, params) == LodLevel::Small);
        CHECK(level_of(150., 110., prev, params) == LodLevel::Small);
        // The height reaches the maximum: the High test comes first.
        CHECK(level_of(140., 200., prev, params) == LodLevel::High);
        // Only one axis under the minimum is not enough for Small.
        CHECK(level_of(140., 150., prev, params) == LodLevel::Middle);
        CHECK(level_of(200., 100., prev, params) == LodLevel::Middle);
        CHECK(level_of(200., 150., prev, params) == LodLevel::Middle);
    }
}

TEST_CASE("A size oscillating around a threshold keeps the previous level", "[MeshLod]")
{
    const LodParams params; // 10 % band
    SECTION("a box oscillating 5 % around the Small threshold keeps the previous level") {
        for (LodLevel prev : {LodLevel::Middle, LodLevel::Small}) {
            CHECK(level_of(150. * 1.05, 110. * 1.05, prev, params) == prev);
            CHECK(level_of(150. * 0.95, 110. * 0.95, prev, params) == prev);
            CHECK(level_of(150., 110., prev, params) == prev);
        }
    }
    SECTION("a box oscillating 5 % around the High threshold keeps the previous level") {
        for (LodLevel prev : {LodLevel::High, LodLevel::Middle}) {
            CHECK(level_of(300. * 1.05, 120., prev, params) == prev);
            CHECK(level_of(300. * 0.95, 120., prev, params) == prev);
            CHECK(level_of(160., 200. * 1.05, prev, params) == prev);
            CHECK(level_of(160., 200. * 0.95, prev, params) == prev);
        }
    }
    SECTION("beyond the band the level changes") {
        CHECK(level_of(150. * 0.85, 110. * 0.85, LodLevel::Middle, params) == LodLevel::Small);
        CHECK(level_of(150. * 1.15, 110. * 1.15, LodLevel::Small, params) == LodLevel::Middle);
        CHECK(level_of(300. * 1.15, 120., LodLevel::Middle, params) == LodLevel::High);
        CHECK(level_of(300. * 0.85, 120., LodLevel::High, params) == LodLevel::Middle);
        // Two levels at once, in both directions.
        CHECK(level_of(100., 80., LodLevel::High, params) == LodLevel::Small);
        CHECK(level_of(500., 400., LodLevel::Small, params) == LodLevel::High);
    }
}

TEST_CASE("An object reaching behind the eye is drawn at full detail", "[MeshLod]")
{
    const Transform3d   view_proj = perspective_view_proj();
    const LodParams     params    = params_without_hysteresis();
    // Far away and tiny on screen: Small.
    const BoundingBoxf3 far_box(Vec3d(-1., -1., -510.), Vec3d(1., 1., -500.));
    CHECK(select_lod_level(far_box, view_proj, VIEWPORT_W, VIEWPORT_H, LodLevel::High, params) == LodLevel::Small);
    // The same footprint, but the box straddles the eye plane: four corners have w <= 0.
    const BoundingBoxf3 straddling_box(Vec3d(-1., -1., -510.), Vec3d(1., 1., 5.));
    for (LodLevel prev : ALL_LEVELS)
        CHECK(select_lod_level(straddling_box, view_proj, VIEWPORT_W, VIEWPORT_H, prev, params) == LodLevel::High);
    // Entirely behind the eye.
    const BoundingBoxf3 behind_box(Vec3d(-1., -1., 5.), Vec3d(1., 1., 10.));
    CHECK(select_lod_level(behind_box, view_proj, VIEWPORT_W, VIEWPORT_H, LodLevel::Small, params) == LodLevel::High);
    // A corner exactly on the eye plane (w == 0).
    const BoundingBoxf3 touching_box(Vec3d(-1., -1., -510.), Vec3d(1., 1., 0.));
    CHECK(select_lod_level(touching_box, view_proj, VIEWPORT_W, VIEWPORT_H, LodLevel::Small, params) == LodLevel::High);
}

TEST_CASE("A viewport without area gives full detail", "[MeshLod]")
{
    const LodParams     params = params_without_hysteresis();
    const BoundingBoxf3 box    = box_of_pixels(10., 10.);
    CHECK(select_lod_level(box, pixel_view_proj(), VIEWPORT_W, VIEWPORT_H, LodLevel::Small, params) == LodLevel::Small);
    CHECK(select_lod_level(box, pixel_view_proj(), 0, VIEWPORT_H, LodLevel::Small, params) == LodLevel::High);
    CHECK(select_lod_level(box, pixel_view_proj(), VIEWPORT_W, 0, LodLevel::Small, params) == LodLevel::High);
    CHECK(select_lod_level(box, pixel_view_proj(), -1, -1, LodLevel::Small, params) == LodLevel::High);
    // An undefined box has no size.
    CHECK(select_lod_level(BoundingBoxf3(), pixel_view_proj(), VIEWPORT_W, VIEWPORT_H, LodLevel::Small, params) == LodLevel::High);
}

TEST_CASE("The pixel scale multiplies both size thresholds", "[MeshLod]")
{
    LodParams params = params_without_hysteresis();
    CHECK(level_of(280., 200., LodLevel::High, params) == LodLevel::High);
    CHECK(level_of(280., 150., LodLevel::High, params) == LodLevel::Middle);
    params.pixel_scale = 2.f;
    CHECK(level_of(280., 200., LodLevel::High, params) == LodLevel::Small);  // under 300 x 220
    CHECK(level_of(280., 150., LodLevel::High, params) == LodLevel::Small);
    CHECK(level_of(280., 300., LodLevel::High, params) == LodLevel::Middle); // under 600 x 400
    CHECK(level_of(280., 400., LodLevel::High, params) == LodLevel::High);
    CHECK(level_of(600., 100., LodLevel::High, params) == LodLevel::High);
}

TEST_CASE("A fine sphere yields two reduced meshes inside its bounding box", "[MeshLod]")
{
    const indexed_triangle_set src = sphere_with_sectors(20., 460);
    const LodParams            params;
    REQUIRE(src.indices.size() > 200000);
    REQUIRE(src.indices.size() <= params.super_large_faces); // the "large" size class
    const uint64_t hash_before = hash_of(src);

    const LodMeshes lod = build_lod_meshes(src, params, nullptr);

    REQUIRE_FALSE(lod.middle.indices.empty());
    REQUIRE_FALSE(lod.small_mesh.indices.empty());
    CHECK(lod.middle.indices.size() < src.indices.size());
    CHECK(lod.small_mesh.indices.size() < lod.middle.indices.size());
    CHECK(float(lod.middle.indices.size()) <= params.min_reduction * float(src.indices.size()));
    CHECK(float(lod.small_mesh.indices.size()) <= params.min_reduction * float(lod.middle.indices.size()));
    CHECK(lod.middle_error > 0.f);
    CHECK(lod.middle_error <= params.middle_err[1]);
    CHECK(lod.small_error > 0.f);
    CHECK(lod.small_error <= params.small_err[1]);

    const float limit = 20.f + params.aabb_epsilon;
    for (const indexed_triangle_set *its : {&lod.middle, &lod.small_mesh}) {
        for (const stl_vertex &v : its->vertices)
            REQUIRE(v.cwiseAbs().maxCoeff() < limit);
        for (const stl_triangle_vertex_indices &t : its->indices)
            for (int i = 0; i < 3; ++i) {
                REQUIRE(t[i] >= 0);
                REQUIRE(size_t(t[i]) < its->vertices.size());
            }
    }
    CHECK(hash_of(src) == hash_before);
}

TEST_CASE("Both reduced meshes are simplified from the source", "[MeshLod]")
{
    const indexed_triangle_set src = sphere_with_sectors(20., 240);
    const LodParams            params;
    REQUIRE(src.indices.size() >= params.min_faces);
    REQUIRE(src.indices.size() <= params.large_faces); // the "normal" size class

    const LodMeshes lod = build_lod_meshes(src, params, nullptr);
    REQUIRE_FALSE(lod.small_mesh.indices.empty());

    indexed_triangle_set direct = src;
    float                error  = params.small_err[0];
    its_quadric_edge_collapse(direct, 0, &error);
    CHECK(direct.indices.size() == lod.small_mesh.indices.size());
    CHECK(direct.vertices.size() == lod.small_mesh.vertices.size());
    CHECK_THAT(error, Catch::Matchers::WithinRel(lod.small_error, 1e-6f));

    // Middle likewise, with its own budget.
    direct = src;
    error  = params.middle_err[0];
    its_quadric_edge_collapse(direct, 0, &error);
    CHECK(direct.indices.size() == lod.middle.indices.size());
    CHECK_THAT(error, Catch::Matchers::WithinRel(lod.middle_error, 1e-6f));
}

TEST_CASE("A mesh that does not reduce well gets no reduced copy", "[MeshLod]")
{
    auto is_empty = [](const LodMeshes &lod) {
        return lod.middle.indices.size() + lod.middle.vertices.size() + lod.small_mesh.indices.size() + lod.small_mesh.vertices.size() == 0;
    };
    LodParams no_minimum;
    no_minimum.min_faces = 0;

    SECTION("empty source") {
        CHECK(is_empty(build_lod_meshes(indexed_triangle_set(), no_minimum, nullptr)));
    }
    SECTION("12-face cube") {
        const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
        REQUIRE(cube.indices.size() == 12);
        CHECK(is_empty(build_lod_meshes(cube, LodParams(), nullptr)));
        // Without the minimum the cube is still rejected: whatever collapses, the result either
        // keeps more than 70 % of the faces or lost more than half of fewer than 1000.
        CHECK(is_empty(build_lod_meshes(cube, no_minimum, nullptr)));
    }
    SECTION("mesh under min_faces") {
        const indexed_triangle_set sphere = sphere_with_sectors(20., 100);
        REQUIRE(sphere.indices.size() > 1000);
        REQUIRE(sphere.indices.size() < LodParams().min_faces);
        CHECK(is_empty(build_lod_meshes(sphere, LodParams(), nullptr)));
        // The same mesh passes once the minimum is out of the way.
        CHECK_FALSE(build_lod_meshes(sphere, no_minimum, nullptr).middle.indices.empty());
    }
    SECTION("reduction stays above 70 %") {
        // A coarse, large sphere: every collapse costs far more than the budgets allow.
        const indexed_triangle_set sphere = sphere_with_sectors(1000., 48);
        REQUIRE(sphere.indices.size() > 1000);
        CHECK(is_empty(build_lod_meshes(sphere, no_minimum, nullptr)));
    }
}

TEST_CASE("A cancel callback that throws stops the build of the reduced meshes", "[MeshLod]")
{
    struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("cancelled") {} };
    const indexed_triangle_set src = sphere_with_sectors(20., 240);
    REQUIRE(src.indices.size() >= LodParams().min_faces);
    const uint64_t hash_before = hash_of(src);

    SECTION("on the first call") {
        std::atomic<int> calls{0}; // its_quadric_edge_collapse also calls back from inside tbb::parallel_for
        CHECK_THROWS_AS(build_lod_meshes(src, LodParams(), [&calls] { ++calls; throw Cancelled(); }), Cancelled);
        CHECK(calls == 1);
    }
    SECTION("in the middle of the simplification") {
        std::atomic<int> calls{0}; // its_quadric_edge_collapse also calls back from inside tbb::parallel_for
        CHECK_THROWS_AS(build_lod_meshes(src, LodParams(), [&calls] { if (++calls == 200) throw Cancelled(); }), Cancelled);
        CHECK(calls == 200);
    }
    CHECK(hash_of(src) == hash_before);
}

TEST_CASE("The memory estimate of a job grows with the mesh", "[MeshLod]")
{
    CHECK(estimate_lod_job_bytes(0, 0) == 0);
    size_t prev = 0;
    for (size_t faces : {size_t(1000), size_t(20000), size_t(100000), size_t(2000000)}) {
        const size_t bytes = estimate_lod_job_bytes(faces, faces / 2);
        CHECK(bytes > prev);
        // At least the working copy and one result of the same size.
        CHECK(bytes >= 2 * (faces * 3 * sizeof(int) + (faces / 2) * 3 * sizeof(float)));
        prev = bytes;
    }
    CHECK(estimate_lod_job_bytes(1000, 600) > estimate_lod_job_bytes(1000, 500));
    CHECK(estimate_lod_job_bytes(1001, 500) > estimate_lod_job_bytes(1000, 500));
}

TEST_CASE("Jobs worth a retry are looked for at most once per interval, and less often while nothing helps", "[MeshLod]")
{
    LodRetryTimer timer;
    // Never while the feature is off, whatever the clock says.
    CHECK_FALSE(timer.due(false, 0));
    CHECK_FALSE(timer.due(false, 1000000));
    // The first look is due at once.
    CHECK(timer.due(true, 5000));

    // A pass that found nothing: the next one is a second later, not earlier.
    timer.passed(5000, 0);
    CHECK(timer.interval_ms() == LodRetryTimer::min_interval_ms);
    CHECK_FALSE(timer.due(true, 5000));
    CHECK_FALSE(timer.due(true, 5999));
    CHECK(timer.due(true, 6000));
    CHECK_FALSE(timer.due(false, 6000));

    // Memory stays short: every pass submits again and the interval doubles up to its cap, so
    // a scene that is drawn at 60 frames per second makes 5 passes in the first 31 seconds.
    int64_t now    = 6000;
    int64_t expect = LodRetryTimer::min_interval_ms;
    size_t  passes = 0;
    for (int64_t frame = now; frame < now + 31000; frame += 16)
        if (timer.due(true, frame)) {
            timer.passed(frame, 1);
            ++passes;
            expect = std::min<int64_t>(expect * 2, LodRetryTimer::max_interval_ms);
            CHECK(timer.interval_ms() == expect);
        }
    CHECK(passes == 5);
    CHECK(timer.interval_ms() == LodRetryTimer::max_interval_ms);

    // The first pass with nothing left to submit brings the short interval back.
    timer.passed(100000, 0);
    CHECK(timer.interval_ms() == LodRetryTimer::min_interval_ms);
    CHECK(timer.due(true, 101000));
}

TEST_CASE("The pin height is the lowest printable height that holds a value", "[MeshLod]")
{
    const double nil = ConfigOptionFloatsNullable::nil_value();
    REQUIRE(std::isnan(nil));

    // One head or none: the shader does not darken anything, nothing is pinned.
    CHECK(lod_pin_height({}) == DBL_MAX);
    CHECK(lod_pin_height({180.}) == DBL_MAX);
    CHECK(lod_pin_height({nil}) == DBL_MAX);

    CHECK(lod_pin_height({270., 180.}) == 180.);
    CHECK(lod_pin_height({180., 270.}) == 180.);
    CHECK(lod_pin_height({270., 270., 200., 270.}) == 200.);

    // A nil entry is skipped wherever it stands. std::min_element returned the NaN itself for
    // a leading nil, and a comparison with that pinned nothing.
    CHECK(lod_pin_height({nil, 270., 180.}) == 180.);
    CHECK(lod_pin_height({270., nil, 180.}) == 180.);
    CHECK(lod_pin_height({270., 180., nil}) == 180.);
    CHECK(lod_pin_height({nil, 180.}) == 180.);

    // All nil: no pin, and a value the caller can compare with.
    CHECK(lod_pin_height({nil, nil}) == DBL_MAX);
    CHECK(lod_pin_height({nil, nil, nil, nil}) == DBL_MAX);
}

TEST_CASE("Wakes of the canvases are at least half a second apart", "[MeshLod]")
{
    // No wake yet: now.
    CHECK(lod_wake_delay_ms(0, -1) == 0);
    CHECK(lod_wake_delay_ms(123456, -1) == 0);
    // Inside the interval: the rest of it.
    CHECK(lod_wake_delay_ms(1000, 1000) == 500);
    CHECK(lod_wake_delay_ms(1001, 1000) == 499);
    CHECK(lod_wake_delay_ms(1499, 1000) == 1);
    // At its end and later: now.
    CHECK(lod_wake_delay_ms(1500, 1000) == 0);
    CHECK(lod_wake_delay_ms(999999, 1000) == 0);
    // A clock that went backwards must not postpone the wake for longer than an interval.
    CHECK(lod_wake_delay_ms(900, 1000) == 0);
    CHECK(lod_wake_delay_ms(1100, 1000, 2000) == 1900);

    // Jobs that finish every 100 ms for three seconds: a wake at 0, 500, ... 3000 ms. A request
    // inside the interval is served by one timer, the requests that follow ride on it.
    int64_t last_wake = -1, timer_due = -1;
    int     wakes = 0;
    for (int64_t now = 0; now <= 3000; now += 100) {
        if (timer_due >= 0 && now >= timer_due) {
            last_wake = timer_due;
            timer_due = -1;
            ++wakes;
        }
        if (now == 3000)
            break;
        const int64_t delay = lod_wake_delay_ms(now, last_wake);
        if (delay == 0) {
            last_wake = now;
            ++wakes;
        } else if (timer_due < 0)
            timer_due = now + delay;
    }
    CHECK(wakes == 7);
}
