#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentCliGates.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/GCode/WipeTowerEstimate.hpp"

#include "../fff_print/test_data.hpp"

#include <algorithm>
#include <vector>

using namespace Slic3r;

// Tests for the pure CLI mixed-filament gate logic extracted from Snapmaker_Orca.cpp
// (src/libslic3r/MixedFilamentCliGates.hpp/.cpp) into PR #29's port of OrcaSlicer #15636's
// wipe/flush/type-compatibility gates. Snapmaker_Orca.cpp is the application's main and is not
// itself unit-testable, so these tests exercise the extracted decision functions directly with
// hand-checkable inputs; the CLI call sites forward to these functions unchanged.

namespace {

// 4 physical filaments, colours are arbitrary/unused by the gates themselves.
DynamicPrintConfig four_physical_filament_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    return config;
}

const std::vector<std::string> four_colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};

std::vector<int> unique_positive(std::vector<int> ids)
{
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    ids.erase(std::remove(ids.begin(), ids.end(), 0), ids.end());
    return ids;
}

bool contains_id(const std::vector<int> &ids, int id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

ModelObject *add_cube_object(Model &model)
{
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(10., 10., 10.));
    object->add_instance();
    return object;
}

void paint_all_facets(ModelVolume &volume, EnforcerBlockerType ebt)
{
    TriangleSelector selector(volume.mesh());
    const auto      &its = volume.mesh().its;
    for (int f = 0; f < int(its.indices.size()); ++f)
        selector.set_facet(f, ebt);
    REQUIRE(volume.mmu_segmentation_facets.set(selector));
}

DynamicPrintConfig u1_cli_config()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_extruders(4);
    cfg.set_num_filaments(4);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4, 0.4, 0.4};
    cfg.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75, 1.75};
    cfg.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    cfg.option<ConfigOptionBool>("enable_prime_tower")->value   = true;
    cfg.option<ConfigOptionBool>("enable_support")->value       = false;
    cfg.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    cfg.option<ConfigOptionBool>("purge_in_prime_tower")->value           = false;
    if (auto *wrapping = cfg.option<ConfigOptionBool>("enable_wrapping_detection"))
        wrapping->value = false;
    cfg.option<ConfigOptionInt>("raft_layers")->value        = 0;
    cfg.option<ConfigOptionFloats>("wipe_tower_x")->values    = {15.};
    cfg.option<ConfigOptionFloats>("wipe_tower_y")->values    = {15.};
    cfg.option<ConfigOptionFloat>("prime_tower_width")->value = 35.;
    cfg.set_key_value("printer_model", new ConfigOptionString("Snapmaker U1"));
    cfg.set_deserialize_strict({{"brim_type", "no_brim"}});
    for (const char *key : { "line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width",
                             "top_surface_line_width", "sparse_infill_line_width", "internal_solid_infill_line_width" }) {
        if (auto *opt = cfg.option<ConfigOptionFloatOrPercent>(key)) {
            opt->value   = 0.42;
            opt->percent = false;
        }
    }
    return cfg;
}

} // namespace

// ============================================================================
// Gate: cli_check_mixed_filament_slots_have_filament
// ============================================================================

TEST_CASE("CLI mixed filament slot gate passes when the feature is off (no definitions, no enabled rows)", "[MixedFilamentCli]")
{
    MixedFilamentManager mgr; // never auto_generate()'d or loaded: enabled_count() == 0
    REQUIRE(mgr.enabled_count() == 0);

    const std::vector<Model> models; // no loaded objects
    DynamicPrintConfig       print_config = four_physical_filament_config();

    const CliMixedFilamentVerdict verdict =
        cli_check_mixed_filament_slots_have_filament(mgr, /*mixed_defs=*/"", /*num_physical=*/4, models, print_config,
                                                       /*filament_count=*/4);
    CHECK(verdict.ok);
    CHECK(verdict.message.empty());
}

TEST_CASE("CLI mixed filament slot gate passes for a valid combination: 4 physical + 1 enabled mixed row, all ids in range", "[MixedFilamentCli]")
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, four_colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);
    REQUIRE(mgr.enabled_count() == 1);
    // With 4 physical filaments the one enabled mixed row is virtual id 4+1 = 5.
    REQUIRE(mgr.filament_id_from_mixed_index(0, 4) == 5);

    const std::string mixed_defs = mgr.serialize_custom_entries();

    // Model uses filament id 5 (the mixed slot) on its wall_filament option: a legitimate use of
    // a mixed slot, not a "slot without filament" violation.
    Model model;
    ModelObject *object = model.add_object();
    object->config.set("wall_filament", 5);

    std::vector<Model> models;
    models.push_back(std::move(model));

    DynamicPrintConfig print_config = four_physical_filament_config();

    const CliMixedFilamentVerdict verdict =
        cli_check_mixed_filament_slots_have_filament(mgr, mixed_defs, /*num_physical=*/4, models, print_config,
                                                       /*filament_count=*/4);
    CHECK(verdict.ok);
}

