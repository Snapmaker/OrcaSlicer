#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// Snapmaker Orca: the High Flow filament check against the allow list that ships with the
// Snapmaker profiles, and the rules that decide which tool heads may run a High Flow nozzle.
#include "slic3r/GUI/HighFlowCompat.hpp"
#include "slic3r/GUI/HighFlowNotices.hpp"

#include "libslic3r/AllowlistManager.hpp"
#include "libslic3r/FilamentFlowColumns.hpp"
#include "libslic3r/NozzleFilamentPresets.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "snapmaker_high_flow_fixture.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <set>

using namespace Slic3r;
using namespace Slic3r::GUI;
using HighFlowCompat::CompatibilityLevel;

namespace {

// AllowlistManager reads its file once per process (data directory copy, else resources), so this
// points both at known places first. Every case reaching HighFlowCompat::check calls it: in random
// test order, a read with unset directories would leave empty lists for all later cases.
void load_shipped_allow_list()
{
    static bool loaded = false;
    if (!loaded) {
        loaded = true;

        const std::string previous_resources = resources_dir();
        const std::string previous_data      = data_dir();
        const boost::filesystem::path empty_data =
            boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("high-flow-notices-%%%%-%%%%");
        boost::filesystem::create_directories(empty_data);
        set_resources_dir(boost::filesystem::path(PROFILES_DIR).parent_path().string());
        set_data_dir(empty_data.string());

        AllowlistManager::instance();

        set_resources_dir(previous_resources);
        set_data_dir(previous_data);
        boost::filesystem::remove_all(empty_data);
    }

    INFO("the allow list was read before this helper ran: a case that reaches HighFlowCompat::check does not call load_shipped_allow_list() first");
    REQUIRE_FALSE(AllowlistManager::instance().get_list("high_flow", "unavailable_filaments").empty());
}

// A printer shaped like the Snapmaker U1 0.4 preset: four direct drive tool heads that each
// declare a Standard and a High Flow column.
DynamicPrintConfig u1_like_printer(const std::vector<double> &diameters, const std::string &declared = "Direct Drive Standard,Direct Drive High Flow")
{
    DynamicPrintConfig config;
    config.set_key_value("printer_variant", new ConfigOptionString("0.4"));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats(diameters));
    config.set_key_value("extruder_type", new ConfigOptionEnumsGeneric(std::vector<int>(diameters.size(), int(etDirectDrive))));
    if (!declared.empty())
        config.set_key_value("extruder_variant_list", new ConfigOptionStrings(std::vector<std::string>(diameters.size(), declared)));
    return config;
}

// The vendor data as a function: the sizes that offer High Flow, for every tool head.
HighFlowNotices::SizeOffersHighFlow offers(const std::set<double> &sizes)
{
    return [sizes](double nozzle_size, size_t) {
        return std::any_of(sizes.begin(), sizes.end(), [nozzle_size](double size) { return std::abs(size - nozzle_size) < EPSILON; });
    };
}

// The shipped Snapmaker vendor, the 0.4 U1 preset selected.
std::unique_ptr<PresetBundle> load_snapmaker_bundle()
{
    auto bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(PROFILES_DIR, "Snapmaker", PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr, /*allow_cache=*/false);
    REQUIRE(bundle->printers.select_preset_by_name("Snapmaker U1 (0.4 nozzle)", true));
    return bundle;
}

} // namespace

TEST_CASE("The shipped allow list rates filaments for High Flow nozzles", "[HighFlow][AllowList]")
{
    load_shipped_allow_list();

    // The list itself is in place: a missing or unreadable file answers with empty lists and
    // every check below would pass as "compatible".
    const AllowlistManager::StringList unavailable = AllowlistManager::instance().get_list("high_flow", "unavailable_filaments");
    const AllowlistManager::StringList discouraged = AllowlistManager::instance().get_list("high_flow", "not_recommended_filaments");
    REQUIRE(unavailable == AllowlistManager::StringList{ "TPU 85A" });
    REQUIRE(discouraged == AllowlistManager::StringList{ "PLA Wood", "TPU 90A", "PEBA 90A" });

    SECTION("an unavailable filament is unsupported and named without vendor and printer suffix") {
        const auto result = HighFlowCompat::check("TPU", "Snapmaker TPU 85A @U1 0.4 nozzle");
        CHECK(result.level == CompatibilityLevel::Unsupported);
        CHECK(result.material == "TPU 85A");
    }
    SECTION("the listed presets of the Snapmaker U1 are not recommended") {
        for (const auto &[preset, material] : std::vector<std::pair<std::string, std::string>>{
                 { "Snapmaker PLA Wood @U1 0.4 nozzle", "PLA Wood" },
                 { "Snapmaker PEBA 90A @U1 0.4 nozzle", "PEBA 90A" },
                 { "Snapmaker TPU 90A @U1 0.6 nozzle", "TPU 90A" } }) {
            const auto result = HighFlowCompat::check("PLA", preset);
            CHECK(result.level == CompatibilityLevel::NotRecommended);
            CHECK(result.material == material);
        }
    }
    SECTION("a user preset derived from a listed one is matched by its name, whatever the case") {
        CHECK(HighFlowCompat::check("TPU", "my  tpu 85a   soft").level == CompatibilityLevel::Unsupported);
    }
    SECTION("fibre filled filaments are not recommended, by type or by name") {
        CHECK(HighFlowCompat::check("PLA-CF", "Generic PLA-CF @U1 base").level == CompatibilityLevel::NotRecommended);
        CHECK(HighFlowCompat::check("PETG", "Snapmaker PETG-GF").material == "CF/GF filaments");
        // "cf" inside a word is no material token.
        CHECK(HighFlowCompat::check("PLA", "Scarcfree PLA").level == CompatibilityLevel::Compatible);
    }
    SECTION("an ordinary filament is compatible") {
        const auto result = HighFlowCompat::check("PLA", "Snapmaker PLA SnapSpeed @U1 0.4 nozzle");
        CHECK(result.level == CompatibilityLevel::Compatible);
        CHECK(result.material.empty());
    }
}

TEST_CASE("Notices name the High Flow tool heads only", "[HighFlow][Notices]")
{
    load_shipped_allow_list();

    // One filament per tool head, filament i in tool head i.
    const std::vector<HighFlowNotices::HeadFilament> loaded{
        { "PLA", "Snapmaker PLA SnapSpeed @U1 0.4 nozzle", true },  // head 1
        { "TPU", "Snapmaker TPU 85A @U1 0.4 nozzle", true },        // head 2
        { "PLA", "Snapmaker PLA Wood @U1 0.4 nozzle", true },       // head 3
        { "PLA", "Generic PLA @U1 base", false },                   // head 4
    };
    const auto filaments = HighFlowNotices::group_by_head(loaded, HighFlowNotices::filament_heads(loaded.size(), 4, {}, false), 4);
    REQUIRE(filaments.size() == 4);
    for (size_t head = 0; head < 4; ++head) {
        REQUIRE(filaments[head].size() == 1);
        CHECK(filaments[head].front().preset_name == loaded[head].preset_name);
    }

    SECTION("all heads Standard: nothing to say, whatever is loaded") {
        const auto report = HighFlowNotices::evaluate({ 0, 0, 0, 0 }, filaments, false);
        CHECK(report.empty());
        CHECK_FALSE(report.blocks_slicing());
    }
    SECTION("an unavailable filament on a High Flow head blocks slicing; the same filament on a Standard head does not") {
        auto report = HighFlowNotices::evaluate({ 0, 1, 0, 0 }, filaments, true);
        REQUIRE(report.unsupported.size() == 1);
        CHECK(report.unsupported.front().head == 1);
        CHECK(report.unsupported.front().material == "TPU 85A");
        CHECK(report.blocks_slicing());
        CHECK(report.not_recommended.empty());
        CHECK(report.standard_values_used.empty());
        CHECK(report.standard_speeds_used.empty());

        report = HighFlowNotices::evaluate({ 1, 0, 0, 0 }, filaments, true);
        CHECK(report.empty());
    }
    SECTION("not recommended filament, filament without High Flow values, process without High Flow speeds") {
        const auto report = HighFlowNotices::evaluate({ 0, 0, 1, 1 }, filaments, false);
        CHECK_FALSE(report.blocks_slicing());
        REQUIRE(report.not_recommended.size() == 1);
        CHECK(report.not_recommended.front().head == 2);
        CHECK(report.not_recommended.front().material == "PLA Wood");
        REQUIRE(report.standard_values_used.size() == 1);
        CHECK(report.standard_values_used.front().head == 3);
        CHECK(report.standard_values_used.front().material == "Generic PLA @U1 base");
        CHECK(report.standard_speeds_used == std::vector<size_t>{ 2, 3 });
    }
    SECTION("a High Flow head without a filament still reports the process preset") {
        const auto report = HighFlowNotices::evaluate({ 1 }, {}, false);
        CHECK(report.standard_speeds_used == std::vector<size_t>{ 0 });
        CHECK(report.not_recommended.empty());
    }
    SECTION("Hybrid and TPU High Flow heads are none of this check's business") {
        CHECK(HighFlowNotices::evaluate({ int(nvtHybrid), int(nvtTPUHighFlow) }, filaments, false).empty());
    }
    SECTION("the High Flow speeds known per tool head: N4 names the heads without, a head beyond the vector among them") {
        auto report = HighFlowNotices::evaluate({ 1, 1, 0, 1 }, filaments, std::vector<bool>{ false, true, false });
        CHECK(report.standard_speeds_used == std::vector<size_t>{ 0, 3 });
        report = HighFlowNotices::evaluate({ 1, 1, 0, 1 }, filaments, std::vector<bool>{ true, true, true, true });
        CHECK(report.standard_speeds_used.empty());
        // The one-value form is the same for every head.
        report = HighFlowNotices::evaluate({ 1, 1, 0, 1 }, filaments, false);
        CHECK(report.standard_speeds_used == std::vector<size_t>{ 0, 1, 3 });
    }
}

