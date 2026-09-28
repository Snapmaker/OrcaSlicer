// Ultra: carrying process settings across a printer switch (PresetBundle.hpp, PrintSettingsCarry).
#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>

using namespace Slic3r;

namespace {

bool contains(const std::vector<std::string> &keys, const std::string &key)
{
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

// A process config with the given values on top of an otherwise identical base.
DynamicPrintConfig process_config(int wall_loops, double infill_percent, double line_width, double layer_height, int standby_delta,
                                  bool prime_tower, const std::string &filename_format)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("wall_loops", new ConfigOptionInt(wall_loops));
    cfg.set_key_value("sparse_infill_density", new ConfigOptionPercent(infill_percent));
    cfg.set_key_value("line_width", new ConfigOptionFloatOrPercent(line_width, false));
    cfg.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(line_width, false));
    cfg.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
    cfg.set_key_value("initial_layer_print_height", new ConfigOptionFloat(layer_height));
    cfg.set_key_value("support_top_z_distance", new ConfigOptionFloat(layer_height));
    cfg.set_key_value("standby_temperature_delta", new ConfigOptionInt(standby_delta));
    cfg.set_key_value("ooze_prevention", new ConfigOptionBool(standby_delta < -100));
    cfg.set_key_value("enable_prime_tower", new ConfigOptionBool(prime_tower));
    cfg.set_key_value("prime_tower_width", new ConfigOptionFloat(prime_tower ? 35. : 60.));
    cfg.set_key_value("wipe_tower_rotation_angle", new ConfigOptionFloat(prime_tower ? 0. : 90.));
    cfg.set_key_value("filename_format", new ConfigOptionString(filename_format));
    cfg.set_key_value("enable_arc_fitting", new ConfigOptionBool(prime_tower));
    cfg.set_key_value("compatible_printers_condition", new ConfigOptionString(filename_format));
    cfg.set_key_value("print_extruder_variant", new ConfigOptionStrings({ filename_format }));
    cfg.set_key_value("outer_wall_speed", new ConfigOptionFloats({ 50. * wall_loops }));
    return cfg;
}

void set_nozzles(PresetBundle &bundle, std::vector<double> nozzles)
{
    bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = std::move(nozzles);
}

} // namespace

TEST_CASE("Print settings carry: key classification", "[Preset][PrintSettingsCarry]")
{
    for (const char *key : { "inherits", "compatible_printers", "compatible_printers_condition", "print_extruder_id", "print_extruder_variant",
                             "process_flow_support", "standby_temperature_delta", "ooze_prevention", "enable_prime_tower", "prime_volume",
                             "prime_tower_width", "prime_tower_brim_width", "wipe_tower_rotation_angle", "wipe_tower_x", "wipe_tower_no_sparse_layers",
                             "filename_format", "enable_arc_fitting", "timelapse_type", "exclude_object", "gcode_label_objects",
                             "flush_into_infill", "toolchange_ordering", "default_jerk", "default_junction_deviation", "post_process",
                             "xy_hole_compensation", "elefant_foot_compensation" }) {
        INFO(key);
        CHECK(print_carry_is_printer_coupled(key));
    }
    for (const char *key : { "wall_loops", "outer_wall_speed", "sparse_infill_density", "sparse_infill_pattern", "enable_support",
                             "support_type", "layer_height", "line_width", "default_acceleration", "brim_width", "seam_position" }) {
        INFO(key);
        CHECK_FALSE(print_carry_is_printer_coupled(key));
    }
    for (const char *key : { "line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width", "sparse_infill_line_width",
                             "internal_solid_infill_line_width", "top_surface_line_width", "support_line_width", "layer_height",
                             "initial_layer_print_height", "support_top_z_distance", "support_bottom_z_distance" }) {
        INFO(key);
        CHECK(print_carry_is_nozzle_geometry(key));
    }
    for (const char *key : { "wall_loops", "outer_wall_speed", "sparse_infill_density", "enable_support", "top_shell_thickness" }) {
        INFO(key);
        CHECK_FALSE(print_carry_is_nozzle_geometry(key));
    }
    // The listed keys are real process options, so the lists cannot silently rot.
    for (const char *key : { "standby_temperature_delta", "ooze_prevention", "enable_prime_tower", "filename_format", "enable_arc_fitting",
                             "print_extruder_id", "print_extruder_variant", "timelapse_type", "exclude_object", "support_top_z_distance",
                             "support_bottom_z_distance", "initial_layer_print_height" }) {
        INFO(key);
        CHECK(contains(Preset::print_options(), key));
    }
}

