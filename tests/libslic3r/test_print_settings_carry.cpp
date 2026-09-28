// Ultra: carrying process settings across a printer switch (PresetBundle.hpp, PrintSettingsCarry).
#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <iostream>
#include <map>
#include <memory>

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

// ---------------------------------------------------------------------------------------------
// Per-variant vectors, settings conflicts, round trips (review of the first hand test, 2026-09-28)
// ---------------------------------------------------------------------------------------------

namespace {

const std::string DDS  = "Direct Drive Standard";
const std::string DDHF = "Direct Drive High Flow";
const std::string E3D  = "Direct Drive E3D High Flow";
const std::string TPU  = "Direct Drive TPU High Flow";

// A process config with a Bambu per-extruder-variant layout and one value per variant.
DynamicPrintConfig variant_config(const std::vector<int> &ids, const std::vector<std::string> &variants, const std::vector<double> &speeds)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("print_extruder_id", new ConfigOptionInts(ids));
    cfg.set_key_value("print_extruder_variant", new ConfigOptionStrings(variants));
    cfg.set_key_value("initial_layer_speed", new ConfigOptionFloats(speeds));
    cfg.set_key_value("overhang_2_4_speed", new ConfigOptionFloats(speeds));
    return cfg;
}

} // namespace

TEST_CASE("Print settings carry: per-variant speeds", "[Preset][PrintSettingsCarry]")
{
    // X1C: one variant (the defaults), H2S: three variants of one extruder, H2D: seven over two.
    const DynamicPrintConfig x1c = variant_config({ 1 }, { DDS }, { 30. });
    const DynamicPrintConfig h2s = variant_config({ 1, 1, 1 }, { DDS, DDHF, E3D }, { 50., 51., 52. });
    const DynamicPrintConfig h2d = variant_config({ 1, 1, 1, 2, 2, 2, 2 }, { DDS, DDHF, E3D, DDS, DDHF, TPU, E3D }, { 50., 51., 52., 53., 54., 55., 56. });

    SECTION("one variant onto three: the standard entry takes it, the others keep the matched values") {
        DynamicPrintConfig                 dst = h2s;
        std::map<std::string, std::string> kept;
        const auto                         changed = carry_print_settings(dst, x1c, true, nullptr, &kept);
        CHECK(dst.option<ConfigOptionFloats>("initial_layer_speed")->values == std::vector<double>{ 30., 51., 52. });
        CHECK(dst.option<ConfigOptionFloats>("overhang_2_4_speed")->values == std::vector<double>{ 30., 51., 52. });
        CHECK(contains(changed, "initial_layer_speed"));
        REQUIRE(kept.count("initial_layer_speed") == 1);
        CHECK(kept["initial_layer_speed"].find("without a counterpart") != std::string::npos);
        // The layout itself stays the matched profile's.
        CHECK(dst.option<ConfigOptionStrings>("print_extruder_variant")->values == std::vector<std::string>{ DDS, DDHF, E3D });
    }

    SECTION("one variant onto two extruders: every standard entry takes it") {
        DynamicPrintConfig dst = h2d;
        carry_print_settings(dst, x1c, true);
        CHECK(dst.option<ConfigOptionFloats>("initial_layer_speed")->values == std::vector<double>{ 30., 51., 52., 30., 54., 55., 56. });
    }

    SECTION("same extruder and variant first, then the same variant on another extruder") {
        DynamicPrintConfig dst = h2d;
        carry_print_settings(dst, h2s, true);
        CHECK(dst.option<ConfigOptionFloats>("initial_layer_speed")->values == std::vector<double>{ 50., 51., 52., 50., 51., 55., 52. });
        DynamicPrintConfig back = h2s;
        carry_print_settings(back, h2d, true);
        CHECK(back.option<ConfigOptionFloats>("initial_layer_speed")->values == std::vector<double>{ 50., 51., 52. });
    }

    SECTION("three variants onto one: the standard entry") {
        DynamicPrintConfig src = h2s;
        src.option<ConfigOptionFloats>("initial_layer_speed")->values = { 41., 42., 43. };
        DynamicPrintConfig dst = x1c;
        carry_print_settings(dst, src, true);
        CHECK(dst.option<ConfigOptionFloats>("initial_layer_speed")->values == std::vector<double>{ 41. });
    }

    SECTION("Bambu variants onto flow modes (U1: standard + high flow)") {
        DynamicPrintConfig dst;
        dst.set_key_value("process_flow_support", new ConfigOptionStrings({ "standard", "high_flow" }));
        dst.set_key_value("initial_layer_speed", new ConfigOptionFloats({ 20., 21. }));
        carry_print_settings(dst, h2s, true);
        CHECK(dst.option<ConfigOptionFloats>("initial_layer_speed")->values == std::vector<double>{ 50., 51. });
        DynamicPrintConfig back = h2s;
        carry_print_settings(back, dst, true);
        CHECK(back.option<ConfigOptionFloats>("initial_layer_speed")->values == std::vector<double>{ 50., 51., 52. });
    }
}

