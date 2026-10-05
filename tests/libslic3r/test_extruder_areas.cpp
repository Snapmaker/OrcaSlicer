#include <catch2/catch.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtruderAreas.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace Slic3r;

// Dual-nozzle reach (Bambu H2D / H2C / X2D): the strips only one nozzle reaches, the check that a filament
// tied to a nozzle only prints what that nozzle reaches, and the quiet paths for printers without such areas.

namespace {

// H2D: 350 x 320 bed, left nozzle X 0..325, right nozzle X 25..350 (resources/profiles/BBL H2D 0.4 nozzle.json).
const Pointfs H2D_BED   = { { 0, 0 }, { 350, 0 }, { 350, 320 }, { 0, 320 } };
const Pointfs H2D_LEFT  = { { 0, 0 }, { 325, 0 }, { 325, 320 }, { 0, 320 } };
const Pointfs H2D_RIGHT = { { 25, 0 }, { 350, 0 }, { 350, 320 }, { 25, 320 } };

ExtruderAreas h2d_areas() { return compute_extruder_areas(H2D_BED, { H2D_LEFT, H2D_RIGHT }, { 320., 325. }); }

Polygons box(double x0, double y0, double x1, double y1)
{
    return { Polygon::new_scale(Pointfs{ { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } }) };
}

double area_mm2(const Polygons &polygons)
{
    double a = 0.;
    for (const Polygon &p : polygons)
        a += std::abs(p.area());
    return unscale<double>(1.) * unscale<double>(1.) * a;
}

} // namespace