TEST_CASE("CLI mixed filament slot gate refuses a serialized definition naming a slot beyond num_physical", "[MixedFilamentCli]")
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, four_colors);
    REQUIRE(mgr.enabled_count() == 1);

    // Hand-construct a serialized row that references physical filament 9, but only 4 physical
    // filaments (ids 1-4) are loaded: "9,2,1" = component_a=9, component_b=2, enabled=1.
    // mixed_definitions_have_slot_without_filament flags this because 9 > num_physical(4).
    const std::string bad_defs = "9,2,1;";

    const std::vector<Model> models;
    DynamicPrintConfig       print_config = four_physical_filament_config();

    const CliMixedFilamentVerdict verdict =
        cli_check_mixed_filament_slots_have_filament(mgr, bad_defs, /*num_physical=*/4, models, print_config,
                                                       /*filament_count=*/4);
    CHECK_FALSE(verdict.ok);
    CHECK(verdict.message.find("mixed filament slot has no filament of its own") != std::string::npos);
    // The message reports the loaded filament_count we passed in (4).
    CHECK(verdict.message.find("only 4 filaments are loaded") != std::string::npos);
}

TEST_CASE("CLI mixed filament slot gate refuses a model reference to a filament id beyond num_physical that is not a mixed slot", "[MixedFilamentCli]")
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, four_colors);
    REQUIRE(mgr.enabled_count() == 1);
    // Only virtual id 5 (4 physical + 1) is a real mixed slot.
    REQUIRE(mgr.is_mixed(5, 4));
    REQUIRE_FALSE(mgr.is_mixed(6, 4));

    const std::string mixed_defs = mgr.serialize_custom_entries();

    // Model references filament id 6: beyond the 4 physical filaments, and not a mixed slot
    // either (only 5 is). This must be refused.
    Model model;
    ModelObject *object = model.add_object();
    object->config.set("solid_infill_filament", 6);

    std::vector<Model> models;
    models.push_back(std::move(model));

    DynamicPrintConfig print_config = four_physical_filament_config();

    const CliMixedFilamentVerdict verdict =
        cli_check_mixed_filament_slots_have_filament(mgr, mixed_defs, /*num_physical=*/4, models, print_config,
                                                       /*filament_count=*/4);
    CHECK_FALSE(verdict.ok);
    CHECK(verdict.message.find("mixed filament slot 6 has no filament of its own") != std::string::npos);
    CHECK(verdict.message.find("only 4 filaments are loaded") != std::string::npos);
}

// ============================================================================
// Gate: cli_check_mixed_filament_type_compatibility
// ============================================================================

TEST_CASE("CLI mixed filament type gate passes when the components share the same filament_type", "[MixedFilamentCli]")
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, four_colors);
    REQUIRE(mgr.enabled_count() == 1);
    REQUIRE(mgr.filament_id_from_mixed_index(0, 4) == 5);

    DynamicPrintConfig cfg = four_physical_filament_config();
    // filaments 1 and 2 (0-based indices 0 and 1) are both "PLA".
    cfg.option<ConfigOptionStrings>("filament_type")->values = {"PLA", "PLA", "PETG", "PETG"};

    const std::vector<int> plate_slots = {1, 2, 5}; // plate uses physical 1, 2, and the mixed slot 5.

    const CliMixedFilamentVerdict verdict =
        cli_check_mixed_filament_type_compatibility(mgr, plate_slots, /*num_physical=*/4, cfg, /*plate_index_1based=*/1);
    CHECK(verdict.ok);
}

TEST_CASE("CLI mixed filament type gate refuses a mixed slot whose components differ in filament_type", "[MixedFilamentCli]")
{
    MixedFilamentManager mgr;
    // Mixed slot combines physical filament 1 (PLA) and physical filament 3 (PETG): a genuine
    // type mismatch.
    mgr.add_custom_filament(1, 3, 50, four_colors);
    REQUIRE(mgr.enabled_count() == 1);
    REQUIRE(mgr.filament_id_from_mixed_index(0, 4) == 5);

    DynamicPrintConfig cfg = four_physical_filament_config();
    cfg.option<ConfigOptionStrings>("filament_type")->values = {"PLA", "PLA", "PETG", "PETG"};

    const std::vector<int> plate_slots = {5}; // plate uses only the mixed slot.

    const CliMixedFilamentVerdict verdict =
        cli_check_mixed_filament_type_compatibility(mgr, plate_slots, /*num_physical=*/4, cfg, /*plate_index_1based=*/3);
    CHECK_FALSE(verdict.ok);
    CHECK(verdict.message.find("plate 3: mixed filament 5 mixes components of different filament types") != std::string::npos);
}

TEST_CASE("CLI mixed filament type gate is skipped entirely when no mixed rows are enabled", "[MixedFilamentCli]")
{
    MixedFilamentManager mgr; // enabled_count() == 0: the feature-off case.
    REQUIRE(mgr.enabled_count() == 0);

    DynamicPrintConfig cfg = four_physical_filament_config();
    cfg.option<ConfigOptionStrings>("filament_type")->values = {"PLA", "PETG", "ABS", "TPU"};

    // Even a slot id that would be an out-of-range mixed id if the feature were on must not be
    // inspected: is_mixed() returns false unconditionally when nothing is enabled, so the loop
    // in the gate never calls mixed_filament_from_id/get_filament_type at all.
    const std::vector<int> plate_slots = {1, 2, 3, 4, 5};

    const CliMixedFilamentVerdict verdict =
        cli_check_mixed_filament_type_compatibility(mgr, plate_slots, /*num_physical=*/4, cfg, /*plate_index_1based=*/1);
    CHECK(verdict.ok);
}

// ============================================================================
// Supporting pure helpers used by the gates above
// ============================================================================

