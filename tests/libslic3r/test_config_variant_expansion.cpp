#include <catch2/catch_all.hpp>

#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

// A 2-extruder printer whose second extruder holds both a Standard and a High Flow nozzle
// (nozzle_volume_type Hybrid), described by extruder_nozzle_stats. The variant lists carry one
// column per (extruder x volume type) as composed from the presets.
DynamicPrintConfig make_hybrid_printer_config()
{
    DynamicPrintConfig config;
    config.option<ConfigOptionStrings>("extruder_nozzle_stats", true)->values = {"Standard#1", "Standard#3|High Flow#2"};
    config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive, etDirectDrive};
    config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nvtStandard, nvtHybrid};
    config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {"Direct Drive Standard,Direct Drive High Flow",
                                                                                 "Direct Drive Standard,Direct Drive High Flow"};
    return config;
}

void add_print_variant_columns(DynamicPrintConfig &config)
{
    config.option<ConfigOptionInts>("print_extruder_id", true)->values = {1, 1, 2, 2};
    config.option<ConfigOptionStrings>("print_extruder_variant", true)->values = {"Direct Drive Standard", "Direct Drive High Flow",
                                                                                  "Direct Drive Standard", "Direct Drive High Flow"};
    config.option<ConfigOptionFloats>("outer_wall_speed", true)->values = {30., 200., 50., 500.};
}

} // namespace

TEST_CASE("apply_override fills nil entries from the 0-based default index", "[Config]")
{
    ConfigOptionFloats machine({10., 20., 30.});
    ConfigOptionFloatsNullable filament;
    filament.values = {ConfigOptionFloatsNullable::nil_value(), 42.};

    SECTION("a nil entry picks the slot addressed by its 0-based index") {
        std::vector<int> slot_index{2, 0};
        ConfigOptionFloats resolved(machine);
        REQUIRE(resolved.apply_override(&filament, slot_index));
        REQUIRE(resolved.values == std::vector<double>({30., 42.}));
    }

    SECTION("an index past the machine slots keeps the slot's own value") {
        std::vector<int> slot_index{5, 0};
        ConfigOptionFloats resolved(machine);
        REQUIRE(resolved.apply_override(&filament, slot_index));
        REQUIRE(resolved.values == std::vector<double>({10., 42.}));
    }

    SECTION("a negative index (unresolved slot) keeps the slot's own value") {
        ConfigOptionFloatsNullable all_nil;
        all_nil.values = {ConfigOptionFloatsNullable::nil_value(), ConfigOptionFloatsNullable::nil_value(),
                          ConfigOptionFloatsNullable::nil_value()};
        std::vector<int> slot_index{2, -1, 0};
        ConfigOptionFloats resolved(machine);
        REQUIRE(!resolved.apply_override(&all_nil, slot_index));
        REQUIRE(resolved.values == std::vector<double>({30., 20., 10.}));
    }

    SECTION("all-nil overrides keyed by unresolved slots leave the machine values intact") {
        // The failed-lookup map a degenerate print_extruder_id used to produce; the negative
        // slots must not collapse the machine array to its first value.
        ConfigOptionFloats per_extruder({100., 70., 70., 70., 100.});
        ConfigOptionFloatsNullable all_nil;
        all_nil.values.assign(5, ConfigOptionFloatsNullable::nil_value());
        std::vector<int> slot_index{0, -1, -1, -1, 0};
        ConfigOptionFloats resolved(per_extruder);
        REQUIRE(!resolved.apply_override(&all_nil, slot_index));
        REQUIRE(resolved.values == std::vector<double>({100., 70., 70., 70., 100.}));
    }
}

TEST_CASE("support_different_extruders is true only when the printer defines more than one variant column", "[Config]")
{
    int extruder_count = 0;

    SECTION("a non-Bambu dual-nozzle printer with one variant column reports false") {
        DynamicPrintConfig config;
        config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = {0.4, 0.4};
        // Both extruders resolve to the same default variant, so there is only one column.
        config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {"Direct Drive Standard",
                                                                                     "Direct Drive Standard"};
        REQUIRE(config.support_different_extruders(extruder_count) == false);
        REQUIRE(extruder_count == 2);
    }

    SECTION("a Bambu H2D-style printer with distinct variants reports true") {
        DynamicPrintConfig config;
        config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = {0.4, 0.4};
        config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {
            "Direct Drive Standard,Direct Drive High Flow",
            "Direct Drive Standard,Direct Drive High Flow,Direct Drive TPU High Flow"};
        REQUIRE(config.support_different_extruders(extruder_count) == true);
        REQUIRE(extruder_count == 2);
    }

    SECTION("a many-toolhead printer that never opts into variants reports false") {
        // A Snapmaker U1 has four identical toolheads and never defines extruder_variant_list,
        // so the config falls back to a single default variant token.
        DynamicPrintConfig config;
        config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = {0.4, 0.4, 0.4, 0.4};
        REQUIRE(config.support_different_extruders(extruder_count) == false);
        REQUIRE(extruder_count == 4);
    }
}

TEST_CASE("get_config_index_base resolves (volume type, extruder type, id) to a slot", "[Config]")
{
    const std::vector<std::string> variant_list = {"Direct Drive Standard", "Direct Drive High Flow",
                                                   "Direct Drive Standard", "Direct Drive High Flow"};
    const std::vector<int> variant_ids = {1, 1, 2, 2};

    SECTION("a matching (variant, id) pair yields its slot") {
        REQUIRE(get_config_index_base(nvtStandard, etDirectDrive, 1, variant_list, variant_ids) == 0);
        REQUIRE(get_config_index_base(nvtHighFlow, etDirectDrive, 1, variant_list, variant_ids) == 1);
        REQUIRE(get_config_index_base(nvtStandard, etDirectDrive, 2, variant_list, variant_ids) == 2);
        REQUIRE(get_config_index_base(nvtHighFlow, etDirectDrive, 2, variant_list, variant_ids) == 3);
    }

    SECTION("no matching variant falls back to the id's first variant") {
        REQUIRE(get_config_index_base(nvtStandard, etBowden, 1, variant_list, variant_ids) == 0);
        REQUIRE(get_config_index_base(nvtStandard, etBowden, 2, variant_list, variant_ids) == 2);
    }

    SECTION("an id without any variant falls back to variant index 0") {
        REQUIRE(get_config_index_base(nvtStandard, etDirectDrive, 3, variant_list, variant_ids) == 0);
    }

    SECTION("Hybrid is not a preset variant string and falls back to the id's first variant") {
        REQUIRE(get_config_index_base(nvtHybrid, etDirectDrive, 2, variant_list, variant_ids) == 2);
    }

    SECTION("a filament without a High Flow variant keeps its own variant, not the first filament's") {
        // filament 1 defines Standard and High Flow, filament 2 only Standard
        const std::vector<std::string> mixed_list = {"Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Standard"};
        const std::vector<int>         mixed_ids  = {1, 1, 2};
        REQUIRE(get_config_index_base(nvtHighFlow, etDirectDrive, 1, mixed_list, mixed_ids) == 1);
        REQUIRE(get_config_index_base(nvtHighFlow, etDirectDrive, 2, mixed_list, mixed_ids) == 2);
    }
}

TEST_CASE("support interface pattern registry includes spiral inset", "[Config]")
{
    const auto &values = ConfigOptionEnum<SupportMaterialInterfacePattern>::get_enum_values();
    REQUIRE(values.at("spiralinset") == SupportMaterialInterfacePattern::smipSpiralInset);
}

