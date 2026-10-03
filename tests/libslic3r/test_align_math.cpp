#include <catch2/catch.hpp>

#include "libslic3r/AlignMath.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::AlignMath;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double TOL = 1e-9;
constexpr double PI_CONST = 3.14159265358979323846;

// Small deterministic generator so the parity tests need no <random> seeding concerns.
struct Lcg
{
    uint32_t state = 12345u;
    double   next(double lo, double hi)
    {
        state = state * 1664525u + 1013904223u;
        return lo + (hi - lo) * (double(state >> 8) / double(1u << 24));
    }
};

std::vector<Span> random_spans(Lcg &rng, size_t n)
{
    std::vector<Span> spans;
    for (size_t i = 0; i < n; ++i) {
        const double lo = rng.next(-120., 220.);
        spans.push_back({lo, lo + rng.next(0.5, 90.)});
    }
    return spans;
}

// The pre-existing maths, copied verbatim in spirit: min of mins / max of maxes / union centre for
// the inter-item mode, and a shrunk-by-0.1 reference box for the plate mode.
std::vector<double> old_inter_item(const std::vector<Span> &spans, Side side)
{
    double reference = 0.;
    if (side == Side::Min) {
        reference = spans.front().lo;
        for (const Span &s : spans) reference = std::min(reference, s.lo);
    } else if (side == Side::Max) {
        reference = spans.front().hi;
        for (const Span &s : spans) reference = std::max(reference, s.hi);
    } else {
        double lo = spans.front().lo, hi = spans.front().hi;
        for (const Span &s : spans) { lo = std::min(lo, s.lo); hi = std::max(hi, s.hi); }
        reference = 0.5 * (lo + hi);
    }
    std::vector<double> out;
    for (const Span &s : spans) {
        const double current = side == Side::Min ? s.lo : side == Side::Max ? s.hi : 0.5 * (s.lo + s.hi);
        const double d       = reference - current;
        out.push_back(std::abs(d) < 1e-6 ? 0. : d);
    }
    return out;
}

std::vector<double> old_plate(const std::vector<Span> &spans, Side side, const Span &plate, double shrink)
{
    const Span ref{plate.lo + shrink, plate.hi - shrink};
    double lo = spans.front().lo, hi = spans.front().hi;
    for (const Span &s : spans) { lo = std::min(lo, s.lo); hi = std::max(hi, s.hi); }
    const double current = side == Side::Min ? lo : side == Side::Max ? hi : 0.5 * (lo + hi);
    const double target  = side == Side::Min ? ref.lo : side == Side::Max ? ref.hi : ref.center();
    const double d       = target - current;
    return std::vector<double>(spans.size(), std::abs(d) < 1e-6 ? 0. : d);
}

// Applies the offsets and returns the moved spans.
std::vector<Span> moved(const std::vector<Span> &spans, const std::vector<double> &offsets)
{
    std::vector<Span> out = spans;
    for (size_t i = 0; i < out.size(); ++i) { out[i].lo += offsets[i]; out[i].hi += offsets[i]; }
    return out;
}

AxisRequest plate_request(Side button, Origin origin, const Span &plate, double inset)
{
    AxisRequest r;
    r.button     = button;
    r.origin     = origin;
    r.reference  = Reference::Fixed;
    r.fixed      = plate;
    r.edge_inset = inset;
    return r;
}

AxisRequest anchor_request(Side button, Origin origin, size_t anchor)
{
    AxisRequest r;
    r.button    = button;
    r.origin    = origin;
    r.reference = Reference::Anchor;
    r.anchor    = anchor;
    return r;
}

const Side ALL_SIDES[3] = {Side::Min, Side::Center, Side::Max};

// World extent on one axis of a Z-rotated cube of the given side, centred on (cx, cy).
BoundingBoxf3 rotated_cube_aabb(double side, double angle_rad, double cx, double cy, double z0)
{
    Transform3d t = Transform3d::Identity();
    t.translate(Vec3d(cx, cy, 0.));
    t.rotate(Eigen::AngleAxisd(angle_rad, Vec3d::UnitZ()));
    BoundingBoxf3 bb;
    for (int i = 0; i < 8; ++i) {
        const Vec3d corner((i & 1 ? 0.5 : -0.5) * side, (i & 2 ? 0.5 : -0.5) * side, (i & 4 ? side : 0.) + z0);
        bb.merge(t * corner);
    }
    return bb;
}

} // namespace

