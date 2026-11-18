#include <catch2/catch_test_macros.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"

#include <boost/algorithm/string.hpp>

#include <string_view>

#include "test_data.hpp" // get access to init_print, etc

using namespace Slic3r::Test;
using namespace Slic3r;

static TriangleMesh brim_test_ring()
{
    std::vector<Vec3f> vertices;
    for (float z : {0.f, 0.6f})
        for (const Vec2f& point :
             {Vec2f(0, 0), Vec2f(20, 0), Vec2f(20, 20), Vec2f(0, 20), Vec2f(6, 6), Vec2f(14, 6), Vec2f(14, 14), Vec2f(6, 14)})
            vertices.emplace_back(point.x(), point.y(), z);
    std::vector<Vec3i32> faces;
    const auto           quad = [&faces](int a, int b, int c, int d) {
        faces.emplace_back(a, b, c);
        faces.emplace_back(a, c, d);
    };
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        quad(i, j, j + 8, i + 8);
        quad(i + 4, i + 12, j + 12, j + 4);
        quad(i + 8, j + 8, j + 12, i + 12);
        quad(i, i + 4, j + 4, j);
    }
    return TriangleMesh(vertices, faces);
}

static DynamicPrintConfig brim_test_config(const std::string& type, double width = 30., double gap = 0.)
{
    DynamicPrintConfig config = Slic3r::Test::default_print_config();
    config.set_deserialize_strict({{"brim_type", type},
                                   {"brim_width", width},
                                   {"brim_object_gap", gap},
                                   {"skirt_loops", 0},
                                   {"enable_prime_tower", false},
                                   {"elefant_foot_compensation", 0}});
    return config;
}

static ModelObject* add_brim_test_object(Model& model, const TriangleMesh& mesh, const Vec3d& offset)
{
    ModelObject* object = model.add_object();
    object->add_volume(mesh, false);
    object->add_instance()->set_offset(offset);
    return object;
}

static Polylines brim_test_paths(Print& print)
{
    Polylines paths;
    for (const auto& entry : print.get_brimMap())
        entry.second.collect_polylines(paths);
    return paths;
}

TEST_CASE("Outer brim does not enter an empty hole from a neighboring island", "[Brim][BrimHoles]")
{
    int    placement    = 0;
    double width        = 30.;
    double gap          = 0.;
    Vec3d  plate_origin = Vec3d::Zero();
    SECTION("Disconnected shells in one volume") {}
    SECTION("Nonzero brim gap") { gap = 1.; }
    SECTION("Narrow brim control") { width = 5.; }
    SECTION("Separate volume groups") { placement = 1; }
    SECTION("Separate objects") { placement = 2; }
    SECTION("Instances on an offset plate")
    {
        placement    = 3;
        plate_origin = Vec3d(512, -256, 0);
    }

    DynamicPrintConfig config   = brim_test_config("outer_only", width, gap);
    TriangleMesh       mesh     = brim_test_ring();
    TriangleMesh       neighbor = make_cube(10, 10, 0.6);
    neighbor.translate(24, 5, 0);
    if (placement == 0 || placement == 3)
        mesh.merge(neighbor);
    Model        model;
    const Vec3d  position = plate_origin + Vec3d(60, 60, 0);
    ModelObject* ring     = add_brim_test_object(model, mesh, position);
    if (placement == 1)
        ring->add_volume(neighbor, false);
    if (placement == 2)
        add_brim_test_object(model, neighbor, position);
    if (placement == 3)
        ring->add_instance()->set_offset(position + Vec3d(0, 60, 0));
    Print print;
    print.set_plate_origin(plate_origin);
    print.apply(model, config);
    print.set_status_silent();
    print.process();
    Polygons holes;
    Polygons contours;
    for (const PrintObject* object : print.objects()) {
        for (const PrintInstance& instance : object->instances()) {
            for (ExPolygon slice : object->layers().front()->lslices) {
                slice.translate(instance.shift);
                append(holes, slice.holes);
                contours.push_back(slice.contour);
            }
        }
    }
    REQUIRE(holes.size() == (placement == 3 ? 2 : 1));
    polygons_reverse(holes);
    const Polylines brim = brim_test_paths(print);
    REQUIRE_FALSE(brim.empty());
    for (const Polygon& hole : holes)
        CHECK(intersection_pl(brim, offset(Polygons{hole}, -scale_(0.5))).empty());
    CHECK_FALSE(intersection_pl(brim, diff(offset(contours, scale_(2.)), contours)).empty());
}

