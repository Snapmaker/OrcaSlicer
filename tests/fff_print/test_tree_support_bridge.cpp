#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <set>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Layer.hpp"
#include "test_data.hpp"

using namespace Slic3r;

namespace {

TriangleMesh short_bridge_mesh(double pillar_height = 6.)
{
    // Two pillars separated by a 6 mm span, joined by a 2 mm thick deck.
    TriangleMesh mesh   = make_cube(4., 10., pillar_height);
    TriangleMesh pillar = make_cube(4., 10., pillar_height);
    pillar.translate(10.f, 0.f, 0.f);
    mesh.merge(pillar);
    TriangleMesh deck = make_cube(14., 10., 2.);
    deck.translate(0.f, 0.f, float(pillar_height));
    mesh.merge(deck);
    return mesh;
}

bool contains_external_bridge(const ExtrusionEntity& entity)
{
    if (entity.role() == erBridgeInfill)
        return true;
    if (entity.is_collection()) {
        for (const ExtrusionEntity* child : static_cast<const ExtrusionEntityCollection&>(entity).entities)
            if (contains_external_bridge(*child))
                return true;
    }
    return false;
}

void collect_fill_paths(const ExtrusionEntity& entity, std::vector<const ExtrusionPath*>& paths)
{
    if (entity.is_collection()) {
        for (const ExtrusionEntity* child : static_cast<const ExtrusionEntityCollection&>(entity).entities)
            collect_fill_paths(*child, paths);
    } else if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        paths.push_back(path);
    } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        for (const ExtrusionPath& path : loop->paths)
            paths.push_back(&path);
    } else if (const auto* multi = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        for (const ExtrusionPath& path : multi->paths)
            paths.push_back(&path);
    }
}

double overlap_mm2(const ExtrusionEntity& first, const ExtrusionEntity& second)
{
    double area = 0.;
    for (const ExPolygon& polygon : intersection_ex(first.polygons_covered_by_width(), second.polygons_covered_by_width()))
        area += polygon.area() * SCALING_FACTOR * SCALING_FACTOR;
    return area;
}

void check_bridge_envelopes(const PrintObject& object, double expected_height = -1.)
{
    bool found_bridge = false;
    for (const Layer* layer : object.layers()) {
        for (const LayerRegion* region : layer->regions()) {
            std::vector<const ExtrusionPath*> paths;
            collect_fill_paths(region->fills, paths);
            for (const ExtrusionPath* path : paths) {
                if (path->role() != erBridgeInfill)
                    continue;
                found_bridge = true;
                if (expected_height > 0.)
                    CHECK(std::abs(path->height - expected_height) < EPSILON);
                for (const SupportLayer* support : object.support_layers()) {
                    if (overlap_mm2(*path, support->support_fills) > 1e-6)
                        CHECK((support->print_z <= layer->print_z - path->height + EPSILON ||
                               support->bottom_z() >= layer->print_z - EPSILON));
                }
            }
        }
    }
    REQUIRE(found_bridge);
}

} // namespace

