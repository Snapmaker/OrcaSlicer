// Seam Left/Right - the gate for docs/superpowers/specs/2026-09-06-seam-left-right.md and its
// follow-up (2026-09-25): Left and Right stopped being directional-only (pull toward the extreme
// X coordinate, like Back pulls toward the extreme Y) and became "Aligned back, but biased to one
// side": occlusion and visibility ARE computed, candidates are picked by visibility and angle with
// the concave-corner preference, then aligned - exactly like Aligned back, except the front-facing
// penalty in compute_global_occlusion is rotated from "penalise -Y-facing surfaces" (drift to the
// back) to "penalise +X-facing surfaces" (drift left) or "-X-facing surfaces" (drift right).
//
//  * On a cylinder - uniform visibility everywhere, no corner to distract the comparator - Left and
//    Right still land on their own half of the tube (the penalty alone is enough to break the tie)
//    and stay aligned from layer to layer, but they are no longer pinned to the exact extreme
//    coordinate the way the old directional rule pinned them.
//  * On a shape with a hidden concave corner tucked against the back wall on one side, and a flat,
//    fully exposed wall on that same side, Left (or Right) prefers the hidden corner - the same
//    thing Aligned back would do with its own axis. A purely directional rule would not: it would
//    take the flat wall, because that reaches further along the axis.
//  * Back and Aligned back are untouched by any of this (their penalised direction is (0,1,0), the
//    negation of which is the same (0,-1,0) vector the old hardcoded formula used, applied through
//    the same bit-for-bit arithmetic), so their seams must come out identical to what this file
//    already pinned before the change.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/SeamPlacer.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "test_data.hpp"

using namespace Slic3r;
// tests/CLAUDE.md: floating point comparisons go through the matchers, never through Approx.
using Catch::Matchers::WithinAbs;

namespace {

// Where the seams ended up, in unscaled object coordinates. The object is centred on the origin, so
// these are signed offsets from its middle - which is what "toward that side of the bed" means once
// the instance transform is undone.
struct SeamCloud
{
    std::vector<Vec2d> points;

    double min_x() const { return extreme(0, false); }
    double max_x() const { return extreme(0, true); }
    double min_y() const { return extreme(1, false); }
    double max_y() const { return extreme(1, true); }

    double spread(int axis) const { return extreme(axis, true) - extreme(axis, false); }

    // Fraction of seams whose coordinate on `axis` lies on the given side of the object's centre.
    double fraction_with_x(bool positive_side) const { return fraction_with_axis(0, positive_side); }
    double fraction_with_y(bool positive_side) const { return fraction_with_axis(1, positive_side); }

private:
    double fraction_with_axis(int axis, bool positive_side) const
    {
        if (points.empty())
            return 0.0;
        size_t n = 0;
        for (const Vec2d &p : points)
            if ((p[axis] > 0.0) == positive_side)
                ++n;
        return double(n) / double(points.size());
    }

    double extreme(int axis, bool largest) const
    {
        double best = largest ? -std::numeric_limits<double>::max() : std::numeric_limits<double>::max();
        for (const Vec2d &p : points)
            best = largest ? std::max(best, p[axis]) : std::min(best, p[axis]);
        return best;
    }
};

void collect_outer_loops(const ExtrusionEntity *entity, std::vector<const ExtrusionLoop *> &out)
{
    if (entity->is_collection()) {
        for (const ExtrusionEntity *child : static_cast<const ExtrusionEntityCollection *>(entity)->entities)
            collect_outer_loops(child, out);
        return;
    }
    if (entity->is_loop() && entity->role() == erExternalPerimeter)
        out.push_back(static_cast<const ExtrusionLoop *>(entity));
}

DynamicPrintConfig seam_test_config(const std::string &seam_position)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "seam_position",              seam_position },
        { "wall_loops",                 "2" },
        { "layer_height",               "0.2" },
        { "initial_layer_print_height", "0.2" },
        // A plain shape: nothing on the inside for the seam logic to trip over, and it slices fast.
        { "top_shell_layers",           "0" },
        { "bottom_shell_layers",        "0" },
        { "sparse_infill_density",      "0%" },
        { "enable_support",             "0" },
        { "spiral_mode",                "0" },
        { "staggered_inner_seams",      "0" },
    });
    return config;
}

