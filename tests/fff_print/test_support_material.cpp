#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include <algorithm>
#include <memory>
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"

#include "test_data.hpp" // get access to init_print, etc
#include "libslic3r/Support/SupportFins.hpp"

using namespace Slic3r::Test;
using namespace Slic3r;

TEST_CASE("SupportMaterial: Three raft layers created", "[SupportMaterial]")
{
	Slic3r::Print print;
	Slic3r::Test::init_and_process_print({ TestMesh::cube_20x20x20 }, print, {
		{ "enable_support",  true },
		{ "raft_layers",      3 }
		});
    REQUIRE(print.objects().front()->support_layers().size() == 3);
}

SCENARIO("SupportMaterial: support_layers_z and contact_distance", "[SupportMaterial]")
{
    // Box h = 20mm, hole bottom at 5mm, hole height 10mm (top edge at 15mm).
    TriangleMesh mesh = Slic3r::Test::mesh(Slic3r::Test::TestMesh::cube_with_hole);
    mesh.rotate_x(float(M_PI / 2));
//    mesh.write_binary("d:\\temp\\cube_with_hole.stl");

	auto check = [](Slic3r::Print &print, bool &first_support_layer_height_ok, bool &layer_height_minimum_ok, bool &layer_height_maximum_ok, bool &top_spacing_ok)
	{
        ConstSupportLayerPtrsAdaptor support_layers = print.objects().front()->support_layers();
        REQUIRE(!support_layers.empty());

		first_support_layer_height_ok = support_layers.front()->print_z == print.config().initial_layer_print_height.value;

		layer_height_minimum_ok = true;
		layer_height_maximum_ok = true;
		double min_layer_height = print.config().min_layer_height.values.front();
		double max_layer_height = print.config().nozzle_diameter.values.front();
		if (print.config().max_layer_height.values.front() > EPSILON)
			max_layer_height = std::min(max_layer_height, print.config().max_layer_height.values.front());
		for (size_t i = 1; i < support_layers.size(); ++ i) {
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z < min_layer_height - EPSILON)
				layer_height_minimum_ok = false;
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z > max_layer_height + EPSILON)
				layer_height_maximum_ok = false;
		}

#if 0
		double expected_top_spacing = print.default_object_config().layer_height + print.config().nozzle_diameter.get_at(0);
		bool wrong_top_spacing = 0;
        std::vector<coordf_t> top_z { 1.1 };
		for (coordf_t top_z_el : top_z) {
			// find layer index of this top surface.
			size_t layer_id = -1;
			for (size_t i = 0; i < support_z.size(); ++ i) {
				if (abs(support_z[i] - top_z_el) < EPSILON) {
					layer_id = i;
					i = static_cast<int>(support_z.size());
				}
			}

			// check that first support layer above this top surface (or the next one) is spaced with nozzle diameter
			if (abs(support_z[layer_id + 1] - support_z[layer_id] - expected_top_spacing) > EPSILON && 
				abs(support_z[layer_id + 2] - support_z[layer_id] - expected_top_spacing) > EPSILON) {
				wrong_top_spacing = 1;
			}
		}
		d = ! wrong_top_spacing;
#else
		top_spacing_ok = true;
#endif
	};

    GIVEN("A print object having one modelObject") {
        WHEN("First layer height = 0.4") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
				{ "enable_support",		true },
				{ "layer_height",		0.2 },
				{ "initial_layer_print_height", 0.4 },
                { "bridge_no_support", false },
			});
			bool a, b, c, d;
            check(print, a, b, c, d);
            THEN("First layer height is honored")					{ REQUIRE(a == true); }
            THEN("No null or negative support layers")				{ REQUIRE(b == true); }
            THEN("No layers thicker than nozzle diameter")			{ REQUIRE(c == true); }