TEST_CASE("get_extruder_nozzle_volume_count reads the per-extruder volume-type layout", "[Config]")
{
    std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;

    SECTION("absent stats fall back to one slot per extruder") {
        DynamicPrintConfig config;
        REQUIRE(config.get_extruder_nozzle_volume_count(2, nozzle_volume_types) == 2);
        REQUIRE(nozzle_volume_types.size() == 2);
        REQUIRE(nozzle_volume_types[0].empty());
        REQUIRE(nozzle_volume_types[1].empty());
    }

    SECTION("stats sized differently from the extruder count are ignored") {
        DynamicPrintConfig config;
        config.option<ConfigOptionStrings>("extruder_nozzle_stats", true)->values = {"Standard#1"};
        REQUIRE(config.get_extruder_nozzle_volume_count(2, nozzle_volume_types) == 2);
        REQUIRE(nozzle_volume_types[0].empty());
        REQUIRE(nozzle_volume_types[1].empty());
    }

    SECTION("single volume type per extruder counts one slot each") {
        DynamicPrintConfig config;
        config.option<ConfigOptionStrings>("extruder_nozzle_stats", true)->values = {"Standard#1", "High Flow#1"};
        REQUIRE(config.get_extruder_nozzle_volume_count(2, nozzle_volume_types) == 2);
        REQUIRE(nozzle_volume_types[0] == std::vector<NozzleVolumeType>{nvtStandard});
        REQUIRE(nozzle_volume_types[1] == std::vector<NozzleVolumeType>{nvtHighFlow});
    }

    SECTION("a mixed-nozzle extruder contributes one slot per volume type, ascending enum order") {
        DynamicPrintConfig config;
        // list High Flow first in the token string: parsing must still order Standard before High Flow
        config.option<ConfigOptionStrings>("extruder_nozzle_stats", true)->values = {"Standard#3", "High Flow#3|Standard#3"};
        REQUIRE(config.get_extruder_nozzle_volume_count(2, nozzle_volume_types) == 3);
        REQUIRE(nozzle_volume_types[0] == std::vector<NozzleVolumeType>{nvtStandard});
        REQUIRE(nozzle_volume_types[1] == std::vector<NozzleVolumeType>({nvtStandard, nvtHighFlow}));
    }
}

TEST_CASE("update_values_to_printer_extruders expands one slot per (extruder x volume type)", "[Config]")
{
    SECTION("Hybrid extruder yields three slots, extruder-ascending then volume-ascending") {
        DynamicPrintConfig config = make_hybrid_printer_config();
        add_print_variant_columns(config);

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);
        REQUIRE(count == 3);

        std::vector<int> variant_index = config.update_values_to_printer_extruders(config, extruder_count, count, nozzle_volume_types,
            print_options_with_variant, "print_extruder_id", "print_extruder_variant");

        REQUIRE(variant_index == std::vector<int>({0, 2, 3}));
        REQUIRE(config.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>({30., 50., 500.}));
        REQUIRE(config.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>({1, 2, 2}));
        REQUIRE(config.option<ConfigOptionStrings>("print_extruder_variant")->values ==
                std::vector<std::string>({"Direct Drive Standard", "Direct Drive Standard", "Direct Drive High Flow"}));
    }

    SECTION("stride-2 options keep (normal, silent) pairs together per slot") {
        DynamicPrintConfig config = make_hybrid_printer_config();
        config.option<ConfigOptionInts>("printer_extruder_id", true)->values = {1, 1, 2, 2};
        config.option<ConfigOptionStrings>("printer_extruder_variant", true)->values = {"Direct Drive Standard", "Direct Drive High Flow",
                                                                                        "Direct Drive Standard", "Direct Drive High Flow"};
        config.option<ConfigOptionFloats>("machine_max_speed_x", true)->values = {100., 50., 110., 55., 120., 60., 130., 65.};

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

        std::vector<int> variant_index = config.update_values_to_printer_extruders(config, extruder_count, count, nozzle_volume_types,
            printer_options_with_variant_2, "printer_extruder_id", "printer_extruder_variant", 2);

        REQUIRE(variant_index == std::vector<int>({0, 2, 3}));
        REQUIRE(config.option<ConfigOptionFloats>("machine_max_speed_x")->values ==
                std::vector<double>({100., 50., 120., 60., 130., 65.}));
    }

    SECTION("single-slot expansion on a Hybrid extruder resolves via the filament volume type") {
        DynamicPrintConfig printer_config = make_hybrid_printer_config();

        DynamicPrintConfig filament_config;
        filament_config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {"Direct Drive Standard", "Direct Drive High Flow"};
        filament_config.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = {12., 20.};

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = printer_config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

        SECTION("default filament volume type selects the Standard column") {
            std::vector<int> variant_index = filament_config.update_values_to_printer_extruders(printer_config, extruder_count, count,
                nozzle_volume_types, filament_options_with_variant, "", "filament_extruder_variant", 1, 2);
            REQUIRE(variant_index == std::vector<int>({0}));
            REQUIRE(filament_config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>({12.}));
        }

        SECTION("a High Flow filament volume type selects the High Flow column") {
            std::vector<int> variant_index = filament_config.update_values_to_printer_extruders(printer_config, extruder_count, count,
                nozzle_volume_types, filament_options_with_variant, "", "filament_extruder_variant", 1, 2, nvtHighFlow);
            REQUIRE(variant_index == std::vector<int>({1}));
            REQUIRE(filament_config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>({20.}));
        }
    }

    SECTION("an extruder without per-type stats does not overrun the slot table when another is Hybrid") {
        DynamicPrintConfig config;
        // e0 carries no per-type stats (empty entry), so the summed volume-type count (2) does
        // not exceed the extruder count even though the Hybrid e1 emits one slot per volume type.
        config.option<ConfigOptionStrings>("extruder_nozzle_stats", true)->values = {"", "Standard#3|High Flow#3"};
        config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive, etDirectDrive};
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nvtStandard, nvtHybrid};
        config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {"Direct Drive Standard,Direct Drive High Flow",
                                                                                     "Direct Drive Standard,Direct Drive High Flow"};
        add_print_variant_columns(config);

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);
        REQUIRE(count == 2);
        REQUIRE(nozzle_volume_types[0].empty());

        std::vector<int> variant_index = config.update_values_to_printer_extruders(config, extruder_count, count, nozzle_volume_types,
            print_options_with_variant, "print_extruder_id", "print_extruder_variant");

        // e0 resolves by its configured type; the Hybrid e1 emits one slot per stats volume type
        REQUIRE(variant_index == std::vector<int>({0, 2, 3}));
        REQUIRE(config.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>({30., 50., 500.}));
    }

    SECTION("without Hybrid or extra slots the expansion matches the per-extruder resolution") {
        DynamicPrintConfig config;
        config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive, etDirectDrive};
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nvtStandard, nvtHighFlow};
        config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {"Direct Drive Standard,Direct Drive High Flow",
                                                                                     "Direct Drive Standard,Direct Drive High Flow"};
        add_print_variant_columns(config);

        // compute what the per-extruder loop resolves directly, before the arrays are rewritten
        std::vector<int> expected_index;
        for (int e_index = 0; e_index < 2; e_index++)
            expected_index.push_back(config.get_index_for_extruder(e_index + 1, "print_extruder_id", etDirectDrive,
                e_index == 0 ? nvtStandard : nvtHighFlow, "print_extruder_variant"));
        REQUIRE(expected_index == std::vector<int>({0, 3}));

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);
        REQUIRE(count == 2);

        std::vector<int> variant_index = config.update_values_to_printer_extruders(config, extruder_count, count, nozzle_volume_types,
            print_options_with_variant, "print_extruder_id", "print_extruder_variant");

        REQUIRE(variant_index == expected_index);
        REQUIRE(config.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>({30., 500.}));
    }
}

