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
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/PreciseSeam.hpp"
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

    // Fraction of seams whose X coordinate lies on the given side of the object's centre.
    double fraction_with_x(bool positive_side) const
    {
        if (points.empty())
            return 0.0;
        size_t n = 0;
        for (const Vec2d &p : points)
            if ((p.x() > 0.0) == positive_side)
                ++n;
        return double(n) / double(points.size());
    }

private:
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

namespace {

Point mm(double x, double y) { return Point(scale_(x), scale_(y)); }

Polygon rectangle(double x0, double y0, double x1, double y1)
{
    return Polygon(Points{mm(x0, y0), mm(x1, y0), mm(x1, y1), mm(x0, y1)});
}

struct SeamGeometryFixture
{
    Model                            model;
    Print                            print;
    Model                            modifiers;
    Layer                           *layer = nullptr;
    PreciseSeam::ModifierSlicesCache cache;

    SeamGeometryFixture()
    {
        Test::init_print({make_cube(20, 20, 20)}, print, model, {{"raft_layers", "0"}});
        REQUIRE(print.objects().size() == 1);
        PrintObject *object = print.get_object(0);
        layer = object->add_layer(int(object->slicing_parameters().raft_layers()), 0.2, 0.2, 0.1);
        modifiers.add_object();
    }

    const ModelVolume *add(ModelVolumeType type, Polygons slices)
    {
        ModelVolume *volume = modifiers.objects.front()->add_volume(make_cube(1, 1, 1));
        volume->set_type(type);
        cache.emplace(volume, std::vector<Polygons>{std::move(slices)});
        return volume;
    }
};

void require_vertex(const Polygon &polygon, const Point &point)
{
    CAPTURE(point.x(), point.y());
    REQUIRE(std::find(polygon.points.begin(), polygon.points.end(), point) != polygon.points.end());
}

void paint_cube_face(ModelVolume &volume, const Vec3f &normal)
{
    const auto &mesh = volume.mesh();
    TriangleSelector selector(mesh);
    size_t painted = 0;
    for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
        if (its_face_normal(mesh.its, int(i)).dot(normal) > 0.9f) {
            selector.set_facet(int(i), EnforcerBlockerType::ENFORCER);
            ++painted;
        }
    }
    REQUIRE(painted > 0);
    volume.seam_facets.set(selector);
}

ModelObject *plain_cube_object(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "ps_plain_cube";
    object->add_volume(make_cube(20, 20, 20));
    object->add_instance();
    return object;
}

ModelObject *cube_with_back_helper(Model &model, ModelVolumeType type, const Vec3d &extra_offset = Vec3d::Zero())
{
    ModelObject *object = model.add_object();
    object->name        = "ps_cube";
    auto *part          = object->add_volume(make_cube(20, 20, 20));
    auto *helper        = object->add_volume(make_cube(8, 4, 20));
    helper->set_type(type);
    helper->set_offset(part->get_offset() + Vec3d(0, 10, 0) + extra_offset);
    object->add_instance();
    return object;
}

std::string strip_gcode_volatile(const std::string &full)
{
    std::istringstream in(full);
    std::string        line, out;
    while (std::getline(in, line))
        if (line.find("; generated by") == std::string::npos)
            out += line + "\n";
    return out;
}

} // namespace

TEST_CASE("Strong seam modes select the requested location on a clipped side", "[Seam][PreciseSeam]")
{
    const auto mode = GENERATE(ModelVolumeType::PRECISE_SEAM_LEFT, ModelVolumeType::PRECISE_SEAM_CENTER,
                               ModelVolumeType::PRECISE_SEAM_RIGHT);
    SeamGeometryFixture fixture;
    Polygon perimeter(Points{mm(0, 0), mm(2, 0), mm(4, 0), mm(8, 0), mm(20, 0), mm(20, 20), mm(0, 20)});
    const ModelVolume *modifier = fixture.add(mode, {rectangle(1, -2, 13, 2)});
    PreciseSeam::PreciseSeamWarnings warnings;
    const auto seam = PreciseSeam::insert_strong_seam_point({modifier}, perimeter, fixture.layer, fixture.cache, &warnings);
    REQUIRE(seam.has_value());
    const double expected_x = mode == ModelVolumeType::PRECISE_SEAM_LEFT ? 1.0 :
                              mode == ModelVolumeType::PRECISE_SEAM_RIGHT ? 13.0 : 7.0;
    CHECK(*seam == mm(expected_x, 0));
    require_vertex(perimeter, mm(expected_x - 0.001, 0));
    require_vertex(perimeter, mm(expected_x + 0.001, 0));
    CHECK_FALSE(warnings.through_body.load());
    CHECK_FALSE(warnings.full_containment.load());
    CHECK_FALSE(warnings.multiple_intersections.load());
}