SCENARIO("extruder areas are computed from the profile values", "[ExtruderAreas]")
{
    GIVEN("the H2D's two overlapping nozzle areas")
    {
        const ExtruderAreas areas = h2d_areas();
        THEN("there are two nozzles with exclusive strips")
        {
            REQUIRE(areas.multi());
            CHECK(areas.count() == 2);
            CHECK(areas.has_exclusive_regions());
            CHECK(areas.has_height_limits());
        }
        THEN("the left nozzle owns X 0..25 and the right nozzle owns X 325..350")
        {
            CHECK(area_mm2(areas.only[0]) == Approx(25. * 320.).margin(0.01));
            CHECK(area_mm2(areas.only[1]) == Approx(25. * 320.).margin(0.01));
            CHECK(point_in_area(Point::new_scale(10., 100.), areas.only[0]));
            CHECK_FALSE(point_in_area(Point::new_scale(10., 100.), areas.only[1]));
            CHECK(point_in_area(Point::new_scale(340., 100.), areas.only[1]));
            CHECK_FALSE(point_in_area(Point::new_scale(340., 100.), areas.only[0]));
            CHECK_FALSE(point_in_area(Point::new_scale(100., 100.), areas.only[0]));
            CHECK_FALSE(point_in_area(Point::new_scale(100., 100.), areas.only[1]));
        }
        THEN("what both reach is X 25..325 and what each cannot reach is the other's strip")
        {
            CHECK(area_mm2(areas.shared) == Approx(300. * 320.).margin(0.01));
            CHECK(area_mm2(areas.unprintable[0]) == Approx(25. * 320.).margin(0.01));
            CHECK(point_in_area(Point::new_scale(340., 100.), areas.unprintable[0]));
            CHECK(point_in_area(Point::new_scale(10., 100.), areas.unprintable[1]));
        }
        THEN("the per-nozzle heights are kept")
        {
            CHECK(areas.height_limit(0) == Approx(320.));
            CHECK(areas.height_limit(1) == Approx(325.));
        }
    }

    GIVEN("the X2D, whose right nozzle reaches nothing the left one does not")
    {
        const Pointfs left  = { { 0, 0 }, { 256, 0 }, { 256, 256 }, { 0, 256 } };
        const Pointfs right = { { 20.5, 0 }, { 256, 0 }, { 256, 256 }, { 20.5, 256 } };
        const ExtruderAreas areas = compute_extruder_areas({ { 0, 0 }, { 256, 0 }, { 256, 256 }, { 0, 256 } }, { left, right }, { 261., 256. });
        THEN("only the left nozzle has a strip")
        {
            CHECK(area_mm2(areas.only[0]) == Approx(20.5 * 256.).margin(0.01));
            CHECK(areas.only[1].empty());
            CHECK(areas.has_exclusive_regions());
        }
    }

    GIVEN("nozzles that reach the same bed to the same height")
    {
        const ExtruderAreas areas = compute_extruder_areas(H2D_BED, { H2D_BED, H2D_BED }, { 300., 300. });
        THEN("nothing is exclusive and nothing is height limited")
        {
            CHECK(areas.multi());
            CHECK_FALSE(areas.has_exclusive_regions());
            CHECK_FALSE(areas.has_height_limits());
        }
    }

    GIVEN("printers that declare no per-nozzle areas")
    {
        THEN("a single-nozzle machine and the four-toolhead U1 give an empty result")
        {
            const ExtruderAreas none = compute_extruder_areas(H2D_BED, {}, {});
            CHECK_FALSE(none.multi());
            CHECK(none.count() == 0);
            CHECK_FALSE(none.has_exclusive_regions());
            const ExtruderAreas one = compute_extruder_areas(H2D_BED, { H2D_LEFT }, { 320. });
            CHECK_FALSE(one.multi());
            // And nothing is reported for an object anywhere.
            CHECK(extruders_reaching(none, box(-500, -500, 900, 900), 9999.).empty());
        }
        THEN("a missing polygon for one nozzle means that nozzle reaches the whole bed")
        {
            const ExtruderAreas areas = compute_extruder_areas(H2D_BED, { H2D_LEFT, {} }, {});
            REQUIRE(areas.multi());
            CHECK(area_mm2(areas.printable[1]) == Approx(350. * 320.).margin(0.01));
            CHECK(area_mm2(areas.only[1]) == Approx(25. * 320.).margin(0.01) );
            CHECK(areas.only[0].empty());
        }
    }

    GIVEN("a dynamic config")
    {
        DynamicPrintConfig cfg;
        cfg.set_key_value("printable_area", new ConfigOptionPoints(H2D_BED));
        cfg.set_key_value("extruder_printable_area", new ConfigOptionPointsGroups(std::vector<Vec2ds>{ Vec2ds(H2D_LEFT.begin(), H2D_LEFT.end()), Vec2ds(H2D_RIGHT.begin(), H2D_RIGHT.end()) }));
        cfg.set_key_value("extruder_printable_height", new ConfigOptionFloats({ 320., 325. }));
        const ExtruderAreas areas = extruder_areas_from_config(cfg);
        THEN("it gives the same areas")
        {
            REQUIRE(areas.multi());
            CHECK(area_mm2(areas.only[0]) == Approx(25. * 320.).margin(0.01));
            CHECK(areas.height_limit(1) == Approx(325.));
        }
        THEN("a config without extruder areas is a single-nozzle result")
        {
            cfg.erase("extruder_printable_area");
            CHECK_FALSE(extruder_areas_from_config(cfg).multi());
        }
    }

    GIVEN("a plate that is not at the origin")
    {
        const ExtruderAreas moved = translate_extruder_areas(h2d_areas(), Vec2d(1000., 500.));
        THEN("the strips move with it")
        {
            CHECK(point_in_area(Point::new_scale(1010., 600.), moved.only[0]));
            CHECK_FALSE(point_in_area(Point::new_scale(10., 100.), moved.only[0]));
        }
    }
}