TEST_CASE("AlignMath Auto reproduces the old inter-item maths on all nine buttons", "[AlignMath]")
{
    Lcg rng;
    for (int trial = 0; trial < 40; ++trial) {
        // Three independent axes of the same set of 3D boxes: X buttons, Y buttons, Z buttons.
        const size_t                  n = 2 + trial % 5;
        const std::vector<Span>       axes[3] = {random_spans(rng, n), random_spans(rng, n), random_spans(rng, n)};
        for (int axis = 0; axis < 3; ++axis) {
            for (Side side : ALL_SIDES) {
                AxisRequest req;
                req.button = side;
                // Auto in Union mode, and Auto in Anchor-less fallback, are the old behaviour.
                const std::vector<double> got      = axis_offsets(axes[axis], req);
                const std::vector<double> expected = old_inter_item(axes[axis], side);
                REQUIRE(got.size() == expected.size());
                for (size_t i = 0; i < got.size(); ++i) {
                    INFO("trial " << trial << " axis " << axis << " side " << int(side) << " item " << i);
                    REQUIRE_THAT(got[i], WithinAbs(expected[i], TOL));
                }
            }
        }
    }
}

TEST_CASE("AlignMath Auto reproduces the old plate maths on all nine buttons", "[AlignMath]")
{
    Lcg          rng;
    const Span   plate_xy{0., 256.};
    const Span   plate_z{0., 270.};
    for (int trial = 0; trial < 40; ++trial) {
        const size_t n = 1 + trial % 4;
        for (int axis = 0; axis < 3; ++axis) {
            const std::vector<Span> spans = random_spans(rng, n);
            for (Side side : ALL_SIDES) {
                // X and Y: the plate shrunk 0.1 on both sides. Z: the panel builds the reference
                // box so that Z needs no inset (see GizmoObjectManipulation), so inset 0 there.
                const double inset = axis == 2 ? 0. : 0.1;
                const Span   plate = axis == 2 ? plate_z : plate_xy;
                const std::vector<double> got      = axis_offsets(spans, plate_request(side, Origin::Auto, plate, inset));
                const std::vector<double> expected = old_plate(spans, side, plate, inset);
                for (size_t i = 0; i < got.size(); ++i) {
                    INFO("trial " << trial << " axis " << axis << " side " << int(side));
                    REQUIRE_THAT(got[i], WithinAbs(expected[i], TOL));
                }
            }
        }
    }
}

TEST_CASE("AlignMath plate mode, every origin times every button", "[AlignMath]")
{
    const Span plate{0., 200.};
    // A selection group spanning 30..70 (two items, so the group is rigid): 30..40 and 60..70.
    const std::vector<Span> items = {{30., 40.}, {60., 70.}};
    const Span              group{30., 70.};
    const double            inset = 0.1;

    const Origin origins[4] = {Origin::Auto, Origin::Center, Origin::Min, Origin::Max};
    for (Origin origin : origins) {
        for (Side button : ALL_SIDES) {
            DYNAMIC_SECTION("origin " << origin_key(origin) << " button " << int(button))
            {
                const std::vector<double> d = axis_offsets(items, plate_request(button, origin, plate, inset));
                REQUIRE(d.size() == 2);
                // Rigid group: one shared displacement, spacing preserved.
                REQUIRE_THAT(d[0], WithinAbs(d[1], TOL));
                const std::vector<Span> after = moved(items, d);
                REQUIRE_THAT(after[1].lo - after[0].lo, WithinAbs(30., TOL));

                const Side   pick       = pick_side(origin, button);
                const double target_raw = plate.at(button);
                // Inset only for an edge going to the same-side plate edge.
                double       target = target_raw;
                if (button != Side::Center && pick == button)
                    target += button == Side::Min ? inset : -inset;
                const Span after_group = union_of(after);
                REQUIRE_THAT(after_group.at(pick), WithinAbs(target, TOL));
                // The group extent itself is untouched.
                REQUIRE_THAT(after_group.hi - after_group.lo, WithinAbs(group.hi - group.lo, TOL));
            }
        }
    }
}

