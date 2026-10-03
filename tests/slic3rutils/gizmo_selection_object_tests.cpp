#include <catch2/catch.hpp>

#include <algorithm>
#include <utility>
#include <vector>

#include "libslic3r/Model.hpp"
#include "slic3r/GUI/Gizmos/GizmoSelectionObject.hpp"
#include "slic3r/GUI/OpaqueVolumeSort.hpp"

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

// Orca #15884 Stage A: opaque draw order is selected first, then nearest (higher
// eye-space z) first. Equal keys compare equivalent; equal-depth order is
// unspecified (volumes_to_render uses std::sort).
TEST_CASE("Opaque volumes draw selected first, then nearest first", "[OpaqueVolumeSort]")
{
    REQUIRE(opaque_volume_front_to_back_less({true, -100.0}, {false, -1.0}));
    REQUIRE_FALSE(opaque_volume_front_to_back_less({false, -1.0}, {true, -100.0}));

    REQUIRE(opaque_volume_front_to_back_less({false, -1.0}, {false, -10.0}));
    REQUIRE_FALSE(opaque_volume_front_to_back_less({false, -10.0}, {false, -1.0}));
    REQUIRE(opaque_volume_front_to_back_less({true, -1.0}, {true, -10.0}));

    REQUIRE_FALSE(opaque_volume_front_to_back_less({false, -3.0}, {false, -3.0}));
    REQUIRE_FALSE(opaque_volume_front_to_back_less({true, 1.0}, {true, 1.0}));

    std::vector<std::pair<OpaqueVolumeSortKey, int>> items = {
        {{false, -10.0}, 0},
        {{true, -50.0},  1},
        {{false, -1.0},  2},
        {{true, -2.0},   3},
        {{false, -1.0},  4},
    };
    std::stable_sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
        return opaque_volume_front_to_back_less(a.first, b.first);
    });

    REQUIRE(items[0].second == 3);
    REQUIRE(items[1].second == 1);
    REQUIRE(items[2].second == 2);
    REQUIRE(items[3].second == 4);
    REQUIRE(items[4].second == 0);
}
