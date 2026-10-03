#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/Gizmos/GizmoSelectionObject.hpp"
#include "slic3r/GUI/SequentialPrintClearance.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;

// 2026-09-23 22:30 crash: with a gizmo open (painting / cut / text / boolean / brim ears),
// selecting a second object in the object list made the selection span two objects, so
// Selection::get_object_idx() returned -1. SelectionInfo::on_update then read objects[-1] and
// Raycaster::on_update dereferenced that garbage pointer. The gizmo's object must be "none"
// for any index that does not name exactly one object.

TEST_CASE("A selection spanning several objects gives the gizmo no object", "[GizmoSelection]")
{
    Model model;
    model.add_object();
    model.add_object();
    model.add_object();

    // -1 is what Selection::get_object_idx() returns for a multi-object selection.
    REQUIRE(gizmo_selection_object(model.objects, -1) == nullptr);
    REQUIRE(gizmo_selection_object(model.objects, -2) == nullptr);
}

TEST_CASE("An index past the end of the object list gives the gizmo no object", "[GizmoSelection]")
{
    Model model;
    model.add_object();
    model.add_object();

    REQUIRE(gizmo_selection_object(model.objects, 2) == nullptr);
    REQUIRE(gizmo_selection_object(model.objects, 100) == nullptr);

    Model empty;
    REQUIRE(gizmo_selection_object(empty.objects, 0) == nullptr);
}

TEST_CASE("A single-object selection gives the gizmo that object", "[GizmoSelection]")
{
    Model model;
    ModelObject* first  = model.add_object();
    ModelObject* second = model.add_object();

    REQUIRE(gizmo_selection_object(model.objects, 0) == first);
    REQUIRE(gizmo_selection_object(model.objects, 1) == second);
}

// Orca #16029 / Edge w1-06: SequentialPrintClearance.hpp is header-only. src/slic3r/CMakeLists.txt
// lists it (+1). tests/slic3rutils/CMakeLists.txt is untouched (Edge #117 / #113 / #245 own it).
// Catch2 v2 port of upstream tests/slic3rutils/test_sequential_clearance.cpp.

namespace {

SequentialClearanceInstance sequential_instance(coord_t x, coord_t y, double height)
{
    return {height, BoundingBox(Point(x, y), Point(x + 5, y + 10)),
        Polygon{Point(x, y), Point(x + 5, y), Point(x + 5, y + 10), Point(x, y + 10)}};
}

// The comparator that used to live in GLCanvas3D::update_sequential_clearance. Combining X order
// for overlapping Y ranges with Y order otherwise is not a strict weak ordering.
bool old_spatial_less(const SequentialClearanceInstance& l, const SequentialClearanceInstance& r)
{
    const auto ly1       = l.bounding_box.min.y();
    const auto ly2       = l.bounding_box.max.y();
    const auto ry1       = r.bounding_box.min.y();
    const auto ry2       = r.bounding_box.max.y();
    const auto inter_min = std::max(ly1, ry1);
    const auto inter_max = std::min(ly2, ry2);
    const auto lx        = l.bounding_box.min.x();
    const auto rx        = r.bounding_box.min.x();
    if (inter_max - inter_min > 0)
        return (lx < rx) || ((lx == rx) && (ly1 < ry1));
    return ly1 < ry1;
}

template<typename Iterator, typename Comp>
bool is_strict_weak_ordering(Iterator first, Iterator last, Comp less)
{
    const auto n = static_cast<size_t>(std::distance(first, last));
    std::vector<char> equivalent(n * n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (less(*(first + i), *(first + i)))
            return false;
        for (size_t j = 0; j < n; ++j) {
            const bool ij = less(*(first + i), *(first + j));
            const bool ji = less(*(first + j), *(first + i));
            if (ij && ji)
                return false;
            equivalent[i * n + j] = (!ij && !ji) ? 1 : 0;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            for (size_t k = 0; k < n; ++k) {
                const bool ij = less(*(first + i), *(first + j));
                const bool jk = less(*(first + j), *(first + k));
                const bool ik = less(*(first + i), *(first + k));
                if (ij && jk && !ik)
                    return false;
                if (equivalent[i * n + j] && equivalent[j * n + k] && !equivalent[i * n + k])
                    return false;
            }
        }
    }
    return true;
}

} // namespace

TEST_CASE("Sequential clearance print-order comparator is a strict weak ordering", "[SequentialClearance]")
{
    std::vector<SequentialClearanceInstance> instances{
        sequential_instance(20, 0, 50.), sequential_instance(10, 8, 50.), sequential_instance(0, 16, 50.),
        sequential_instance(5, 8, 50.)};
    for (size_t i = 0; i < instances.size(); ++i)
        instances[i].instance_id = ObjectID(i + 1);
    // Equal print orders (1 and 4 share order 2) plus the chain-overlap layout.
    const std::map<ObjectID, int> print_order{{ObjectID(1), 1}, {ObjectID(2), 2}, {ObjectID(3), 3}, {ObjectID(4), 2}};

    REQUIRE(is_strict_weak_ordering(instances.begin(), instances.end(), SequentialClearancePrintOrderLess{print_order}));
    CHECK_FALSE(is_strict_weak_ordering(instances.begin(), instances.end(), old_spatial_less));
}