TEST_CASE("AlignMath plate mode inset is only applied to same-side edges", "[AlignMath]")
{
    const Span plate{0., 200.};
    const std::vector<Span> one = {{50., 60.}};

    // Right face to the right plate edge: stays inside, shrunk by 0.1.
    REQUIRE_THAT(moved(one, axis_offsets(one, plate_request(Side::Max, Origin::Max, plate, 0.1)))[0].hi, WithinAbs(199.9, TOL));
    REQUIRE_THAT(moved(one, axis_offsets(one, plate_request(Side::Max, Origin::Auto, plate, 0.1)))[0].hi, WithinAbs(199.9, TOL));
    // Left face to the left plate edge: shrunk by 0.1.
    REQUIRE_THAT(moved(one, axis_offsets(one, plate_request(Side::Min, Origin::Min, plate, 0.1)))[0].lo, WithinAbs(0.1, TOL));
    // Left face to the RIGHT plate edge: the item sits outside the plate, touching exactly.
    REQUIRE_THAT(moved(one, axis_offsets(one, plate_request(Side::Max, Origin::Min, plate, 0.1)))[0].lo, WithinAbs(200., TOL));
    // Right face to the LEFT plate edge: exact.
    REQUIRE_THAT(moved(one, axis_offsets(one, plate_request(Side::Min, Origin::Max, plate, 0.1)))[0].hi, WithinAbs(0., TOL));
    // Centre of the item onto a plate edge: exact.
    REQUIRE_THAT(moved(one, axis_offsets(one, plate_request(Side::Max, Origin::Center, plate, 0.1)))[0].center(), WithinAbs(200., TOL));
    // Anything onto the plate centre line: exact.
    REQUIRE_THAT(moved(one, axis_offsets(one, plate_request(Side::Center, Origin::Max, plate, 0.1)))[0].hi, WithinAbs(100., TOL));
    REQUIRE_THAT(moved(one, axis_offsets(one, plate_request(Side::Center, Origin::Auto, plate, 0.1)))[0].center(), WithinAbs(100., TOL));
}

TEST_CASE("AlignMath two cubes meet on the plate centre line without knowing each other", "[AlignMath]")
{
    const Span plate{0., 256.};
    const double centre = 128.;

    // Cube A, 10 mm wide, at x 20..30. Origin Right, click X Center: its right face on the centre.
    std::vector<Span> a = {{20., 30.}};
    a = moved(a, axis_offsets(a, plate_request(Side::Center, Origin::Max, plate, 0.1)));
    REQUIRE_THAT(a[0].hi, WithinAbs(centre, TOL));
    REQUIRE_THAT(a[0].lo, WithinAbs(centre - 10., TOL));

    // Cube B, 10 mm wide, at x 200..210. Origin Left, click X Center: its left face on the centre.
    std::vector<Span> b = {{200., 210.}};
    b = moved(b, axis_offsets(b, plate_request(Side::Center, Origin::Min, plate, 0.1)));
    REQUIRE_THAT(b[0].lo, WithinAbs(centre, TOL));
    REQUIRE_THAT(b[0].hi, WithinAbs(centre + 10., TOL));

    // The faces coincide exactly on the centre line.
    REQUIRE_THAT(a[0].hi, WithinAbs(b[0].lo, TOL));

    // A cube of a different width lands the same way: right face of a 25 mm cube on the line.
    std::vector<Span> c = {{3., 28.}};
    c = moved(c, axis_offsets(c, plate_request(Side::Center, Origin::Max, plate, 0.1)));
    REQUIRE_THAT(c[0].hi, WithinAbs(centre, TOL));
}