TEST_CASE("Automatic support preserves external bridge infill with zero top Z distance",
          "[SupportMaterial][TreeSupport][NormalSupport][Bridge]")
{
    DynamicPrintConfig config = Test::default_print_config();
    // A zero bridge-length limit requests support under bridges; it must not
    // change their extrusion role to ordinary bottom infill for automatic supports.
    config.set_key_value("max_bridge_length", new ConfigOptionFloat(0.));
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stTreeAuto));
    config.set_key_value("support_top_z_distance", new ConfigOptionFloat(0.));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(2));
    config.set_key_value("support_critical_regions_only", new ConfigOptionBool(false));

    bool expect_support = true;
    SECTION("Automatic tree support with a zero top Z distance") {}
    SECTION("Automatic tree support with a nonzero top Z distance")
    {
        config.set_key_value("support_top_z_distance", new ConfigOptionFloat(0.2));
    }
    SECTION("A positive bridge-length limit also preserves external bridges")
    {
        config.set_key_value("max_bridge_length", new ConfigOptionFloat(10.));
        expect_support = false;
    }
    SECTION("Critical-only tree support cannot promise full bottom coverage")
    {
        config.set_key_value("support_critical_regions_only", new ConfigOptionBool(true));
        expect_support = false;
    }
    SECTION("Disabled support preserves external bridges")
    {
        config.set_key_value("enable_support", new ConfigOptionBool(false));
        expect_support = false;
    }
    SECTION("Normal automatic support with a zero gap preserves external bridges")
    {
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
        config.set_key_value("bridge_no_support", new ConfigOptionBool(false));
    }
    SECTION("Normal automatic support with a nonzero top Z distance")
    {
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
        config.set_key_value("support_top_z_distance", new ConfigOptionFloat(0.2));
    }
    SECTION("Normal automatic support without interface layers")
    {
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
        config.set_key_value("support_interface_top_layers", new ConfigOptionInt(0));
    }
    SECTION("Normal automatic support with solid interface spacing")
    {
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
        config.set_key_value("support_interface_spacing", new ConfigOptionFloat(0.));
    }
    SECTION("Normal automatic support skipping bridges")
    {
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
        config.set_key_value("bridge_no_support", new ConfigOptionBool(true));
        expect_support = false;
    }

    Print print;
    Model model;
    Test::init_print({short_bridge_mesh()}, print, model, config);
    print.process();

    REQUIRE(print.objects().size() == 1);
    REQUIRE_FALSE(print.objects().front()->layers().empty());
    if (expect_support)
        CHECK_FALSE(print.objects().front()->support_layers().empty());
    if (config.opt_bool("bridge_no_support"))
        CHECK(print.objects().front()->support_layers().empty());
    // The bed-contacting first layer must remain ordinary bottom infill.
    for (const LayerRegion* region : print.objects().front()->layers().front()->regions())
        CHECK_FALSE(contains_external_bridge(region->fills));

    bool found_bridge = false;
    for (const PrintObject* object : print.objects())
        for (const Layer* layer : object->layers())
            for (const LayerRegion* region : layer->regions())
                found_bridge |= contains_external_bridge(region->fills);

    CHECK(found_bridge);

    // Preview obtains its feature types from the exported G-code role tags. reserved_tag() picks its
    // dialect from a mutable static that do_export() rewrites to the print's BBL flag, so sync it
    // before building the expectation or the first section compares "; FEATURE: " against ";TYPE:".
    GCodeProcessor::s_IsBBLPrinter = print.is_BBL_printer();
    const std::string bridge_tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) +
                                   ExtrusionEntity::role_to_string(erBridgeInfill) + "\n";
    const std::string gcode = Test::gcode(print);
    CHECK(gcode.find(bridge_tag) != std::string::npos);
}

TEST_CASE("Zero-gap normal support clears the external bridge extrusion envelope", "[SupportMaterial][Bridge][BridgeEnvelope]")
{
    const bool   thick       = GENERATE(false, true);
    const bool   independent = GENERATE(false, true);
    const bool   extra_layer = GENERATE(false, true);
    const int    raft_layers = GENERATE(0, 3);
    const double bridge_flow = GENERATE(1., 1.21, 1.44);
    CAPTURE(thick, independent, extra_layer, raft_layers, bridge_flow);

    DynamicPrintConfig config = Test::default_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
    config.set_key_value("support_top_z_distance", new ConfigOptionFloat(0.));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("min_layer_height", new ConfigOptionFloats({0.08}));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(2));
    config.set_key_value("support_interface_spacing", new ConfigOptionFloat(0.));
    config.set_key_value("bridge_no_support", new ConfigOptionBool(false));
    config.set_key_value("thick_bridges", new ConfigOptionBool(thick));
    config.set_key_value("bridge_flow", new ConfigOptionFloat(bridge_flow));
    config.set_key_value("independent_support_layer_height", new ConfigOptionBool(independent));
    config.set_key_value("enable_extra_bridge_layer",
                         new ConfigOptionEnum<EnableExtraBridgeLayer>(extra_layer ? eblExternalBridgeOnly : eblDisabled));
    config.set_key_value("raft_layers", new ConfigOptionInt(raft_layers));

    Print print;
    Model model;
    Test::init_print({short_bridge_mesh()}, print, model, config);
    print.process();

    REQUIRE(print.objects().size() == 1);
    const PrintObject& object = *print.objects().front();
    REQUIRE_FALSE(object.support_layers().empty());
    std::set<coordf_t> bridge_layers;
    bool               found_contact = false;
    for (const Layer* layer : object.layers()) {
        for (const LayerRegion* region : layer->regions()) {
            std::vector<const ExtrusionPath*> paths;
            collect_fill_paths(region->fills, paths);
            for (const ExtrusionPath* path : paths) {
                if (path->role() != erBridgeInfill)
                    continue;
                bridge_layers.insert(layer->print_z);
                const Polygons bridge_area     = path->polygons_covered_by_width();
                double         highest_contact = 0.;
                for (const SupportLayer* support : object.support_layers()) {
                    if (support->bottom_z() >= layer->print_z - EPSILON)
                        continue;
                    const ExPolygons overlap      = intersection_ex(bridge_area, support->support_fills.polygons_covered_by_width());
                    double           overlap_area = 0.;
                    for (const ExPolygon& polygon : overlap)
                        overlap_area += polygon.area() * SCALING_FACTOR * SCALING_FACTOR;
                    // Ignore polygon clipping dust smaller than one square micron.
                    if (overlap_area <= 1e-6)
                        continue;
                    found_contact              = true;
                    highest_contact            = std::max(highest_contact, support->print_z);
                    const double bridge_bottom = layer->print_z - path->height;
                    CAPTURE(layer->print_z, path->height, support->print_z, overlap_area);
                    CHECK(support->print_z <= bridge_bottom + EPSILON);
                }
                if (bridge_layers.size() == 1) {
                    REQUIRE(highest_contact > 0.);
                    const Layer* expected = layer->lower_layer;
                    while (expected && expected->print_z > layer->print_z - path->height + EPSILON)
                        expected = expected->lower_layer;
                    REQUIRE(expected != nullptr);
                    CHECK(std::abs(highest_contact - expected->print_z) < EPSILON);
                }
            }
        }
    }
    REQUIRE(found_contact);
    for (const SupportLayer* support : object.support_layers()) {
        std::vector<const ExtrusionPath*> paths;
        collect_fill_paths(support->support_fills, paths);
        for (const ExtrusionPath* path : paths)
            CHECK(path->height >= 0.08 - EPSILON);
    }
    CHECK(bridge_layers.size() == (extra_layer ? 2 : 1));
    // A raft changes the object Z origin, not its first-layer surface semantics.
    for (const LayerRegion* region : object.layers().front()->regions())
        CHECK_FALSE(contains_external_bridge(region->fills));
}