SCENARIO("the merged form old builds loaded from a profile is split back", "[ExtruderAreas]")
{
    GIVEN("one group of eight points that is two rectangles")
    {
        Pointfs merged = H2D_LEFT;
        merged.insert(merged.end(), H2D_RIGHT.begin(), H2D_RIGHT.end());
        const auto split = split_merged_extruder_areas({ merged });
        THEN("it is the two nozzles' areas")
        {
            REQUIRE(split.size() == 2);
            CHECK(split[0] == H2D_LEFT);
            CHECK(split[1] == H2D_RIGHT);
        }
        THEN("a config carrying it still gives the H2D strips")
        {
            DynamicPrintConfig cfg;
            cfg.set_key_value("printable_area", new ConfigOptionPoints(H2D_BED));
            cfg.set_key_value("extruder_printable_area", new ConfigOptionPointsGroups(std::vector<Vec2ds>{ Vec2ds(merged.begin(), merged.end()) }));
            const ExtruderAreas areas = extruder_areas_from_config(cfg);
            REQUIRE(areas.multi());
            CHECK(area_mm2(areas.only[0]) == Approx(25. * 320.).margin(0.01));
        }
    }
    GIVEN("anything else")
    {
        THEN("it is left alone")
        {
            CHECK(split_merged_extruder_areas({ H2D_LEFT }).size() == 1);
            CHECK(split_merged_extruder_areas({ H2D_LEFT, H2D_RIGHT }).size() == 2);
            // Eight points that are not two rectangles (an octagon).
            const Pointfs octagon = { { 10, 0 }, { 20, 0 }, { 30, 10 }, { 30, 20 }, { 20, 30 }, { 10, 30 }, { 0, 20 }, { 0, 10 } };
            CHECK(split_merged_extruder_areas({ octagon }).size() == 1);
            // A rectangle repeated is four distinct corners twice: still two rectangles, split (it is no valid polygon either way).
            Pointfs skew = H2D_LEFT;
            skew.push_back({ 5, 5 });
            skew.push_back({ 50, 5 });
            skew.push_back({ 50, 50 });
            skew.push_back({ 5, 60 });
            CHECK(split_merged_extruder_areas({ skew }).size() == 1);
        }
    }
}

SCENARIO("the hatching drawn over a nozzle-only strip", "[ExtruderAreas]")
{
    const ExtruderAreas areas = h2d_areas();
    GIVEN("the left-only strip")
    {
        const ExPolygons stripes = hatch_region(areas.only[0], 4., 1.);
        THEN("a quarter of the strip is covered and nothing is drawn outside it")
        {
            REQUIRE_FALSE(stripes.empty());
            const Polygons stripe_polygons = to_polygons(stripes);
            CHECK(area_mm2(stripe_polygons) == Approx(0.25 * 25. * 320.).epsilon(0.05));
            CHECK(area_mm2(diff(stripe_polygons, areas.only[0])) < 0.01);
        }
        THEN("an empty region or a non-positive size gives no stripes")
        {
            CHECK(hatch_region({}, 4., 1.).empty());
            CHECK(hatch_region(areas.only[0], 0., 1.).empty());
            CHECK(hatch_region(areas.only[0], 4., 0.).empty());
        }
    }
}

SCENARIO("which nozzles reach an object", "[ExtruderAreas]")
{
    const ExtruderAreas areas = h2d_areas();

    GIVEN("objects in each part of the bed")
    {
        THEN("an object in the left-only strip is reached by the left nozzle only")
        {
            CHECK(extruders_reaching(areas, box(5, 100, 15, 110), 10.) == std::vector<bool>{ true, false });
        }
        THEN("an object in the right-only strip is reached by the right nozzle only")
        {
            CHECK(extruders_reaching(areas, box(335, 100, 345, 110), 10.) == std::vector<bool>{ false, true });
        }
        THEN("an object in the shared middle is reached by both")
        {
            CHECK(extruders_reaching(areas, box(150, 100, 200, 150), 10.) == std::vector<bool>{ true, true });
        }
        THEN("an object across the left strip and the middle is reached by the left nozzle only")
        {
            CHECK(extruders_reaching(areas, box(10, 100, 60, 150), 10.) == std::vector<bool>{ true, false });
        }
        THEN("an object across the whole bed is reached by neither nozzle")
        {
            CHECK(extruders_reaching(areas, box(10, 100, 340, 150), 10.) == std::vector<bool>{ false, false });
        }
        THEN("an object exactly at the edge of a nozzle's reach is still reached (float round-off)")
        {
            CHECK(extruders_reaching(areas, box(100, 100, 325.00001, 150), 10.) == std::vector<bool>{ true, true });
            CHECK(extruders_reaching(areas, box(24.99999, 100, 100, 150), 10.) == std::vector<bool>{ true, true });
        }
        THEN("an object a millimetre into the other strip is not")
        {
            CHECK(extruders_reaching(areas, box(100, 100, 326, 150), 10.) == std::vector<bool>{ false, true });
        }
    }

    GIVEN("nozzles with different heights")
    {
        THEN("an object taller than one nozzle's limit is reached by the other nozzle only")
        {
            CHECK(extruders_reaching(areas, box(150, 100, 200, 150), 322.) == std::vector<bool>{ false, true });
            CHECK(extruders_reaching(areas, box(150, 100, 200, 150), 320.) == std::vector<bool>{ true, true });
            CHECK(extruders_reaching(areas, box(150, 100, 200, 150), 330.) == std::vector<bool>{ false, false });
        }
    }
}

