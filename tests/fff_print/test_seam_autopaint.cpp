// Auto-paint seam (seam painting gizmo).
//
// SeamPlacer::plan_object_seams() says where the slicer would put the seam of every outer wall for a given seam
// position and joint preference, without touching the object's settings; SeamAutoPaint::paint() turns those seams
// into painted enforcer strips. These cases hold both to it:
//
//  * the planned seams are exactly the seams the slicer places for that mode (compared with SeamPlacer::init +
//    place_seam on a print configured with the mode), whatever mode the print itself is configured with;
//  * Aligned front / back / left / right put the planned seams on their side of a cylinder;
//  * the joint preference puts them on the contact line of two touching parts, and can be switched either way;
//  * painting marks enforcer triangles along the seams and nowhere else, on every part a joint seam touches, and
//    in the right place when the instance is rotated and scaled and the part is offset;
//  * re-slicing the painted object puts the seam on the painted strip even with the opposite seam position;
//  * "aligned_front" is a seam_position value of its own.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/SeamAutoPaint.hpp"
#include "libslic3r/GCode/SeamPlacer.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "test_data.hpp"

using namespace Slic3r;

namespace {

using Planned = std::vector<std::vector<SeamPlacer::PlannedSeam>>;

DynamicPrintConfig autopaint_config(const std::string &seam_position, bool prefer_joints = true)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "seam_position",              seam_position },
        { "seam_prefer_part_joints",    prefer_joints ? "1" : "0" },
        { "wall_loops",                 "2" },
        { "layer_height",               "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "top_shell_layers",           "0" },
        { "bottom_shell_layers",        "0" },
        { "sparse_infill_density",      "0%" },
        { "enable_support",             "0" },
        { "spiral_mode",                "0" },
        { "staggered_inner_seams",      "0" },
        { "brim_type",                  "no_brim" },
        { "skirt_loops",                "0" },
    });
    return config;
}

using ModelBuilder = std::function<void(Model &)>;

// Only slicing and walls: that is all the planner needs, and what the gizmo computes when the plate is not sliced.
void slice_walls(Print &print, Model &model, const DynamicPrintConfig &config)
{
    for (ModelObject *object : model.objects) {
        object->ensure_on_bed();
        print.auto_assign_extruders(object);
    }
    print.apply(model, config);
    print.set_status_silent();
    for (size_t i = 0; i < print.objects().size(); ++i)
        print.process_perimeters_only(*print.get_object(i));
}

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

// Where the slicer itself splits every outer wall loop of the first object (SeamPlacer::init + place_seam, the
// calls G-code export makes), per layer.
std::vector<std::vector<Vec2d>> placed_seams(const Print &print)
{
    SeamPlacer placer;
    placer.init(print, []() {});
    const PrintObject              *po = print.objects().front();
    std::vector<std::vector<Vec2d>> out(po->layer_count());
    for (size_t layer_idx = 0; layer_idx < po->layer_count(); ++layer_idx) {
        const Layer *layer = po->get_layer(int(layer_idx));
        for (const LayerRegion *region : layer->regions()) {
            std::vector<const ExtrusionLoop *> loops;
            collect_outer_loops(&region->perimeters, loops);
            for (const ExtrusionLoop *source : loops) {
                ExtrusionLoop loop     = *source;
                float         overhang = 0.f;
                placer.place_seam(layer, loop, Point(0, 0), overhang);
                out[layer_idx].push_back(unscale(loop.first_point()));
            }
        }
    }
    return out;
}

Planned plan(const Print &print, SeamPosition mode, bool prefer_joints, bool use_paint = true)
{
    return SeamPlacer::plan_object_seams(print, *print.objects().front(), mode, prefer_joints, use_paint, []() {});
}

std::vector<SeamPlacer::PlannedSeam> outlines(const Planned &planned)
{
    std::vector<SeamPlacer::PlannedSeam> out;
    for (const auto &layer : planned)
        for (const SeamPlacer::PlannedSeam &seam : layer)
            if (!seam.is_hole)
                out.push_back(seam);
    return out;
}

void cylinder(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "cylinder20";
    object->add_volume(make_cylinder(10., 10.));
    object->add_instance();
}

// Two 10 mm cubes side by side, touching on a face: one object, two parts. Centred, the joint is the line x = 0 on
// the front (y = -5) and back (y = +5) faces.
void flush_boxes(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "flush_boxes";
    TriangleMesh a      = make_cube(10., 10., 10.);
    TriangleMesh b      = make_cube(10., 10., 10.);
    b.translate(10.f, 0.f, 0.f);
    object->add_volume(a);
    object->add_volume(b);
    object->add_instance();
}

