#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Point.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Fill/FillHoneycomb.hpp"

#include <memory>

using namespace Slic3r;

// Regression tests for "support_base_pattern_spacing = 0" reaching the honeycomb
// filler through the legacy tree-support toolpath (TreeSupport.cpp): the pattern
// steps collapsed to zero and align_to_grid() raised an integer division by zero
// (exit code 0xC0000094). The fillers must degrade gracefully instead.

TEST_CASE("align_to_grid with zero spacing is a no-op", "[Fill][Regression]")
{
    // The dividend from the field: rotated bbox min X of the keycap project.
    REQUIRE(align_to_grid(coord_t(-6879104), coord_t(0)) == coord_t(-6879104));
    REQUIRE(align_to_grid(coord_t(0), coord_t(0)) == coord_t(0));
    REQUIRE(align_to_grid(coord_t(12345), coord_t(0)) == coord_t(12345));

    // Non-zero spacing behavior must be preserved.
    REQUIRE(align_to_grid(coord_t(250), coord_t(100)) == coord_t(200));
    REQUIRE(align_to_grid(coord_t(-250), coord_t(100)) == coord_t(-300));
    REQUIRE(align_to_grid(coord_t(0), coord_t(100)) == coord_t(0));
}

TEST_CASE("FillHoneycomb with zero spacing completes and returns no infill", "[Fill][Regression]")
{
    std::unique_ptr<Fill> filler(Fill::new_from_type(ipHoneycomb));
    filler->spacing = 0.;

    Points outline{ Point(0, 0), Point(20000000, 0), Point(20000000, 20000000), Point(0, 20000000) };
    ExPolygon expolygon;
    expolygon.contour.points = outline;
    Surface surface(stInternal, std::move(expolygon));

    FillParams params;
    params.density   = 0.35f;
    params.multiline = 1;

    // Pre-fix this crashed with 0xC0000094 (align_to_grid / idiv by zero).
    Polylines out = filler->fill_surface(&surface, params);
    REQUIRE(out.empty());
}

TEST_CASE("FillHoneycomb with valid spacing still generates hexagons", "[Fill]")
{
    std::unique_ptr<Fill> filler(Fill::new_from_type(ipHoneycomb));
    filler->spacing = 2.5; // Orca default support base pattern spacing

    Points outline{ Point(0, 0), Point(20000000, 0), Point(20000000, 20000000), Point(0, 20000000) };
    ExPolygon expolygon;
    expolygon.contour.points = outline;
    Surface surface(stInternal, std::move(expolygon));

    FillParams params;
    params.density   = 0.35f;
    params.multiline = 1;

    Polylines out = filler->fill_surface(&surface, params);
    REQUIRE(!out.empty());
}
