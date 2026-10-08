#include <catch2/catch.hpp>

#include <vector>

#include "slic3r/GUI/EmbossPicking.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI::EmbossPicking;

// Owner report 2026-10-07: text and SVG parts were hard to grab; the click kept landing on the
// object behind them unless it hit a glyph stroke exactly. These are the two rules of the fix.

namespace {

VolumeHit part(double depth, int object_id = 0, int instance_id = 0) { return { depth, object_id, instance_id, -1. }; }
VolumeHit text(double depth, double tolerance, int object_id = 0, int instance_id = 0)
{
    return { depth, object_id, instance_id, tolerance };
}

} // namespace

TEST_CASE("Text sunk a little into its object is picked instead of the object", "[EmbossPicking]")
{
    // Object surface at 100 mm, text front face 0.4 mm behind it (sunk), text 1 mm deep.
    const std::vector<VolumeHit> hits = { part(100.0), text(100.4, 1.0) };
    REQUIRE(prefer_text_or_svg(hits, 0) == 1);
}

TEST_CASE("Text hidden deeper than its tolerance stays hidden", "[EmbossPicking]")
{
    const std::vector<VolumeHit> hits = { part(100.0), text(102.5, 1.0) };
    REQUIRE(prefer_text_or_svg(hits, 0) == 0);
}

TEST_CASE("Text in front already wins and is kept", "[EmbossPicking]")
{
    const std::vector<VolumeHit> hits = { part(100.0), text(99.0, 1.0) };
    REQUIRE(prefer_text_or_svg(hits, 1) == 1);
}

TEST_CASE("Text of another object or instance is never preferred", "[EmbossPicking]")
{
    SECTION("other object") {
        const std::vector<VolumeHit> hits = { part(100.0, 0, 0), text(100.2, 1.0, 1, 0) };
        REQUIRE(prefer_text_or_svg(hits, 0) == 0);
    }
    SECTION("other instance of the same object") {
        const std::vector<VolumeHit> hits = { part(100.0, 0, 0), text(100.2, 1.0, 0, 1) };
        REQUIRE(prefer_text_or_svg(hits, 0) == 0);
    }
    SECTION("closest hit is not a model volume (wipe tower)") {
        const std::vector<VolumeHit> hits = { part(100.0, -1, -1), text(100.2, 1.0, -1, -1) };
        REQUIRE(prefer_text_or_svg(hits, 0) == 0);
    }
}

TEST_CASE("Of several qualifying text parts the nearest wins", "[EmbossPicking]")
{
    const std::vector<VolumeHit> hits = { text(100.8, 1.0), part(100.0), text(100.3, 1.0), part(100.1) };
    REQUIRE(prefer_text_or_svg(hits, 1) == 2);
}

TEST_CASE("Ordinary parts, modifiers and negative volumes keep plain closest-hit picking", "[EmbossPicking]")
{
    // A second (non text) part right behind the closest one is not preferred.
    const std::vector<VolumeHit> hits = { part(100.0), part(100.05) };
    REQUIRE(prefer_text_or_svg(hits, 0) == 0);
    // Out-of-range closest index is returned unchanged.
    REQUIRE(prefer_text_or_svg(hits, 7) == 7);
}

TEST_CASE("Text tolerance follows the emboss depth within bounds", "[EmbossPicking]")
{
    REQUIRE(prefer_tolerance_from_depth(1.5) == Approx(1.5));
    REQUIRE(prefer_tolerance_from_depth(0.0) == Approx(MIN_PREFER_TOLERANCE));
    REQUIRE(prefer_tolerance_from_depth(-0.5) == Approx(0.5));
    REQUIRE(prefer_tolerance_from_depth(50.0) == Approx(MAX_PREFER_TOLERANCE));
}

TEST_CASE("The edited volume's footprint takes over only from its own object or empty space", "[EmbossPicking]")
{
    REQUIRE(footprint_takes_over(HoverOwner::Nothing));
    REQUIRE(footprint_takes_over(HoverOwner::EditedVolume));
    REQUIRE(footprint_takes_over(HoverOwner::SameObjectPart));
    REQUIRE_FALSE(footprint_takes_over(HoverOwner::OtherTextOrSvg));
    REQUIRE_FALSE(footprint_takes_over(HoverOwner::OtherObject));
    REQUIRE_FALSE(footprint_takes_over(HoverOwner::GizmoHandle));
}

TEST_CASE("Footprint covers the gaps between glyphs and a small padding", "[EmbossPicking]")
{
    // Hull of a line of text, 200 x 40 px; either winding.
    Polygon hull{ Point(100, 100), Point(300, 100), Point(300, 140), Point(100, 140) };
    Polygon hull_cw{ Point(100, 100), Point(100, 140), Point(300, 140), Point(300, 100) };

    for (const Polygon *h : { &hull, &hull_cw }) {
        // Between two letters, in the middle of the line.
        REQUIRE(footprint_contains(*h, Vec2d(205., 120.), 0.));
        // On the outline.
        REQUIRE(footprint_contains(*h, Vec2d(100., 120.), 0.));
        // Just outside, inside the padding.
        REQUIRE(footprint_contains(*h, Vec2d(96., 120.), 6.));
        REQUIRE(footprint_contains(*h, Vec2d(304., 144.), 6.)); // near a corner (distance ~5.7)
        // Outside the padding.
        REQUIRE_FALSE(footprint_contains(*h, Vec2d(90., 120.), 6.));
        REQUIRE_FALSE(footprint_contains(*h, Vec2d(306., 146.), 6.)); // corner distance ~8.5
        REQUIRE_FALSE(footprint_contains(*h, Vec2d(200., 160.), 6.));
    }
}

TEST_CASE("Degenerate footprints are padded too", "[EmbossPicking]")
{
    // Text seen exactly edge-on projects to a segment.
    Polygon segment{ Point(0, 0), Point(100, 0) };
    REQUIRE(footprint_contains(segment, Vec2d(50., 4.), 6.));
    REQUIRE_FALSE(footprint_contains(segment, Vec2d(50., 8.), 6.));
    REQUIRE_FALSE(footprint_contains(segment, Vec2d(50., 1.), 0.));

    Polygon dot{ Point(10, 10) };
    REQUIRE(footprint_contains(dot, Vec2d(13., 13.), 6.));
    REQUIRE_FALSE(footprint_contains(dot, Vec2d(20., 20.), 6.));

    REQUIRE_FALSE(footprint_contains(Polygon(), Vec2d(0., 0.), 6.));
}
