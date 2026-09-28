#include <catch2/catch.hpp>

#include "libslic3r/GCode/WipeTower2.hpp"

#include <set>

using namespace Slic3r;

TEST_CASE("Wipe tower entry stagger distributes start offsets", "[WipeTower]")
{
    SECTION("disabled stagger keeps the legacy entry point")
    {
        REQUIRE(WipeTower2::toolchange_entry_stagger_offset(false, 10, 3, 0.5f) == Approx(0.f));
    }

    SECTION("enabled stagger varies by layer and toolchange")
    {
        const float line_spacing = 0.5f;

        REQUIRE(WipeTower2::toolchange_entry_stagger_offset(true, 0, 0, line_spacing) == Approx(0.f));
        REQUIRE(WipeTower2::toolchange_entry_stagger_offset(true, 1, 0, line_spacing) == Approx(7.f * line_spacing));
        REQUIRE(WipeTower2::toolchange_entry_stagger_offset(true, 0, 1, line_spacing) == Approx(5.f * line_spacing));
        REQUIRE(WipeTower2::toolchange_entry_stagger_offset(true, 17, 0, line_spacing) == Approx(0.f));
    }

    SECTION("enabled stagger spreads starts across a long cycle")
    {
        std::set<size_t> slots;
        for (size_t layer_idx = 0; layer_idx < 17; ++layer_idx)
            slots.insert(WipeTower2::toolchange_entry_stagger_slot(true, layer_idx, 0));

        REQUIRE(slots.size() == 17);
    }

    SECTION("non-positive line spacing disables the offset")
    {
        REQUIRE(WipeTower2::toolchange_entry_stagger_offset(true, 2, 1, 0.f) == Approx(0.f));
        REQUIRE(WipeTower2::toolchange_entry_stagger_offset(true, 2, 1, -0.5f) == Approx(0.f));
    }
}
