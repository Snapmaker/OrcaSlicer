#include <catch2/catch_test_macros.hpp>

#include "libslic3r/AABBTreeLines.hpp"

TEST_CASE("Integer-coordinate line queries find only segments within the radius", "[geometry][line-radius]")
{
    using namespace Slic3r;
    AABBTreeLines::LinesDistancer<Line> distancer(Lines{Line(Point(0, 0), Point(100, 0)), Line(Point(0, 100), Point(100, 100))});
    const auto                          nearby = distancer.all_lines_in_radius(Point(50, 3), 5.0);
    REQUIRE(nearby == std::vector<size_t>{0});
    REQUIRE(distancer.all_lines_in_radius(Point(50, 50), 5.0).empty());
}

TEST_CASE("Empty line queries return no segments", "[geometry][line-radius]")
{
    Slic3r::AABBTreeLines::LinesDistancer<Slic3r::Line> distancer;
    REQUIRE(distancer.all_lines_in_radius(Slic3r::Point(0, 0), 5.0).empty());
}