TEST_CASE("Print settings carry: never leave a combination the settings page rewrites", "[Preset][PrintSettingsCarry]")
{
    auto support = [](SupportType type, SupportMaterialStyle style) {
        DynamicPrintConfig cfg;
        cfg.set_key_value("enable_support", new ConfigOptionBool(true));
        cfg.set_key_value("support_type", new ConfigOptionEnum<SupportType>(type));
        cfg.set_key_value("support_style", new ConfigOptionEnum<SupportMaterialStyle>(style));
        return cfg;
    };

    SECTION("a tree style under normal support is not carried") {
        DynamicPrintConfig                 dst = support(stNormalAuto, smsGrid);
        std::map<std::string, std::string> kept;
        const auto changed = carry_print_settings(dst, support(stNormalAuto, smsTreeHybrid), true, nullptr, &kept);
        CHECK(dst.opt_enum<SupportMaterialStyle>("support_style") == smsGrid);
        CHECK(changed.empty());
        CHECK(kept["support_style"].find("conflict") != std::string::npos);
    }

    SECTION("type and style travel together when they fit") {
        DynamicPrintConfig dst = support(stNormalAuto, smsGrid);
        carry_print_settings(dst, support(stTreeAuto, smsTreeHybrid), true);
        CHECK(dst.opt_enum<SupportType>("support_type") == stTreeAuto);
        CHECK(dst.opt_enum<SupportMaterialStyle>("support_style") == smsTreeHybrid);
    }

    SECTION("a matched profile that already breaks a rule is left alone") {
        DynamicPrintConfig dst = support(stNormalAuto, smsTreeHybrid);
        DynamicPrintConfig src = support(stNormalAuto, smsTreeSlim);
        carry_print_settings(dst, src, true);
        CHECK(dst.opt_enum<SupportMaterialStyle>("support_style") == smsTreeSlim);
    }

    SECTION("spiral vase is not carried onto a profile whose timelapse mode forbids it") {
        DynamicPrintConfig vase;
        vase.set_key_value("spiral_mode", new ConfigOptionBool(true));
        vase.set_key_value("wall_loops", new ConfigOptionInt(1));
        vase.set_key_value("top_shell_layers", new ConfigOptionInt(0));
        vase.set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
        vase.set_key_value("enable_support", new ConfigOptionBool(false));
        vase.set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
        DynamicPrintConfig dst = vase;
        dst.set_key_value("spiral_mode", new ConfigOptionBool(false));
        dst.set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlSmooth)); // printer-coupled, stays
        carry_print_settings(dst, vase, true);
        CHECK_FALSE(dst.opt_bool("spiral_mode"));
    }

    SECTION("extrusion rate smoothing is not carried onto a profile with arc fitting") {
        DynamicPrintConfig src;
        src.set_key_value("max_volumetric_extrusion_rate_slope", new ConfigOptionFloats({ 300. }));
        src.set_key_value("enable_arc_fitting", new ConfigOptionBool(false));
        DynamicPrintConfig dst;
        dst.set_key_value("max_volumetric_extrusion_rate_slope", new ConfigOptionFloats({ 0. }));
        dst.set_key_value("enable_arc_fitting", new ConfigOptionBool(true)); // printer-coupled, stays
        carry_print_settings(dst, src, true);
        CHECK(dst.option<ConfigOptionFloats>("max_volumetric_extrusion_rate_slope")->values == std::vector<double>{ 0. });
    }
}

namespace {

Preset &add_printer(PresetBundle &bundle, const std::string &name, std::vector<double> nozzles)
{
    DynamicPrintConfig cfg = bundle.printers.default_preset().config;
    cfg.option<ConfigOptionFloats>("nozzle_diameter", true)->values = std::move(nozzles);
    Preset &printer = bundle.printers.load_preset(std::string(), name, cfg, false);
    printer.is_visible = true;
    return printer;
}

void add_process(PresetBundle &bundle, const std::string &name, const std::string &printer, int wall_loops, SupportMaterialStyle style)
{
    DynamicPrintConfig cfg = bundle.prints.default_preset().config;
    cfg.set_key_value("compatible_printers", new ConfigOptionStrings({ printer }));
    cfg.set_key_value("wall_loops", new ConfigOptionInt(wall_loops));
    cfg.set_key_value("enable_support", new ConfigOptionBool(true));
    cfg.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
    cfg.set_key_value("support_style", new ConfigOptionEnum<SupportMaterialStyle>(style));
    bundle.prints.load_preset(std::string(), name, cfg, false).is_visible = true;
}

// A GUI printer switch: snapshot, select the printer, re-match (optionally overridden, like
// "Remember printer configuration" does), prefer the carry origin, apply.
void switch_printer(PresetBundle &bundle, const std::string &printer, const std::string &matcher_pick = std::string())
{
    const PrintSettingsCarry carry = bundle.capture_print_settings_carry();
    REQUIRE(bundle.printers.select_preset_by_name(printer, true));
    bundle.update_compatible(PresetSelectCompatibleType::Always);
    if (!matcher_pick.empty())
        REQUIRE(bundle.prints.select_preset_by_name(matcher_pick, true));
    bundle.select_print_carry_target(carry);
    bundle.apply_print_settings_carry(carry);
}

} // namespace

