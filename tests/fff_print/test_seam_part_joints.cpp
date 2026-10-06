// Seam part joints (seam_prefer_part_joints).
//
// For the Aligned seam positions, an object made of several touching parts - or objects placed against each
// other - gets its seam put on the line where two parts meet, where it hides, instead of on a corner elsewhere.
// These cases hold it to that:
//
//  * two boxes touching flush on a side face: the seam lands on the joint line, in every Aligned mode;
//  * a box split by a slanted plane into two stacked parts: the seam follows the slanted joint up the side;
//  * two separate objects placed against each other: the seam goes into the face they share;
//  * seam blockers painted over one joint push the seam to the other joint - painting still wins;
//  * nothing changes where there is no joint: a single-part object, parts that do not touch, and the
//    Back / Random / Nearest positions all give the same G-code with the option on and off.

#include <catch2/catch.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/GCode/SeamPlacer.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"

#include "test_data.hpp"

using namespace Slic3r;

namespace {

// One outer-wall seam: where it is in the object's centred coordinates, the layer's slice height, and where its
// object sits on the bed (the X of its instance shift, to tell objects apart once the Print is gone).
struct Seam
{
    Vec2d  pos;
    double z;
    double object_x;
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

DynamicPrintConfig joint_config(const std::string &seam_position, bool prefer_joints)
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

void process(Print &print, Model &model, const ModelBuilder &build, const DynamicPrintConfig &config)
{
    build(model);
    for (ModelObject *object : model.objects) {
        object->ensure_on_bed();
        print.auto_assign_extruders(object);
    }
    print.apply(model, config);
    print.set_status_silent();
    print.process();
}

// Where SeamPlacer puts the seam of every outer wall loop (place_seam, so seam_gap and scarf joints stay out).
std::vector<Seam> seams_for(const ModelBuilder &build, const std::string &seam_position, bool prefer_joints = true)
{
    Print print;
    Model model;
    process(print, model, build, joint_config(seam_position, prefer_joints));

    SeamPlacer placer;
    placer.init(print, []() {});

    std::vector<Seam> seams;
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
                    seams.push_back({ unscale(loop.first_point()), layer->slice_z, unscale<double>(po->instances().front().shift.x()) });
                }
            }
        }
    }
    return seams;
}

// The G-code with the lines that must differ taken out: the option itself (config block) and the time stamp.
std::string gcode_for(const ModelBuilder &build, const DynamicPrintConfig &config)
{
    Print print;
    Model model;
    process(print, model, build, config);
    std::string        full = Test::gcode(print);
    std::istringstream in(full);
    std::string        line, out;
    while (std::getline(in, line))
        if (line.find("seam_prefer_part_joints") == std::string::npos && line.find("; generated by") == std::string::npos)
            out += line + "\n";
    return out;
}

// Two 10 mm cubes side by side, touching on the face x = 10: one object, two parts, one filament.
// The object is 20 x 10 and centred, so the joint is the line x = 0 on the front (y = -5) and back (y = +5).
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

// The same two cubes 5 mm apart: two parts that do not touch.
void separated_boxes(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "separated_boxes";
    TriangleMesh a      = make_cube(10., 10., 10.);
    TriangleMesh b      = make_cube(10., 10., 10.);
    b.translate(15.f, 0.f, 0.f);
    object->add_volume(a);
    object->add_volume(b);
    object->add_instance();
}

// A 20 mm cube split by the plane z = 5 + x / 2 into a lower and an upper part. Centred, the joint crosses the
// front and back faces (y = -10 / +10) along x = 2 (z - 5) - 10.
void slanted_split_box(Model &model)
{
    const std::vector<Vec3f> lower = { { 0, 0, 0 },  { 20, 0, 0 },  { 0, 20, 0 },  { 20, 20, 0 },
                                       { 0, 0, 5 },  { 0, 20, 5 },  { 20, 0, 15 }, { 20, 20, 15 } };
    const std::vector<Vec3f> upper = { { 0, 0, 5 },  { 0, 20, 5 },  { 20, 0, 15 }, { 20, 20, 15 },
                                       { 0, 0, 20 }, { 20, 0, 20 }, { 0, 20, 20 }, { 20, 20, 20 } };
    ModelObject *object = model.add_object();
    object->name        = "slanted_split_box";
    object->add_volume(TriangleMesh(its_convex_hull(lower)));
    object->add_volume(TriangleMesh(its_convex_hull(upper)));
    object->add_instance();
}

