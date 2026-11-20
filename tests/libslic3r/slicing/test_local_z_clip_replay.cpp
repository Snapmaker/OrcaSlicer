#include <catch2/catch_test_macros.hpp>

#include "libslic3r/LocalZ/LocalZClipDecision.hpp"
#include "libslic3r/LocalZ/LocalZClipper.hpp"

#include <random>

using namespace Slic3r;
namespace {
bool bounded(const Point& p) { return p.x() >= -(1 << 29) && p.x() <= (1 << 29) && p.y() >= -(1 << 29) && p.y() <= (1 << 29); }

} // namespace

TEST_CASE("Local-Z empty decisions preserve boundaries and holes", "[LocalZ][ClipIndex]")
{
    const std::vector<ClipMaskEdge> edges{{0, 0, 100, 0},   {100, 0, 100, 100}, {100, 100, 0, 100}, {0, 100, 0, 0},
                                          {30, 30, 30, 70}, {30, 70, 70, 70},   {70, 70, 70, 30},   {70, 30, 30, 30}};
    auto                            decision = [&](std::array<int32_t, 4> p, bool difference) {
        return local_z_segment_proven_empty(edges, {p, 0, uint32_t(edges.size()), uint32_t(difference), 0});
    };
    CHECK(decision({10, 10, 20, 20}, true) == 1);
    CHECK(decision({40, 40, 60, 60}, false) == 1); // Hole.
    CHECK(decision({110, 50, 200, 50}, false) == 1);
    CHECK(decision({-10, 50, 110, 50}, false) == 0);
    CHECK(decision({0, 10, 0, 90}, true) == 0);
    CHECK(decision({1, 10, 1, 90}, true) == 0); // Rounding guard.
    CHECK(decision({40, 40, 40, 40}, false) == 0);
    CHECK(decision({1 << 30, 0, (1 << 30) + 10, 10}, false) == 0);
}

TEST_CASE("Indexed Local-Z decisions exactly preserve the reference predicates", "[LocalZ][ClipIndex]")
{
    std::mt19937                           engine(73195);
    std::uniform_int_distribution<int32_t> coordinate(-20000, 20000);
    std::vector<ClipMaskEdge>              edges{{INT32_MIN, 0, INT32_MAX, 0}}; // Outside the indexed range.
    for (int i = 0; i < 100; ++i) {
        const int32_t x = coordinate(engine), y = coordinate(engine);
        edges.insert(edges.end(), {{x, y, x + 200, y}, {x + 200, y, x + 200, y + 300}, {x + 200, y + 300, x, y + 300}, {x, y + 300, x, y}});
    }
    edges.insert(edges.end(), {{-100, -100, 100, -100},
                               {100, -100, 100, 100},
                               {100, 100, -100, 100},
                               {-100, 100, -100, -100},
                               {-30, -30, -30, 30},
                               {-30, 30, 30, 30},
                               {30, 30, 30, -30},
                               {30, -30, -30, -30},
                               {5, 5, 5, 5}});
    const uint32_t        count = uint32_t(edges.size() - 1);
    const LocalZClipIndex index(edges, 1, count);
    auto                  compare = [&](const std::array<int32_t, 4>& p) {
        for (uint32_t difference : {0u, 1u})
            for (uint32_t keep : {0u, 1u}) {
                ClipSegmentQuery query{p, 1, count, difference, keep};
                CHECK(index.classify(query) == local_z_segment_decision(edges, query));
                CHECK(local_z_segment_proven_empty(edges, query) == uint32_t(local_z_segment_decision(edges, query) == 1));
            }
    };
    for (int i = 0; i < 12000; ++i)
        compare({coordinate(engine), coordinate(engine), coordinate(engine), coordinate(engine)});
    for (const auto& e : edges)
        for (int delta = -3; delta <= 3; ++delta) {
            if (e[0] == INT32_MIN)
                continue;
            compare({e[0] + delta, e[1], e[2] + delta, e[3]});
            compare({e[0], e[1] + delta, e[2], e[3] + delta});
        }
    compare({0, 0, 0, 0});
    compare({INT32_MIN, 0, INT32_MAX, 1});
    const int32_t                   bound = 1 << 29;
    const std::vector<ClipMaskEdge> extremes{{-bound, -bound, bound, -bound},
                                             {bound, -bound, bound, bound},
                                             {bound, bound, -bound, bound},
                                             {-bound, bound, -bound, -bound}};
    const LocalZClipIndex           extreme_index(extremes, 0, 4);
    for (int delta = 0; delta < 5; ++delta) {
        ClipSegmentQuery q{{-bound + delta, -bound + delta, bound - delta, bound - delta}, 0, 4, 1, 1};
        CHECK(extreme_index.classify(q) == local_z_segment_decision(extremes, q));
    }
    const LocalZClipIndex invalid(edges, 0, uint32_t(edges.size()));
    CHECK(invalid.classify({{1000, 1000, 1001, 1001}, 0, uint32_t(edges.size()), 0, 1}) == 0);
    const LocalZClipIndex empty({}, 0, 0);
    CHECK(empty.classify({{1, 1, 2, 2}, 0, 0, 0, 1}) == 1);
    CHECK(empty.classify({{1, 1, 2, 2}, 0, 0, 1, 1}) == 2);
    CHECK(index.classify({{1, 1, 2, 2}, 0, count, 0, 1}) == 0);
    const LocalZClipIndex invalid_range(edges, uint32_t(edges.size()) + 1, 1);
    CHECK(invalid_range.classify({{1, 1, 2, 2}, uint32_t(edges.size()) + 1, 1, 0, 1}) == 0);
}

