#include <catch2/catch.hpp>

#include "slic3r/GUI/EmbossFreeTransform.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI::EmbossFreeTransform;

// Owner request 2026-10-07: the Text, SVG and Image tools get full 3D move and rotate handles.
// These are the rules that decide what a handle drag does to the surface projection, and the
// geometry the drags are built on.

namespace {

const Vec3d NZ = Vec3d::UnitZ();

Projection flat() { return {}; }
Projection on_surface() { Projection p; p.use_surface = true; return p; }
Projection per_glyph() { Projection p; p.per_glyph = true; return p; }
Projection curved_on_surface() { Projection p; p.use_surface = true; p.per_glyph = true; p.curved = true; return p; }

} // namespace

TEST_CASE("A move is along the surface or off it", "[EmbossFreeTransform]")
{
    CHECK(classify_move(Vec3d(0., 0., 0.), NZ) == Kind::None);
    CHECK(classify_move(Vec3d(1e-4, 0., 0.), NZ) == Kind::None);
    CHECK(classify_move(Vec3d(5., -2., 0.), NZ) == Kind::Tangent);
    CHECK(classify_move(Vec3d(0., 0., 1.5), NZ) == Kind::Normal);
    CHECK(classify_move(Vec3d(0., 0., -0.5), NZ) == Kind::Normal);
    // A world move with any noticeable part along the normal leaves the surface.
    CHECK(classify_move(Vec3d(3., 0., 0.2), NZ) == Kind::Normal);
    // The normal need not be unit length (scaled parts).
    CHECK(classify_move(Vec3d(4., 0., 0.), Vec3d(0., 0., 7.)) == Kind::Tangent);
}

TEST_CASE("A turn is in the part's plane or tilts it", "[EmbossFreeTransform]")
{
    CHECK(classify_rotation(NZ, NZ, 0.) == Kind::None);
    CHECK(classify_rotation(NZ, NZ, 0.3) == Kind::InPlane);
    CHECK(classify_rotation(-NZ, NZ, 0.3) == Kind::InPlane);
    CHECK(classify_rotation(Vec3d::UnitX(), NZ, 0.3) == Kind::Tilt);
    CHECK(classify_rotation(Vec3d::UnitY(), NZ, -1.0) == Kind::Tilt);
}

TEST_CASE("Flat parts move freely and keep a measured distance", "[EmbossFreeTransform]")
{
    for (Kind kind : {Kind::Tangent, Kind::Normal, Kind::InPlane, Kind::Tilt}) {
        const Outcome o = outcome(flat(), kind);
        CHECK_FALSE(o.detach);
        CHECK_FALSE(o.reprocess);
        CHECK(o.measure_distance);
    }
    const Outcome none = outcome(flat(), Kind::None);
    CHECK_FALSE(none.detach);
    CHECK_FALSE(none.reprocess);
    CHECK_FALSE(none.measure_distance);
}

TEST_CASE("A projection follows a move along the surface and a turn in its plane", "[EmbossFreeTransform]")
{
    for (const Projection &p : {on_surface(), per_glyph(), curved_on_surface()})
        for (Kind kind : {Kind::Tangent, Kind::InPlane}) {
            const Outcome o = outcome(p, kind);
            CHECK_FALSE(o.detach);
            CHECK(o.reprocess);
            CHECK_FALSE(o.measure_distance);
        }
}

TEST_CASE("Lifting, pushing or tilting a projected part frees it", "[EmbossFreeTransform]")
{
    for (const Projection &p : {on_surface(), per_glyph(), curved_on_surface()})
        for (Kind kind : {Kind::Normal, Kind::Tilt}) {
            const Outcome o = outcome(p, kind);
            CHECK(o.detach);
            CHECK(o.reprocess);
            CHECK(o.measure_distance);
        }

    const Projection d = detached(curved_on_surface());
    CHECK_FALSE(d.use_surface);
    CHECK_FALSE(d.per_glyph);
    CHECK(d.curved); // the curve stays, flat in the part's plane
}

TEST_CASE("The next surface drag puts the projection back", "[EmbossFreeTransform]")
{
    const Projection before = curved_on_surface();
    const Projection now    = detached(before);

    std::optional<Projection> back = reattach(now, before);
    REQUIRE(back.has_value());
    CHECK(*back == before);

    // Nothing remembered, or a part that was flat anyway: nothing to put back.
    CHECK_FALSE(reattach(now, std::nullopt).has_value());
    CHECK_FALSE(reattach(flat(), flat()).has_value());
    // The user (or undo) already turned a projection on again: theirs is kept.
    CHECK_FALSE(reattach(on_surface(), before).has_value());
}

TEST_CASE("Placement shown in the tool", "[EmbossFreeTransform]")
{
    const double max_d = 4.;
    CHECK(classify_placement(true, flat(), SurfaceProbe{0., 1.}, max_d) == Placement::Object);
    CHECK(classify_placement(false, on_surface(), std::nullopt, max_d) == Placement::Projected);
    CHECK(classify_placement(false, per_glyph(), SurfaceProbe{2., 0.5}, max_d) == Placement::Projected);
    CHECK(classify_placement(false, flat(), std::nullopt, max_d) == Placement::Free);
    CHECK(classify_placement(false, flat(), SurfaceProbe{9., 1.}, max_d) == Placement::Free);
    CHECK(classify_placement(false, flat(), SurfaceProbe{0.001, 1.}, max_d) == Placement::OnSurface);
    CHECK(classify_placement(false, flat(), SurfaceProbe{1.5, 0.99999}, max_d) == Placement::Floating);
    CHECK(classify_placement(false, flat(), SurfaceProbe{-0.8, -1.}, max_d) == Placement::Floating);
    CHECK(classify_placement(false, flat(), SurfaceProbe{0., 0.9}, max_d) == Placement::Tilted);
}

