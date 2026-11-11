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