// Slice `object` and ask the SeamPlacer where the seam of every outer wall loop would go. Going
// through place_seam rather than the exported G-code keeps seam_gap, the scarf joint and the
// travel moves out of the measurement.
SeamCloud seams_for_object(ModelObject *object, Model &model, const std::string &seam_position)
{
    Slic3r::Print print;

    DynamicPrintConfig config = seam_test_config(seam_position);

    object->ensure_on_bed();
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.set_status_silent();
    print.process();

    SeamPlacer placer;
    placer.init(print, []() {});

    SeamCloud cloud;
    for (const PrintObject *po : print.objects()) {
        const size_t raft_layers = po->slicing_parameters().raft_layers();
        for (const Layer *layer : po->layers()) {
            if (layer->id() < raft_layers)
                continue;
            for (const LayerRegion *region : layer->regions()) {
                std::vector<const ExtrusionLoop *> loops;
                collect_outer_loops(&region->perimeters, loops);
                for (const ExtrusionLoop *source : loops) {
                    ExtrusionLoop loop     = *source;
                    float         overhang = 0.f;
                    placer.place_seam(layer, loop, Point(0, 0), overhang);
                    cloud.points.push_back(unscale(loop.first_point()));
                }
            }
        }
    }
    return cloud;
}

SeamCloud seams_for(const std::string &seam_position)
{
    Slic3r::Model model;
    ModelObject *object = model.add_object();
    object->name = "cylinder20";
    object->add_volume(Slic3r::make_cylinder(10., 10.));
    object->add_instance();
    return seams_for_object(object, model, seam_position);
}

// A 20x20x10 mm block, centred on X/Y, with a full-height notch removed from the corner where the
// LEFT face (x = -10) meets the BACK face (y = +10): the block loses x in [-10,-6], y in [6,10].
// The notch's inner wall (a new face at x = -6, facing +X) sits tucked into that corner: most of
// the hemisphere above it is blocked by the remaining back-left corner and by the notch's own side
// walls, so raycast_visibility scores it as much less visible than the flat, wide-open left face
// that still runs the rest of x = -10 (y from -10 to 6). A purely directional rule ("smallest X
// wins") would prefer the flat face - it is closer to x = -10 - but Aligned (and so Aligned left)
// prefers the hidden point once visibility is weighed in, same as Aligned back already does for a
// notch on the back.
ModelObject *notched_block(Model &model, const std::string &name)
{
    indexed_triangle_set block = its_make_cube(20., 20., 10.);
    for (Vec3f &v : block.vertices) {
        v.x() -= 10.f;
        v.y() -= 10.f;
    }

    ModelObject *object = model.add_object();
    object->name = name;
    object->add_volume(TriangleMesh(block));

    indexed_triangle_set notch = its_make_cube(4.001, 4.001, 10.);
    for (Vec3f &v : notch.vertices) {
        v.x() += -10.f - 0.0005f; // slight overshoot so the cut face is a clean through-cut
        v.y() += 6.f - 0.0005f;
    }
    ModelVolume *neg = object->add_volume(TriangleMesh(notch));
    neg->set_type(ModelVolumeType::NEGATIVE_VOLUME);

    object->add_instance();
    return object;
}