SCENARIO("which filaments cannot be printed", "[ExtruderAreas]")
{
    const ExtruderAreas areas = h2d_areas();
    auto object = [&areas](int id, std::vector<int> filaments, double x0, double x1) {
        ObjectReach o;
        o.id        = id;
        o.filaments = std::move(filaments);
        o.reach     = extruders_reaching(areas, box(x0, 100, x1, 150), 10.);
        return o;
    };
    // filament_map is 1-based: 1 = left nozzle, 2 = right nozzle.

    GIVEN("manual filament assignment")
    {
        THEN("an object in the left strip with a filament on the right nozzle is a violation")
        {
            const auto v = find_reach_violations({ object(7, { 0 }, 5, 15) }, 2, { 2 }, true);
            REQUIRE(v.size() == 1);
            CHECK(v[0].filament == 0);
            CHECK(v[0].extruder == 1);
            CHECK(v[0].object_ids == std::vector<int>{ 7 });
        }
        THEN("the same object with its filament on the left nozzle is fine")
        {
            CHECK(find_reach_violations({ object(7, { 0 }, 5, 15) }, 2, { 1 }, true).empty());
        }
        THEN("an object in the right strip needs its filament on the right nozzle")
        {
            CHECK(find_reach_violations({ object(1, { 0 }, 335, 345) }, 2, { 1 }, true).size() == 1);
            CHECK(find_reach_violations({ object(1, { 0 }, 335, 345) }, 2, { 2 }, true).empty());
        }
        THEN("an object in the shared middle is never a violation")
        {
            CHECK(find_reach_violations({ object(1, { 0, 1 }, 100, 200) }, 2, { 1, 2 }, true).empty());
            CHECK(find_reach_violations({ object(1, { 0, 1 }, 100, 200) }, 2, { 2, 1 }, true).empty());
        }
        THEN("only the filament on the wrong nozzle is reported for a multi-material object")
        {
            // Object in the left strip printed with filaments 0 (left) and 1 (right): filament 1 is the problem.
            const auto v = find_reach_violations({ object(3, { 0, 1 }, 5, 15) }, 2, { 1, 2 }, true);
            REQUIRE(v.size() == 1);
            CHECK(v[0].filament == 1);
            CHECK(v[0].extruder == 1);
        }
        THEN("several objects on one filament are collected under one violation, without duplicates")
        {
            const auto v = find_reach_violations({ object(1, { 0 }, 5, 15), object(2, { 0 }, 16, 24), object(3, { 0 }, 100, 150) }, 2, { 2 }, true);
            REQUIRE(v.size() == 1);
            CHECK(v[0].object_ids == std::vector<int>{ 1, 2 });
        }
        THEN("a filament the map does not cover is not judged")
        {
            CHECK(find_reach_violations({ object(1, { 4 }, 5, 15) }, 2, { 2, 2 }, true).empty());
        }
    }

    GIVEN("automatic filament grouping")
    {
        THEN("an object in one strip is fine: the grouping puts the filament on the nozzle that reaches it")
        {
            CHECK(find_reach_violations({ object(1, { 0 }, 5, 15) }, 2, { 2 }, false).empty());
            CHECK(find_reach_violations({ object(1, { 0 }, 335, 345) }, 2, { 1 }, false).empty());
        }
        THEN("one filament used in both strips cannot be printed by either nozzle")
        {
            const auto v = find_reach_violations({ object(1, { 0 }, 5, 15), object(2, { 0 }, 335, 345) }, 2, {}, false);
            REQUIRE(v.size() == 1);
            CHECK(v[0].filament == 0);
            CHECK(v[0].extruder == -1);
            CHECK(v[0].object_ids == std::vector<int>{ 1, 2 });
        }
        THEN("different filaments in the two strips are fine")
        {
            CHECK(find_reach_violations({ object(1, { 0 }, 5, 15), object(2, { 1 }, 335, 345) }, 2, {}, false).empty());
        }
        THEN("an object across the whole bed blocks both nozzles for its filament")
        {
            const auto v = find_reach_violations({ object(1, { 0 }, 5, 345) }, 2, {}, false);
            REQUIRE(v.size() == 1);
            CHECK(v[0].extruder == -1);
        }
    }

    GIVEN("one nozzle, or four toolheads without areas (the U1)")
    {
        THEN("nothing is ever reported")
        {
            ObjectReach o;
            o.id        = 1;
            o.filaments = { 0, 1, 2, 3 };
            CHECK(find_reach_violations({ o }, 1, { 1, 1, 1, 1 }, true).empty());
            CHECK(find_reach_violations({ o }, 0, { 1, 2, 3, 4 }, true).empty());
            // A U1 object has no reach vector at all (no areas): every toolhead reaches it.
            CHECK(find_reach_violations({ o }, 4, { 1, 2, 3, 4 }, true).empty());
            CHECK(find_reach_violations({ o }, 4, {}, false).empty());
        }
    }
}

