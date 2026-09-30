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

namespace {

static Vec3d area_centroid(const std::vector<Vec3d>& tris)
{
    Vec3d s = Vec3d::Zero(); double a = 0;
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        const double ta = 0.5 * (tris[i + 1] - tris[i]).cross(tris[i + 2] - tris[i]).norm();
        s += (tris[i] + tris[i + 1] + tris[i + 2]) / 3.0 * ta; a += ta;
    }
    return s / a;
}

static double rotation_angle(const Matrix3d& r) { return Eigen::AngleAxisd(r).angle(); }

} // namespace

TEST_CASE("Exact footprint: the reference point of the shape is exactly under the cursor", "[Assembly][ExactFootprint]")
{
    // Target: the top of a 10 x 20 block. Its feature "centre" is deliberately off (a border-vertex mean dragged
    // toward one end), which must not matter: the shape's own centroid goes under the cursor.
    const indexed_triangle_set block = box(10, 20, 10);
    const std::vector<int>     top   = facets_facing(block, Vec3d::UnitZ(), 0.99);
    const indexed_triangle_set plate = box(100, 60, 5);
    const std::vector<int>     bottom = facets_facing(plate, -Vec3d::UnitZ(), 0.99);
    const Transform3d plate_w = Transform3d(Eigen::Translation3d(40, -70, 25)) * Eigen::AngleAxisd(M_PI / 6, Vec3d::UnitZ());

    ExactFootprintInput in;
    in.t_its = &block; in.t_facets = &top; in.t_w = Transform3d::Identity();
    in.t_normal = Vec3d::UnitZ(); in.t_centre = Vec3d(5, 2, 10);
    in.a_its = &plate; in.a_facets = &bottom; in.a_w = plate_w;
    in.a_normal = plate_w.linear() * -Vec3d::UnitZ();

    for (const Vec3d cursor_local : { Vec3d(15, 12, 0), Vec3d(80, 45, 0), Vec3d(50.123, 30.456, 0) }) {
        in.a_hit = plate_w * cursor_local;
        const ExactFootprint fp = exact_footprint(in);
        REQUIRE(fp.ok);
        CHECK((fp.ref_target - Vec3d(5, 10, 10)).norm() < 1e-9);          // area centroid, not t_centre
        CHECK((fp.anchor - in.a_hit).norm() < 1e-12);
        CHECK((area_centroid(fp.tris) - in.a_hit).norm() < 1e-9);          // the drawn shape is centred on the cursor hit
        CHECK((fp.mate * in.a_hit - fp.ref_target).norm() < 1e-9);         // and Auto-fit puts that point on the target's
    }
}

TEST_CASE("Exact footprint: on a curved face the reference point stays on the surface under the cursor", "[Assembly][ExactFootprint]")
{
    const indexed_triangle_set ball_t = sphere(20);
    const std::vector<int>     cap    = facets_facing(ball_t, Vec3d::UnitZ(), std::cos(15.0 * M_PI / 180));
    const indexed_triangle_set ball_a = sphere(30);
    const std::vector<int>     patch  = facets_facing(ball_a, Vec3d(0.3, 0.2, -1).normalized(), std::cos(60.0 * M_PI / 180));
    const Transform3d ball_w = Transform3d(Eigen::Translation3d(100, 50, 60)) * Eigen::AngleAxisd(0.4, Vec3d(1, 1, 0).normalized());

    ExactFootprintInput in;
    in.t_its = &ball_t; in.t_facets = &cap; in.t_w = Transform3d::Identity();
    in.t_normal = Vec3d::UnitZ(); in.t_centre = Vec3d(3, 3, 20);
    in.a_its = &ball_a; in.a_facets = &patch; in.a_w = ball_w; in.a_curved = true;

    for (const Vec3d dir : { Vec3d(0.5, 0.1, -1), Vec3d(-0.3, 0.4, -1), Vec3d(0.1, -0.5, -1) }) {
        const Vec3d d = dir.normalized();
        in.a_hit    = ball_w * (d * 30.0);
        in.a_normal = ball_w.linear() * d;
        const ExactFootprint fp = exact_footprint(in);
        REQUIRE(fp.ok);
        CHECK((fp.anchor - in.a_hit).norm() < 1e-12);
        // Auto-fit carries the cursor point onto the target's reference point, along the target normal only.
        const Vec3d landed = fp.mate * in.a_hit;
        CHECK(std::hypot((landed - fp.ref_target).x(), (landed - fp.ref_target).y()) < 1e-9);
    }
}

