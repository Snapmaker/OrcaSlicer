#include <catch2/catch.hpp>

#include "../../src/slic3r/GUI/UltraExactFootprint.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI::UltraFit;

// Assembly "Exact highlight": the target face's shape placed on the moving part at the cursor, and the
// mate that lands it back. The case it exists for is a large moving face landing on a small target.

namespace {

// Axis-aligned box [0,sx]x[0,sy]x[0,sz], outward-facing triangles.
static indexed_triangle_set box(double sx, double sy, double sz)
{
    indexed_triangle_set its;
    for (int i = 0; i < 8; ++i)
        its.vertices.emplace_back(float((i & 1) ? sx : 0), float((i & 2) ? sy : 0), float((i & 4) ? sz : 0));
    const int q[6][4] = { {0,2,3,1}, {4,5,7,6}, {0,1,5,4}, {2,6,7,3}, {0,4,6,2}, {1,3,7,5} };
    for (const auto& f : q) {
        its.indices.emplace_back(f[0], f[1], f[2]);
        its.indices.emplace_back(f[0], f[2], f[3]);
    }
    return its;
}

// UV sphere of radius r about the origin, outward-facing triangles.
static indexed_triangle_set sphere(double r, int stacks = 48, int slices = 96)
{
    indexed_triangle_set its;
    for (int i = 0; i <= stacks; ++i) {
        const double th = M_PI * i / stacks;
        for (int j = 0; j < slices; ++j) {
            const double ph = 2 * M_PI * j / slices;
            its.vertices.emplace_back(float(r * std::sin(th) * std::cos(ph)), float(r * std::sin(th) * std::sin(ph)), float(r * std::cos(th)));
        }
    }
    auto id = [slices](int i, int j) { return i * slices + (j % slices); };
    for (int i = 0; i < stacks; ++i)
        for (int j = 0; j < slices; ++j) {
            if (i > 0)          its.indices.emplace_back(id(i, j), id(i + 1, j), id(i, j + 1));
            if (i < stacks - 1) its.indices.emplace_back(id(i, j + 1), id(i + 1, j), id(i + 1, j + 1));
        }
    return its;
}

static Vec3d facet_normal(const indexed_triangle_set& its, int t)
{
    const auto& f = its.indices[t];
    const Vec3d a = its.vertices[f[0]].cast<double>(), b = its.vertices[f[1]].cast<double>(), c = its.vertices[f[2]].cast<double>();
    return (b - a).cross(c - a).normalized();
}

static std::vector<int> facets_facing(const indexed_triangle_set& its, const Vec3d& dir, double min_dot)
{
    std::vector<int> out;
    for (int t = 0; t < (int) its.indices.size(); ++t)
        if (facet_normal(its, t).dot(dir) > min_dot) out.push_back(t);
    return out;
}

static Vec3d centroid(const indexed_triangle_set& its, const std::vector<int>& facets, const Transform3d& W)
{
    Vec3d s = Vec3d::Zero(); double a = 0;
    for (int t : facets) {
        const auto& f = its.indices[t];
        const Vec3d p0 = W * its.vertices[f[0]].cast<double>(), p1 = W * its.vertices[f[1]].cast<double>(), p2 = W * its.vertices[f[2]].cast<double>();
        const double ta = 0.5 * (p1 - p0).cross(p2 - p0).norm();
        s += (p0 + p1 + p2) / 3.0 * ta; a += ta;
    }
    return s / a;
}

} // namespace

TEST_CASE("Exact footprint: large plate lands on a small post where the cursor was", "[Assembly][ExactFootprint]")
{
    // Target: the top of a 10 x 10 x 10 post.
    const indexed_triangle_set post  = box(10, 10, 10);
    const std::vector<int>     top   = facets_facing(post, Vec3d::UnitZ(), 0.99);
    REQUIRE(top.size() == 2);
    // Moving: the bottom of a 100 x 60 x 5 plate, spun 30 deg and floating somewhere else.
    const indexed_triangle_set plate  = box(100, 60, 5);
    const std::vector<int>     bottom = facets_facing(plate, -Vec3d::UnitZ(), 0.99);
    const Transform3d plate_w = Transform3d(Eigen::Translation3d(40, -70, 25)) * Eigen::AngleAxisd(M_PI / 6, Vec3d::UnitZ());

    ExactFootprintInput in;
    in.t_its = &post;  in.t_facets = &top;    in.t_w = Transform3d::Identity();
    in.t_normal = Vec3d::UnitZ(); in.t_centre = Vec3d(5, 5, 10);
    in.a_its = &plate; in.a_facets = &bottom; in.a_w = plate_w;
    in.a_normal = plate_w.linear() * -Vec3d::UnitZ();

    SECTION("the cursor point, not the plate's centre, is what lands on the post") {
        for (const Vec3d cursor_local : { Vec3d(15, 12, 0), Vec3d(80, 45, 0), Vec3d(50, 30, 0) }) {
            in.a_hit = plate_w * cursor_local;
            const ExactFootprint fp = exact_footprint(in);
            REQUIRE(fp.ok);
            CHECK((fp.mate * in.a_hit - in.t_centre).norm() < 1e-6);
            // Faces meet anti-parallel: the plate's bottom ends up facing straight down onto the post.
            CHECK((fp.mate.linear() * in.a_normal + in.t_normal).norm() < 1e-9);
            CHECK(std::abs(fp.contact_shift) < 1e-6);
            CHECK(fp.missed == 0);
        }
    }

    SECTION("the footprint is the post's square, lying on the plate's bottom face") {
        in.a_hit = plate_w * Vec3d(20, 20, 0);
        const ExactFootprint fp = exact_footprint(in);
        REQUIRE(fp.ok);
        REQUIRE(fp.tris.size() == top.size() * 3);
        const Transform3d to_plate = plate_w.inverse();
        double area = 0;
        for (size_t i = 0; i < fp.tris.size(); i += 3) {
            for (int k = 0; k < 3; ++k) CHECK(std::abs((to_plate * fp.tris[i + k]).z()) < 1e-6);
            const Vec3d n = (fp.tris[i + 1] - fp.tris[i]).cross(fp.tris[i + 2] - fp.tris[i]);
            CHECK(n.normalized().dot(in.a_normal) > 0.999); // wound to face out of the plate
            area += 0.5 * n.norm();
        }
        CHECK(area == Approx(100.0).epsilon(1e-9));
        // And it maps back onto the post's top exactly.
        for (const Vec3d& q : fp.tris) {
            const Vec3d p = fp.mate * q;
            CHECK(p.z() == Approx(10.0).margin(1e-6));
            CHECK(p.x() > -1e-6); CHECK(p.x() < 10 + 1e-6);
            CHECK(p.y() > -1e-6); CHECK(p.y() < 10 + 1e-6);
        }
    }

    SECTION("with reference axes, the plate's edges line up with the post's") {
        in.a_hit  = plate_w * Vec3d(30, 20, 0);
        in.t_axis = Vec3d::UnitX();
        in.a_axis = plate_w.linear() * Vec3d::UnitX(); // plate's long side
        in.axis_fold = M_PI / 2;                        // the post is square
        const ExactFootprint fp = exact_footprint(in);
        REQUIRE(fp.ok);
        const Vec3d long_side = fp.mate.linear() * in.a_axis;
        CHECK(std::min(std::abs(long_side.x()), std::abs(long_side.y())) < 1e-9);
        CHECK(std::abs(fp.roll) <= M_PI / 4 + 1e-9);
        CHECK((fp.mate * in.a_hit - in.t_centre).norm() < 1e-6);
    }
}

