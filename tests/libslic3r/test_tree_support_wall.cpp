#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Support/TreeSupport.hpp"

using namespace Slic3r;

// Covers the hybrid-tree fix for "Support wall loops" (tree_support_wall_count):
// in hybrid support only, the auto value 0 must behave like an explicit 1 so the
// trunk is always drawn with a perimeter wall. All other styles keep their existing
// "auto" behavior, and explicit 1 / 2 are never changed.
TEST_CASE("Tree support wall count: hybrid auto maps to single wall", "[TreeSupport]")
{
    SECTION("Hybrid tree + auto (0) behaves like 1")
    {
        REQUIRE(tree_support_effective_wall_count(smsTreeHybrid, 0) == 1);
    }
    SECTION("Hybrid tree + explicit 1 stays 1")
    {
        REQUIRE(tree_support_effective_wall_count(smsTreeHybrid, 1) == 1);
    }
    SECTION("Hybrid tree + explicit 2 stays 2")
    {
        REQUIRE(tree_support_effective_wall_count(smsTreeHybrid, 2) == 2);
    }
    SECTION("Non-hybrid styles + auto (0) stay 0")
    {
        REQUIRE(tree_support_effective_wall_count(smsDefault, 0) == 0);
        REQUIRE(tree_support_effective_wall_count(smsGrid, 0) == 0);
        REQUIRE(tree_support_effective_wall_count(smsSnug, 0) == 0);
        REQUIRE(tree_support_effective_wall_count(smsTreeSlim, 0) == 0);
        REQUIRE(tree_support_effective_wall_count(smsTreeStrong, 0) == 0);
        REQUIRE(tree_support_effective_wall_count(smsTreeOrganic, 0) == 0);
    }
    SECTION("Non-hybrid styles + explicit values stay unchanged")
    {
        REQUIRE(tree_support_effective_wall_count(smsTreeOrganic, 1) == 1);
        REQUIRE(tree_support_effective_wall_count(smsTreeOrganic, 2) == 2);
        REQUIRE(tree_support_effective_wall_count(smsGrid, 1) == 1);
        REQUIRE(tree_support_effective_wall_count(smsGrid, 2) == 2);
    }
}