TEST_CASE("update_values_to_printer_extruders synthesizes degenerate process variant columns", "[Config]")
{
    // Non-BBL process presets and 3mf project configs keep the length-1 defaults for
    // print_extruder_id/print_extruder_variant; only BBL system presets ship full-width columns.
    auto add_degenerate_print_columns = [](DynamicPrintConfig &config) {
        config.option<ConfigOptionInts>("print_extruder_id", true)->values = {1};
        config.option<ConfigOptionStrings>("print_extruder_variant", true)->values = {"Direct Drive Standard"};
        config.option<ConfigOptionFloats>("outer_wall_speed", true)->values = {30.};
    };

    SECTION("a single-column pair on a multi-extruder machine expands to one column per extruder") {
        DynamicPrintConfig config;
        config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive, etDirectDrive};
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nvtStandard, nvtStandard};
        config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {"Direct Drive Standard", "Direct Drive Standard"};
        add_degenerate_print_columns(config);

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

        std::vector<int> variant_index = config.update_values_to_printer_extruders(config, extruder_count, count, nozzle_volume_types,
            print_options_with_variant, "print_extruder_id", "print_extruder_variant");

        REQUIRE(variant_index == std::vector<int>({0, 1}));
        REQUIRE(config.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>({1, 2}));
        REQUIRE(config.option<ConfigOptionStrings>("print_extruder_variant")->values ==
                std::vector<std::string>({"Direct Drive Standard", "Direct Drive Standard"}));
        // width-1 data arrays replicate their only column into every slot
        REQUIRE(config.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>({30., 30.}));
    }

    SECTION("a multi-variant list synthesizes one column per (extruder x variant)") {
        DynamicPrintConfig config = make_hybrid_printer_config();
        add_degenerate_print_columns(config);

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);
        REQUIRE(count == 3);

        std::vector<int> variant_index = config.update_values_to_printer_extruders(config, extruder_count, count, nozzle_volume_types,
            print_options_with_variant, "print_extruder_id", "print_extruder_variant");

        // same slot resolution as the explicit BBL-style 4-column layout
        REQUIRE(variant_index == std::vector<int>({0, 2, 3}));
        REQUIRE(config.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>({1, 2, 2}));
        REQUIRE(config.option<ConfigOptionStrings>("print_extruder_variant")->values ==
                std::vector<std::string>({"Direct Drive Standard", "Direct Drive Standard", "Direct Drive High Flow"}));
        REQUIRE(config.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>({30., 30., 30.}));
    }

    SECTION("a single-extruder single-column layout is not treated as degenerate") {
        DynamicPrintConfig config;
        config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive};
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nvtStandard};
        config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {"Direct Drive Standard"};
        add_degenerate_print_columns(config);

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 1;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

        config.update_values_to_printer_extruders(config, extruder_count, count, nozzle_volume_types,
            print_options_with_variant, "print_extruder_id", "print_extruder_variant");

        REQUIRE(config.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>({1}));
        REQUIRE(config.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>({30.}));
    }

    SECTION("a second expansion leaves the synthesized layout unchanged") {
        DynamicPrintConfig config;
        config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive, etDirectDrive};
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nvtStandard, nvtStandard};
        config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {"Direct Drive Standard", "Direct Drive Standard"};
        add_degenerate_print_columns(config);

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

        config.update_values_to_printer_extruders(config, extruder_count, count, nozzle_volume_types,
            print_options_with_variant, "print_extruder_id", "print_extruder_variant");
        DynamicPrintConfig once = config;
        config.update_values_to_printer_extruders(config, extruder_count, count, nozzle_volume_types,
            print_options_with_variant, "print_extruder_id", "print_extruder_variant");

        REQUIRE(config.option<ConfigOptionInts>("print_extruder_id")->values ==
                once.option<ConfigOptionInts>("print_extruder_id")->values);
        REQUIRE(config.option<ConfigOptionStrings>("print_extruder_variant")->values ==
                once.option<ConfigOptionStrings>("print_extruder_variant")->values);
        REQUIRE(config.option<ConfigOptionFloats>("outer_wall_speed")->values ==
                once.option<ConfigOptionFloats>("outer_wall_speed")->values);
    }
}

