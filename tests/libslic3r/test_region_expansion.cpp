#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Algorithm/RegionExpansion.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <numeric>

using namespace Slic3r;

namespace {

ExPolygon rectangle(coord_t x0, coord_t y0, coord_t x1, coord_t y1)
{
    ExPolygon out;
    out.contour.points = { Point(x0, y0), Point(x1, y0), Point(x1, y1), Point(x0, y1) };
    return out;
}

double path_length(const Points &path)
{
    double len = 0.;
    for (size_t i = 1; i < path.size(); ++ i)
        len += (path[i] - path[i - 1]).cast<double>().norm();
    return len;
}

double total_area(const Polygons &polys)
{
    return std::accumulate(polys.begin(), polys.end(), 0., [](double acc, const Polygon &p) { return acc + p.area(); });
}

// Expanded by 0.05 mm (50000 scaled units) with jtSquare, the square [0, 10 mm] x [0, 10 mm]
// has its lower-right beveled edge along x - y = 10 mm + sqrt(2) * 0.05 mm (~10070711).
// An edge of this triangle lies on that line, so both endpoints of the clipped seed land directly
// on boundary vertices and carry no source Z coordinate.
const ExPolygon by_the_corner_of_a_square{
    Polygon{ { 10050000, -70000 }, { 10045000, -25710 }, { 10025000, -45711 } }
};

} // namespace

TEST_CASE("A wave seed whose endpoints both land on boundary vertices keeps its source and boundary", "[RegionExpansion]")
{
    constexpr float  tiny_expansion = 50000.f; // 0.05 mm
    const ExPolygons boundary{ by_the_corner_of_a_square };

    SECTION("Single square source meeting boundary vertices along its beveled corner")
    {
        const ExPolygon          target = rectangle(0, 0, 10000000, 10000000);
        const Algorithm::WaveSeeds seeds = Algorithm::wave_seeds({ target }, boundary, tiny_expansion, true);
        REQUIRE(seeds.size() == 1);
        CHECK(seeds.front().src == 0u);
        CHECK(seeds.front().boundary == 0u);
        REQUIRE(seeds.front().path.size() >= 2);
        CHECK(path_length(seeds.front().path) > 0.);
    }

    SECTION("Source with a hole whose shrunk outline lands on both boundary vertices")
    {
        // Diamond hole whose lower-right edge lies on x - y = 10 mm + sqrt(2) * 0.1 mm;
        // shrunk by tiny_expansion it lands on the boundary's edge (10045000, -25710)--(10025000, -45711).
        ExPolygon source = rectangle(-20000000, -10000000, 20000000, 30000000);
        source.holes.emplace_back().points = {
            { 5025000, -5116422 }, { -4975000, 4883578 }, { 5025000, 14883578 }, { 15025000, 4883578 }
        };
        const Algorithm::WaveSeeds seeds = Algorithm::wave_seeds({ source }, boundary, tiny_expansion, true);
        REQUIRE(seeds.size() == 1);
        CHECK(seeds.front().src == 0u);
        CHECK(seeds.front().boundary == 0u);
        CHECK(path_length(seeds.front().path) > 0.);
    }

    SECTION("Closely spaced sources with overlapping inflated bboxes preserve exact source attribution")
    {
        // Expanded by tiny_expansion, adjacent extends left to x = 10.01 mm so its outline encloses
        // by_the_corner_of_a_square without intersecting it, while its inflated bbox overlaps target's.
        // Regardless of source order, the seed must be attributed to target, never to adjacent.
        const ExPolygon adjacent = rectangle(10060000, -10000000, 20060000, 200000);
        const ExPolygon target   = rectangle(0, 0, 10000000, 10000000);
        for (int target_first = 0; target_first < 2; ++ target_first) {
            const ExPolygons          sources = target_first ? ExPolygons{ target, adjacent } : ExPolygons{ adjacent, target };
            const Algorithm::WaveSeeds seeds   = Algorithm::wave_seeds(sources, boundary, tiny_expansion, true);
            REQUIRE(seeds.size() == 1);
            CHECK(seeds.front().src == (target_first ? 0u : 1u));
            CHECK(seeds.front().boundary == 0u);
            CHECK(path_length(seeds.front().path) > 0.);
        }
    }

    SECTION("Two closely spaced sources with overlapping inflated bboxes both produce attributed seeds")
    {
        // Gap between left_src and right_src is 0.04 mm < 2 * tiny_expansion (0.10 mm), so their expanded bboxes overlap.
        const ExPolygon  left_src  = rectangle(0, 0, 5000000, 5000000);
        const ExPolygon  right_src = rectangle(5040000, 0, 10040000, 5000000);
        const ExPolygons upper_bnd{ rectangle(0, 5000000, 10040000, 10000000) };

        for (int left_first = 0; left_first < 2; ++ left_first) {
            const ExPolygons          sources = left_first ? ExPolygons{ left_src, right_src } : ExPolygons{ right_src, left_src };
            const Algorithm::WaveSeeds seeds   = Algorithm::wave_seeds(sources, upper_bnd, tiny_expansion, true);
            REQUIRE(seeds.size() == 2);
            CHECK(seeds[0].boundary == 0u);
            CHECK(seeds[1].boundary == 0u);
            CHECK(seeds[0].src == 0u);
            CHECK(seeds[1].src == 1u);

            const uint32_t left_id  = left_first ? 0u : 1u;
            const uint32_t right_id = left_first ? 1u : 0u;
            for (const Algorithm::WaveSeed &seed : seeds) {
                CHECK(path_length(seed.path) > 0.);
                for (const Point &pt : seed.path) {
                    if (seed.src == left_id)
                        CHECK(pt.x() <= 5050000 + SCALED_EPSILON);
                    else {
                        REQUIRE(seed.src == right_id);
                        CHECK(pt.x() >= 4990000 - SCALED_EPSILON);
                    }
                }
            }
        }
    }
}