TEST_CASE("A filament is rated for the tool head that prints it", "[HighFlow][Notices]")
{
    load_shipped_allow_list();

    SECTION("without a binding map filament i belongs to tool head i and the rest to the master head, whatever the stored map says") {
        // The stored map of a project that was never sliced: all 1.
        CHECK(HighFlowNotices::filament_heads(6, 4, { 1, 1, 1, 1, 1, 1 }, false) == std::vector<size_t>{ 0, 1, 2, 3, 0, 0 });
        CHECK(HighFlowNotices::filament_heads(6, 4, {}, false, 2) == std::vector<size_t>{ 0, 1, 2, 3, 2, 2 });
        // A master head the printer does not have.
        CHECK(HighFlowNotices::filament_heads(5, 4, {}, false, 7) == std::vector<size_t>{ 0, 1, 2, 3, 0 });
        CHECK(HighFlowNotices::filament_heads(3, 0, { 1, 2, 3 }, true) == std::vector<size_t>{ 0, 0, 0 });
    }
    SECTION("a binding map is followed; entries it lacks or that name no tool head fall back") {
        CHECK(HighFlowNotices::filament_heads(5, 4, { 2, 1, 4, 4, 3 }, true) == std::vector<size_t>{ 1, 0, 3, 3, 2 });
        CHECK(HighFlowNotices::filament_heads(5, 4, { 2, 0, 9 }, true) == std::vector<size_t>{ 1, 1, 2, 3, 0 });
    }

    // Five filament slots on four tool heads, mapped by hand: the TPU 85A of slot 2 is printed by
    // tool head 1, the PLA Wood of slot 1 and the Generic PLA of slot 5 by tool head 2.
    const std::vector<HighFlowNotices::HeadFilament> loaded{
        { "PLA", "Snapmaker PLA Wood @U1 0.4 nozzle", true },
        { "TPU", "Snapmaker TPU 85A @U1 0.4 nozzle", true },
        { "PLA", "Snapmaker PLA SnapSpeed @U1 0.4 nozzle", true },
        { "PLA", "Snapmaker PLA SnapSpeed @U1 0.4 nozzle", true },
        { "PLA", "Generic PLA @U1 base", false },
    };
    const std::vector<int> manual_map{ 2, 1, 3, 4, 2 };
    const auto filaments = HighFlowNotices::group_by_head(loaded, HighFlowNotices::filament_heads(loaded.size(), 4, manual_map, true), 4);
    REQUIRE(filaments.size() == 4);
    REQUIRE(filaments[1].size() == 2);

    SECTION("tool head 2 on High Flow rates the two filaments mapped to it, not the filament of slot 2") {
        const auto report = HighFlowNotices::evaluate({ 0, 1, 0, 0 }, filaments, true);
        CHECK_FALSE(report.blocks_slicing());
        REQUIRE(report.not_recommended.size() == 1);
        CHECK(report.not_recommended.front().head == 1);
        CHECK(report.not_recommended.front().material == "PLA Wood");
        REQUIRE(report.standard_values_used.size() == 1);
        CHECK(report.standard_values_used.front().head == 1);
        CHECK(report.standard_values_used.front().material == "Generic PLA @U1 base");
    }
    SECTION("tool head 1 on High Flow is blocked by the TPU 85A mapped to it") {
        const auto report = HighFlowNotices::evaluate({ 1, 0, 0, 0 }, filaments, true);
        REQUIRE(report.unsupported.size() == 1);
        CHECK(report.unsupported.front().head == 0);
        CHECK(report.unsupported.front().material == "TPU 85A");
        CHECK(report.not_recommended.empty());
    }
    SECTION("the same map read as not binding gives the answer of the slots") {
        const auto by_slot = HighFlowNotices::group_by_head(loaded, HighFlowNotices::filament_heads(loaded.size(), 4, manual_map, false), 4);
        const auto report  = HighFlowNotices::evaluate({ 0, 1, 0, 0 }, by_slot, true);
        REQUIRE(report.unsupported.size() == 1);
        CHECK(report.unsupported.front().head == 1);
    }
    SECTION("a preset is named once per tool head") {
        const std::vector<HighFlowNotices::HeadFilament> twice{
            { "PLA", "Generic PLA @U1 base", false }, { "PLA", "Generic PLA @U1 base", false }, { "PLA-CF", "Generic PLA-CF @U1 base", false },
            { "PLA-CF", "Generic PLA-CF @U1 base", false } };
        const auto all_on_one = HighFlowNotices::group_by_head(twice, HighFlowNotices::filament_heads(twice.size(), 2, { 1, 1, 1, 1 }, true), 2);
        const auto report     = HighFlowNotices::evaluate({ 1, 1 }, all_on_one, true);
        CHECK(report.not_recommended.size() == 1);
        CHECK(report.standard_values_used.size() == 2);
    }
    SECTION("a filament of a tool head the printer does not have is dropped") {
        const auto grouped = HighFlowNotices::group_by_head(loaded, { 0, 9, 1, 1, 1 }, 2);
        REQUIRE(grouped.size() == 2);
        CHECK(grouped[0].size() == 1);
        CHECK(grouped[1].size() == 3);
    }
}

TEST_CASE("A tool head may run High Flow with a nozzle size the vendor data has High Flow values for", "[HighFlow][Notices]")
{
    SECTION("declared and of the preset's size, or of a size the vendor data offers") {
        const DynamicPrintConfig printer = u1_like_printer({ 0.4, 0.4, 0.6, 0.4 });
        CHECK(HighFlowNotices::declared_volume_types(printer, 0) == std::vector<int>{ int(nvtStandard), int(nvtHighFlow) });
        CHECK(HighFlowNotices::head_declares_high_flow(printer, 2));
        CHECK(HighFlowNotices::head_can_use_high_flow(printer, 0));
        // Without the function only the preset's own size may run High Flow.
        CHECK_FALSE(HighFlowNotices::head_can_use_high_flow(printer, 2));
        CHECK(HighFlowNotices::head_can_use_high_flow(printer, 2, offers({ 0.4, 0.6, 0.8 })));
        CHECK_FALSE(HighFlowNotices::head_can_use_high_flow(printer, 2, offers({ 0.4 })));
        // The preset's own size never asks the function.
        CHECK(HighFlowNotices::head_can_use_high_flow(printer, 0, offers({})));
    }
    SECTION("a printer that declares no variants offers Standard only") {
        const DynamicPrintConfig printer = u1_like_printer({ 0.4, 0.4 }, "");
        CHECK(HighFlowNotices::declared_volume_types(printer, 1) == std::vector<int>{ int(nvtStandard) });
        CHECK_FALSE(HighFlowNotices::head_declares_high_flow(printer, 1));
        CHECK_FALSE(HighFlowNotices::head_can_use_high_flow(printer, 1));
    }
    SECTION("a printer that declares Standard only") {
        const DynamicPrintConfig printer = u1_like_printer({ 0.4 }, "Direct Drive Standard");
        CHECK_FALSE(HighFlowNotices::head_can_use_high_flow(printer, 0));
    }
    SECTION("the sanitizer resets inadmissible High Flow heads and nothing else") {
        const DynamicPrintConfig printer = u1_like_printer({ 0.4, 0.6, 0.4, 0.8 });
        std::vector<int> types{ int(nvtHighFlow), int(nvtHighFlow), int(nvtStandard), int(nvtHighFlow) };
        std::vector<int> offered = types;
        CHECK(HighFlowNotices::sanitize(printer, offered, offers({ 0.4, 0.6, 0.8 })).empty());
        CHECK(offered == types);
        CHECK(HighFlowNotices::sanitize(printer, types) == std::vector<size_t>{ 1, 3 });
        CHECK(types == std::vector<int>{ int(nvtHighFlow), int(nvtStandard), int(nvtStandard), int(nvtStandard) });
        // A second pass finds nothing.
        CHECK(HighFlowNotices::sanitize(printer, types).empty());

        // A size the vendor data has no values for is reset whatever the function offers elsewhere.
        const DynamicPrintConfig with_0_2 = u1_like_printer({ 0.4, 0.6, 0.2, 0.8 });
        std::vector<int> all_high_flow(4, int(nvtHighFlow));
        CHECK(HighFlowNotices::sanitize(with_0_2, all_high_flow, offers({ 0.4, 0.6, 0.8 })) == std::vector<size_t>{ 2 });
        CHECK(all_high_flow == std::vector<int>{ int(nvtHighFlow), int(nvtHighFlow), int(nvtStandard), int(nvtHighFlow) });

        const DynamicPrintConfig plain = u1_like_printer({ 0.4, 0.4 }, "");
        std::vector<int> other{ int(nvtHighFlow), int(nvtHybrid) };
        CHECK(HighFlowNotices::sanitize(plain, other) == std::vector<size_t>{ 0 });
        CHECK(other == std::vector<int>{ int(nvtStandard), int(nvtHybrid) });
    }
}