TEST_CASE("update_values_to_printer_extruders_for_multiple_filaments resolves per-filament slots", "[Config]")
{
    auto make_filament_arrays = [](DynamicPrintConfig &config) {
        config.option<ConfigOptionInts>("filament_self_index", true)->values = {1, 1, 2, 2};
        config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {"Direct Drive Standard", "Direct Drive High Flow",
                                                                                         "Direct Drive Standard", "Direct Drive High Flow"};
        config.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = {12., 20., 13., 21.};
    };

    std::set<std::string> filament_keys = filament_options_with_variant;
    filament_keys.insert("filament_self_index");

    SECTION("filament_volume_map picks the concrete volume type on a Hybrid extruder") {
        DynamicPrintConfig config = make_hybrid_printer_config();
        make_filament_arrays(config);
        config.option<ConfigOptionInts>("filament_map", true)->values = {2, 2};
        config.option<ConfigOptionInts>("filament_volume_map", true)->values = {nvtStandard, nvtHighFlow};

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

        config.update_values_to_printer_extruders_for_multiple_filaments(config, extruder_count, count, filament_keys,
            "filament_self_index", "filament_extruder_variant");

        REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>({12., 21.}));
        REQUIRE(config.option<ConfigOptionStrings>("filament_extruder_variant")->values ==
                std::vector<std::string>({"Direct Drive Standard", "Direct Drive High Flow"}));
        REQUIRE(config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>({1, 2}));
    }

    SECTION("a volume map not sized to the filament count is ignored") {
        DynamicPrintConfig config = make_hybrid_printer_config();
        make_filament_arrays(config);
        config.option<ConfigOptionInts>("filament_map", true)->values = {2, 2};
        // the registered default is a single-element vector; it must not override slot resolution
        config.option<ConfigOptionInts>("filament_volume_map", true)->values = {nvtStandard};

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

        config.update_values_to_printer_extruders_for_multiple_filaments(config, extruder_count, count, filament_keys,
            "filament_self_index", "filament_extruder_variant");

        // Hybrid resolves as Standard when no usable per-filament map exists
        REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>({12., 13.}));
        REQUIRE(config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>({1, 2}));
    }

    SECTION("a single-filament explicit assignment on a Hybrid extruder is honored") {
        DynamicPrintConfig config = make_hybrid_printer_config();
        config.option<ConfigOptionInts>("filament_self_index", true)->values = {1, 1};
        config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {"Direct Drive Standard", "Direct Drive High Flow"};
        config.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = {12., 20.};
        config.option<ConfigOptionInts>("filament_map", true)->values = {2};
        // sized to the (single) filament count: the producers guarantee sizing, so a
        // single-filament map is as trustworthy as any other and the explicit High Flow
        // request must win over the Hybrid->Standard fallback
        config.option<ConfigOptionInts>("filament_volume_map", true)->values = {nvtHighFlow};

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

        config.update_values_to_printer_extruders_for_multiple_filaments(config, extruder_count, count, filament_keys,
            "filament_self_index", "filament_extruder_variant");

        REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>({20.}));
        REQUIRE(config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>({1}));
    }

    SECTION("without Hybrid or extra slots the volume map is not consulted") {
        DynamicPrintConfig config;
        config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive, etDirectDrive};
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nvtStandard, nvtHighFlow};
        config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {"Direct Drive Standard,Direct Drive High Flow",
                                                                                     "Direct Drive Standard,Direct Drive High Flow"};
        make_filament_arrays(config);
        config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2};
        // sized to the filament count, but inert because no extruder exposes multiple volume types
        config.option<ConfigOptionInts>("filament_volume_map", true)->values = {nvtHighFlow, nvtStandard};

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);
        REQUIRE(count == 2);

        config.update_values_to_printer_extruders_for_multiple_filaments(config, extruder_count, count, filament_keys,
            "filament_self_index", "filament_extruder_variant");

        // filament 1 keeps its extruder's Standard column, filament 2 its extruder's High Flow column
        REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>({12., 21.}));
        REQUIRE(config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>({1, 2}));
    }

    SECTION("a variant option shorter than the filament slots keeps its first value instead of zero") {
        DynamicPrintConfig config;
        config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive, etDirectDrive};
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nvtStandard, nvtHighFlow};
        config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {"Direct Drive Standard,Direct Drive High Flow",
                                                                                     "Direct Drive Standard,Direct Drive High Flow"};
        make_filament_arrays(config);
        config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2};
        // no loaded preset carries the key, so only its single registered default is present
        config.option<ConfigOptionFloatsNullable>("filament_cooling_before_tower", true)->values = {10.};
        // only the first filament's two variant columns were loaded
        config.option<ConfigOptionFloatsNullable>("filament_ramming_volumetric_speed", true)->values = {-1., -2.};

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

        config.update_values_to_printer_extruders_for_multiple_filaments(config, extruder_count, count, filament_keys,
            "filament_self_index", "filament_extruder_variant");

        // filament 2 resolves to column 3 (its extruder's High Flow column), past the end of both vectors
        REQUIRE_THAT(config.option<ConfigOptionFloatsNullable>("filament_cooling_before_tower")->values,
                     Catch::Matchers::Approx(std::vector<double>({10., 10.})));
        REQUIRE_THAT(config.option<ConfigOptionFloatsNullable>("filament_ramming_volumetric_speed")->values,
                     Catch::Matchers::Approx(std::vector<double>({-1., -1.})));
        REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>({12., 21.}));
    }

    // A single extruder whose only variant is Standard or High Flow; filament 1 defines both
    // variants, filament 2 only Standard.
    auto make_single_variant_config = [](NozzleVolumeType nozzle_volume_type, const std::string &variant) {
        DynamicPrintConfig config;
        config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive};
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nozzle_volume_type};
        config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {variant};
        config.option<ConfigOptionFloats>("filament_diameter", true)->values = {1.75, 1.75};
        config.option<ConfigOptionInts>("filament_self_index", true)->values = {1, 1, 2};
        config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {"Direct Drive Standard", "Direct Drive High Flow",
                                                                                         "Direct Drive Standard"};
        config.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = {12., 20., 13.};
        config.option<ConfigOptionInts>("filament_map", true)->values = {1, 1};
        return config;
    };

    SECTION("a single-variant printer picks each filament's variant of that variant string") {
        auto [nozzle_volume_type, variant, speeds] = GENERATE(table<NozzleVolumeType, std::string, std::vector<double>>({
            {nvtStandard, "Direct Drive Standard", {12., 13.}},
            // filament 2 defines no High Flow variant and keeps its first
            {nvtHighFlow, "Direct Drive High Flow", {20., 13.}},
        }));
        DynamicPrintConfig config = make_single_variant_config(nozzle_volume_type, variant);
        int extruder_count = 1;
        REQUIRE_FALSE(config.support_different_extruders(extruder_count));
        REQUIRE(config.has_multi_variant_filament());

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);
        config.update_values_to_printer_extruders_for_multiple_filaments(config, extruder_count, count, filament_keys,
            "filament_self_index", "filament_extruder_variant");

        REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == speeds);
        REQUIRE(config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>({1, 2}));
    }

    SECTION("a filament map shorter than the filament count keeps every filament") {
        DynamicPrintConfig config = make_single_variant_config(nvtStandard, "Direct Drive Standard");
        config.option<ConfigOptionInts>("filament_map", true)->values = {1};

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 1;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);
        config.update_values_to_printer_extruders_for_multiple_filaments(config, extruder_count, count, filament_keys,
            "filament_self_index", "filament_extruder_variant");

        REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>({12., 13.}));
        REQUIRE(config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>({1, 2}));
    }
}

TEST_CASE("has_multi_variant_filament is true only when a filament defines more than one variant", "[Config]")
{
    DynamicPrintConfig config;
    config.option<ConfigOptionFloats>("filament_diameter", true)->values = {1.75, 1.75};

    SECTION("one variant per filament reports false") {
        config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {"Direct Drive Standard", "Bowden Standard"};
        REQUIRE_FALSE(config.has_multi_variant_filament());
    }

    SECTION("a filament with Standard and High Flow variants reports true") {
        config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {"Direct Drive Standard", "Direct Drive High Flow",
                                                                                         "Direct Drive Standard"};
        REQUIRE(config.has_multi_variant_filament());
    }
}

// update_values_from_multi_to_multi_2 walks the DESTINATION PRINTER's variant list while writing
// into a row taken from the destination PRINT preset, whose arrays are sized to its own
// print_extruder_variant. Those two widths disagree until the print preset is re-selected for the
// new printer -- Tab::load_current_preset() runs this migration first -- so a project authored on
// a single-variant printer, opened and switched to a wider one, wrote past the end of the row.
TEST_CASE("update_values_from_multi_to_multi_2 sizes the destination row to the variant count",
          "[Config][VariantExpansion]")
{
    const std::vector<std::string> src_variants{"Direct Drive Standard"};
    const std::vector<std::string> dst_variants{"Direct Drive Standard", "Direct Drive High Flow",
                                                "Direct Drive Standard", "Direct Drive High Flow"};
    const std::set<std::string>    keys{"outer_wall_speed"};

    // The per-object override as authored on the single-variant printer.
    const auto object_override = [] {
        DynamicPrintConfig c;
        c.option<ConfigOptionFloatsNullable>("outer_wall_speed", true)->values = {42.};
        return c;
    };

    SECTION("a row narrower than the variant list is grown, not overrun") {
        DynamicPrintConfig object_config = object_override();
        DynamicPrintConfig dst;
        dst.option<ConfigOptionFloatsNullable>("outer_wall_speed", true)->values = {200.};

        REQUIRE(object_config.update_values_from_multi_to_multi_2(src_variants, dst_variants, dst, keys) == 0);

        const auto& out = object_config.option<ConfigOptionFloatsNullable>("outer_wall_speed")->values;
        REQUIRE(out.size() == dst_variants.size());
        // Both "Direct Drive Standard" columns match the source variant, so they take the override.
        CHECK(out[0] == Catch::Approx(42.));
        CHECK(out[2] == Catch::Approx(42.));
        // The High Flow columns have no matching source variant: nil, so the destination keeps
        // tracking the print preset rather than being pinned to another variant's value.
        CHECK(std::isnan(out[1]));
        CHECK(std::isnan(out[3]));
    }

    // The regression guard: where the row already matches the variant list -- every case that was
    // not corrupting the heap -- the resize is a no-op and the output is unchanged.
    SECTION("a correctly sized row is untouched") {
        DynamicPrintConfig object_config = object_override();
        DynamicPrintConfig dst;
        dst.option<ConfigOptionFloatsNullable>("outer_wall_speed", true)->values = {200., 500., 210., 510.};

        REQUIRE(object_config.update_values_from_multi_to_multi_2(src_variants, dst_variants, dst, keys) == 0);

        const auto& out = object_config.option<ConfigOptionFloatsNullable>("outer_wall_speed")->values;
        REQUIRE(out.size() == 4);
        CHECK(out[0] == Catch::Approx(42.));    // matched -> override
        CHECK(out[1] == Catch::Approx(500.));   // unmatched -> preset value preserved
        CHECK(out[2] == Catch::Approx(42.));
        CHECK(out[3] == Catch::Approx(510.));
    }

    // is_nil(idx) indexes values[idx] with no bounds check, so a source shorter than its own
    // variant list read out of range before the guard was added.
    SECTION("a source shorter than its variant list is read in range") {
        DynamicPrintConfig object_config = object_override();   // one value...
        DynamicPrintConfig dst;
        dst.option<ConfigOptionFloatsNullable>("outer_wall_speed", true)->values = {200., 500.};

        REQUIRE(object_config.update_values_from_multi_to_multi_2(
            {"Direct Drive Standard", "Direct Drive Standard"},   // ...but two source variants
            {"Direct Drive Standard", "Direct Drive High Flow"}, dst, keys) == 0);

        const auto& out = object_config.option<ConfigOptionFloatsNullable>("outer_wall_speed")->values;
        REQUIRE(out.size() == 2);
        CHECK(out[0] == Catch::Approx(42.));
        CHECK(out[1] == Catch::Approx(500.));
    }

    SECTION("an empty destination variant list is refused") {
        DynamicPrintConfig object_config = object_override();
        DynamicPrintConfig dst;
        dst.option<ConfigOptionFloatsNullable>("outer_wall_speed", true)->values = {200.};

        CHECK(object_config.update_values_from_multi_to_multi_2(src_variants, {}, dst, keys) == -1);
    }
}