TEST_CASE("Brim modes preserve intentional inner and outer paths", "[Brim][BrimHoles]")
{
    std::string type;
    bool        want_inner = false;
    bool        want_outer = false;
    SECTION("Outer only")
    {
        type       = "outer_only";
        want_outer = true;
    }
    SECTION("Inner only")
    {
        type       = "inner_only";
        want_inner = true;
    }
    SECTION("Outer and inner")
    {
        type       = "outer_and_inner";
        want_inner = want_outer = true;
    }
    SECTION("No brim") { type = "no_brim"; }
    SECTION("Painted outer ear")
    {
        type       = "painted";
        want_outer = true;
    }
    Model        model;
    ModelObject* model_ring = add_brim_test_object(model, brim_test_ring(), Vec3d(60, 60, 0));
    if (type == "painted")
        model_ring->brim_points.emplace_back(0.f, 0.f, 0.f, 3.f);
    Print print;
    print.apply(model, brim_test_config(type, 3., 0.2));
    print.set_status_silent();
    print.process();
    REQUIRE(print.objects().size() == 1);
    const PrintObject* object = print.objects().front();
    REQUIRE(object->layers().front()->lslices.size() == 1);
    ExPolygon ring = object->layers().front()->lslices.front();
    ring.translate(object->instances().front().shift);
    REQUIRE(ring.holes.size() == 1);
    polygons_reverse(ring.holes);
    const Polylines brim = brim_test_paths(print);
    CHECK(!intersection_pl(brim, offset(ring.holes, -scale_(0.5))).empty() == want_inner);
    CHECK(!diff_pl(brim, Polygons{ring.contour}).empty() == want_outer);
    CHECK(brim.empty() == (!want_inner && !want_outer));
}

TEST_CASE("Outer brim of a nested island stays in its enclosing hole", "[Brim][BrimHoles]")
{
    bool nested_twice = false;
    bool same_object  = false;
    SECTION("Separate nested object") {}
    SECTION("Nested shell in the same object") { same_object = true; }
    SECTION("Two enclosing holes") { nested_twice = true; }
    TriangleMesh ring_mesh  = brim_test_ring();
    TriangleMesh child_mesh = make_cube(2, 2, 0.6);
    child_mesh.translate(9, 9, 0);
    if (same_object)
        ring_mesh.merge(child_mesh);
    Model        model;
    const Vec3d  position(60, 60, 0);
    ModelObject* ring = add_brim_test_object(model, ring_mesh, position);
    if (!same_object) {
        ring->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btNoBrim));
        add_brim_test_object(model, child_mesh, position);
    }
    if (nested_twice) {
        TriangleMesh outer_ring = brim_test_ring();
        outer_ring.scale(3.);
        outer_ring.translate(-20, -20, 0);
        ModelObject* ancestor = add_brim_test_object(model, outer_ring, position);
        ancestor->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btNoBrim));
    }
    Print print;
    print.apply(model, brim_test_config("outer_only"));
    print.set_status_silent();
    print.process();
    const Polylines brim = brim_test_paths(print);
    REQUIRE_FALSE(brim.empty());
    size_t checked_holes = 0;
    for (const PrintObject* object : print.objects()) {
        for (ExPolygon slice : object->layers().front()->lslices) {
            if (slice.holes.empty())
                continue;
            slice.translate(object->instances().front().shift);
            polygons_reverse(slice.holes);
            CHECK_FALSE(intersection_pl(brim, offset(slice.holes, -scale_(1.))).empty());
            if (!same_object)
                CHECK(diff_pl(brim, slice.holes).empty());
            ++checked_holes;
        }
    }
    CHECK(checked_holes == (nested_twice ? 2 : 1));
}

TEST_CASE("Nested automatic mouse ears cannot escape their hole", "[Brim][BrimHoles]")
{
    double gap = 0.;
    SECTION("Ears attach inside the hole") {}
    SECTION("Gap puts every ear outside the hole") { gap = 10.; }
    Model        model;
    ModelObject* ring = add_brim_test_object(model, brim_test_ring(), Vec3d(60, 60, 0));
    ring->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btNoBrim));
    TriangleMesh child = make_cube(2, 2, 0.6);
    child.translate(9, 9, 0);
    add_brim_test_object(model, child, Vec3d(60, 60, 0));
    Print print;
    print.apply(model, brim_test_config("brim_ears", 30., gap));
    print.set_status_silent();
    print.process();
    const Polylines brim = brim_test_paths(print);
    CHECK(brim.empty() == (gap > 0.));
    size_t checked_holes = 0;
    for (const PrintObject* object : print.objects()) {
        for (ExPolygon slice : object->layers().front()->lslices) {
            if (slice.holes.empty())
                continue;
            slice.translate(object->instances().front().shift);
            polygons_reverse(slice.holes);
            CHECK(diff_pl(brim, slice.holes).empty());
            ++checked_holes;
        }
    }
    REQUIRE(checked_holes == 1);
}