TEST_CASE("AlignMath anchor touch", "[AlignMath]")
{
    // A x 0..10, B x 40..50; B is the anchor (last selected).
    const std::vector<Span> items = {{0., 10.}, {40., 50.}};

    SECTION("A's right face to B's left face: origin Right, button Left")
    {
        const std::vector<double> d = axis_offsets(items, anchor_request(Side::Min, Origin::Max, 1));
        REQUIRE_THAT(d[0], WithinAbs(30., TOL));
        REQUIRE(d[1] == 0.); // the anchor stays put, exactly
        const std::vector<Span> after = moved(items, d);
        REQUIRE_THAT(after[0].hi, WithinAbs(after[1].lo, TOL));
        REQUIRE_THAT(after[0].lo, WithinAbs(30., TOL));
    }
    SECTION("A's left face to B's right face: origin Left, button Right")
    {
        const std::vector<double> d = axis_offsets(items, anchor_request(Side::Max, Origin::Min, 1));
        REQUIRE_THAT(d[0], WithinAbs(50., TOL));
        REQUIRE(d[1] == 0.);
        const std::vector<Span> after = moved(items, d);
        REQUIRE_THAT(after[0].lo, WithinAbs(after[1].hi, TOL));
    }
    SECTION("centre onto the anchor's centre")
    {
        const std::vector<double> d = axis_offsets(items, anchor_request(Side::Center, Origin::Center, 1));
        REQUIRE_THAT(d[0], WithinAbs(40., TOL));
        REQUIRE(d[1] == 0.);
    }
    SECTION("centre onto the anchor's left edge")
    {
        const std::vector<double> d = axis_offsets(items, anchor_request(Side::Min, Origin::Center, 1));
        REQUIRE_THAT(moved(items, d)[0].center(), WithinAbs(40., TOL));
    }
    SECTION("anchor is the first item: the other one moves instead")
    {
        const std::vector<double> d = axis_offsets(items, anchor_request(Side::Max, Origin::Min, 0));
        REQUIRE(d[0] == 0.);
        REQUIRE_THAT(moved(items, d)[1].lo, WithinAbs(10., TOL));
    }
    SECTION("several moved items, the anchor in the middle of the list")
    {
        const std::vector<Span>   three = {{0., 10.}, {100., 130.}, {300., 305.}};
        const std::vector<double> d     = axis_offsets(three, anchor_request(Side::Min, Origin::Max, 1));
        const std::vector<Span>   after = moved(three, d);
        REQUIRE_THAT(after[0].hi, WithinAbs(100., TOL));
        REQUIRE_THAT(after[2].hi, WithinAbs(100., TOL));
        REQUIRE(d[1] == 0.);
    }
    SECTION("a second run changes nothing")
    {
        const AxisRequest         req   = anchor_request(Side::Min, Origin::Max, 1);
        const std::vector<Span>   after = moved(items, axis_offsets(items, req));
        for (double d : axis_offsets(after, req))
            REQUIRE(d == 0.);
    }
}

TEST_CASE("AlignMath Z stacking of separate objects", "[AlignMath]")
{
    // P is 20 mm tall (z 0..20), Q is 10 mm tall (z 0..10), both on the bed. P is the anchor.
    const std::vector<Span> items = {{0., 10.} /*Q*/, {0., 20.} /*P*/};

    // Q's bottom to P's top: Q ends up at 20..30, resting on P. The maths does not clamp to the bed.
    const std::vector<double> d     = axis_offsets(items, anchor_request(Side::Max, Origin::Min, 1));
    const std::vector<Span>   after = moved(items, d);
    REQUIRE_THAT(after[0].lo, WithinAbs(20., TOL));
    REQUIRE_THAT(after[0].hi, WithinAbs(30., TOL));
    REQUIRE(d[1] == 0.);

    // Auto, Top button: tops aligned (the shorter object is lifted, which the panel used to undo).
    const std::vector<double> auto_d = axis_offsets(items, AxisRequest{Side::Max});
    REQUIRE_THAT(moved(items, auto_d)[0].hi, WithinAbs(20., TOL));

    // Hanging it underneath: Q's top to P's bottom puts Q at -10..0.
    const std::vector<double> under = axis_offsets(items, anchor_request(Side::Min, Origin::Max, 1));
    REQUIRE_THAT(moved(items, under)[0].hi, WithinAbs(0., TOL));
}