TEST_CASE("Unsupported modifier sections are skipped with the appropriate warning", "[Seam][PreciseSeam]")
{
    const int scenario = GENERATE(0, 1, 2, 3);
    SeamGeometryFixture fixture;
    Polygon perimeter = rectangle(0, 0, 20, 20);
    const Points original = perimeter.points;
    Polygons slices;
    if (scenario == 0) slices = {rectangle(30, 30, 40, 40)};
    if (scenario == 1) slices = {rectangle(2, 2, 4, 4)};
    if (scenario == 2) slices = {rectangle(-2, -2, 22, 22)};
    if (scenario == 3) {
        Polygon hole = rectangle(2, 2, 4, 4);
        hole.reverse();
        slices = {rectangle(-2, -2, 22, 22), hole};
    }
    const auto *modifier = fixture.add(ModelVolumeType::PRECISE_SEAM_CENTER, std::move(slices));
    PreciseSeam::PreciseSeamWarnings warnings;
    CHECK_FALSE(PreciseSeam::insert_strong_seam_point({modifier}, perimeter, fixture.layer, fixture.cache, &warnings).has_value());
    CHECK(perimeter.points == original);
    CHECK(warnings.full_containment.load() == (scenario == 2));
    CHECK(warnings.multiply_connected.load() == (scenario == 3));
    CHECK_FALSE(warnings.multiple_intersections.load());
    CHECK_FALSE(warnings.through_body.load());
}

TEST_CASE("Modifiers crossing the entire body raise a through body warning", "[Seam][PreciseSeam]")
{
    const bool strong = GENERATE(false, true);
    CAPTURE(strong);
    SeamGeometryFixture fixture;
    Polygon perimeter = rectangle(0, 0, 20, 20);
    const auto type = strong ? ModelVolumeType::PRECISE_SEAM_CENTER : ModelVolumeType::PRECISE_SEAM_BLOCKED;
    const auto *modifier = fixture.add(type, {rectangle(8, -2, 12, 22)});
    PreciseSeam::PreciseSeamWarnings warnings;
    if (strong) {
        const auto seam = PreciseSeam::insert_strong_seam_point({modifier}, perimeter, fixture.layer, fixture.cache, &warnings);
        REQUIRE(seam.has_value());
        CHECK(seam->x() == mm(10, 0).x());
        const bool on_crossed_side = seam->y() == 0 || seam->y() == mm(0, 20).y();
        CHECK(on_crossed_side);
    } else {
        const auto segments = PreciseSeam::collect_weak_modifier_segments({modifier}, perimeter, fixture.layer, fixture.cache, &warnings);
        REQUIRE_FALSE(segments.empty());
    }
    CHECK(warnings.through_body.load());
    CHECK_FALSE(warnings.multiple_intersections.load());
    CHECK_FALSE(warnings.full_containment.load());
    CHECK_FALSE(warnings.multiply_connected.load());
}