TEST_CASE("A partially overlapping object keeps brim on both sides of a hole", "[Brim][BrimHoles]")
{
    Model        model;
    ModelObject* ring = add_brim_test_object(model, brim_test_ring(), Vec3d(60, 60, 0));
    ring->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btNoBrim));
    TriangleMesh bar = make_cube(15, 2, 0.6);
    bar.translate(9, 9, 0);
    add_brim_test_object(model, bar, Vec3d(60, 60, 0));
    Print print;
    print.apply(model, brim_test_config("outer_only", 3.));
    print.set_status_silent();
    print.process();
    const Polylines brim = brim_test_paths(print);
    REQUIRE_FALSE(brim.empty());
    const Polygon hole({Point(scale_(66.), scale_(66.)), Point(scale_(74.), scale_(66.)), Point(scale_(74.), scale_(74.)),
                        Point(scale_(66.), scale_(74.))});
    CHECK_FALSE(intersection_pl(brim, offset(Polygons{hole}, -scale_(0.5))).empty());
    const Polygon outer_side({Point(scale_(80.5), scale_(66.)), Point(scale_(88.), scale_(66.)), Point(scale_(88.), scale_(74.)),
                              Point(scale_(80.5), scale_(74.))});
    CHECK_FALSE(intersection_pl(brim, Polygons{outer_side}).empty());
}

TEST_CASE("Automatic brim of a tall neighbor does not enter an empty hole", "[Brim][BrimHoles]")
{
    Model        model;
    ModelObject* ring = add_brim_test_object(model, brim_test_ring(), Vec3d(60, 60, 0));
    ring->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btNoBrim));
    TriangleMesh tower = make_cube(2, 2, 20);
    tower.translate(24, 9, 0);
    add_brim_test_object(model, tower, Vec3d(60, 60, 0));
    Print print;
    print.apply(model, brim_test_config("auto_brim"));
    print.set_status_silent();
    print.process();
    const Polylines brim = brim_test_paths(print);
    REQUIRE_FALSE(brim.empty());
    const Polygon hole({Point(scale_(66.), scale_(66.)), Point(scale_(74.), scale_(66.)), Point(scale_(74.), scale_(74.)),
                        Point(scale_(66.), scale_(74.))});
    CHECK(intersection_pl(brim, offset(Polygons{hole}, -scale_(0.5))).empty());
    const Polygon ring_contour({Point(scale_(60.), scale_(60.)), Point(scale_(80.), scale_(60.)), Point(scale_(80.), scale_(80.)),
                                Point(scale_(60.), scale_(80.))});
    CHECK_FALSE(diff_pl(brim, Polygons{ring_contour}).empty());
}
/// Helper method to find the tool used for the brim (always the first extrusion)
static int get_brim_tool(const std::string &gcode)
{
    int brim_tool	= -1;
    int tool		= -1;
	GCodeReader parser;
    parser.parse_buffer(gcode, [&tool, &brim_tool] (Slic3r::GCodeReader &self, const Slic3r::GCodeReader::GCodeLine &line)
    {
        // if the command is a T command, set the the current tool
        if (boost::starts_with(line.cmd(), "T")) {
            tool = atoi(line.cmd().data() + 1);
        } else if (line.cmd() == "G1" && line.extruding(self) && line.dist_XY(self) > 0 && brim_tool < 0) {
            brim_tool = tool;
        }
    });
    return brim_tool;
}

TEST_CASE("Skirt height is honored", "[Skirt]") {
    DynamicPrintConfig config = Slic3r::Test::default_print_config();
    config.set_deserialize_strict({
        { "skirt_loops",            1 },
        { "skirt_height",           5 },
        { "wall_loops",             0 },
        { "support_speed",          99 },
		// avoid altering speeds unexpectedly
        { "slow_down_for_layer_cooling", false },
        { "initial_layer_speed",    100 },
        { "gcode_comments",         true }
    });

	std::string gcode;
    SECTION("printing a single object") {
        gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
    }
    SECTION("printing multiple objects") {
        gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20, TestMesh::cube_20x20x20}, config);
    }

    std::map<double, bool> layers_with_skirt;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&layers_with_skirt] (Slic3r::GCodeReader &self, const Slic3r::GCodeReader::GCodeLine &line) {
        if (line.extruding(self) && line.comment().find("skirt") != std::string_view::npos) {
            layers_with_skirt[self.z()] = 1;
        }
    });
    REQUIRE(layers_with_skirt.size() == (size_t)config.opt_int("skirt_height"));
}