TEST_CASE("Exact footprint: the roll holds while the cursor moves", "[Assembly][ExactFootprint]")
{
    const indexed_triangle_set block = box(10, 20, 10);
    const std::vector<int>     top   = facets_facing(block, Vec3d::UnitZ(), 0.99);
    const indexed_triangle_set plate = box(100, 60, 5);
    const std::vector<int>     bottom = facets_facing(plate, -Vec3d::UnitZ(), 0.99);
    const Transform3d plate_w = Transform3d(Eigen::Translation3d(40, -70, 25)) * Eigen::AngleAxisd(0.7, Vec3d(0.2, 0.3, 1).normalized());

    ExactFootprintInput in;
    in.t_its = &block; in.t_facets = &top; in.t_w = Transform3d::Identity();
    in.t_normal = Vec3d::UnitZ(); in.t_centre = Vec3d(5, 10, 10);
    in.t_axis = Vec3d::UnitY();
    in.a_its = &plate; in.a_facets = &bottom; in.a_w = plate_w;
    in.a_normal = plate_w.linear() * -Vec3d::UnitZ();
    in.a_axis = plate_w.linear() * Vec3d::UnitX(); // the plate's long side
    in.axis_fold = M_PI;

    ExactRollState state;
    in.roll_state = &state;
    in.a_hit = plate_w * Vec3d(20, 20, 0);
    const ExactFootprint first = exact_footprint(in);
    REQUIRE(first.ok);
    CHECK(!first.roll_reused);
    CHECK(state.valid);
    CHECK(std::abs(first.roll) > 0.1); // the plate's long side is not along the block's: a real roll was decided

    SECTION("small cursor moves never re-roll, whatever the axes the caller measures at the new spot") {
        for (int i = 1; i <= 40; ++i) {
            in.a_hit = plate_w * Vec3d(20 + 1.5 * i, 20 + 0.7 * i, 0);
            // Garbage axes, as a tiny facet patch under the cursor would give.
            in.a_axis = plate_w.linear() * Vec3d(std::cos(i * 1.3), std::sin(i * 1.3), 0);
            const ExactFootprint fp = exact_footprint(in);
            REQUIRE(fp.ok);
            CHECK(fp.roll_reused);
            CHECK(fp.roll == first.roll);
            CHECK((fp.mate.linear() - first.mate.linear()).norm() < 1e-12); // same orientation, only the spot moves
            CHECK((fp.mate * in.a_hit - fp.ref_target).norm() < 1e-9);
        }
    }

    SECTION("without the memory the same garbage axes would spin it") {
        in.roll_state = nullptr;
        in.a_hit = plate_w * Vec3d(40, 30, 0);
        in.a_axis = plate_w.linear() * Vec3d(std::cos(2.0), std::sin(2.0), 0);
        const ExactFootprint spun = exact_footprint(in);
        CHECK(std::abs(spun.roll - first.roll) > 0.1);
    }

    SECTION("a normal that turns past the hysteresis onto a comparable face rolls again") {
        // A 30 deg turn of the normal stands for moving to another face of the part.
        const Transform3d other_w = plate_w * Eigen::AngleAxisd(M_PI / 6, Vec3d::UnitY());
        in.a_w = other_w;
        in.a_normal = other_w.linear() * -Vec3d::UnitZ();
        in.a_axis = other_w.linear() * Vec3d::UnitX();
        in.a_hit = other_w * Vec3d(30, 30, 0);
        CHECK(exact_roll_needs_axes(&state, in.a_normal, in.roll_hysteresis));
        const ExactFootprint fp = exact_footprint(in);
        REQUIRE(fp.ok);
        CHECK(!fp.roll_reused);
        // ... and settles there: the next small move keeps the new roll.
        in.a_hit = other_w * Vec3d(35, 31, 0);
        const ExactFootprint next = exact_footprint(in);
        CHECK(next.roll_reused);
        CHECK(next.roll == fp.roll);
    }

    SECTION("a normal turning by less than the hysteresis keeps the roll") {
        const Transform3d tilt_w = plate_w * Eigen::AngleAxisd(5.0 * M_PI / 180.0, Vec3d::UnitY());
        in.a_w = tilt_w;
        in.a_normal = tilt_w.linear() * -Vec3d::UnitZ();
        in.a_hit = tilt_w * Vec3d(30, 30, 0);
        CHECK(!exact_roll_needs_axes(&state, in.a_normal, in.roll_hysteresis));
        const ExactFootprint fp = exact_footprint(in);
        CHECK(fp.roll_reused);
        CHECK(fp.roll == first.roll);
    }

    SECTION("a small facet of a curved part does not reset the roll of a big one") {
        const indexed_triangle_set ball = sphere(30);
        const std::vector<int> one_facet{ 1000 }; // about a square millimetre against the block's 200 mm2 face
        in.a_its = &ball;
        in.a_facets = &one_facet;
        const Transform3d turned_w = plate_w * Eigen::AngleAxisd(40.0 * M_PI / 180.0, Vec3d::UnitY());
        in.a_w = turned_w;
        in.a_normal = turned_w.linear() * -Vec3d::UnitZ();
        in.a_axis = turned_w.linear() * Vec3d(0.3, 0.9, 0);
        in.a_hit = turned_w * Vec3d(30, 30, 0);
        const ExactFootprint fp = exact_footprint(in);
        REQUIRE(fp.ok);
        CHECK(fp.roll_reused);
        CHECK(fp.roll == first.roll);
    }
}