TEST_CASE("A preset has a High Flow column when its variant list names one", "[HighFlow][Notices]")
{
    DynamicPrintConfig preset;
    CHECK_FALSE(HighFlowNotices::has_high_flow_column(preset, "filament_extruder_variant"));
    preset.set_key_value("filament_extruder_variant", new ConfigOptionStrings({ "Direct Drive Standard" }));
    CHECK_FALSE(HighFlowNotices::has_high_flow_column(preset, "filament_extruder_variant"));
    preset.set_key_value("filament_extruder_variant", new ConfigOptionStrings({ "Direct Drive Standard", "Direct Drive TPU High Flow" }));
    CHECK_FALSE(HighFlowNotices::has_high_flow_column(preset, "filament_extruder_variant"));
    preset.set_key_value("filament_extruder_variant", new ConfigOptionStrings({ "Direct Drive Standard", "Direct Drive High Flow" }));
    CHECK(HighFlowNotices::has_high_flow_column(preset, "filament_extruder_variant"));
}

namespace {

DynamicPrintConfig process_columns(const std::vector<int> &ids, const std::vector<std::string> &variants)
{
    DynamicPrintConfig config;
    config.set_key_value("print_extruder_id", new ConfigOptionInts(ids));
    config.set_key_value("print_extruder_variant", new ConfigOptionStrings(variants));
    return config;
}

} // namespace

TEST_CASE("The flow selector offers the columns of a flow-only process preset", "[HighFlow][FlowSelector]")
{
    const DynamicPrintConfig printer   = u1_like_printer({ 0.4, 0.4, 0.4, 0.4 });
    const DynamicPrintConfig flow_only = process_columns({ 1, 1 }, { "Direct Drive Standard", "Direct Drive High Flow" });

    SECTION("one entry per column, in column order") {
        CHECK(HighFlowNotices::flow_selector_types(printer, flow_only) == std::vector<int>{ int(nvtStandard), int(nvtHighFlow) });
        CHECK(HighFlowNotices::flow_selector_types(printer, process_columns({ 1, 1 }, { "Direct Drive High Flow", "Direct Drive Standard" })) ==
              std::vector<int>{ int(nvtHighFlow), int(nvtStandard) });
    }

    SECTION("the entry selects the column the engine reads for a tool head of that flow type") {
        const std::vector<int> types = HighFlowNotices::flow_selector_types(printer, flow_only);
        REQUIRE(types.size() == 2);
        for (size_t selection = 0; selection < types.size(); ++selection)
            for (int head = 1; head <= 4; ++head)
                CHECK(flow_only.get_index_for_extruder(head, "print_extruder_id", etDirectDrive, NozzleVolumeType(types[selection]), "print_extruder_variant") == int(selection));
    }

    SECTION("no selector for a preset with a single column") {
        CHECK(HighFlowNotices::flow_selector_types(printer, process_columns({ 1 }, { "Direct Drive Standard" })).empty());
    }

    SECTION("no selector for a preset with one column per tool head") {
        CHECK(HighFlowNotices::flow_selector_types(printer, process_columns({ 1, 1, 2, 2, 3, 3, 4, 4 },
            { "Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Standard", "Direct Drive High Flow",
              "Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Standard", "Direct Drive High Flow" })).empty());
        CHECK(HighFlowNotices::flow_selector_types(printer, process_columns({ 1, 2 }, { "Direct Drive Standard", "Direct Drive High Flow" })).empty());
    }

    SECTION("no selector on a printer of two extruders, which has the extruder switch") {
        CHECK(HighFlowNotices::flow_selector_types(u1_like_printer({ 0.4, 0.4 }), flow_only).empty());
    }

    SECTION("no selector on a printer that declares Standard only") {
        CHECK(HighFlowNotices::flow_selector_types(u1_like_printer({ 0.4, 0.4, 0.4, 0.4 }, "Direct Drive Standard"), flow_only).empty());
        CHECK(HighFlowNotices::flow_selector_types(u1_like_printer({ 0.4, 0.4, 0.4, 0.4 }, ""), flow_only).empty());
    }

    SECTION("no selector for columns that do not belong to the tool heads") {
        CHECK(HighFlowNotices::flow_selector_types(printer, process_columns({ 1, 1 }, { "Bowden Standard", "Bowden High Flow" })).empty());
        CHECK(HighFlowNotices::flow_selector_types(printer, process_columns({ 1, 1 }, { "Direct Drive Standard", "Direct Drive Standard" })).empty());
        CHECK(HighFlowNotices::flow_selector_types(printer, process_columns({ 1 }, { "Direct Drive Standard", "Direct Drive High Flow" })).empty());
        CHECK(HighFlowNotices::flow_selector_types(printer, DynamicPrintConfig()).empty());
    }
}

TEST_CASE("Variant columns drop the drive name when all of them share it", "[HighFlow][FlowSelector]")
{
    using HighFlowNotices::VariantName;
    using HighFlowNotices::variant_column_label;

    SECTION("the two columns of a Snapmaker filament") {
        const std::vector<std::string> columns{ "Direct Drive Standard", "Direct Drive High Flow" };
        const VariantName standard  = variant_column_label(columns, 0);
        const VariantName high_flow = variant_column_label(columns, 1);
        CHECK(standard.drive.empty());
        CHECK(standard.volume_type == "Standard");
        CHECK(high_flow.drive.empty());
        CHECK(high_flow.volume_type == "High Flow");
    }

    SECTION("the longest volume type wins the split") {
        const VariantName tpu = HighFlowNotices::split_variant_name("Direct Drive TPU High Flow");
        CHECK(tpu.drive == "Direct Drive");
        CHECK(tpu.volume_type == "TPU High Flow");
    }

    SECTION("columns of different drives keep both parts") {
        const std::vector<std::string> columns{ "Direct Drive Standard", "Bowden Standard" };
        CHECK(variant_column_label(columns, 0).drive == "Direct Drive");
        CHECK(variant_column_label(columns, 1).drive == "Bowden");
        CHECK(variant_column_label(columns, 1).volume_type == "Standard");
    }

    SECTION("a single column keeps its full name and a missing column has none") {
        CHECK(variant_column_label({ "Direct Drive Standard" }, 0).drive == "Direct Drive");
        CHECK(variant_column_label({ "Direct Drive Standard" }, 1).drive.empty());
        CHECK(variant_column_label({ "Direct Drive Standard" }, 1).volume_type.empty());
    }

    SECTION("a name without a volume type is all drive") {
        CHECK(HighFlowNotices::split_variant_name("Custom").drive == "Custom");
        CHECK(HighFlowNotices::split_variant_name("Custom").volume_type.empty());
        CHECK(variant_column_label({ "Custom", "Custom" }, 1).drive == "Custom");
    }
}

TEST_CASE("Only ids that differ name tool heads in the list of changed settings", "[HighFlow][FlowSelector]")
{
    CHECK_FALSE(HighFlowNotices::ids_name_tool_heads({}));
    CHECK_FALSE(HighFlowNotices::ids_name_tool_heads({ 1 }));
    CHECK_FALSE(HighFlowNotices::ids_name_tool_heads({ 1, 1 }));
    CHECK(HighFlowNotices::ids_name_tool_heads({ 1, 1, 2, 2 }));
    CHECK(HighFlowNotices::ids_name_tool_heads({ 1, 2, 3, 4 }));
}

TEST_CASE("The flow selector opens on the entry of a flow type", "[HighFlow][FlowSelector]")
{
    // Tab::update_extruder_variants: the entry of the flow type of the tool head whose nozzle tab
    // the sidebar shows; the first entry when the preset has no column of that type.
    const std::vector<int> standard_first{ int(nvtStandard), int(nvtHighFlow) };
    CHECK(HighFlowNotices::flow_selector_index(standard_first, int(nvtStandard)) == 0);
    CHECK(HighFlowNotices::flow_selector_index(standard_first, int(nvtHighFlow)) == 1);
    CHECK(HighFlowNotices::flow_selector_index({ int(nvtHighFlow), int(nvtStandard) }, int(nvtHighFlow)) == 0);
    CHECK(HighFlowNotices::flow_selector_index(standard_first, int(nvtHybrid)) == 0);
    CHECK(HighFlowNotices::flow_selector_index({}, int(nvtHighFlow)) == 0);
}

TEST_CASE("The Material settings open on the column of the tool head's flow type", "[HighFlow][FlowSelector]")
{
    // Tab::select_flow_column on the filament tab: the column of "filament_extruder_variant"
    // whose name ends in the flow type; -1 when the preset has no such column.
    const std::vector<std::string> snapmaker{ "Direct Drive Standard", "Direct Drive High Flow" };
    CHECK(HighFlowNotices::variant_column_for_type(snapmaker, int(nvtStandard)) == 0);
    CHECK(HighFlowNotices::variant_column_for_type(snapmaker, int(nvtHighFlow)) == 1);
    CHECK(HighFlowNotices::variant_column_for_type({ "Direct Drive High Flow", "Direct Drive Standard" }, int(nvtHighFlow)) == 0);
    CHECK(HighFlowNotices::variant_column_for_type({ "Direct Drive Standard" }, int(nvtHighFlow)) == -1);
    // "TPU High Flow" is a type of its own, not a High Flow column.
    CHECK(HighFlowNotices::variant_column_for_type({ "Direct Drive Standard", "Direct Drive TPU High Flow" }, int(nvtHighFlow)) == -1);
    CHECK(HighFlowNotices::variant_column_for_type({}, int(nvtStandard)) == -1);
}