TEST_CASE("zero_mixed_flush_rows_and_cols zeros the row and column of a mixed slot only", "[MixedFilamentCli]")
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, four_colors);
    REQUIRE(mgr.filament_id_from_mixed_index(0, 4) == 5);

    // 5x5 matrix (4 physical + 1 mixed), flattened row-major, all entries start at a distinct
    // nonzero value: matrix[from][to] = from*5 + to + 1 (so no accidental zero already present).
    const size_t         n = 5;
    std::vector<double>  matrix(n * n);
    for (size_t from = 0; from < n; ++from)
        for (size_t to = 0; to < n; ++to)
            matrix[from * n + to] = double(from * n + to + 1);

    zero_mixed_flush_rows_and_cols(matrix, n, mgr, /*num_physical=*/4);

    for (size_t from = 0; from < n; ++from) {
        for (size_t to = 0; to < n; ++to) {
            const double value = matrix[from * n + to];
            // 0-based index 4 is filament id 5 (the mixed slot): its row and column are zeroed,
            // as is every from==to diagonal cell.
            if (from == to || from == 4 || to == 4)
                CHECK(value == 0.0);
            else
                CHECK(value == double(from * n + to + 1));
        }
    }
}

TEST_CASE("mixed_definitions_have_slot_without_filament flags an out-of-range component and accepts an in-range one", "[MixedFilamentCli]")
{
    // "1,2,1" = component_a=1, component_b=2, enabled=1: both within num_physical=4.
    CHECK_FALSE(mixed_definitions_have_slot_without_filament("1,2,1;", 4));
    // "1,9,1": component_b=9 is beyond num_physical=4.
    CHECK(mixed_definitions_have_slot_without_filament("1,9,1;", 4));
    // A disabled row (third field 0) referencing an out-of-range id is not flagged.
    CHECK_FALSE(mixed_definitions_have_slot_without_filament("1,9,0;", 4));
    // A deleted row ("d1" token present) referencing an out-of-range id is not flagged.
    CHECK_FALSE(mixed_definitions_have_slot_without_filament("1,9,1,d1;", 4));
    // Empty serialization: nothing to flag.
    CHECK_FALSE(mixed_definitions_have_slot_without_filament("", 4));
}

TEST_CASE("mixed_physical_component_ids resolves component_a/component_b for a row with no manual pattern", "[MixedFilamentCli]")
{
    MixedFilament mf;
    mf.component_a = 2;
    mf.component_b = 3;
    mf.manual_pattern.clear();

    const std::vector<unsigned int> ids = mixed_physical_component_ids(mf, /*num_physical=*/4);
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == 2);
    CHECK(ids[1] == 3);
}

TEST_CASE("mixed_components_differ_in_filament_type reports no difference for a single-component resolution", "[MixedFilamentCli]")
{
    MixedFilament mf;
    mf.component_a = 1;
    mf.component_b = 1; // degenerate: resolves to the single id {1} after dedup.

    DynamicPrintConfig cfg = four_physical_filament_config();
    cfg.option<ConfigOptionStrings>("filament_type")->values = {"PLA", "PETG", "ABS", "TPU"};

    CHECK_FALSE(mixed_components_differ_in_filament_type(mf, cfg, 4));
}

// ============================================================================
// Per-feature filament helpers used by PartPlate's plate set and the CLI mixed gate
// ============================================================================

TEST_CASE("resolve_outer_wall_filament follows object, then global, and requires walls", "[MixedFilamentCli]")
{
    DynamicPrintConfig global = four_physical_filament_config();
    global.option<ConfigOptionInt>("outer_wall_filament")->value = 2;
    REQUIRE(global.option<ConfigOptionInt>("wall_loops")->value > 0);

    SECTION("global outer_wall_filament=2 gives 2") {
        CHECK(resolve_outer_wall_filament(nullptr, global) == 2);
    }
    SECTION("object key 0 over global 2 gives 0 (explicit follow-walls)") {
        DynamicPrintConfig object_cfg;
        object_cfg.set_key_value("outer_wall_filament", new ConfigOptionInt(0));
        CHECK(resolve_outer_wall_filament(&object_cfg, global) == 0);
    }
    SECTION("object key 3 gives 3") {
        DynamicPrintConfig object_cfg;
        object_cfg.set_key_value("outer_wall_filament", new ConfigOptionInt(3));
        CHECK(resolve_outer_wall_filament(&object_cfg, global) == 3);
    }
    SECTION("wall_loops=0 gives 0") {
        global.option<ConfigOptionInt>("wall_loops")->value = 0;
        CHECK(resolve_outer_wall_filament(nullptr, global) == 0);
        DynamicPrintConfig object_cfg;
        object_cfg.set_key_value("wall_loops", new ConfigOptionInt(0));
        global.option<ConfigOptionInt>("wall_loops")->value = 2;
        CHECK(resolve_outer_wall_filament(&object_cfg, global) == 0);
    }
    SECTION("null object config uses the global value") {
        CHECK(resolve_outer_wall_filament(nullptr, global) == 2);
    }
}

TEST_CASE("append_feature_filament_overrides collects wall and solid ids and skips zeros", "[MixedFilamentCli]")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("wall_filament", new ConfigOptionInt(2));
    cfg.set_key_value("solid_infill_filament", new ConfigOptionInt(3));
    cfg.set_key_value("outer_wall_filament", new ConfigOptionInt(0));

    std::vector<int> ids;
    append_feature_filament_overrides(cfg, ids);
    REQUIRE(ids == std::vector<int>{2, 3});
}