TEST_CASE("Shared boundary vertex between adjacent boundaries attributes seed to the true containing boundary", "[RegionExpansion]")
{
    constexpr float tiny_expansion = 50000.f; // 0.05 mm

    SECTION("Two-point seed crossing the interior of one boundary with an endpoint on a shared vertex")
    {
        // Expanded by 0.05 mm, source's top edge runs along y = 1.05 mm for x in [-0.02, 1.02] mm.
        // right_bnd has vertices at V0 = (0.5, 1.05) mm and V2 = (0.9, 1.05) mm, so the clipped seed is the
        // exact 2-point segment V0--V2 crossing strictly through right_bnd's interior with no Z callback.
        // left_bnd touches right_bnd only at the shared vertex V0 and stays above y = 1.05 mm.
        const ExPolygon source = rectangle(0, 0, 1000000, 1000000);
        const ExPolygon left_bnd{ Polygon{ { 500000, 1050000 }, { 400000, 1500000 }, { 100000, 1200000 } } };
        const ExPolygon right_bnd{ Polygon{ { 500000, 1050000 }, { 700000, 900000 }, { 900000, 1050000 }, { 700000, 1300000 } } };

        for (int left_first = 0; left_first < 2; ++ left_first) {
            const ExPolygons          boundary = left_first ? ExPolygons{ left_bnd, right_bnd } : ExPolygons{ right_bnd, left_bnd };
            const Algorithm::WaveSeeds seeds    = Algorithm::wave_seeds({ source }, boundary, tiny_expansion, true);
            REQUIRE(seeds.size() == 1);
            CHECK(seeds.front().src == 0u);
            CHECK(seeds.front().boundary == (left_first ? 1u : 0u));
            REQUIRE(seeds.front().path.size() == 2);
            CHECK(path_length(seeds.front().path) == Catch::Approx(400000.));
        }
    }

    SECTION("Two-point seed lying along a boundary edge when another boundary shares one endpoint")
    {
        // neighbor touches by_the_corner_of_a_square only at vertex (10025000, -45711) and stays strictly outside the expanded target.
        const ExPolygon target = rectangle(0, 0, 10000000, 10000000);
        const ExPolygon neighbor{ Polygon{ { 10025000, -45711 }, { 10025000, -95711 }, { 10045000, -95711 } } };

        for (int neighbor_first = 0; neighbor_first < 2; ++ neighbor_first) {
            const ExPolygons boundary = neighbor_first ? ExPolygons{ neighbor, by_the_corner_of_a_square }
                                                       : ExPolygons{ by_the_corner_of_a_square, neighbor };
            const Algorithm::WaveSeeds seeds = Algorithm::wave_seeds({ target }, boundary, tiny_expansion, true);
            REQUIRE(seeds.size() == 1);
            CHECK(seeds.front().src == 0u);
            CHECK(seeds.front().boundary == (neighbor_first ? 1u : 0u));
            CHECK(path_length(seeds.front().path) > 0.);
        }
    }

    SECTION("Shared boundary vertex with AABB tree enabled (>= 16 boundaries)")
    {
        const ExPolygon source = rectangle(0, 0, 1000000, 1000000);
        const ExPolygon left_bnd{ Polygon{ { 500000, 1050000 }, { 400000, 1500000 }, { 100000, 1200000 } } };
        const ExPolygon right_bnd{ Polygon{ { 500000, 1050000 }, { 700000, 900000 }, { 900000, 1050000 }, { 700000, 1300000 } } };

        ExPolygons boundary;
        boundary.push_back(left_bnd);
        for (int i = 0; i < 18; ++ i) {
            const coord_t x0 = 5000000 + i * 2000000;
            boundary.push_back(rectangle(x0, 0, x0 + 1000000, 1000000));
        }
        boundary.push_back(right_bnd);
        const uint32_t expected_boundary_id = uint32_t(boundary.size() - 1);

        const Algorithm::WaveSeeds seeds = Algorithm::wave_seeds({ source }, boundary, tiny_expansion, true);
        REQUIRE(seeds.size() == 1);
        CHECK(seeds.front().src == 0u);
        CHECK(seeds.front().boundary == expected_boundary_id);
        REQUIRE(seeds.front().path.size() == 2);
        CHECK(path_length(seeds.front().path) == Catch::Approx(400000.));
    }
}