TEST_CASE("Zero-gap bridge flow and angle changes rebuild their support", "[SupportMaterial][Bridge][BridgeEnvelope]")
{
    DynamicPrintConfig config = Test::default_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
    config.set_key_value("support_top_z_distance", new ConfigOptionFloat(0.));
    config.set_key_value("support_interface_spacing", new ConfigOptionFloat(0.));
    config.set_key_value("thick_bridges", new ConfigOptionBool(true));
    Print print;
    Model model;
    Test::init_print({short_bridge_mesh()}, print, model, config);
    print.process();
    config.set_key_value("bridge_flow", new ConfigOptionFloat(1.44));
    print.apply(model, config);
    CHECK_FALSE(print.objects().front()->is_step_done(posInfill));
    CHECK_FALSE(print.objects().front()->is_step_done(posSupportMaterial));
    print.process();
    check_bridge_envelopes(*print.objects().front(), 0.48);
    config.set_key_value("bridge_angle", new ConfigOptionFloat(45.));
    print.apply(model, config);
    CHECK_FALSE(print.objects().front()->is_step_done(posSupportMaterial));
    print.process();
    check_bridge_envelopes(*print.objects().front(), 0.48);
}

TEST_CASE("Zero-gap support preview preserves subsequent infill and G-code", "[SupportMaterial][Bridge][BridgeEnvelope][SupportPreview]")
{
    const bool   thick       = GENERATE(false, true);
    const int    raft_layers = GENERATE(0, 3);
    const bool   extra_layer = GENERATE(false, true);
    const double bridge_flow = GENERATE(1., 1.44);
    CAPTURE(thick, raft_layers, extra_layer, bridge_flow);
    DynamicPrintConfig config = Test::default_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
    config.set_key_value("support_top_z_distance", new ConfigOptionFloat(0.));
    config.set_key_value("thick_bridges", new ConfigOptionBool(thick));
    config.set_key_value("raft_layers", new ConfigOptionInt(raft_layers));
    config.set_key_value("bridge_flow", new ConfigOptionFloat(bridge_flow));
    config.set_key_value("enable_extra_bridge_layer",
                         new ConfigOptionEnum<EnableExtraBridgeLayer>(extra_layer ? eblExternalBridgeOnly : eblDisabled));
    auto fill_counts = [](const PrintObject& object) {
        std::array<size_t, 2> counts{};
        for (const Layer* layer : object.layers())
            for (const LayerRegion* region : layer->regions()) {
                std::vector<const ExtrusionPath*> paths;
                collect_fill_paths(region->fills, paths);
                counts[0] += paths.size();
                for (const ExtrusionPath* path : paths)
                    counts[1] += path->role() == erBridgeInfill;
            }
        return counts;
    };
    Print direct;
    Model direct_model;
    Test::init_print({short_bridge_mesh()}, direct, direct_model, config);
    direct.process();
    const auto expected = fill_counts(*direct.objects().front());
    REQUIRE(expected[0] > 0);
    REQUIRE(expected[1] > 0);

    Print print;
    Model model;
    Test::init_print({short_bridge_mesh()}, print, model, config);
    // Reusing the preview must not leave empty infill marked as complete.
    for (int preview = 0; preview < 2; ++preview) {
        print.get_object(0)->generate_support_preview();
        CHECK(print.objects().front()->is_step_done(posPerimeters));
        CHECK_FALSE(print.objects().front()->support_layers().empty());
        CHECK(fill_counts(*print.objects().front()) == expected);
    }
    print.process();
    CHECK_FALSE(print.objects().front()->support_layers().empty());
    CHECK(fill_counts(*print.objects().front()) == expected);
    const std::string gcode      = Test::gcode(print);
    const std::string bridge_tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) +
                                   ExtrusionEntity::role_to_string(erBridgeInfill) + "\n";
    CHECK(gcode.find(bridge_tag) != std::string::npos);
    check_bridge_envelopes(*print.objects().front());
}

