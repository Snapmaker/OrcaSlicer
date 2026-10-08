#include <catch2/catch.hpp>

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

SCENARIO("PrintObject: object layer heights", "[PrintObject]") {
    GIVEN("20mm cube and default initial config, initial layer height of 2mm") {
        WHEN("generate_object_layers() is called for 2mm layer heights and nozzle diameter of 3mm") {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
        		{ "initial_layer_print_height", 2 },
				{ "layer_height", 		2 },
	            { "nozzle_diameter", 	3 }
	        });
            ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
            THEN("The output vector has 10 entries") {
                REQUIRE(layers.size() == 10);
            }
            AND_THEN("Each layer is approximately 2mm above the previous Z") {
                coordf_t last = 0.0;
                for (size_t i = 0; i < layers.size(); ++ i) {
                    REQUIRE((layers[i]->print_z - last) == Approx(2.0));
                    last = layers[i]->print_z;
                }
            }
        }
        WHEN("generate_object_layers() is called for 10mm layer heights and nozzle diameter of 11mm") {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
        		{ "initial_layer_print_height", 2 },
				{ "layer_height", 		10 },
	            { "nozzle_diameter", 	11 }
	        });
            ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
			THEN("The output vector has 3 entries") {
                REQUIRE(layers.size() == 3);
            }
            AND_THEN("Layer 0 is at 2mm") {
                REQUIRE(layers.front()->print_z == Approx(2.0));
            }
            AND_THEN("Layer 1 is at 12mm") {
                REQUIRE(layers[1]->print_z == Approx(12.0));
            }
        }
        WHEN("generate_object_layers() is called for 15mm layer heights and nozzle diameter of 16mm") {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
        		{ "initial_layer_print_height", 2 },
				{ "layer_height", 		15 },
	            { "nozzle_diameter", 	16 }
	        });
            ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
			THEN("The output vector has 2 entries") {
                REQUIRE(layers.size() == 2);
            }
            AND_THEN("Layer 0 is at 2mm") {
                REQUIRE(layers[0]->print_z == Approx(2.0));
            }
            AND_THEN("Layer 1 is at 17mm") {
                REQUIRE(layers[1]->print_z == Approx(17.0));
            }
        }
#if 0
        WHEN("generate_object_layers() is called for 15mm layer heights and nozzle diameter of 5mm") {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
        		{ "initial_layer_print_height", 2 },
				{ "layer_height", 		15 },
	            { "nozzle_diameter", 	5 }
	        });
			const std::vector<Slic3r::Layer*> &layers = print.objects().front()->layers();
			THEN("The layer height is limited to 5mm.") {
                CHECK(layers.size() == 5);
                coordf_t last = 2.0;
                for (size_t i = 1; i < layers.size(); i++) {
                    REQUIRE((layers[i]->print_z - last) == Approx(5.0));
                    last = layers[i]->print_z;
                }
            }
        }
#endif
    }
}

static TriangleMesh internal_bridge_step()
{
    // Orca: The smaller tower leaves a shoulder whose solid skin needs internal bridges
    // over the sparse infill in the base, without relying on an external model file.
    TriangleMesh mesh = make_cube(30, 24, 3);
    TriangleMesh tower = make_cube(14, 10, 1);
    tower.translate(8, 7, 3);
    mesh.merge(tower);
    return mesh;
}

static DynamicPrintConfig internal_bridge_config(const std::string &pattern)
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"sparse_infill_pattern", pattern},
                                   {"sparse_infill_density", "15%"},
                                   {"infill_direction", 45},
                                   {"internal_bridge_angle", 0},
                                   {"thick_internal_bridges", true},
                                   {"top_shell_layers", 3},
                                   {"bottom_shell_layers", 2},
                                   {"top_shell_thickness", 0},
                                   {"bottom_shell_thickness", 0},
                                   {"layer_height", 0.2},
                                   {"initial_layer_print_height", 0.2}});
    return config;
}