TEST_CASE("A source reaching two boundaries that share an edge leaves a seed in each", "[RegionExpansion]")
{
    constexpr float tiny_expansion = 50000.f; // 0.05 mm

    // Two halves of a square sharing the edge x = 5 mm, and a source straddling that edge.
    const ExPolygon left   = rectangle(0, 0, 5000000, 10000000);
    const ExPolygon right  = rectangle(5000000, 0, 10000000, 10000000);
    const ExPolygon source = rectangle(4500000, 4000000, 5500000, 6000000);

    for (int left_first = 0; left_first < 2; ++ left_first) {
        const ExPolygons          boundary = left_first ? ExPolygons{ left, right } : ExPolygons{ right, left };
        const Algorithm::WaveSeeds seeds    = Algorithm::wave_seeds({ source }, boundary, tiny_expansion, true);
        REQUIRE(seeds.size() == 2);
        for (const Algorithm::WaveSeed &seed : seeds) {
            CHECK(seed.src == 0u);
            const bool in_left = seed.boundary == (left_first ? 0u : 1u);
            REQUIRE(seed.path.size() > 1);
            CHECK(seed.path.front() != seed.path.back());
            CHECK(seed.path.front().x() == 5000000);
            CHECK(seed.path.back().x() == 5000000);
            for (const Point &pt : seed.path) {
                const bool same_side = in_left ? pt.x() <= 5000000 : pt.x() >= 5000000;
                CHECK(same_side);
            }
        }
        CHECK(seeds.front().boundary != seeds.back().boundary);
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
        const ExPolygon           inside = rectangle(9000000, 9000000, 9500000, 9500000);
        const Algorithm::WaveSeeds seeds  = Algorithm::wave_seeds({ inside }, boundary, tiny_expansion, true);
        REQUIRE(seeds.size() == 1);
        CHECK(seeds.front().src == 0u);
        CHECK(seeds.front().boundary == 0u);
        REQUIRE(seeds.front().path.size() > 3);
        CHECK(seeds.front().path.front() == seeds.front().path.back());
    }

    SECTION("A source reaching no boundary leaves no seed")
    {
        const ExPolygons sources{ rectangle(4200000, 2100000, 4800000, 3900000), rectangle(20000000, 20000000, 21000000, 21000000) };
        CHECK(Algorithm::wave_seeds(sources, boundary, tiny_expansion, true).empty());
    }

    SECTION("A source crossing the island leaves an open seed in it")
    {
        const ExPolygon           crossing = rectangle(3500000, 2500000, 4500000, 3500000);
        const ExPolygons          sources{ rectangle(4200000, 2100000, 4800000, 3900000), crossing };
        const Algorithm::WaveSeeds seeds = Algorithm::wave_seeds(sources, boundary, tiny_expansion, true);
        REQUIRE(seeds.size() == 1);
        CHECK(seeds.front().src == 1u);
        CHECK(seeds.front().boundary == 1u);
        REQUIRE(seeds.front().path.size() > 1);
        CHECK(seeds.front().path.front() != seeds.front().path.back());
        CHECK(seeds.front().path.front().x() == 3800000);
        CHECK(seeds.front().path.back().x() == 3800000);
    }
}