TEST_CASE("append_config_filament_ids includes outer_wall_filament", "[MixedFilamentCli]")
{
    DynamicPrintConfig cfg = four_physical_filament_config();
    cfg.option<ConfigOptionInt>("outer_wall_filament")->value = 2;
    // Defaults: wall/sparse/solid = 1, support = 0. Only positive ids are pushed.
    std::vector<int> ids;
    append_config_filament_ids(cfg, ids);
    REQUIRE(std::find(ids.begin(), ids.end(), 2) != ids.end());
}

TEST_CASE("CLI mixed filament slot gate refuses outer_wall_filament beyond the mixed slots", "[MixedFilamentCli]")
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, four_colors);
    REQUIRE(mgr.enabled_count() == 1);
    REQUIRE(mgr.is_mixed(5, 4));
    REQUIRE_FALSE(mgr.is_mixed(6, 4));
    const std::string mixed_defs = mgr.serialize_custom_entries();
    DynamicPrintConfig print_config = four_physical_filament_config();

    SECTION("outer_wall_filament=6 is not a mixed slot and is refused") {
        Model        model;
        ModelObject *object = model.add_object();
        object->config.set("outer_wall_filament", 6);
        std::vector<Model> models;
        models.push_back(std::move(model));

        const CliMixedFilamentVerdict verdict =
            cli_check_mixed_filament_slots_have_filament(mgr, mixed_defs, /*num_physical=*/4, models, print_config,
                                                           /*filament_count=*/4);
        CHECK_FALSE(verdict.ok);
        CHECK(verdict.message.find("mixed filament slot 6 has no filament of its own") != std::string::npos);
    }
    SECTION("outer_wall_filament=5 (the mixed slot) passes") {
        Model        model;
        ModelObject *object = model.add_object();
        object->config.set("outer_wall_filament", 5);
        std::vector<Model> models;
        models.push_back(std::move(model));

        const CliMixedFilamentVerdict verdict =
            cli_check_mixed_filament_slots_have_filament(mgr, mixed_defs, /*num_physical=*/4, models, print_config,
                                                           /*filament_count=*/4);
        CHECK(verdict.ok);
    }
}

TEST_CASE("volume_contributes_feature_filaments is MODEL_PART or PARAMETER_MODIFIER", "[MixedFilamentCli]")
{
    Model        model;
    ModelObject *object = add_cube_object(model);
    REQUIRE(volume_contributes_feature_filaments(*object->volumes.front()));

    ModelVolume *modifier = object->add_volume(make_cube(5., 5., 5.));
    modifier->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    REQUIRE(volume_contributes_feature_filaments(*modifier));

    ModelVolume *negative = object->add_volume(make_cube(2., 2., 2.));
    negative->set_type(ModelVolumeType::NEGATIVE_VOLUME);
    REQUIRE_FALSE(volume_contributes_feature_filaments(*negative));

    ModelVolume *blocker = object->add_volume(make_cube(2., 2., 2.));
    blocker->set_type(ModelVolumeType::SUPPORT_BLOCKER);
    REQUIRE_FALSE(volume_contributes_feature_filaments(*blocker));
}

TEST_CASE("append_object_plate_filament_ids matches PartPlate get_extruders paths on a real Model", "[MixedFilamentCli]")
{
    // PartPlate::get_extruders / get_extruders_under_cli both call this helper; libslic3r_tests
    // cannot construct a wx PartPlate, so the shared body is what those methods run.
    DynamicPrintConfig cfg = four_physical_filament_config();
    cfg.option<ConfigOptionInt>("outer_wall_filament")->value = 2;
    cfg.option<ConfigOptionBool>("enable_support")->value     = false;
    cfg.option<ConfigOptionInt>("raft_layers")->value         = 0;
    cfg.set_deserialize_strict({{"brim_type", "no_brim"}});

    Model        model;
    ModelObject *object = add_cube_object(model);
    object->ensure_on_bed();
    object->volumes.front()->config.set("outer_wall_filament", 3);

    ModelVolume *modifier = object->add_volume(make_cube(5., 5., 5.));
    modifier->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    modifier->config.set("wall_filament", 4);

    object->layer_config_ranges[t_layer_height_range{0.0, 5.0}].set("extruder", 2);
    object->layer_config_ranges[t_layer_height_range{0.0, 5.0}].set("sparse_infill_filament", 4);

    std::vector<int> plate_ids;
    append_object_plate_filament_ids(*object, cfg, plate_ids);
    plate_ids = unique_positive(plate_ids);
    CHECK(contains_id(plate_ids, 1));
    CHECK(contains_id(plate_ids, 2));
    CHECK(contains_id(plate_ids, 3));
    CHECK(contains_id(plate_ids, 4));

    std::vector<int> cli_ids;
    std::vector<Model> models;
    models.push_back(std::move(model));
    collect_cli_filament_ids(models, cfg, cli_ids);
    cli_ids = unique_positive(cli_ids);
    CHECK(contains_id(cli_ids, 3));
    CHECK(contains_id(cli_ids, 4));
}