// Two separate one-part objects, each a 10 mm cube, placed against each other on the face x = 10.
void touching_objects(Model &model)
{
    for (int i = 0; i < 2; ++i) {
        ModelObject *object = model.add_object();
        object->name        = i == 0 ? "left_cube" : "right_cube";
        object->add_volume(make_cube(10., 10., 10.));
        ModelInstance *instance = object->add_instance();
        // add_volume centres the cube on its own origin; put the two side by side on the bed.
        instance->set_offset(Vec3d(95. + 10. * i, 100., 5.));
    }
}

// flush_boxes with a Strong CENTER helper on the right outer face (x = +10 after centering).
// The helper is 4 mm thick in X and 8 mm in Y, sitting on the right cube's +X face so it clips
// that wall and not the joint at x = 0.
void flush_boxes_strong_on_right(Model &model)
{
    flush_boxes(model);
    ModelObject *object = model.objects.front();
    ModelVolume *right  = object->volumes[1];
    ModelVolume *helper = object->add_volume(make_cube(4., 8., 10.));
    helper->set_type(ModelVolumeType::PRECISE_SEAM_CENTER);
    helper->set_offset(right->get_offset() + Vec3d(5., 0., 0.));
}

// flush_boxes with seam blockers painted on the front faces (y = 0) of both parts.
void flush_boxes_front_blocked(Model &model)
{
    flush_boxes(model);
    for (ModelVolume *volume : model.objects.front()->volumes) {
        const indexed_triangle_set &its = volume->mesh().its;
        TriangleSelector            selector(volume->mesh());
        for (int f = 0; f < int(its.indices.size()); ++f)
            if (its_face_normal(its, f).y() < -0.9f)
                selector.set_facet(f, EnforcerBlockerType::BLOCKER);
        volume->seam_facets.set(selector);
    }
}

// A single part: the L-shaped test mesh (it has a concave corner, so the seam has somewhere to go).
void single_part(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "L";
    object->add_volume(Test::mesh(Test::TestMesh::L));
    object->add_instance();
}

// Seams on layers whose slice height lies strictly within [z_min, z_max].
std::vector<Seam> between(const std::vector<Seam> &seams, double z_min, double z_max)
{
    std::vector<Seam> out;
    for (const Seam &s : seams)
        if (s.z > z_min && s.z < z_max)
            out.push_back(s);
    return out;
}

size_t count_if_seams(const std::vector<Seam> &seams, const std::function<bool(const Seam &)> &pred)
{
    return size_t(std::count_if(seams.begin(), seams.end(), pred));
}

} // namespace

SCENARIO("Aligned seams hide in the joint between two touching parts", "[Seam][SeamJoints]")
{
    for (const char *mode : { "aligned", "aligned_back", "left", "right" }) {
        GIVEN(std::string("two cubes touching flush, seam_position = ") + mode)
        {
            const std::vector<Seam> seams = seams_for(flush_boxes, mode);
            THEN("there is a seam on every layer")
            {
                REQUIRE(seams.size() >= 45);
            }
            THEN("every seam is on the joint line, on the front or the back face")
            {
                for (const Seam &s : seams) {
                    INFO("z = " << s.z << ", seam at " << s.pos.x() << ", " << s.pos.y());
                    CHECK(std::abs(s.pos.x()) < 0.6);
                    CHECK(std::abs(s.pos.y()) > 4.);
                }
            }
            THEN("they all sit on the same joint line, one above the other")
            {
                const size_t front = count_if_seams(seams, [](const Seam &s) { return s.pos.y() < 0.; });
                CHECK((front == 0 || front == seams.size()));
            }
        }
    }

    GIVEN("the same cubes with the option switched off")
    {
        const std::vector<Seam> seams = seams_for(flush_boxes, "aligned", false);
        THEN("the seam is not on the joint: the option is what puts it there")
        {
            REQUIRE(seams.size() >= 45);
            CHECK(count_if_seams(seams, [](const Seam &s) { return std::abs(s.pos.x()) < 0.6; }) < seams.size() / 2);
        }
    }
}

