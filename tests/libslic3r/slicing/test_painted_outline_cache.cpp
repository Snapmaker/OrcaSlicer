#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Slicing/PaintedLayerOutlineCache.hpp"
using namespace Slic3r;
namespace {
ExPolygon square(coord_t x, coord_t y, coord_t size)
{
    return ExPolygon(Polygon{Points{{x, y}, {x + size, y}, {x + size, y + size}, {x, y + size}}});
}
} // namespace
TEST_CASE("Painted shell outline reuse preserves bounded and uncached chains", "[PaintPreparation][PaintIslands]")
{
    std::vector<ExPolygons> layers;
    for (coord_t layer = 0; layer < 31; ++layer) {
        ExPolygon polygon = square(layer * 10000, 0, 10000000 - layer * 20000);
        Polygon   hole    = square(2000000, 2000000 + layer * 10000, 2000000).contour;
        hole.reverse();
        polygon.holes.push_back(std::move(hole));
        layers.push_back({std::move(polygon)});
    }
    for (size_t point_limit : {size_t(0), size_t(16), size_t(32768)}) {
        PaintedLayerOutlineCache cache(layers, 15, point_limit, 3);
        for (size_t color = 0; color < 4; ++color)
            for (bool top : {false, true}) {
                ExPolygons expected = layers[15], scratch;
                for (size_t distance = 1; distance <= 5 + color; ++distance) {
                    expected = intersection_ex(expected, layers[top ? 15 - distance : 15 + distance]);
                    CHECK(cache.step(top, distance, scratch) == expected);
                }
            }
    }
}