TEST_CASE("Internal bridge angles follow the lower infill layer and model rotation", "[PrintObject][InternalBridge][Regression]")
{
    const std::string pattern = GENERATE("hilbertcurve", "octagramspiral");
    const double rotation = GENERATE(23., -123.);
    const std::vector<double> cycle{10., 30., 70.};
    auto config = internal_bridge_config(pattern);
    config.set_deserialize_strict({{"sparse_infill_rotate_template", "10,30,70"},
                                   {"align_infill_direction_to_model", true}});
    Print print;
    Model model;
    init_print({internal_bridge_step()}, print, model, config, false);
    model.objects.front()->instances.front()->set_rotation(Vec3d(0., 0., Geometry::deg2rad(rotation)));
    print.apply(model, config);
    print.process();
    const PrintObject &object = *print.objects().front();
    size_t bridges = 0;
    for (size_t i = 1; i < object.layer_count(); ++i) {
        // Orca: The support is one layer below the bridge. Check the template and model
        // rotation together, including normalization when the resulting angle is negative.
        double expected = std::fmod(cycle[(i - 1) % cycle.size()] + 90. + rotation, 180.);
        if (expected < 0.) expected += 180.;
        for (const LayerRegion *region : object.get_layer(i)->regions())
            for (const Surface *surface : region->fill_surfaces.filter_by_type(stInternalBridge)) {
                CAPTURE(pattern, rotation, i);
                CHECK_THAT(Geometry::rad2deg(surface->bridge_angle), Catch::Matchers::WithinAbs(expected, 0.001));
                ++bridges;
            }
    }
    REQUIRE(bridges > 0);
}

TEST_CASE("Turning infill does not replace the anchors of another region", "[PrintObject][InternalBridge][Regression]")
{
    // Orca: Keep the right-hand region fixed while changing the left-hand pattern in the
    // same object. Its bridge areas must be independent of a previous candidate's anchors.
    auto right_bridges = [](const std::string &left_pattern) {
        auto config = internal_bridge_config(left_pattern);
        Print print;
        Model model;
        init_print({internal_bridge_step()}, print, model, config, false);
        TriangleMesh right = internal_bridge_step();
        right.translate(50, 0, 0);
        ModelVolume *volume = model.objects.front()->add_volume(std::move(right));
        volume->config.set_key_value("sparse_infill_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
        volume->config.set_key_value("infill_direction", new ConfigOptionFloat(17.));
        print.apply(model, config);
        print.process();
        std::map<size_t, Polygons> result;
        const PrintObject &object = *print.objects().front();
        for (size_t i = 0; i < object.layer_count(); ++i)
            for (const LayerRegion *region : object.get_layer(i)->regions())
                if (region->region().config().infill_direction == 17.)
                    polygons_append(result[i], to_polygons(region->fill_surfaces.filter_by_type(stInternalBridge)));
        return result;
    };
    const auto baseline = right_bridges("rectilinear");
    const auto actual = right_bridges(GENERATE("hilbertcurve", "octagramspiral"));
    REQUIRE(actual.size() == baseline.size());
    double total_area = 0.;
    for (const auto &[layer, expected] : baseline) {
        CAPTURE(layer);
        const auto &polys = actual.at(layer);
        CHECK(area(diff(expected, polys)) < scaled<double>(1.) * scaled<double>(1.) * 1e-6);
        CHECK(area(diff(polys, expected)) < scaled<double>(1.) * scaled<double>(1.) * 1e-6);
        total_area += area(expected);
    }
    REQUIRE(total_area > 0.);
}

namespace {

const double tab_top_z = 5.0;
const double narrow_wall = 1.186;

Print &tube_with_tab(Print &print, Model &model, const DynamicPrintConfig &config)
{
    ModelObject *object = model.add_object();
    object->name = "tube_with_tab.stl";
    object->add_volume(make_cube(20., 30., 10.), ModelVolumeType::MODEL_PART, false);
    TriangleMesh tab = make_cube(20., 8.5, 5.);
    tab.translate(0.f, -8.f, 0.f);
    object->add_volume(std::move(tab), ModelVolumeType::MODEL_PART, false);
    TriangleMesh bore = make_cube(20. - 2. * narrow_wall, 30. - 2. * narrow_wall, 12.);
    bore.translate(float(narrow_wall), float(narrow_wall), -1.f);
    object->add_volume(std::move(bore), ModelVolumeType::NEGATIVE_VOLUME, false);
    object->add_instance();
    object->ensure_on_bed();

    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    return print;
}

DynamicPrintConfig narrow_wall_config(bool only_one_wall_top)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "wall_generator",        "arachne" },
        { "wall_loops",            2 },
        { "nozzle_diameter",       "0.4" },
        { "line_width",            0.42 },
        { "outer_wall_line_width", 0.42 },
        { "inner_wall_line_width", 0.45 },
        { "min_bead_width",        "85%" },
        { "precise_outer_wall",    true },
        { "wall_sequence",         "inner wall/outer wall" },
        { "only_one_wall_top",     only_one_wall_top },
    });
    return config;
}

