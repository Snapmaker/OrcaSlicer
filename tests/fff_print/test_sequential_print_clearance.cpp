// Coverage for Print::sequential_print_clearance_valid(), which had none.
//
// Written against the two sequential print fixes: finding a valid print order instead of trusting
// the object list order, and counting only the geometry that reaches above
// extruder_clearance_height_to_rod when deciding what the rod can hit.
//
// Every plate is built from the stock 20mm test cube, scaled and merged, so no new assets are
// needed. Note that Test::init_print() auto-arranges, so each case places the instances itself
// afterwards and re-applies the model: these tests are entirely about relative Y positions.

#include <catch2/catch_test_macros.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Snapmaker U1 clearances: the tightest of the stock profiles, which is what makes these cases
// interesting. Nothing here is printer specific.
constexpr double HEIGHT_TO_ROD = 27.5;

DynamicPrintConfig sequential_config()
{
    // init_print() merges what it is given onto default_print_config(); build the same full config
    // here so the re-apply in place() below hands Print a complete one rather than a fragment.
    DynamicPrintConfig config = Slic3r::Test::default_print_config();
    config.set_key_value("print_sequence",
                         new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));
    config.set_key_value("extruder_clearance_height_to_rod", new ConfigOptionFloat(HEIGHT_TO_ROD));
    config.set_key_value("extruder_clearance_height_to_lid", new ConfigOptionFloat(140));
    config.set_key_value("extruder_clearance_radius",        new ConfigOptionFloat(72.5));
    config.set_key_value("skirt_loops",                      new ConfigOptionInt(0));
    return config;
}

// A box of the given size, minimum corner at the origin.
TriangleMesh box(double sx, double sy, double sz)
{
    TriangleMesh m = Slic3r::Test::mesh(TestMesh::cube_20x20x20);   // spans 0..20 on each axis
    m.scale(Vec3f(static_cast<float>(sx / 20.0), static_cast<float>(sy / 20.0), static_cast<float>(sz / 20.0)));
    return m;
}

// A low slab carrying one tall fin, either at its back edge (+Y) or its front edge (-Y). One mesh,
// so it becomes a single object: the check reasons per object, not per volume.
TriangleMesh slab_with_fin(bool fin_at_back)
{
    const double slab_x = 40, slab_y = 30, slab_z = 2;
    const double fin_x = 8, fin_y = 2, fin_z = 32;

    TriangleMesh slab = box(slab_x, slab_y, slab_z);
    TriangleMesh fin  = box(fin_x, fin_y, fin_z);
    fin.translate(static_cast<float>(slab_x / 2 - fin_x / 2),
                  static_cast<float>(fin_at_back ? slab_y - fin_y : 0.0), 0.0f);
    slab.merge(fin);
    return slab;
}

// Place each instance at the given centre and re-apply, undoing init_print's auto-arrange.
void place(Print &print, Model &model, const DynamicPrintConfig &config, const std::vector<Vec2d> &centres)
{
    REQUIRE(model.objects.size() == centres.size());
    for (size_t i = 0; i < centres.size(); ++i) {
        ModelInstance *inst = model.objects[i]->instances.front();
        Vec3d off = inst->get_offset();
        inst->set_offset(Vec3d(centres[i].x(), centres[i].y(), off.z()));
    }
    print.apply(model, config);
}

// Specifically the height error. The same function also reports "too close to others" for the
// toolhead radius, which is a different rule and must not be confused with this one.
bool reports_too_tall(const Print &print)
{
    return Print::sequential_print_clearance_valid(print).string.find("too tall") != std::string::npos;
}

} // namespace

SCENARIO("Sequential print clearance: a valid order is searched for, not assumed", "[SequentialClearance]")
{
    GIVEN("a tall object listed first and a short one listed second, overlapping in Y")
    {
        // The tall object can only be printed last. When the check validated the object list order
        // verbatim, listing the tall one first produced a false "too tall" error even though
        // printing it second is perfectly valid.
        Print print;
        Model model;
        DynamicPrintConfig config = sequential_config();
        init_print({ box(40, 40, 35), box(40, 40, 10) }, print, model, config);
        place(print, model, config, { Vec2d(60, 130), Vec2d(190, 130) });

        THEN("a valid order is found and no error is reported")
        {
            REQUIRE_FALSE(reports_too_tall(print));
        }
    }
}

SCENARIO("Sequential print clearance: genuine collisions still error", "[SequentialClearance]")
{
    GIVEN("two objects taller than height_to_rod sharing a Y band")
    {
        // Both exceed the rod height and each sits in the other's sweep band, so whichever prints
        // first is in the way of the second. No order can fix that, and the check must still refuse.
        Print print;
        Model model;
        DynamicPrintConfig config = sequential_config();
        init_print({ box(40, 40, 35), box(40, 40, 35) }, print, model, config);
        place(print, model, config, { Vec2d(60, 130), Vec2d(190, 140) });

        THEN("the error is still reported")
        {
            REQUIRE(reports_too_tall(print));
        }
    }
}

SCENARIO("Sequential print clearance: only material above the rod is counted", "[SequentialClearance]")
{
    GIVEN("two slabs whose above-rod fins face away from each other")
    {
        // The footprints are close enough in Y to overlap under the toolhead radius, but the parts
        // that actually reach above the rod are far apart, so the rod passes over nothing.
        Print print;
        Model model;
        DynamicPrintConfig config = sequential_config();
        init_print({ slab_with_fin(true), slab_with_fin(false) }, print, model, config);
        place(print, model, config, { Vec2d(60, 100), Vec2d(190, 60) });

        THEN("no error is reported")
        {
            REQUIRE_FALSE(reports_too_tall(print));
        }
    }

    GIVEN("the same two slabs with their fins facing each other")
    {
        // Mirror image: both above-rod fins now sit inside the other object's sweep band, which is
        // a real collision. This is the case that proves the rule did not simply disable the check.
        Print print;
        Model model;
        DynamicPrintConfig config = sequential_config();
        init_print({ slab_with_fin(false), slab_with_fin(true) }, print, model, config);
        place(print, model, config, { Vec2d(60, 100), Vec2d(190, 60) });

        THEN("the error is still reported")
        {
            REQUIRE(reports_too_tall(print));
        }
    }
}

SCENARIO("Sequential print clearance: short objects are never constrained", "[SequentialClearance]")
{
    GIVEN("several objects all below height_to_rod, packed close together in Y")
    {
        // Nothing reaches the rod, so no ordering constraint exists whatever the layout. Guards
        // against a regression that starts flagging plates which have always been fine.
        Print print;
        Model model;
        DynamicPrintConfig config = sequential_config();
        // Kept far enough apart in X to satisfy the separate toolhead radius rule, which is not
        // what this scenario is about.
        init_print({ box(40, 40, 10), box(40, 40, 12) }, print, model, config);
        place(print, model, config, { Vec2d(60, 125), Vec2d(180, 135) });

        THEN("no error is reported")
        {
            REQUIRE_FALSE(reports_too_tall(print));
        }
    }
}