TEST_CASE("AlignMath rotated objects use the world AABB", "[AlignMath]")
{
    // A 10 mm cube turned 45 degrees about Z is 14.142 mm wide in X and Y.
    const double          angle = PI_CONST / 4.;
    const BoundingBoxf3   diamond = rotated_cube_aabb(10., angle, 0., 0., 0.);
    const BoundingBoxf3   plain   = rotated_cube_aabb(10., 0., 60., 0., 0.);
    REQUIRE_THAT(diamond.max.x() - diamond.min.x(), WithinAbs(10. * std::sqrt(2.), 1e-9));
    REQUIRE_THAT(diamond.max.z() - diamond.min.z(), WithinAbs(10., 1e-9));

    const std::vector<Span> xs = {{diamond.min.x(), diamond.max.x()}, {plain.min.x(), plain.max.x()}};

    SECTION("touching meets the bounding-box faces, not the cube faces")
    {
        // diamond is the moved item, plain the anchor: diamond's AABB right face to plain's left face.
        const std::vector<Span> after = moved(xs, axis_offsets(xs, anchor_request(Side::Min, Origin::Max, 1)));
        REQUIRE_THAT(after[0].hi, WithinAbs(after[1].lo, TOL));
        REQUIRE_THAT(after[0].hi, WithinAbs(55., TOL)); // plain spans 55..65
    }
    SECTION("Auto left aligns the AABB minima")
    {
        const std::vector<Span> after = moved(xs, axis_offsets(xs, AxisRequest{Side::Min}));
        REQUIRE_THAT(after[0].lo, WithinAbs(after[1].lo, TOL));
        REQUIRE_THAT(after[0].lo, WithinAbs(diamond.min.x(), TOL));
    }
    SECTION("plate centre line with the diamond's right AABB face")
    {
        const std::vector<Span> only = {xs[0]};
        const std::vector<Span> after = moved(only, axis_offsets(only, plate_request(Side::Center, Origin::Max, Span{0., 100.}, 0.1)));
        REQUIRE_THAT(after[0].hi, WithinAbs(50., TOL));
    }
    SECTION("Z of the rotated cube is unchanged by a Z-axis rotation")
    {
        const std::vector<Span> zs = {{diamond.min.z(), diamond.max.z()}, {0., 10.}};
        for (double d : axis_offsets(zs, AxisRequest{Side::Min}))
            REQUIRE(d == 0.);
    }
}

TEST_CASE("AlignMath degenerate input", "[AlignMath]")
{
    SECTION("empty list")
    {
        REQUIRE(axis_offsets({}, AxisRequest{}).empty());
    }
    SECTION("a single item in Union mode never moves, in any mode")
    {
        const std::vector<Span> one = {{5., 15.}};
        for (Side s : ALL_SIDES)
            REQUIRE(axis_offsets(one, AxisRequest{s})[0] == 0.);
    }
    SECTION("anchor index out of range falls back to the union")
    {
        const std::vector<Span> items = {{0., 10.}, {40., 50.}};
        AxisRequest             req   = anchor_request(Side::Min, Origin::Auto, 7);
        const std::vector<double> d   = axis_offsets(items, req);
        REQUIRE_THAT(d[0], WithinAbs(0., TOL));
        REQUIRE_THAT(d[1], WithinAbs(-40., TOL));
    }
    SECTION("zero-width spans and negative coordinates")
    {
        const std::vector<Span> items = {{-30., -30.}, {-10., -4.}};
        const std::vector<double> d   = axis_offsets(items, anchor_request(Side::Center, Origin::Center, 1));
        REQUIRE_THAT(d[0], WithinAbs(23., TOL));
        REQUIRE(d[1] == 0.);
    }
    SECTION("offsets smaller than the epsilon are exactly zero")
    {
        const std::vector<Span> items = {{0., 10.}, {1e-8, 10. + 1e-8}};
        for (double d : axis_offsets(items, AxisRequest{Side::Min}))
            REQUIRE(d == 0.);
    }
    SECTION("Auto picks the same side as the button")
    {
        REQUIRE(pick_side(Origin::Auto, Side::Min) == Side::Min);
        REQUIRE(pick_side(Origin::Auto, Side::Center) == Side::Center);
        REQUIRE(pick_side(Origin::Auto, Side::Max) == Side::Max);
        REQUIRE(pick_side(Origin::Center, Side::Max) == Side::Center);
        REQUIRE(pick_side(Origin::Min, Side::Max) == Side::Min);
        REQUIRE(pick_side(Origin::Max, Side::Min) == Side::Max);
    }
}

