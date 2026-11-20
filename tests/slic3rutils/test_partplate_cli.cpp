#include <catch_main.hpp>

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "slic3r/GUI/PartPlate.hpp"

#include <wx/app.h>

using namespace Slic3r;

namespace {
struct AutoGenerateGuard
{
    const bool previous = MixedFilamentManager::auto_generate_enabled();
    AutoGenerateGuard() { MixedFilamentManager::set_auto_generate_enabled(false); }
    ~AutoGenerateGuard() { MixedFilamentManager::set_auto_generate_enabled(previous); }
};
} // namespace

TEST_CASE("CLI plate filament discovery needs no GUI application", "[PartPlate][CLI]")
{
    // The CLI never constructs GUI_App. Filament expansion must use the
    // project configuration even when the GUI helper skips headless calls.
    REQUIRE(wxApp::GetInstance() == nullptr);
    AutoGenerateGuard  guard;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(4);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75, 1.75};
    const std::vector<std::string> colours                         = {"#FF0000", "#777777", "#000000", "#FFFFFF"};
    config.option<ConfigOptionStrings>("filament_colour")->values  = colours;

    Model          model;
    GUI::PartPlate plate(nullptr, Vec3d::Zero(), 270, 270, 270, nullptr, &model);
    plate.set_index(0);
    std::vector<int> expected;

    SECTION("Empty plate") { CHECK(plate.get_extruders_under_cli(false, config).empty()); }
    SECTION("Physical filaments are sorted and deduplicated")
    {
        auto* object = model.add_object();
        object->add_instance();
        for (int id : {4, 2, 4})
            object->add_volume(make_cube(1., 1., 1.))->config.set("extruder", id);
        REQUIRE(plate.add_instance(0, 0, false) == 0);
        expected = {2, 4};
    }
    SECTION("Painted virtual filament expands using the project definitions")
    {
        MixedFilamentManager mixed;
        mixed.add_custom_filament(2, 4, 50, colours);
        config.set("mixed_filament_definitions", mixed.serialize_custom_entries());
        auto* object = model.add_object();
        object->add_instance();
        auto* volume = object->add_volume(make_cube(1., 1., 1.));
        volume->config.set("extruder", 2);
        TriangleSelector selector(volume->mesh());
        selector.set_facet(0, EnforcerBlockerType(5));
        REQUIRE(volume->mmu_segmentation_facets.set(selector));
        REQUIRE(plate.add_instance(0, 0, false) == 0);
        expected = {2, 4};
    }
    SECTION("Physical filament count can differ from the printer tool count")
    {
        // A U1 project may have three loaded colors and four nozzles.
        config.set_num_extruders(4);
        config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75};
        const std::vector<std::string> project_colours                 = {"#FF0000", "#777777", "#000000"};
        config.option<ConfigOptionStrings>("filament_colour")->values  = project_colours;
        MixedFilamentManager mixed;
        mixed.add_custom_filament(1, 3, 50, project_colours);
        config.set("mixed_filament_definitions", mixed.serialize_custom_entries());
        auto* object = model.add_object();
        object->add_instance();
        object->add_volume(make_cube(1., 1., 1.))->config.set("extruder", 4);
        REQUIRE(plate.add_instance(0, 0, false) == 0);
        expected = {1, 3};
    }
    SECTION("Missing colour entries do not change physical filament numbering")
    {
        config.option<ConfigOptionStrings>("filament_colour")->values.clear();
        auto* object = model.add_object();
        object->add_instance();
        object->add_volume(make_cube(1., 1., 1.))->config.set("extruder", 4);
        REQUIRE(plate.add_instance(0, 0, false) == 0);
        expected = {4};
    }
    SECTION("Project changes do not reuse stale mixed filament mappings")
    {
        auto* object = model.add_object();
        object->add_instance();
        object->add_volume(make_cube(1., 1., 1.))->config.set("extruder", 5);
        REQUIRE(plate.add_instance(0, 0, false) == 0);
        MixedFilamentManager first;
        first.add_custom_filament(1, 3, 50, colours);
        config.set("mixed_filament_definitions", first.serialize_custom_entries());
        const std::vector<int> first_expected = {1, 3};
        CHECK(plate.get_extruders_under_cli(true, config) == first_expected);
        MixedFilamentManager second;
        second.add_custom_filament(2, 4, 50, colours);
        config.set("mixed_filament_definitions", second.serialize_custom_entries());
        expected = {2, 4};
    }
    SECTION("Gradient mixes include every physical component")
    {
        MixedFilamentManager mixed;
        mixed.add_custom_filament(1, 2, 50, colours);
        mixed.mixed_filaments().front().gradient_component_ids = "123";
        config.set("mixed_filament_definitions", mixed.serialize_custom_entries());
        auto* object = model.add_object();
        object->add_instance();
        object->add_volume(make_cube(1., 1., 1.))->config.set("extruder", 5);
        REQUIRE(plate.add_instance(0, 0, false) == 0);
        expected = {1, 2, 3};
    }
    SECTION("Disabled mixed rows do not occupy virtual filament slots")
    {
        MixedFilamentManager mixed;
        mixed.add_custom_filament(1, 2, 50, colours);
        mixed.add_custom_filament(3, 4, 50, colours);
        mixed.mixed_filaments().front().enabled = false;
        config.set("mixed_filament_definitions", mixed.serialize_custom_entries());
        auto* object = model.add_object();
        object->add_instance();
        object->add_volume(make_cube(1., 1., 1.))->config.set("extruder", 5);
        REQUIRE(plate.add_instance(0, 0, false) == 0);
        expected = {3, 4};
    }
    SECTION("Nonprintable instance is ignored")
    {
        auto* object                      = model.add_object();
        object->add_instance()->printable = false;
        object->add_volume(make_cube(1., 1., 1.))->config.set("extruder", 3);
        REQUIRE(plate.add_instance(0, 0, false) == 0);
    }
    CHECK(plate.get_extruders_under_cli(false, config) == expected);
    CHECK(plate.get_extruders_under_cli(true, config) == expected);
}

TEST_CASE("CLI plates retain names without creating GUI textures", "[PartPlate][CLI]")
{
    REQUIRE(wxApp::GetInstance() == nullptr);
    Model          model;
    GUI::PartPlate plate(nullptr, Vec3d::Zero(), 270, 270, 270, nullptr, &model);
    plate.set_index(0);
    for (const std::string& name : {"Factory plate 01", "Factory plate 01", "Furnace", ""}) {
        plate.set_plate_name(name);
        CHECK(plate.get_plate_name() == name);
    }
}