//            THEN("Layers above top surfaces are spaced correctly")	{ REQUIRE(d == true); }
        }
        WHEN("Layer height = 0.2 and, first layer height = 0.3") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
				{ "enable_support",		true },
				{ "layer_height",		0.2 },
				{ "initial_layer_print_height", 0.3 },
                { "bridge_no_support", false },
            });
            bool a, b, c, d;
            check(print, a, b, c, d);
            THEN("First layer height is honored")					{ REQUIRE(a == true); }
            THEN("No null or negative support layers")				{ REQUIRE(b == true); }
            THEN("No layers thicker than nozzle diameter")			{ REQUIRE(c == true); }
//            THEN("Layers above top surfaces are spaced correctly")	{ REQUIRE(d == true); }
        }
        WHEN("Layer height = nozzle_diameter[0]") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
				{ "enable_support",		true },
				{ "layer_height",		0.2 },
				{ "initial_layer_print_height", 0.3 },
                { "bridge_no_support", false },
            });
            bool a, b, c, d;
            check(print, a, b, c, d);
            THEN("First layer height is honored")					{ REQUIRE(a == true); }
            THEN("No null or negative support layers")				{ REQUIRE(b == true); }
            THEN("No layers thicker than nozzle diameter")			{ REQUIRE(c == true); }
//            THEN("Layers above top surfaces are spaced correctly")	{ REQUIRE(d == true); }
        }
    }
}

#if 0
// Test 8.
TEST_CASE("SupportMaterial: forced support is generated", "[SupportMaterial]")
{
    // Create a mesh & modelObject.
    TriangleMesh mesh = TriangleMesh::make_cube(20, 20, 20);

    Model model = Model();
    ModelObject *object = model.add_object();
    object->add_volume(mesh);
    model.add_default_instances();
    model.align_instances_to_origin();

    Print print = Print();

    std::vector<coordf_t> contact_z = {1.9};
    std::vector<coordf_t> top_z = {1.1};
    print.default_object_config.support_material_enforce_layers = 100;
    print.default_object_config.support_material = 0;
    print.default_object_config.layer_height = 0.2;
    print.default_object_config.set_deserialize("first_layer_height", "0.3");

    print.add_model_object(model.objects[0]);
    print.objects.front()->_slice();

    SupportMaterial *support = print.objects.front()->_support_material();
    auto support_z = support->support_layers_z(contact_z, top_z, print.default_object_config.layer_height);

    bool check = true;
    for (size_t i = 1; i < support_z.size(); i++) {
        if (support_z[i] - support_z[i - 1] <= 0)
            check = false;
    }

    REQUIRE(check == true);
}

// TODO
bool test_6_checks(Print& print)
{
	bool has_bridge_speed = true;

	// Pre-Processing.
	PrintObject* print_object = print.objects.front();
	print_object->infill();
	SupportMaterial* support_material = print.objects.front()->_support_material();
	support_material->generate(print_object);
	// TODO but not needed in test 6 (make brims and make skirts).

	// Exporting gcode.
	// TODO validation found in Simple.pm


	return has_bridge_speed;
}

// Test 6.
SCENARIO("SupportMaterial: Checking bridge speed", "[SupportMaterial]")
{
    GIVEN("Print object") {
        // Create a mesh & modelObject.
        TriangleMesh mesh = TriangleMesh::make_cube(20, 20, 20);

        Model model = Model();
        ModelObject *object = model.add_object();
        object->add_volume(mesh);
        model.add_default_instances();
        model.align_instances_to_origin();

        Print print = Print();
        print.config.brim_width = 0;
        print.config.skirts = 0;
        print.config.skirts = 0;
        print.default_object_config.support_material = 1;
        print.default_region_config.top_solid_layers = 0; // so that we don't have the internal bridge over infill.
        print.default_region_config.bridge_speed = 99;
        print.config.cooling = 0;
        print.config.set_deserialize("first_layer_speed", "100%");

        WHEN("support_material_contact_distance = 0.2") {
            print.default_object_config.support_material_contact_distance = 0.2;
            print.add_model_object(model.objects[0]);

            bool check = test_6_checks(print);
            REQUIRE(check == true); // bridge speed is used.
        }

        WHEN("support_material_contact_distance = 0") {
            print.default_object_config.support_material_contact_distance = 0;
            print.add_model_object(model.objects[0]);

            bool check = test_6_checks(print);
            REQUIRE(check == true); // bridge speed is not used.
        }

        WHEN("support_material_contact_distance = 0.2 & raft_layers = 5") {
            print.default_object_config.support_material_contact_distance = 0.2;
            print.default_object_config.raft_layers = 5;
            print.add_model_object(model.objects[0]);

            bool check = test_6_checks(print);
            REQUIRE(check == true); // bridge speed is used.
        }

        WHEN("support_material_contact_distance = 0 & raft_layers = 5") {
            print.default_object_config.support_material_contact_distance = 0;
            print.default_object_config.raft_layers = 5;
            print.add_model_object(model.objects[0]);

            bool check = test_6_checks(print);

            REQUIRE(check == true); // bridge speed is not used.
        }
    }
}