TEST_CASE("expand_expolygons propagates waves into touching boundaries", "[RegionExpansion]")
{
    SECTION("Standard rectangular expansion stays within the boundary")
    {
        const ExPolygons src{ rectangle(0, 0, 5000000, 5000000) };
        const ExPolygons boundary{ rectangle(5000000, 0, 15000000, 5000000) };
        const float      expansion = scaled<float>(2.0f);
        const float      step      = scaled<float>(0.5f);

        const std::vector<Polygons> expanded = Algorithm::expand_expolygons(src, boundary, expansion, step, 5);
        REQUIRE(expanded.size() == 1);
        REQUIRE(! expanded.front().empty());

        const double area = total_area(expanded.front());
        // Expected expanded strip is roughly 2 mm x 5 mm = 10 mm^2 (1e13 scaled^2).
        CHECK(area > scaled<double>(8.0) * scaled<double>(1.0));
        CHECK(area <= boundary.front().area() + 1.);

        const BoundingBox ext = get_extents(expanded.front());
        CHECK(ext.min.x() >= 5000000 - SCALED_EPSILON);
        CHECK(ext.max.x() <= 7000000 + scaled<coord_t>(0.15));
        CHECK(ext.min.y() >= 0 - SCALED_EPSILON);
        CHECK(ext.max.y() <= 5000000 + SCALED_EPSILON);
    }

    SECTION("Expansion succeeds when wave seed endpoints both land on boundary vertices")
    {
        const ExPolygons src{ rectangle(0, 0, 10000000, 10000000) };
        const ExPolygons boundary{ by_the_corner_of_a_square };
        // Full expansion 1.0 mm with step 0.3 mm produces tiny_expansion = scaled<float>(0.05f) = 50000.
        const std::vector<Polygons> expanded = Algorithm::expand_expolygons(src, boundary, scaled<float>(1.0f), scaled<float>(0.3f), 4);
        REQUIRE(expanded.size() == 1);
        REQUIRE(! expanded.front().empty());
        CHECK(total_area(expanded.front()) > 0.);
        CHECK(total_area(expanded.front()) <= boundary.front().area() + 1.);
    }

    SECTION("Touching boundaries share horizontal source edge without truncating open seed")
    {
        const ExPolygon left_bnd{ Polygon{ { scale_(0.), scale_(0.) }, { scale_(10.), scale_(0.) }, { scale_(10.), scale_(10.) }, { scale_(0.), scale_(10.) } } };
        const ExPolygon right_bnd{ Polygon{ { scale_(10.), scale_(0.) }, { scale_(20.), scale_(0.) }, { scale_(20.), scale_(10.) }, { scale_(10.), scale_(10.) } } };
        const ExPolygons boundary{ left_bnd, right_bnd };
        const ExPolygons src{ ExPolygon{ Polygon{ { scale_(2.), scale_(4.) }, { scale_(18.), scale_(4.) }, { scale_(18.), scale_(6.) }, { scale_(2.), scale_(6.) } } } };

        const auto params = Algorithm::RegionExpansionParameters::build(float(scale_(0.8)), float(scale_(0.4)), 2);
        const auto seeds  = Algorithm::wave_seeds(src, boundary, params.tiny_expansion, true);
        double left_len = 0., right_len = 0.;
        for (const auto &s : seeds) {
            if (s.boundary == 0)
                left_len += Polyline(s.path).length();
            else if (s.boundary == 1)
                right_len += Polyline(s.path).length();
        }
        CHECK(left_len > scale_(7.));
        CHECK(right_len > scale_(7.));
    }

    SECTION("Narrow collinear diagonal boundary attributes seeds via doubled midpoints")
    {
        const ExPolygon strip{ Polygon{ { 0, 0 }, { 25082843, 25082843 }, { 25082843, 25082846 }, { 0, 3 } } };
        const ExPolygons boundary{ strip };
        const ExPolygons src{ ExPolygon{ Polygon{ { 1000000, 0 }, { 2000000, 0 }, { 2000000, 3000000 }, { 1000000, 3000000 } } } };

        const auto seeds = Algorithm::wave_seeds(src, boundary, float(SCALED_EPSILON), true);
        REQUIRE(! seeds.empty());
        for (const auto &s : seeds)
            CHECK(s.boundary == 0);
    }

    SECTION("32 parallel diagonal strips attribute 1-to-1 and expand cleanly")
    {
        ExPolygons src, boundary;
        const size_t n = 32;
        src.reserve(n);
        boundary.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            const coord_t off = coord_t(i * scale_(2.5));
            boundary.emplace_back(Polygon{
                { off, 0 },
                { off + scale_(10.), scale_(10.) },
                { off + scale_(10.) + scale_(0.8), scale_(10.) - scale_(0.8) },
                { off + scale_(0.8), -scale_(0.8) }
            });
            src.emplace_back(Polygon{
                { off + scale_(2.), scale_(2.) },
                { off + scale_(4.), scale_(4.) },
                { off + scale_(4.) + scale_(0.4), scale_(4.) - scale_(0.4) },
                { off + scale_(2.) + scale_(0.4), scale_(2.) - scale_(0.4) }
            });
        }
        const auto params = Algorithm::RegionExpansionParameters::build(float(scale_(0.6)), float(scale_(0.3)), 2);
        const auto seeds  = Algorithm::wave_seeds(src, boundary, params.tiny_expansion, true);
        REQUIRE(seeds.size() >= n);
        for (const auto &s : seeds)
            CHECK(s.src == s.boundary);
        ExPolygons src_copy = src;
        const auto expanded = Algorithm::expand_merge_expolygons(std::move(src_copy), boundary, params);
        CHECK(expanded.size() == n);
    }
}