TEST_CASE("get_index_for_extruder returns -1 when no variant column matches the extruder", "[Config]")
{
    DynamicPrintConfig config;
    config.option<ConfigOptionInts>("print_extruder_id", true)->values = {1};
    config.option<ConfigOptionStrings>("print_extruder_variant", true)->values = {"Direct Drive Standard"};

    SECTION("the extruder that owns the column resolves to its slot") {
        REQUIRE(config.get_index_for_extruder(1, "print_extruder_id", etDirectDrive, nvtStandard, "print_extruder_variant") == 0);
    }

    SECTION("an extruder with no matching column resolves to -1") {
        REQUIRE(config.get_index_for_extruder(2, "print_extruder_id", etDirectDrive, nvtStandard, "print_extruder_variant") == -1);
    }
}

// stride scales the returned slot so callers can address stride-2 options (machine_max_*, a
// Normal/Silent pair per column) by their pair's base slot. The printer Tab's extruder sync
// relies on this to copy the right slots on the Motion ability page.
TEST_CASE("get_index_for_extruder scales the variant column by the requested stride", "[Config]")
{
    DynamicPrintConfig config;
    config.option<ConfigOptionInts>("printer_extruder_id", true)->values = {1, 2};
    config.option<ConfigOptionStrings>("printer_extruder_variant", true)->values = {"Direct Drive Standard",
                                                                                    "Direct Drive High Flow"};

    // extruder 1 resolves to column 0, extruder 2 to column 1
    const int col0_stride1 = config.get_index_for_extruder(1, "printer_extruder_id", etDirectDrive, nvtStandard,
                                                           "printer_extruder_variant", 1);
    const int col1_stride1 = config.get_index_for_extruder(2, "printer_extruder_id", etDirectDrive, nvtHighFlow,
                                                           "printer_extruder_variant", 1);
    REQUIRE(col0_stride1 == 0);
    REQUIRE(col1_stride1 == 1);

    // stride 2 returns exactly twice the stride-1 index (the pair's base slot)
    const int col0_stride2 = config.get_index_for_extruder(1, "printer_extruder_id", etDirectDrive, nvtStandard,
                                                           "printer_extruder_variant", 2);
    const int col1_stride2 = config.get_index_for_extruder(2, "printer_extruder_id", etDirectDrive, nvtHighFlow,
                                                           "printer_extruder_variant", 2);
    REQUIRE(col0_stride2 == col0_stride1 * 2);
    REQUIRE(col1_stride2 == col1_stride1 * 2);
    REQUIRE(col0_stride2 == 0);
    REQUIRE(col1_stride2 == 2);
}

// ---------------------------------------------------------------------------------------------
// Snapmaker Orca: High Flow nozzles on a printer with one fixed nozzle per tool head.
// ---------------------------------------------------------------------------------------------

namespace {

const std::string DD_STANDARD  = "Direct Drive Standard";
const std::string DD_HIGH_FLOW = "Direct Drive High Flow";

// Four tool heads, each able to carry a Standard or a High Flow nozzle; heads 2 and 3 carry High Flow.
DynamicPrintConfig make_four_head_flow_printer_config()
{
    DynamicPrintConfig config;
    config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive, etDirectDrive, etDirectDrive, etDirectDrive};
    config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nvtStandard, nvtHighFlow, nvtHighFlow, nvtStandard};
    config.option<ConfigOptionStrings>("extruder_variant_list", true)->values =
        std::vector<std::string>(4, DD_STANDARD + "," + DD_HIGH_FLOW);
    return config;
}

} // namespace

TEST_CASE("A flow-only process preset gives every tool head the column of its flow type", "[Config][HighFlow][hf_flow_only_process_columns]")
{
    DynamicPrintConfig config = make_four_head_flow_printer_config();
    // One column per flow type instead of one per (head x flow type): all ids equal.
    config.option<ConfigOptionInts>("print_extruder_id", true)->values = {1, 1};
    config.option<ConfigOptionStrings>("print_extruder_variant", true)->values = {DD_STANDARD, DD_HIGH_FLOW};
    config.option<ConfigOptionFloats>("outer_wall_speed", true)->values = {200., 500.};

    SECTION("the lookup on the preset returns the flow column for every head") {
        CHECK(config.get_index_for_extruder(1, "print_extruder_id", etDirectDrive, nvtStandard, "print_extruder_variant") == 0);
        CHECK(config.get_index_for_extruder(1, "print_extruder_id", etDirectDrive, nvtHighFlow, "print_extruder_variant") == 1);
        CHECK(config.get_index_for_extruder(3, "print_extruder_id", etDirectDrive, nvtStandard, "print_extruder_variant") == 0);
        CHECK(config.get_index_for_extruder(3, "print_extruder_id", etDirectDrive, nvtHighFlow, "print_extruder_variant") == 1);
        // A flow type the preset has no column for still misses.
        CHECK(config.get_index_for_extruder(3, "print_extruder_id", etDirectDrive, nvtTPUHighFlow, "print_extruder_variant") == -1);
        CHECK(config.get_index_for_extruder(3, "print_extruder_id", etBowden, nvtHighFlow, "print_extruder_variant") == -1);
    }

    SECTION("narrowing keeps the returned indices in the preset's column space and rewrites the slot ids") {
        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        const int extruder_count = 4;
        const int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);
        REQUIRE(count == 4);

        const std::vector<int> variant_index = config.update_values_to_printer_extruders(config, extruder_count, count, nozzle_volume_types,
            print_options_with_variant, "print_extruder_id", "print_extruder_variant");

        // Per-object overrides are stored 2 wide and narrowed with these indices.
        REQUIRE(variant_index == std::vector<int>({0, 1, 1, 0}));
        REQUIRE(config.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>({200., 500., 500., 200.}));
        REQUIRE(config.option<ConfigOptionStrings>("print_extruder_variant")->values ==
                std::vector<std::string>({DD_STANDARD, DD_HIGH_FLOW, DD_HIGH_FLOW, DD_STANDARD}));
        // Without the rewrite this table would be {1, 1, 1, 1}.
        REQUIRE(config.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>({1, 2, 3, 4}));

        // Print::apply keys filament_map_2 by a lookup on the narrowed table: an exact match per head.
        CHECK(config.get_index_for_extruder(1, "print_extruder_id", etDirectDrive, nvtStandard, "print_extruder_variant") == 0);
        CHECK(config.get_index_for_extruder(2, "print_extruder_id", etDirectDrive, nvtHighFlow, "print_extruder_variant") == 1);
        CHECK(config.get_index_for_extruder(3, "print_extruder_id", etDirectDrive, nvtHighFlow, "print_extruder_variant") == 2);
        CHECK(config.get_index_for_extruder(4, "print_extruder_id", etDirectDrive, nvtStandard, "print_extruder_variant") == 3);
        // The narrowed table is a per-head layout: the flow-only rule no longer applies to it.
        CHECK(config.get_index_for_extruder(3, "print_extruder_id", etDirectDrive, nvtStandard, "print_extruder_variant") == -1);

        // A second narrowing of the narrowed config changes nothing.
        DynamicPrintConfig once = config;
        const std::vector<int> second_index = config.update_values_to_printer_extruders(config, extruder_count, count, nozzle_volume_types,
            print_options_with_variant, "print_extruder_id", "print_extruder_variant");
        CHECK(second_index == std::vector<int>({0, 1, 2, 3}));
        CHECK(config.option<ConfigOptionInts>("print_extruder_id")->values == once.option<ConfigOptionInts>("print_extruder_id")->values);
        CHECK(config.option<ConfigOptionFloats>("outer_wall_speed")->values == once.option<ConfigOptionFloats>("outer_wall_speed")->values);
    }
}

