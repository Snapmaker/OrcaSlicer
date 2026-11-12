#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/GCode/WipeTower2.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Polyline.hpp"

#include <vector>

using namespace Slic3r;

namespace {

// Config for the pure extract_wipe_volumes parsing.
FullPrintConfig make_config(bool semm = true,
                            bool purge_in_prime_tower = true,
                            std::vector<double> flush_matrix = { 0, 50, 50, 0 },
                            double flush_multiplier = 1.0,
                            std::vector<double> min_purge = { 0, 0 })
{
    FullPrintConfig c = FullPrintConfig::defaults();
    c.single_extruder_multi_material.value = semm;
    c.purge_in_prime_tower.value           = purge_in_prime_tower;
    c.flush_volumes_matrix.values          = std::move(flush_matrix);
    c.flush_multiplier.value               = flush_multiplier;
    c.filament_minimal_purge_on_wipe_tower.values = std::move(min_purge);
    return c;
}

// Full config for constructing a 2-extruder WipeTower2.
FullPrintConfig make_full_config(const std::string &printer_model = "Generic")
{
    FullPrintConfig c = FullPrintConfig::defaults();
    c.single_extruder_multi_material.value = true;
    c.purge_in_prime_tower.value           = true;
    c.flush_volumes_matrix.values          = { 0, 50, 50, 0 };
    c.flush_multiplier.value               = 1.0;
    c.filament_minimal_purge_on_wipe_tower.values = { 0, 0 };
    c.filament_adhesiveness_category.values = { 0, 0 };
    c.printable_area.values = { Vec2d(0, 0), Vec2d(250, 0), Vec2d(250, 250), Vec2d(0, 250) };
    c.gcode_flavor.value    = gcfKlipper; // isolate the U1 branch from the Marlin flavor branch
    c.printer_model.value   = printer_model;
    return c;
}

// Construct a 2-extruder WipeTower2 with the production-required setup.
WipeTower2 make_tower(const FullPrintConfig &config)
{
    WipeTower2 wt(config, config, 0, Vec3d::Zero(), { { 0.f, 50.f }, { 50.f, 0.f } }, 0);
    wt.set_extruder(0, config);
    wt.set_extruder(1, config);
    wt.set_filament_categories({ 0, 0 }); // production calls this before generate_new (Print.cpp:3461)
    return wt;
}

// Does any generated tool-change gcode contain the given substring?
bool any_gcode_contains(const std::vector<std::vector<WipeTower::ToolChangeResult>> &result, const char *needle)
{
    for (const auto &layer : result)
        for (const auto &tcr : layer)
            if (tcr.gcode.find(needle) != std::string::npos)
                return true;
    return false;
}

// Does a single layer's generated gcode contain the given substring?
bool layer_gcode_contains(const std::vector<WipeTower::ToolChangeResult> &layer, const char *needle)
{
    for (const auto &tcr : layer)
        if (tcr.gcode.find(needle) != std::string::npos)
            return true;
    return false;
}

// Depth (y-extent in mm) of a tower cross-section, from its wall+brim polylines.
// The wipe tower is a stack of blocks along y, so the depth at a layer is the
// y-extent of the generated outer wall at that z.
double tower_depth_mm(const Polylines &polylines)
{
    BoundingBox box = get_extents(polylines);
    return unscale<double>(box.max.y() - box.min.y());
}

} // namespace

// ---------------------------------------------------------------------------
// extract_wipe_volumes (pure parsing)
// ---------------------------------------------------------------------------

SCENARIO("WipeTower2 extract_wipe_volumes parses the flush matrix", "[WipeTower2]") {
    GIVEN("a 2x2 flush matrix with SEMM and purge enabled") {
        FullPrintConfig config = make_config(true, true, { 10, 20, 30, 40 }, 1.0, { 0, 0 });
        WHEN("extracted") {
            auto vols = WipeTower2::extract_wipe_volumes(config);
            THEN("the matrix is returned unchanged") {
                REQUIRE(vols.size() == 2);
                REQUIRE(vols[0] == std::vector<float>{ 10.f, 20.f });
                REQUIRE(vols[1] == std::vector<float>{ 30.f, 40.f });
            }
        }
    }
}

SCENARIO("WipeTower2 extract_wipe_volumes is zeroed when purge or SEMM is disabled", "[WipeTower2]") {
    GIVEN("SEMM disabled") {
        FullPrintConfig config = make_config(false, true, { 10, 20, 30, 40 }, 1.0, { 0, 0 });
        THEN("the matrix is all zeros") {
            auto vols = WipeTower2::extract_wipe_volumes(config);
            REQUIRE(vols[0] == std::vector<float>{ 0.f, 0.f });
            REQUIRE(vols[1] == std::vector<float>{ 0.f, 0.f });
        }
    }
    GIVEN("purge_in_prime_tower disabled") {
        FullPrintConfig config = make_config(true, false, { 10, 20, 30, 40 }, 1.0, { 0, 0 });
        THEN("the matrix is all zeros") {
            auto vols = WipeTower2::extract_wipe_volumes(config);
            REQUIRE(vols[0] == std::vector<float>{ 0.f, 0.f });
            REQUIRE(vols[1] == std::vector<float>{ 0.f, 0.f });
        }
    }
}