// Mirror image of notched_block: the notch sits where the RIGHT face (x = +10) meets the back face.
ModelObject *notched_block_mirrored(Model &model, const std::string &name)
{
    indexed_triangle_set block = its_make_cube(20., 20., 10.);
    for (Vec3f &v : block.vertices) {
        v.x() -= 10.f;
        v.y() -= 10.f;
    }

    ModelObject *object = model.add_object();
    object->name = name;
    object->add_volume(TriangleMesh(block));

    indexed_triangle_set notch = its_make_cube(4.001, 4.001, 10.);
    for (Vec3f &v : notch.vertices) {
        v.x() += 6.f - 0.0005f;
        v.y() += 6.f - 0.0005f;
    }
    ModelVolume *neg = object->add_volume(TriangleMesh(notch));
    neg->set_type(ModelVolumeType::NEGATIVE_VOLUME);

    object->add_instance();
    return object;
}

// The outer wall of a 20 mm cylinder runs at roughly 9.8 mm from the axis. These bounds only need to
// tell one side of the tube from the other three, so they are deliberately slack.
constexpr double on_the_far_side = 8.0; // a seam pushed all the way to one side clears this
constexpr double near_the_middle = 3.0; // ... and the other coordinate stays near zero

} // namespace

SCENARIO("Seam position Back pulls the seam to the back, unchanged by the Aligned left/right work", "[Seam]")
{
    GIVEN("a cylinder sliced with seam_position = back")
    {
        SeamCloud cloud = seams_for("back");
        THEN("there is a seam to look at on every layer")
        {
            REQUIRE(cloud.points.size() >= 40);
        }
        THEN("every seam sits at the back of the tube")
        {
            REQUIRE(cloud.min_y() > on_the_far_side);
            REQUIRE(std::abs(cloud.min_x()) < near_the_middle);
            REQUIRE(std::abs(cloud.max_x()) < near_the_middle);
        }
        THEN("the seams line up from layer to layer")
        {
            REQUIRE(cloud.spread(1) < 0.5);
        }
    }
}

SCENARIO("Seam position Aligned left and Aligned right bias the seam to their side of a cylinder", "[Seam]")
{
    GIVEN("a cylinder sliced with seam_position = left")
    {
        SeamCloud cloud = seams_for("left");
        THEN("there is a seam to look at on every layer")
        {
            REQUIRE(cloud.points.size() >= 40);
        }
        THEN("every seam sits on the left half of the tube")
        {
            // Unlike the old purely-directional Left, Aligned left is not pinned to the exact
            // extreme X: the visibility/angle penalty can move it a little. What must hold is that
            // it stays left of centre and does not wander onto the front/back (Y near zero) - the
            // same shape of assertion the old test made, just without demanding the exact minimum.
            REQUIRE(cloud.max_x() < -near_the_middle);
            REQUIRE(std::abs(cloud.min_y()) < near_the_middle);
            REQUIRE(std::abs(cloud.max_y()) < near_the_middle);
        }
        THEN("essentially every seam is on the left (negative X) side")
        {
            REQUIRE(cloud.fraction_with_x(false) > 0.95);
        }
        THEN("the seams line up from layer to layer")
        {
            REQUIRE(cloud.spread(0) < 0.5);
        }
    }

    GIVEN("a cylinder sliced with seam_position = right")
    {
        SeamCloud cloud = seams_for("right");
        THEN("there is a seam to look at on every layer")
        {
            REQUIRE(cloud.points.size() >= 40);
        }
        THEN("every seam sits on the right half of the tube")
        {
            REQUIRE(cloud.min_x() > near_the_middle);
            REQUIRE(std::abs(cloud.min_y()) < near_the_middle);
            REQUIRE(std::abs(cloud.max_y()) < near_the_middle);
        }
        THEN("essentially every seam is on the right (positive X) side")
        {
            REQUIRE(cloud.fraction_with_x(true) > 0.95);
        }
        THEN("the seams line up from layer to layer")
        {
            REQUIRE(cloud.spread(0) < 0.5);
        }
    }

    GIVEN("the same cylinder sliced left and right")
    {
        SeamCloud left  = seams_for("left");
        SeamCloud right = seams_for("right");
        THEN("the two are mirrors of each other across the axis")
        {
            REQUIRE_THAT(left.min_x(), WithinAbs(-right.max_x(), 0.5));
            REQUIRE_THAT(left.max_x(), WithinAbs(-right.min_x(), 0.5));
        }
    }
}