TEST_CASE("A single-column process preset expands per head and never takes the flow-only rule", "[Config][HighFlow][hf_expanded_single_column_process]")
{
    DynamicPrintConfig config = make_four_head_flow_printer_config();
    config.option<ConfigOptionInts>("print_extruder_id", true)->values = {1};
    config.option<ConfigOptionStrings>("print_extruder_variant", true)->values = {DD_STANDARD};
    config.option<ConfigOptionFloats>("outer_wall_speed", true)->values = {200.};

    // A single column is not a flow-only layout: head 2 misses as it always did.
    CHECK(config.get_index_for_extruder(2, "print_extruder_id", etDirectDrive, nvtStandard, "print_extruder_variant") == -1);

    std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
    const int extruder_count = 4;
    const int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

    const std::vector<int> variant_index = config.update_values_to_printer_extruders(config, extruder_count, count, nozzle_volume_types,
        print_options_with_variant, "print_extruder_id", "print_extruder_variant");

    // The columns were synthesized as ids {1,1,2,2,3,3,4,4}: every head finds its own (variant, id)
    // column. The flow-only rule would have answered 0 or 1.
    REQUIRE(variant_index == std::vector<int>({0, 3, 5, 6}));
    REQUIRE(config.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>({1, 2, 3, 4}));
    REQUIRE(config.option<ConfigOptionStrings>("print_extruder_variant")->values ==
            std::vector<std::string>({DD_STANDARD, DD_HIGH_FLOW, DD_HIGH_FLOW, DD_STANDARD}));
    // The width-1 value is what every head prints with, High Flow or not.
    REQUIRE(config.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>({200., 200., 200., 200.}));
}

TEST_CASE("A per-extruder variant layout resolves exactly as before the flow-only rule", "[Config][HighFlow][hf_bbl_layout_unchanged]")
{
    DynamicPrintConfig config;
    config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive, etDirectDrive};
    config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nvtStandard, nvtHighFlow};
    config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = std::vector<std::string>(2, DD_STANDARD + "," + DD_HIGH_FLOW);
    add_print_variant_columns(config); // ids {1, 1, 2, 2}

    CHECK(config.get_index_for_extruder(1, "print_extruder_id", etDirectDrive, nvtStandard, "print_extruder_variant") == 0);
    CHECK(config.get_index_for_extruder(1, "print_extruder_id", etDirectDrive, nvtHighFlow, "print_extruder_variant") == 1);
    CHECK(config.get_index_for_extruder(2, "print_extruder_id", etDirectDrive, nvtStandard, "print_extruder_variant") == 2);
    CHECK(config.get_index_for_extruder(2, "print_extruder_id", etDirectDrive, nvtHighFlow, "print_extruder_variant") == 3);
    // Hybrid is matched as Standard.
    CHECK(config.get_index_for_extruder(2, "print_extruder_id", etDirectDrive, nvtHybrid, "print_extruder_variant") == 2);
    // Misses stay misses: the ids are not uniform, so no column of another extruder is borrowed.
    CHECK(config.get_index_for_extruder(3, "print_extruder_id", etDirectDrive, nvtHighFlow, "print_extruder_variant") == -1);
    CHECK(config.get_index_for_extruder(2, "print_extruder_id", etBowden, nvtStandard, "print_extruder_variant") == -1);
    CHECK(config.get_index_for_extruder(2, "print_extruder_id", etDirectDrive, nvtTPUHighFlow, "print_extruder_variant") == -1);

    SECTION("a table narrowed from missed lookups keeps missing") {
        // Without extruder_variant_list, heads 2-4 miss and latch ids {1,1,1,1}; that is no flow-only
        // layout, and Print::apply relies on the -1 to key such a head by its extruder index.
        DynamicPrintConfig latched;
        latched.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive, etDirectDrive, etDirectDrive, etDirectDrive};
        latched.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nvtStandard, nvtStandard, nvtStandard, nvtStandard};
        latched.option<ConfigOptionInts>("print_extruder_id", true)->values = {1};
        latched.option<ConfigOptionStrings>("print_extruder_variant", true)->values = {DD_STANDARD};
        latched.option<ConfigOptionFloats>("outer_wall_speed", true)->values = {30.};

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        const int count = latched.get_extruder_nozzle_volume_count(4, nozzle_volume_types);
        const std::vector<int> variant_index = latched.update_values_to_printer_extruders(latched, 4, count, nozzle_volume_types,
            print_options_with_variant, "print_extruder_id", "print_extruder_variant");
        REQUIRE(variant_index == std::vector<int>({0, 0, 0, 0}));
        // Missed slots keep the id the narrowing gave them.
        REQUIRE(latched.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>({1, 1, 1, 1}));
        REQUIRE(latched.option<ConfigOptionStrings>("print_extruder_variant")->values == std::vector<std::string>(4, DD_STANDARD));
        CHECK(latched.get_index_for_extruder(1, "print_extruder_id", etDirectDrive, nvtStandard, "print_extruder_variant") == 0);
        CHECK(latched.get_index_for_extruder(3, "print_extruder_id", etDirectDrive, nvtStandard, "print_extruder_variant") == -1);
    }

    SECTION("uniform ids outside the process scope keep missing") {
        // The rule covers process presets only: a single-extruder printer table and a filament
        // table with two columns for one id answer a foreign id with -1.
        DynamicPrintConfig other;
        other.option<ConfigOptionInts>("printer_extruder_id", true)->values = {1, 1};
        other.option<ConfigOptionStrings>("printer_extruder_variant", true)->values = {DD_STANDARD, DD_HIGH_FLOW};
        other.option<ConfigOptionInts>("filament_self_index", true)->values = {1, 1};
        other.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {DD_STANDARD, DD_HIGH_FLOW};
        CHECK(other.get_index_for_extruder(1, "printer_extruder_id", etDirectDrive, nvtHighFlow, "printer_extruder_variant") == 1);
        CHECK(other.get_index_for_extruder(2, "printer_extruder_id", etDirectDrive, nvtHighFlow, "printer_extruder_variant") == -1);
        CHECK(other.get_index_for_extruder(1, "filament_self_index", etDirectDrive, nvtHighFlow, "filament_extruder_variant") == 1);
        CHECK(other.get_index_for_extruder(2, "filament_self_index", etDirectDrive, nvtHighFlow, "filament_extruder_variant") == -1);
    }

    SECTION("narrowing yields the same indices, values and ids") {
        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        const int extruder_count = 2;
        const int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);
        const std::vector<int> variant_index = config.update_values_to_printer_extruders(config, extruder_count, count, nozzle_volume_types,
            print_options_with_variant, "print_extruder_id", "print_extruder_variant");
        REQUIRE(variant_index == std::vector<int>({0, 3}));
        REQUIRE(config.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>({30., 500.}));
        REQUIRE(config.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>({1, 2}));
        REQUIRE(config.option<ConfigOptionStrings>("print_extruder_variant")->values == std::vector<std::string>({DD_STANDARD, DD_HIGH_FLOW}));
    }
}

