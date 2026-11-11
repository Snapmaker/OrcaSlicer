#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCodeReader.hpp"
#include <string>

using namespace Slic3r;


TEST_CASE("Read-only extrusion flattening borrows leaves without changing order", "[CPUSlicing][GCode]")
{
    ExtrusionPath path(erPerimeter, 0.08, 0.4f, 0.2f);
    path.polyline = Polyline(Points{{0, 0}, {100, 0}, {100, 100}});
    ExtrusionEntityCollection inner;
    inner.no_sort = true;
    inner.append(ExtrusionLoop(path));
    inner.append(ExtrusionMultiPath(path));
    ExtrusionEntityCollection source;
    source.append(path);
    source.append(inner);
    source.append(ExtrusionEntityCollection{});
    source.append(path);

    const auto  leaves = source.flatten_refs();
    const auto* nested = static_cast<const ExtrusionEntityCollection*>(source.entities[1]);
    REQUIRE(leaves == std::vector<const ExtrusionEntity*>{source.entities[0], nested->entities[0], nested->entities[1], source.entities[3]});
    const auto copied = source.flatten(false);
    REQUIRE(leaves.size() == copied.entities.size());
    for (size_t i = 0; i < leaves.size(); ++i) {
        CHECK(leaves[i] != copied.entities[i]);
        CHECK(leaves[i]->role() == copied.entities[i]->role());
        CHECK(leaves[i]->is_loop() == copied.entities[i]->is_loop());
        Points original_points, copied_points;
        leaves[i]->collect_points(original_points);
        copied.entities[i]->collect_points(copied_points);
        CHECK(original_points == copied_points);
    }
    REQUIRE(ExtrusionEntityCollection{}.flatten_refs().empty());
}

TEST_CASE("Borrowed streaming buffers preserve commands and modal coordinates", "[CPUSlicing][GCode]")
{
    const std::vector<std::string> buffers{"G90\nG1 X10 Y5 Z0.2 E1\n", "G91\r\nG1 X2 Y-1\r\n", "G1 X1", ""};
    GCodeReader                    owned, borrowed;
    std::vector<std::string>       expected, actual;
    for (const auto& buffer : buffers) {
        owned.parse_buffer(buffer, [&](GCodeReader&, const GCodeReader::GCodeLine& line) { expected.push_back(line.raw()); });
        borrowed.parse_buffer(buffer.c_str(), buffer.size(),
                              [&](GCodeReader&, const GCodeReader::GCodeLine& line) { actual.push_back(line.raw()); });
    }
    CHECK(actual == expected);
    REQUIRE(actual.size() == 5);
    CHECK(actual.back() == "G1 X1");
    CHECK(borrowed.x() == 13.f);
    CHECK(borrowed.y() == 4.f);
    CHECK(borrowed.x() == owned.x());
    CHECK(borrowed.z() == owned.z());
    CHECK(borrowed.e() == owned.e());
    CHECK(borrowed.relative_xyz());
}
