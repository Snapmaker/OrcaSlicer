// Your own filament prices (PR 1b) through a real slice: the config the GUI hands to Print::apply
// goes through FilamentPrices::apply() (BackgroundSlicingProcess::apply), and the G-code, the
// processor result and the statistics then all carry your price.

#include <catch2/catch.hpp>

#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>

#include "libslic3r/CostEstimate.hpp"
#include "libslic3r/FilamentPrices.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// Two filaments named like the shipped presets, two cubes, a prime tower.
DynamicPrintConfig two_named_filaments()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionFloats>("filament_density")->values  = {1.24, 1.27};
    config.option<ConfigOptionFloats>("filament_cost")->values     = {25., 31.5};
    config.option<ConfigOptionFloat>("time_cost")->value           = 1.5;
    config.option<ConfigOptionBool>("enable_prime_tower")->value   = true;
    config.option<ConfigOptionBool>("enable_support")->value       = false;
    config.option<ConfigOptionFloats>("wipe_tower_x")->values      = {15.};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values      = {15.};
    config.option<ConfigOptionFloat>("prime_tower_width")->value   = 35.;
    config.set_deserialize_strict({{"brim_type", "no_brim"}, {"skirt_loops", "0"}, {"wipe_tower_wall_type", "rectangle"},
                                   {"sparse_infill_density", "10%"}});
    config.option<ConfigOptionStrings>("filament_settings_id", true)->values = {"Bambu PLA Basic @BBL X1C", "Polymaker PLA @BBL X1C"};
    config.option<ConfigOptionStrings>("filament_vendor", true)->values      = {"Bambu Lab", "Polymaker"};
    config.option<ConfigOptionStrings>("filament_type", true)->values        = {"PLA", "PLA"};
    return config;
}

void two_cubes(Model &model)
{
    ModelObject *first = model.add_object();
    first->name        = "cube-a.stl";
    first->add_volume(mesh(TestMesh::cube_20x20x20));
    first->add_instance()->set_offset(Vec3d(80., 40., 0.));
    first->ensure_on_bed();
    ModelObject *second = model.add_object();
    second->name        = "cube-b.stl";
    second->add_volume(mesh(TestMesh::cube_20x20x20));
    second->add_instance()->set_offset(Vec3d(120., 40., 0.));
    second->ensure_on_bed();
    second->volumes.front()->config.set("extruder", 2);
}

std::string read_file(const std::string &path)
{
    std::ifstream t(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
}

std::string export_to(Print &print, GCodeProcessorResult *result)
{
    const boost::filesystem::path path = scratch_path();
    print.set_status_silent();
    print.process();
    print.export_gcode(path.string(), result, nullptr);
    std::string gcode = read_file(path.string());
    boost::nowide::remove(path.string().c_str());
    return gcode;
}

// What the GUI funnel does before Print::apply.
DynamicPrintConfig funnel(DynamicPrintConfig config, const FilamentPrices::Store &store)
{
    FilamentPrices::apply(config, store, nullptr);
    return config;
}

std::string line_value(const std::string &gcode, const std::string &prefix)
{
    const size_t at = gcode.find("\n" + prefix);
    if (at == std::string::npos)
        return {};
    const size_t start = at + 1 + prefix.size();
    return gcode.substr(start, gcode.find('\n', start) - start);
}

std::vector<double> numbers(const std::string &csv)
{
    std::vector<double> out;
    std::stringstream   ss(csv);
    std::string         item;
    while (std::getline(ss, item, ','))
        out.push_back(std::stod(item));
    return out;
}

FilamentPrices::Store bambu_basic_at(double price)
{
    FilamentPrices::Store store;
    REQUIRE(store.set_family("Bambu Lab", "PLA", "Bambu PLA Basic", price));
    return store;
}

} // namespace