TEST_CASE("A filament with a single column falls back to it on a High Flow head", "[Config][HighFlow]")
{
    // Two filaments on heads 1 and 2; head 2 carries a High Flow nozzle. Filament 1 has both columns,
    // filament 2 (a preset without High Flow values) has one.
    DynamicPrintConfig config;
    config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive, etDirectDrive};
    config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nvtHighFlow, nvtHighFlow};
    config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2};
    config.option<ConfigOptionInts>("filament_self_index", true)->values = {1, 1, 2};
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD};
    config.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = {22., 40., 12.};

    std::set<std::string> keys = filament_options_with_variant;
    keys.insert("filament_self_index");
    config.update_values_to_printer_extruders_for_multiple_filaments(config, 2, 2, keys, "filament_self_index", "filament_extruder_variant");

    // The fallback index (the log level of the miss is not observable here).
    REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>({40., 12.}));
}

namespace {

std::set<std::string> no_keys;

// System parent of a four-head printer that declares Standard and High Flow for every head.
DynamicPrintConfig make_eight_column_printer_parent()
{
    DynamicPrintConfig parent;
    parent.option<ConfigOptionInts>("printer_extruder_id", true)->values = {1, 1, 2, 2, 3, 3, 4, 4};
    parent.option<ConfigOptionStrings>("printer_extruder_variant", true)->values = {DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD, DD_HIGH_FLOW,
                                                                                    DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD, DD_HIGH_FLOW};
    parent.option<ConfigOptionFloats>("retraction_length", true)->values = std::vector<double>(8, 0.8);
    parent.option<ConfigOptionFloats>("machine_max_speed_z", true)->values = {20., 12., 20., 12., 20., 12., 20., 12.,
                                                                              20., 12., 20., 12., 20., 12., 20., 12.};
    return parent;
}

} // namespace

TEST_CASE("A one-column-per-extruder user printer preset fills both flow columns of its extruder", "[Config][HighFlow]")
{
    DynamicPrintConfig parent = make_eight_column_printer_parent();
    DynamicPrintConfig child;
    child.option<ConfigOptionInts>("printer_extruder_id", true)->values = {1, 2, 3, 4};
    child.option<ConfigOptionStrings>("printer_extruder_variant", true)->values = std::vector<std::string>(4, DD_STANDARD);
    child.option<ConfigOptionFloats>("retraction_length", true)->values = {1., 2., 3., 4.};
    child.option<ConfigOptionFloats>("machine_max_speed_z", true)->values = {21., 11., 22., 12., 23., 13., 24., 14.};

    parent.update_diff_values_to_child_config(child, "printer_extruder_id", "printer_extruder_variant",
                                              printer_options_with_variant_1, printer_options_with_variant_2);

    // The user's per-head value holds whatever nozzle the head carries.
    REQUIRE(parent.option<ConfigOptionFloats>("retraction_length")->values == std::vector<double>({1., 1., 2., 2., 3., 3., 4., 4.}));
    REQUIRE(parent.option<ConfigOptionFloats>("machine_max_speed_z")->values ==
            std::vector<double>({21., 11., 21., 11., 22., 12., 22., 12., 23., 13., 23., 13., 24., 14., 24., 14.}));
    // The column layout is the parent's.
    REQUIRE(parent.option<ConfigOptionInts>("printer_extruder_id")->values == std::vector<int>({1, 1, 2, 2, 3, 3, 4, 4}));
}

TEST_CASE("A user printer preset saved with every column is matched column by column", "[Config][HighFlow]")
{
    DynamicPrintConfig parent;
    parent.option<ConfigOptionInts>("printer_extruder_id", true)->values = {1, 1, 2, 2};
    parent.option<ConfigOptionStrings>("printer_extruder_variant", true)->values = {DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD, DD_HIGH_FLOW};
    parent.option<ConfigOptionFloats>("retraction_length", true)->values = {0.8, 0.8, 0.8, 0.8};

    DynamicPrintConfig child;
    child.option<ConfigOptionInts>("printer_extruder_id", true)->values = {1, 1, 2, 2};
    child.option<ConfigOptionStrings>("printer_extruder_variant", true)->values = {DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD, DD_HIGH_FLOW};
    child.option<ConfigOptionFloats>("retraction_length", true)->values = {1., 1.5, 2., 2.5};

    parent.update_diff_values_to_child_config(child, "printer_extruder_id", "printer_extruder_variant", printer_options_with_variant_1, no_keys);
    REQUIRE(parent.option<ConfigOptionFloats>("retraction_length")->values == std::vector<double>({1., 1.5, 2., 2.5}));
}

TEST_CASE("A user printer preset with two columns for an extruder leaves unmatched parent columns alone", "[Config][HighFlow]")
{
    const std::string DD_TPU_HIGH_FLOW = "Direct Drive TPU High Flow";
    DynamicPrintConfig parent;
    parent.option<ConfigOptionInts>("printer_extruder_id", true)->values = {1, 1, 1, 2, 2, 2};
    parent.option<ConfigOptionStrings>("printer_extruder_variant", true)->values = {DD_STANDARD, DD_HIGH_FLOW, DD_TPU_HIGH_FLOW,
                                                                                    DD_STANDARD, DD_HIGH_FLOW, DD_TPU_HIGH_FLOW};
    parent.option<ConfigOptionFloats>("retraction_length", true)->values = {0.8, 0.8, 0.6, 0.8, 0.8, 0.6};

    DynamicPrintConfig child;
    child.option<ConfigOptionInts>("printer_extruder_id", true)->values = {1, 1, 2, 2};
    child.option<ConfigOptionStrings>("printer_extruder_variant", true)->values = {DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD, DD_HIGH_FLOW};
    child.option<ConfigOptionFloats>("retraction_length", true)->values = {1., 1.5, 2., 2.5};

    parent.update_diff_values_to_child_config(child, "printer_extruder_id", "printer_extruder_variant", printer_options_with_variant_1, no_keys);
    // Which of the two child columns a third flow type should follow is not decidable: system value kept.
    REQUIRE(parent.option<ConfigOptionFloats>("retraction_length")->values == std::vector<double>({1., 1.5, 0.6, 2., 2.5, 0.6}));
}

TEST_CASE("A user printer preset with a variant its parent does not declare fills no other column", "[Config][HighFlow]")
{
    DynamicPrintConfig parent = make_eight_column_printer_parent();
    DynamicPrintConfig child;
    child.option<ConfigOptionInts>("printer_extruder_id", true)->values = {1, 2, 3, 4};
    child.option<ConfigOptionStrings>("printer_extruder_variant", true)->values = {DD_STANDARD, "Bowden Standard", DD_STANDARD, DD_STANDARD};
    child.option<ConfigOptionFloats>("retraction_length", true)->values = {1., 2., 3., 4.};

    parent.update_diff_values_to_child_config(child, "printer_extruder_id", "printer_extruder_variant", printer_options_with_variant_1, no_keys);
    // Exact (variant, id) matches only.
    REQUIRE(parent.option<ConfigOptionFloats>("retraction_length")->values == std::vector<double>({1., 0.8, 0.8, 0.8, 3., 0.8, 4., 0.8}));
}