TEST_CASE("Print settings carry: same nozzle size", "[Preset][PrintSettingsCarry]")
{
    CHECK(print_carry_same_nozzle_size({ 0.4 }, { 0.4 }));
    CHECK(print_carry_same_nozzle_size({ 0.4 }, { 0.4, 0.4 }));             // X1C -> H2D
    CHECK(print_carry_same_nozzle_size({ 0.4 }, { 0.4, 0.4, 0.4, 0.4 }));   // X1C -> U1
    CHECK(print_carry_same_nozzle_size({ 0.4 }, { 0.4000000001 }));
    CHECK_FALSE(print_carry_same_nozzle_size({ 0.4 }, { 0.2 }));           // X1C 0.4 -> X1C 0.2
    CHECK_FALSE(print_carry_same_nozzle_size({ 0.4 }, { 0.4, 0.6 }));      // mixed nozzles
    CHECK_FALSE(print_carry_same_nozzle_size({ 0.4, 0.6 }, { 0.4 }));
    CHECK_FALSE(print_carry_same_nozzle_size({}, { 0.4 }));
    CHECK_FALSE(print_carry_same_nozzle_size({ 0.4 }, {}));
}

TEST_CASE("Print settings carry: config rules", "[Preset][PrintSettingsCarry]")
{
    const DynamicPrintConfig previous = process_config(4, 25., 0.42, 0.2, -5, true, "x1c");
    const DynamicPrintConfig matched  = process_config(2, 15., 0.22, 0.1, -150, false, "u1");

    SECTION("same nozzle size carries everything but the printer-coupled keys") {
        DynamicPrintConfig dst     = matched;
        const auto         changed = carry_print_settings(dst, previous, true);
        CHECK(dst.opt_int("wall_loops") == 4);
        CHECK(dst.option<ConfigOptionPercent>("sparse_infill_density")->value == Approx(25.));
        CHECK(dst.option<ConfigOptionFloatOrPercent>("line_width")->value == Approx(0.42));
        CHECK(dst.opt_float("layer_height") == Approx(0.2));
        CHECK(dst.opt_float("support_top_z_distance") == Approx(0.2));
        CHECK(dst.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{ 200. });
        // Printer-coupled: the matched profile's values stay.
        CHECK(dst.opt_int("standby_temperature_delta") == -150);
        CHECK(dst.opt_bool("ooze_prevention"));
        CHECK_FALSE(dst.opt_bool("enable_prime_tower"));
        CHECK(dst.opt_float("prime_tower_width") == Approx(60.));
        CHECK(dst.opt_float("wipe_tower_rotation_angle") == Approx(90.));
        CHECK(dst.opt_string("filename_format") == "u1");
        CHECK_FALSE(dst.opt_bool("enable_arc_fitting"));
        CHECK(dst.opt_string("compatible_printers_condition") == "u1");
        CHECK(dst.option<ConfigOptionStrings>("print_extruder_variant")->values == std::vector<std::string>{ "u1" });

        CHECK(contains(changed, "wall_loops"));
        CHECK(contains(changed, "line_width"));
        CHECK_FALSE(contains(changed, "standby_temperature_delta"));
        CHECK_FALSE(contains(changed, "filename_format"));
        CHECK(changed.size() == 8); // wall_loops, density, 2 line widths, 2 layer heights, z distance, outer_wall_speed
    }

    SECTION("different nozzle size also keeps the nozzle geometry of the matched profile") {
        DynamicPrintConfig dst     = matched;
        const auto         changed = carry_print_settings(dst, previous, false);
        CHECK(dst.opt_int("wall_loops") == 4);
        CHECK(dst.option<ConfigOptionPercent>("sparse_infill_density")->value == Approx(25.));
        CHECK(dst.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{ 200. });
        CHECK(dst.option<ConfigOptionFloatOrPercent>("line_width")->value == Approx(0.22));
        CHECK(dst.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value == Approx(0.22));
        CHECK(dst.opt_float("layer_height") == Approx(0.1));
        CHECK(dst.opt_float("initial_layer_print_height") == Approx(0.1));
        CHECK(dst.opt_float("support_top_z_distance") == Approx(0.1));
        CHECK(dst.opt_int("standby_temperature_delta") == -150);
        CHECK(changed.size() == 3); // wall_loops, density, outer_wall_speed
    }

    SECTION("vectors of a different length keep the matched profile's value") {
        DynamicPrintConfig dst = matched;
        dst.set_key_value("outer_wall_speed", new ConfigOptionFloats({ 10., 20. }));
        carry_print_settings(dst, previous, true);
        CHECK(dst.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{ 10., 20. });
    }

    SECTION("per-flow-mode values are matched up by flow mode") {
        DynamicPrintConfig src = previous;
        src.set_key_value("process_flow_support", new ConfigOptionStrings({ "standard" }));
        src.set_key_value("outer_wall_speed", new ConfigOptionFloats({ 200. }));
        DynamicPrintConfig dst = matched;
        dst.set_key_value("process_flow_support", new ConfigOptionStrings({ "standard", "high_flow" }));
        dst.set_key_value("outer_wall_speed", new ConfigOptionFloats({ 150., 250. }));
        const auto changed = carry_print_settings(dst, src, true);
        CHECK(dst.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{ 200., 250. });
        CHECK(dst.option<ConfigOptionStrings>("process_flow_support")->values == std::vector<std::string>{ "standard", "high_flow" });
        CHECK(contains(changed, "outer_wall_speed"));

        // And back, from a two-mode profile to a reordered one.
        DynamicPrintConfig back = matched;
        back.set_key_value("process_flow_support", new ConfigOptionStrings({ "high_flow", "standard" }));
        back.set_key_value("outer_wall_speed", new ConfigOptionFloats({ 1., 2. }));
        carry_print_settings(back, dst, true);
        CHECK(back.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{ 250., 200. });
    }

    SECTION("nothing to do when the profiles already agree") {
        DynamicPrintConfig dst = previous;
        CHECK(carry_print_settings(dst, previous, true).empty());
    }
}

