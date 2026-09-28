#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"

#include "test_data.hpp"

using namespace Slic3r;

static inline Slic3r::Point random_point(float LO=-50, float HI=50) 
{
    Vec2f pt = Vec2f(LO, LO) + (Vec2d(rand(), rand()) * (HI-LO) / RAND_MAX).cast<float>();
	return pt.cast<coord_t>();
}

// build a sample extrusion entity collection with random start and end points.
static Slic3r::ExtrusionPath random_path(size_t length = 20, float LO = -50, float HI = 50)
{
    ExtrusionPath t {erPerimeter, 1.0, 1.0, 1.0};
    for (size_t j = 0; j < length; ++ j)
        t.polyline.append(random_point(LO, HI));
    return t;
}

static Slic3r::ExtrusionPaths random_paths(size_t count = 10, size_t length = 20, float LO = -50, float HI = 50)
{
    Slic3r::ExtrusionPaths p;
    for (size_t i = 0; i < count; ++ i)
        p.push_back(random_path(length, LO, HI));
    return p;
}

SCENARIO("ExtrusionEntityCollection: Polygon flattening", "[ExtrusionEntity]") {
    srand(0xDEADBEEF); // consistent seed for test reproducibility.

    // Generate one specific random path set and save it for later comparison
    Slic3r::ExtrusionPaths nosort_path_set = random_paths();

    Slic3r::ExtrusionEntityCollection sub_nosort;
    sub_nosort.append(nosort_path_set);
    sub_nosort.no_sort = true;

    Slic3r::ExtrusionEntityCollection sub_sort;
    sub_sort.no_sort = false;
    sub_sort.append(random_paths());

    GIVEN("A Extrusion Entity Collection with a child that has one child that is marked as no-sort") {
        Slic3r::ExtrusionEntityCollection sample;
        Slic3r::ExtrusionEntityCollection output;

        sample.append(sub_sort);
        sample.append(sub_nosort);
        sample.append(sub_sort);

        WHEN("The EEC is flattened with default options (preserve_order=false)") {
			output = sample.flatten();
            THEN("The output EEC contains no Extrusion Entity Collections") {
                CHECK(std::count_if(output.entities.cbegin(), output.entities.cend(), [=](const ExtrusionEntity* e) {return e->is_collection();}) == 0);
            }
        }
        WHEN("The EEC is flattened with preservation (preserve_order=true)") {
			output = sample.flatten(true);
            THEN("The output EECs contains one EEC.") {
                CHECK(std::count_if(output.entities.cbegin(), output.entities.cend(), [=](const ExtrusionEntity* e) {return e->is_collection();}) == 1);
            }
            AND_THEN("The ordered EEC contains the same order of elements than the original") {
                // find the entity in the collection
                for (auto e : output.entities)
                    if (e->is_collection()) {
                        ExtrusionEntityCollection *temp = dynamic_cast<ExtrusionEntityCollection*>(e);
                        // check each Extrusion path against nosort_path_set to see if the first and last match the same
                        CHECK(nosort_path_set.size() == temp->entities.size());
                        for (size_t i = 0; i < nosort_path_set.size(); ++ i) {
                            CHECK(temp->entities[i]->first_point() == nosort_path_set[i].first_point());
                            CHECK(temp->entities[i]->last_point() == nosort_path_set[i].last_point());
                        }
                    }
            }
        }
    }
}

// Full ExtrusionRole <-> string mapping, shared by the role<->string unit tests.
namespace {
struct RoleStringPair { ExtrusionRole role; const char* str; };
const RoleStringPair kRoleStringMap[] = {
    { erNone,                     "Undefined" },
    { erPerimeter,                "Inner wall" },
    { erExternalPerimeter,        "Outer wall" },
    { erOverhangPerimeter,        "Overhang wall" },
    { erInternalInfill,           "Sparse infill" },
    { erSolidInfill,              "Internal solid infill" },
    { erTopSolidInfill,           "Top surface" },
    { erBottomSurface,            "Bottom surface" },
    { erIroning,                  "Ironing" },
    { erBridgeInfill,             "Bridge" },
    { erInternalBridgeInfill,     "Internal Bridge" },
    { erGapFill,                  "Gap infill" },
    { erSkirt,                    "Skirt" },
    { erBrim,                     "Brim" },
    { erSupportMaterial,          "Support" },
    { erSupportMaterialInterface, "Support interface" },
    { erSupportTransition,        "Support transition" },
    { erWipeTower,                "Prime tower" },
    { erCustom,                   "Custom" },
    { erMixed,                    "Multiple" },
};
} // namespace

TEST_CASE("ExtrusionEntity: role_to_string exact golden", "[ExtrusionEntity]") {
    for (const auto& p : kRoleStringMap) {
        DYNAMIC_SECTION(p.str) {
            REQUIRE(ExtrusionEntity::role_to_string(p.role) == p.str);
        }
    }
}

TEST_CASE("ExtrusionEntity: string_to_role exact golden", "[ExtrusionEntity]") {
    for (const auto& p : kRoleStringMap) {
        DYNAMIC_SECTION(p.str) {
            REQUIRE(ExtrusionEntity::string_to_role(p.str) == p.role);
        }
    }

    // Unknown / malformed strings fall back to erNone (the default branch).
    REQUIRE(ExtrusionEntity::string_to_role("") == erNone);
    REQUIRE(ExtrusionEntity::string_to_role("garbage") == erNone);
    REQUIRE(ExtrusionEntity::string_to_role("inner wall") == erNone); // case-sensitive
}

TEST_CASE("ExtrusionEntity: role↔string full round-trip", "[ExtrusionEntity]") {
    for (const auto& p : kRoleStringMap) {
        DYNAMIC_SECTION(p.str) {
            REQUIRE(ExtrusionEntity::string_to_role(ExtrusionEntity::role_to_string(p.role)) == p.role);
        }
    }
}

// Pin the Skirt/Brim i18n asymmetry bug (source-level, not runtime-observable).
// role_to_string marks "Skirt"/"Brim" via L() for gettext extraction, but
// string_to_role compares against bare string literals — the only two entries
// not wrapped in L(). Because L() is a no-op macro (#define L(s) (s) in
// ExtrusionEntity.cpp), the two directions agree at runtime; the asymmetry only
// affects whether xgettext extracts these strings. This test locks the runtime
// behavior and documents the source bug so it isn't silently "fixed" without a
// corresponding gettext review.
TEST_CASE("ExtrusionEntity: Skirt/Brim i18n asymmetry is pinned", "[ExtrusionEntity]") {
    // Forward direction (role -> string): marks the strings via L().
    REQUIRE(ExtrusionEntity::role_to_string(erSkirt) == "Skirt");
    REQUIRE(ExtrusionEntity::role_to_string(erBrim) == "Brim");
    // Reverse direction (string -> role): compares against the bare literals.
    REQUIRE(ExtrusionEntity::string_to_role("Skirt") == erSkirt);
    REQUIRE(ExtrusionEntity::string_to_role("Brim") == erBrim);
}