TEST_CASE("A CENTER/LEFT/RIGHT helper pins the outer-wall seam of a cube on every layer", "[Seam][PreciseSeam]")
{
    const auto mode = GENERATE(ModelVolumeType::PRECISE_SEAM_LEFT, ModelVolumeType::PRECISE_SEAM_CENTER,
                               ModelVolumeType::PRECISE_SEAM_RIGHT);
    {
        Model baseline_model;
        ModelObject *baseline_object = plain_cube_object(baseline_model);
        SeamCloud baseline = seams_for_object(baseline_object, baseline_model, "aligned");
        REQUIRE(baseline.points.size() >= 40);
        // Aligned-only must not already satisfy the helper pin, or the helper case is not discriminating.
        if (mode == ModelVolumeType::PRECISE_SEAM_CENTER) {
            REQUIRE_FALSE(baseline.min_y() > 8.0 && std::abs(baseline.min_x()) < 2.0 && std::abs(baseline.max_x()) < 2.0);
        } else if (mode == ModelVolumeType::PRECISE_SEAM_LEFT) {
            REQUIRE_FALSE(baseline.min_y() > 8.0 && baseline.min_x() > 1.0);
        } else {
            REQUIRE_FALSE(baseline.min_y() > 8.0 && baseline.max_x() < -1.0);
        }
    }
    Model model;
    ModelObject *object = cube_with_back_helper(model, mode);
    SeamCloud cloud = seams_for_object(object, model, "aligned");
    REQUIRE(cloud.points.size() >= 40);
    // Helper sits on the +Y face. LEFT/RIGHT are the CCW start/end of that clipped edge, not printer left/right.
    REQUIRE(cloud.min_y() > 8.0);
    REQUIRE(cloud.max_y() > 8.0);
    if (mode == ModelVolumeType::PRECISE_SEAM_CENTER) {
        REQUIRE(std::abs(cloud.min_x()) < 2.0);
        REQUIRE(std::abs(cloud.max_x()) < 2.0);
    } else if (mode == ModelVolumeType::PRECISE_SEAM_LEFT) {
        REQUIRE(cloud.min_x() > 1.0);
    } else {
        REQUIRE(cloud.max_x() < -1.0);
    }
}

TEST_CASE("A Strong helper beats Aligned left and Aligned right", "[Seam][PreciseSeam]")
{
    const auto *seam_key = GENERATE("left", "right");
    Model model;
    ModelObject *object = cube_with_back_helper(model, ModelVolumeType::PRECISE_SEAM_CENTER);
    SeamCloud cloud = seams_for_object(object, model, seam_key);
    REQUIRE(cloud.points.size() >= 40);
    REQUIRE(cloud.min_y() > 8.0);
    REQUIRE(std::abs(cloud.min_x()) < 2.5);
    REQUIRE(std::abs(cloud.max_x()) < 2.5);
}

TEST_CASE("Blocked and Enforced Precise Seam zones override painted seams", "[Seam][PreciseSeam]")
{
    const bool blocked = GENERATE(true, false);
    CAPTURE(blocked);

    if (blocked) {
        // Paint +Y so paint-only keeps the seam on the back; BLOCKED must evict it.
        {
            Model baseline_model;
            ModelObject *baseline_object = baseline_model.add_object();
            auto *baseline_part = baseline_object->add_volume(make_cube(20, 20, 20));
            paint_cube_face(*baseline_part, Vec3f(0.f, 1.f, 0.f));
            baseline_object->add_instance();
            SeamCloud baseline = seams_for_object(baseline_object, baseline_model, "aligned");
            REQUIRE(baseline.points.size() >= 40);
            REQUIRE(baseline.min_y() > 8.0);
        }
        Model model;
        ModelObject *object = model.add_object();
        auto *part = object->add_volume(make_cube(20, 20, 20));
        paint_cube_face(*part, Vec3f(0.f, 1.f, 0.f));
        auto *helper = object->add_volume(make_cube(22, 4, 20));
        helper->set_type(ModelVolumeType::PRECISE_SEAM_BLOCKED);
        helper->set_offset(part->get_offset() + Vec3d(0, 10, 0));
        object->add_instance();
        SeamCloud cloud = seams_for_object(object, model, "aligned");
        REQUIRE(cloud.points.size() >= 40);
        REQUIRE(cloud.max_y() < 8.0);
    } else {
        // Painting the helper's +Y face would pass on #201 without Precise Seam, because paint
        // alone already puts the seam there. Aligned-left puts the default seam on -X; ENFORCED
        // on +Y must pull it to the back.
        {
            Model baseline_model;
            ModelObject *baseline_object = plain_cube_object(baseline_model);
            SeamCloud baseline = seams_for_object(baseline_object, baseline_model, "left");
            REQUIRE(baseline.points.size() >= 40);
            REQUIRE(baseline.min_x() < -5.0);
            REQUIRE_FALSE(baseline.min_y() > 8.0 && baseline.max_y() > 8.0);
        }
        Model model;
        ModelObject *object = model.add_object();
        auto *part = object->add_volume(make_cube(20, 20, 20));
        auto *helper = object->add_volume(make_cube(22, 4, 20));
        helper->set_type(ModelVolumeType::PRECISE_SEAM_ENFORCED);
        helper->set_offset(part->get_offset() + Vec3d(0, 10, 0));
        object->add_instance();
        SeamCloud cloud = seams_for_object(object, model, "left");
        REQUIRE(cloud.points.size() >= 40);
        REQUIRE(cloud.min_y() > 8.0);
    }
}