TEST_CASE("AlignMath origin config keys round-trip", "[AlignMath]")
{
    const Origin all[4] = {Origin::Auto, Origin::Center, Origin::Min, Origin::Max};
    for (Origin o : all)
        REQUIRE(origin_from_key(origin_key(o)) == o);
    REQUIRE(origin_from_key("") == Origin::Auto);
    REQUIRE(origin_from_key("nonsense") == Origin::Auto);
    REQUIRE(int(Origin::Auto) == 0);
    REQUIRE(int(Origin::Center) == 1);
    REQUIRE(int(Origin::Min) == 2);
    REQUIRE(int(Origin::Max) == 3);
}

TEST_CASE("AlignMath origin dropdown spans exactly its button group", "[AlignMath]")
{
    // Window-local item edges of three groups of 3 buttons (37 px wide), 16 px apart, at any scale.
    for (double scale : {1.0, 1.5, 2.0}) {
        const double button = 37. * scale, gap = 16. * scale, start = 120. * scale;
        double       left[3], right[3];
        for (int g = 0; g < 3; ++g) {
            left[g]  = start + g * (3. * button + gap);
            right[g] = left[g] + 3. * button;
            REQUIRE_THAT(combo_frame_width(left[g], right[g]), WithinAbs(3. * button, TOL));
        }
        // The dropdown row never reaches past the last group's right edge.
        REQUIRE_THAT(left[2] + combo_frame_width(left[2], right[2]), WithinAbs(right[2], TOL));
        // Groups with a fourth (Distribute) button are simply wider.
        REQUIRE_THAT(combo_frame_width(left[0], right[0] + button), WithinAbs(4. * button, TOL));
    }
    REQUIRE(combo_frame_width(50., 40.) == 0.); // degenerate input never goes negative
}

