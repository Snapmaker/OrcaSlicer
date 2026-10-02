#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem.hpp>

#include <memory>
#include <string>

using namespace Slic3r;

// Print by object: Bambu Studio keeps extruder_clearance_max_radius (H2D: 96 mm) between objects
// in its collision check, its collision preview and its arrange; the fork used its own
// extruder_clearance_radius (H2D profile: 49 mm, a leftover Bambu Studio ignores). On a Bambu Lab
// printer the fork now uses the max radius too (sequential_clearance_radius); every other printer
// keeps extruder_clearance_radius, so a profile that never set the max radius does not pick up its
// 68 mm default.

namespace {

PresetBundle &bbl_bundle()
{
    static std::unique_ptr<PresetBundle> bundle;
    static std::unique_ptr<PresetBundle> library;
    if (bundle)
        return *bundle;
    const std::string saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("by_object_clearance_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    library = std::make_unique<PresetBundle>();
    library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                           ForwardCompatibilitySubstitutionRule::EnableSilent);
    bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                          library.get());
    set_data_dir(saved_data_dir);
    return *bundle;
}

DynamicPrintConfig h2d_by_object_config()
{
    PresetBundle &b = bbl_bundle();
    const std::string filament = "Bambu PLA Basic @BBL H2D";
    REQUIRE(b.printers.select_preset_by_name("Bambu Lab H2D 0.4 nozzle", true));
    REQUIRE(b.prints.select_preset_by_name("0.20mm Standard @BBL H2D", true));
    REQUIRE(b.filaments.select_preset_by_name(filament, true));
    b.filament_presets = { filament };
    b.set_num_filaments(1, std::vector<std::string>{ "#E01919" });
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "print_sequence", "by object" },
        { "enable_prime_tower", "0" },
        { "skirt_loops", 0 },
        { "brim_type", "no_brim" },
    });
    return cfg;
}

// Two 20 x 20 x 40 mm blocks (taller than the nozzle, so the full clearance applies) with `gap`
// millimetres between them along X.
void add_two_blocks(Model &model, double gap)
{
    const double xs[] = { 175. - (20. + gap) / 2., 175. + (20. + gap) / 2. };
    for (int i = 0; i < 2; ++i) {
        TriangleMesh block = Test::mesh(Test::TestMesh::cube_20x20x20);
        block.scale(Vec3f(1.f, 1.f, 2.f));
        ModelObject *object = model.add_object();
        object->name        = "block" + std::to_string(i);
        object->add_volume(block);
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(xs[i], 160., 0.));
        object->ensure_on_bed();
    }
}

StringObjectException clearance(const DynamicPrintConfig &cfg, double gap, bool bbl)
{
    Print print;
    Model model;
    add_two_blocks(model, gap);
    print.is_BBL_printer() = bbl;
    print.apply(model, cfg);
    return Print::sequential_print_clearance_valid(print);
}

} // namespace

TEST_CASE("Print by object on an H2D keeps Bambu Studio's 96 mm clearance", "[ByObjectClearance]")
{
    const DynamicPrintConfig h2d = h2d_by_object_config();
    REQUIRE(h2d.opt_float("extruder_clearance_max_radius") == Approx(96.));
    REQUIRE(h2d.opt_float("extruder_clearance_radius") == Approx(49.));

    SECTION("the clearance radius and the arrange distance are the max radius") {
        CHECK(sequential_clearance_radius(h2d) == Approx(96.));
        CHECK(min_object_distance(h2d) == Approx(96.));
    }
    SECTION("70 mm apart (more than 49, less than 96): now a collision") {
        CHECK_FALSE(clearance(h2d, 70., true).string.empty());
    }
    SECTION("100 mm apart: clear") {
        CHECK(clearance(h2d, 100., true).string.empty());
    }
}

TEST_CASE("Print by object on other printers keeps extruder_clearance_radius", "[ByObjectClearance]")
{
    // The same H2D settings under a non-Bambu printer model: the 96 mm max radius is ignored.
    DynamicPrintConfig other = h2d_by_object_config();
    other.set_deserialize_strict({ { "printer_model", "Generic Klipper Printer" } });

    CHECK(sequential_clearance_radius(other) == Approx(49.));
    CHECK(min_object_distance(other) == Approx(49.));
    CHECK(clearance(other, 70., false).string.empty());
    CHECK_FALSE(clearance(other, 40., false).string.empty());

    SECTION("a profile that never set the max radius keeps its own radius, not the 68 mm default") {
        DynamicPrintConfig plain = DynamicPrintConfig::full_print_config();
        plain.set_deserialize_strict({ { "print_sequence", "by object" }, { "extruder_clearance_radius", 35. } });
        REQUIRE(plain.opt_float("extruder_clearance_max_radius") == Approx(68.));
        CHECK(sequential_clearance_radius(plain) == Approx(35.));
        CHECK(min_object_distance(plain) == Approx(35.));
    }
}
