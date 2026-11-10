#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

// Build a model with one object holding one model part, optionally setting
// "extruder" on the object / the volume and feature-filament overrides.
static ModelVolume *build_single_part_model(Model &model, int object_extruder, int volume_extruder)
{
    ModelObject *object = model.add_object();
    object->name = "TestObject";
    ModelVolume *volume = object->add_volume(make_cube(20., 20., 20.));
    volume->name = "TestPart";
    volume->set_new_unique_id();
    if (object_extruder > 0)
        object->config.set_key_value("extruder", new ConfigOptionInt(object_extruder));
    if (volume_extruder > 0)
        volume->config.set_key_value("extruder", new ConfigOptionInt(volume_extruder));
    object->ensure_on_bed();
    return volume;
}

static PrintRegionConfig region_config_with_filaments(int wall, int sparse, int solid)
{
    PrintRegionConfig config;
    config.wall_filament.value = wall;
    config.sparse_infill_filament.value = sparse;
    config.solid_infill_filament.value = solid;
    return config;
}

TEST_CASE("Feature filaments: Default follows the object/part filament", "[FilamentRoles]")
{
    const size_t num_filaments = 4;

    SECTION("Global Default (0) resolves to the object extruder")
    {
        Model model;
        ModelVolume *volume = build_single_part_model(model, 2, 0);
        PrintRegionConfig region = region_config_from_model_volume(region_config_with_filaments(0, 0, 0), nullptr, *volume, num_filaments);
        REQUIRE(region.wall_filament.value == 2);
        REQUIRE(region.sparse_infill_filament.value == 2);
        REQUIRE(region.solid_infill_filament.value == 2);
    }

    SECTION("Global Default (0) resolves to the part extruder over the object extruder")
    {
        Model model;
        ModelVolume *volume = build_single_part_model(model, 3, 2);
        PrintRegionConfig region = region_config_from_model_volume(region_config_with_filaments(0, 0, 0), nullptr, *volume, num_filaments);
        REQUIRE(region.wall_filament.value == 2);
    }

    SECTION("Global Default (0) with no extruder anywhere resolves to filament 1")
    {
        Model model;
        ModelVolume *volume = build_single_part_model(model, 0, 0);
        PrintRegionConfig region = region_config_from_model_volume(region_config_with_filaments(0, 0, 0), nullptr, *volume, num_filaments);
        REQUIRE(region.wall_filament.value == 1);
        REQUIRE(region.sparse_infill_filament.value == 1);
        REQUIRE(region.solid_infill_filament.value == 1);
    }

    SECTION("Global values flow into an object that has not overridden the keys")
    {
        Model model;
        ModelVolume *volume = build_single_part_model(model, 2, 0);
        PrintRegionConfig region = region_config_from_model_volume(region_config_with_filaments(3, 4, 3), nullptr, *volume, num_filaments);
        // The global process values win over the object's own extruder when the
        // object has not manually set the keys.
        REQUIRE(region.wall_filament.value == 3);
        REQUIRE(region.sparse_infill_filament.value == 4);
        REQUIRE(region.solid_infill_filament.value == 3);
    }

    SECTION("Object overrides win over the global values")
    {
        Model model;
        ModelVolume *volume = build_single_part_model(model, 2, 0);
        volume->get_object()->config.set_key_value("wall_filament", new ConfigOptionInt(4));
        volume->get_object()->config.set_key_value("sparse_infill_filament", new ConfigOptionInt(1));
        PrintRegionConfig region = region_config_from_model_volume(region_config_with_filaments(3, 3, 3), nullptr, *volume, num_filaments);
        REQUIRE(region.wall_filament.value == 4);
        REQUIRE(region.sparse_infill_filament.value == 1);
        // solid not overridden -> global value
        REQUIRE(region.solid_infill_filament.value == 3);
    }

    SECTION("An explicit object Default (0) does not override the global value")
    {
        Model model;
        ModelVolume *volume = build_single_part_model(model, 2, 0);
        volume->get_object()->config.set_key_value("wall_filament", new ConfigOptionInt(0));
        PrintRegionConfig region = region_config_from_model_volume(region_config_with_filaments(3, 0, 0), nullptr, *volume, num_filaments);
        // 0 means "not overridden": the global 3 flows in for the wall, while the
        // keys without a global value fall back to the object extruder.
        REQUIRE(region.wall_filament.value == 3);
        REQUIRE(region.sparse_infill_filament.value == 2);
    }

    SECTION("Part-level overrides win over the object and global values")
    {
        Model model;
        ModelVolume *volume = build_single_part_model(model, 2, 0);
        volume->config.set_key_value("sparse_infill_filament", new ConfigOptionInt(4));
        PrintRegionConfig region = region_config_from_model_volume(region_config_with_filaments(3, 3, 3), nullptr, *volume, num_filaments);
        REQUIRE(region.sparse_infill_filament.value == 4);
        REQUIRE(region.wall_filament.value == 3);
    }

    SECTION("Layer range extruder provides the fallback base for its range")
    {
        Model model;
        ModelVolume *volume = build_single_part_model(model, 2, 0);
        DynamicPrintConfig range_config;
        range_config.set_key_value("extruder", new ConfigOptionInt(4));
        PrintRegionConfig region = region_config_from_model_volume(region_config_with_filaments(0, 0, 0), &range_config, *volume, num_filaments);
        REQUIRE(region.wall_filament.value == 4);
        // An explicit key on the range still wins over the range extruder.
        range_config.set_key_value("wall_filament", new ConfigOptionInt(1));
        region = region_config_from_model_volume(region_config_with_filaments(0, 0, 0), &range_config, *volume, num_filaments);
        REQUIRE(region.wall_filament.value == 1);
        REQUIRE(region.sparse_infill_filament.value == 4);
    }

    SECTION("Out-of-range values fall back to the object/part filament")
    {
        Model model;
        ModelVolume *volume = build_single_part_model(model, 2, 0);
        PrintRegionConfig region = region_config_from_model_volume(region_config_with_filaments(9, -1, 2), nullptr, *volume, num_filaments);
        REQUIRE(region.wall_filament.value == 2);
        REQUIRE(region.sparse_infill_filament.value == 2);
        REQUIRE(region.solid_infill_filament.value == 2);
    }
}

