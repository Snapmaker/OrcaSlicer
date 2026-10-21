#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"

#include <memory>

using namespace Slic3r;

namespace {

// Coordinates are in scaled units (microns), 10 mm = 10'000'000.
// Both factories hand the ownership over to the caller.

ExtrusionPath *make_path()
{
    // Open polyline, support interface role.
    std::unique_ptr<ExtrusionPath> path = std::make_unique<ExtrusionPath>(erSupportMaterialInterface, 1.0, 0.5f, 0.2f);
    path->polyline.points = {Point(0, 0), Point(10000000, 0), Point(10000000, 10000000)};
    return path.release();
}

ExtrusionLoop *make_loop()
{
    // Closed polyline (first point repeated at the end), support interface role.
    ExtrusionPath path_template(erSupportMaterialInterface, 1.0, 0.5f, 0.2f);
    path_template.polyline.points = {Point(0, 0), Point(10000000, 0), Point(10000000, 10000000), Point(0, 0)};
    return std::make_unique<ExtrusionLoop>(std::move(path_template)).release();
}

// Registers every entity for deletion through an ExtrusionEntityCollection,
// which owns and destroys the entities it holds.
void collect_for_cleanup(ExtrusionEntityCollection &cleanup, ExtrusionEntitiesPtr &entities)
{
    cleanup.entities.insert(cleanup.entities.end(), entities.begin(), entities.end());
    entities.clear();
}

} // namespace

// Regression coverage for the support height-modulation crash (crashes on
// 2.4.1 alpha, GUI and CLI): support transition perimeter loops are
// ExtrusionLoop entities living in the same list as ExtrusionPath entities,
// and the modulator used to dynamic_cast the front entity to ExtrusionPath
// unconditionally. detach_extrusion_loops() / prepend_extrusion_entities()
// are the type-safe pass-through it now relies on.
TEST_CASE("Detach and prepend extrusion loops", "[ExtrusionEntity]")
{
    SECTION("Entity types are told apart")
    {
        ExtrusionEntityCollection cleanup;
        ExtrusionEntitiesPtr entities{make_loop(), make_path()};

        REQUIRE(entities[0]->is_loop());
        REQUIRE(! entities[1]->is_loop());

        collect_for_cleanup(cleanup, entities);
    }

    SECTION("Loops are detached, paths are kept, relative order is preserved")
    {
        ExtrusionEntityCollection cleanup;
        ExtrusionPath  *path1 = make_path();
        ExtrusionLoop  *loop1 = make_loop();
        ExtrusionPath  *path2 = make_path();
        ExtrusionLoop  *loop2 = make_loop();
        ExtrusionEntitiesPtr entities{path1, loop1, path2, loop2};

        ExtrusionEntitiesPtr loops;
        detach_extrusion_loops(entities, loops);

        REQUIRE(loops.size() == 2);
        REQUIRE(loops[0] == loop1);
        REQUIRE(loops[1] == loop2);
        REQUIRE(entities.size() == 2);
        REQUIRE(entities[0] == path1);
        REQUIRE(entities[1] == path2);

        collect_for_cleanup(cleanup, entities);
        collect_for_cleanup(cleanup, loops);
    }

    SECTION("Prepend puts the entities back in front and empties the source")
    {
        ExtrusionEntityCollection cleanup;
        ExtrusionPath *path = make_path();
        ExtrusionEntitiesPtr entities{path};

        ExtrusionLoop  *loop1 = make_loop();
        ExtrusionLoop  *loop2 = make_loop();
        ExtrusionEntitiesPtr loops{loop1, loop2};

        prepend_extrusion_entities(entities, std::move(loops));

        REQUIRE(loops.empty());
        REQUIRE(entities.size() == 3);
        REQUIRE(entities[0] == loop1);
        REQUIRE(entities[1] == loop2);
        REQUIRE(entities[2] == path);

        collect_for_cleanup(cleanup, entities);
    }

    SECTION("Lists without loops are left untouched by both helpers")
    {
        ExtrusionEntityCollection cleanup;
        ExtrusionPath *path1 = make_path();
        ExtrusionPath *path2 = make_path();
        ExtrusionEntitiesPtr entities{path1, path2};
        ExtrusionEntitiesPtr snapshot{path1, path2};

        ExtrusionEntitiesPtr loops;
        detach_extrusion_loops(entities, loops);
        REQUIRE(loops.empty());
        REQUIRE(entities.size() == 2);

        prepend_extrusion_entities(entities, std::move(loops));
        REQUIRE(entities == snapshot);

        collect_for_cleanup(cleanup, entities);
    }
}