TEST_CASE("AlignMath ellipsize", "[AlignMath]")
{
    const auto mono = [](const std::string &s) {
        double n = 0;
        for (unsigned char c : s)
            if ((c & 0xC0) != 0x80) n += 10.; // 10 px per code point
        return n;
    };
    REQUIRE(ellipsize("Bottom", 60., mono) == "Bottom");  // fits exactly
    REQUIRE(ellipsize("Bottom", 59., mono) == "Bo...");   // 5 code points = 50 px
    REQUIRE(ellipsize("Bottom", 30., mono) == "...");
    REQUIRE(ellipsize("Bottom", 5., mono) == "...");      // even the dots do not fit: still just dots
    REQUIRE(ellipsize("", 0., mono) == "");
    // UTF-8: never cuts inside a code point.
    const std::string r = ellipsize("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 50., mono); // four e-acute, 40 px
    REQUIRE(r == "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9");
    const std::string c = ellipsize("éééééé", 45., mono); // six e-acute, 60 px
    REQUIRE(c == "\xC3\xA9...");
    REQUIRE(mono(c) <= 45.);
}

TEST_CASE("AlignMath anchor resolution", "[AlignMath]")
{
    SECTION("Last and First use the click order")
    {
        AnchorPick p = resolve_anchor(AnchorMode::Last, false, NO_ITEM, 2, 0);
        REQUIRE(p.index == 2);
        REQUIRE_FALSE(p.use_union);
        REQUIRE_FALSE(p.explicit_missing);
        p = resolve_anchor(AnchorMode::First, false, NO_ITEM, 2, 1);
        REQUIRE(p.index == 1);
    }
    SECTION("unknown click order falls back to the first item")
    {
        REQUIRE(resolve_anchor(AnchorMode::Last, false, NO_ITEM, NO_ITEM, NO_ITEM).index == 0);
        REQUIRE(resolve_anchor(AnchorMode::First, false, NO_ITEM, NO_ITEM, NO_ITEM).index == 0);
    }
    SECTION("None means the selection extremes, nothing fixed")
    {
        const AnchorPick p = resolve_anchor(AnchorMode::None, false, NO_ITEM, 2, 0);
        REQUIRE(p.use_union);
        REQUIRE_FALSE(p.explicit_missing);
    }
    SECTION("an explicit item wins over the mode while it is selected")
    {
        for (AnchorMode m : {AnchorMode::Last, AnchorMode::First, AnchorMode::None}) {
            const AnchorPick p = resolve_anchor(m, true, 1, 2, 0);
            REQUIRE(p.index == 1);
            REQUIRE_FALSE(p.use_union);
            REQUIRE_FALSE(p.explicit_missing);
        }
    }
    SECTION("an explicit item that left the selection falls back to Last, whatever the mode")
    {
        for (AnchorMode m : {AnchorMode::Last, AnchorMode::First, AnchorMode::None}) {
            const AnchorPick p = resolve_anchor(m, true, NO_ITEM, 2, 0);
            REQUIRE(p.explicit_missing);
            REQUIRE(p.index == 2);
            REQUIRE_FALSE(p.use_union);
        }
        // ... and with no known last item, to the first item
        REQUIRE(resolve_anchor(AnchorMode::First, true, NO_ITEM, NO_ITEM, NO_ITEM).index == 0);
    }
    SECTION("mode keys round-trip, unknown text means Last")
    {
        for (AnchorMode m : {AnchorMode::Last, AnchorMode::First, AnchorMode::None})
            REQUIRE(anchor_mode_from_key(anchor_mode_key(m)) == m);
        REQUIRE(anchor_mode_from_key("") == AnchorMode::Last);
        REQUIRE(anchor_mode_from_key("zzz") == AnchorMode::Last);
    }
    SECTION("the resolved anchor drives the maths: None aligns to the extremes, an item stays put")
    {
        const std::vector<Span> items = {{0., 10.}, {40., 50.}, {90., 95.}};
        AxisRequest             req;
        req.button = Side::Min;
        req.origin = Origin::Max;

        const AnchorPick none = resolve_anchor(AnchorMode::None, false, NO_ITEM, 2, 0);
        req.reference         = none.use_union ? Reference::Union : Reference::Anchor;
        // Every item's right face to the selection's left extreme (0).
        const std::vector<Span> after_none = moved(items, axis_offsets(items, req));
        for (const Span &s : after_none)
            REQUIRE_THAT(s.hi, WithinAbs(0., TOL));

        const AnchorPick first = resolve_anchor(AnchorMode::First, false, NO_ITEM, 2, 1);
        req.reference          = Reference::Anchor;
        req.anchor             = first.index;
        const std::vector<double> d = axis_offsets(items, req);
        REQUIRE(d[1] == 0.); // item 1 (the "first selected") stays
        REQUIRE_THAT(moved(items, d)[0].hi, WithinAbs(40., TOL));
        REQUIRE_THAT(moved(items, d)[2].hi, WithinAbs(40., TOL));
    }
}

TEST_CASE("AlignMath anchor names with repeats", "[AlignMath]")
{
    SECTION("unique names are untouched")
    {
        const auto out = disambiguate_names({"Cube", "Sphere"}, {1, 1});
        REQUIRE(out[0] == "Cube");
        REQUIRE(out[1] == "Sphere");
    }
    SECTION("repeated names get the instance or part number")
    {
        const auto out = disambiguate_names({"Cube", "Cube", "Sphere", "Cube"}, {1, 2, 1, 4});
        REQUIRE(out[0] == "Cube #1");
        REQUIRE(out[1] == "Cube #2");
        REQUIRE(out[2] == "Sphere");
        REQUIRE(out[3] == "Cube #4");
    }
    SECTION("empty list and missing numbers do not crash")
    {
        REQUIRE(disambiguate_names({}, {}).empty());
        const auto out = disambiguate_names({"A", "A"}, {});
        REQUIRE(out[0] == "A");
        REQUIRE(out[1] == "A");
    }
}