struct Painted
{
    std::vector<std::unique_ptr<TriangleSelector>> selectors;
    std::vector<Transform3d>                       trafos; // mesh -> planned seam coordinates, per part
    SeamAutoPaint::Stats                           stats;
};

// Paints the planned seams onto the model parts of the first object, as the gizmo does.
Painted paint(const Print &print, const Planned &planned, float radius = 0.f)
{
    Painted                             out;
    const PrintObject                  *po = print.objects().front();
    std::vector<SeamAutoPaint::Volume>  volumes;
    for (const ModelVolume *mv : po->model_object()->volumes) {
        if (!mv->is_model_part())
            continue;
        out.selectors.emplace_back(std::make_unique<TriangleSelector>(mv->mesh()));
        out.trafos.push_back(po->trafo_centered() * mv->get_matrix());
        volumes.push_back({ &mv->mesh().its, out.trafos.back(), out.selectors.back().get() });
    }
    SeamAutoPaint::Params params;
    params.radius = radius;
    out.stats     = SeamAutoPaint::paint(planned, volumes, params);
    return out;
}

// Enforcer triangle vertices of part `idx`, in the planned seams' coordinates.
std::vector<Vec3f> enforcer_vertices(const Painted &painted, size_t idx)
{
    const indexed_triangle_set its = painted.selectors[idx]->get_facets(EnforcerBlockerType::ENFORCER);
    std::vector<Vec3f>         out;
    for (const Vec3f &v : its.vertices)
        out.push_back((painted.trafos[idx] * v.cast<double>()).cast<float>());
    return out;
}

float distance_to_nearest(const Vec3f &p, const std::vector<SeamPlacer::PlannedSeam> &seams)
{
    float best = std::numeric_limits<float>::max();
    for (const SeamPlacer::PlannedSeam &s : seams)
        best = std::min(best, (s.position - p).norm());
    return best;
}

const std::vector<std::pair<const char *, SeamPosition>> aligned_modes = {
    { "aligned", spAligned }, { "aligned_back", spAlignedBack }, { "aligned_front", spAlignedFront },
    { "left", spLeft },       { "right", spRight },
};

} // namespace

TEST_CASE("Planned seams are the seams the slicer places, whatever the print is set to", "[Seam][SeamAutoPaint]")
{
    // The print is sliced with the Back seam; the plan is asked for each Aligned mode and compared with a print
    // that is really configured with that mode.
    Model model;
    cylinder(model);
    Print print;
    slice_walls(print, model, autopaint_config("back"));
    for (const auto &[key, mode] : aligned_modes) {
        DYNAMIC_SECTION("mode " << key)
        {
            const Planned planned = plan(print, mode, true);

            Model reference_model;
            cylinder(reference_model);
            Print reference;
            slice_walls(reference, reference_model, autopaint_config(key));
            const std::vector<std::vector<Vec2d>> placed = placed_seams(reference);

            REQUIRE(planned.size() == placed.size());
            size_t compared = 0;
            for (size_t layer = 0; layer < placed.size(); ++layer) {
                REQUIRE(planned[layer].size() == placed[layer].size());
                for (const Vec2d &p : placed[layer]) {
                    double best = std::numeric_limits<double>::max();
                    for (const SeamPlacer::PlannedSeam &s : planned[layer])
                        best = std::min(best, (s.position.head<2>().cast<double>() - p).norm());
                    INFO("layer " << layer << ", placed at " << p.x() << ", " << p.y());
                    CHECK(best < 0.01);
                    ++compared;
                }
            }
            CHECK(compared >= 45);
            // The print's own setting is untouched.
            CHECK(print.objects().front()->config().seam_position.value == spRear);
        }
    }
}

TEST_CASE("Planned seams of the directional Aligned modes sit on their side", "[Seam][SeamAutoPaint]")
{
    Model model;
    cylinder(model);
    Print print;
    slice_walls(print, model, autopaint_config("aligned"));

    struct Side { SeamPosition mode; int axis; double sign; const char *name; };
    for (const Side &side : { Side{ spAlignedFront, 1, -1., "front" }, Side{ spAlignedBack, 1, 1., "back" },
                              Side{ spLeft, 0, -1., "left" }, Side{ spRight, 0, 1., "right" } }) {
        DYNAMIC_SECTION("aligned " << side.name)
        {
            const std::vector<SeamPlacer::PlannedSeam> seams = outlines(plan(print, side.mode, true));
            REQUIRE(seams.size() >= 45);
            for (const SeamPlacer::PlannedSeam &s : seams) {
                INFO("seam at " << s.position.x() << ", " << s.position.y() << ", z " << s.position.z());
                CHECK(side.sign * s.position[side.axis] > 5.);
            }
        }
    }
    SECTION("plain aligned lines the seams up")
    {
        const std::vector<SeamPlacer::PlannedSeam> seams = outlines(plan(print, spAligned, true));
        REQUIRE(seams.size() >= 45);
        float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
        for (const SeamPlacer::PlannedSeam &s : seams) {
            min_x = std::min(min_x, s.position.x());
            max_x = std::max(max_x, s.position.x());
            min_y = std::min(min_y, s.position.y());
            max_y = std::max(max_y, s.position.y());
        }
        CHECK(max_x - min_x < 1.f);
        CHECK(max_y - min_y < 1.f);
    }
}