TEST_CASE("Conservative empty decisions agree with polygon clipping", "[LocalZ][ClipIndex]")
{
    std::mt19937                       engine(307117);
    std::uniform_int_distribution<int> coordinate(-10000, 10000), size(20, 1000);
    size_t                             checked_empty = 0;
    for (size_t iteration = 0; iteration < 400; ++iteration) {
        ExPolygons                polygons;
        std::vector<ClipMaskEdge> edges;
        auto                      add_ring = [&](const Points& points) {
            for (size_t i = 0; i < points.size(); ++i) {
                const auto& a = points[i];
                const auto& b = points[(i + 1) % points.size()];
                edges.push_back({int32_t(a.x()), int32_t(a.y()), int32_t(b.x()), int32_t(b.y())});
            }
        };
        for (size_t m = 0; m < 6; ++m) {
            const int x = coordinate(engine), y = coordinate(engine), width = size(engine), height = size(engine);
            ExPolygon polygon(Points{{x, y}, {x + width, y}, {x + width + 5, y + height}, {x, y + height / 2}});
            if (m % 2 == 0) {
                polygon.holes.emplace_back(Points{{x + 3, y + 3}, {x + 3, y + 8}, {x + 8, y + 8}, {x + 8, y + 3}});
                add_ring(polygon.holes.back().points);
            }
            add_ring(polygon.contour.points);
            polygons.push_back(std::move(polygon));
        }
        LocalZClipper clipper(polygons);
        for (size_t path = 0; path < 20; ++path) {
            Points points;
            for (size_t i = 0; i < 2 + path % 5; ++i)
                points.emplace_back(coordinate(engine), coordinate(engine));
            const Polyline line(points);
            for (bool difference : {false, true}) {
                bool empty = true;
                for (size_t i = 1; i < points.size(); ++i)
                    empty = empty && local_z_segment_proven_empty(edges, {{int32_t(points[i - 1].x()), int32_t(points[i - 1].y()),
                                                                           int32_t(points[i].x()), int32_t(points[i].y())},
                                                                          0,
                                                                          uint32_t(edges.size()),
                                                                          uint32_t(difference),
                                                                          0});
                if (empty) {
                    const auto clipped = difference ? clipper.difference(Polylines{line}) : clipper.intersection(line);
                    REQUIRE(clipped.empty());
                    ++checked_empty;
                }
            }
        }
    }
    CHECK(checked_empty > 1000);
}