TEST_CASE("disabled features are not counted as plate filaments", "[MixedFilamentCli]")
{
    DynamicPrintConfig cfg = four_physical_filament_config();
    cfg.set_deserialize_strict({{"brim_type", "no_brim"}});
    cfg.option<ConfigOptionInt>("raft_layers")->value = 0;

    SECTION("wall_loops 0 skips outer_wall_filament") {
        cfg.option<ConfigOptionInt>("wall_loops")->value           = 0;
        cfg.option<ConfigOptionInt>("outer_wall_filament")->value  = 2;
        cfg.option<ConfigOptionBool>("enable_support")->value      = false;
        Model        model;
        ModelObject *object = add_cube_object(model);
        std::vector<int> ids;
        append_object_plate_filament_ids(*object, cfg, ids);
        CHECK_FALSE(contains_id(unique_positive(ids), 2));
    }
    SECTION("sparse density 0 skips sparse_infill_filament") {
        cfg.option<ConfigOptionPercent>("sparse_infill_density")->value = 0;
        cfg.option<ConfigOptionInt>("sparse_infill_filament")->value    = 2;
        cfg.option<ConfigOptionBool>("enable_support")->value           = false;
        Model        model;
        ModelObject *object = add_cube_object(model);
        std::vector<int> ids;
        append_object_plate_filament_ids(*object, cfg, ids);
        CHECK_FALSE(contains_id(unique_positive(ids), 2));
    }
    SECTION("zero shells skip solid_infill_filament") {
        cfg.option<ConfigOptionInt>("top_shell_layers")->value    = 0;
        cfg.option<ConfigOptionInt>("bottom_shell_layers")->value = 0;
        cfg.option<ConfigOptionInt>("solid_infill_filament")->value = 2;
        cfg.option<ConfigOptionBool>("enable_support")->value       = false;
        Model        model;
        ModelObject *object = add_cube_object(model);
        std::vector<int> ids;
        append_object_plate_filament_ids(*object, cfg, ids);
        CHECK_FALSE(contains_id(unique_positive(ids), 2));
    }
    SECTION("support off skips support_filament") {
        cfg.option<ConfigOptionBool>("enable_support")->value  = false;
        cfg.option<ConfigOptionInt>("support_filament")->value = 2;
        cfg.option<ConfigOptionInt>("support_interface_filament")->value = 3;
        Model        model;
        ModelObject *object = add_cube_object(model);
        std::vector<int> ids;
        append_object_plate_filament_ids(*object, cfg, ids);
        ids = unique_positive(ids);
        CHECK_FALSE(contains_id(ids, 2));
        CHECK_FALSE(contains_id(ids, 3));
    }
    SECTION("a negative volume does not contribute per-feature filaments") {
        cfg.option<ConfigOptionInt>("outer_wall_filament")->value = 1;
        cfg.option<ConfigOptionBool>("enable_support")->value     = false;
        Model        model;
        ModelObject *object = add_cube_object(model);
        ModelVolume *negative = object->add_volume(make_cube(2., 2., 2.));
        negative->set_type(ModelVolumeType::NEGATIVE_VOLUME);
        negative->config.set("outer_wall_filament", 2);
        std::vector<int> ids;
        append_object_plate_filament_ids(*object, cfg, ids);
        CHECK_FALSE(contains_id(unique_positive(ids), 2));
    }
}

TEST_CASE("U1 CLI smoke: outer_wall_filament=2 with a real Model yields two plate filaments, a tower, and G-code markers", "[MixedFilamentCli]")
{
    // Snapmaker U1: four nozzles, not dual-nozzle (size==2). One filament still yields depth 0;
    // two project filament ids (volume 1 + outer wall 2) must produce a tower. This is the CLI
    // plate-set collection get_extruders_under_cli uses, including objects without support.
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_extruders(4);
    cfg.set_num_filaments(4);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4, 0.4, 0.4};
    cfg.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75, 1.75};
    cfg.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    cfg.option<ConfigOptionBool>("enable_prime_tower")->value   = true;
    cfg.option<ConfigOptionBool>("enable_support")->value       = false;
    // full_print_config defaults SEMM+purge_in_prime_tower on, which sizes a tower even for one
    // filament. U1 is not SEMM; turn that path off so a 1-id plate still has depth 0.
    cfg.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    cfg.option<ConfigOptionBool>("purge_in_prime_tower")->value = false;
    if (auto *wrapping = cfg.option<ConfigOptionBool>("enable_wrapping_detection"))
        wrapping->value = false;
    cfg.option<ConfigOptionInt>("raft_layers")->value           = 0;
    cfg.option<ConfigOptionInt>("outer_wall_filament")->value   = 2;
    cfg.option<ConfigOptionInt>("wall_filament")->value         = 1;
    cfg.option<ConfigOptionInt>("sparse_infill_filament")->value = 1;
    cfg.option<ConfigOptionInt>("solid_infill_filament")->value  = 1;
    cfg.option<ConfigOptionFloats>("wipe_tower_x")->values       = {15.};
    cfg.option<ConfigOptionFloats>("wipe_tower_y")->values       = {15.};
    cfg.option<ConfigOptionFloat>("prime_tower_width")->value    = 35.;
    cfg.set_key_value("printer_model", new ConfigOptionString("Snapmaker U1"));

    Model        model;
    ModelObject *object = add_cube_object(model);
    ModelVolume *modifier = object->add_volume(make_cube(4., 4., 4.));
    modifier->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    modifier->config.set("wall_filament", 1);

    std::vector<int> plate_ids;
    append_object_plate_filament_ids(*object, cfg, plate_ids);
    std::vector<Model> models;
    models.push_back(std::move(model));
    collect_cli_filament_ids(models, cfg, plate_ids);
    plate_ids = unique_positive(plate_ids);
    REQUIRE(contains_id(plate_ids, 1));
    REQUIRE(contains_id(plate_ids, 2));
    REQUIRE(plate_ids.size() == 2);

    std::vector<unsigned int> filament_ids;
    for (int id : plate_ids)
        filament_ids.push_back(static_cast<unsigned int>(id - 1));
    const WipeTowerFootprint one = estimate_wipe_tower_footprint(cfg, WipeTowerType::Type2, {0}, 0.2, 20.);
    const WipeTowerFootprint two = estimate_wipe_tower_footprint(cfg, WipeTowerType::Type2, filament_ids, 0.2, 20.);
    CHECK(one.depth == 0.);
    CHECK(two.depth > 0.);

    Print              print;
    Model              slice_model;
    Slic3r::Test::init_print({Slic3r::Test::TestMesh::cube_20x20x20}, print, slice_model, cfg);
    REQUIRE(print.has_wipe_tower());
    const std::string gcode = Slic3r::Test::gcode(print);
    REQUIRE(gcode.find("WIPE_TOWER_START") != std::string::npos);
    REQUIRE(gcode.find("CP TOOLCHANGE") != std::string::npos);
}