SCENARIO("Original Slic3r Skirt/Brim tests", "[SkirtBrim]") {
    GIVEN("A default configuration") {
	    DynamicPrintConfig config = Slic3r::Test::default_print_config();
		config.set_num_extruders(4);
		config.set_deserialize_strict({
			{ "support_speed", 				99 },
			{ "initial_layer_print_height", 	0.3 },
        	{ "gcode_comments", 				true },
        	// avoid altering speeds unexpectedly
			{ "slow_down_for_layer_cooling", false },
			{ "initial_layer_speed", 		100 },
        	// remove noise from top/solid layers
			{ "top_shell_layers", 			0 },
			{ "bottom_shell_layers", 		1 },
			{ "machine_start_gcode",			"T[initial_tool]\n" }
        });

        WHEN("Brim width is set to 5") {
        	config.set_deserialize_strict({
				{ "wall_loops", 		0 },
				{ "skirt_loops", 		0 },
				{ "brim_type", 			"outer_only" },
				{ "brim_width", 		5 }
			});
			THEN("Brim is generated") {
		        std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
                bool brim_generated = false;
			    Slic3r::GCodeReader parser;
                parser.parse_buffer(gcode, [&brim_generated] (Slic3r::GCodeReader& self, const Slic3r::GCodeReader::GCodeLine& line) {
                    if (std::abs(self.z() - 0.3) < 1e-6 || std::abs(line.new_Z(self) - 0.3) < 1e-6) {
                        if (line.extruding(self) && line.comment().find("brim") != std::string_view::npos) {
                            brim_generated = true;
                        }
                    }
                });
                REQUIRE(brim_generated);
            }
        }

        WHEN("Skirt area is smaller than the brim") {
            config.set_deserialize_strict({
				{ "skirt_loops", 1 },
				{ "brim_type", 	"outer_only" },
            	{ "brim_width", 10}
            });
            THEN("Gcode generates") {
                REQUIRE(! Slic3r::Test::slice({TestMesh::cube_20x20x20}, config).empty());
            }
        }

        WHEN("Skirt height is 0 and skirt_loops > 0") {
            config.set_deserialize_strict({
				{ "skirt_loops",  2 },
            	{ "skirt_height", 0 }
            });
            THEN("Gcode generates") {
                REQUIRE(! Slic3r::Test::slice({TestMesh::cube_20x20x20}, config).empty());
            }
        }

#if 0
		// This is a real error! One shall print the brim with the external perimeter extruder!
        WHEN("Perimeter extruder = 2 and support extruders = 3") {
            THEN("Brim is printed with the extruder used for the perimeters of first object") {
				config.set_deserialize_strict({
					{ "skirts", 					0 },
					{ "brim_width", 				5 },
					{ "perimeter_extruder", 		2 },
					{ "support_material_extruder", 	3 },
					{ "infill_extruder", 			4 }
				});
		        std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
                int tool = get_brim_tool(gcode);
                REQUIRE(tool == config.opt_int("perimeter_extruder") - 1);
            }
        }
        WHEN("Perimeter extruder = 2, support extruders = 3, raft is enabled") {
            THEN("brim is printed with same extruder as skirt") {
				config.set_deserialize_strict({
					{ "skirts",						0 },
					{ "brim_width", 				5 },
					{ "perimeter_extruder", 		2 },
					{ "support_material_extruder", 	3 },
					{ "infill_extruder", 			4 },
					{ "raft_layers", 				1 }
				});
		        std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
                int tool = get_brim_tool(gcode);
                REQUIRE(tool == config.opt_int("support_material_extruder") - 1);
            }
        }
#endif

        WHEN("brim width to 1 with layer_width of 0.5") {
        	config.set_deserialize_strict({
				{ "skirt_loops", 					0 },
				{ "brim_type", 						"outer_only" },
				{ "initial_layer_line_width", 	0.5 },
				{ "brim_width", 					1 }
        	});			
            THEN("2 brim lines") {
		        Slic3r::Print print;
		        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, config);
                size_t total_entities = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_entities += pair.second.entities.size();
                }
                REQUIRE(total_entities == 2);
            }
        }

