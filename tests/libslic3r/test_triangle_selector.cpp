#include <catch2/catch.hpp>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <algorithm>
#include <vector>

using namespace Slic3r;

TEST_CASE("Triangle selector round-trips painted states above sixteen", "[TriangleSelector][MMUPaint]")
{
    indexed_triangle_set its;
    its.vertices = {
        Vec3f(0.f, 0.f, 0.f),
        Vec3f(1.f, 0.f, 0.f),
        Vec3f(0.f, 1.f, 0.f),
    };
    its.indices = {
        stl_triangle_vertex_indices(0, 1, 2),
    };

    TriangleMesh mesh(its);
    TriangleSelector selector(mesh);

    constexpr int painted_state = 120;
    selector.set_facet(0, static_cast<EnforcerBlockerType>(painted_state));

    auto data = selector.serialize();
    REQUIRE_FALSE(data.triangles_to_split.empty());
    REQUIRE(data.used_states.size() > painted_state);
    CHECK(data.used_states[painted_state]);

    data.reset_used_states();
    REQUIRE(data.update_used_states(size_t(data.triangles_to_split.front().bitstream_start_idx)));
    CHECK(data.used_states[painted_state]);
    CHECK(TriangleSelector::has_facets(data, static_cast<EnforcerBlockerType>(painted_state)));

    TriangleSelector restored(mesh);
    restored.deserialize(data, true, static_cast<EnforcerBlockerType>(painted_state));
    CHECK(restored.has_facets(static_cast<EnforcerBlockerType>(painted_state)));
}

// Pack 4-bit codes into a bitstream, least significant bit first, in the order the decoder reads them.
static std::vector<bool> pack_nibbles(const std::vector<int> &nibbles)
{
    std::vector<bool> bitstream;
    for (const int nibble : nibbles)
        for (int bit = 0; bit < 4; ++bit)
            bitstream.push_back((nibble >> bit) & 1);
    return bitstream;
}

TEST_CASE("A valid paint stream with nested splits round-trips bit for bit", "[TriangleSelector]")
{
    const TriangleMesh mesh = make_cube(10., 10., 10.);

    TriangleSelector::TriangleSplittingData data;
    data.triangles_to_split.emplace_back(0, 0);
    // A three-side split whose children, in stream order, are: a one-side split (side 2) into two
    // leaves, a two-side split (side 1) into leaves of states 20, 0 and 8, then two plain leaves.
    // State 20 uses one 0b1111 extension; that encoding is the same in Edge and upstream.
    const std::vector<int> triangle_0 = {0b0011,
                                         0b1001, 0b1000, 0b0100,
                                         0b0110, 0b1100, 0b1111, 20 - 18, 0b0000, 0b1100, 8 - 3,
                                         0b1000,
                                         0b0100};
    data.bitstream = pack_nibbles(triangle_0);
    data.triangles_to_split.emplace_back(5, int(data.bitstream.size()));
    const std::vector<bool> triangle_5 = pack_nibbles({0b1100, 3 - 3});
    data.bitstream.insert(data.bitstream.end(), triangle_5.begin(), triangle_5.end());
    data.reset_used_states();
    REQUIRE(data.update_used_states(0));

    TriangleSelector restored(mesh);
    restored.deserialize(data);

    REQUIRE(restored.num_facets(static_cast<EnforcerBlockerType>(20)) == 1);
    REQUIRE(restored.num_facets(EnforcerBlockerType::Extruder3) == 1);
    REQUIRE(restored.serialize() == data);
}

TEST_CASE("Painted states 20 and 200 round-trip through Edge multi-nibble encoding", "[TriangleSelector][Edge][MMUPaint]")
{
    const int painted_state = GENERATE(20, 200);
    INFO("painted state " << painted_state);

    const TriangleMesh mesh = make_cube(10., 10., 10.);
    TriangleSelector   selector(mesh);
    selector.set_facet(0, static_cast<EnforcerBlockerType>(painted_state));

    auto data = selector.serialize();
    REQUIRE_FALSE(data.triangles_to_split.empty());
    REQUIRE(data.used_states.size() > size_t(painted_state));
    CHECK(data.used_states[size_t(painted_state)]);

    data.reset_used_states();
    REQUIRE(data.update_used_states(0));
    CHECK(data.used_states[size_t(painted_state)]);
    CHECK(TriangleSelector::has_facets(data, static_cast<EnforcerBlockerType>(painted_state)));

    TriangleSelector restored(mesh);
    REQUIRE_NOTHROW(restored.deserialize(data));
    CHECK(restored.num_facets(static_cast<EnforcerBlockerType>(painted_state)) == 1);
    REQUIRE(restored.serialize() == selector.serialize());
}

TEST_CASE("A truncated or malformed paint stream drops only the damaged triangle", "[TriangleSelector][Regression]")
{
    struct Case
    {
        const char      *name;
        std::vector<int> nibbles;
    };
    // Loop + DYNAMIC_SECTION: Catch2 v2 has no StringMaker for this aggregate.
    const Case cases[] = {
        {"three-side split missing two children", {0b0011, 0b1000, 0b1000}},
        {"leaf missing its state nibble", {0b1100}},
        {"leaf missing its second state nibble", {0b1100, 0b1111}},
        {"all-F extension stream",
         {0b1100, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF}},
        {"splits nested past the end",
         {0b0011, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF}},
        {"one-side split of the nonexistent side 3", {0b1101, 0b1000, 0b1000}},
    };

    const TriangleMesh mesh = make_cube(10., 10., 10.);
    TriangleSelector   intact(mesh);
    intact.set_facet(0, EnforcerBlockerType::Extruder2);

    for (const auto &c : cases) {
        DYNAMIC_SECTION(c.name)
        {
            // Triangle 0 stays intact, triangle 1 carries the damaged stream.
            TriangleSelector::TriangleSplittingData data = intact.serialize();
            data.triangles_to_split.emplace_back(1, int(data.bitstream.size()));
            const std::vector<bool> damaged = pack_nibbles(c.nibbles);
            data.bitstream.insert(data.bitstream.end(), damaged.begin(), damaged.end());

            TriangleSelector restored(mesh);
            REQUIRE_NOTHROW(restored.deserialize(data));
            // Triangle 1 unwinds completely, so the selector holds exactly the intact paint.
            REQUIRE(restored.serialize() == intact.serialize());

            REQUIRE_NOTHROW(TriangleSelector::has_facets(data, EnforcerBlockerType::Extruder3));

            TriangleSelector::TriangleSplittingData recomputed = data;
            recomputed.reset_used_states();
            REQUIRE_FALSE(recomputed.update_used_states(0));
            REQUIRE(std::none_of(recomputed.used_states.begin(), recomputed.used_states.end(), [](bool used) { return used; }));
        }
    }
}

TEST_CASE("update_used_states leaves used_states untouched on a truncated stream", "[TriangleSelector][Regression]")
{
    TriangleSelector::TriangleSplittingData data;
    data.triangles_to_split.emplace_back(0, 0);
    data.bitstream = pack_nibbles({0b1100}); // leaf prefix, missing the state nibble
    data.reset_used_states();
    data.used_states[size_t(EnforcerBlockerType::Extruder2)] = true;

    REQUIRE_FALSE(data.update_used_states(0));
    for (size_t i = 0; i < data.used_states.size(); ++i) {
        if (i == size_t(EnforcerBlockerType::Extruder2))
            REQUIRE(data.used_states[i]);
        else
            REQUIRE_FALSE(data.used_states[i]);
    }
}