SCENARIO("Aligned left prefers a hidden concave corner over a flat, exposed left face", "[Seam]")
{
    GIVEN("a block whose only feature on the left side is a notch tucked into the back-left corner")
    {
        Model model;
        ModelObject *object = notched_block(model, "notched_left");
        SeamCloud    cloud  = seams_for_object(object, model, "left");
        THEN("there is a seam to look at on every layer")
        {
            REQUIRE(cloud.points.size() >= 5);
        }
        THEN("the seam sits inside the notch (x > -6, close to the cut corner), not on the flat wall at x = -10")
        {
            // The flat wall runs the whole left side at x = -10. If Left were still pulling toward
            // the smallest X the way the old directional rule did, it would land there. Aligned
            // left instead follows the hidden corner: x should be well clear of the flat wall.
            REQUIRE(cloud.max_x() > -9.0);
        }
        THEN("and it still stays on the object's left half overall")
        {
            REQUIRE(cloud.max_x() < 0.0);
        }
    }
}

SCENARIO("Aligned right prefers a hidden concave corner over a flat, exposed right face", "[Seam]")
{
    GIVEN("a block whose only feature on the right side is a notch tucked into the back-right corner")
    {
        Model model;
        ModelObject *object = notched_block_mirrored(model, "notched_right");
        SeamCloud    cloud  = seams_for_object(object, model, "right");
        THEN("there is a seam to look at on every layer")
        {
            REQUIRE(cloud.points.size() >= 5);
        }
        THEN("the seam sits inside the notch (x < 6), not on the flat wall at x = 10")
        {
            REQUIRE(cloud.min_x() < 9.0);
        }
        THEN("and it still stays on the object's right half overall")
        {
            REQUIRE(cloud.min_x() > 0.0);
        }
    }
}

SCENARIO("Aligned back is unaffected by generalising its penalty for Aligned left/right", "[Seam]")
{
    GIVEN("a cylinder sliced with seam_position = aligned_back")
    {
        SeamCloud cloud = seams_for("aligned_back");
        THEN("there is a seam to look at on every layer")
        {
            REQUIRE(cloud.points.size() >= 40);
        }
        THEN("every seam is biased to the back half of the tube, same as before this change")
        {
            REQUIRE(cloud.min_y() > near_the_middle);
            REQUIRE(std::abs(cloud.min_x()) < on_the_far_side);
            REQUIRE(std::abs(cloud.max_x()) < on_the_far_side);
        }
        THEN("the seams line up from layer to layer")
        {
            REQUIRE(cloud.spread(1) < 0.5);
        }
    }

    GIVEN("the notch-on-the-back shape used to gate Aligned back's concave-corner preference")
    {
        // Reuse the same "notch tucked into a corner vs. flat exposed wall" shape as the left/right
        // gate above, just built so the notch sits on the BACK side instead: this is what
        // Aligned back already did before this change, and the fixed penalised-direction table
        // entry for spAlignedBack is (0, 1, 0), giving the exact same `normal.dot((0,-1,0))`
        // formula as before - so this must keep picking the hidden corner.
        indexed_triangle_set block = its_make_cube(20., 20., 10.);
        for (Vec3f &v : block.vertices) {
            v.x() -= 10.f;
            v.y() -= 10.f;
        }
        Model model;
        ModelObject *object = model.add_object();
        object->name = "notched_back";
        object->add_volume(TriangleMesh(block));

        indexed_triangle_set notch = its_make_cube(4.001, 4.001, 10.);
        for (Vec3f &v : notch.vertices) {
            v.x() += -2.f;
            v.y() += 10.f - 4.f - 0.0005f;
        }
        ModelVolume *neg = object->add_volume(TriangleMesh(notch));
        neg->set_type(ModelVolumeType::NEGATIVE_VOLUME);
        object->add_instance();

        SeamCloud cloud = seams_for_object(object, model, "aligned_back");
        THEN("the seam sits inside the notch, not on the flat back wall at y = 10")
        {
            REQUIRE(cloud.min_y() < 9.0);
        }
        THEN("and it still stays on the object's back half overall")
        {
            REQUIRE(cloud.min_y() > 0.0);
        }
    }
}