TEST_CASE("Zero-gap bridge envelopes also trim a nearby raft", "[SupportMaterial][Bridge][BridgeEnvelope]")
{
    DynamicPrintConfig config = Test::default_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
    config.set_key_value("support_top_z_distance", new ConfigOptionFloat(0.));
    config.set_key_value("thick_bridges", new ConfigOptionBool(true));
    config.set_key_value("bridge_flow", new ConfigOptionFloat(1.44));
    config.set_key_value("raft_layers", new ConfigOptionInt(3));
    Print print;
    Model model;
    // The bridge bottom extends below the raft top, so trimming only ordinary support is insufficient.
    Test::init_print({short_bridge_mesh(0.2)}, print, model, config);
    print.process();
    REQUIRE_FALSE(print.objects().front()->support_layers().empty());
    check_bridge_envelopes(*print.objects().front(), 0.48);
}

TEST_CASE("Zero-gap contacts preserve ordinary walls beside different bridge heights", "[SupportMaterial][Bridge][BridgeEnvelope]")
{
    DynamicPrintConfig config = Test::default_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
    config.set_key_value("support_top_z_distance", new ConfigOptionFloat(0.));
    config.set_key_value("support_interface_spacing", new ConfigOptionFloat(0.));
    config.set_key_value("thick_bridges", new ConfigOptionBool(true));
    config.set_key_value("detect_overhang_wall", new ConfigOptionBool(false));
    Print print;
    Model model;
    Test::init_print({short_bridge_mesh()}, print, model, config);
    TriangleMesh adjacent = short_bridge_mesh();
    adjacent.translate(0.f, 10.f, 0.f);
    ModelVolume* volume = model.objects.front()->add_volume(adjacent);
    volume->config.set_key_value("bridge_flow", new ConfigOptionFloat(1.44));
    print.apply(model, config);
    print.process();
    const PrintObject& object = *print.objects().front();
    REQUIRE(object.layers().front()->regions().size() == 2);
    check_bridge_envelopes(object);
    std::set<int> heights;
    bool          ordinary_contact = false;
    for (const Layer* layer : object.layers()) {
        if (std::abs(layer->print_z - 6.2) > EPSILON)
            continue;
        for (const LayerRegion* region : layer->regions()) {
            std::vector<const ExtrusionPath*> paths;
            collect_fill_paths(region->fills, paths);
            for (const ExtrusionPath* path : paths)
                if (path->role() == erBridgeInfill)
                    heights.insert(int(std::round(path->height * 1000.)));
            collect_fill_paths(region->perimeters, paths);
            for (const ExtrusionPath* path : paths) {
                if (is_bridge(path->role()))
                    continue;
                for (const SupportLayer* support : object.support_layers())
                    if (std::abs(support->print_z - layer->bottom_z()) < EPSILON && overlap_mm2(*path, support->support_fills) > 1e-6)
                        ordinary_contact = true;
            }
        }
    }
    CHECK(heights == std::set<int>{400, 480});
    CHECK(ordinary_contact);
}
