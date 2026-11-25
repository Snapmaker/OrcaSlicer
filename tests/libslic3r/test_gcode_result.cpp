#include <catch2/catch_test_macros.hpp>
#include "libslic3r/GCode/GCodeProcessor.hpp"

using Slic3r::GCodeProcessorResult;

TEST_CASE("G-code result assignment transfers or copies the toolpath buffers", "[GCodeProcessorResult]")
{
    GCodeProcessorResult source{}, destination{};
    source.moves.resize(1);
    source.moves.front().interpolation_points.emplace_back(1.f, 2.f, 3.f);
    source.lines_ends = {42};
    source.layer_endpoints.emplace_back(0.2f, std::make_pair(0u, 1u));

    const auto* moves = source.moves.data();
    const auto* points = source.moves.front().interpolation_points.data();
    const auto* offsets = source.lines_ends.data();

    SECTION("copy preserves independent storage")
    {
        destination = source;
        REQUIRE(destination.moves.data() != moves);
        REQUIRE(destination.moves.front().interpolation_points.data() != points);
        REQUIRE(destination.lines_ends.data() != offsets);
        REQUIRE(destination.layer_endpoints == source.layer_endpoints);
    }

    SECTION("export moves the original storage and reset releases it")
    {
        destination = std::move(source);
        REQUIRE(destination.moves.data() == moves);
        REQUIRE(destination.moves.front().interpolation_points.data() == points);
        REQUIRE(destination.lines_ends.data() == offsets);
        REQUIRE(destination.lines_ends.front() == 42);
        REQUIRE(destination.layer_endpoints.size() == 1);
        destination.reset();
        REQUIRE(destination.moves.capacity() == 0);
        REQUIRE(destination.lines_ends.capacity() == 0);
    }
}