#endif

// A 30 x 30 x 2 mm slab held 10 mm above the bed by a 10 x 10 mm stem: the slab hangs over the bed
// all around the stem, and fins crossing the stem end right at its walls.
static TriangleMesh support_table()
{
    TriangleMesh model = make_cube(30, 30, 2);  model.translate(0, 0, 10);   // slab z 10..12
    TriangleMesh stem  = make_cube(10, 10, 11); stem.translate(10, 10, 0);   // stem z 0..11
    model.merge(stem);
    return model;
}

// Y of the area centroid of `expolys`.
static double area_centroid_y(const ExPolygons &expolys, double &area)
{
    double sum = 0.;
    area = 0.;
    for (const ExPolygon &expoly : expolys) {
        sum  += expoly.area() * get_extents(expoly).center().y();
        area += expoly.area();
    }
    return area > 0. ? sum / area : 0.;
}

// A 20 mm cube tipped 45 degrees about X onto an edge: its lower faces are exactly 45 degrees.
static TriangleMesh cube_on_edge()
{
    TriangleMesh cube = make_cube(20, 20, 20);
    cube.translate(-10, -10, -10);
    cube.rotate_x(float(M_PI / 4));
    cube.translate(0, 0, float(- cube.bounding_box().min.z()));
    return cube;
}

TEST_CASE("SupportMaterial: fin support is built from walls no thicker than the fin thickness", "[SupportMaterial][Fins]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ support_table() }, print, {
        { "enable_support",            1 },
        { "support_type",              "fins" },
        { "layer_height",              0.2 },
        { "support_fin_spacing",       6 },
        // Fins are whole two-line loops; with the 0.4 mm default line this is one loop, about 0.7 mm.
        { "support_fin_thickness",     0.7 },
        { "support_fin_tine_spacing", 0 },
    });
    size_t checked = 0;
    for (const SupportLayer *layer : print.objects().front()->support_layers()) {
        // The fin foot within 1 mm of the bed is wider by design.
        if (layer->print_z < 1.5)
            continue;
        // Any region wider than one loop (at most 0.8 mm) survives an opening by half of that width.
        CAPTURE(layer->print_z);
        CHECK(opening(layer->support_islands, float(scale_(0.5 * 0.8 + 0.05))).empty());
        ++ checked;
    }
    REQUIRE(checked > 0);
}

TEST_CASE("SupportMaterial: fin support touches the object only on tine layers", "[SupportMaterial][Fins]")
{
    const double tine_spacing = 3.;
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ support_table() }, print, {
        { "enable_support",             1 },
        { "support_type",               "fins" },
        { "layer_height",               0.2 },
        { "support_object_xy_distance", 0.3 },
        { "support_fin_spacing",        4 },
        { "support_fin_thickness",      1.2 },
        { "support_fin_tine_spacing",   tine_spacing },
        { "support_fin_tine_depth",     0.3 },
    });
    const PrintObject &object = *print.objects().front();
    const std::vector<bool> tine_row = fin_tine_layers(object);
    size_t tine_layers = 0;
    for (const SupportLayer *layer : object.support_layers()) {
        const Layer *object_layer = object.get_layer_at_printz(layer->print_z, EPSILON);
        if (object_layer == nullptr)
            continue;
        // Ignore numerical slivers; a tine is about 0.8 x 0.3 mm = 0.24 mm^2.
        const bool touches = area(intersection_ex(layer->support_islands, object_layer->lslices)) > scale_(scale_(0.05));
        CAPTURE(layer->print_z);
        if (tine_row[object_layer->id()])
            tine_layers += touches;
        else
            CHECK_FALSE(touches);
    }
    REQUIRE(tine_layers > 0);
}