TEST_CASE("Feature filaments: handle_legacy does not remap values", "[FilamentRoles]")
{
    // The legacy default "1" -> "0" migration is NOT done in handle_legacy(): it lives in the
    // 3MF project-open flow, gated on the 3MF generator version (see bbs_3mf.cpp). Values
    // loaded through the generic config paths keep their exact value, so an explicit "1"
    // written by the current semantics survives save/load round trips.
    DynamicPrintConfig config;
    config.set_deserialize_strict({
        {"wall_filament", "1"},
        {"sparse_infill_filament", "2"},
        {"solid_infill_filament", "1"},
    });
    CHECK(config.opt_int("wall_filament") == 1);
    CHECK(config.opt_int("sparse_infill_filament") == 2);
    CHECK(config.opt_int("solid_infill_filament") == 1);

    // Ancient key aliases are still renamed (no value change).
    DynamicPrintConfig legacy_aliases;
    legacy_aliases.set_deserialize_strict({
        {"perimeter_extruder", "1"},
        {"infill_extruder", "3"},
        {"solid_infill_extruder", "2"},
    });
    CHECK(legacy_aliases.opt_int("wall_filament") == 1);
    CHECK(legacy_aliases.opt_int("sparse_infill_filament") == 3);
    CHECK(legacy_aliases.opt_int("solid_infill_filament") == 2);
}

TEST_CASE("Feature filaments: legacy project migration maps the default 1 to Default", "[FilamentRoles]")
{
    SECTION("DynamicPrintConfig (global project settings / embedded presets)")
    {
        DynamicPrintConfig config;
        config.set_deserialize_strict({
            {"wall_filament", "1"},
            {"sparse_infill_filament", "2"},
            {"solid_infill_filament", "1"},
        });
        CHECK(migrate_legacy_feature_filament_default(config));
        CHECK(config.opt_int("wall_filament") == 0);
        CHECK(config.opt_int("sparse_infill_filament") == 2);
        CHECK(config.opt_int("solid_infill_filament") == 0);
        // Idempotent: running it again changes nothing.
        CHECK_FALSE(migrate_legacy_feature_filament_default(config));
    }

    SECTION("ModelConfig (object / part / layer range configs)")
    {
        ModelConfig config;
        config.set("wall_filament", 1);
        config.set("sparse_infill_filament", 1);
        // No solid key at all: untouched, stays absent.
        CHECK(migrate_legacy_feature_filament_default(config));
        CHECK(config.opt_int("wall_filament") == 0);
        CHECK(config.opt_int("sparse_infill_filament") == 0);
        CHECK_FALSE(config.has("solid_infill_filament"));
        CHECK_FALSE(migrate_legacy_feature_filament_default(config));
    }

    SECTION("Explicit values > 1 are preserved")
    {
        DynamicPrintConfig config;
        config.set_deserialize_strict({
            {"wall_filament", "3"},
            {"sparse_infill_filament", "2"},
            {"solid_infill_filament", "4"},
        });
        CHECK_FALSE(migrate_legacy_feature_filament_default(config));
        CHECK(config.opt_int("wall_filament") == 3);
        CHECK(config.opt_int("sparse_infill_filament") == 2);
        CHECK(config.opt_int("solid_infill_filament") == 4);
    }
}