TEST_CASE("A Flow row is hidden, a choice, or ruled out by the nozzle size", "[HighFlow][FlowRow]")
{
    using HighFlowNotices::FlowRowState;

    SECTION("one declared type leaves nothing to choose") {
        const DynamicPrintConfig plain = u1_like_printer({ 0.4, 0.4 }, "");
        CHECK(HighFlowNotices::flow_row_state(plain, 0) == FlowRowState::Hidden);
        CHECK(HighFlowNotices::flow_choice_usable(plain, 0));

        const DynamicPrintConfig standard_only = u1_like_printer({ 0.4 }, "Direct Drive Standard");
        CHECK(HighFlowNotices::flow_row_state(standard_only, 0) == FlowRowState::Hidden);
        CHECK(HighFlowNotices::flow_choice_usable(standard_only, 0));
    }
    SECTION("a High Flow only column of another nozzle size is hidden and stays unusable") {
        // The row state is a view: the combo of the Printer tab is still disabled for such a head,
        // unless the vendor data has High Flow values for that size.
        const DynamicPrintConfig printer = u1_like_printer({ 0.6 }, "Direct Drive High Flow");
        CHECK(HighFlowNotices::flow_row_state(printer, 0) == FlowRowState::Hidden);
        CHECK_FALSE(HighFlowNotices::flow_choice_usable(printer, 0));
        CHECK(HighFlowNotices::flow_choice_usable(printer, 0, offers({ 0.4, 0.6, 0.8 })));
    }
    SECTION("the nozzle size and the vendor data decide between a choice and a ruled out row") {
        DynamicPrintConfig printer = u1_like_printer({ 0.4, 0.4, 0.6, 0.2 });
        CHECK(HighFlowNotices::flow_row_state(printer, 0) == FlowRowState::Choice);
        CHECK(HighFlowNotices::flow_row_state(printer, 2) == FlowRowState::RuledOut);
        CHECK(HighFlowNotices::flow_choice_usable(printer, 0));
        CHECK_FALSE(HighFlowNotices::flow_choice_usable(printer, 2));
        CHECK(HighFlowNotices::flow_row_state(printer, 2, offers({ 0.4, 0.6, 0.8 })) == FlowRowState::Choice);
        CHECK(HighFlowNotices::flow_choice_usable(printer, 2, offers({ 0.4, 0.6, 0.8 })));
        CHECK(HighFlowNotices::flow_row_state(printer, 3, offers({ 0.4, 0.6, 0.8 })) == FlowRowState::RuledOut);

        // A preset that does not name its nozzle size rules nothing out, with or without the function.
        printer.erase("printer_variant");
        CHECK(HighFlowNotices::flow_row_state(printer, 2) == FlowRowState::Choice);
        CHECK(HighFlowNotices::flow_choice_usable(printer, 2));
        CHECK(HighFlowNotices::flow_row_state(printer, 3, offers({ 0.4 })) == FlowRowState::Choice);
    }
    SECTION("a tool head beyond the list has no row") {
        const DynamicPrintConfig printer = u1_like_printer({ 0.4, 0.4, 0.6, 0.4 });
        CHECK(HighFlowNotices::flow_row_state(printer, 7) == FlowRowState::Hidden);
    }
    SECTION("a Standard-only head is ruled out when another size of the model offers High Flow") {
        // The function answers the "any size" question (a size of 0) for tool heads 1 and 2 only.
        const HighFlowNotices::SizeOffersHighFlow any_size = [](double nozzle_size, size_t head) { return nozzle_size <= 0. && head < 2; };
        const DynamicPrintConfig standard_only = u1_like_printer({ 0.2, 0.2, 0.2 }, "Direct Drive Standard");
        CHECK(HighFlowNotices::flow_row_state(standard_only, 0, any_size) == FlowRowState::RuledOut);
        CHECK(HighFlowNotices::flow_row_state(standard_only, 1, any_size) == FlowRowState::RuledOut);
        CHECK(HighFlowNotices::flow_row_state(standard_only, 2, any_size) == FlowRowState::Hidden);
        CHECK(HighFlowNotices::flow_row_state(standard_only, 7, any_size) == FlowRowState::Hidden);
        // The shown type stays Standard, and a function that knows no size at all hides the row.
        CHECK(HighFlowNotices::shown_volume_type(standard_only, 0, int(nvtHighFlow), any_size) == int(nvtStandard));
        CHECK(HighFlowNotices::flow_row_state(standard_only, 0, offers({ 0.4 })) == FlowRowState::Hidden);
    }
}

TEST_CASE("A tool head of another size than the printer preset offers High Flow when the machine preset of its size declares it", "[HighFlow][FlowRow][hf_offsize_row]")
{
    using HighFlowNotices::FlowRowState;
    // Shaped like the 0.6 mm U1 preset: Standard only; tool heads of 0.6, 0.4, 0.2 and 0.8 mm.
    DynamicPrintConfig printer = u1_like_printer({ 0.6, 0.4, 0.2, 0.8 }, "Direct Drive Standard");
    printer.set_key_value("printer_variant", new ConfigOptionString("0.6"));
    const std::vector<int> standard_only{ int(nvtStandard) };
    const std::vector<int> both{ int(nvtStandard), int(nvtHighFlow) };

    SECTION("the 0.4 mm head offers and may use High Flow, the other sizes are ruled out") {
        const auto vendor = offers({ 0.4 });
        CHECK(HighFlowNotices::offered_volume_types(printer, 1, vendor) == both);
        CHECK(HighFlowNotices::head_offers_high_flow(printer, 1, vendor));
        CHECK(HighFlowNotices::head_can_use_high_flow(printer, 1, vendor));
        CHECK(HighFlowNotices::flow_choice_usable(printer, 1, vendor));
        CHECK(HighFlowNotices::shown_volume_type(printer, 1, int(nvtHighFlow), vendor) == int(nvtHighFlow));
        for (size_t head : { size_t(0), size_t(2), size_t(3) }) {
            INFO("tool head " << head + 1);
            CHECK(HighFlowNotices::offered_volume_types(printer, head, vendor) == standard_only);
            CHECK_FALSE(HighFlowNotices::head_can_use_high_flow(printer, head, vendor));
            CHECK(HighFlowNotices::shown_volume_type(printer, head, int(nvtHighFlow), vendor) == int(nvtStandard));
        }
        // The row: the "any size" question of a Standard-only head is answered for the 0.4 mm size.
        const HighFlowNotices::SizeOffersHighFlow with_any = [](double nozzle_size, size_t) {
            return nozzle_size <= 0. || std::abs(nozzle_size - 0.4) < EPSILON;
        };
        CHECK(HighFlowNotices::flow_row_state(printer, 1, with_any) == FlowRowState::Choice);
        CHECK(HighFlowNotices::flow_row_state(printer, 0, with_any) == FlowRowState::RuledOut);
        CHECK(HighFlowNotices::flow_row_state(printer, 2, with_any) == FlowRowState::RuledOut);
        CHECK(HighFlowNotices::flow_row_state(printer, 3, with_any) == FlowRowState::RuledOut);
    }
    SECTION("the sanitizer keeps the 0.4 mm head on High Flow and resets the others") {
        std::vector<int> types(4, int(nvtHighFlow));
        CHECK(HighFlowNotices::sanitize(printer, types, offers({ 0.4 })) == std::vector<size_t>{ 0, 2, 3 });
        CHECK(types == std::vector<int>{ int(nvtStandard), int(nvtHighFlow), int(nvtStandard), int(nvtStandard) });
    }
    SECTION("without the vendor data only the preset's declared columns count") {
        CHECK(HighFlowNotices::offered_volume_types(printer, 1) == standard_only);
        CHECK_FALSE(HighFlowNotices::head_can_use_high_flow(printer, 1));
        CHECK(HighFlowNotices::flow_row_state(printer, 1) == FlowRowState::Hidden);
        CHECK(HighFlowNotices::flow_row_state(printer, 1, offers({ 0.6 })) == FlowRowState::Hidden);
    }
    SECTION("the preset's own size asks its declared columns, not the function") {
        // A 0.6 mm head on this preset stays Standard even where the function would answer for 0.6 mm.
        CHECK(HighFlowNotices::offered_volume_types(printer, 0, offers({ 0.4, 0.6 })) == standard_only);
        CHECK_FALSE(HighFlowNotices::head_can_use_high_flow(printer, 0, offers({ 0.4, 0.6 })));
    }
}