TEST_CASE("The joint preference of the plan overrides the print's", "[Seam][SeamAutoPaint][SeamJoints]")
{
    Model model;
    flush_boxes(model);
    Print print;
    // The print has the option OFF: the plan switches it on by itself.
    slice_walls(print, model, autopaint_config("aligned", false));

    const std::vector<SeamPlacer::PlannedSeam> on  = outlines(plan(print, spAligned, true));
    const std::vector<SeamPlacer::PlannedSeam> off = outlines(plan(print, spAligned, false));
    REQUIRE(on.size() >= 45);
    REQUIRE(off.size() >= 45);
    for (const SeamPlacer::PlannedSeam &s : on) {
        INFO("seam at " << s.position.x() << ", " << s.position.y() << ", z " << s.position.z());
        CHECK(std::abs(s.position.x()) < 0.6f);
        CHECK(std::abs(s.position.y()) > 4.f);
    }
    CHECK(std::count_if(off.begin(), off.end(), [](const SeamPlacer::PlannedSeam &s) { return std::abs(s.position.x()) < 0.6f; }) <
          ptrdiff_t(off.size() / 2));

    SECTION("the front mode takes the joint on the front face")
    {
        const std::vector<SeamPlacer::PlannedSeam> front = outlines(plan(print, spAlignedFront, true));
        REQUIRE(front.size() >= 45);
        for (const SeamPlacer::PlannedSeam &s : front) {
            INFO("seam at " << s.position.x() << ", " << s.position.y() << ", z " << s.position.z());
            CHECK(std::abs(s.position.x()) < 0.6f);
            CHECK(s.position.y() < -4.f);
        }
    }
}

TEST_CASE("Auto-paint marks enforcers along the planned seams and nowhere else", "[Seam][SeamAutoPaint]")
{
    Model model;
    cylinder(model);
    Print print;
    slice_walls(print, model, autopaint_config("aligned"));
    const Planned                              planned = plan(print, spAlignedFront, true);
    const std::vector<SeamPlacer::PlannedSeam> seams   = outlines(planned);
    REQUIRE(seams.size() >= 45);

    const float   radius  = 0.6f;
    const Painted painted = paint(print, planned, radius);
    CHECK(painted.stats.seams == seams.size());
    CHECK(painted.stats.links + 1 >= seams.size());

    const std::vector<Vec3f> vertices = enforcer_vertices(painted, 0);
    REQUIRE(!vertices.empty());
    // Near every seam: the seam is half a line width inside the surface, the strip is `radius` wide on each side.
    for (const SeamPlacer::PlannedSeam &s : seams) {
        float best = std::numeric_limits<float>::max();
        for (const Vec3f &v : vertices)
            best = std::min(best, (v - s.position).norm());
        INFO("seam at " << s.position.x() << ", " << s.position.y() << ", z " << s.position.z());
        CHECK(best < radius + s.flow_width);
    }
    // And only there.
    for (const Vec3f &v : vertices) {
        INFO("enforcer vertex at " << v.x() << ", " << v.y() << ", " << v.z());
        CHECK(distance_to_nearest(v, seams) < radius + 2.f * seams.front().flow_width + 0.1f);
        CHECK(v.y() < -8.f);
    }
    // No blockers were made.
    CHECK(painted.selectors[0]->get_facets(EnforcerBlockerType::BLOCKER).indices.empty());
}