TEST_CASE("Sequential clearance handles equal and overlapping items", "[SequentialClearance]")
{
    std::vector<SequentialClearanceInstance> instances{sequential_instance(0, 0, 50.), sequential_instance(0, 0, 50.)};
    instances[0].instance_id = ObjectID(1);
    instances[1].instance_id = ObjectID(2);

    SECTION("Equal print order is stable")
    {
        // 17+ equal arrange_order values: two items would also pass std::sort.
        constexpr int n = 17;
        instances.clear();
        instances.reserve(n);
        std::map<ObjectID, int> print_order;
        for (int i = 0; i < n; ++i) {
            instances.push_back(sequential_instance(coord_t(i * 10), 0, 50.));
            instances.back().instance_id = ObjectID(i + 1);
            print_order.emplace(ObjectID(i + 1), 2);
        }
        sort_sequential_clearance_instances(instances.begin(), instances.end(), print_order);
        for (int i = 0; i < n; ++i)
            REQUIRE(instances[i].instance_id == ObjectID(i + 1));
    }

    SECTION("Identical overlapping hulls warn the earlier copy at rod height")
    {
        const auto warnings = sequential_clearance_height_polygons(instances, 200., 30., 10.);
        REQUIRE(warnings.size() == 1);
        CHECK(warnings.front().first == instances.front().hull_polygon);
        CHECK(warnings.front().second == Approx(10.).margin(1e-6));
    }
}

TEST_CASE("Sequential clearance preserves model print order when objects move", "[SequentialClearance][Regression]")
{
    const coord_t last_y = GENERATE(-20, 20);
    std::vector<SequentialClearanceInstance> instances{sequential_instance(20, 0, 50.), sequential_instance(0, last_y, 50.)};
    const auto warnings = sequential_clearance_height_polygons(instances, 200., 30., 10.);

    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().first == sequential_instance(20, 0, 50.).hull_polygon);
    CHECK(warnings.front().second == Approx(30.).margin(1e-6));
}

TEST_CASE("Sequential clearance handles chains of overlapping Y ranges", "[SequentialClearance][Regression]")
{
    // A overlaps B, B overlaps C, but A does not overlap C. Combining X order
    // for overlaps with Y order otherwise formed the cycle B < A < C < B and
    // crashed std::sort (MSVC debug "invalid comparator").
    std::vector<SequentialClearanceInstance> instances{
        sequential_instance(20, 0, 50.), sequential_instance(10, 8, 50.), sequential_instance(0, 16, 50.)};

    REQUIRE_FALSE(is_strict_weak_ordering(instances.begin(), instances.end(), old_spatial_less));
    CHECK(old_spatial_less(instances[1], instances[0]));
    CHECK(old_spatial_less(instances[0], instances[2]));
    CHECK(old_spatial_less(instances[2], instances[1]));

    const auto warnings = sequential_clearance_height_polygons(instances, 200., 30., 10.);
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0].first == sequential_instance(20, 0, 50.).hull_polygon);
    CHECK(warnings[1].first == sequential_instance(10, 8, 50.).hull_polygon);
    CHECK(warnings[0].second == Approx(10.).margin(1e-6));
    CHECK(warnings[1].second == Approx(10.).margin(1e-6));
}

TEST_CASE("Sequential clearance applies rod height only to positive Y overlap", "[SequentialClearance]")
{
    const coord_t next_y = GENERATE(9, 10, 11);
    std::vector<SequentialClearanceInstance> instances{sequential_instance(0, 0, 50.), sequential_instance(20, next_y, 50.)};
    const auto warnings = sequential_clearance_height_polygons(instances, 200., 30., 10.);

    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().first == instances.front().hull_polygon);
    CHECK(warnings.front().second == Approx(next_y < 10 ? 10. : 30.).margin(1e-6));
}

TEST_CASE("Sequential clearance allows the last object up to printable height", "[SequentialClearance]")
{
    std::vector<SequentialClearanceInstance> instances;
    CHECK(sequential_clearance_height_polygons(instances, 200., 30., 10.).empty());

    instances.push_back(sequential_instance(0, 0, 200.));
    CHECK(sequential_clearance_height_polygons(instances, 200., 30., 10.).empty());

    instances.front().instance_height = 201.;
    const auto warnings = sequential_clearance_height_polygons(instances, 200., 30., 10.);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().second == Approx(200.).margin(1e-6));
}