SCENARIO("WipeTower2 extract_wipe_volumes applies the multiplier and the minimal-purge floor", "[WipeTower2]") {
    GIVEN("a multiplier of 2") {
        FullPrintConfig config = make_config(true, true, { 10, 20, 30, 40 }, 2.0, { 0, 0 });
        THEN("the volumes are scaled") {
            auto vols = WipeTower2::extract_wipe_volumes(config);
            REQUIRE(vols[0] == std::vector<float>{ 20.f, 40.f });
            REQUIRE(vols[1] == std::vector<float>{ 60.f, 80.f });
        }
    }
    GIVEN("a minimal-purge floor above some entries") {
        FullPrintConfig config = make_config(true, true, { 10, 20, 30, 40 }, 1.0, { 15, 35 });
        THEN("entries are raised to the floor") {
            auto vols = WipeTower2::extract_wipe_volumes(config);
            REQUIRE(vols[0] == std::vector<float>{ 15.f, 35.f }); // max(10,15), max(20,35)
            REQUIRE(vols[1] == std::vector<float>{ 30.f, 40.f }); // max(30,15), max(40,35)
        }
    }
}

// ---------------------------------------------------------------------------
// get_wipe_tower_cone_base (pure math)
// ---------------------------------------------------------------------------

SCENARIO("WipeTower2 get_wipe_tower_cone_base", "[WipeTower2]") {
    GIVEN("a zero cone angle") {
        THEN("the cone base radius is zero and support scale is one") {
            auto [R, support_scale] = WipeTower2::get_wipe_tower_cone_base(60.0, 10.0, 10.0, 0.0);
            REQUIRE_THAT(R, WithinRel(0.0, 1e-9));
            REQUIRE(support_scale == 1.0);
        }
    }
    GIVEN("a 90° cone angle with a small height") {
        THEN("the cone base radius is tan(45°)*height") {
            auto [R, support_scale] = WipeTower2::get_wipe_tower_cone_base(60.0, 10.0, 10.0, 90.0);
            REQUIRE_THAT(R, WithinRel(10.0, 1e-6)); // tan(45°)*10 = 10
            REQUIRE(support_scale == 1.0);          // R < diag, no scaling
        }
    }
}

// ---------------------------------------------------------------------------
// plan -> generate flow
// ---------------------------------------------------------------------------

SCENARIO("WipeTower2 plan→generate accounts depth monotonically", "[WipeTower2]") {
    GIVEN("toolchanges on several layers") {
        FullPrintConfig config = make_full_config();
        WipeTower2 wt = make_tower(config);

        wt.plan_toolchange(0.2f, 0.2f, 0, 1, 10.f);
        wt.plan_toolchange(0.4f, 0.2f, 1, 0, 10.f);
        wt.plan_toolchange(0.6f, 0.2f, 0, 1, 10.f);

        WHEN("generated") {
            std::vector<std::vector<WipeTower::ToolChangeResult>> result, local_z_result;
            wt.generate_new(result, local_z_result);

            THEN("the tower has positive depth") {
                REQUIRE(wt.get_depth() > 0.f);
            }
            THEN("the generated wall depth (y-extent) is monotonically non-increasing") {
                // get_z_and_depth_pairs() filters m_plan to strictly-decreasing depths,
                // so asserting over it is tautological. Instead assert the actual
                // generated per-layer geometry from get_outer_wall(): the tower's
                // depth at each layer is the y-extent of its wall polylines.
                const auto &wall = wt.get_outer_wall();
                REQUIRE(wall.size() >= 2);

                std::vector<double> depths;
                depths.reserve(wall.size());
                for (const auto &entry : wall)
                    depths.push_back(tower_depth_mm(entry.second));

                for (size_t i = 1; i < depths.size(); ++i)
                    REQUIRE(depths[i] <= depths[i - 1] + 1e-6);
            }
        }
    }
}

