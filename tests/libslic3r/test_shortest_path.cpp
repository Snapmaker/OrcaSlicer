#include <catch2/catch_test_macros.hpp>
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/ShortestPath.hpp"

#include <memory>
#include <vector>

using namespace Slic3r;

// Regression tests for the slicing crash with non-organic tree supports (upstream OrcaSlicer PR #14074,
// commit 454b6c0045). chain_and_reorder_extrusion_entities() used to drop unusable entities with an
// unchecked static_cast<ExtrusionEntityCollection*>(entity)->empty(), which dereferences nullptr entries
// and only inspects the top-level container. The chaining algorithm then calls first_point()/last_point()
// on every surviving entity, and those dereference empty containers of nested entities.
//
// Ownership: entities stored inside an ExtrusionEntityCollection are deleted by its destructor, so
// children are pushed via unique_ptr::release(). Standalone entities passed to the chaining API stay
// owned by unique_ptr in the test body; the API never takes ownership.

namespace {

std::unique_ptr<ExtrusionPath> make_valid_path(const Point& a, const Point& b)
{
    auto path             = std::make_unique<ExtrusionPath>(erSupportMaterial, 1., 0.4f, 0.2f);
    path->polyline.points = {a, b};
    return path;
}

// A path with no points; passed as an EEC child, ownership transfers to the collection.
std::unique_ptr<ExtrusionPath> make_empty_path() { return std::make_unique<ExtrusionPath>(erSupportMaterial, 1., 0.4f, 0.2f); }

// A non-empty collection whose front child is a path with an empty polyline:
// EEC::first_point() == entities.front()->first_point() == polyline.points.front() on an empty vector.
std::unique_ptr<ExtrusionEntityCollection> make_collection_with_empty_front_path()
{
    auto eec = std::make_unique<ExtrusionEntityCollection>();
    eec->entities.push_back(make_empty_path().release());
    eec->entities.push_back(make_valid_path(Point(0, 0), Point(100, 0)).release());
    return eec;
}

// A non-empty collection whose front child is an empty collection:
// EEC::first_point() == entities.front()->first_point() == entities.front() on an empty vector.
std::unique_ptr<ExtrusionEntityCollection> make_collection_with_empty_front_collection()
{
    auto eec = std::make_unique<ExtrusionEntityCollection>();
    eec->entities.push_back(std::make_unique<ExtrusionEntityCollection>().release());
    eec->entities.push_back(make_valid_path(Point(0, 0), Point(100, 0)).release());
    return eec;
}

// A collection with valid front/back children but a degenerate child in the middle. The upstream fix
// only validates the front and back children, so this entity survives chaining.
std::unique_ptr<ExtrusionEntityCollection> make_collection_with_empty_middle_path()
{
    auto eec = std::make_unique<ExtrusionEntityCollection>();
    eec->entities.push_back(make_valid_path(Point(0, 0), Point(100, 0)).release());
    eec->entities.push_back(make_empty_path().release());
    eec->entities.push_back(make_valid_path(Point(0, 200), Point(100, 200)).release());
    return eec;
}

} // namespace

TEST_CASE("chain_and_reorder_extrusion_entities drops nullptr entries without crashing", "[ShortestPath]")
{
    auto                          valid = make_valid_path(Point(0, 0), Point(scale_(10.), scale_(10.)));
    std::vector<ExtrusionEntity*> entities{nullptr, valid.get(), nullptr};
    chain_and_reorder_extrusion_entities(entities);
    REQUIRE(entities.size() == 1);
    CHECK(entities[0] == valid.get());
}

TEST_CASE("chain_and_reorder_extrusion_entities drops a collection whose front child has no endpoints", "[ShortestPath]")
{
    SECTION("front child is a path with an empty polyline")
    {
        auto                          bad   = make_collection_with_empty_front_path();
        auto                          valid = make_valid_path(Point(0, 0), Point(scale_(10.), scale_(10.)));
        std::vector<ExtrusionEntity*> entities{bad.get(), valid.get()};
        chain_and_reorder_extrusion_entities(entities);
        REQUIRE(entities.size() == 1);
        CHECK(entities[0] == valid.get());
    }

    SECTION("front child is an empty collection")
    {
        auto                          bad   = make_collection_with_empty_front_collection();
        auto                          valid = make_valid_path(Point(0, 0), Point(scale_(10.), scale_(10.)));
        std::vector<ExtrusionEntity*> entities{valid.get(), bad.get()};
        chain_and_reorder_extrusion_entities(entities);
        REQUIRE(entities.size() == 1);
        CHECK(entities[0] == valid.get());
    }
}

TEST_CASE("chain_and_reorder_extrusion_entities keeps a collection with degenerate middle children", "[ShortestPath]")
{
    // Documents the semantics of the upstream fix: only the front and back children must provide
    // endpoints, a degenerate child in the middle is not filtered here.
    auto                          collection = make_collection_with_empty_middle_path();
    auto                          valid      = make_valid_path(Point(0, 0), Point(scale_(10.), scale_(10.)));
    std::vector<ExtrusionEntity*> entities{collection.get(), valid.get()};
    chain_and_reorder_extrusion_entities(entities);
    REQUIRE(entities.size() == 2);
    CHECK(entities[0] == collection.get());
    CHECK(entities[1] == valid.get());
}

TEST_CASE("chain_and_reorder_extrusion_entities reorders valid mixed entities", "[ShortestPath]")
{
    auto far   = make_valid_path(Point(scale_(1000.), scale_(1000.)), Point(scale_(1100.), scale_(1000.)));
    auto near_ = make_valid_path(Point(0, 0), Point(scale_(10.), scale_(0.)));
    auto eec   = std::make_unique<ExtrusionEntityCollection>();
    eec->entities.push_back(make_valid_path(Point(scale_(20.), scale_(0.)), Point(scale_(30.), scale_(0.))).release());
    std::vector<ExtrusionEntity*> entities{far.get(), eec.get(), near_.get()};

    Point start_near(0, 0);
    chain_and_reorder_extrusion_entities(entities, &start_near);
    REQUIRE(entities.size() == 3);
    CHECK(entities[0] == near_.get());
}