TEST_CASE("Sequential clearance warns the same rotated copy as print validation", "[SequentialClearance][Regression]")
{
    // slic3rutils does not link fff_print/test_data.cpp, so build the model inline instead of Test::model.
    constexpr double rod_height       = 10.;
    constexpr double lid_height       = 30.;
    constexpr double printable_height = 200.;
    Model            model;
    ModelObject*     object = model.add_object();
    object->name            = "rotated copies";
    object->add_volume(make_cube(10., 10., 50.));
    object->add_instance()->set_offset(Vec3d(150., 80., 0.));
    ModelInstance* rotated = object->add_instance();
    rotated->set_offset(Vec3d(50., 80., 0.));
    rotated->set_rotation(Z, PI / 2.);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"print_sequence", "by object"}, {"extruder_clearance_radius", 20.},
        {"extruder_clearance_height_to_lid", lid_height}, {"extruder_clearance_height_to_rod", rod_height},
        {"printable_height", printable_height}});
    Print print;
    print.auto_assign_extruders(object);
    print.apply(model, config);
    Polygons                               polygons;
    std::vector<std::pair<Polygon, float>> validation_warnings;
    Print::sequential_print_clearance_valid(print, &polygons, &validation_warnings);

    std::map<ObjectID, int> print_order;
    for (const PrintObject* print_object : print.objects())
        for (const PrintInstance& print_instance : print_object->instances())
            print_order.emplace(print_instance.model_instance->id(), print_instance.model_instance->arrange_order);

    std::vector<SequentialClearanceInstance> instances;
    for (size_t i = 0; i < object->instances.size(); ++i) {
        Polygon hull = object->convex_hull_2d(object->instances[i]->get_matrix());
        instances.push_back({object->get_instance_max_z(i), hull.bounding_box(), hull, object->instances[i]->id()});
    }

    // Edge's sequential_print_clearance_valid can pick a different last instance than upstream
    // (Kahn reorder / above-rod Y). Keep the helper aligned with whatever validation reports.
    REQUIRE_FALSE(validation_warnings.empty());
    sort_sequential_clearance_instances(instances.begin(), instances.end(), print_order);
    const auto warnings = sequential_clearance_height_polygons(instances, printable_height, lid_height, rod_height);
    REQUIRE(warnings.size() == validation_warnings.size());
    REQUIRE(warnings.size() == 1);
    const bool warned_first  = validation_warnings.front().first.bounding_box().contains(instances[0].bounding_box.center());
    const bool warned_second = validation_warnings.front().first.bounding_box().contains(instances[1].bounding_box.center());
    CHECK(warned_first != warned_second);
    CHECK(warnings.front().first.bounding_box().contains(warned_first ? instances[0].bounding_box.center() : instances[1].bounding_box.center()));
    CHECK(warnings.front().second == Approx(rod_height).margin(1e-6));
}

TEST_CASE("Sequential clearance keeps copy order when print order is unavailable", "[SequentialClearance]")
{
    std::vector<SequentialClearanceInstance> instances{sequential_instance(20, 0, 50.), sequential_instance(0, 0, 50.)};
    instances[0].instance_id = ObjectID(1);
    instances[1].instance_id = ObjectID(2);
    std::map<ObjectID, int> print_order{{ObjectID(1), 2}, {ObjectID(2), 1}};

    SECTION("No validation order") { print_order.clear(); }
    SECTION("New copy missing from print") { print_order.erase(ObjectID(2)); }
    SECTION("Copy has not been ordered") { print_order[ObjectID(2)] = 0; }
    SECTION("Copy has an invalid order") { print_order[ObjectID(2)] = -1; }

    sort_sequential_clearance_instances(instances.begin(), instances.end(), print_order);
    CHECK(instances[0].instance_id == ObjectID(1));
    CHECK(instances[1].instance_id == ObjectID(2));
}

TEST_CASE("Sequential clearance keeps object order when sorting copies", "[SequentialClearance]")
{
    std::vector<SequentialClearanceInstance> instances(4, sequential_instance(0, 0, 50.));
    for (size_t i = 0; i < instances.size(); ++i)
        instances[i].instance_id = ObjectID(instances.size() - i);
    const std::map<ObjectID, int> print_order{{ObjectID(1), 1}, {ObjectID(2), 2}, {ObjectID(3), 3}, {ObjectID(4), 4}};

    // The two objects were reordered in the model since the last validation.
    sort_sequential_clearance_instances(instances.begin(), instances.begin() + 2, print_order);
    sort_sequential_clearance_instances(instances.begin() + 2, instances.end(), print_order);
    CHECK(instances[0].instance_id == ObjectID(3));
    CHECK(instances[1].instance_id == ObjectID(4));
    CHECK(instances[2].instance_id == ObjectID(1));
    CHECK(instances[3].instance_id == ObjectID(2));
}