TEST_CASE("A ruled out tool head shows the first declared type", "[HighFlow][FlowRow]")
{
    const DynamicPrintConfig printer = u1_like_printer({ 0.4, 0.6 });

    CHECK(HighFlowNotices::shown_volume_type(printer, 0, int(nvtHighFlow)) == int(nvtHighFlow));
    CHECK(HighFlowNotices::shown_volume_type(printer, 1, int(nvtHighFlow)) == int(nvtStandard));
    CHECK(HighFlowNotices::shown_volume_type(printer, 1, int(nvtHighFlow), offers({ 0.4, 0.6, 0.8 })) == int(nvtHighFlow));

    CHECK(HighFlowNotices::shown_volume_type(printer, 0, int(nvtStandard)) == int(nvtStandard));
    CHECK(HighFlowNotices::shown_volume_type(printer, 1, int(nvtStandard)) == int(nvtStandard));

    // A stored type the head does not declare.
    CHECK(HighFlowNotices::shown_volume_type(printer, 0, int(nvtHybrid)) == int(nvtStandard));
    CHECK(HighFlowNotices::shown_volume_type(printer, 1, int(nvtHybrid)) == int(nvtStandard));

    // A High Flow only column: the first declared type is High Flow.
    const DynamicPrintConfig high_flow_only = u1_like_printer({ 0.4 }, "Direct Drive High Flow");
    CHECK(HighFlowNotices::shown_volume_type(high_flow_only, 0, int(nvtStandard)) == int(nvtHighFlow));
}

TEST_CASE("The reason names the nozzle size of the tool head", "[HighFlow][FlowRow]")
{
    DynamicPrintConfig printer = u1_like_printer({ 0.4, 0.6, 0.25 });
    CHECK(HighFlowNotices::head_nozzle_size_label(printer, 1) == "0.6");
    CHECK(HighFlowNotices::head_nozzle_size_label(printer, 0) == "0.4");
    CHECK(HighFlowNotices::head_nozzle_size_label(printer, 2) == "0.25");
    // A tool head beyond the list, and a preset without diameters.
    CHECK(HighFlowNotices::head_nozzle_size_label(printer, 3).empty());
    printer.erase("nozzle_diameter");
    CHECK(HighFlowNotices::head_nozzle_size_label(printer, 0).empty());
}

TEST_CASE("The vendor data says which nozzle sizes offer High Flow", "[HighFlow][Notices][Profiles]")
{
    const auto loaded = load_snapmaker_bundle();
    const HighFlowNotices::SizeOffersHighFlow shipped = HighFlowNotices::size_offers_high_flow(*loaded);

    SECTION("the shipped vendor: the U1 0.4 mm machine preset declares High Flow for every tool head, the other sizes do not") {
        // Snapmaker ships High Flow values for the 0.4 mm nozzle only; the 0.2 / 0.6 / 0.8 mm
        // presets keep their Standard values until Snapmaker ships theirs.
        for (size_t head = 0; head < 4; ++head) {
            CHECK(shipped(0.4, head));
            CHECK_FALSE(shipped(0.2, head));
            CHECK_FALSE(shipped(0.6, head));
            CHECK_FALSE(shipped(0.8, head));
        }
        // A size without a machine preset, and a printer without a model.
        CHECK_FALSE(shipped(0.5, 0));
        loaded->printers.get_edited_preset().config.set_key_value("printer_model", new ConfigOptionString(""));
        CHECK_FALSE(shipped(0.4, 0));
    }
    SECTION("a vendor update that declares High Flow on the 0.6 mm preset offers the size, one that withdraws it rules the size out again") {
        const DynamicPrintConfig printer = u1_like_printer({ 0.4, 0.6 });
        std::vector<int>         types{ int(nvtHighFlow), int(nvtHighFlow) };
        CHECK(HighFlowNotices::flow_row_state(printer, 1, shipped) == HighFlowNotices::FlowRowState::RuledOut);

        // The 0.6 mm machine preset in the layout of the 0.4 mm one (the fixture).
        Preset &machine = Slic3r::Test::u1_0_6_declares_high_flow(*loaded);
        for (size_t head = 0; head < 4; ++head)
            CHECK(shipped(0.6, head));
        CHECK_FALSE(shipped(0.8, 0));
        CHECK(HighFlowNotices::flow_row_state(printer, 1, shipped) == HighFlowNotices::FlowRowState::Choice);
        CHECK(HighFlowNotices::sanitize(printer, types, shipped).empty());
        CHECK(types == std::vector<int>{ int(nvtHighFlow), int(nvtHighFlow) });

        // The shape of Snapmaker's 0.6 mm machine preset: no variant list.
        machine.config.erase("extruder_variant_list");
        CHECK_FALSE(shipped(0.6, 0));
        CHECK(HighFlowNotices::flow_row_state(printer, 1, shipped) == HighFlowNotices::FlowRowState::RuledOut);
        CHECK(HighFlowNotices::sanitize(printer, types, shipped) == std::vector<size_t>{ 1 });
        CHECK(types == std::vector<int>{ int(nvtHighFlow), int(nvtStandard) });
    }
}

TEST_CASE("Notices are grouped by the nozzle size of their tool heads", "[HighFlow][Notices]")
{
    const DynamicPrintConfig printer = u1_like_printer({ 0.4, 0.6, 0.6 });
    const std::vector<HighFlowNotices::Report::Entry> entries{ { 0, "PLA Wood" }, { 1, "TPU 90A" }, { 2, "PLA Wood" } };
    const auto groups = HighFlowNotices::group_by_nozzle_size(entries, printer);
    REQUIRE(groups.size() == 2);
    REQUIRE(groups.count("0.4") == 1);
    REQUIRE(groups.count("0.6") == 1);
    CHECK(groups.at("0.4").size() == 1);
    CHECK(groups.at("0.6").size() == 2);
    CHECK(groups.at("0.6").front().material == "TPU 90A");

    // A tool head without a diameter falls back to the preset's own size.
    DynamicPrintConfig without = printer;
    without.erase("nozzle_diameter");
    const auto by_preset = HighFlowNotices::group_by_nozzle_size(entries, without);
    REQUIRE(by_preset.size() == 1);
    CHECK(by_preset.count("0.4") == 1);
}

namespace {

DynamicPrintConfig two_column_filament(const std::vector<double> &max_flow, const std::vector<int> &temperature)
{
    DynamicPrintConfig config;
    config.set_key_value("filament_extruder_variant", new ConfigOptionStrings({ "Direct Drive Standard", "Direct Drive High Flow" }));
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats(max_flow));
    config.set_key_value("nozzle_temperature", new ConfigOptionInts(temperature));
    return config;
}

} // namespace

TEST_CASE("A Standard value changed without its High Flow value is named", "[HighFlow][Notices]")
{
    const std::set<std::string>    keys{ "filament_max_volumetric_speed", "nozzle_temperature", "filament_retraction_length" };
    const DynamicPrintConfig       parent = two_column_filament({ 20., 40. }, { 215, 220 });

    SECTION("the Standard column differs, the High Flow column keeps the parent's value") {
        CHECK(HighFlowNotices::standard_only_edits(two_column_filament({ 15., 40. }, { 215, 220 }), parent, keys, "filament_extruder_variant") ==
              std::vector<std::string>{ "filament_max_volumetric_speed" });
        // Both columns changed: the High Flow column was edited too.
        CHECK(HighFlowNotices::standard_only_edits(two_column_filament({ 15., 15. }, { 215, 220 }), parent, keys, "filament_extruder_variant").empty());
        // Only the High Flow column changed.
        CHECK(HighFlowNotices::standard_only_edits(two_column_filament({ 20., 35. }, { 215, 220 }), parent, keys, "filament_extruder_variant").empty());
        // Two keys, in the order of the set.
        CHECK(HighFlowNotices::standard_only_edits(two_column_filament({ 15., 40. }, { 210, 220 }), parent, keys, "filament_extruder_variant") ==
              std::vector<std::string>{ "filament_max_volumetric_speed", "nozzle_temperature" });
    }
    SECTION("a preset with one column, or a parent without a High Flow column, names nothing") {
        DynamicPrintConfig one_column;
        one_column.set_key_value("filament_extruder_variant", new ConfigOptionStrings({ "Direct Drive Standard" }));
        one_column.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats({ 15. }));
        CHECK(HighFlowNotices::standard_only_edits(one_column, parent, keys, "filament_extruder_variant").empty());
        CHECK(HighFlowNotices::standard_only_edits(two_column_filament({ 15., 40. }, { 215, 220 }), one_column, keys, "filament_extruder_variant").empty());
    }
    SECTION("a key one side lacks, and a nil value, are compared as they are") {
        DynamicPrintConfig child = two_column_filament({ 15., 40. }, { 215, 220 });
        child.set_key_value("filament_retraction_length", new ConfigOptionFloatsNullable({ 0.8, 1.0 }));
        // The parent has no retraction: skipped.
        CHECK(HighFlowNotices::standard_only_edits(child, parent, keys, "filament_extruder_variant") ==
              std::vector<std::string>{ "filament_max_volumetric_speed" });
        DynamicPrintConfig with_retraction = parent;
        auto *retraction = new ConfigOptionFloatsNullable({ 0.8, 1.0 });
        retraction->values[0] = ConfigOptionFloatsNullable::nil_value();
        with_retraction.set_key_value("filament_retraction_length", retraction);
        CHECK(HighFlowNotices::standard_only_edits(child, with_retraction, keys, "filament_extruder_variant") ==
              std::vector<std::string>{ "filament_max_volumetric_speed", "filament_retraction_length" });
    }
}