TEST_CASE("Wave seeds retain boundary ownership through nested holes and intersecting solids", "[RegionExpansion]")
{
    ExPolygons sources, boundaries;
    for (coord_t i = 0; i < 20; ++i) {
        const coord_t radius = 1000000 + i * 500000;
        ExPolygon     ring   = rectangle(-radius, -radius, radius, radius);
        ring.holes.emplace_back(rectangle(-radius + 200000, -radius + 200000, radius - 200000, radius - 200000).contour);
        ring.holes.back().reverse();
        boundaries.emplace_back(std::move(ring));
        sources.emplace_back(rectangle(radius, -500000, radius + 150000, 500000));
    }
    // This solid overlaps the innermost ring and lies inside the holes of the remaining rings.
    boundaries.emplace_back(rectangle(850000, -200000, 1100000, 200000));
    for (int order = 0; order < 2; ++order) {
        std::map<std::pair<size_t, size_t>, double> lengths;
        for (const auto& seed : Algorithm::wave_seeds(sources, boundaries, 50000.f, true))
            lengths[{seed.src, seed.boundary}] += path_length(seed.path);
        for (size_t s = 0; s < sources.size(); ++s) {
            for (size_t b = 0; b < boundaries.size(); ++b) {
                const auto reference = Algorithm::wave_seeds({sources[s]}, {boundaries[b]}, 50000.f, true);
                double     expected  = 0.;
                for (const auto& seed : reference)
                    expected += path_length(seed.path);
                CHECK(lengths[std::make_pair(s, b)] == Catch::Approx(expected));
            }
        }
        std::reverse(boundaries.begin(), boundaries.end());
    }
}

TEST_CASE("Disjoint rows and columns keep the original wave seed IDs", "[RegionExpansion]")
{
    for (bool vertical : {false, true}) {
        ExPolygons sources, boundaries;
        for (int i = 0; i < 25; ++i) {
            const coord_t x = vertical ? 0 : 2000000 * i;
            const coord_t y = vertical ? 2000000 * i : 0;
            sources.emplace_back(rectangle(x, y, x + 500000, y + 500000));
            boundaries.emplace_back(rectangle(x + 500000, y, x + 1000000, y + 500000));
        }
        std::reverse(boundaries.begin(), boundaries.end());
        const auto seeds = Algorithm::wave_seeds(sources, boundaries, 50000.f, true);
        REQUIRE(seeds.size() == sources.size());
        for (size_t i = 0; i < seeds.size(); ++i) {
            CHECK(seeds[i].boundary == i);
            CHECK(seeds[i].src == sources.size() - 1 - i);
            CHECK(path_length(seeds[i].path) == Catch::Approx(500000.));
        }
    }
}