TEST_CASE("Feature filaments: defaults are Default", "[FilamentRoles]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    CHECK(config.opt_int("wall_filament") == 0);
    CHECK(config.opt_int("sparse_infill_filament") == 0);
    CHECK(config.opt_int("solid_infill_filament") == 0);
}

TEST_CASE("New objects created through Model::add_object get filament 1", "[FilamentRoles]")
{
    SECTION("lvalue-mesh overload stamps extruder 1 when absent")
    {
        Model model;
        const TriangleMesh mesh = make_cube(20., 20., 20.);
        ModelObject *object = model.add_object("TestObject", "test.stl", mesh);
        REQUIRE(object->config.has("extruder"));
        REQUIRE(object->config.opt_int("extruder") == 1);
    }

    SECTION("rvalue-mesh overload stamps extruder 1 when absent")
    {
        Model model;
        ModelObject *object = model.add_object("TestObject", "test.stl", make_cube(20., 20., 20.));
        REQUIRE(object->config.has("extruder"));
        REQUIRE(object->config.opt_int("extruder") == 1);
    }

    SECTION("copy overload replaces a missing or default extruder with 1 and keeps an explicit one")
    {
        Model model;
        ModelObject *original = model.add_object();

        // No extruder at all -> the copy is assigned filament 1.
        ModelObject *copy = model.add_object(*original);
        REQUIRE(copy->config.opt_int("extruder") == 1);

        // Explicit "Default" (0) -> the copy is assigned filament 1.
        original->config.set_key_value("extruder", new ConfigOptionInt(0));
        ModelObject *copy2 = model.add_object(*original);
        REQUIRE(copy2->config.opt_int("extruder") == 1);

        // An explicit filament is preserved by the copy.
        original->config.set_key_value("extruder", new ConfigOptionInt(3));
        ModelObject *copy3 = model.add_object(*original);
        REQUIRE(copy3->config.opt_int("extruder") == 3);
    }
}

TEST_CASE("Changing the object extruder re-triggers slicing", "[FilamentRoles]")
{
    // With feature filaments left at "Default" (0), the resolved region config follows
    // the object extruder, so changing it must invalidate the print (re-slice).
    Model model;
    ModelVolume *volume = build_single_part_model(model, 1, 0);
    volume->get_object()->add_instance();
    volume->get_object()->ensure_on_bed();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_diameter", new ConfigOptionFloats{1.75, 1.75});

    Print print;
    print.set_status_silent();
    print.apply(model, config);

    // Change the object's filament 1 -> 2: this must invalidate steps so that
    // re-slicing is triggered.
    volume->get_object()->config.set("extruder", 2);
    const bool invalidated = print.apply(model, config);
    REQUIRE(invalidated);
}

TEST_CASE("Changing the object extruder does not re-slice when all feature filaments are explicit", "[FilamentRoles]")
{
    // With every feature filament explicitly overridden, the object extruder is the
    // fallback for none of them, so changing it changes no effective config value
    // and the print is not invalidated.
    Model model;
    ModelVolume *volume = build_single_part_model(model, 1, 0);
    volume->get_object()->add_instance();
    volume->get_object()->ensure_on_bed();
    volume->get_object()->config.set_key_value("wall_filament", new ConfigOptionInt(1));
    volume->get_object()->config.set_key_value("sparse_infill_filament", new ConfigOptionInt(2));
    volume->get_object()->config.set_key_value("solid_infill_filament", new ConfigOptionInt(1));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_diameter", new ConfigOptionFloats{1.75, 1.75});

    Print print;
    print.set_status_silent();
    print.apply(model, config);

    volume->get_object()->config.set("extruder", 2);
    const bool invalidated = print.apply(model, config);
    REQUIRE_FALSE(invalidated);
}