TEST_CASE("A user preset that lowers a Standard value keeps the High Flow value of its parent and is named on a High Flow tool head", "[HighFlow][Notices][Profiles]")
{
    // A user preset of the shipped SnapSpeed (0.4 mm, 20 / 40 mm3/s) saving "Max volumetric speed" 15
    // in the Standard column only. The loader keeps the parent's 40 mm3/s in the High Flow column,
    // an untested value, which N7 tells.
    load_shipped_allow_list(); // evaluate() rates the filament on the High Flow tool head
    const auto    loaded = load_snapmaker_bundle();
    PresetBundle &bundle = *loaded;
    const char   *parent_name = "Snapmaker PLA SnapSpeed @U1";

    const boost::filesystem::path user_dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("high-flow-user-%%%%-%%%%");
    boost::filesystem::create_directories(user_dir / "filament");
    {
        std::ofstream file((user_dir / "filament" / "My SnapSpeed.json").string());
        REQUIRE(file.good());
        file << "{\n"
                "    \"type\": \"filament\",\n"
                "    \"name\": \"My SnapSpeed\",\n"
                "    \"from\": \"User\",\n"
                "    \"inherits\": \"" << parent_name << "\",\n"
                "    \"version\": \"2.5.0\",\n"
                "    \"filament_settings_id\": [\"My SnapSpeed\"],\n"
                "    \"filament_max_volumetric_speed\": [\"15\"]\n"
                "}\n";
    }
    PresetsConfigSubstitutions substitutions;
    bundle.filaments.load_presets(user_dir.string(), "filament", substitutions, ForwardCompatibilitySubstitutionRule::Enable, nullptr, PresetOrigin(), true);
    boost::filesystem::remove_all(user_dir);

    const Preset *preset = bundle.filaments.find_preset("My SnapSpeed", false);
    REQUIRE(preset != nullptr);
    const Preset *parent = NozzleFilament::system_ancestor(bundle.filaments, *preset);
    REQUIRE(parent != nullptr);
    CHECK(parent->name == parent_name);
    const auto *speed = preset->config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    REQUIRE(speed != nullptr);
    REQUIRE(speed->values.size() == 2);
    CHECK_THAT(speed->values[0], Catch::Matchers::WithinAbs(15., 1e-9));
    CHECK_THAT(speed->values[1], Catch::Matchers::WithinAbs(40., 1e-9));

    const std::vector<std::string> keys = HighFlowNotices::standard_only_edits(preset->config, parent->config, filament_options_with_variant, "filament_extruder_variant");
    CHECK(keys == std::vector<std::string>{"filament_max_volumetric_speed"});
    // The parent against itself names nothing.
    CHECK(HighFlowNotices::standard_only_edits(parent->config, parent->config, filament_options_with_variant, "filament_extruder_variant").empty());

    // On a High Flow tool head the report names the preset and the parent; on a Standard head nothing.
    HighFlowNotices::HeadFilament filament;
    filament.filament_type        = "PLA";
    filament.preset_name          = preset->name;
    filament.has_high_flow_column = HighFlowNotices::has_high_flow_column(preset->config, "filament_extruder_variant");
    filament.standard_only_keys   = keys;
    filament.parent_name          = parent->name;
    REQUIRE(filament.has_high_flow_column);
    const auto filaments = HighFlowNotices::group_by_head({ filament }, { 1 }, 2);
    auto       report    = HighFlowNotices::evaluate({ int(nvtStandard), int(nvtHighFlow) }, filaments, true);
    REQUIRE(report.standard_only_edited.size() == 1);
    CHECK(report.standard_only_edited.front().head == 1);
    CHECK(report.standard_only_edited.front().material == "My SnapSpeed");
    CHECK(report.standard_only_edited.front().parent == parent_name);
    CHECK(report.standard_values_used.empty());
    CHECK_FALSE(report.empty());
    report = HighFlowNotices::evaluate({ int(nvtHighFlow), int(nvtStandard) }, filaments, true);
    CHECK(report.empty());
    // The process preset: N7 per High Flow tool head, only with a High Flow column.
    report = HighFlowNotices::evaluate({ int(nvtHighFlow), int(nvtHighFlow) }, {}, true, true);
    CHECK(report.process_standard_only == std::vector<size_t>{ 0, 1 });
    CHECK(report.standard_speeds_used.empty());
    report = HighFlowNotices::evaluate({ int(nvtHighFlow) }, {}, false, true);
    CHECK(report.process_standard_only.empty());
    CHECK(report.standard_speeds_used == std::vector<size_t>{ 0 });
}

// A U1 tool head whose preset declares Standard only (0.2 / 0.6 / 0.8 mm) still shows its Flow row,
// disabled at "Standard" with the reason line of its size, as the model offers High Flow on 0.4 mm.
TEST_CASE("The speed selector shortens its labels before it breaks the row", "[HighFlow][SpeedSelector][hs_selector_fit]")
{
    // "All extruders", four "Extruder k · s" entries; the short set "All", "Ek · s".
    const std::vector<int> long_widths{ 120, 90, 90, 90, 90 };
    const std::vector<int> short_widths{ 44, 62, 62, 62, 62 };
    CHECK(HighFlowNotices::head_selector_fit(long_widths, short_widths, 480) == HighFlowNotices::SelectorFit::Long);
    CHECK(HighFlowNotices::head_selector_fit(long_widths, short_widths, 900) == HighFlowNotices::SelectorFit::Long);
    CHECK(HighFlowNotices::head_selector_fit(long_widths, short_widths, 479) == HighFlowNotices::SelectorFit::Short);
    CHECK(HighFlowNotices::head_selector_fit(long_widths, short_widths, 292) == HighFlowNotices::SelectorFit::Short);
    CHECK(HighFlowNotices::head_selector_fit(long_widths, short_widths, 291) == HighFlowNotices::SelectorFit::ShortRows);
    CHECK(HighFlowNotices::head_selector_fit(long_widths, short_widths, 100) == HighFlowNotices::SelectorFit::ShortRows);
    // An unknown width changes nothing; a row without entries fits anywhere.
    CHECK(HighFlowNotices::head_selector_fit(long_widths, short_widths, 0) == HighFlowNotices::SelectorFit::Long);
    CHECK(HighFlowNotices::head_selector_fit(long_widths, short_widths, -5) == HighFlowNotices::SelectorFit::Long);
    CHECK(HighFlowNotices::head_selector_fit({}, {}, 1) == HighFlowNotices::SelectorFit::Long);
}

TEST_CASE("Every tool head of a U1 shows its Flow row", "[FirstRun][HighFlow][FlowRow][fr1_flow_row]")
{
    const auto loaded = load_snapmaker_bundle();
    const HighFlowNotices::SizeOffersHighFlow shipped = HighFlowNotices::size_offers_high_flow(*loaded);

    for (const char *name : { "Snapmaker U1 (0.2 nozzle)", "Snapmaker U1 (0.6 nozzle)", "Snapmaker U1 (0.8 nozzle)" }) {
        REQUIRE(loaded->printers.select_preset_by_name(name, true));
        const DynamicPrintConfig &printer = loaded->printers.get_edited_preset().config;
        REQUIRE(printer.option<ConfigOptionFloats>("nozzle_diameter")->size() == 4);
        for (size_t head = 0; head < 4; ++head) {
            INFO(name << ", tool head " << head + 1);
            CHECK(HighFlowNotices::flow_row_state(printer, head, shipped) == HighFlowNotices::FlowRowState::RuledOut);
            CHECK_FALSE(HighFlowNotices::head_nozzle_size_label(printer, head).empty());
            CHECK(HighFlowNotices::shown_volume_type(printer, head, int(nvtStandard), shipped) == int(nvtStandard));
        }
    }

    REQUIRE(loaded->printers.select_preset_by_name("Snapmaker U1 (0.4 nozzle)", true));
    const DynamicPrintConfig &printer = loaded->printers.get_edited_preset().config;
    for (size_t head = 0; head < 4; ++head) {
        INFO("Snapmaker U1 (0.4 nozzle), tool head " << head + 1);
        CHECK(HighFlowNotices::flow_row_state(printer, head, shipped) == HighFlowNotices::FlowRowState::Choice);
    }
}