TEST_CASE("RegionExpansion MR13 benchmarks", "[region_expansion][.mr13_bench]")
{
    auto median_ms = [](auto &&fn) {
        fn(); // warmup
        std::array<double, 3> ts{};
        for (double &t : ts) {
            const auto t0 = std::chrono::steady_clock::now();
            fn();
            const auto t1 = std::chrono::steady_clock::now();
            t = std::chrono::duration<double, std::milli>(t1 - t0).count();
        }
        std::sort(ts.begin(), ts.end());
        return ts[1];
    };

    for (size_t n : { size_t(128), size_t(256) }) {
        ExPolygons src, boundary;
        src.reserve(n);
        boundary.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            const coord_t off = coord_t(i * scale_(2.5));
            boundary.emplace_back(Polygon{
                { off, 0 },
                { off + scale_(10.), scale_(10.) },
                { off + scale_(10.) + scale_(0.8), scale_(10.) - scale_(0.8) },
                { off + scale_(0.8), -scale_(0.8) }
            });
            src.emplace_back(Polygon{
                { off + scale_(2.), scale_(2.) },
                { off + scale_(4.), scale_(4.) },
                { off + scale_(4.) + scale_(0.4), scale_(4.) - scale_(0.4) },
                { off + scale_(2.) + scale_(0.4), scale_(2.) - scale_(0.4) }
            });
        }
        const auto params = Algorithm::RegionExpansionParameters::build(float(scale_(0.6)), float(scale_(0.3)), 2);
        size_t seed_cnt = 0;
        const double ms = median_ms([&]() {
            ExPolygons s_copy = src;
            seed_cnt = Algorithm::wave_seeds(s_copy, boundary, params.tiny_expansion, true).size();
            auto res = Algorithm::expand_merge_expolygons(std::move(s_copy), boundary, params);
            REQUIRE(res.size() == n);
        });
        std::cout << "[MR13_BENCH] diagonal_strips n=" << n << " seeds=" << seed_cnt << " median_ms=" << ms << "\n";
    }

    {
        // Comb source touching 1000 islands + narrow collinear boundary (25.082843 mm).
        ExPolygons boundary;
        boundary.reserve(1001);
        Points comb_pts;
        comb_pts.push_back(Point(0, -coord_t(scale_(2.))));
        for (size_t i = 0; i < 1000; ++i) {
            const coord_t x0 = coord_t(i * scale_(2.));
            boundary.emplace_back(Polygon{
                Point(x0, coord_t(scale_(0.5))),
                Point(x0 + coord_t(scale_(1.)), coord_t(scale_(0.5))),
                Point(x0 + coord_t(scale_(1.)), coord_t(scale_(2.5))),
                Point(x0, coord_t(scale_(2.5)))
            });
            comb_pts.push_back(Point(x0, coord_t(scale_(1.))));
            comb_pts.push_back(Point(x0 + coord_t(scale_(1.)), coord_t(scale_(1.))));
            comb_pts.push_back(Point(x0 + coord_t(scale_(1.)), 0));
        }
        comb_pts.push_back(Point(coord_t(1000 * scale_(2.)), -coord_t(scale_(2.))));
        boundary.emplace_back(Polygon{
            Point(0, coord_t(scale_(5.))),
            Point(25082843, coord_t(scale_(5.))),
            Point(25082843, coord_t(scale_(5.)) + 2),
            Point(0, coord_t(scale_(5.)) + 2)
        });

        ExPolygons src;
        src.emplace_back(Polygon(std::move(comb_pts)));
        src.emplace_back(Polygon{ { scale_(1.), scale_(4.5) }, { scale_(20.), scale_(4.5) }, { scale_(20.), scale_(5.5) }, { scale_(1.), scale_(5.5) } });

        size_t seed_cnt = 0;
        const double ms = median_ms([&]() {
            seed_cnt = Algorithm::wave_seeds(src, boundary, float(scale_(0.05)), true).size();
        });
        CHECK(seed_cnt >= 1000);
        std::cout << "[MR13_BENCH] comb_1000_islands_plus_narrow seeds=" << seed_cnt << " median_ms=" << ms << "\n";
    }
}