TEST_CASE("Print settings carry: round trips land on the original preset", "[Preset][PrintSettingsCarry][Bundle]")
{
    PresetBundle bundle;
    add_printer(bundle, "Printer A", { 0.4 });
    add_printer(bundle, "Printer B", { 0.4, 0.4 });
    add_printer(bundle, "Printer C", { 0.4 });
    // Proc A stores a tree style under normal support, like an old user preset can.
    add_process(bundle, "Proc A", "Printer A", 4, smsTreeHybrid);
    add_process(bundle, "Proc A nearest", "Printer A", 3, smsGrid);
    add_process(bundle, "Proc B", "Printer B", 2, smsGrid);
    add_process(bundle, "Proc C", "Printer C", 5, smsSnug);

    REQUIRE(bundle.printers.select_preset_by_name("Printer A", true));
    bundle.update_compatible(PresetSelectCompatibleType::Always);
    REQUIRE(bundle.prints.select_preset_by_name("Proc A", true));
    // The user's own edit.
    bundle.prints.get_edited_preset().config.set_key_value("sparse_infill_density", new ConfigOptionPercent(42.));
    const std::vector<std::string> own_edit { "sparse_infill_density" };

    SECTION("A -> B -> A, the matcher picking another A preset") {
        switch_printer(bundle, "Printer B");
        CHECK(bundle.prints.get_edited_preset().name == "Proc B");
        CHECK(bundle.prints.get_edited_preset().config.opt_int("wall_loops") == 4);
        // The tree style cannot live under normal support on B: B's style is kept, not rewritten later.
        CHECK(bundle.prints.get_edited_preset().config.opt_enum<SupportMaterialStyle>("support_style") == smsGrid);

        switch_printer(bundle, "Printer A", "Proc A nearest");
        CHECK(bundle.prints.get_edited_preset().name == "Proc A");
        CHECK(bundle.prints.current_dirty_options() == own_edit);
        CHECK(bundle.prints.get_edited_preset().config.opt_enum<SupportMaterialStyle>("support_style") == smsTreeHybrid);
    }

    SECTION("A -> B -> C -> A") {
        switch_printer(bundle, "Printer B");
        switch_printer(bundle, "Printer C");
        CHECK(bundle.prints.get_edited_preset().name == "Proc C");
        CHECK(bundle.prints.get_edited_preset().config.opt_int("wall_loops") == 4);
        switch_printer(bundle, "Printer A", "Proc A nearest");
        CHECK(bundle.prints.get_edited_preset().name == "Proc A");
        CHECK(bundle.prints.current_dirty_options() == own_edit);
    }

    SECTION("the process last used on a printer wins when the origin does not fit") {
        // A -> B -> C -> B: the origin (Proc A) does not fit B, so B's last used process wins over
        // whatever the matcher offers.
        add_process(bundle, "Proc B other", "Printer B", 6, smsGrid);
        switch_printer(bundle, "Printer B");
        switch_printer(bundle, "Printer C");
        switch_printer(bundle, "Printer B", "Proc B other");
        CHECK(bundle.prints.get_edited_preset().name == "Proc B");
        CHECK(bundle.prints.get_edited_preset().config.opt_int("wall_loops") == 4);
    }

    SECTION("a manual process change starts a new origin") {
        switch_printer(bundle, "Printer B");
        // The user picks another process on A's printer later: simulate by going back and choosing
        // "Proc A nearest" by hand, then round-tripping again.
        switch_printer(bundle, "Printer A");
        REQUIRE(bundle.prints.select_preset_by_name("Proc A nearest", true));
        switch_printer(bundle, "Printer B");
        switch_printer(bundle, "Printer A", "Proc A");
        CHECK(bundle.prints.get_edited_preset().name == "Proc A nearest");
        CHECK_FALSE(bundle.prints.current_is_dirty());
    }
}

// ---------------------------------------------------------------------------------------------
// Audit with the bundled profiles: X1C 0.4 -> H2S / H2D / H2C / U1 0.4. Lists every process key
// whose previous value was not (fully) carried, with the reason, and fails on any reason outside
// the deliberate ones (denylist, nozzle geometry, variant entries without a counterpart, conflicts).
// ---------------------------------------------------------------------------------------------