SCENARIO("WipeTower2 tool_change gcode contains a T command within the tower bbox", "[WipeTower2]") {
    GIVEN("a toolchange on a single layer") {
        FullPrintConfig config = make_full_config();
        WipeTower2 wt = make_tower(config);
        wt.plan_toolchange(0.2f, 0.2f, 0, 1, 10.f);

        WHEN("generated") {
            std::vector<std::vector<WipeTower::ToolChangeResult>> result, local_z_result;
            wt.generate_new(result, local_z_result);

            THEN("a tool change from tool 0 to tool 1 is emitted") {
                // The raw wipe-tower gcode carries the tool change as the
                // [change_filament_gcode] placeholder (substituted with "T<n>"
                // later), so verify the structured fields instead of grepping "T".
                bool found = false;
                for (const auto &layer : result)
                    for (const auto &tcr : layer)
                        if (tcr.is_tool_change) {
                            found = true;
                            REQUIRE(tcr.initial_tool == 0);
                            REQUIRE(tcr.new_tool == 1);
                        }
                REQUIRE(found);
            }
            THEN("the tool-change positions stay within the tower bbox (local coords)") {
                // start_pos/end_pos are in the tower's local coordinates, before the
                // global wipe-tower position is applied later.
                const float w = wt.width();
                const float d = wt.get_depth();
                for (const auto &layer : result)
                    for (const auto &tcr : layer) {
                        // start_pos is the local cleaning-box corner (inset by the
                        // perimeter width), so it is always inside the bbox. end_pos
                        // may be a retract/travel position outside the tower, so it
                        // is intentionally not bounded here.
                        REQUIRE(tcr.start_pos.x() >= -1e-3f);
                        REQUIRE(tcr.start_pos.y() >= -1e-3f);
                        REQUIRE(tcr.start_pos.x() <= w + 1e-3f);
                        REQUIRE(tcr.start_pos.y() <= d + 1e-3f);
                    }
            }
        }
    }
}

SCENARIO("WipeTower2 emits the U1 speed-override backup only for Snapmaker U1", "[WipeTower2]") {
    GIVEN("a Snapmaker U1 printer on a Klipper flavor") {
        FullPrintConfig config = make_full_config("Snapmaker U1");
        WipeTower2 wt = make_tower(config);
        wt.plan_toolchange(0.2f, 0.2f, 0, 1, 10.f);

        WHEN("generated") {
            std::vector<std::vector<WipeTower::ToolChangeResult>> result, local_z_result;
            wt.generate_new(result, local_z_result);
            THEN("the gcode contains the U1-only M220 B backup") {
                REQUIRE(any_gcode_contains(result, "M220 B"));
            }
        }
    }
    GIVEN("a non-U1 printer on the same flavor") {
        FullPrintConfig config = make_full_config("Generic");
        WipeTower2 wt = make_tower(config);
        wt.plan_toolchange(0.2f, 0.2f, 0, 1, 10.f);

        WHEN("generated") {
            std::vector<std::vector<WipeTower::ToolChangeResult>> result, local_z_result;
            wt.generate_new(result, local_z_result);
            THEN("the gcode does not contain M220 B") {
                REQUIRE_FALSE(any_gcode_contains(result, "M220 B"));
            }
        }
    }
}

SCENARIO("WipeTower2 chamfer controls the non-first-layer brim", "[WipeTower2]") {
    // The chamfer only changes the brim on non-first layers, which is observable
    // via get_outer_wall() (the per-layer wall+brim polylines), not get_brim_width().
    auto generate_wall = [](bool chamfer, float wipe0, float wipe1) {
        FullPrintConfig config = make_full_config();
        config.prime_tower_brim_width.value             = 5.0;
        config.prime_tower_brim_chamfer.value           = chamfer;
        config.prime_tower_brim_chamfer_max_width.value = 4.0;
        WipeTower2 wt = make_tower(config);
        wt.plan_toolchange(0.2f, 0.2f, 0, 1, wipe0);
        wt.plan_toolchange(0.4f, 0.2f, 1, 0, wipe1);
        std::vector<std::vector<WipeTower::ToolChangeResult>> result, local_z_result;
        wt.generate_new(result, local_z_result);
        return wt.get_outer_wall();
    };

    GIVEN("chamfer disabled") {
        std::map<float, Polylines> wall = generate_wall(false, 10.f, 10.f);
        THEN("the second layer has fewer polylines than the first (no brim)") {
            REQUIRE(wall.count(0.2f) == 1);
            REQUIRE(wall.count(0.4f) == 1);
            REQUIRE(wall.at(0.4f).size() < wall.at(0.2f).size());
        }
    }
    GIVEN("chamfer enabled with an equal wipe volume") {
        // Equal wipe volume (10 == 10) keeps required_depth equal across layers,
        // so depth_changed is false and the chamfer reduction actually prints.
        std::map<float, Polylines> wall = generate_wall(true, 10.f, 10.f);
        THEN("the second layer also carries a brim") {
            REQUIRE(wall.count(0.4f) == 1);
            REQUIRE(wall.at(0.4f).size() > 1);
        }
    }
    GIVEN("chamfer enabled but the tower depth changes") {
        // A larger first-layer wipe volume makes the tower taper: depth_changed is
        // true, so the non-first-layer brim is suppressed ("Stop print chamfer if
        // depth changes", WipeTower2.cpp).
        std::map<float, Polylines> wall = generate_wall(true, 40.f, 10.f);
        THEN("the second layer has no brim (wall only)") {
            REQUIRE(wall.count(0.4f) == 1);
            REQUIRE(wall.at(0.4f).size() == 1);
        }
    }
}
