#include <catch2/catch_test_macros.hpp>

#include "libslic3r/GCode.hpp"
#include "libslic3r/Print.hpp"

using namespace Slic3r;

TEST_CASE("Tower skips must not suppress required object tool changes", "[WipeTower][LocalZ][GCode]")
{
    auto dynamic_config = DynamicPrintConfig::full_print_config();
    dynamic_config.set_num_extruders(3);
    dynamic_config.set_num_filaments(3);
    dynamic_config.set("enable_prime_tower", true);
    dynamic_config.set("wipe_tower_no_sparse_layers", true);
    dynamic_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.assign(9, 100.);
    PrintConfig config;
    config.apply(dynamic_config, true);

    const std::vector<WipeTower::ToolChangeResult>              priming;
    std::vector<std::vector<WipeTower::ToolChangeResult>>       nominal;
    const std::vector<std::vector<WipeTower::ToolChangeResult>> local_z;
    const std::vector<std::vector<WipeTower::box_coordinates>>  reserves;
    const WipeTower::ToolChangeResult                           final_purge{};
    SECTION("Above the last planned tower layer") {}
    SECTION("Sparse tower layers without a planned tool change")
    {
        WipeTower::ToolChangeResult sparse{};
        sparse.initial_tool = sparse.new_tool = 0;
        sparse.layer_height                   = .2f;
        nominal                               = {{sparse}, {sparse}};
    }
    WipeTowerIntegration tower(config, 0, Vec3d::Zero(), priming, nominal, local_z, reserves, final_purge, 0.f, BoundingBoxf{});
    tower.next_layer();
    tower.next_layer();
    tower.set_is_first_print(false);
    GCode generator;
    generator.apply_print_config(config);
    generator.writer().set_extruders({0, 1, 2});
    generator.writer().set_extruder(2); // The previous pass left a different filament active.
    generator.writer().set_position(Vec3d(100., 100., 23.05));

    CHECK(tower.is_empty_wipe_tower_gcode(generator, 2, false));
    REQUIRE_FALSE(tower.is_empty_wipe_tower_gcode(generator, 0, false));
    // Once the requested filament is active, the same omitted tower layer
    // must not request another switch or emit extra G-code.
    generator.writer().set_extruder(0);
    CHECK(tower.is_empty_wipe_tower_gcode(generator, 0, false));
    CHECK(tower.tool_change(generator, 0, false).empty());
}