TEST_CASE("Exact footprint: curved faces are draped and slid into contact", "[Assembly][ExactFootprint]")
{
    // Target: a small cap on top of a 20 mm ball. Moving: a big patch of a 30 mm ball, somewhere else.
    const indexed_triangle_set ball_t = sphere(20);
    const std::vector<int>     cap    = facets_facing(ball_t, Vec3d::UnitZ(), std::cos(15.0 * M_PI / 180));
    const indexed_triangle_set ball_a = sphere(30);
    const Vec3d                down   = Vec3d(0.3, 0.2, -1).normalized();
    const std::vector<int>     patch  = facets_facing(ball_a, down, std::cos(60.0 * M_PI / 180));
    REQUIRE(!cap.empty());
    REQUIRE(patch.size() > cap.size());
    const Transform3d ball_w = Transform3d(Eigen::Translation3d(100, 50, 60)) * Eigen::AngleAxisd(0.4, Vec3d(1, 1, 0).normalized());

    ExactFootprintInput in;
    in.t_its = &ball_t; in.t_facets = &cap; in.t_w = Transform3d::Identity();
    in.t_normal = Vec3d::UnitZ(); in.t_centre = centroid(ball_t, cap, Transform3d::Identity());
    in.a_its = &ball_a; in.a_facets = &patch; in.a_w = ball_w; in.a_curved = true;
    // Cursor somewhere on the patch, off its centre; the hint normal is the sphere normal there.
    const Vec3d cursor_dir = Vec3d(0.5, 0.1, -1).normalized();
    in.a_hit    = ball_w * (cursor_dir * 30.0);
    in.a_normal = ball_w.linear() * cursor_dir;

    const ExactFootprint fp = exact_footprint(in);
    REQUIRE(fp.ok);
    CHECK(fp.draped > 0);
    CHECK(fp.missed == 0);
    // Draped points sit on the 30 mm ball (within tessellation).
    const Transform3d to_ball = ball_w.inverse();
    for (const Vec3d& q : fp.tris) CHECK((to_ball * q).norm() == Approx(30.0).margin(0.3));
    // After the mate: the two convex caps touch -- nothing of the footprint is below the target cap,
    // and something is at it.
    double min_gap = 1e9;
    for (const Vec3d& q : fp.tris) {
        const Vec3d p = fp.mate * q;              // footprint point after the move
        min_gap = std::min(min_gap, p.norm() - 20.0); // distance outside the 20 mm target ball
    }
    CHECK(min_gap > -0.3);
    CHECK(min_gap < 0.3);
    // The spot under the cursor ends up over the target cap, on its axis.
    const Vec3d landed = fp.mate * in.a_hit;
    CHECK(std::hypot(landed.x(), landed.y()) < 0.5);
    CHECK(landed.z() > 19.0);
}

TEST_CASE("Exact footprint: folded axis roll", "[Assembly][ExactFootprint]")
{
    const Vec3d z = Vec3d::UnitZ();
    CHECK(exact_axis_roll(z, Vec3d::UnitX(), Vec3d::UnitY(), M_PI) == Approx(M_PI / 2));
    CHECK(exact_axis_roll(z, Vec3d::UnitX(), -Vec3d::UnitX(), M_PI) == Approx(0.0).margin(1e-12));
    CHECK(exact_axis_roll(z, Vec3d::UnitX(), Vec3d::UnitY(), M_PI / 2) == Approx(0.0).margin(1e-12));
    CHECK(exact_axis_roll(z, Vec3d::UnitX(), Vec3d(1, 1, 0), M_PI / 2) == Approx(M_PI / 4));
    CHECK(exact_axis_roll(z, Vec3d::UnitX(), Vec3d::Zero(), M_PI) == 0.0);
}