// ---- Print::validate on the real H2D profile ----------------------------------------------------------------

namespace {

PresetBundle &bbl_bundle()
{
    static std::unique_ptr<PresetBundle> bundle;
    static std::unique_ptr<PresetBundle> library;
    if (bundle)
        return *bundle;
    const std::string saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("extruder_areas_%%%%-%%%%");
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

// tower_x < 0: no prime tower; otherwise a tower with its corner at (tower_x, 250).
DynamicPrintConfig h2d_config(const std::string &sequence, const std::string &mode, const std::string &filament_map, double tower_x = -1.)
{
    PresetBundle &b = bbl_bundle();
    const std::string filament = "Bambu PLA Basic @BBL H2D";
    REQUIRE(b.printers.select_preset_by_name("Bambu Lab H2D 0.4 nozzle", true));
    REQUIRE(b.prints.select_preset_by_name("0.20mm Standard @BBL H2D", true));
    REQUIRE(b.filaments.select_preset_by_name(filament, true));
    b.filament_presets = { filament };
    b.set_num_filaments(2, std::vector<std::string>{ "#E01919", "#1943E0" });
    b.filament_presets = std::vector<std::string>(2, filament);
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "print_sequence", sequence },
        { "enable_prime_tower", "0" },
        { "filament_map_mode", mode },
        { "filament_map", filament_map },
        { "gcode_comments", 0 },
    });
    if (tower_x >= 0.)
        cfg.set_deserialize_strict({
            { "enable_prime_tower", "1" },
            { "wipe_tower_x", tower_x },
            { "wipe_tower_y", 250. },
            { "wipe_tower_rotation_angle", 0 },
        });
    return cfg;
}

// A 10 x 10 x 3 mm cube centred at x (y = 160), printed with `filament` (1-based).
void add_cube(Model &model, double x, int filament, double height_scale = 0.15f)
{
    TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
    cube.scale(Vec3f(0.5f, 0.5f, float(height_scale)));
    ModelObject *object = model.add_object();
    object->name        = "cube_x" + std::to_string(int(x));
    object->add_volume(cube);
    object->volumes.front()->name = object->name;
    object->config.set("extruder", filament);
    object->add_instance();
    object->center_around_origin();
    object->instances.front()->set_offset(Vec3d(x, 160., 0.));
    object->ensure_on_bed();
}

std::string validate(const DynamicPrintConfig &cfg, Model &model)
{
    Print print;
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    return print.validate().string;
}

// The non-blocking warning validate() raises.
std::string validate_warning(const DynamicPrintConfig &cfg, Model &model)
{
    Print print;
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    StringObjectException warning;
    print.validate(&warning);
    return warning.string;
}

bool reach_error(const std::string &text) { return text.find("can reach") != std::string::npos || text.find("can print everything") != std::string::npos; }

} // namespace