TEST_CASE("A 0.4 mm tool head offers High Flow on the 0.2, 0.6 and 0.8 mm U1 presets", "[HighFlow][FlowRow][Profiles][hf_offsize_row]")
{
    const auto loaded = load_snapmaker_bundle();
    const HighFlowNotices::SizeOffersHighFlow shipped = HighFlowNotices::size_offers_high_flow(*loaded);
    const std::vector<int> both{ int(nvtStandard), int(nvtHighFlow) };

    for (const auto &[name, size] : std::vector<std::pair<std::string, double>>{
             { "Snapmaker U1 (0.2 nozzle)", 0.2 }, { "Snapmaker U1 (0.6 nozzle)", 0.6 }, { "Snapmaker U1 (0.8 nozzle)", 0.8 } }) {
        REQUIRE(loaded->printers.select_preset_by_name(name, true));
        DynamicPrintConfig &printer = loaded->printers.get_edited_preset().config;
        // As the sidebar sets it: tool head 2 at 0.4 mm, tool head 4 at a third size without High Flow.
        const double third = size < 0.3 ? 0.6 : 0.2;
        printer.set_key_value("nozzle_diameter", new ConfigOptionFloats({ size, 0.4, size, third }));
        INFO(name);
        CHECK(HighFlowNotices::offered_volume_types(printer, 1, shipped) == both);
        CHECK(HighFlowNotices::head_can_use_high_flow(printer, 1, shipped));
        CHECK(HighFlowNotices::flow_row_state(printer, 1, shipped) == HighFlowNotices::FlowRowState::Choice);
        CHECK(HighFlowNotices::shown_volume_type(printer, 1, int(nvtHighFlow), shipped) == int(nvtHighFlow));
        for (size_t head : { size_t(0), size_t(2), size_t(3) }) {
            INFO("tool head " << head + 1);
            CHECK(HighFlowNotices::flow_row_state(printer, head, shipped) == HighFlowNotices::FlowRowState::RuledOut);
            CHECK_FALSE(HighFlowNotices::head_can_use_high_flow(printer, head, shipped));
        }
        std::vector<int> types(4, int(nvtHighFlow));
        CHECK(HighFlowNotices::sanitize(printer, types, shipped) == std::vector<size_t>{ 0, 2, 3 });
        CHECK(types == std::vector<int>{ int(nvtStandard), int(nvtHighFlow), int(nvtStandard), int(nvtStandard) });
    }

    // The 0.4 mm preset: every 0.4 mm head a choice, as before.
    REQUIRE(loaded->printers.select_preset_by_name("Snapmaker U1 (0.4 nozzle)", true));
    const DynamicPrintConfig &printer = loaded->printers.get_edited_preset().config;
    for (size_t head = 0; head < 4; ++head) {
        CHECK(HighFlowNotices::offered_volume_types(printer, head, shipped) == both);
        CHECK(HighFlowNotices::head_can_use_high_flow(printer, head, shipped));
    }
}

// The speed picker: the sentence that says why the quality rule gave a tool head its preset, per
// step of PerHeadProcess::source_for_head.
TEST_CASE("The automatic reason names the step of the quality rule and the layer height it matched", "[HighFlow][SpeedPicker][phs_picker]")
{
    using PerHeadProcess::Step;
    const wxString same = HighFlowNotices::automatic_reason(Step::SameQuality, "Standard", "Standard", "0.6", 0.2, false);
    CHECK(same.Contains("Standard"));
    CHECK(same.Contains("0.20"));
    CHECK(same.Contains("plate"));
    CHECK_FALSE(same.Contains("preferred"));
    const wxString preferred = HighFlowNotices::automatic_reason(Step::SameQuality, "Standard", "Standard", "0.6", 0.3, true);
    CHECK(preferred.Contains("preferred"));
    CHECK(preferred.Contains("0.30"));
    const wxString ladder = HighFlowNotices::automatic_reason(Step::ClassLadder, "High Quality", "Standard", "0.8", 0.2, false);
    CHECK(ladder.Contains("High Quality"));
    CHECK(ladder.Contains("0.8"));
    CHECK(ladder.Contains("Standard"));
    // A classless plate names the class the ladder landed on and no missing class.
    const wxString classless = HighFlowNotices::automatic_reason(Step::ClassLadder, "", "Standard", "0.8", 0.2, false);
    CHECK(classless.Contains("Standard"));
    CHECK_FALSE(classless.Contains("no "));
    CHECK(HighFlowNotices::automatic_reason(Step::SameQuality, "", "", "0.8", 0.2, false).Contains("nearest layer height"));
    CHECK(HighFlowNotices::automatic_reason(Step::SizeDefault, "Standard", "", "0.5", 0.2, false).Contains("default"));
    CHECK(HighFlowNotices::automatic_reason(Step::FirstByName, "Standard", "", "0.5", 0.2, false).Contains("first"));
    // Not derived by the rule: nothing to say.
    CHECK(HighFlowNotices::automatic_reason(Step::SelectedPreset, "Standard", "", "0.4", 0.2, false).IsEmpty());
    CHECK(HighFlowNotices::automatic_reason(Step::Chosen, "Standard", "", "0.4", 0.2, false).IsEmpty());
}

// The flow toggle of the Speed page (PerHeadProcess::flow_key): the texts of a High Flow tool head
// that prints the Standard speeds by choice, on the page, in the entry's tooltip and in the hint.
TEST_CASE("The texts of a High Flow tool head printing the Standard speeds name the choice", "[HighFlow][SpeedSelector][hs_flow_choice_text]")
{
    using HighFlowNotices::SpeedsNote;
    const wxString description = HighFlowNotices::head_flow_description(2, "0.4", nvtHighFlow, true);
    CHECK(description.Contains("Extruder 3"));
    CHECK(description.Contains("0.4 mm"));
    CHECK(description.Contains("High Flow"));
    CHECK(description.Contains("Standard speeds (chosen)"));
    const wxString plain = HighFlowNotices::head_flow_description(2, "0.4", nvtHighFlow, false);
    CHECK(plain.Contains("High Flow"));
    CHECK_FALSE(plain.Contains("chosen"));
    CHECK(HighFlowNotices::head_flow_description(0, "0.2", nvtStandard, false).Contains("Standard."));

    const wxString tooltip = HighFlowNotices::head_entry_tooltip(0, "0.4", nvtHighFlow, true);
    CHECK(tooltip.Contains("Extruder 1 (sidebar: Nozzle 1)"));
    CHECK(tooltip.Contains("High Flow"));
    CHECK(tooltip.Contains("Standard speeds (chosen)"));
    const wxString plain_tooltip = HighFlowNotices::head_entry_tooltip(0, "0.4", nvtStandard, false);
    CHECK(plain_tooltip.Contains("sidebar: Nozzle 1"));
    CHECK(plain_tooltip.Contains("Standard."));
    CHECK_FALSE(plain_tooltip.Contains("chosen"));

    // The hint: the selected preset without a state, an automatic source, the High Flow column, a chosen preset.
    const wxString hint = HighFlowNotices::speeds_hint_label("0.20mm High Quality", wxString(), SpeedsNote::StandardChosen, "60", "100", "4000");
    CHECK(hint.Contains("Preset: 0.20mm High Quality, Standard (chosen for this High Flow nozzle)"));
    CHECK(hint.Contains("speeds: outer wall 60"));
    CHECK_FALSE(hint.Contains("Speeds:"));
    CHECK(hint.Contains("sparse 100"));
    CHECK(hint.Contains("accel 4000"));
    const wxString automatic = HighFlowNotices::speeds_hint_label("0.18mm Standard", "(automatic)", SpeedsNote::StandardChosen, "120", "100", "10000");
    CHECK(automatic.Contains("0.18mm Standard (automatic), Standard (chosen for this High Flow nozzle)"));
    const wxString high_flow = HighFlowNotices::speeds_hint_label("0.20mm Standard", "(automatic)", SpeedsNote::HighFlow, "500", "600", "10000");
    CHECK(high_flow.Contains("Preset: 0.20mm Standard, High Flow (automatic)"));
    CHECK(high_flow.Contains("speeds: outer wall 500"));
    CHECK_FALSE(high_flow.Contains("chosen"));
    const wxString chosen = HighFlowNotices::speeds_hint_label("0.12mm Standard", "(chosen)", SpeedsNote::Plain, "120", "150", "10000");
    CHECK(chosen.Contains("Preset: 0.12mm Standard (chosen)"));
    CHECK(chosen.Contains("accel 10000"));

    CHECK(HighFlowNotices::standard_chosen_tooltip().Contains("High Flow nozzle"));
    CHECK(HighFlowNotices::standard_chosen_tooltip().Contains("Standard speeds"));
}