TEST_CASE("Exact footprint: hovering over a curved face only slides the footprint", "[Assembly][ExactFootprint]")
{
    // No axes at all (a sphere has none): the orientation follows the surface normal smoothly, so a small
    // cursor step turns the footprint by about the normal's own turn and no more.
    const indexed_triangle_set ball_t = sphere(20);
    const std::vector<int>     cap    = facets_facing(ball_t, Vec3d::UnitZ(), std::cos(15.0 * M_PI / 180));
    const indexed_triangle_set ball_a = sphere(30);
    const std::vector<int>     patch  = facets_facing(ball_a, Vec3d(0, 0, -1), std::cos(70.0 * M_PI / 180));
    const Transform3d ball_w = Transform3d(Eigen::Translation3d(100, 50, 60)) * Eigen::AngleAxisd(0.4, Vec3d(1, 1, 0).normalized());

    ExactFootprintInput in;
    in.t_its = &ball_t; in.t_facets = &cap; in.t_w = Transform3d::Identity();
    in.t_normal = Vec3d::UnitZ();
    in.a_its = &ball_a; in.a_facets = &patch; in.a_w = ball_w; in.a_curved = true;
    ExactRollState state;
    in.roll_state = &state;

    Matrix3d prev = Matrix3d::Identity();
    Vec3d prev_dir = Vec3d::Zero();
    for (int i = 0; i < 30; ++i) {
        const double th = M_PI - 0.3 + 0.01 * i; // walks along a meridian, 0.01 rad (0.3 mm) at a time
        const Vec3d d(std::sin(th) * 0.6, std::sin(th) * 0.8, std::cos(th));
        in.a_hit    = ball_w * (d * 30.0);
        in.a_normal = ball_w.linear() * d;
        const ExactFootprint fp = exact_footprint(in);
        REQUIRE(fp.ok);
        CHECK(fp.roll == 0.0);
        if (i > 0) {
            const double step = std::acos(std::clamp(d.dot(prev_dir), -1.0, 1.0));
            // Follows the normal's turn (the footprint's normal is re-estimated over the draped region, so allow
            // some tessellation jitter) -- never a swing.
            CHECK(rotation_angle(fp.mate.linear() * prev.transpose()) < 10.0 * step + 0.05);
        }
        prev = fp.mate.linear(); prev_dir = d;
    }
}