TEST_CASE("SupportMaterial: fin support without tines never touches the object", "[SupportMaterial][Fins]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ support_table() }, print, {
        { "enable_support",            1 },
        { "support_type",              "fins" },
        { "layer_height",              0.2 },
        { "support_fin_tine_spacing", 0 },
    });
    const PrintObject &object = *print.objects().front();
    REQUIRE(! object.support_layers().empty());
    for (const SupportLayer *layer : object.support_layers())
        if (const Layer *object_layer = object.get_layer_at_printz(layer->print_z, EPSILON)) {
            CAPTURE(layer->print_z);
            CHECK(area(intersection_ex(layer->support_islands, object_layer->lslices)) < scale_(scale_(0.05)));
        }
}

// Normal support at a 30 degree threshold finds nothing on 45 degree faces; fins do not use the threshold.
TEST_CASE("SupportMaterial: fin support holds a cube tipped onto an edge regardless of the overhang threshold", "[SupportMaterial][Fins]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ cube_on_edge() }, print, {
        { "enable_support",          1 },
        { "support_type",            "fins" },
        { "layer_height",            0.2 },
        { "support_threshold_angle", 30 },
    });
    const PrintObject &object = *print.objects().front();
    REQUIRE(! object.support_layers().empty());
    // The cube is widest at 14.1 mm (20 mm * sqrt(2) / 2); fins reach most of the way up.
    REQUIRE(object.support_layers().back()->print_z > 10.);

    // The fins cross the edge the cube rests on (along X), so each fin runs along Y.
    const SupportLayer *mid = nullptr;
    for (const SupportLayer *layer : object.support_layers())
        if (layer->print_z > 5.)
            { mid = layer; break; }
    REQUIRE(mid != nullptr);
    REQUIRE(! mid->support_islands.empty());
    for (const ExPolygon &fin : mid->support_islands) {
        const BoundingBox b = get_extents(fin);
        CHECK(b.size().y() > b.size().x());
    }
}

TEST_CASE("SupportMaterial: fin support stops at the fin height", "[SupportMaterial][Fins]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ cube_on_edge() }, print, {
        { "enable_support",     1 },
        { "support_type",       "fins" },
        { "layer_height",       0.2 },
        { "support_fin_height", 4 },
    });
    const PrintObject &object = *print.objects().front();
    REQUIRE(! object.support_layers().empty());
    REQUIRE(object.support_layers().back()->print_z <= 4. + EPSILON);
}

TEST_CASE("SupportMaterial: fin support on a raft is rejected", "[SupportMaterial][Fins]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({ cube_on_edge() }, print, model, {
        { "enable_support", 1 },
        { "support_type",   "fins" },
        { "raft_layers",    2 },
    });
    const StringObjectException error = print.validate();
    CHECK(error.opt_key == "raft_layers");
    CHECK(error.string.find("Fin support") != std::string::npos);
}

TEST_CASE("SupportMaterial: fin support on the lean side only keeps one side of a balanced part", "[SupportMaterial][Fins]")
{
    // The cube on its edge is balanced, so both sides hang over the bed; only the larger (here, either) side keeps fins.
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ cube_on_edge() }, print, {
        { "enable_support",             1 },
        { "support_type",               "fins" },
        { "layer_height",               0.2 },
        { "support_fin_lean_side_only", 1 },
    });
    const PrintObject &object = *print.objects().front();
    REQUIRE(! object.support_layers().empty());
    // The cube rests on an edge along X at the middle of its footprint in Y.
    double area;
    const double edge_y = area_centroid_y(object.layers().front()->lslices, area);
    size_t below = 0, above = 0;
    for (const SupportLayer *layer : object.support_layers())
        for (const ExPolygon &fin : layer->support_islands)
            ++ (fin.contour.centroid().y() < edge_y ? below : above);
    CAPTURE(below, above);
    CHECK(below + above > 0);
    CHECK(std::min(below, above) == 0);
}