TEST_CASE("Your filament price is what the slice, its G-code and its statistics use", "[FilamentPrices][GCode]")
{
    Print                plain_print, priced_print;
    Model                plain_model, priced_model;
    GCodeProcessorResult plain_result, priced_result;
    two_cubes(plain_model);
    two_cubes(priced_model);

    plain_print.apply(plain_model, funnel(two_named_filaments(), FilamentPrices::Store()));
    plain_print.set_gcode_filament_prices(true);
    const std::string plain = export_to(plain_print, &plain_result);

    priced_print.apply(priced_model, funnel(two_named_filaments(), bambu_basic_at(40.)));
    priced_print.set_gcode_filament_prices(true);
    const std::string priced = export_to(priced_print, &priced_result);

    // CONFIG_BLOCK: your price for filament 1, the preset price for filament 2 (no price of yours).
    CHECK(line_value(plain, "; filament_cost = ") == "25,31.5");
    CHECK(line_value(priced, "; filament_cost = ") == "40,31.5");

    // "; filament cost = a, b": filament 1 scales with the price, filament 2 does not move.
    const std::vector<double> plain_costs  = numbers(line_value(plain, "; filament cost = "));
    const std::vector<double> priced_costs = numbers(line_value(priced, "; filament cost = "));
    REQUIRE(plain_costs.size() == 2);
    REQUIRE(priced_costs.size() == 2);
    // Both are printed with 2 decimals: allow the rounding of each.
    CHECK(priced_costs[0] > plain_costs[0]);
    CHECK_THAT(priced_costs[0], WithinAbs(plain_costs[0] * 40. / 25., 0.02));
    CHECK_THAT(priced_costs[1], WithinAbs(plain_costs[1], 0.011));

    // Processor result, breakdown and statistics agree on it.
    REQUIRE(priced_result.filament_costs.size() >= 2);
    CHECK_THAT(double(priced_result.filament_costs[0]), WithinRel(40., 1e-6));
    CHECK_THAT(double(priced_result.filament_costs[1]), WithinRel(31.5, 1e-6));
    const CostBreakdown plain_cost  = compute_cost(plain_result);
    const CostBreakdown priced_cost = compute_cost(priced_result);
    REQUIRE(priced_cost.lines.size() == 2);
    CHECK_THAT(priced_cost.lines[0].price_per_kg, WithinRel(40., 1e-6));
    CHECK_THAT(priced_cost.lines[0].total_cost, WithinRel(plain_cost.lines[0].total_cost * 40. / 25., 1e-6));
    CHECK_THAT(priced_cost.lines[1].total_cost, WithinRel(plain_cost.lines[1].total_cost, 1e-9));
    CHECK_THAT(priced_print.print_statistics().total_cost, WithinRel(priced_cost.total, 1e-6));
    CHECK(priced_cost.total > plain_cost.total);
}

TEST_CASE("Your filament price with prices left out of the G-code: statistics only", "[FilamentPrices][GCode]")
{
    Print                print;
    Model                model;
    GCodeProcessorResult result;
    two_cubes(model);
    print.apply(model, funnel(two_named_filaments(), bambu_basic_at(40.)));
    print.set_gcode_filament_prices(false);
    const std::string gcode = export_to(print, &result);

    // The 1a preference holds for your prices too: nothing in the file...
    CHECK(gcode.find("filament_cost") == std::string::npos);
    CHECK(gcode.find("filament cost") == std::string::npos);
    CHECK(gcode.find("40,31.5") == std::string::npos);
    // ...while the slice's own numbers use it.
    CHECK_THAT(double(result.filament_costs[0]), WithinRel(40., 1e-6));
    CHECK_THAT(print.print_statistics().total_cost, WithinRel(compute_cost(result).total, 1e-6));
}

TEST_CASE("Changing your filament price re-runs only the G-code export", "[FilamentPrices][Invalidation]")
{
    Print print;
    Model model;
    two_cubes(model);
    print.apply(model, funnel(two_named_filaments(), bambu_basic_at(40.)));
    export_to(print, nullptr);
    REQUIRE(print.is_step_done(psGCodeExport));

    // The same prices again: nothing to redo.
    CHECK(print.apply(model, funnel(two_named_filaments(), bambu_basic_at(40.))) == PrintBase::APPLY_STATUS_UNCHANGED);
    CHECK(print.is_step_done(psGCodeExport));

    // A new price: only the export.
    CHECK(print.apply(model, funnel(two_named_filaments(), bambu_basic_at(18.))) == PrintBase::APPLY_STATUS_INVALIDATED);
    CHECK_FALSE(print.is_step_done(psGCodeExport));
    CHECK(print.is_step_done(psWipeTower));
    CHECK(print.is_step_done(psSkirtBrim));
    CHECK(print.is_step_done(posSlice));
    CHECK(print.is_step_done(posPerimeters));
    CHECK(print.is_step_done(posInfill));
}