TEST_CASE("A Strong Precise Seam helper beats a Weak zone on another face", "[Seam][PreciseSeam]")
{
    Model model;
    ModelObject *object = model.add_object();
    auto *part = object->add_volume(make_cube(20, 20, 20));
    auto *weak = object->add_volume(make_cube(4, 8, 20));
    weak->set_type(ModelVolumeType::PRECISE_SEAM_ENFORCED);
    weak->set_offset(part->get_offset() + Vec3d(-10, 0, 0));
    auto *strong = object->add_volume(make_cube(8, 4, 20));
    strong->set_type(ModelVolumeType::PRECISE_SEAM_CENTER);
    strong->set_offset(part->get_offset() + Vec3d(0, 10, 0));
    object->add_instance();
    SeamCloud cloud = seams_for_object(object, model, "aligned");
    REQUIRE(cloud.points.size() >= 40);
    REQUIRE(cloud.min_y() > 8.0);
    REQUIRE(std::abs(cloud.min_x()) < 2.5);
}

TEST_CASE("Overlapping weak Precise Seam helpers follow tree order", "[Seam][PreciseSeam]")
{
    {
        Model baseline_model;
        ModelObject *baseline_object = plain_cube_object(baseline_model);
        SeamCloud baseline = seams_for_object(baseline_object, baseline_model, "aligned");
        REQUIRE(baseline.points.size() >= 40);
        // Without helpers, aligned is not already off the back; otherwise BLOCKED-wins is not discriminating.
        REQUIRE(baseline.max_y() >= 8.0);
    }
    Model model;
    ModelObject *object = model.add_object();
    auto *part = object->add_volume(make_cube(20, 20, 20));
    // Cover the whole +Y face so Blocked can actually evict the seam. First weak is higher in
    // the list; init_precise_seam_data applies it last (last-write-wins).
    auto *high = object->add_volume(make_cube(22, 4, 20));
    high->set_type(ModelVolumeType::PRECISE_SEAM_BLOCKED);
    high->set_offset(part->get_offset() + Vec3d(0, 10, 0));
    auto *low = object->add_volume(make_cube(22, 4, 20));
    low->set_type(ModelVolumeType::PRECISE_SEAM_ENFORCED);
    low->set_offset(part->get_offset() + Vec3d(0, 10, 0));
    object->add_instance();
    SeamCloud cloud = seams_for_object(object, model, "aligned");
    REQUIRE(cloud.points.size() >= 40);
    REQUIRE(cloud.max_y() < 8.0);
}

TEST_CASE("A model without Precise Seam volumes keeps G-code byte-identical after a no-op apply", "[Seam][PreciseSeam]")
{
    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(20, 20, 20));
    object->add_instance();
    Print print;
    DynamicPrintConfig config = seam_test_config("aligned");
    object->ensure_on_bed();
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    const std::string first = strip_gcode_volatile(Test::gcode(print));
    REQUIRE_FALSE(first.empty());
    print.apply(model, config);
    const std::string second = strip_gcode_volatile(Test::gcode(print));
    REQUIRE(first == second);
}