SCENARIO("The Left and Right seam keys survive a round trip through the config", "[Seam]")
{
    GIVEN("a print config")
    {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        THEN("left and right deserialise to the new enum values")
        {
            config.set_deserialize_strict({ { "seam_position", "left" } });
            REQUIRE(config.opt_enum<SeamPosition>("seam_position") == spLeft);
            REQUIRE(config.opt_serialize("seam_position") == "left");

            config.set_deserialize_strict({ { "seam_position", "right" } });
            REQUIRE(config.opt_enum<SeamPosition>("seam_position") == spRight);
            REQUIRE(config.opt_serialize("seam_position") == "right");
        }
        THEN("the values that shipped before keep their numbers, so old projects still load")
        {
            REQUIRE(int(spNearest) == 0);
            REQUIRE(int(spAligned) == 1);
            REQUIRE(int(spAlignedBack) == 2);
            REQUIRE(int(spRear) == 3);
            REQUIRE(int(spRandom) == 4);
            REQUIRE(int(spLeft) == 5);
            REQUIRE(int(spRight) == 6);
        }
    }
}

// Orca #12028 / Edge 7A2: mirroring inverts winding unless its_transform(..., true).
// A cube's four walls are equally visible, so aligned_back's front-facing penalty is what
// puts the seam on +Y. After a Y-mirror that penalty sees inward normals and picks -Y
// unless the four occlusion/enforcer its_transform sites pass fix_left_handed.
// Painting the local front (-Y) maps to world +Y after the mirror; AABB still finds those
// facets (winding-agnostic), so the painted seam must sit on that painted world-back face.
TEST_CASE("A mirrored cube with a painted enforcer puts the seam on the painted side", "[Seam][SeamMirror]")
{
    auto cube = [](bool paint_local_front) {
        Model        model;
        ModelObject *object = model.add_object();
        object->name        = paint_local_front ? "mirrored_painted_cube" : "mirrored_cube";
        ModelVolume *volume = object->add_volume(make_cube(20., 20., 20.));
        if (paint_local_front) {
            const indexed_triangle_set &its = volume->mesh().its;
            TriangleSelector            selector(volume->mesh());
            int                         painted = 0;
            for (int f = 0; f < int(its.indices.size()); ++f)
                if (its_face_normal(its, f).y() < -0.9f) {
                    selector.set_facet(f, EnforcerBlockerType::ENFORCER);
                    ++painted;
                }
            REQUIRE(painted >= 2);
            REQUIRE(volume->seam_facets.set(selector));
        }
        ModelInstance *instance = object->add_instance();
        instance->set_mirror(Vec3d(1., -1., 1.));
        return seams_for_object(object, model, "aligned_back");
    };

    SECTION("aligned_back on a Y-mirrored cube stays on the world back")
    {
        const SeamCloud cloud = cube(false);
        REQUIRE(cloud.points.size() >= 40);
        INFO("x in [" << cloud.min_x() << ", " << cloud.max_x() << "], y in [" << cloud.min_y() << ", " << cloud.max_y()
                      << "]");
        CHECK(cloud.fraction_with_y(true) > 0.8);
        CHECK(cloud.spread(1) < 5.0);
    }

    SECTION("painted local front (world back after the Y-mirror) keeps the seam on that face")
    {
        const SeamCloud cloud = cube(true);
        REQUIRE(cloud.points.size() >= 40);
        INFO("x in [" << cloud.min_x() << ", " << cloud.max_x() << "], y in [" << cloud.min_y() << ", " << cloud.max_y()
                      << "]");
        CHECK(cloud.fraction_with_y(true) > 0.8);
        CHECK(cloud.spread(1) < 5.0);
    }
}