// The texts of the Quality page under the speed selector.
TEST_CASE("The texts of the Quality page name line widths, the values by kind and what an older version reads", "[HighFlow][SpeedSelector][PerHeadWidth][hs_quality_page_text]")
{
    // The line under All tool heads: one derived head, several, and every head derived.
    const wxString one = HighFlowNotices::widths_description("3 (0.6 mm)", "0.18mm Standard", "1, 2, 4", false);
    CHECK(one.Contains("Extruder 3 (0.6 mm) prints with the line widths of 0.18mm Standard."));
    CHECK(one.Contains("apply to extruders 1, 2, 4, and to every extruder for the widths you changed"));
    CHECK(one.Contains("every other setting on this page applies to every extruder"));
    const wxString several = HighFlowNotices::widths_description("1 (0.2 mm), 3 (0.6 mm), 4 (0.8 mm)", "0.10mm High Quality, 0.18mm Standard, 0.24mm Standard", "2", true);
    CHECK(several.Contains("Extruders 1 (0.2 mm), 3 (0.6 mm), 4 (0.8 mm) print with the line widths of"));
    CHECK(several.Contains("apply to extruders 2, and"));
    const wxString all = HighFlowNotices::widths_description("1 (0.2 mm), 2 (0.6 mm)", "0.10mm High Quality, 0.18mm Standard", "", true);
    CHECK(all.Contains("apply to every extruder for the widths you changed"));
    CHECK_FALSE(all.Contains("extruders ,"));

    // The line under the picker, per page.
    CHECK(HighFlowNotices::picker_note(true).Contains("Its line widths print on this extruder"));
    CHECK(HighFlowNotices::picker_note(true).Contains("(Speed page)"));
    CHECK(HighFlowNotices::picker_note(false).Contains("speeds, accelerations and jerk print on this extruder"));
    CHECK(HighFlowNotices::picker_note(false).Contains("(Quality page)"));

    // Under a head: the preferred layer height, a width changed under All, the counts.
    const wxString height = HighFlowNotices::preferred_height_sentence(0.1, 0);
    CHECK(height.Contains("prints 0.10 mm layers"));
    CHECK(height.Contains("nozzle 1 in the sidebar"));
    const wxString edited = HighFlowNotices::all_edited_width_sentence("Inner wall", "110%", "0.10mm High Quality", "112.5%");
    CHECK(edited == "Inner wall 110% from All extruders (0.10mm High Quality has 112.5%).");
    CHECK(HighFlowNotices::head_values_by_kind(1, 3) == "1 line width, 3 speeds");
    CHECK(HighFlowNotices::head_values_by_kind(2, 0) == "2 line widths");
    CHECK(HighFlowNotices::head_values_by_kind(0, 1) == "1 speed");
    CHECK(HighFlowNotices::head_values_by_kind(0, 0).IsEmpty());
    CHECK(HighFlowNotices::head_values_set_sentence(true, 1) == "1 line width set for this extruder.");
    CHECK(HighFlowNotices::head_values_set_sentence(true, 3) == "3 line widths set for this extruder.");
    CHECK(HighFlowNotices::head_values_set_sentence(false, 1) == "1 speed set for this extruder.");
    CHECK(HighFlowNotices::head_values_set_sentence(false, 2) == "2 speeds set for this extruder.");
    CHECK(HighFlowNotices::head_values_set_sentence(true, 0).IsEmpty());
    CHECK(HighFlowNotices::shared_settings_sentence().Contains("Greyed settings are shared by every extruder"));
    CHECK(HighFlowNotices::shared_settings_sentence().Contains("layer height of an extruder is set in the sidebar"));
    CHECK(HighFlowNotices::clear_head_link_label(true) == "Clear the line widths set for this extruder");
    CHECK(HighFlowNotices::clear_head_link_label(false) == "Clear the speeds set for this extruder");

    // What an older version reads from a width written as a full array, and the guard.
    CHECK(HighFlowNotices::old_reader_width({"0.42", "0.42", "110%", "110%"}) == "0.42 %");
    CHECK(HighFlowNotices::old_reader_width({"105%", "105%", "0.5", "0.5"}) == "105 %");
    CHECK(HighFlowNotices::old_reader_width({"0.42", "0.42", "0.5"}) == "0.42 mm");
    CHECK(HighFlowNotices::old_reader_width({}).empty());
    const wxString guard = HighFlowNotices::mixed_unit_sentence("Default", "0.42 %");
    CHECK(guard.Contains("Older versions of Snapmaker Orca read this preset's Default as 0.42 %"));
    CHECK(guard.Contains("use the same unit for every extruder"));
    CHECK_FALSE(guard.Contains("OrcaSlicer"));

    // A width in a tooltip: a percent against the head's nozzle, an absolute value, zero.
    CHECK(HighFlowNotices::width_value_label(FloatOrPercent(110., true), 0.2) == "110 % (0.22 mm)");
    CHECK(HighFlowNotices::width_value_label(FloatOrPercent(112.5, true), 0.4) == "112.5 % (0.45 mm)");
    CHECK(HighFlowNotices::width_value_label(FloatOrPercent(0.62, false), 0.6) == "0.62 mm");
    CHECK(HighFlowNotices::width_value_label(FloatOrPercent(0., true), 0.4) == "auto");

    // Two presets differ in their widths when a Standard shared column of one of the nine keys differs.
    DynamicPrintConfig a, b;
    for (const char *key : {"line_width", "outer_wall_line_width"}) {
        a.option<ConfigOptionFloatsOrPercentsNullable>(key, true)->values = {FloatOrPercent(105., true)};
        b.option<ConfigOptionFloatsOrPercentsNullable>(key, true)->values = {FloatOrPercent(105., true)};
    }
    CHECK_FALSE(HighFlowNotices::widths_differ(a, b));
    b.option<ConfigOptionFloatsOrPercentsNullable>("outer_wall_line_width", true)->values = {FloatOrPercent(110., true)};
    CHECK(HighFlowNotices::widths_differ(a, b));
    b.option<ConfigOptionFloatsOrPercentsNullable>("outer_wall_line_width", true)->values = {FloatOrPercent(105., true)};
    a.option<ConfigOptionFloatsOrPercentsNullable>("bridge_line_width", true)->values = {FloatOrPercent(100., true)};
    CHECK(HighFlowNotices::widths_differ(a, b)); // a key one of them lacks
}

TEST_CASE("The notice of a line width added to an object names the tool heads that printed another value", "[HighFlow][SpeedSelector][PerHeadWidth][hs_object_width_text]")
{
    CHECK(HighFlowNotices::object_width_notice({}).IsEmpty());
    const wxString two = HighFlowNotices::object_width_notice({0, 3});
    CHECK(two.Contains("Line widths added to an object apply on every extruder that prints it"));
    CHECK(two.Contains("extruders 1 and 4 printed it with the line widths of their own nozzle size"));
    CHECK(HighFlowNotices::object_width_notice({0, 2, 3}).Contains("extruders 1, 3 and 4 printed it"));
    const wxString one = HighFlowNotices::object_width_notice({2});
    CHECK(one.Contains("extruder 3 printed it with the line widths of its own nozzle size"));
    CHECK_FALSE(one.Contains("extruders"));
}

TEST_CASE("A filament given High Flow values is not reported as printing Standard values", "[HighFlow][Notices][FilamentFlow]")
{
    load_shipped_allow_list();
    // Generic PETG with the High Flow column the Filament tab adds, not yet saved: the report is
    // built from the edited config.
    DynamicPrintConfig edited;
    edited.set_key_value("filament_extruder_variant", new ConfigOptionStrings({ "Direct Drive Standard" }));
    edited.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats({ 12. }));
    CHECK_FALSE(HighFlowNotices::has_high_flow_column(edited, "filament_extruder_variant"));
    REQUIRE(filament_add_flow_column(edited, nvtHighFlow));
    CHECK(HighFlowNotices::has_high_flow_column(edited, "filament_extruder_variant"));

    auto report_for = [](bool has_column) {
        const std::vector<HighFlowNotices::HeadFilament> loaded{
            { "PLA", "Snapmaker PLA SnapSpeed @U1", true },
            { "PETG", "Generic PETG", has_column },
        };
        const auto filaments = HighFlowNotices::group_by_head(loaded, HighFlowNotices::filament_heads(loaded.size(), 4, {}, false), 4);
        return HighFlowNotices::evaluate({ 0, 1, 0, 0 }, filaments, true);
    };
    const auto without = report_for(false);
    REQUIRE(without.standard_values_used.size() == 1);
    CHECK(without.standard_values_used.front().head == 1);
    CHECK(report_for(HighFlowNotices::has_high_flow_column(edited, "filament_extruder_variant")).standard_values_used.empty());
}

TEST_CASE("High Flow values added over a parent without them are named when a Standard value leaves them behind", "[HighFlow][Notices][FilamentFlow]")
{
    const std::set<std::string> keys{ "filament_max_volumetric_speed", "nozzle_temperature" };
    DynamicPrintConfig parent;
    parent.set_key_value("filament_extruder_variant", new ConfigOptionStrings({ "Direct Drive Standard" }));
    parent.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats({ 12. }));
    parent.set_key_value("nozzle_temperature", new ConfigOptionInts({ 255 }));
    DynamicPrintConfig child = parent;
    REQUIRE(filament_add_flow_column(child, nvtHighFlow));
    // A High Flow speed of its own, and a Standard temperature changed after the copy: the High Flow
    // temperature still holds the copied 255.
    child.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = { 10., 22. };
    child.option<ConfigOptionInts>("nozzle_temperature")->values = { 250, 255 };
    // The notice compares with the parent widened by a copy of its Standard column.
    DynamicPrintConfig storage;
    CHECK(HighFlowNotices::standard_only_edits(child, filament_reference_in_layout_of(child, parent, storage), keys, "filament_extruder_variant") ==
          std::vector<std::string>{ "nozzle_temperature" });
    // The parent as it is has no High Flow column to compare with.
    CHECK(HighFlowNotices::standard_only_edits(child, parent, keys, "filament_extruder_variant").empty());
}

TEST_CASE("A user preset made from a filament High Flow nozzles cannot print is rated by its system preset", "[HighFlow][Notices][FilamentFlow]")
{
    load_shipped_allow_list();
    // "Flex 85" alone names no listed material.
    CHECK(HighFlowCompat::check("TPU", "Flex 85").level == CompatibilityLevel::Compatible);
    const auto result = HighFlowCompat::check("TPU", "Flex 85", "Snapmaker TPU 85A @U1 0.4 nozzle");
    CHECK(result.level == CompatibilityLevel::Unsupported);
    CHECK(result.material == "TPU 85A");
    // Without another ancestor, or with a compatible one, the preset's own rating stands.
    CHECK(HighFlowCompat::check("PLA", "Snapmaker PLA Wood @U1 0.4 nozzle", "").level == CompatibilityLevel::NotRecommended);
    CHECK(HighFlowCompat::check("PLA", "Snapmaker PLA Wood @U1 0.4 nozzle", "Generic PLA").level == CompatibilityLevel::NotRecommended);
    CHECK(HighFlowCompat::check("PETG", "My PETG", "Generic PETG").level == CompatibilityLevel::Compatible);
}