SCENARIO("The aligned seam follows a slanted joint up the side of the object", "[Seam][SeamJoints]")
{
    GIVEN("a cube split into two stacked parts by a slanted plane")
    {
        const std::vector<Seam> seams = seams_for(slanted_split_box, "aligned");
        // Away from the ends of the joint (it meets the side faces x = 0 / x = 20 at z = 5 / 15).
        const std::vector<Seam> on_joint = between(seams, 6., 14.);
        THEN("each of those layers has a seam")
        {
            REQUIRE(on_joint.size() >= 35);
        }
        THEN("every seam there is on the joint line of the front or back face")
        {
            for (const Seam &s : on_joint) {
                const double joint_x = 2. * (s.z - 5.) - 10.;
                INFO("z = " << s.z << ", seam at " << s.pos.x() << ", " << s.pos.y() << ", joint at x = " << joint_x);
                CHECK(std::abs(s.pos.x() - joint_x) < 0.8);
                CHECK(std::abs(s.pos.y()) > 9.);
            }
        }
        THEN("they all follow the same one of the two joint lines")
        {
            const size_t front = count_if_seams(on_joint, [](const Seam &s) { return s.pos.y() < 0.; });
            CHECK((front == 0 || front == on_joint.size()));
        }
    }
}

SCENARIO("Separate objects placed against each other hide the seam in the face they share", "[Seam][SeamJoints]")
{
    GIVEN("two one-part cubes side by side, touching")
    {
        const std::vector<Seam> seams = seams_for(touching_objects, "aligned");
        REQUIRE(seams.size() >= 90);
        // The left cube is the one further toward -X on the bed.
        double left = seams.front().object_x;
        for (const Seam &s : seams)
            left = std::min(left, s.object_x);
        REQUIRE(count_if_seams(seams, [left](const Seam &s) { return s.object_x == left; }) >= 45);
        REQUIRE(count_if_seams(seams, [left](const Seam &s) { return s.object_x != left; }) >= 45);
        THEN("each cube's seam is in the middle of the face that touches the other cube")
        {
            for (const Seam &s : seams) {
                // The left cube touches on its +X face, the right one on its -X face.
                const bool   left_cube = s.object_x == left;
                const double face_x    = left_cube ? 5. : -5.;
                INFO((left_cube ? "left" : "right") << " cube, z = " << s.z << ", seam at " << s.pos.x() << ", " << s.pos.y());
                CHECK(std::abs(s.pos.x() - face_x) < 0.6);
                CHECK(std::abs(s.pos.y()) < 1.5);
            }
        }
    }
}

SCENARIO("A Strong Precise Seam helper wins over a part joint", "[Seam][SeamJoints][PreciseSeam]")
{
    GIVEN("two touching cubes with a CENTER helper on the right outer face, joints still preferred")
    {
        const std::vector<Seam> seams = seams_for(flush_boxes_strong_on_right, "aligned", true);
        REQUIRE(seams.size() >= 45);
        THEN("every seam is on the helper's face, not the joint at x = 0")
        {
            for (const Seam &s : seams) {
                INFO("z = " << s.z << ", seam at " << s.pos.x() << ", " << s.pos.y());
                CHECK(s.pos.x() > 7.);
                CHECK(std::abs(s.pos.y()) < 4.);
            }
        }
    }
}

SCENARIO("Painted seam blockers still win over a part joint", "[Seam][SeamJoints]")
{
    GIVEN("two touching cubes with blockers painted over their front faces, and so over the front joint")
    {
        const std::vector<Seam> seams = seams_for(flush_boxes_front_blocked, "aligned");
        REQUIRE(seams.size() >= 45);
        THEN("the seam goes to the unpainted joint at the back")
        {
            for (const Seam &s : seams) {
                INFO("z = " << s.z << ", seam at " << s.pos.x() << ", " << s.pos.y());
                CHECK(std::abs(s.pos.x()) < 0.6);
                CHECK(s.pos.y() > 4.);
            }
        }
    }
}