#if 0
        WHEN("brim ears on a square") {
			config.set_deserialize_strict({
				{ "skirts",							0 },
				{ "initial_layer_line_width",	0.5 },
				{ "brim_width",						1 },
				{ "brim_ears",						1 },
				{ "brim_ears_max_angle",			91 }
			});
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, config);
            THEN("Four brim ears") {
                REQUIRE(print.brim().entities.size() == 4);
            }
        }

        WHEN("brim ears on a square but with a too small max angle") {
			config.set_deserialize_strict({
				{ "skirts",							0 },
				{ "initial_layer_line_width",	0.5 },
				{ "brim_width",						1 },
				{ "brim_ears",						1 },
				{ "brim_ears_max_angle",			89 }
				});
            THEN("no brim") {
		        Slic3r::Print print;
                Slic3r::Test::init_and_process_print({ TestMesh::cube_20x20x20 }, print, config);
                REQUIRE(print.brim().entities.size() == 0);
            }
        }
#endif

        WHEN("Object is plated with overhang support and a brim") {
        	config.set_deserialize_strict({
	            { "layer_height", 				0.4 },
	            { "initial_layer_print_height", 0.4 },
	            { "skirt_loops", 				1 },
	            { "skirt_distance", 			0 },
	            { "support_speed", 			99 },
	            { "perimeter_extruder", 		1 },
	            { "support_material_extruder", 	2 },
	            { "infill_extruder", 			3 },			// ensure that a tool command gets emitted.
	            { "slow_down_for_layer_cooling", false },		// to prevent speeds to be altered
	            { "initial_layer_speed", 		100 },			// to prevent speeds to be altered
				{ "machine_start_gcode",		"T[initial_tool]\n" }
        	});

            THEN("overhang generates?") {
            	//FIXME does it make sense?
                REQUIRE(! Slic3r::Test::slice({TestMesh::overhang}, config).empty());
            }

            // config.set("support_material", true);      // to prevent speeds to be altered

#if 0
			// This test is not finished.
            THEN("skirt length is large enough to contain object with support") {
                CHECK(config.opt_bool("support_material")); // test is not valid if support material is off
				std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
                double support_speed = config.opt<ConfigOptionFloat>("support_material_speed")->value * MM_PER_MIN;
				double skirt_length = 0.0;
				Points extrusion_points;
				int tool = -1;
				GCodeReader parser;
                parser.parse_buffer(gcode, [config, &extrusion_points, &tool, &skirt_length, support_speed] (Slic3r::GCodeReader& self, const Slic3r::GCodeReader::GCodeLine& line) {
                    // std::cerr << line.cmd() << "\n";
					if (boost::starts_with(line.cmd(), "T")) {
						tool = atoi(line.cmd().data() + 1);
					} else if (std::abs(self.z() - config.opt<ConfigOptionFloat>("first_layer_height")->value) < 1e-6) {
                        // on first layer
						if (line.extruding(self) && line.dist_XY(self) > 0) {
                            float speed = ( self.f() > 0 ?  self.f() : line.new_F(self));
                            // std::cerr << "Tool " << tool << "\n";
                            if (std::abs(speed - support_speed) < 1e-6 && tool == config.opt_int("perimeter_extruder") - 1) {
                                // Skirt uses first material extruder, support material speed.
                                skirt_length += line.dist_XY(self);
                            } else
                                extrusion_points.push_back(Slic3r::Point::new_scale(line.new_X(self), line.new_Y(self)));
                        }
                    }
                    if (std::abs(self.z() - 0.3) < 1e-6 || std::abs(line.new_Z(self) - 0.3) < 1e-6) {
                        if (line.extruding(self) && std::abs(self.f() - support_speed) < 1e-6) {
                        }
                    }
                });
                Slic3r::Polygon convex_hull = Slic3r::Geometry::convex_hull(extrusion_points);
                double hull_perimeter = unscale<double>(convex_hull.split_at_first_point().length());
                REQUIRE(skirt_length > hull_perimeter);
            }
#endif

        }
        WHEN("Large minimum skirt length is used.") {
            config.set("min_skirt_length", 20);
            THEN("Gcode generation doesn't crash") {
                REQUIRE(! Slic3r::Test::slice({TestMesh::cube_20x20x20}, config).empty());
            }
        }
    }
}