double perimeter_length_at(const Print &print, double print_z)
{
    for (const Layer *layer : print.objects().front()->layers()) {
        if (std::abs(layer->print_z - print_z) > EPSILON)
            continue;
        double length = 0.;
        for (const LayerRegion *region : layer->regions()) {
            const ExtrusionEntityCollection walls = region->perimeters.flatten();
            for (const ExtrusionEntity *entity : walls.entities)
                length += unscaled<double>(entity->length());
        }
        return length;
    }
    return 0.;
}

double far_wall_inner_wall_length(const Print &print, double print_z)
{
    for (const Layer *layer : print.objects().front()->layers()) {
        if (std::abs(layer->print_z - print_z) > EPSILON)
            continue;
        BoundingBox band = get_extents(layer->lslices);
        band.min.y()     = band.max.y() - scaled<coord_t>(3.);

        Polylines inner_walls;
        auto      collect = [&inner_walls](const ExtrusionPaths &paths) {
            for (const ExtrusionPath &path : paths)
                if (path.role() == erPerimeter)
                    inner_walls.emplace_back(path.as_polyline());
        };
        for (const LayerRegion *region : layer->regions()) {
            const ExtrusionEntityCollection walls = region->perimeters.flatten();
            for (const ExtrusionEntity *entity : walls.entities) {
                if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(entity))
                    collect(loop->paths);
                else if (const auto *multi_path = dynamic_cast<const ExtrusionMultiPath*>(entity))
                    collect(multi_path->paths);
                else if (const auto *path = dynamic_cast<const ExtrusionPath*>(entity))
                    collect({ *path });
            }
        }
        return unscaled<double>(total_length(intersection_pl(inner_walls, band.polygon())));
    }
    return 0.;
}

} // namespace

TEST_CASE("Only one wall on top surfaces keeps the inner walls of narrow walls away from the top surface", "[PrintObject][Perimeters]")
{
    struct TabTopLayer {
        double perimeters;
        double far_wall_inner_walls;
    };
    auto tab_top_layer_for = [](bool only_one_wall_top) {
        Print print;
        Model model;
        tube_with_tab(print, model, narrow_wall_config(only_one_wall_top));
        print.process();
        REQUIRE_FALSE(print.objects().empty());
        return TabTopLayer{ perimeter_length_at(print, tab_top_z), far_wall_inner_wall_length(print, tab_top_z) };
    };

    const TabTopLayer plain    = tab_top_layer_for(false);
    const TabTopLayer one_wall = tab_top_layer_for(true);

    REQUIRE(plain.far_wall_inner_walls > 10.);
    CHECK(one_wall.perimeters < plain.perimeters);
    CHECK_THAT(one_wall.far_wall_inner_walls, Catch::Matchers::WithinAbs(plain.far_wall_inner_walls, 1.0));
}

// Orca #16177: a hole in an internal-bridge area is its own (CW) polygon. Filtering polygon-by-polygon
// dropped the hole when it did not touch internal_unsupported_area, so the next layer saw unsupported
// infill over the hole and stacked a second internal bridge. Filtering whole ExPolygons keeps the hole.
TEST_CASE("Internal bridge areas keep holes so they are not re-bridged on the next layer", "[PrintObject][InternalBridge]")
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"sparse_infill_density", "15%"},
                                   {"thick_internal_bridges", true},
                                   {"top_shell_layers", 3},
                                   {"bottom_shell_layers", 2},
                                   {"top_shell_thickness", 0},
                                   {"bottom_shell_thickness", 0},
                                   {"layer_height", 0.2},
                                   {"initial_layer_print_height", 0.2}});
    Print print;
    Model model;
    init_print({TestMesh::cube_with_hole}, print, model, config, false);
    print.process();
    REQUIRE_FALSE(print.objects().empty());
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.layer_count() > 2);

    const double max_overlap = scaled<double>(1.) * scaled<double>(1.) * 1e-3;
    for (size_t i = 0; i + 1 < object.layer_count(); ++i) {
        Polygons this_bridge;
        Polygons next_bridge;
        for (const LayerRegion *region : object.get_layer(i)->regions())
            polygons_append(this_bridge, to_polygons(region->fill_surfaces.filter_by_type(stInternalBridge)));
        for (const LayerRegion *region : object.get_layer(i + 1)->regions())
            polygons_append(next_bridge, to_polygons(region->fill_surfaces.filter_by_type(stInternalBridge)));
        if (this_bridge.empty() || next_bridge.empty())
            continue;
        CAPTURE(i);
        CHECK(area(intersection(this_bridge, next_bridge)) < max_overlap);
    }
}