SCENARIO("Without a joint, or outside the Aligned family, the option changes nothing", "[Seam][SeamJoints]")
{
    for (const char *mode : { "aligned", "aligned_back", "left", "right" }) {
        GIVEN(std::string("a single-part object, seam_position = ") + mode)
        {
            THEN("the G-code is byte-identical with the option on and off")
            {
                const std::string on  = gcode_for(single_part, joint_config(mode, true));
                const std::string off = gcode_for(single_part, joint_config(mode, false));
                REQUIRE(on.size() > 1000);
                REQUIRE(on == off);
            }
        }
        GIVEN(std::string("two parts that do not touch, seam_position = ") + mode)
        {
            THEN("the G-code is byte-identical with the option on and off")
            {
                REQUIRE(gcode_for(separated_boxes, joint_config(mode, true)) ==
                        gcode_for(separated_boxes, joint_config(mode, false)));
            }
        }
    }
    for (const char *mode : { "back", "random", "nearest" }) {
        GIVEN(std::string("two touching parts, seam_position = ") + mode)
        {
            THEN("the G-code is byte-identical with the option on and off")
            {
                REQUIRE(gcode_for(flush_boxes, joint_config(mode, true)) ==
                        gcode_for(flush_boxes, joint_config(mode, false)));
            }
        }
    }
}

SCENARIO("seam_prefer_part_joints is an EdgeSlicer setting", "[Seam][SeamJoints]")
{
    GIVEN("a fresh print config")
    {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        THEN("it is on by default and round-trips")
        {
            REQUIRE(config.opt_bool("seam_prefer_part_joints"));
            config.set_deserialize_strict({ { "seam_prefer_part_joints", "0" } });
            REQUIRE_FALSE(config.opt_bool("seam_prefer_part_joints"));
            REQUIRE(config.opt_serialize("seam_prefer_part_joints") == "0");
        }
        THEN("Bambu Studio does not know it, so Export Bambu 3MF leaves it out")
        {
            REQUIRE(BambuExport::find_key(BambuExport::bambu_key_name("seam_prefer_part_joints")) == nullptr);
        }
    }
}

// Timing on a large assembly: 3 x 3 finely tessellated spheres, each overlapping its neighbours, in one object
// (about 1.4 M triangles). Hidden; run with "[SeamJointsBench]" and read the printed times.
SCENARIO("Part joint detection cost on a large assembly", "[.][SeamJointsBench]")
{
    auto spheres = [](Model &model) {
        ModelObject *object = model.add_object();
        object->name        = "sphere_grid";
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                TriangleMesh sphere = make_sphere(10., 2. * PI / 400.);
                sphere.translate(float(18 * i), float(18 * j), 0.f);
                object->add_volume(sphere);
            }
        object->add_instance();
    };
    for (bool prefer : { false, true }) {
        Print      print;
        Model      model;
        const auto slice_start = std::chrono::steady_clock::now();
        process(print, model, spheres, joint_config("aligned", prefer));
        const double slice_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - slice_start).count();
        size_t triangles = 0;
        for (const ModelVolume *v : model.objects.front()->volumes)
            triangles += v->mesh().its.indices.size();
        double         best      = 1e9;
        const unsigned log_level = get_logging_level();
        for (int run = 0; run < 3; ++run) {
            SeamPlacer placer;
            // The last run logs the phases of the joint detection (debug level).
            if (run == 2)
                set_logging_level(4);
            const auto start = std::chrono::steady_clock::now();
            placer.init(print, []() {});
            best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            set_logging_level(log_level);
        }
        WARN("seam_prefer_part_joints=" << prefer << ": SeamPlacer::init on " << triangles << " triangles, "
                                         << print.objects().front()->layers().size() << " layers: " << best << " s (best of 3); slicing it took "
                                         << slice_time << " s");
    }
}