TEST_CASE("Print settings carry: printer switch in a preset bundle", "[Preset][PrintSettingsCarry][Bundle]")
{
    PresetBundle bundle;
    // Loaded through the const& overload, which fills in every other process option from the defaults.
    const DynamicPrintConfig x1c = process_config(4, 25., 0.42, 0.2, -5, true, "x1c");
    const DynamicPrintConfig u1  = process_config(2, 15., 0.22, 0.1, -150, false, "u1");
    bundle.prints.load_preset(std::string(), "Process X1C", x1c, false);
    bundle.prints.load_preset(std::string(), "Process U1", u1, false);
    REQUIRE(bundle.prints.select_preset_by_name("Process X1C", true));
    // A modification on top of the previous profile is carried as well.
    bundle.prints.get_edited_preset().config.set_key_value("sparse_infill_density", new ConfigOptionPercent(40.));
    set_nozzles(bundle, { 0.4 });
    const PrintSettingsCarry carry = bundle.capture_print_settings_carry();
    REQUIRE(carry.from_preset == "Process X1C");
    REQUIRE(carry.nozzle_diameters == std::vector<double>{ 0.4 });

    // The printer switch re-matches the process preset, which drops the previous values.
    auto switch_to_u1 = [&bundle](std::vector<double> nozzles) {
        set_nozzles(bundle, std::move(nozzles));
        REQUIRE(bundle.prints.select_preset_by_name("Process U1", true));
        REQUIRE_FALSE(bundle.prints.current_is_dirty());
    };

    SECTION("same nozzle size: all process settings carried, dirty and revertable") {
        switch_to_u1({ 0.4, 0.4, 0.4, 0.4 });
        const auto changed = bundle.apply_print_settings_carry(carry);
        const DynamicPrintConfig &edited = bundle.prints.get_edited_preset().config;
        CHECK(edited.opt_int("wall_loops") == 4);
        CHECK(edited.option<ConfigOptionPercent>("sparse_infill_density")->value == Approx(40.));
        CHECK(edited.option<ConfigOptionFloatOrPercent>("line_width")->value == Approx(0.42));
        CHECK(edited.opt_int("standby_temperature_delta") == -150);
        CHECK_FALSE(edited.opt_bool("enable_prime_tower"));

        CHECK(bundle.prints.current_is_dirty());
        const auto dirty = bundle.prints.current_dirty_options();
        for (const std::string &key : changed) {
            INFO(key);
            CHECK(contains(dirty, key));
        }
        CHECK_FALSE(contains(dirty, "standby_temperature_delta"));

        // Reverting one value restores the matched profile's value for it only.
        bundle.prints.get_edited_preset().config.apply_only(bundle.prints.get_selected_preset().config, { "wall_loops" });
        CHECK(bundle.prints.get_edited_preset().config.opt_int("wall_loops") == 2);
        CHECK(bundle.prints.current_is_dirty());

        // Discarding everything restores the matched profile exactly.
        bundle.prints.discard_current_changes();
        CHECK_FALSE(bundle.prints.current_is_dirty());
        CHECK(bundle.prints.get_edited_preset().config.opt_int("wall_loops") == 2);
        CHECK(bundle.prints.get_edited_preset().config.option<ConfigOptionFloatOrPercent>("line_width")->value == Approx(0.22));
    }

    SECTION("different nozzle size: geometry follows the matched profile") {
        switch_to_u1({ 0.2 });
        bundle.apply_print_settings_carry(carry);
        const DynamicPrintConfig &edited = bundle.prints.get_edited_preset().config;
        CHECK(edited.opt_int("wall_loops") == 4);
        CHECK(edited.option<ConfigOptionPercent>("sparse_infill_density")->value == Approx(40.));
        CHECK(edited.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{ 200. });
        CHECK(edited.option<ConfigOptionFloatOrPercent>("line_width")->value == Approx(0.22));
        CHECK(edited.opt_float("layer_height") == Approx(0.1));
        CHECK_FALSE(contains(bundle.prints.current_dirty_options(), "line_width"));
        CHECK_FALSE(contains(bundle.prints.current_dirty_options(), "layer_height"));
    }

    SECTION("round trip A -> B -> A restores the previous settings") {
        switch_to_u1({ 0.4, 0.4 });
        bundle.apply_print_settings_carry(carry);
        const PrintSettingsCarry back = bundle.capture_print_settings_carry();
        set_nozzles(bundle, { 0.4 });
        REQUIRE(bundle.prints.select_preset_by_name("Process X1C", true));
        bundle.apply_print_settings_carry(back);
        const auto dirty = bundle.prints.current_dirty_options();
        // Only the modification made before the first switch is left as a change.
        CHECK(dirty == std::vector<std::string>{ "sparse_infill_density" });
    }
}
