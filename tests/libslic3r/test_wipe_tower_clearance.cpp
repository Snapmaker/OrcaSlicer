#include <catch2/catch_test_macros.hpp>

#include "libslic3r/WipeTowerClearance.hpp"

using namespace Slic3r;

static Polygon square_mm(double x, double y, double side)
{
    return Polygon({{scale_(x), scale_(y)}, {scale_(x + side), scale_(y)}, {scale_(x + side), scale_(y + side)}, {scale_(x), scale_(y + side)}});
}

TEST_CASE("No sparse layers: object inside the extruder clearance radius of the prime tower is reported", "[WipeTowerClearance]")
{
    const Polygon tower = square_mm(0, 0, 20);
    // U1 profile: extruder_clearance_radius = 72.5 mm.
    const coord_t radius = scale_(72.5);

    SECTION("object 50 mm to the right of the tower is inside the radius") {
        Polygons objects{square_mm(150, 150, 10), square_mm(70, 0, 10)};
        REQUIRE(no_sparse_layers_tower_conflict(tower, objects, radius) == 1);
    }
    SECTION("object 100 mm away is outside the radius") {
        Polygons objects{square_mm(120, 0, 10)};
        REQUIRE(no_sparse_layers_tower_conflict(tower, objects, radius) == -1);
    }
    SECTION("no clearance radius configured means no warning") {
        Polygons objects{square_mm(25, 0, 10)};
        REQUIRE(no_sparse_layers_tower_conflict(tower, objects, 0) == -1);
    }
    SECTION("no tower means no warning") {
        Polygons objects{square_mm(25, 0, 10)};
        REQUIRE(no_sparse_layers_tower_conflict(Polygon{}, objects, radius) == -1);
    }
}