TEST_CASE("Auto-paint paints both parts a joint seam lies between", "[Seam][SeamAutoPaint][SeamJoints]")
{
    Model model;
    flush_boxes(model);
    Print print;
    slice_walls(print, model, autopaint_config("aligned"));
    const Planned                              planned = plan(print, spAlignedFront, true);
    const std::vector<SeamPlacer::PlannedSeam> seams   = outlines(planned);
    const Painted                              painted = paint(print, planned);
    REQUIRE(painted.selectors.size() == 2);
    for (size_t part = 0; part < 2; ++part) {
        const std::vector<Vec3f> vertices = enforcer_vertices(painted, part);
        INFO("part " << part);
        REQUIRE(!vertices.empty());
        for (const Vec3f &v : vertices) {
            INFO("enforcer vertex at " << v.x() << ", " << v.y() << ", " << v.z());
            CHECK(std::abs(v.x()) < 1.2f);
            CHECK(v.y() < -4.f);
        }
    }
}

TEST_CASE("Auto-paint follows the instance and part transformations", "[Seam][SeamAutoPaint]")
{
    // The part is offset inside the object and the instance is rotated by 90 degrees and scaled: Aligned front is
    // the bed's front (-Y), so the strip must be on the mesh's +X side (the instance turns +X of the mesh to -Y).
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "turned_cylinder";
    ModelVolume *part   = object->add_volume(make_cylinder(10., 10.));
    part->set_offset(Vec3d(3., -2., 0.));
    ModelInstance *instance = object->add_instance();
    instance->set_rotation(Vec3d(0., 0., -0.5 * PI));
    instance->set_scaling_factor(Vec3d(1.5, 1.5, 1.5));
    instance->set_offset(Vec3d(100., 100., 0.));
    Print print;
    slice_walls(print, model, autopaint_config("aligned"));

    const Planned planned = plan(print, spAlignedFront, true);
    REQUIRE(outlines(planned).size() >= 45);
    const Painted painted = paint(print, planned);

    const indexed_triangle_set its = painted.selectors[0]->get_facets(EnforcerBlockerType::ENFORCER);
    REQUIRE(!its.vertices.empty());
    const ModelObject  *printed  = print.objects().front()->model_object();
    const Transform3d   to_world = printed->instances.front()->get_matrix() * printed->volumes.front()->get_matrix();
    const BoundingBoxf3 bbox     = printed->instance_bounding_box(0);
    for (const Vec3f &v : its.vertices) {
        // Mesh coordinates: the +X side of the cylinder (radius 10).
        CHECK(v.x() > 8.f);
        // World coordinates: the front of the object on the bed.
        const Vec3d w = to_world * v.cast<double>();
        INFO("world " << w.x() << ", " << w.y() << ", " << w.z());
        CHECK(w.y() < bbox.min.y() + 3.);
    }
}

TEST_CASE("Re-slicing an auto-painted object puts the seam on the strip", "[Seam][SeamAutoPaint]")
{
    Model model;
    cylinder(model);
    Planned planned;
    {
        Print print;
        slice_walls(print, model, autopaint_config("aligned"));
        planned               = plan(print, spAlignedFront, true);
        const Painted painted = paint(print, planned);
        // Into the model, as the gizmo's update_model_object() does.
        REQUIRE(model.objects.front()->volumes.front()->seam_facets.set(*painted.selectors.front()));
    }
    // Sliced with the opposite position: the painted enforcers must still win.
    Print print;
    slice_walls(print, model, autopaint_config("aligned_back"));
    const std::vector<std::vector<Vec2d>> placed = placed_seams(print);
    REQUIRE(placed.size() == planned.size());
    size_t checked = 0;
    for (size_t layer = 0; layer < placed.size(); ++layer)
        for (const Vec2d &p : placed[layer]) {
            double best = std::numeric_limits<double>::max();
            for (const SeamPlacer::PlannedSeam &s : planned[layer])
                best = std::min(best, (s.position.head<2>().cast<double>() - p).norm());
            INFO("layer " << layer << ", placed at " << p.x() << ", " << p.y());
            CHECK(best < 1.0);
            CHECK(p.y() < -8.);
            ++checked;
        }
    CHECK(checked >= 45);
}

TEST_CASE("Aligned front is a seam position of its own", "[Seam][SeamAutoPaint][Config]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "seam_position", "aligned_front" } });
    CHECK(config.opt_enum<SeamPosition>("seam_position") == spAlignedFront);
    CHECK(config.opt_serialize("seam_position") == "aligned_front");
    // Appended: every value that shipped before keeps its number.
    CHECK(int(spRight) == 6);
    CHECK(int(spAlignedFront) == 7);
    const ConfigOptionDef *def = print_config_def.get("seam_position");
    REQUIRE(def != nullptr);
    CHECK(std::find(def->enum_values.begin(), def->enum_values.end(), "aligned_front") != def->enum_values.end());
    CHECK(def->enum_values.size() == def->enum_labels.size());
}