TEST_CASE("The first variant column of a filament is found through the self index, whatever the layout", "[Config][HighFlow][hf_first_filament_variant_column]")
{
    SECTION("before narrowing every filament owns a Standard and a High Flow column") {
        const std::vector<int> self_index = {1, 1, 2, 2, 3, 3, 4, 4};
        for (size_t filament = 0; filament < 4; ++filament) {
            CAPTURE(filament);
            // Not the filament id: that position belongs to another filament or to the other flow type.
            CHECK(first_filament_variant_column(self_index, filament) == 2 * filament);
        }
    }

    SECTION("after narrowing to one column per filament the column is the filament id") {
        const std::vector<int> self_index = {1, 2, 3, 4};
        for (size_t filament = 0; filament < 4; ++filament) {
            CAPTURE(filament);
            CHECK(first_filament_variant_column(self_index, filament) == filament);
        }
    }

    SECTION("a filament that prints through several nozzles owns several slots, the first one counts") {
        // Filament 2 owns the slots 1 and 2, which shifts filament 3 off its id.
        const std::vector<int> self_index = {1, 2, 2, 3};
        CHECK(first_filament_variant_column(self_index, 0) == 0);
        CHECK(first_filament_variant_column(self_index, 1) == 1);
        CHECK(first_filament_variant_column(self_index, 2) == 3);
    }

    SECTION("the columns need not be sorted by filament") {
        const std::vector<int> self_index = {3, 1, 2, 1};
        CHECK(first_filament_variant_column(self_index, 0) == 1);
        CHECK(first_filament_variant_column(self_index, 1) == 2);
        CHECK(first_filament_variant_column(self_index, 2) == 0);
    }

    SECTION("a filament the index does not name keeps its id, as does every filament without an index") {
        CHECK(first_filament_variant_column({1, 1, 2, 2}, 3) == 3);
        CHECK(first_filament_variant_column({}, 0) == 0);
        CHECK(first_filament_variant_column({}, 2) == 2);
    }
}

// A printer preset that declares Standard only (the 0.6 mm U1 preset) with a High Flow tool head of
// another size: the head reads its own Standard column, which holds the values of its size.
TEST_CASE("A High Flow extruder of a printer table without High Flow columns reads its own Standard column", "[Config][HighFlow][hf_offsize_printer_column]")
{
    DynamicPrintConfig config;
    config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values      = std::vector<int>(4, int(etDirectDrive));
    config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {int(nvtStandard), int(nvtHighFlow), int(nvtStandard), int(nvtStandard)};
    config.option<ConfigOptionStrings>("extruder_variant_list", true)->values   = std::vector<std::string>(4, DD_STANDARD);
    config.option<ConfigOptionInts>("printer_extruder_id", true)->values        = {1, 2, 3, 4};
    config.option<ConfigOptionStrings>("printer_extruder_variant", true)->values = std::vector<std::string>(4, DD_STANDARD);
    // Tool head 2 carries the retraction of the 0.4 mm preset (1.5), the others the 0.6 mm one (1.4).
    config.option<ConfigOptionFloats>("retraction_length", true)->values = {1.4, 1.5, 1.4, 1.4};
    config.option<ConfigOptionFloats>("machine_max_speed_x", true)->values = {100., 50., 110., 55., 120., 60., 130., 65.};

    SECTION("the lookup falls back to the Standard column of the same extruder") {
        CHECK(config.get_index_for_extruder(2, "printer_extruder_id", etDirectDrive, nvtHighFlow, "printer_extruder_variant") == 1);
        CHECK(config.get_index_for_extruder(2, "printer_extruder_id", etDirectDrive, nvtHighFlow, "printer_extruder_variant", 2) == 2);
        CHECK(config.get_index_for_extruder(4, "printer_extruder_id", etDirectDrive, nvtStandard, "printer_extruder_variant") == 3);
        // An extruder the table does not name still misses.
        CHECK(config.get_index_for_extruder(5, "printer_extruder_id", etDirectDrive, nvtHighFlow, "printer_extruder_variant") == -1);
    }

    SECTION("a declared High Flow column is read as before") {
        DynamicPrintConfig declared = config;
        declared.option<ConfigOptionInts>("printer_extruder_id")->values        = {1, 2, 2, 3, 4};
        declared.option<ConfigOptionStrings>("printer_extruder_variant")->values = {DD_STANDARD, DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD, DD_STANDARD};
        CHECK(declared.get_index_for_extruder(2, "printer_extruder_id", etDirectDrive, nvtHighFlow, "printer_extruder_variant") == 2);
        CHECK(declared.get_index_for_extruder(2, "printer_extruder_id", etDirectDrive, nvtStandard, "printer_extruder_variant") == 1);
    }

    SECTION("filament and process tables keep missing") {
        DynamicPrintConfig tables;
        tables.option<ConfigOptionInts>("filament_self_index", true)->values         = {1, 2};
        tables.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {DD_STANDARD, DD_STANDARD};
        tables.option<ConfigOptionInts>("print_extruder_id", true)->values            = {1, 2};
        tables.option<ConfigOptionStrings>("print_extruder_variant", true)->values     = {DD_STANDARD, DD_STANDARD};
        CHECK(tables.get_index_for_extruder(2, "filament_self_index", etDirectDrive, nvtHighFlow, "filament_extruder_variant") == -1);
        CHECK(tables.get_index_for_extruder(2, "print_extruder_id", etDirectDrive, nvtHighFlow, "print_extruder_variant") == -1);
    }

    SECTION("narrowing keeps the values of the head and names its slot by the flow it prints") {
        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        const int count = config.get_extruder_nozzle_volume_count(4, nozzle_volume_types);
        // In the order of Print::apply: the stride-2 keys first.
        std::vector<int> index_2 = config.update_values_to_printer_extruders(config, 4, count, nozzle_volume_types, printer_options_with_variant_2,
                                                                             "printer_extruder_id", "printer_extruder_variant", 2);
        CHECK(index_2 == std::vector<int>({0, 1, 2, 3}));
        CHECK(config.option<ConfigOptionFloats>("machine_max_speed_x")->values == std::vector<double>({100., 50., 110., 55., 120., 60., 130., 65.}));
        std::vector<int> index_1 = config.update_values_to_printer_extruders(config, 4, count, nozzle_volume_types, printer_options_with_variant_1,
                                                                             "printer_extruder_id", "printer_extruder_variant");
        CHECK(index_1 == std::vector<int>({0, 1, 2, 3}));
        CHECK(config.option<ConfigOptionFloats>("retraction_length")->values == std::vector<double>({1.4, 1.5, 1.4, 1.4}));
        CHECK(config.option<ConfigOptionInts>("printer_extruder_id")->values == std::vector<int>({1, 2, 3, 4}));
        CHECK(config.option<ConfigOptionStrings>("printer_extruder_variant")->values ==
              std::vector<std::string>({DD_STANDARD, DD_HIGH_FLOW, DD_STANDARD, DD_STANDARD}));
    }
}

// A per-variant filament option read with a single value gives it to every filament variant. A project
// exported by an older CLI holds a single value for an option no loaded preset defined, such as
// filament_ironing_flow.
TEST_CASE("A per-variant filament option read with a single value gives it to every filament variant", "[Config]")
{
    // filament 1 defines Standard and High Flow, filament 2 Standard
    DynamicPrintConfig config;
    config.option<ConfigOptionInts>("filament_self_index", true)->values = {1, 1, 2};
    config.load_from_ini_string("pressure_advance = 0.021", ForwardCompatibilitySubstitutionRule::Disable);
    REQUIRE(config.option<ConfigOptionFloats>("pressure_advance")->values == std::vector<double>({0.021, 0.021, 0.021}));
}