TEST_CASE("object extruder 2 is the plate set without a phantom 1", "[MixedFilamentCli]")
{
    DynamicPrintConfig cfg = four_physical_filament_config();
    cfg.option<ConfigOptionBool>("enable_support")->value = false;
    cfg.option<ConfigOptionInt>("raft_layers")->value     = 0;
    cfg.set_deserialize_strict({{"brim_type", "no_brim"}});

    Model        model;
    ModelObject *object = add_cube_object(model);
    object->config.set("extruder", 2);

    std::vector<int> ids;
    append_object_plate_filament_ids(*object, cfg, ids);
    REQUIRE(unique_positive(ids) == std::vector<int>{2});
}

TEST_CASE("part extruder 3 does not inject filament 1", "[MixedFilamentCli]")
{
    DynamicPrintConfig cfg = four_physical_filament_config();
    cfg.option<ConfigOptionBool>("enable_support")->value = false;
    cfg.option<ConfigOptionInt>("raft_layers")->value     = 0;
    cfg.set_deserialize_strict({{"brim_type", "no_brim"}});

    Model        model;
    ModelObject *object = add_cube_object(model);
    object->volumes.front()->config.set("extruder", 3);

    std::vector<int> ids;
    append_object_plate_filament_ids(*object, cfg, ids);
    ids = unique_positive(ids);
    CHECK_FALSE(contains_id(ids, 1));
    CHECK(contains_id(ids, 3));
}

TEST_CASE("height range extruder 2 does not inject filament 1", "[MixedFilamentCli]")
{
    DynamicPrintConfig cfg = four_physical_filament_config();
    cfg.option<ConfigOptionBool>("enable_support")->value = false;
    cfg.option<ConfigOptionInt>("raft_layers")->value     = 0;
    cfg.set_deserialize_strict({{"brim_type", "no_brim"}});

    Model        model;
    ModelObject *object = add_cube_object(model);
    object->layer_config_ranges[t_layer_height_range{0.0, 10.0}].set("extruder", 2);

    std::vector<int> ids;
    append_object_plate_filament_ids(*object, cfg, ids);
    ids = unique_positive(ids);
    CHECK_FALSE(contains_id(ids, 1));
    CHECK(contains_id(ids, 2));
}

TEST_CASE("object sparse_infill_filament 0 keeps the global sparse filament", "[MixedFilamentCli]")
{
    DynamicPrintConfig cfg = four_physical_filament_config();
    cfg.option<ConfigOptionBool>("enable_support")->value        = false;
    cfg.option<ConfigOptionInt>("raft_layers")->value            = 0;
    cfg.option<ConfigOptionInt>("sparse_infill_filament")->value = 2;
    cfg.set_deserialize_strict({{"brim_type", "no_brim"}});

    Model        model;
    ModelObject *object = add_cube_object(model);
    object->config.set("sparse_infill_filament", 0);

    std::vector<int> ids;
    append_object_plate_filament_ids(*object, cfg, ids);
    CHECK(contains_id(unique_positive(ids), 2));
}

TEST_CASE("U1 CLI smoke: support plus independent_support_layer_height still keeps the tower", "[MixedFilamentCli]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_extruders(4);
    cfg.set_num_filaments(4);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4, 0.4, 0.4};
    cfg.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75, 1.75};
    cfg.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    cfg.option<ConfigOptionBool>("enable_prime_tower")->value   = true;
    cfg.option<ConfigOptionBool>("enable_support")->value       = true;
    cfg.option<ConfigOptionBool>("independent_support_layer_height")->value = true;
    cfg.option<ConfigOptionBool>("single_extruder_multi_material")->value   = false;
    cfg.option<ConfigOptionBool>("purge_in_prime_tower")->value             = false;
    if (auto *wrapping = cfg.option<ConfigOptionBool>("enable_wrapping_detection"))
        wrapping->value = false;
    cfg.option<ConfigOptionInt>("raft_layers")->value           = 0;
    cfg.option<ConfigOptionInt>("outer_wall_filament")->value   = 2;
    cfg.option<ConfigOptionInt>("wall_filament")->value         = 1;
    cfg.option<ConfigOptionInt>("sparse_infill_filament")->value = 1;
    cfg.option<ConfigOptionInt>("solid_infill_filament")->value  = 1;
    cfg.option<ConfigOptionFloats>("wipe_tower_x")->values       = {15.};
    cfg.option<ConfigOptionFloats>("wipe_tower_y")->values       = {15.};
    cfg.option<ConfigOptionFloat>("prime_tower_width")->value    = 35.;
    cfg.set_key_value("printer_model", new ConfigOptionString("Snapmaker U1"));

    Model        model;
    ModelObject *object = add_cube_object(model);
    std::vector<int> plate_ids;
    append_object_plate_filament_ids(*object, cfg, plate_ids);
    plate_ids = unique_positive(plate_ids);
    REQUIRE(contains_id(plate_ids, 1));
    REQUIRE(contains_id(plate_ids, 2));

    Print              print;
    Model              slice_model;
    Slic3r::Test::init_print({Slic3r::Test::TestMesh::cube_20x20x20}, print, slice_model, cfg);
    REQUIRE(print.has_wipe_tower());
    REQUIRE_FALSE(print.config().independent_support_layer_height.value);
    const std::string gcode = Slic3r::Test::gcode(print);
    REQUIRE(gcode.find("WIPE_TOWER_START") != std::string::npos);
}

