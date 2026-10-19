#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/UsedFilaments.hpp"

#include <algorithm>
#include <vector>

using namespace Slic3r;

// The filaments a plate counts before slicing decide whether the prime tower is drawn and how
// big it is reserved. Four extruders with 0.6 / 0.4 / 0.4 / 0.2 mm nozzles, PETG on the 0.2 mm
// nozzle, the object on filament 1 and its sparse infill on filament 2.

namespace {

DynamicPrintConfig four_filament_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("nozzle_diameter",           new ConfigOptionFloats({0.6, 0.4, 0.4, 0.2}));
    config.set_key_value("filament_diameter",         new ConfigOptionFloats({1.75, 1.75, 1.75, 1.75}));
    config.set_key_value("filament_type",             new ConfigOptionStrings({"PLA", "PLA", "PLA", "PETG"}));
    config.set_key_value("filament_soluble",          new ConfigOptionBools({false, false, false, false}));
    config.set_key_value("sparse_infill_filament_id", new ConfigOptionInt(2));
    config.set_key_value("sparse_infill_density",     new ConfigOptionPercent(30.));
    config.set_key_value("enable_support",            new ConfigOptionBool(true));
    config.set_key_value("support_filament",          new ConfigOptionInt(0));
    config.set_key_value("support_interface_filament", new ConfigOptionInt(0));
    config.set_key_value("support_nozzle_diameter",   new ConfigOptionFloat(0.2));
    config.set_key_value("support_base_material",     new ConfigOptionString("PETG"));
    config.set_key_value("support_interface_material", new ConfigOptionString("PETG"));
    return config;
}

ModelObject *add_cube(Model &model)
{
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    return object;
}

std::vector<int> used_filaments(const ModelObject &object, const ConfigBase &print_config, const ConfigBase &filament_config)
{
    std::vector<int> filaments;
    append_model_object_filaments(object, print_config, filament_config, filaments);
    std::sort(filaments.begin(), filaments.end());
    filaments.erase(std::unique(filaments.begin(), filaments.end()), filaments.end());
    return filaments;
}

} // namespace

TEST_CASE("A support filament picked by nozzle size and material counts as used", "[UsedFilaments]")
{
    const DynamicPrintConfig config = four_filament_config();
    Model        model;
    ModelObject *object = add_cube(model);

    SECTION("the PETG on the 0.2 mm nozzle prints the support next to the object and infill filaments") {
        CHECK(model_object_has_restricted_support(*object, config));
        CHECK(used_filaments(*object, config, config) == std::vector<int>{1, 2, 4});
    }

    SECTION("without support the filament is not counted") {
        object->config.set_key_value("enable_support", new ConfigOptionBool(false));
        CHECK_FALSE(model_object_has_restricted_support(*object, config));
        CHECK(used_filaments(*object, config, config) == std::vector<int>{1, 2});
    }

    SECTION("a raft alone prints the support filament too") {
        object->config.set_key_value("enable_support", new ConfigOptionBool(false));
        object->config.set_key_value("raft_layers", new ConfigOptionInt(2));
        CHECK(used_filaments(*object, config, config) == std::vector<int>{1, 2, 4});
    }

    SECTION("an explicit support filament wins over the restriction, the interface still resolves") {
        object->config.set_key_value("support_filament", new ConfigOptionInt(3));
        CHECK(used_filaments(*object, config, config) == std::vector<int>{1, 2, 3, 4});
    }

    SECTION("a material no filament is loaded with adds nothing") {
        DynamicPrintConfig abs = config;
        abs.set_key_value("support_base_material",      new ConfigOptionString("ABS"));
        abs.set_key_value("support_interface_material", new ConfigOptionString("ABS"));
        CHECK(used_filaments(*object, abs, abs) == std::vector<int>{1, 2});
    }

    SECTION("without nozzle diameters and filament types the restriction resolves to nothing") {
        DynamicPrintConfig print_only;
        print_only.apply_only(config, {"enable_support", "support_nozzle_diameter", "support_base_material",
                                       "support_interface_material", "sparse_infill_filament_id"});
        CHECK(used_filaments(*object, print_only, print_only) == std::vector<int>{1, 2});
        CHECK(used_filaments(*object, print_only, config) == std::vector<int>{1, 2, 4});
    }
}

TEST_CASE("The support filament resolves as the print resolves it", "[UsedFilaments]")
{
    DynamicPrintConfig config = four_filament_config();
    CHECK(resolve_restricted_support_filament(config, {0.2, "PETG"}) == 4u);
    CHECK(resolve_restricted_support_filament(config, {0.4, ""}) == 2u);
    CHECK(resolve_restricted_support_filament(config, {0., "PLA"}) == 1u);
    CHECK(resolve_restricted_support_filament(config, {0., ""}) == 0u);
    CHECK(resolve_restricted_support_filament(config, {0.8, ""}) == 0u);
    // A soluble filament only when no other passes.
    config.set_key_value("filament_soluble", new ConfigOptionBools({true, false, true, false}));
    CHECK(resolve_restricted_support_filament(config, {0., "PLA"}) == 2u);
    CHECK(resolve_restricted_support_filament(config, {0.6, ""}) == 1u);
    CHECK(support_filament_passes(config, 0, {0.2, "PETG"}));
    CHECK_FALSE(support_filament_passes(config, 2, {0.2, "PETG"}));
}

TEST_CASE("Role filaments of parts and height ranges count as used", "[UsedFilaments]")
{
    DynamicPrintConfig config = four_filament_config();
    config.set_key_value("enable_support",            new ConfigOptionBool(false));
    config.set_key_value("sparse_infill_filament_id", new ConfigOptionInt(0));
    Model        model;
    ModelObject *object = add_cube(model);
    REQUIRE(used_filaments(*object, config, config) == std::vector<int>{1});

    SECTION("a part's own role filament") {
        object->volumes.front()->config.set_key_value("top_surface_filament_id", new ConfigOptionInt(3));
        CHECK(used_filaments(*object, config, config) == std::vector<int>{1, 3});
    }

    SECTION("a height range's role filament") {
        object->layer_config_ranges[{5., 10.}].set_key_value("outer_wall_filament_id", new ConfigOptionInt(4));
        CHECK(used_filaments(*object, config, config) == std::vector<int>{1, 4});
    }

    SECTION("a role that prints nothing is not counted") {
        config.set_key_value("sparse_infill_filament_id", new ConfigOptionInt(2));
        config.set_key_value("sparse_infill_density",     new ConfigOptionPercent(0.));
        CHECK(used_filaments(*object, config, config) == std::vector<int>{1});
    }

    SECTION("roles left on default follow the part's filament") {
        object->volumes.front()->config.set_key_value("extruder", new ConfigOptionInt(2));
        CHECK(used_filaments(*object, config, config) == std::vector<int>{2});
    }
}
