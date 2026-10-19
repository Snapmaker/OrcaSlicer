// Snapmaker Orca: the parts of the scene cache that decide whether a captured frame may be
// replayed, and the texture coordinate its blit derives from a quad vertex. SceneCache.hpp pulls
// in nothing but libslic3r/Point.hpp, so both run without a window and without an OpenGL context.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cfloat>
#include <vector>

#include "libslic3r/Point.hpp"
#include "slic3r/GUI/SceneCache.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

namespace {

SceneCache::Key base_key()
{
    SceneCache::Key key;
    key.size = {{1280, 720}};
    key.view_matrix = Transform3d::Identity();
    key.view_matrix.translate(Vec3d(0.0, 0.0, -250.0));
    key.projection_matrix = Transform3d::Identity();
    key.projection_matrix.scale(Vec3d(0.5, 0.5, 0.001));
    key.sinking_hover_volume_idxs = {2, 5};
    key.forced_sinking_volume_idxs = {7};
    key.hover_plate_icon_idxs = {1};
    key.gizmo_hover_id = 3;
    key.render_preview = true;
    key.lod_allowed = true;
    key.lod_pixel_scale = 2.f;
    key.lod_pin_above_z = 180.0;
    return key;
}

} // namespace

TEST_CASE("A forced sinking contour is part of the scene cache key", "[SceneCache]")
{
    SceneCache::Key a = base_key();
    SceneCache::Key b = base_key();
    REQUIRE(a == b);

    // A gizmo move sets force_sinking_contours on the moved volumes; the next mouse event clears it
    // again. Neither transition schedules a frame, so a key that ignored the flag would replay the
    // capture of the other state.
    b.forced_sinking_volume_idxs = {7, 8};
    CHECK_FALSE(a == b);

    b.forced_sinking_volume_idxs.clear();
    CHECK_FALSE(a == b);

    b.forced_sinking_volume_idxs = {8};
    CHECK_FALSE(a == b);

    // The order the volume loop of GLCanvas3D::_scene_cache_key() produces is the volume order, so
    // the same set in another order is a different scene state, not the same one.
    a.forced_sinking_volume_idxs = {7, 8};
    b.forced_sinking_volume_idxs = {8, 7};
    CHECK_FALSE(a == b);
}

TEST_CASE("Every other input of the scene cache key separates two keys", "[SceneCache]")
{
    const SceneCache::Key a = base_key();

    SECTION("viewport size") {
        SceneCache::Key b = base_key();
        b.size = {{1280, 721}};
        CHECK_FALSE(a == b);
    }
    SECTION("camera view matrix") {
        SceneCache::Key b = base_key();
        b.view_matrix.translate(Vec3d(0.0, 0.0, -0.5));
        CHECK_FALSE(a == b);
    }
    SECTION("camera projection matrix") {
        SceneCache::Key b = base_key();
        b.projection_matrix.scale(Vec3d(1.1, 1.1, 1.0));
        CHECK_FALSE(a == b);
    }
    SECTION("hovered sinking volumes") {
        SceneCache::Key b = base_key();
        b.sinking_hover_volume_idxs = {2};
        CHECK_FALSE(a == b);
    }
    SECTION("hovered plate icons") {
        SceneCache::Key b = base_key();
        b.hover_plate_icon_idxs.clear();
        CHECK_FALSE(a == b);
    }
    SECTION("hovered gizmo grabber") {
        SceneCache::Key b = base_key();
        b.gizmo_hover_id = -1;
        CHECK_FALSE(a == b);
    }
    SECTION("preview toggle") {
        SceneCache::Key b = base_key();
        b.render_preview = false;
        CHECK_FALSE(a == b);
    }
    SECTION("a default key equals another default key") {
        CHECK(SceneCache::Key() == SceneCache::Key());
    }
}

TEST_CASE("The render LOD rule is part of the scene cache key", "[SceneCache]")
{
    // A replay skips GLVolumeCollection::update_lod(), whose non-camera arguments (gizmo LOD ban,
    // display scale, printable height) can change without dirtying the scene, so each is in the key.
    const SceneCache::Key a = base_key();

    SECTION("a gizmo or a clipping plane pins the scene to full detail") {
        SceneCache::Key b = base_key();
        b.lod_allowed = false;
        CHECK_FALSE(a == b);
    }
    SECTION("the display scale the pixel thresholds follow") {
        SceneCache::Key b = base_key();
        b.lod_pixel_scale = 1.f;
        CHECK_FALSE(a == b);
    }
    SECTION("the height above which volumes stay at full detail") {
        SceneCache::Key b = base_key();
        b.lod_pin_above_z = 120.0;
        CHECK_FALSE(a == b);
    }
    SECTION("no pin at all is a state of its own") {
        SceneCache::Key b = base_key();
        b.lod_pin_above_z = DBL_MAX;
        CHECK_FALSE(a == b);

        SceneCache::Key c = base_key();
        c.lod_pin_above_z = DBL_MAX;
        CHECK(b == c);
    }
}

TEST_CASE("The scene cache blit maps the quad corners to the corners of the texture", "[SceneCache]")
{
    // flat_texture builds UVs as u_uvTransformMatrix * (x, -y, 1) of the NDC full-screen quad; the
    // texture comes from the framebuffer (origin bottom left). The corners pin scale and vertical flip.
    const Matrix3f uv = SceneCache::blit_uv_matrix();

    const std::array<Vec2f, 4> corners  = {{{-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f}, {-1.f, 1.f}}};
    const std::array<Vec2f, 4> expected = {{{ 0.f,  0.f}, {1.f,  0.f}, {1.f, 1.f}, { 0.f, 1.f}}};

    for (size_t i = 0; i < corners.size(); ++i) {
        const Vec3f tex_coord = uv * Vec3f(corners[i].x(), -corners[i].y(), 1.f);
        CHECK_THAT(tex_coord.x(), WithinAbs(expected[i].x(), 1e-6f));
        CHECK_THAT(tex_coord.y(), WithinAbs(expected[i].y(), 1e-6f));
        CHECK_THAT(tex_coord.z(), WithinAbs(1.f, 1e-6f));
    }

    // The centre of the quad samples the centre of the texture; no half texel drift.
    const Vec3f centre = uv * Vec3f(0.f, 0.f, 1.f);
    CHECK_THAT(centre.x(), WithinAbs(0.5f, 1e-6f));
    CHECK_THAT(centre.y(), WithinAbs(0.5f, 1e-6f));
}