namespace {

std::unique_ptr<PresetBundle> vendor_bundle(const std::string &vendor)
{
    const std::string saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("carry_audit_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    static PresetBundle library;
    static bool         library_loaded = false;
    if (!library_loaded) {
        library.load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                              ForwardCompatibilitySubstitutionRule::EnableSilent);
        library_loaded = true;
    }
    auto bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(profiles, vendor, PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent, &library);
    set_data_dir(saved_data_dir);
    return bundle;
}

bool deliberate(const std::string &reason)
{
    return reason.rfind("printer-coupled", 0) == 0 || reason.rfind("nozzle geometry", 0) == 0 ||
           reason.rfind("variant entries without a counterpart", 0) == 0 || reason.rfind("conflict:", 0) == 0;
}

} // namespace

TEST_CASE("Print settings carry: audit X1C 0.4 onto the bundled H2S, H2D, H2C and U1 profiles", "[PrintSettingsCarry][Audit]")
{
    auto bbl = vendor_bundle("BBL");
    REQUIRE(bbl->printers.select_preset_by_name("Bambu Lab X1 Carbon 0.4 nozzle", true));
    bbl->update_compatible(PresetSelectCompatibleType::Always);
    REQUIRE(bbl->prints.select_preset_by_name("0.20mm Standard @BBL X1C", true));
    // Make every carried key observable: the X1C values must differ from any target's.
    DynamicPrintConfig &x1c = bbl->prints.get_edited_preset().config;
    for (const char *key : { "initial_layer_speed", "top_surface_speed", "overhang_1_4_speed", "overhang_2_4_speed", "overhang_3_4_speed",
                             "overhang_4_4_speed", "outer_wall_speed", "inner_wall_speed", "travel_speed", "default_acceleration" })
        if (auto *opt = dynamic_cast<ConfigOptionFloats *>(x1c.option(key)))
            for (double &v : opt->values)
                v = v + 7.;
        else if (auto *fp = dynamic_cast<ConfigOptionFloatsOrPercents *>(x1c.option(key)))
            for (FloatOrPercent &v : fp->values)
                v.value = v.value + 7.;
    const PrintSettingsCarry carry = bbl->capture_print_settings_carry();

    auto snapmaker = vendor_bundle("Snapmaker");
    struct Target { PresetBundle *bundle; const char *printer; const char *process; };
    for (const Target &t : { Target{ bbl.get(), "Bambu Lab H2S 0.4 nozzle", "0.20mm Standard @BBL H2S" },
                             Target{ bbl.get(), "Bambu Lab H2D 0.4 nozzle", "0.20mm Standard @BBL H2D" },
                             Target{ bbl.get(), "Bambu Lab H2C 0.4 nozzle", "0.20mm Standard @BBL H2C" },
                             Target{ snapmaker.get(), "Snapmaker U1 (0.4 nozzle)", "0.20mm Standard @Snapmaker U1 (0.4 nozzle)" } }) {
        INFO(t.printer);
        REQUIRE(t.bundle->printers.select_preset_by_name(t.printer, true));
        t.bundle->update_compatible(PresetSelectCompatibleType::Always);
        // What the matcher lands on from an X1C 0.20mm Standard (a fresh bundle starts on the default preset).
        REQUIRE(t.bundle->prints.select_preset_by_name(t.process, true));
        const std::string matched = t.bundle->prints.get_edited_preset().name;
        std::map<std::string, std::string> kept;
        const auto changed = t.bundle->apply_print_settings_carry(carry, &kept);
        std::cout << "CARRY-AUDIT " << t.printer << " (matched \"" << matched << "\"): " << changed.size() << " carried, " << kept.size()
                  << " not (fully) carried\n";
        for (const auto &[key, reason] : kept) {
            std::cout << "CARRY-AUDIT   " << key << ": " << reason << "\n";
            INFO(key << ": " << reason);
            CHECK(deliberate(reason));
        }
        // Nothing may differ silently: every key that still differs from the X1C value is explained.
        const DynamicPrintConfig &dst = t.bundle->prints.get_edited_preset().config;
        for (const std::string &key : carry.config.keys()) {
            const ConfigOption *a = carry.config.option(key);
            const ConfigOption *b = dst.option(key);
            if (b != nullptr && !(*a == *b)) {
                INFO("silently skipped: " << key);
                CHECK(kept.count(key) == 1);
            }
        }
        for (const char *key : { "initial_layer_speed", "top_surface_speed", "overhang_2_4_speed", "overhang_3_4_speed", "outer_wall_speed" }) {
            INFO(key);
            CHECK(contains(changed, key));
        }
        // Re-select the target's matched preset for the next printer (the carry dirtied it).
        t.bundle->prints.discard_current_changes();
    }
}