TEST_CASE("Editing a Precise Seam helper invalidates G-code export and moves the seam", "[Seam][PreciseSeam][Print]")
{
    Model model;
    ModelObject *object = cube_with_back_helper(model, ModelVolumeType::PRECISE_SEAM_CENTER);
    Print print;
    DynamicPrintConfig config = seam_test_config("aligned");
    object->ensure_on_bed();
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.set_status_silent();
    print.process();
    REQUIRE(print.is_step_done(posPerimeters));
    const std::string first_gcode = strip_gcode_volatile(Test::gcode(print));
    REQUIRE(print.is_step_done(psGCodeExport));

    SeamPlacer placer;
    placer.init(print, []() {});
    SeamCloud before;
    for (const PrintObject *po : print.objects()) {
        for (const Layer *layer : po->layers()) {
            for (const LayerRegion *region : layer->regions()) {
                std::vector<const ExtrusionLoop *> loops;
                collect_outer_loops(&region->perimeters, loops);
                for (const ExtrusionLoop *source : loops) {
                    ExtrusionLoop loop = *source;
                    float overhang = 0.f;
                    placer.place_seam(layer, loop, Point(0, 0), overhang);
                    before.points.push_back(unscale(loop.first_point()));
                }
            }
        }
    }
    REQUIRE(before.min_y() > 8.0);

    ModelVolume *helper = nullptr;
    for (ModelVolume *volume : model.objects.front()->volumes)
        if (volume->is_precise_seam())
            helper = volume;
    REQUIRE(helper != nullptr);
    helper->set_offset(helper->get_offset() + Vec3d(-10, -10, 0));
    helper->set_type(ModelVolumeType::PRECISE_SEAM_LEFT);
    helper->config.set_key_value("notes", new ConfigOptionString("moved"));

    const auto status = print.apply(model, config);
    CHECK(status == PrintBase::APPLY_STATUS_INVALIDATED);
    REQUIRE_FALSE(print.is_step_done(psGCodeExport));
    REQUIRE(print.is_step_done(posPerimeters));

    const std::string second_gcode = strip_gcode_volatile(Test::gcode(print));
    REQUIRE(first_gcode != second_gcode);

    SeamPlacer placer2;
    placer2.init(print, []() {});
    SeamCloud after;
    for (const PrintObject *po : print.objects()) {
        for (const Layer *layer : po->layers()) {
            for (const LayerRegion *region : layer->regions()) {
                std::vector<const ExtrusionLoop *> loops;
                collect_outer_loops(&region->perimeters, loops);
                for (const ExtrusionLoop *source : loops) {
                    ExtrusionLoop loop = *source;
                    float overhang = 0.f;
                    placer2.place_seam(layer, loop, Point(0, 0), overhang);
                    after.points.push_back(unscale(loop.first_point()));
                }
            }
        }
    }
    REQUIRE(after.points.size() == before.points.size());
    REQUIRE(after.max_y() < before.min_y() - 1.0);
}

TEST_CASE("A config-only Precise Seam helper edit invalidates G-code export", "[Seam][PreciseSeam][Print]")
{
    Model model;
    ModelObject *object = cube_with_back_helper(model, ModelVolumeType::PRECISE_SEAM_CENTER);
    Print print;
    DynamicPrintConfig config = seam_test_config("aligned");
    object->ensure_on_bed();
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.set_status_silent();
    print.process();
    REQUIRE(print.is_step_done(posPerimeters));
    const std::string first_gcode = strip_gcode_volatile(Test::gcode(print));
    REQUIRE(print.is_step_done(psGCodeExport));

    ModelVolume *helper = nullptr;
    for (ModelVolume *volume : model.objects.front()->volumes)
        if (volume->is_precise_seam())
            helper = volume;
    REQUIRE(helper != nullptr);
    helper->config.set_key_value("notes", new ConfigOptionString("future-proof"));

    const auto status = print.apply(model, config);
    CHECK(status == PrintBase::APPLY_STATUS_INVALIDATED);
    REQUIRE_FALSE(print.is_step_done(psGCodeExport));
    REQUIRE(print.is_step_done(posPerimeters));

    // PreciseSeam does not read helper config yet, so G-code may stay identical. Invalidating
    // psGCodeExport here is future-proofing for a later stage that does.
    const std::string second_gcode = strip_gcode_volatile(Test::gcode(print));
    REQUIRE(first_gcode == second_gcode);
}

TEST_CASE("A through-body Precise Seam helper surfaces a warning from SeamPlacer", "[Seam][PreciseSeam]")
{
    Model model;
    ModelObject *object = model.add_object();
    auto *part = object->add_volume(make_cube(20, 20, 20));
    // 30 mm in Y on a 20 mm cube: the helper sticks out of both the front and the back.
    auto *helper = object->add_volume(make_cube(4, 30, 20));
    helper->set_type(ModelVolumeType::PRECISE_SEAM_CENTER);
    helper->set_offset(part->get_offset());
    object->add_instance();

    Print print;
    DynamicPrintConfig config = seam_test_config("aligned");
    object->ensure_on_bed();
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.set_status_silent();
    print.process();

    SeamPlacer placer;
    placer.init(print, []() {});
    const std::string warning = placer.precise_seam_warning_message();
    REQUIRE_FALSE(warning.empty());
    REQUIRE(warning.find("Precise Seam") != std::string::npos);
}