TEST_CASE("U1 CLI smoke: object on filament 2 has plate set {2} and no tower", "[MixedFilamentCli]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_extruders(4);
    cfg.set_num_filaments(4);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4, 0.4, 0.4};
    cfg.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75, 1.75};
    cfg.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    cfg.option<ConfigOptionBool>("enable_prime_tower")->value   = true;
    cfg.option<ConfigOptionBool>("enable_support")->value       = false;
    cfg.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    cfg.option<ConfigOptionBool>("purge_in_prime_tower")->value           = false;
    if (auto *wrapping = cfg.option<ConfigOptionBool>("enable_wrapping_detection"))
        wrapping->value = false;
    cfg.option<ConfigOptionInt>("raft_layers")->value         = 0;
    cfg.option<ConfigOptionFloats>("wipe_tower_x")->values     = {15.};
    cfg.option<ConfigOptionFloats>("wipe_tower_y")->values     = {15.};
    cfg.option<ConfigOptionFloat>("prime_tower_width")->value  = 35.;
    cfg.set_key_value("printer_model", new ConfigOptionString("Snapmaker U1"));
    cfg.set_deserialize_strict({{"brim_type", "no_brim"}});

    auto check_single_filament = [&](int filament_1based) {
        Model        model;
        ModelObject *object = add_cube_object(model);
        object->config.set("extruder", filament_1based);
        std::vector<int> plate_ids;
        append_object_plate_filament_ids(*object, cfg, plate_ids);
        REQUIRE(unique_positive(plate_ids) == std::vector<int>{filament_1based});

        Print print;
        print.apply(model, cfg);
        print.set_status_silent();
        REQUIRE(print.extruders() == std::vector<unsigned int>{unsigned(filament_1based - 1)});
        REQUIRE_FALSE(print.has_wipe_tower());
        REQUIRE_FALSE(print.config().enable_prime_tower.value);
        const std::string gcode = Slic3r::Test::gcode(print);
        CHECK(gcode.find("WIPE_TOWER_START") == std::string::npos);
    };

    SECTION("filament 2") { check_single_filament(2); }
    SECTION("filament 3") { check_single_filament(3); }
}

TEST_CASE("painted facets contribute their filament to the plate set", "[MixedFilamentCli]")
{
    DynamicPrintConfig cfg = four_physical_filament_config();
    cfg.option<ConfigOptionBool>("enable_support")->value = false;
    cfg.option<ConfigOptionInt>("raft_layers")->value     = 0;
    cfg.set_deserialize_strict({{"brim_type", "no_brim"}});

    Model        model;
    ModelObject *object = add_cube_object(model);
    paint_all_facets(*object->volumes.front(), EnforcerBlockerType::Extruder2);

    std::vector<int> ids;
    append_object_plate_filament_ids(*object, cfg, ids);
    CHECK(contains_id(unique_positive(ids), 2));
}

TEST_CASE("print-options-only global config does not clamp plate filaments to 1", "[MixedFilamentCli]")
{
    DynamicPrintConfig cfg;
    cfg.set_deserialize_strict({{"brim_type", "no_brim"}});

    SECTION("object extruder=3 gives {3}") {
        Model        model;
        ModelObject *object = add_cube_object(model);
        object->config.set("extruder", 3);
        std::vector<int> ids;
        append_object_plate_filament_ids(*object, cfg, ids);
        REQUIRE(unique_positive(ids) == std::vector<int>{3});
    }
    SECTION("outer_wall_filament=2 is kept") {
        cfg.set_key_value("outer_wall_filament", new ConfigOptionInt(2));
        Model        model;
        ModelObject *object = add_cube_object(model);
        std::vector<int> ids;
        append_object_plate_filament_ids(*object, cfg, ids);
        CHECK(contains_id(unique_positive(ids), 2));
    }
}

TEST_CASE("height range leaves the rest of the object on the default filament", "[MixedFilamentCli]")
{
    DynamicPrintConfig cfg = four_physical_filament_config();
    cfg.option<ConfigOptionBool>("enable_support")->value = false;
    cfg.option<ConfigOptionInt>("raft_layers")->value     = 0;
    cfg.set_deserialize_strict({{"brim_type", "no_brim"}});

    Model        model;
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();
    object->layer_config_ranges[t_layer_height_range{10.0, 20.0}].set("extruder", 2);
    object->layer_config_ranges[t_layer_height_range{10.0, 20.0}].set("layer_height", 0.2);

    std::vector<int> ids;
    append_object_plate_filament_ids(*object, cfg, ids);
    ids = unique_positive(ids);
    REQUIRE(contains_id(ids, 1));
    REQUIRE(contains_id(ids, 2));
}

