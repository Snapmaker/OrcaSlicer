#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Algorithm/RegionExpansion.hpp"
#include "libslic3r/ExPolygon.hpp"

using namespace Slic3r;

static ExPolygon rectangle(coord_t x0, coord_t y0, coord_t x1, coord_t y1)
{
    ExPolygon out;
    out.contour.points = { Point(x0, y0), Point(x1, y0), Point(x1, y1), Point(x0, y1) };
    return out;
}

static double path_length(const Points &path)
{
    double len = 0.;
    for (size_t i = 1; i < path.size(); ++ i)
        len += (path[i] - path[i - 1]).cast<double>().norm();
    return len;
}

// Expanded by 0.05 mm, the square with its corner at (10 mm, 0) has an edge of its outline on the line
// x - y = 10 mm + sqrt(2) * 0.05 mm. An edge of this boundary lies on that line, so both ends of the piece of the
// outline inside the boundary are vertices of the boundary and carry no index of the source.
static const ExPolygon by_the_corner_of_a_square{ Polygon{ { 10050000, -70000 }, { 10045000, -25710 }, { 10025000, -45711 } } };

TEST_CASE("A seed whose ends are both vertices of the boundary belongs to the source it was clipped from", "[RegionExpansion]")
{
    constexpr float  tiny_expansion = 50000.f; // 0.05 mm
    const ExPolygons boundary{ by_the_corner_of_a_square };

    SECTION("The first point of the seed lies on the outline of a hole of the source")
    {
        // The diamond is 0.1 mm from the corner of the rectangle, its lower right edge on the line
        // x - y = 10 mm + sqrt(2) * 0.1 mm: shrunk by tiny_expansion, it is the line the boundary lies on too.
        ExPolygon source = rectangle(-20000000, -10000000, 20000000, 30000000);
        source.holes.emplace_back().points = { { 5025000, -5116422 }, { -4975000, 4883578 }, { 5025000, 14883578 }, { 15025000, 4883578 } };
        const Algorithm::WaveSeeds seeds = Algorithm::wave_seeds({ source }, boundary, tiny_expansion, true);
        REQUIRE(seeds.size() == 1);
        CHECK(seeds.front().src == 0);
        CHECK(seeds.front().boundary == 0);
        CHECK(path_length(seeds.front().path) > 0.);
    }
    SECTION("The first point of the seed lies on the outline of another source")
    {
        // Expanded by tiny_expansion, the rectangle to the right reaches the seed as well, so the first point of the
        // seed is inside the outlines of both sources. The seed is of the square the boundary is at, whichever
        // index that one has.
        const ExPolygon adjacent = rectangle(10060000, -10000000, 20060000, 200000);
        const ExPolygon target   = rectangle(0, 0, 10000000, 10000000);
        for (int target_first = 0; target_first < 2; ++ target_first) {
            const ExPolygons sources = target_first ? ExPolygons{ target, adjacent } : ExPolygons{ adjacent, target };
            const Algorithm::WaveSeeds seeds = Algorithm::wave_seeds(sources, boundary, tiny_expansion, true);
            REQUIRE(seeds.size() == 1);
            CHECK(seeds.front().src == (target_first ? 0u : 1u));
            CHECK(seeds.front().boundary == 0);
            CHECK(path_length(seeds.front().path) > 0.);
        }
    }
}

TEST_CASE("Seeds inside, around and away from boundaries", "[RegionExpansion]")
{
    constexpr float tiny_expansion = 50000.f; // 0.05 mm

    // A square with a square hole, and inside the hole an island of its own.
    ExPolygons boundary;
    boundary.push_back(rectangle(0, 0, 10000000, 10000000));
    boundary.front().holes.push_back(Polygon{ { 2000000, 2000000 }, { 2000000, 8000000 }, { 8000000, 8000000 }, { 8000000, 2000000 } });
    boundary.push_back(rectangle(2200000, 2200000, 3800000, 3800000));

    SECTION("A source inside a boundary leaves a closed seed in it")
    {
        // In the solid of the square, clear of its hole.
        const ExPolygon inside = rectangle(9000000, 9000000, 9500000, 9500000);
        const Algorithm::WaveSeeds seeds = Algorithm::wave_seeds({ inside }, boundary, tiny_expansion, true);
        REQUIRE(seeds.size() == 1);
        CHECK(seeds.front().src == 0);
        CHECK(seeds.front().boundary == 0);
        REQUIRE(seeds.front().path.size() > 3);
        CHECK(seeds.front().path.front() == seeds.front().path.back());
    }
    SECTION("A source reaching no boundary leaves no seed")
    {
        // In the hole of the square, beside its island, and far away from both.
        const ExPolygons sources{ rectangle(4200000, 2100000, 4800000, 3900000), rectangle(20000000, 20000000, 21000000, 21000000) };
        CHECK(Algorithm::wave_seeds(sources, boundary, tiny_expansion, true).empty());
    }
    SECTION("A source crossing the island leaves an open seed in it")
    {
        // Half of it is in the island, half in the hole around the island.
        const ExPolygon crossing = rectangle(3500000, 2500000, 4500000, 3500000);
        const ExPolygons sources{ rectangle(4200000, 2100000, 4800000, 3900000), crossing };
        const Algorithm::WaveSeeds seeds = Algorithm::wave_seeds(sources, boundary, tiny_expansion, true);
        REQUIRE(seeds.size() == 1);
        CHECK(seeds.front().src == 1);
        CHECK(seeds.front().boundary == 1);
        REQUIRE(seeds.front().path.size() > 1);
        CHECK(seeds.front().path.front() != seeds.front().path.back());
        // The expanded outline leaves the island at the edge of the island, at x = 3.8 mm.
        CHECK(seeds.front().path.front().x() == 3800000);
        CHECK(seeds.front().path.back().x() == 3800000);
    }
}

TEST_CASE("A source reaching two boundaries that share an edge leaves a seed in each", "[RegionExpansion]")
{
    constexpr float tiny_expansion = 50000.f; // 0.05 mm

    // Two halves of a square sharing the edge x = 5 mm, and a source straddling that edge.
    const ExPolygon left  = rectangle(0, 0, 5000000, 10000000);
    const ExPolygon right = rectangle(5000000, 0, 10000000, 10000000);
    const ExPolygon source = rectangle(4500000, 4000000, 5500000, 6000000);

    for (int left_first = 0; left_first < 2; ++ left_first) {
        const ExPolygons boundary = left_first ? ExPolygons{ left, right } : ExPolygons{ right, left };
        const Algorithm::WaveSeeds seeds = Algorithm::wave_seeds({ source }, boundary, tiny_expansion, true);
        // One seed on each side of the shared edge, none split where the outline of the source opens.
        REQUIRE(seeds.size() == 2);
        for (const Algorithm::WaveSeed &seed : seeds) {
            CHECK(seed.src == 0);
            // The left half has the index 0 or 1, the right half the other one.
            const bool in_left = seed.boundary == (left_first ? 0u : 1u);
            REQUIRE(seed.path.size() > 1);
            CHECK(seed.path.front() != seed.path.back());
            // Both ends are on the shared edge.
            CHECK(seed.path.front().x() == 5000000);
            CHECK(seed.path.back().x() == 5000000);
            // And on the same side of it.
            for (const Point &pt : seed.path) {
                const bool same_side = in_left ? pt.x() <= 5000000 : pt.x() >= 5000000;
                CHECK(same_side);
            }
        }
        CHECK(seeds.front().boundary != seeds.back().boundary);
    }
}