SCENARIO("the H2D profile declares the per-nozzle areas", "[ExtruderAreas][H2D]")
{
    const DynamicPrintConfig cfg = h2d_config("by layer", "Manual", "2,1");
    const auto *groups = cfg.option<ConfigOptionPointsGroups>("extruder_printable_area");
    REQUIRE(groups != nullptr);
    CAPTURE(groups->serialize());
    CAPTURE(cfg.option("printable_area")->serialize());
    CAPTURE(cfg.option("nozzle_diameter")->serialize());
    CAPTURE(cfg.option("extruder_printable_height")->serialize());
    REQUIRE(groups->values.size() == 2);
    const ExtruderAreas areas = extruder_areas_from_config(cfg);
    REQUIRE(areas.multi());
    CHECK(areas.has_exclusive_regions());
    CHECK(area_mm2(areas.only[0]) == Approx(25. * 320.).margin(0.01));
    CHECK(area_mm2(areas.only[1]) == Approx(25. * 320.).margin(0.01));
    CHECK(areas.height_limit(0) == Approx(320.));

    Model model;
    add_cube(model, 12., 1);
    Print print;
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    const ExtruderAreas print_areas = print.get_extruder_areas();
    CAPTURE(print.config().nozzle_diameter.size(), print.config().extruder_printable_area.values.size());
    CHECK(print_areas.multi());
    CHECK(print_areas.has_exclusive_regions());
    REQUIRE(print.objects().size() == 1);
    CAPTURE(print.objects().front()->object_extruders().size(), print.objects().front()->instances().size());
    CHECK(print.objects().front()->object_extruders().size() == 1);
    CAPTURE(print.filament_map_input().size(), print.config().filament_map_mode.value);
    CHECK(print.filament_map_input() == std::vector<int>{ 2, 1 });
}

SCENARIO("H2D plates are refused when a filament cannot reach its object", "[ExtruderAreas][H2D]")
{
    GIVEN("manual filament assignment: filament 1 on the right nozzle, filament 2 on the left")
    {
        const DynamicPrintConfig cfg = h2d_config("by layer", "Manual", "2,1");
        WHEN("filament 1 prints an object in the left-only strip")
        {
            Model model;
            add_cube(model, 12., 1);
            const std::string text = validate(cfg, model);
            THEN("the plate is refused and the message names the filament, the nozzle and the object")
            {
                CAPTURE(text);
                CHECK(reach_error(text));
                CHECK(text.find("Filament 1") != std::string::npos);
                CHECK(text.find("right") != std::string::npos);
                CHECK(text.find("cube_x12") != std::string::npos);
            }
        }
        WHEN("filament 2 prints the same object (it is on the left nozzle)")
        {
            Model model;
            add_cube(model, 12., 2);
            THEN("the plate is fine") { CHECK_FALSE(reach_error(validate(cfg, model))); }
        }
        WHEN("filament 2 prints an object in the right-only strip")
        {
            Model model;
            add_cube(model, 337., 2);
            THEN("the plate is refused") { CHECK(reach_error(validate(cfg, model))); }
        }
        WHEN("filament 1 prints an object in the right-only strip")
        {
            Model model;
            add_cube(model, 337., 1);
            THEN("the plate is fine") { CHECK_FALSE(reach_error(validate(cfg, model))); }
        }
        WHEN("objects sit in the shared middle")
        {
            Model model;
            add_cube(model, 150., 1);
            add_cube(model, 200., 2);
            THEN("the plate is fine") { CHECK_FALSE(reach_error(validate(cfg, model))); }
        }
        WHEN("an object is taller than the left nozzle can print")
        {
            // 20 mm cube scaled by 16.1 -> 322 mm: above the left nozzle's 320 mm, below the right's 325 mm.
            Model model;
            add_cube(model, 150., 2, 16.1);
            THEN("filament 2 (left nozzle) is refused") { CHECK(reach_error(validate(cfg, model))); }
        }
    }

    GIVEN("automatic grouping")
    {
        const DynamicPrintConfig cfg = h2d_config("by layer", "Auto For Flush", "1,1");
        WHEN("one filament prints an object in the left strip")
        {
            Model model;
            add_cube(model, 12., 1);
            THEN("the grouping can move it to the left nozzle, so the plate is fine")
            {
                CHECK_FALSE(reach_error(validate(cfg, model)));
            }
        }
        WHEN("one filament prints objects in both strips")
        {
            Model model;
            add_cube(model, 12., 1);
            add_cube(model, 337., 1);
            const std::string text = validate(cfg, model);
            THEN("no nozzle can print them all and the plate is refused")
            {
                CAPTURE(text);
                CHECK(reach_error(text));
            }
        }
        WHEN("two filaments print the two strips")
        {
            Model model;
            add_cube(model, 12., 1);
            add_cube(model, 337., 2);
            THEN("the plate is fine") { CHECK_FALSE(reach_error(validate(cfg, model))); }
        }
    }

    GIVEN("print by object with several objects (the grouping does not run, the saved filament_map is used)")
    {
        const DynamicPrintConfig cfg = h2d_config("by object", "Auto For Flush", "2,1");
        WHEN("filament 1 (right nozzle) prints an object in the left strip")
        {
            Model model;
            add_cube(model, 12., 1);
            add_cube(model, 200., 2);
            THEN("the plate is refused") { CHECK(reach_error(validate(cfg, model))); }
        }
        WHEN("filament 2 (left nozzle) prints it")
        {
            Model model;
            add_cube(model, 12., 2);
            add_cube(model, 200., 1);
            THEN("the plate is fine") { CHECK_FALSE(reach_error(validate(cfg, model))); }
        }
    }

    GIVEN("a printer that declares no per-nozzle areas (single nozzle, the U1)")
    {
        DynamicPrintConfig cfg = h2d_config("by layer", "Manual", "2,1");
        cfg.option<ConfigOptionPointsGroups>("extruder_printable_area")->values.clear();
        WHEN("an object sits at the very edge of the bed")
        {
            Model model;
            add_cube(model, 12., 1);
            add_cube(model, 337., 2);
            THEN("nothing is checked") { CHECK_FALSE(reach_error(validate(cfg, model))); }
        }
    }
}

