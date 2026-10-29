#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Layer.hpp"
#include "test_data.hpp"

using namespace Slic3r;

namespace {

TriangleMesh short_bridge_mesh()
{
    // Two pillars separated by a 6 mm span, joined by a 2 mm thick deck.
    TriangleMesh mesh   = make_cube(4., 10., 6.);
    TriangleMesh pillar = make_cube(4., 10., 6.);
    pillar.translate(10.f, 0.f, 0.f);
    mesh.merge(pillar);
    TriangleMesh deck = make_cube(14., 10., 2.);
    deck.translate(0.f, 0.f, 6.f);
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

} // namespace

TEST_CASE("Tree support preserves external bridge infill with zero top Z distance", "[SupportMaterial][TreeSupport][Bridge]")
{
    DynamicPrintConfig config = Test::default_print_config();
    // A zero bridge-length limit requests support under bridges; it must not
    // change their extrusion role to ordinary bottom infill for tree supports.
    config.set_key_value("max_bridge_length", new ConfigOptionFloat(0.));
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stTreeAuto));
    config.set_key_value("support_top_z_distance", new ConfigOptionFloat(0.));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(2));
    config.set_key_value("support_critical_regions_only", new ConfigOptionBool(false));

    bool expect_bridge  = true;
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
    SECTION("Normal automatic support with a zero gap retains supported bottom infill")
    {
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
        config.set_key_value("bridge_no_support", new ConfigOptionBool(false));
        expect_bridge = false;
    }

    Print print;
    Model model;
    Test::init_print({short_bridge_mesh()}, print, model, config);
    print.process();

    REQUIRE(print.objects().size() == 1);
    REQUIRE_FALSE(print.objects().front()->layers().empty());
    if (expect_support)
        CHECK_FALSE(print.objects().front()->support_layers().empty());
    // The bed-contacting first layer must remain ordinary bottom infill.
    for (const LayerRegion* region : print.objects().front()->layers().front()->regions())
        CHECK_FALSE(contains_external_bridge(region->fills));

    bool found_bridge = false;
    for (const PrintObject* object : print.objects())
        for (const Layer* layer : object->layers())
            for (const LayerRegion* region : layer->regions())
                found_bridge |= contains_external_bridge(region->fills);

    CHECK(found_bridge == expect_bridge);

    // Preview obtains its feature types from the exported G-code role tags.
    const std::string bridge_tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) +
                                   ExtrusionEntity::role_to_string(erBridgeInfill) + "\n";
    const std::string gcode = Test::gcode(print);
    CHECK((gcode.find(bridge_tag) != std::string::npos) == expect_bridge);
}