static double support_area_at(const PrintObject &object, double z)
{
    for (const SupportLayer *layer : object.support_layers())
        if (layer->print_z > z)
            return area(layer->support_islands);
    return 0.;
}

TEST_CASE("SupportMaterial: fin support with cross fins adds bracing across the main fins", "[SupportMaterial][Fins]")
{
    auto slice_table = [](double cross) {
        auto print = std::make_unique<Slic3r::Print>();
        Slic3r::Test::init_and_process_print({ support_table() }, *print, {
            { "enable_support",            1 },
            { "support_type",              "fins" },
            { "layer_height",              0.2 },
            { "support_fin_tine_spacing", 0 },
            { "support_fin_cross_spacing", cross },
        });
        return print;
    };
    const auto plain  = slice_table(0.);
    const auto braced = slice_table(6.);
    // Halfway up the stem, the scaffold covers more area than the parallel fins alone.
    CHECK(support_area_at(*braced->objects().front(), 5.) > support_area_at(*plain->objects().front(), 5.));
}

// Support layers carrying interface extrusions.
static size_t fin_interface_layer_count(const PrintObject &object)
{
    size_t count = 0;
    for (const SupportLayer *layer : object.support_layers()) {
        const ExtrusionEntityCollection flat = layer->support_fills.flatten();
        count += std::any_of(flat.entities.begin(), flat.entities.end(),
                             [](const ExtrusionEntity *e) { return e->role() == ExtrusionRole::erSupportMaterialInterface; });
    }
    return count;
}

TEST_CASE("SupportMaterial: fin support prints interface layers under the object only when asked", "[SupportMaterial][Fins]")
{
    auto interface_layers = [](int layers) {
        Slic3r::Print print;
        Slic3r::Test::init_and_process_print({ support_table() }, print, {
            { "enable_support",               1 },
            { "support_type",                 "fins" },
            { "layer_height",                 0.2 },
            { "support_fin_interface_layers", layers },
        });
        return fin_interface_layer_count(*print.objects().front());
    };
    CHECK(interface_layers(0) == 0);
    // Two interface layers under the slab, above its Z gap.
    CHECK(interface_layers(2) >= 2);
}

TEST_CASE("SupportMaterial: fin support tines are packed near the bed, then follow the tine spacing", "[SupportMaterial][Fins]")
{
    // The cube on its edge is 28 mm tall, sliced at 0.2 mm.
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({ cube_on_edge() }, print, model, {
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "support_fin_tine_spacing",   5 },
    });
    print.process();
    const PrintObject &object = *print.objects().front();
    const auto layers = object.layers();
    const std::vector<bool> rows = fin_tine_layers(object);
    std::vector<double> z;
    for (size_t i = 0; i < rows.size(); ++ i)
        if (rows[i])
            z.push_back(layers[i]->print_z);
    REQUIRE(z.size() >= 6);
    // The first tine sits just above the 1 mm foot; the gaps widen, then settle at 5 mm.
    CHECK(z.front() > 1.0);
    CHECK(z.front() < 1.5);
    CHECK(z[1] - z[0] < 1.5);
    CHECK(std::abs(z.back() - z[z.size() - 2] - 5.) < 0.25);
    // Without the extra rows near the bed, every gap is the tine spacing.
    const_cast<PrintObjectConfig&>(object.config()).support_fin_tine_base_rows.value = false;
    const std::vector<bool> plain = fin_tine_layers(object);
    CHECK(std::count(plain.begin(), plain.end(), true) < std::count(rows.begin(), rows.end(), true));
}