SCENARIO("the prime tower should sit where both nozzles reach", "[ExtruderAreas][H2D]")
{
    auto tower_warned = [](const DynamicPrintConfig &cfg) {
        Model model;
        add_cube(model, 150., 1);
        add_cube(model, 200., 2);
        return validate_warning(cfg, model).find("both nozzles") != std::string::npos;
    };
    GIVEN("both nozzles change filament (automatic grouping)")
    {
        THEN("a tower in the left-only strip is warned about")
        {
            CHECK(tower_warned(h2d_config("by layer", "Auto For Flush", "1,2", 5.)));
        }
        THEN("a tower in the middle is not")
        {
            CHECK_FALSE(tower_warned(h2d_config("by layer", "Auto For Flush", "1,2", 150.)));
        }
    }
    GIVEN("a manual map that keeps every filament on the left nozzle")
    {
        THEN("a tower in the left-only strip is fine: only the left nozzle purges into it")
        {
            CHECK_FALSE(tower_warned(h2d_config("by layer", "Manual", "1,1", 5.)));
        }
    }
    GIVEN("a manual map that uses both nozzles")
    {
        THEN("a tower in the left-only strip is warned about")
        {
            CHECK(tower_warned(h2d_config("by layer", "Manual", "1,2", 5.)));
        }
    }
}

SCENARIO("the geometric unprintables the grouping gets", "[ExtruderAreas][H2D]")
{
    GIVEN("an H2D plate with objects in each strip and the middle")
    {
        const DynamicPrintConfig cfg = h2d_config("by layer", "Auto For Flush", "1,1");
        Model model;
        add_cube(model, 12., 1);    // left strip: the right nozzle cannot print filament 1
        add_cube(model, 337., 2);   // right strip: the left nozzle cannot print filament 2
        add_cube(model, 150., 1);
        Print print;
        print.is_BBL_printer() = true;
        print.apply(model, cfg);
        print.validate();
        print.set_status_silent();
        Test::gcode(print);
        const auto &unprintables = print.get_geometric_unprintable_filaments();
        REQUIRE(unprintables.size() == 2);
        THEN("filament 1 is unprintable on the right nozzle and filament 2 on the left")
        {
            CHECK(unprintables[1].count(0) == 1);
            CHECK(unprintables[0].count(1) == 1);
            CHECK(unprintables[0].count(0) == 0);
            CHECK(unprintables[1].count(1) == 0);
        }
        THEN("the grouping honoured it: filament 1 prints on the left nozzle, filament 2 on the right")
        {
            const auto group = print.get_layered_nozzle_group_result();
            REQUIRE(group);
            CHECK(group->get_extruder_map(false) == std::vector<int>{ 1, 2 });
        }
    }
}