TEST_CASE("U1 CLI smoke: painted cube on filament 1 with paint 2 has a tower", "[MixedFilamentCli]")
{
    DynamicPrintConfig cfg = u1_cli_config();
    Print              print;
    Model              model;
    Slic3r::Test::init_print({Slic3r::Test::TestMesh::cube_20x20x20}, print, model, cfg);
    paint_all_facets(*model.objects.front()->volumes.front(), EnforcerBlockerType::Extruder2);
    print.apply(model, cfg);

    std::vector<int> plate_ids;
    append_object_plate_filament_ids(*model.objects.front(), cfg, plate_ids);
    plate_ids = unique_positive(plate_ids);
    REQUIRE(contains_id(plate_ids, 1));
    REQUIRE(contains_id(plate_ids, 2));

    std::vector<unsigned int> filament_ids;
    for (int id : plate_ids)
        filament_ids.push_back(static_cast<unsigned int>(id - 1));
    const WipeTowerFootprint two = estimate_wipe_tower_footprint(cfg, WipeTowerType::Type2, filament_ids, 0.2, 20.);
    CHECK(two.depth > 0.);
    REQUIRE(print.has_wipe_tower());
    const std::string gcode = Slic3r::Test::gcode(print);
    REQUIRE(gcode.find("WIPE_TOWER_START") != std::string::npos);
}

TEST_CASE("U1 CLI smoke: 20 mm cube with range 10-20 extruder=2 has a tower", "[MixedFilamentCli]")
{
    DynamicPrintConfig cfg = u1_cli_config();
    Print              print;
    Model              model;
    Slic3r::Test::init_print({Slic3r::Test::TestMesh::cube_20x20x20}, print, model, cfg);
    model.objects.front()->layer_config_ranges[t_layer_height_range{10.0, 20.0}].set("extruder", 2);
    model.objects.front()->layer_config_ranges[t_layer_height_range{10.0, 20.0}].set("layer_height", 0.2);
    print.apply(model, cfg);

    std::vector<int> plate_ids;
    append_object_plate_filament_ids(*model.objects.front(), cfg, plate_ids);
    plate_ids = unique_positive(plate_ids);
    REQUIRE(contains_id(plate_ids, 1));
    REQUIRE(contains_id(plate_ids, 2));

    std::vector<unsigned int> filament_ids;
    for (int id : plate_ids)
        filament_ids.push_back(static_cast<unsigned int>(id - 1));
    const WipeTowerFootprint two = estimate_wipe_tower_footprint(cfg, WipeTowerType::Type2, filament_ids, 0.2, 20.);
    CHECK(two.depth > 0.);
    REQUIRE(print.has_wipe_tower());
    const std::string gcode = Slic3r::Test::gcode(print);
    REQUIRE(gcode.find("WIPE_TOWER_START") != std::string::npos);
}

TEST_CASE("U1 CLI smoke: non-intersecting modifier with wall_filament=3 does not produce a tower", "[MixedFilamentCli]")
{
    DynamicPrintConfig cfg = u1_cli_config();
    Print              print;
    Model              model;
    Slic3r::Test::init_print({Slic3r::Test::TestMesh::cube_20x20x20}, print, model, cfg);
    ModelVolume *modifier = model.objects.front()->add_volume(make_cube(5., 5., 5.));
    modifier->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    modifier->set_offset(Vec3d(1000., 0., 0.));
    modifier->config.set("wall_filament", 3);
    print.apply(model, cfg);

    std::vector<int> plate_ids;
    append_object_plate_filament_ids(*model.objects.front(), cfg, plate_ids);
    plate_ids = unique_positive(plate_ids);
    CHECK_FALSE(contains_id(plate_ids, 3));

    std::vector<unsigned int> filament_ids;
    for (int id : plate_ids)
        filament_ids.push_back(static_cast<unsigned int>(id - 1));
    const WipeTowerFootprint fp = estimate_wipe_tower_footprint(cfg, WipeTowerType::Type2, filament_ids, 0.2, 20.);
    CHECK(fp.depth == 0.);
    REQUIRE_FALSE(print.has_wipe_tower());
}

TEST_CASE("Precise Seam helper does not count as a plate filament or turn the tower on", "[MixedFilamentCli][PreciseSeam]")
{
    DynamicPrintConfig cfg = u1_cli_config();
    Print              print;
    Model              model;
    Slic3r::Test::init_print({Slic3r::Test::TestMesh::cube_20x20x20}, print, model, cfg);
    ModelVolume *helper = model.objects.front()->add_volume(make_cube(2., 2., 2.));
    helper->set_type(ModelVolumeType::PRECISE_SEAM_LEFT);
    helper->config.set("extruder", 3);
    helper->config.set("wall_filament", 4);
    print.apply(model, cfg);

    REQUIRE(helper->is_precise_seam());
    CHECK_FALSE(volume_contributes_feature_filaments(*helper));

    std::vector<int> plate_ids;
    append_object_plate_filament_ids(*model.objects.front(), cfg, plate_ids);
    plate_ids = unique_positive(plate_ids);
    CHECK_FALSE(contains_id(plate_ids, 3));
    CHECK_FALSE(contains_id(plate_ids, 4));

    std::vector<unsigned int> filament_ids;
    for (int id : plate_ids)
        filament_ids.push_back(static_cast<unsigned int>(id - 1));
    const WipeTowerFootprint fp = estimate_wipe_tower_footprint(cfg, WipeTowerType::Type2, filament_ids, 0.2, 20.);
    CHECK(fp.depth == 0.);
    REQUIRE_FALSE(print.has_wipe_tower());
}