TEST_CASE("A world motion of the part becomes its volume transformation", "[EmbossFreeTransform]")
{
    // Instance turned 90 degrees around Z, scaled and moved; the part sits on its side.
    Transform3d instance = Transform3d::Identity();
    instance.translate(Vec3d(100., 50., 0.));
    instance.rotate(Eigen::AngleAxisd(0.5 * PI, Vec3d::UnitZ()));
    instance.scale(Vec3d(2., 2., 2.));
    Transform3d volume = Transform3d::Identity();
    volume.translate(Vec3d(10., 0., 5.));
    volume.rotate(Eigen::AngleAxisd(0.5 * PI, Vec3d::UnitX()));

    // 3MF fix (a translation right of the volume transformation) must not matter.
    Transform3d fix = Transform3d::Identity();
    fix.translate(Vec3d(1., 2., 3.));

    SECTION("move") {
        const Transform3d delta(Eigen::Translation3d(0., 0., 3.));
        const Transform3d v1 = moved_volume_matrix(instance, volume * fix, delta);
        const Transform3d world0 = instance * volume * fix;
        const Transform3d world1 = instance * v1;
        CHECK((world1.matrix() - (delta * world0).matrix()).norm() < 1e-9);
        // The text frame (without the fix) moved the same way.
        const Transform3d text1 = instance * v1 * fix.inverse();
        CHECK((text1.translation() - ((instance * volume).translation() + Vec3d(0., 0., 3.))).norm() < 1e-9);
    }

    SECTION("turn about a pivot") {
        const Vec3d       pivot = instance * volume * Vec3d(0., 0., 0.);
        const Transform3d delta = rotation_about(pivot, Vec3d(0., 1., 0.), 0.25 * PI);
        const Transform3d v1    = moved_volume_matrix(instance, volume, delta);
        // The pivot stays where it was, and the turn is rigid in the world.
        CHECK(((instance * v1) * Vec3d::Zero() - pivot).norm() < 1e-9);
        const Matrix3d lin0 = (instance * volume).linear();
        const Matrix3d lin1 = (instance * v1).linear();
        for (int i = 0; i < 3; ++i)
            CHECK(std::abs(lin0.col(i).norm() - lin1.col(i).norm()) < 1e-9);
    }

    SECTION("other instances share the volume transformation") {
        Transform3d other = Transform3d::Identity();
        other.translate(Vec3d(-40., 0., 0.));
        const Transform3d delta(Eigen::Translation3d(0., 0., 3.));
        const Transform3d v1 = moved_volume_matrix(instance, volume, delta);
        // The other instance sees the same change of the part relative to its object.
        const Vec3d local_shift = (v1 * Vec3d::Zero()) - (volume * Vec3d::Zero());
        CHECK((((other * v1) * Vec3d::Zero()) - ((other * volume) * Vec3d::Zero()) - other.linear() * local_shift).norm() < 1e-9);
        // In the edited instance that shift is the world move.
        CHECK((instance.linear() * local_shift - Vec3d(0., 0., 3.)).norm() < 1e-9);
    }
}

TEST_CASE("Arrow drag distance follows the mouse along the axis", "[EmbossFreeTransform]")
{
    const Vec3d start(0., 0., 10.);
    const Vec3d axis = Vec3d::UnitZ();
    // Camera looking along -Y; the mouse ray passes 4 mm above the start.
    const Vec3d dir(0., 1., 0.);
    CHECK(axis_drag_distance(start, axis, Vec3d(0., -100., 14.), dir) == Approx(4.));
    CHECK(axis_drag_distance(start, axis, Vec3d(3., -100., 7.), dir) == Approx(-3.));
    CHECK(axis_drag_distance(start, axis, Vec3d(0., -100., 14.4), dir, 1.) == Approx(4.));
}

TEST_CASE("Ring angle differences wrap", "[EmbossFreeTransform]")
{
    CHECK(angle_delta(0.5 * PI, 0.75 * PI) == Approx(0.25 * PI));
    CHECK(angle_delta(1.5 * PI, 0.1) == Approx(0.5 * PI + 0.1));
    CHECK(angle_delta(0.1, 1.9 * PI) == Approx(-0.1 * PI - 0.1));
    CHECK(angle_delta(1., 1.) == Approx(0.));
}

TEST_CASE("Arrows pointing at the camera are hidden", "[EmbossFreeTransform]")
{
    const Vec3d view(0., 0., -1.);
    CHECK_FALSE(arrow_visible(Vec3d::UnitZ(), view));
    CHECK_FALSE(arrow_visible(Vec3d(0.1, 0., 1.), view));
    CHECK(arrow_visible(Vec3d::UnitX(), view));
    CHECK(arrow_visible(Vec3d(0., 1., 1.), view));
}
