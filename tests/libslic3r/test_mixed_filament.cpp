#include <catch2/catch.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

using namespace Slic3r;

namespace {

static std::vector<std::string> split_rows(const std::string &serialized)
{
    std::vector<std::string> rows;
    std::stringstream ss(serialized);
    std::string row;
    while (std::getline(ss, row, ';')) {
        if (!row.empty())
            rows.push_back(row);
    }
    return rows;
}

static std::string join_rows(const std::vector<std::string> &rows)
{
    std::ostringstream ss;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i != 0)
            ss << ';';
        ss << rows[i];
    }
    return ss.str();
}

static unsigned int virtual_id_for_stable_id(const std::vector<MixedFilament> &mixed, size_t num_physical, uint64_t stable_id)
{
    unsigned int next_virtual_id = unsigned(num_physical + 1);
    for (const MixedFilament &mf : mixed) {
        if (!mf.enabled || mf.deleted)
            continue;
        if (mf.stable_id == stable_id)
            return next_virtual_id;
        ++next_virtual_id;
    }
    return 0;
}

static std::string single_custom_mixed_definition(unsigned int component_a, unsigned int component_b, uint64_t stable_id)
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    MixedFilamentManager           mgr;
    mgr.add_custom_filament(component_a, component_b, 50, colors);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.stable_id         = stable_id;
    row.distribution_mode = int(MixedFilament::Simple);
    row.manual_pattern.clear();

    return mgr.serialize_custom_entries();
}

static DynamicPrintConfig mixed_region_print_config(const std::string &mixed_definitions)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    // Print::apply uses filament_diameter.size() as the physical filament count.
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.76, 1.77, 1.78};
    config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    config.set("mixed_filament_definitions", mixed_definitions);
    return config;
}

static std::vector<unsigned int> first_layer_range_painted_extruders(const Print &print)
{
    std::vector<unsigned int> extruders;
    if (print.objects().empty())
        return extruders;

    const PrintObjectRegions *regions = print.objects().front()->shared_regions();
    if (regions == nullptr || regions->layer_ranges.empty())
        return extruders;

    for (const PrintObjectRegions::PaintedRegion &painted_region : regions->layer_ranges.front().painted_regions)
        extruders.emplace_back(painted_region.extruder_id);

    std::sort(extruders.begin(), extruders.end());
    extruders.erase(std::unique(extruders.begin(), extruders.end()), extruders.end());
    return extruders;
}

static const PrintObjectRegions *first_print_object_regions(const Print &print)
{
    if (print.objects().empty())
        return nullptr;
    return print.objects().front()->shared_regions();
}

struct MixedAutoGenerateGuard
{
    explicit MixedAutoGenerateGuard(bool enabled)
        : previous(MixedFilamentManager::auto_generate_enabled())
    {
        MixedFilamentManager::set_auto_generate_enabled(enabled);
    }

    ~MixedAutoGenerateGuard()
    {
        MixedFilamentManager::set_auto_generate_enabled(previous);
    }

    bool previous = true;
};

// Enable auto_generate by default for all tests (matches production behavior where
// AppConfig sets this to true during GUI startup).
static const int _auto_generate_enabler = []() {
    MixedFilamentManager::set_auto_generate_enabled(true);
    return 0;
}();

} // namespace

TEST_CASE("Mixed filament remap follows stable row ids when same-pair rows reorder", "[MixedFilament]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"Default Filament", "Default Filament"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#0000FF"};
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    auto &mixed = mgr.mixed_filaments();
    REQUIRE(mixed.size() == 1);

    mixed[0].deleted = true;
    mixed[0].enabled = false;

    const auto colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    mgr.add_custom_filament(1, 2, 25, colors);
    mgr.add_custom_filament(1, 2, 75, colors);

    auto &old_mixed = mgr.mixed_filaments();
    REQUIRE(old_mixed.size() == 3);
    REQUIRE(old_mixed[1].enabled);
    REQUIRE(old_mixed[2].enabled);
    const uint64_t first_custom_id = old_mixed[1].stable_id;
    const uint64_t second_custom_id = old_mixed[2].stable_id;

    std::vector<std::string> rows = split_rows(mgr.serialize_custom_entries());
    REQUIRE(rows.size() == 3);
    std::swap(rows[1], rows[2]);

    auto *definitions = bundle.project_config.option<ConfigOptionString>("mixed_filament_definitions");
    REQUIRE(definitions != nullptr);
    definitions->value = join_rows(rows);

    bundle.filament_presets.push_back(bundle.filament_presets.back());
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.push_back("#00FF00");
    bundle.update_multi_material_filament_presets(size_t(-1), 2);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() >= 5);

    const auto &rebuilt = bundle.mixed_filaments.mixed_filaments();
    const unsigned int new_first_custom_virtual_id = virtual_id_for_stable_id(rebuilt, 3, first_custom_id);
    const unsigned int new_second_custom_virtual_id = virtual_id_for_stable_id(rebuilt, 3, second_custom_id);

    REQUIRE(new_first_custom_virtual_id != 0);
    REQUIRE(new_second_custom_virtual_id != 0);
    CHECK(remap[3] == new_first_custom_virtual_id);
    CHECK(remap[4] == new_second_custom_virtual_id);
}

TEST_CASE("Mixed filament remap keeps later painted colors stable when an earlier mixed row is deleted", "[MixedFilament]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"Default Filament", "Default Filament", "Default Filament", "Default Filament"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    bundle.update_multi_material_filament_presets();

    auto &mixed = bundle.mixed_filaments.mixed_filaments();
    REQUIRE(mixed.size() >= 6);

    const uint64_t stable_id_6 = mixed[1].stable_id;
    const uint64_t stable_id_7 = mixed[2].stable_id;
    const uint64_t stable_id_8 = mixed[3].stable_id;

    const std::vector<MixedFilament> old_mixed = mixed;
    mixed[0].enabled = false;
    mixed[0].deleted = true;

    bundle.update_mixed_filament_id_remap(old_mixed, 4, 4);
    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

    REQUIRE(remap.size() >= 11);
    CHECK(remap[6] == virtual_id_for_stable_id(mixed, 4, stable_id_6));
    CHECK(remap[7] == virtual_id_for_stable_id(mixed, 4, stable_id_7));
    CHECK(remap[8] == virtual_id_for_stable_id(mixed, 4, stable_id_8));
}

TEST_CASE("Mixed filament remap shifts virtual ids on physical-count expansion with structurally identical rows", "[MixedFilament]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"F1", "F2"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#00FF00"};

    // One enabled mixed row after 2 physical filaments: virtual id 3.
    MixedFilament row;
    row.component_a = 1;
    row.component_b = 2;
    row.stable_id   = 4242;
    row.enabled     = true;
    row.custom      = true;
    std::vector<MixedFilament> old_mixed{row};
    bundle.mixed_filaments.mixed_filaments() = old_mixed;

    // Physical-count expansion 2 -> 4: the row keeps every field, so the two
    // lists are structurally identical (MixedFilament::operator== ignores
    // display_color) — yet the row's virtual id shifts from 3 (2 physical + 1)
    // to 5 (4 physical + 1) because virtual ids are positional. The remap must
    // still be built in this case (Sidebar runs it whenever the count changes,
    // not only when the row list differs).
    std::vector<MixedFilament> new_mixed{row};
    bundle.mixed_filaments.mixed_filaments() = new_mixed;
    REQUIRE(bundle.mixed_filaments.mixed_filaments() == old_mixed);

    bundle.update_mixed_filament_id_remap(old_mixed, 2, 4);
    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

    REQUIRE(remap.size() >= 4);
    CHECK(remap[1] == 1);   // physical ids keep identity
    CHECK(remap[2] == 2);
    CHECK(remap[3] == 5);   // mixed row vid 3 -> 5 (shifted past the 2 new physicals)
}

TEST_CASE("Mixed filament grouped manual patterns normalize and round-trip", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#0000FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("11111112,11121111");
    REQUIRE(row.manual_pattern == "11111112,11121111");

    const std::string serialized = mgr.serialize_custom_entries();

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() == 1);
    CHECK(loaded.mixed_filaments().front().manual_pattern == "11111112,11121111");
    CHECK(loaded.mixed_filaments().front().mix_b_percent == 13);
}

TEST_CASE("Mixed filament component surface offsets round-trip and bias the second layer component", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#FFFF00"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.ratio_a = 1;
    row.ratio_b = 1;
    row.component_a_surface_offset = 0.02f;
    row.component_b_surface_offset = -0.01f;

    const std::string serialized = mgr.serialize_custom_entries();
    CHECK(serialized.find("xa0.02") != std::string::npos);
    CHECK(serialized.find("xb-0.01") != std::string::npos);

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() == 1);

    using Catch::Matchers::WithinAbs;

    const MixedFilament &loaded_row = loaded.mixed_filaments().front();
    CHECK_THAT(double(loaded_row.component_a_surface_offset), WithinAbs(0.02, 0.0001));
    CHECK_THAT(double(loaded_row.component_b_surface_offset), WithinAbs(-0.01, 0.0001));
    CHECK_THAT(double(loaded.component_surface_offset(3, 2, 0)), WithinAbs(0.01, 0.0001));
    CHECK_THAT(double(loaded.component_surface_offset(3, 2, 1)), WithinAbs(0.0, 0.0001));
}

TEST_CASE("Mixed filament apparent mix percent follows the signed bias target", "[MixedFilament]")
{
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, 0.00f, 0.00f, 0.4f) == 50);
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, 0.00f, 0.02f, 0.4f) == 45);
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, 0.02f, 0.00f, 0.4f) == 55);
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, -0.02f, 0.00f, 0.4f) == 45);
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, 0.00f, -0.02f, 0.4f) == 55);
}

TEST_CASE("Mixed filament bias helper maps signed bias to a one-sided safe offset pair", "[MixedFilament]")
{
    const auto [offset_a, offset_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.06f, 0.4f);
    CHECK(offset_a == Approx(0.0f));
    CHECK(offset_b == Approx(0.06f));

    CHECK(MixedFilamentManager::bias_ui_value_from_surface_offsets(offset_a, offset_b, 0.4f) == Approx(0.06f));

    CHECK(MixedFilamentManager::bias_ui_value_from_surface_offsets(0.02f, 0.0f, 0.4f) == Approx(-0.02f));
    CHECK(MixedFilamentManager::bias_ui_value_from_surface_offsets(-0.02f, 0.0f, 0.4f) == Approx(0.02f));

    const auto [negative_a, negative_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.06f, 0.4f);
    CHECK(negative_a == Approx(0.06f));
    CHECK(negative_b == Approx(0.0f));

    const auto [unclamped_a, unclamped_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.30f, 0.4f);
    CHECK(unclamped_a == Approx(0.0f));
    CHECK(unclamped_b == Approx(0.30f));

    const auto [unclamped_negative_a, unclamped_negative_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.30f, 0.4f);
    CHECK(unclamped_negative_a == Approx(0.30f));
    CHECK(unclamped_negative_b == Approx(0.0f));

    const auto [clamped_a, clamped_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.40f, 0.4f);
    CHECK(clamped_a == Approx(0.0f));
    CHECK(clamped_b == Approx(0.35f));

    const auto [clamped_negative_a, clamped_negative_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.40f, 0.4f);
    CHECK(clamped_negative_a == Approx(0.35f));
    CHECK(clamped_negative_b == Approx(0.0f));
}

TEST_CASE("Mixed filament component surface offsets follow the signed bias target across alternating layers", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#FFFF00"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern.clear();
    row.distribution_mode = int(MixedFilament::Simple);
    row.ratio_a = 1;
    row.ratio_b = 1;

    using Catch::Matchers::WithinAbs;
    {
        const auto [offset_a, offset_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.05f, 0.4f);
        row.component_a_surface_offset = offset_a;
        row.component_b_surface_offset = offset_b;

        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 0)), WithinAbs(0.0, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 1)), WithinAbs(0.05, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 2)), WithinAbs(0.0, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 3)), WithinAbs(0.05, 0.0001));
    }

    {
        row.component_a_surface_offset = 0.05f;
        row.component_b_surface_offset = 0.0f;

        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 0)), WithinAbs(0.05, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 1)), WithinAbs(0.0, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 2)), WithinAbs(0.05, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 3)), WithinAbs(0.0, 0.0001));
    }

    {
        const auto [offset_a, offset_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.05f, 0.4f);
        row.component_a_surface_offset = offset_a;
        row.component_b_surface_offset = offset_b;

        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 0)), WithinAbs(0.05, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 1)), WithinAbs(0.0, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 2)), WithinAbs(0.05, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 3)), WithinAbs(0.0, 0.0001));
    }
}

TEST_CASE("Mixed filament auto generation can be disabled without dropping custom rows", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};

    MixedFilamentManager enabled_mgr;
    enabled_mgr.auto_generate(colors);
    REQUIRE(enabled_mgr.mixed_filaments().size() == 3);
    const std::string serialized_auto_rows = enabled_mgr.serialize_custom_entries();

    MixedAutoGenerateGuard guard(false);

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    mgr.auto_generate(colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);
    CHECK(mgr.mixed_filaments().front().custom);
    CHECK(mgr.mixed_filaments().front().component_a == 1);
    CHECK(mgr.mixed_filaments().front().component_b == 2);

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized_auto_rows, colors);
    CHECK(loaded.mixed_filaments().empty());
}

TEST_CASE("Mixed filament perimeter resolver uses grouped manual patterns by inset", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,21");
    REQUIRE(row.manual_pattern == "12,21");

    const unsigned int mixed_filament_id = 3;
    CHECK(mgr.resolve(mixed_filament_id, 2, 0) == 1);
    CHECK(mgr.resolve(mixed_filament_id, 2, 1) == 2);

    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 0) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 0) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 1) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 1) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 3) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 3) == 1);

    const std::vector<unsigned int> ordered_layer0 = mgr.ordered_perimeter_extruders(mixed_filament_id, 2, 0);
    const std::vector<unsigned int> ordered_layer1 = mgr.ordered_perimeter_extruders(mixed_filament_id, 2, 1);
    REQUIRE(ordered_layer0.size() == 2);
    REQUIRE(ordered_layer1.size() == 2);
    CHECK(ordered_layer0[0] == 1);
    CHECK(ordered_layer0[1] == 2);
    CHECK(ordered_layer1[0] == 2);
    CHECK(ordered_layer1[1] == 1);
}

TEST_CASE("Grouped manual perimeter patterns keep grouped resolution on collapsed single-tool layers", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("2,12");
    REQUIRE(row.manual_pattern == "2,12");

    const unsigned int mixed_filament_id = 3;

    // The flattened row cadence resolves this layer to component A, but both
    // perimeter groups collapse onto physical filament 2. G-code generation
    // and tool ordering must keep using the grouped perimeter result here.
    CHECK(mgr.resolve(mixed_filament_id, 2, 1) == 1);

    const std::vector<unsigned int> ordered_layer1 = mgr.ordered_perimeter_extruders(mixed_filament_id, 2, 1);
    REQUIRE(ordered_layer1.size() == 1);
    CHECK(ordered_layer1.front() == 2);

    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 0) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 1) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 2) == 2);
}

TEST_CASE("Grouped manual perimeter patterns resolve overlapping singleton inner groups", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,1");
    REQUIRE(row.manual_pattern == "12,1");

    const unsigned int mixed_filament_id = 3;

    const std::vector<unsigned int> ordered_layer0 = mgr.ordered_perimeter_extruders(mixed_filament_id, 2, 0);
    const std::vector<unsigned int> ordered_layer1 = mgr.ordered_perimeter_extruders(mixed_filament_id, 2, 1);

    REQUIRE(ordered_layer0.size() == 1);
    CHECK(ordered_layer0.front() == 1);
    REQUIRE(ordered_layer1.size() == 2);
    CHECK(ordered_layer1[0] == 2);
    CHECK(ordered_layer1[1] == 1);

    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 0) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 1) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 0) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 1) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 2, 0) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 2, 1) == 1);
}

TEST_CASE("Grouped manual wall patterns make infill follow the innermost perimeter tool", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,1");
    REQUIRE(row.manual_pattern == "12,1");

    PrintRegionConfig region_config = static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults());
    region_config.outer_wall_filament_id.value         = 3;
    region_config.inner_wall_filament_id.value         = 3;
    region_config.wall_loops.value                     = 2;
    region_config.sparse_infill_density.value          = 15.;
    region_config.sparse_infill_filament_id.value      = 2;
    region_config.internal_solid_filament_id.value     = 3;

    PrintRegion region(region_config);

    LayerTools layer0(0.2);
    layer0.layer_index       = 0;
    layer0.object_layer_count = 6;
    layer0.layer_height      = 0.2;
    layer0.mixed_mgr         = &mgr;
    layer0.num_physical      = 2;

    LayerTools layer1(0.4);
    layer1.layer_index       = 1;
    layer1.object_layer_count = 6;
    layer1.layer_height      = 0.2;
    layer1.mixed_mgr         = &mgr;
    layer1.num_physical      = 2;

    CHECK(layer0.wall_extruder_id(region) == 0);
    CHECK(layer1.wall_extruder_id(region) == 1);
    CHECK(layer0.sparse_infill_filament_id(region) == 1);
    CHECK(layer1.sparse_infill_filament_id(region) == 1);
    CHECK(layer0.internal_solid_filament_id(region) == 0);
    CHECK(layer1.internal_solid_filament_id(region) == 0);

    region_config.sparse_infill_filament_id.value       = 2;
    region_config.internal_solid_filament_id.value      = 2;
    PrintRegion overridden_region(region_config);

    CHECK(layer0.sparse_infill_filament_id(overridden_region) == 1);
    CHECK(layer1.sparse_infill_filament_id(overridden_region) == 1);
    CHECK(layer0.internal_solid_filament_id(overridden_region) == 1);
    CHECK(layer1.internal_solid_filament_id(overridden_region) == 1);
}

TEST_CASE("Mixed filament painted-region resolver collapses ordinary mixed rows to the active physical extruder", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.ratio_a = 1;
    row.ratio_b = 1;
    row.manual_pattern.clear();
    row.distribution_mode = int(MixedFilament::Simple);

    CHECK(mgr.effective_painted_region_filament_id(3, 2, 0) == 1);
    CHECK(mgr.effective_painted_region_filament_id(3, 2, 1) == 2);
}

TEST_CASE("Mixed filament painted-region resolver preserves virtual channels for grouped and same-layer modes", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    using Catch::Matchers::WithinAbs;
    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,21");
    CHECK(mgr.effective_painted_region_filament_id(3, 2, 0) == 3);
    row.component_a_surface_offset = 0.02f;
    row.component_b_surface_offset = -0.02f;
    CHECK_THAT(double(mgr.component_surface_offset(3, 2, 0)), WithinAbs(0.0, 0.0001));

    row.manual_pattern.clear();
    row.distribution_mode = int(MixedFilament::SameLayerPointillisme);
    CHECK(mgr.effective_painted_region_filament_id(3, 2, 0) == 3);
    CHECK_THAT(double(mgr.component_surface_offset(3, 2, 0)), WithinAbs(0.0, 0.0001));
}

TEST_CASE("Mixed filament component edits rebuild painted region targets", "[MixedFilament][PrintApply]")
{
    MixedAutoGenerateGuard guard(false);

    Model model;
    ModelObject *object = model.add_object();
    object->name = "mixed-painted-object.stl";
    ModelVolume *volume = object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();

    constexpr unsigned int mixed_virtual_id = 5;
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType(mixed_virtual_id));
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    constexpr uint64_t stable_id = 4242;
    DynamicPrintConfig config = mixed_region_print_config(single_custom_mixed_definition(1, 2, stable_id));

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    const PrintObjectRegions *initial_regions = first_print_object_regions(print);
    REQUIRE(initial_regions != nullptr);
    REQUIRE_FALSE(initial_regions->layer_ranges.empty());
    const std::vector<unsigned int> expected_initial_extruders {1, 2, mixed_virtual_id};
    CHECK(first_layer_range_painted_extruders(print) == expected_initial_extruders);

    config.set("mixed_filament_definitions", single_custom_mixed_definition(3, 2, stable_id));
    print.apply(model, config);
    const PrintObjectRegions *updated_regions = first_print_object_regions(print);
    REQUIRE(updated_regions != nullptr);
    REQUIRE_FALSE(updated_regions->layer_ranges.empty());
    const std::vector<unsigned int> expected_updated_extruders {2, 3, mixed_virtual_id};
    CHECK(first_layer_range_painted_extruders(print) == expected_updated_extruders);

    config.set("mixed_filament_definitions", single_custom_mixed_definition(3, 4, stable_id));
    print.apply(model, config);
    const std::vector<unsigned int> expected_updated_second_component_extruders {3, 4, mixed_virtual_id};
    CHECK(first_layer_range_painted_extruders(print) == expected_updated_second_component_extruders);
}

TEST_CASE("ExtrusionPath copies preserve inset index", "[MixedFilament]")
{
    ExtrusionPath src(erPerimeter);
    src.inset_idx = 3;

    ExtrusionPath copied(src);
    CHECK(copied.inset_idx == 3);

    ExtrusionPath assigned(erExternalPerimeter);
    assigned.inset_idx = 0;
    assigned = src;
    CHECK(assigned.inset_idx == 3);
}

TEST_CASE("Extrusion loop and multipath entities preserve inset index", "[MixedFilament]")
{
    ExtrusionPath src(erPerimeter);
    src.inset_idx = 2;

    ExtrusionMultiPath multi_from_path(src);
    CHECK(multi_from_path.inset_idx == 2);

    ExtrusionMultiPath multi_copy(multi_from_path);
    CHECK(multi_copy.inset_idx == 2);

    ExtrusionMultiPath multi_assigned;
    multi_assigned.inset_idx = 0;
    multi_assigned = multi_from_path;
    CHECK(multi_assigned.inset_idx == 2);

    ExtrusionLoop loop_from_path(src);
    CHECK(loop_from_path.inset_idx == 2);

    ExtrusionLoop loop_copy(loop_from_path);
    CHECK(loop_copy.inset_idx == 2);
}

// ============================================================================
// [MixedFilament][Utility]
// ============================================================================

TEST_CASE("Mixed filament safe_mod handles negative wrapping and edge cases", "[MixedFilament][Utility]")
{
    CHECK(MixedFilamentManager::safe_mod(5, 3) == 2);
    CHECK(MixedFilamentManager::safe_mod(-1, 5) == 4);
    CHECK(MixedFilamentManager::safe_mod(5, 1) == 0);
    CHECK(MixedFilamentManager::safe_mod(0, 5) == 0);
    CHECK(MixedFilamentManager::safe_mod(7, 5) == 2);
}

TEST_CASE("Mixed filament normalize_ratio_pair clamps and normalizes", "[MixedFilament][Utility]")
{
    int a = 0, b = 0;
    MixedFilamentManager::normalize_ratio_pair(a, b);
    CHECK(a == 1);
    CHECK(b == 0);

    a = 0; b = 3;
    MixedFilamentManager::normalize_ratio_pair(a, b);
    CHECK(a == 0);
    CHECK(b == 3);

    a = -1; b = -2;
    MixedFilamentManager::normalize_ratio_pair(a, b);
    // negatives clamped to 0, then (0,0) → (1,0)
    CHECK(a == 1);
    CHECK(b == 0);
}

TEST_CASE("Mixed filament fill_continuous_layer_range fills gaps", "[MixedFilament][Utility]")
{
    const std::vector<int> empty;
    CHECK(fill_continuous_layer_range(empty).empty());

    const std::vector<int> single = {3};
    const auto filled_single = fill_continuous_layer_range(single);
    REQUIRE(filled_single.size() == 1);
    CHECK(filled_single[0] == 3);

    const std::vector<int> sparse = {1, 5};
    const auto filled_sparse = fill_continuous_layer_range(sparse);
    REQUIRE(filled_sparse.size() == 5);
    CHECK(filled_sparse[0] == 1);
    CHECK(filled_sparse[4] == 5);

    const std::vector<int> cont = {1, 2, 3};
    const auto filled_cont = fill_continuous_layer_range(cont);
    REQUIRE(filled_cont.size() == 3);
    CHECK(filled_cont[0] == 1);
    CHECK(filled_cont[2] == 3);
}

TEST_CASE("Mixed filament canonical_signed_bias_value extracts signed bias", "[MixedFilament][Utility]")
{
    using Catch::Matchers::WithinAbs;
    CHECK_THAT(double(MixedFilamentManager::canonical_signed_bias_value(0.f, 0.f)), WithinAbs(0.0, 0.0001));
    CHECK_THAT(double(MixedFilamentManager::canonical_signed_bias_value(0.f, 0.05f)), WithinAbs(0.05, 0.0001));
    CHECK_THAT(double(MixedFilamentManager::canonical_signed_bias_value(0.05f, 0.f)), WithinAbs(-0.05, 0.0001));
    CHECK_THAT(double(MixedFilamentManager::canonical_signed_bias_value(-0.05f, 0.f)), WithinAbs(0.05, 0.0001));
}

TEST_CASE("Mixed filament format_surface_offset_token formats and strips", "[MixedFilament][Utility]")
{
    CHECK(MixedFilamentManager::format_surface_offset_token(0.f) == "0");
    CHECK(MixedFilamentManager::format_surface_offset_token(0.02f) == "0.02");
    CHECK(MixedFilamentManager::format_surface_offset_token(-0.01f) == "-0.01");
    CHECK(MixedFilamentManager::format_surface_offset_token(2.f) == "2");
    CHECK(MixedFilamentManager::format_surface_offset_token(-0.f) == "0");
    CHECK(MixedFilamentManager::format_surface_offset_token(0.1234f) == "0.1234");
}

TEST_CASE("Mixed filament max surface offset clamps correctly", "[MixedFilament][Utility]")
{
    using Catch::Matchers::WithinRel;
    CHECK_THAT(double(MixedFilamentManager::max_component_surface_offset_mm(0.4f)), WithinRel(0.35, 0.001));
    CHECK_THAT(double(MixedFilamentManager::max_component_surface_offset_mm(0.001f)), WithinRel(0.05, 0.001));
    CHECK_THAT(double(MixedFilamentManager::max_component_surface_offset_mm(2.f)), WithinRel(0.35, 0.001));
    CHECK_THAT(double(MixedFilamentManager::max_pair_bias_mm(0.4f)), WithinRel(0.35, 0.001));
}

// ============================================================================
// [MixedFilament][Pattern]
// ============================================================================

TEST_CASE("Mixed filament manual pattern bracket notation acceptance", "[MixedFilament][Pattern]")
{
    CHECK(MixedFilamentManager::normalize_manual_pattern("[10]") == "[10]");
    CHECK(MixedFilamentManager::normalize_manual_pattern("[1]") == "1");
    CHECK(MixedFilamentManager::normalize_manual_pattern("[99]") == "[99]");
    CHECK(MixedFilamentManager::normalize_manual_pattern("[11]") == "[11]");
    CHECK(MixedFilamentManager::normalize_manual_pattern("1[10],[11]2") == "1[10],[11]2");
}

TEST_CASE("Mixed filament manual pattern bracket overflow rejection", "[MixedFilament][Pattern]")
{
    CHECK(MixedFilamentManager::normalize_manual_pattern("[100]").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("[123]").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("[9999]").empty());
}

TEST_CASE("Mixed filament manual pattern zero and leading-zero rejection", "[MixedFilament][Pattern]")
{
    CHECK(MixedFilamentManager::normalize_manual_pattern("0").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("[0]").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("[01]").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("[00]").empty());
}

TEST_CASE("Mixed filament manual pattern malformed bracket rejection", "[MixedFilament][Pattern]")
{
    CHECK(MixedFilamentManager::normalize_manual_pattern("[").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("]").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("[]").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("[1").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("1]").empty());
}

TEST_CASE("Mixed filament manual pattern empty group rejection", "[MixedFilament][Pattern]")
{
    CHECK(MixedFilamentManager::normalize_manual_pattern("1,").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern(",1").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("1,,2").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern(",").empty());
}

TEST_CASE("Mixed filament manual pattern invalid character rejection", "[MixedFilament][Pattern]")
{
    CHECK(MixedFilamentManager::normalize_manual_pattern("a").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("(1)").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern(" ").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("[1a]").empty());
}

TEST_CASE("Mixed filament mix_percent_from_manual_pattern with bracket tokens", "[MixedFilament][Pattern]")
{
    CHECK(MixedFilamentManager::mix_percent_from_manual_pattern("1112") == 25);
    CHECK(MixedFilamentManager::mix_percent_from_manual_pattern("22[2]1") == 75);
    CHECK(MixedFilamentManager::mix_percent_from_manual_pattern("[10][10]1[10]2") == 20);
}

TEST_CASE("Mixed filament physical_filament_from_token maps tokens correctly", "[MixedFilament][Pattern]")
{
    MixedFilament mf;
    mf.component_a = 3;
    mf.component_b = 4;
    const size_t num_physical = 10;

    CHECK(MixedFilamentManager::physical_filament_from_token("1", mf, num_physical) == 3);
    CHECK(MixedFilamentManager::physical_filament_from_token("2", mf, num_physical) == 4);
    CHECK(MixedFilamentManager::physical_filament_from_token("10", mf, num_physical) == 10);
    CHECK(MixedFilamentManager::physical_filament_from_token("999", mf, num_physical) == 0);
    CHECK(MixedFilamentManager::physical_filament_from_token("abc", mf, num_physical) == 0);
    CHECK(MixedFilamentManager::physical_filament_from_token("0", mf, num_physical) == 0);
}

// ============================================================================
// [MixedFilament][Color]
// ============================================================================

TEST_CASE("Mixed filament blend_color_multi blends N colors", "[MixedFilament][Color]")
{
    const std::vector<std::pair<std::string, int>> empty;
    CHECK(MixedFilamentManager::blend_color_multi(empty) == "#000000");

    const std::vector<std::pair<std::string, int>> single = {{"#FF0000", 100}};
    CHECK(MixedFilamentManager::blend_color_multi(single) == "#FF0000");

    const std::vector<std::pair<std::string, int>> all_zero = {{"#FF0000", 0}, {"#00FF00", 0}};
    CHECK(MixedFilamentManager::blend_color_multi(all_zero) == "#000000");

    const std::vector<std::pair<std::string, int>> three = {{"#FF0000", 40}, {"#00FF00", 30}, {"#0000FF", 30}};
    const std::string result = MixedFilamentManager::blend_color_multi(three);
    CHECK(!result.empty());
    CHECK(result[0] == '#');
}

TEST_CASE("Mixed filament blend_color handles ratio edge cases", "[MixedFilament][Color]")
{
    const std::string blended = MixedFilamentManager::blend_color("#FF0000", "#00FF00", 1, 1);
    CHECK(!blended.empty());
    CHECK(blended[0] == '#');

    const std::string zero_both = MixedFilamentManager::blend_color("#FF0000", "#00FF00", 0, 0);
    CHECK(!zero_both.empty());
    CHECK(zero_both[0] == '#');

    const std::string pure_b = MixedFilamentManager::blend_color("#FF0000", "#00FF00", 0, 1);
    CHECK(pure_b == "#00FF00");

    const std::string pure_a = MixedFilamentManager::blend_color("#FF0000", "#00FF00", 1, 0);
    CHECK(pure_a == "#FF0000");
}

// ============================================================================
// [MixedFilament][Lifecycle]
// ============================================================================

TEST_CASE("Mixed filament add_custom_filament guards and auto-swap", "[MixedFilament][Lifecycle]")
{
    // n<2 → no-op
    MixedFilamentManager mgr_small;
    const std::vector<std::string> one_color = {"#FF0000"};
    mgr_small.add_custom_filament(1, 2, 50, one_color);
    CHECK(mgr_small.mixed_filaments().empty());

    // Normal addition
    MixedFilamentManager mgr;
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);
    const auto &entry = mgr.mixed_filaments().front();
    CHECK(entry.custom);
    CHECK(entry.enabled);
    CHECK_FALSE(entry.deleted);
    CHECK(entry.mix_b_percent == 50);

    // a==b → auto-swap
    MixedFilamentManager mgr_swap;
    mgr_swap.add_custom_filament(1, 1, 30, colors);
    REQUIRE(mgr_swap.mixed_filaments().size() == 1);
    CHECK(mgr_swap.mixed_filaments().front().component_a == 1);
    CHECK(mgr_swap.mixed_filaments().front().component_b == 2);
}

TEST_CASE("Mixed filament auto_generate preserves state and respects disable", "[MixedFilament][Lifecycle]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};

    MixedFilamentManager::set_auto_generate_enabled(true);
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    REQUIRE(mgr.mixed_filaments().size() >= 3);

    // Disable one auto row, then re-generate
    mgr.mixed_filaments()[0].enabled = false;
    mgr.auto_generate(colors);
    CHECK_FALSE(mgr.mixed_filaments()[0].enabled);

    // Custom rows survive auto_generate
    mgr.add_custom_filament(1, 3, 50, colors);
    size_t custom_count_before = 0;
    for (const auto &mf : mgr.mixed_filaments())
        if (mf.custom) ++custom_count_before;
    mgr.auto_generate(colors);
    size_t custom_count_after = 0;
    for (const auto &mf : mgr.mixed_filaments())
        if (mf.custom) ++custom_count_after;
    CHECK(custom_count_after == custom_count_before);

    // Disabled auto-generate: only custom rows remain
    {
        MixedAutoGenerateGuard guard_disabled(false);
        mgr.auto_generate(colors);
        for (const auto &mf : mgr.mixed_filaments())
            CHECK(mf.custom);
    }
}

TEST_CASE("Mixed filament enabled_count and cleanup operations", "[MixedFilament][Lifecycle]")
{
    MixedFilamentManager mgr;
    CHECK(mgr.enabled_count() == 0);

    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    mgr.add_custom_filament(1, 2, 50, colors);
    CHECK(mgr.enabled_count() == 1);

    mgr.mixed_filaments()[0].enabled = false;
    CHECK(mgr.enabled_count() == 0);

    mgr.mixed_filaments()[0].enabled = true;
    mgr.mixed_filaments()[0].deleted = true;
    CHECK(mgr.enabled_count() == 0);

    // cleanup removes deleted
    mgr.cleanup_deleted_entries();
    CHECK(mgr.mixed_filaments().empty());
}

TEST_CASE("Mixed filament clear_custom_entries removes only custom", "[MixedFilament][Lifecycle]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager::set_auto_generate_enabled(true);
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    size_t auto_count = mgr.mixed_filaments().size();

    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.add_custom_filament(1, 3, 50, colors);
    CHECK(mgr.mixed_filaments().size() == auto_count + 2);

    mgr.clear_custom_entries();
    CHECK(mgr.mixed_filaments().size() == auto_count);
    for (const auto &mf : mgr.mixed_filaments())
        CHECK_FALSE(mf.custom);
}

TEST_CASE("Mixed filament mixed_filaments_using_physical finds dependents", "[MixedFilament][Lifecycle]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager::set_auto_generate_enabled(true);
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    // Physical 1 used in auto pairs with 2 and 3
    auto deps = mgr.mixed_filaments_using_physical(1);
    CHECK(deps.size() >= 2);

    // Physical 2 used in auto pairs
    deps = mgr.mixed_filaments_using_physical(2);
    CHECK(deps.size() >= 2);

    // Deleted entries skipped
    mgr.mixed_filaments()[0].deleted = true;
    mgr.mixed_filaments()[0].enabled = false;
    deps = mgr.mixed_filaments_using_physical(1);
    for (size_t idx : deps)
        CHECK(idx != 0);
}

TEST_CASE("Mixed filament remove_physical_filament removes and shifts", "[MixedFilament][Lifecycle]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    MixedFilamentManager::set_auto_generate_enabled(true);
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    mgr.add_custom_filament(2, 3, 50, colors);

    // Verify initial state has entries for filament 1
    size_t count_before = mgr.mixed_filaments().size();
    CHECK(count_before > 0);

    mgr.remove_physical_filament(1);

    // After removal, total count decreased (entries using old filament 1 removed)
    size_t count_after = mgr.mixed_filaments().size();
    CHECK(count_after < count_before);

    // Higher IDs shifted: old filament 2 → 1, 3 → 2, 4 → 3
    for (const auto &mf : mgr.mixed_filaments()) {
        CHECK(mf.component_a > 0);
        CHECK(mf.component_b > 0);
    }
}

TEST_CASE("Mixed filament mixed_index_from_filament_id maps correctly", "[MixedFilament][Lifecycle]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    const size_t num_physical = 2;
    CHECK(mgr.mixed_index_from_filament_id(1, num_physical) == -1);
    CHECK(mgr.mixed_index_from_filament_id(2, num_physical) == -1);
    CHECK(mgr.mixed_index_from_filament_id(3, num_physical) >= 0);

    // Deleted/disabled entry skipped
    mgr.mixed_filaments()[0].enabled = false;
    mgr.mixed_filaments()[0].deleted = true;
    CHECK(mgr.mixed_index_from_filament_id(3, num_physical) == -1);
}

TEST_CASE("Mixed filament stable_id allocation and normalize", "[MixedFilament][Lifecycle]")
{
    MixedFilamentManager mgr;
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    mgr.add_custom_filament(1, 2, 50, colors);

    const auto &entry = mgr.mixed_filaments().front();
    CHECK(entry.stable_id >= 1);
}

TEST_CASE("Mixed filament accessors total_filaments display_colors is_mixed", "[MixedFilament][Lifecycle]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    CHECK(mgr.total_filaments(2) == 3);

    CHECK(mgr.is_mixed(1, 2) == false);
    CHECK(mgr.is_mixed(3, 2) == true);

    auto display = mgr.display_colors();
    CHECK(display.size() == 1);
}

TEST_CASE("LIFE-REGRESS-01: deleted entry not returned by mixed_filaments_using_physical", "[MixedFilament][Lifecycle]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    REQUIRE(mgr.mixed_filaments().size() == 3);

    // Mark first entry as deleted and disabled
    mgr.mixed_filaments()[0].deleted = true;
    mgr.mixed_filaments()[0].enabled = false;

    const unsigned int phys_a = mgr.mixed_filaments()[0].component_a;
    const auto deps = mgr.mixed_filaments_using_physical(phys_a);

    // The deleted entry (index 0) should NOT appear in the results
    for (size_t idx : deps)
        CHECK(idx != 0);
}

TEST_CASE("LIFE-REGRESS-02: empty manager state after all entries cleared", "[MixedFilament][Lifecycle]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    REQUIRE(mgr.mixed_filaments().size() == 3);

    // Mark ALL entries as deleted and disabled to clear everything
    for (auto &mf : mgr.mixed_filaments()) {
        mf.deleted = true;
        mf.enabled = false;
    }

    const size_t num_physical = 3;
    const unsigned int first_mixed_id = 4;

    CHECK(mgr.enabled_count() == 0);
    CHECK(mgr.total_filaments(num_physical) == num_physical);
    CHECK(mgr.is_mixed(first_mixed_id, num_physical) == false);
}

// ============================================================================
// [MixedFilament][Serialization]
// ============================================================================

TEST_CASE("Mixed filament full 17-field serialization round-trip", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().front();
    mf.gradient_component_ids = "123";
    mf.gradient_component_weights = "40/30/30";
    mf.gradient_enabled = true;
    mf.gradient_start = 0.85f;
    mf.gradient_end = 0.15f;
    mf.manual_pattern = "12,21";
    mf.distribution_mode = int(MixedFilament::LayerCycle);
    mf.local_z_max_sublayers = 4;
    mf.component_a_surface_offset = 0.02f;
    mf.component_b_surface_offset = -0.01f;
    mf.deleted = false;
    mf.origin_auto = false;

    const std::string serialized = mgr.serialize_custom_entries();

    MixedFilamentManager loaded;
    loaded.add_custom_filament(1, 3, 50, colors); // add an auto pairing trigger row
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() >= 1);

    // Find the loaded custom entry
    const MixedFilament *loaded_mf = nullptr;
    for (const auto &row : loaded.mixed_filaments()) {
        if (row.custom && row.component_a == 1 && row.component_b == 2) {
            loaded_mf = &row;
            break;
        }
    }
    REQUIRE(loaded_mf != nullptr);
    CHECK(loaded_mf->gradient_enabled == true);
    CHECK(loaded_mf->distribution_mode == int(MixedFilament::LayerCycle));
    CHECK(loaded_mf->local_z_max_sublayers == 4);
    CHECK(loaded_mf->manual_pattern == "12,21");
}

TEST_CASE("Mixed filament legacy 4-token format parsing", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.load_custom_entries("1,2,1,50", colors);

    REQUIRE(mgr.mixed_filaments().size() == 1);
    const auto &entry = mgr.mixed_filaments().front();
    CHECK(entry.component_a == 1);
    CHECK(entry.component_b == 2);
    CHECK(entry.enabled);
    CHECK(entry.mix_b_percent == 50);
    CHECK(entry.custom);
}

TEST_CASE("Mixed filament legacy pointillism flag", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager::set_auto_generate_enabled(true);
    MixedFilamentManager mgr;
    // Add an auto row first so the serialized custom row is recognizable
    mgr.auto_generate(colors);
    mgr.load_custom_entries("1,2,1,1,50,1", colors);
    // The row is custom (tokens.size() > 4 with custom=1)
    bool found_custom = false;
    for (const auto &mf : mgr.mixed_filaments())
        if (mf.custom) found_custom = true;
    CHECK(found_custom);
}

TEST_CASE("Mixed filament deleted entry serialization round-trip", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().front().deleted = true;
    mgr.mixed_filaments().front().enabled = false;

    const std::string serialized = mgr.serialize_custom_entries();
    CHECK(serialized.find("d1") != std::string::npos);

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() >= 1);
    CHECK(loaded.mixed_filaments().front().deleted);
    CHECK_FALSE(loaded.mixed_filaments().front().enabled);
}

TEST_CASE("Mixed filament duplicate stable_id dedup on load", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.add_custom_filament(1, 3, 50, colors);
    // Force same stable_id
    mgr.mixed_filaments()[0].stable_id = 100;
    mgr.mixed_filaments()[1].stable_id = 100;

    const std::string serialized = mgr.serialize_custom_entries();

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() >= 2);
    // The two entries should have different stable_ids after dedup
    CHECK(loaded.mixed_filaments()[0].stable_id != loaded.mixed_filaments()[1].stable_id);
}

TEST_CASE("SER-REGRESS-01: manual_pattern preserved through serialize-load cycle", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};

    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    mgr.add_custom_filament(1, 2, 50, colors);

    // Find the custom entry and set manual_pattern
    MixedFilament *custom_entry = nullptr;
    for (auto &mf : mgr.mixed_filaments()) {
        if (mf.custom && mf.component_a == 1 && mf.component_b == 2) {
            custom_entry = &mf;
            break;
        }
    }
    REQUIRE(custom_entry != nullptr);
    // Pattern must stay in range for physical_count=4: tokens '1'/'2' are
    // symbolic (component_a/b), '3'/'4' are literal physical ids.  Out-of-range
    // ids are intentionally rejected by load_custom_entries.
    custom_entry->manual_pattern = MixedFilamentManager::normalize_manual_pattern("123434");
    REQUIRE(custom_entry->manual_pattern == "123434");

    const std::string serialized = mgr.serialize_custom_entries();

    MixedFilamentManager loaded;
    loaded.auto_generate(colors);
    loaded.load_custom_entries(serialized, colors);

    bool found = false;
    for (const auto &mf : loaded.mixed_filaments()) {
        if (mf.custom && mf.component_a == 1 && mf.component_b == 2) {
            CHECK(mf.manual_pattern == "123434");
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("SER-REGRESS-02: duplicate custom entries survive load_custom_entries", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};

    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    // Add two custom entries with same pair but different manual_patterns
    mgr.add_custom_filament(1, 2, 30, colors);
    mgr.add_custom_filament(1, 2, 70, colors);

    // Set distinct manual_patterns and stable_ids
    auto &mixed = mgr.mixed_filaments();
    std::vector<MixedFilament *> customs;
    for (auto &mf : mixed) {
        if (mf.custom && mf.component_a == 1 && mf.component_b == 2)
            customs.push_back(&mf);
    }
    REQUIRE(customs.size() == 2);

    customs[0]->manual_pattern = MixedFilamentManager::normalize_manual_pattern("1212");
    customs[0]->stable_id = 1001;
    customs[1]->manual_pattern = MixedFilamentManager::normalize_manual_pattern("2121");
    customs[1]->stable_id = 1002;

    const std::string serialized = mgr.serialize_custom_entries();

    MixedFilamentManager loaded;
    loaded.auto_generate(colors);
    loaded.load_custom_entries(serialized, colors);

    // Custom entries do NOT get pair-based dedup like auto entries
    int custom_count = 0;
    for (const auto &mf : loaded.mixed_filaments()) {
        if (mf.custom && mf.component_a == 1 && mf.component_b == 2)
            ++custom_count;
    }
    CHECK(custom_count == 2);
}

TEST_CASE("SER-REGRESS-03: deleted flag round-trips through serialize-load", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().front().deleted = true;
    mgr.mixed_filaments().front().enabled  = false;

    const std::string serialized = mgr.serialize_custom_entries();
    CHECK(serialized.find("d1") != std::string::npos);

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() >= 1);
    CHECK(loaded.mixed_filaments().front().deleted);
    CHECK_FALSE(loaded.mixed_filaments().front().enabled);
}

TEST_CASE("SER-REGRESS-04: gradient r1 token round-trip", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().front();
    mf.gradient_enabled = true;
    mf.gradient_start   = 0.8f;
    mf.gradient_end     = 0.2f;

    const std::string serialized = mgr.serialize_custom_entries();
    CHECK(serialized.find("r1/0.8000/0.2000") != std::string::npos);

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() >= 1);

    const auto &loaded_mf = loaded.mixed_filaments().front();
    CHECK(loaded_mf.gradient_enabled);
    using Catch::Matchers::WithinAbs;
    CHECK_THAT(double(loaded_mf.gradient_start), WithinAbs(0.8, 0.0001));
    CHECK_THAT(double(loaded_mf.gradient_end),   WithinAbs(0.2, 0.0001));
}

TEST_CASE("SER-REGRESS-05: ui_mode persistence through serialization", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().front();
    mf.ui_mode        = 1;
    mf.manual_pattern = MixedFilamentManager::normalize_manual_pattern("1212");
    REQUIRE(mf.manual_pattern == "1212");

    const std::string serialized = mgr.serialize_custom_entries();

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() >= 1);

    const auto &loaded_mf = loaded.mixed_filaments().front();
    CHECK(loaded_mf.ui_mode == 1);
}

TEST_CASE("SER-REGRESS-06: surface offset serialization round-trip xa/xb tokens", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().back();
    mf.component_a_surface_offset = 0.15f;
    mf.component_b_surface_offset = -0.10f;

    const std::string serialized = mgr.serialize_custom_entries();
    // format_surface_offset_token strips trailing zeros after setprecision(4):
    // 0.15 -> "xa0.15", -0.10 -> "xb-0.1"
    CHECK(serialized.find("xa0.15") != std::string::npos);
    CHECK(serialized.find("xb-0.1") != std::string::npos);

    MixedFilamentManager loaded;
    loaded.auto_generate(colors);
    loaded.load_custom_entries(serialized, colors);

    // Find the loaded custom entry by component pair
    const MixedFilament *loaded_mf = nullptr;
    for (const auto &row : loaded.mixed_filaments()) {
        if (row.custom && row.component_a == 1 && row.component_b == 2) {
            loaded_mf = &row;
            break;
        }
    }
    REQUIRE(loaded_mf != nullptr);

    using Catch::Matchers::WithinAbs;
    CHECK_THAT(double(loaded_mf->component_a_surface_offset), WithinAbs(0.15, 0.001));
    CHECK_THAT(double(loaded_mf->component_b_surface_offset), WithinAbs(-0.10, 0.001));
}

TEST_CASE("SER-REGRESS-07: origin_auto flag serialization round-trip", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().back();
    mf.origin_auto = true;
    mf.custom      = false;

    const std::string serialized = mgr.serialize_custom_entries();
    CHECK(serialized.find("o1") != std::string::npos);

    MixedFilamentManager loaded;
    loaded.auto_generate(colors);
    loaded.load_custom_entries(serialized, colors);

    // Find the loaded custom entry by component pair
    const MixedFilament *loaded_mf = nullptr;
    for (const auto &row : loaded.mixed_filaments()) {
        if (row.custom == false && row.component_a == 1 && row.component_b == 2) {
            loaded_mf = &row;
            break;
        }
    }
    REQUIRE(loaded_mf != nullptr);
    CHECK(loaded_mf->origin_auto == true);
}

// ============================================================================
// [MixedFilament][Gradient]
// ============================================================================

TEST_CASE("Mixed filament is_simple_gradient all branches", "[MixedFilament][Gradient]")
{
    MixedFilament mf;
    mf.gradient_enabled = true;
    mf.component_a = 1;
    mf.component_b = 2;
    mf.manual_pattern.clear();
    mf.gradient_component_ids = "1";

    CHECK(is_simple_gradient(mf));

    mf.gradient_enabled = false;
    CHECK_FALSE(is_simple_gradient(mf));

    mf.gradient_enabled = true;
    mf.component_a = 1;
    mf.component_b = 1;
    CHECK_FALSE(is_simple_gradient(mf));

    mf.component_b = 2;
    mf.manual_pattern = "12";
    CHECK_FALSE(is_simple_gradient(mf));

    mf.manual_pattern.clear();
    mf.gradient_component_ids = "123";
    CHECK_FALSE(is_simple_gradient(mf));
}

TEST_CASE("Mixed filament gradient serialization round-trip with r1 token", "[MixedFilament][Gradient]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().front();
    mf.gradient_component_ids = "12";
    mf.gradient_component_weights = "60/40";
    mf.gradient_enabled = true;
    mf.gradient_start = 0.85f;
    mf.gradient_end = 0.15f;

    const std::string serialized = mgr.serialize_custom_entries();
    CHECK(serialized.find("r1/0.8500/0.1500") != std::string::npos);

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() >= 1);
    const auto &loaded_mf = loaded.mixed_filaments().front();
    CHECK(loaded_mf.gradient_enabled);
    CHECK(loaded_mf.gradient_component_ids == "12");
    CHECK(loaded_mf.gradient_component_weights == "60/40");
}

TEST_CASE("Mixed filament gradient auto-disables when range too small", "[MixedFilament][Gradient]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().front();
    mf.gradient_enabled = true;
    mf.gradient_start = 0.80f;
    mf.gradient_end = 0.82f;

    const std::string serialized = mgr.serialize_custom_entries();

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() >= 1);
    CHECK_FALSE(loaded.mixed_filaments().front().gradient_enabled);
}

TEST_CASE("Mixed filament apply_gradient_settings modes", "[MixedFilament][Gradient]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager::set_auto_generate_enabled(true);
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    mgr.add_custom_filament(1, 2, 50, colors);

    // Mode 0: layer-cycle
    mgr.apply_gradient_settings(0, 0.04f, 0.16f, false);
    for (const auto &mf : mgr.mixed_filaments()) {
        if (!mf.custom) {
            CHECK(mf.ratio_a == 1);
            CHECK(mf.ratio_b == 1);
        }
    }

    // Mode 1: height-weighted
    mgr.apply_gradient_settings(1, 0.04f, 0.16f, false);
    for (const auto &mf : mgr.mixed_filaments()) {
        CHECK(mf.ratio_a >= 0);
        CHECK(mf.ratio_b >= 0);
    }
}

TEST_CASE("Mixed filament gradient multi-color resolve", "[MixedFilament][Gradient]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().front();
    mf.gradient_component_ids = "123";
    mf.gradient_component_weights = "50/30/20";
    mf.distribution_mode = int(MixedFilament::LayerCycle);

    const size_t num_physical = 3;
    const unsigned int mixed_id = 4;
    // Should resolve to different physical IDs at different layers
    unsigned int r0 = mgr.resolve(mixed_id, num_physical, 0);
    unsigned int r1 = mgr.resolve(mixed_id, num_physical, 1);
    unsigned int r2 = mgr.resolve(mixed_id, num_physical, 2);
    CHECK(r0 >= 1);
    CHECK(r0 <= num_physical);
    CHECK(r1 >= 1);
    CHECK(r1 <= num_physical);
    CHECK(r2 >= 1);
    CHECK(r2 <= num_physical);
}

// ============================================================================
// [MixedFilament][Resolve]
// ============================================================================

TEST_CASE("Mixed filament resolve passthrough and simple cadence", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    const size_t num_physical = 2;
    // Physical filament passthrough
    CHECK(mgr.resolve(1, num_physical, 0) == 1);
    CHECK(mgr.resolve(2, num_physical, 0) == 2);

    // Simple cadence: ratio_a=1, ratio_b=1
    const unsigned int mixed_id = 3;
    unsigned int r0 = mgr.resolve(mixed_id, num_physical, 0);
    unsigned int r1 = mgr.resolve(mixed_id, num_physical, 1);
    CHECK(r0 != r1);
}

TEST_CASE("Mixed filament resolve height-weighted mode", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    const size_t num_physical = 2;
    const unsigned int mixed_id = 3;
    unsigned int result = mgr.resolve(mixed_id, num_physical, 0, 0.5f, 0.2f, true);
    CHECK(result >= 1);
    CHECK(result <= num_physical);
}

TEST_CASE("Mixed filament resolve advanced dithering", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.apply_gradient_settings(0, 0.04f, 0.16f, true);
    mgr.add_custom_filament(1, 2, 50, colors);

    const size_t num_physical = 2;
    const unsigned int mixed_id = 3;
    unsigned int result = mgr.resolve(mixed_id, num_physical, 0);
    CHECK(result >= 1);
    CHECK(result <= num_physical);
}

TEST_CASE("Mixed filament resolve_perimeter and ordered_perimeter_extruders", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().front();
    mf.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,21");
    const size_t num_physical = 2;
    const unsigned int mixed_id = 3;

    CHECK(mgr.resolve_perimeter(mixed_id, num_physical, 0, 0) == 1);
    CHECK(mgr.resolve_perimeter(mixed_id, num_physical, 0, 1) == 2);

    auto ordered = mgr.ordered_perimeter_extruders(mixed_id, num_physical, 0);
    CHECK_FALSE(ordered.empty());
}

TEST_CASE("Mixed filament effective_painted_region_filament_id", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    const size_t num_physical = 2;
    const unsigned int mixed_id = 3;

    // Simple mode collapses to physical
    unsigned int eff = mgr.effective_painted_region_filament_id(mixed_id, num_physical, 0);
    CHECK(eff >= 1);
    CHECK(eff <= num_physical);

    // Pointillisme keeps virtual
    mgr.mixed_filaments().front().distribution_mode = int(MixedFilament::SameLayerPointillisme);
    CHECK(mgr.effective_painted_region_filament_id(mixed_id, num_physical, 0) == mixed_id);

    // Grouped pattern keeps virtual
    mgr.mixed_filaments().front().distribution_mode = int(MixedFilament::LayerCycle);
    mgr.mixed_filaments().front().manual_pattern = "12,21";
    CHECK(mgr.effective_painted_region_filament_id(mixed_id, num_physical, 0) == mixed_id);
}

TEST_CASE("Mixed filament component_surface_offset bias routing", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#FF0000", "#FFFF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().front();
    mf.ratio_a = 1;
    mf.ratio_b = 1;
    mf.distribution_mode = int(MixedFilament::Simple);

    const size_t num_physical = 2;
    const unsigned int mixed_id = 3;

    // Pointillisme → 0
    mf.distribution_mode = int(MixedFilament::SameLayerPointillisme);
    using Catch::Matchers::WithinAbs;
    CHECK_THAT(double(mgr.component_surface_offset(mixed_id, num_physical, 0)), WithinAbs(0.0, 0.0001));

    // Grouped → 0
    mf.distribution_mode = int(MixedFilament::LayerCycle);
    mf.manual_pattern = "12,21";
    CHECK_THAT(double(mgr.component_surface_offset(mixed_id, num_physical, 0)), WithinAbs(0.0, 0.0001));

    // Bias routing (simple mode)
    mf.distribution_mode = int(MixedFilament::Simple);
    mf.manual_pattern.clear();
    mf.component_a_surface_offset = 0.f;
    mf.component_b_surface_offset = 0.05f;
    // At layer 1 (resolved to component_b), offset should be positive
    float off = mgr.component_surface_offset(mixed_id, num_physical, 1);
    CHECK(off >= -0.01f);
}

TEST_CASE("Mixed filament mixed_filament_from_id lookup", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    const size_t num_physical = 2;
    CHECK(mgr.mixed_filament_from_id(1, num_physical) == nullptr);
    CHECK(mgr.mixed_filament_from_id(3, num_physical) != nullptr);
    CHECK(mgr.mixed_filament_from_id(999, num_physical) == nullptr);
}

// ============================================================================
// [MixedFilament][Merge] — Physical filament merge / remap (PresetBundle level)
// ============================================================================

TEST_CASE("Mixed filament merge physical shifts IDs down and removes dependents", "[MixedFilament][Merge]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"PLA Red", "PLA Green", "PLA Blue", "PLA Yellow"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF", "#FFFF00"
    };
    bundle.update_multi_material_filament_presets();

    const size_t physical_before = 4;
    auto &mgr = bundle.mixed_filaments;
    REQUIRE(mgr.mixed_filaments().size() >= 6);

    // Delete the last physical filament (truncation — the function handles this case).
    // Erase filament at 0-based index 3 (physical #4), then call with to_delete=3.
    bundle.filament_presets.pop_back();
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.pop_back();
    bundle.update_multi_material_filament_presets(3, physical_before);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    // After truncating physical 4: physical 1→1, 2→2, 3→3, 4→0 (deleted)
    REQUIRE(remap.size() >= 5);
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 2);
    CHECK(remap[3] == 3);
    CHECK(remap[4] == 0);
}

TEST_CASE("Mixed filament merge stable_id preserves surviving mixed rows", "[MixedFilament][Merge]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"PLA Red", "PLA Green", "PLA Blue"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF"
    };
    bundle.update_multi_material_filament_presets();

    const size_t physical_before = 3;
    auto &mgr = bundle.mixed_filaments;

    // Record stable_ids of mixed rows NOT depending on physical 3 (the last one)
    std::vector<uint64_t> surviving_stable_ids;
    for (const auto &mf : mgr.mixed_filaments()) {
        if (mf.enabled && !mf.deleted && mf.component_a != 3 && mf.component_b != 3)
            surviving_stable_ids.push_back(mf.stable_id);
    }
    REQUIRE_FALSE(surviving_stable_ids.empty());

    // Delete the last filament (truncation from end — 0-based index 2)
    bundle.filament_presets.pop_back();
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.pop_back();
    bundle.update_multi_material_filament_presets(2, physical_before);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() >= 4);
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 2);
    CHECK(remap[3] == 0);

    // Surviving rows should not reference the deleted physical filament
    for (const auto &mf : mgr.mixed_filaments()) {
        if (mf.enabled && !mf.deleted) {
            CHECK(mf.component_a != 3);
            CHECK(mf.component_b != 3);
        }
    }
}

TEST_CASE("Mixed filament merge canonical_pair fallback when stable_id missing", "[MixedFilament][Merge]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"PLA Red", "PLA Green", "PLA Blue"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF"
    };
    bundle.update_multi_material_filament_presets();

    const size_t physical_before = 3;
    auto &mgr = bundle.mixed_filaments;

    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    // Add a custom row that does NOT depend on the last physical filament (3)
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() >= 4);

    bool has_custom_pair_12 = false;
    for (const auto &mf : mgr.mixed_filaments())
        if (mf.custom && ((mf.component_a == 1 && mf.component_b == 2) ||
                          (mf.component_a == 2 && mf.component_b == 1)))
            has_custom_pair_12 = true;
    CHECK(has_custom_pair_12);

    // Delete the last filament (truncation — 0-based index 2 = physical 3)
    bundle.filament_presets.pop_back();
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.pop_back();
    bundle.update_multi_material_filament_presets(2, physical_before);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    CHECK(remap.size() >= 6);

    // Custom pair (1,2) should survive since it doesn't depend on deleted physical 3
    bool has_surviving_custom_pair = false;
    for (const auto &mf : mgr.mixed_filaments())
        if (mf.custom && ((mf.component_a == 1 && mf.component_b == 2) ||
                          (mf.component_a == 2 && mf.component_b == 1)))
            has_surviving_custom_pair = true;
    CHECK(has_surviving_custom_pair);
}

// ============================================================================
// [MixedFilament][Display]
// ============================================================================

TEST_CASE("Mixed filament reference nozzle mm computation", "[MixedFilament][Display]")
{
    using Catch::Matchers::WithinRel;
    const std::vector<double> nozzles = {0.4, 0.6, 0.2};

    double ref = MixedFilamentManager::mixed_filament_reference_nozzle_mm(1, 2, nozzles);
    CHECK_THAT(ref, WithinRel(0.5, 0.01));

    double single = MixedFilamentManager::mixed_filament_reference_nozzle_mm(1, 0, nozzles);
    CHECK_THAT(single, WithinRel(0.4, 0.01));

    const std::vector<double> empty_nozzles;
    double fallback = MixedFilamentManager::mixed_filament_reference_nozzle_mm(0, 0, empty_nozzles);
    CHECK_THAT(fallback, WithinRel(0.4, 0.01));
}

TEST_CASE("Mixed filament supports_bias_apparent_color conditions", "[MixedFilament][Display]")
{
    MixedFilament mf;
    mf.component_a = 1;
    mf.component_b = 2;
    mf.distribution_mode = int(MixedFilament::Simple);
    mf.gradient_component_ids.clear();
    mf.manual_pattern.clear();
    mf.gradient_enabled = false;

    MixedFilamentPreviewSettings preview;
    preview.local_z_mode = false;

    CHECK(mixed_filament_supports_bias_apparent_color(mf, preview, true));

    CHECK_FALSE(mixed_filament_supports_bias_apparent_color(mf, preview, false));
    preview.local_z_mode = true;
    CHECK_FALSE(mixed_filament_supports_bias_apparent_color(mf, preview, true));
    preview.local_z_mode = false;
    mf.distribution_mode = int(MixedFilament::SameLayerPointillisme);
    CHECK_FALSE(mixed_filament_supports_bias_apparent_color(mf, preview, true));
    mf.distribution_mode = int(MixedFilament::Simple);
    mf.manual_pattern = "12";
    CHECK_FALSE(mixed_filament_supports_bias_apparent_color(mf, preview, true));
    mf.manual_pattern.clear();
    mf.gradient_component_ids = "123";
    CHECK_FALSE(mixed_filament_supports_bias_apparent_color(mf, preview, true));
}

TEST_CASE("Mixed filament effective_local_z_preview_mix_b_percent", "[MixedFilament][Display]")
{
    MixedFilament mf;
    mf.mix_b_percent = 50;
    mf.distribution_mode = int(MixedFilament::Simple);
    mf.manual_pattern.clear();
    mf.gradient_component_ids.clear();

    MixedFilamentPreviewSettings preview;
    preview.local_z_mode = false;

    CHECK(mixed_filament_effective_local_z_preview_mix_b_percent(mf, preview) == 50);

    preview.local_z_mode = true;
    preview.nominal_layer_height = 0.2;
    preview.mixed_lower_bound = 0.04;
    preview.mixed_upper_bound = 0.16;
    preview.preferred_a_height = 0.0;
    preview.preferred_b_height = 0.0;

    int local_z_mix = mixed_filament_effective_local_z_preview_mix_b_percent(mf, preview);
    CHECK(local_z_mix >= 0);
    CHECK(local_z_mix <= 100);

    // Manual pattern → passthrough
    mf.manual_pattern = "12";
    CHECK(mixed_filament_effective_local_z_preview_mix_b_percent(mf, preview) == 50);

    // Pointillisme → passthrough
    mf.manual_pattern.clear();
    mf.distribution_mode = int(MixedFilament::SameLayerPointillisme);
    CHECK(mixed_filament_effective_local_z_preview_mix_b_percent(mf, preview) == 50);
}

TEST_CASE("Mixed filament compute_mixed_filament_display_color fallback paths", "[MixedFilament][Display]")
{
    MixedFilament entry;
    entry.component_a = 1;
    entry.component_b = 2;
    entry.mix_b_percent = 50;
    entry.distribution_mode = int(MixedFilament::Simple);

    MixedFilamentDisplayContext ctx;
    ctx.num_physical = 0;
    CHECK(compute_mixed_filament_display_color(entry, ctx) == "#26A69A");

    ctx.num_physical = 2;
    ctx.physical_colors = {"#FF0000", "#0000FF"};
    ctx.preview_settings.nominal_layer_height = 0.2;
    ctx.preview_settings.wall_loops = 1;
    ctx.nozzle_diameters = {0.4, 0.4};
    ctx.component_bias_enabled = false;

    std::string color = compute_mixed_filament_display_color(entry, ctx);
    CHECK(!color.empty());
    CHECK(color[0] == '#');

    // Out of range component → fallback
    MixedFilament bad;
    bad.component_a = 0;
    bad.component_b = 2;
    bad.mix_b_percent = 50;
    bad.distribution_mode = int(MixedFilament::Simple);
    CHECK(compute_mixed_filament_display_color(bad, ctx) == "#26A69A");
}

// ============================================================================
// [MixedFilament][Display] — Remaining display helpers
// ============================================================================

TEST_CASE("Mixed filament set_display_context auto-corrects and refreshes", "[MixedFilament][Display]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    MixedFilamentDisplayContext ctx;
    ctx.num_physical = 0;
    ctx.physical_colors = colors;
    ctx.preview_settings.wall_loops = 0;
    ctx.preview_settings.nominal_layer_height = 0.2;
    // nozzle_diameters left empty

    mgr.set_display_context(ctx);

    // After set_display_context, num_physical should be corrected to colors.size()
    auto display = mgr.display_colors();
    CHECK(display.size() >= 1);

    // total_filaments should reflect correct num_physical
    CHECK(mgr.total_filaments(3) == 3 + mgr.enabled_count());
}

TEST_CASE("Mixed filament apparent pair percentages bias on vs off", "[MixedFilament][Display]")
{
    MixedFilament mf;
    mf.component_a = 1;
    mf.component_b = 2;
    mf.mix_b_percent = 50;
    mf.distribution_mode = int(MixedFilament::Simple);
    mf.component_a_surface_offset = 0.f;
    mf.component_b_surface_offset = 0.05f;
    mf.manual_pattern.clear();
    mf.gradient_component_ids.clear();
    mf.gradient_enabled = false;

    MixedFilamentPreviewSettings preview;
    preview.local_z_mode = false;
    preview.nominal_layer_height = 0.2;
    preview.mixed_lower_bound = 0.04;
    preview.mixed_upper_bound = 0.16;

    const std::vector<double> nozzles = {0.4, 0.6};

    // Bias off: returns (100-base, base)
    auto [pct_a_off, pct_b_off] = mixed_filament_apparent_pair_percentages(mf, preview, nozzles, false);
    CHECK(pct_a_off == 50);
    CHECK(pct_b_off == 50);

    // Bias on: apparent percents shift due to surface offset
    auto [pct_a_on, pct_b_on] = mixed_filament_apparent_pair_percentages(mf, preview, nozzles, true);
    CHECK(pct_a_on + pct_b_on == 100);
    // With positive B offset (0.05mm), B's apparent percent should decrease
    CHECK(pct_b_on < pct_b_off);
}

// ============================================================================
// [MixedFilament][Resolve] — RES-REGRESS regression tests
// ============================================================================

TEST_CASE("RES-REGRESS-01: resolve handles deleted mixed filament", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    // 3 physical -> 3 auto-generated pairs: (1,2), (1,3), (2,3)
    // Virtual IDs: 4, 5, 6
    REQUIRE(mgr.mixed_filaments().size() == 3);

    const size_t num_physical = 3;
    const unsigned int virt_4 = 4;
    const unsigned int virt_5 = 5;
    const unsigned int virt_6 = 6;

    // Verify is_mixed for two of the auto-generated pairs
    CHECK(mgr.is_mixed(virt_4, num_physical));
    CHECK(mgr.is_mixed(virt_5, num_physical));

    // All 3 auto pairs are enabled, total = 3 physical + 3 mixed = 6
    const size_t total_before = mgr.total_filaments(num_physical);
    CHECK(total_before == 6);

    // Find the entry to delete by component pair (1,3) using component-based lookup
    // instead of a hardcoded index, so the test is robust against changes in
    // auto_generate ordering.
    for (auto &mf : mgr.mixed_filaments()) {
        if (mf.component_a == 1 && mf.component_b == 3) {
            mf.deleted = true;
            mf.enabled = false;
            break;
        }
    }

    // total_filaments decreases: 3 physical + 2 enabled mixed = 5
    const size_t total_after = mgr.total_filaments(num_physical);
    CHECK(total_after == 5);

    // The highest old virtual ID (6) is now unmapped because virtual IDs are
    // dynamically re-enumerated over enabled entries.  Only 2 enabled mixed remain.
    CHECK(mgr.mixed_index_from_filament_id(virt_6, num_physical) == -1);

    // resolve returns filament_id unchanged when mixed_index returns -1 (passthrough)
    CHECK(mgr.resolve(virt_6, num_physical, 0) == virt_6);

    // The surviving mixed entries still resolve correctly (virt_4 -> index 0, virt_5 -> index 1)
    CHECK(mgr.mixed_index_from_filament_id(virt_4, num_physical) >= 0);
    CHECK(mgr.mixed_index_from_filament_id(virt_5, num_physical) >= 0);
}

TEST_CASE("resolve: deleted middle mixed's virtual id re-aliases to a survivor (R3 root)", "[MixedFilament][Resolve]")
{
    // R3 root mechanism at the resolve layer. cleanup_unused_filaments_after_batch_match
    // marks redundant mixed rows `deleted` WITHOUT remapping model painting. Because
    // virtual IDs re-enumerate over *enabled* rows, the deleted row's former virtual ID
    // is silently taken by the next survivor, so painting still holding that ID resolves
    // to the WRONG mixed filament (a different valid color — not a crash, not a
    // filament-1 fallback). The fix belongs in the GUI cleanup (repoint painting off
    // redundant rows before/while marking them deleted); this test only characterizes
    // why that matters.
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    const size_t num_physical = 3;
    REQUIRE(mgr.mixed_filaments().size() == 3); // (1,2)=v4, (1,3)=v5, (2,3)=v6

    auto &rows = mgr.mixed_filaments();
    int idx_13 = -1, idx_23 = -1;
    for (int i = 0; i < int(rows.size()); ++i) {
        if (rows[size_t(i)].component_a == 1 && rows[size_t(i)].component_b == 3) idx_13 = i;
        if (rows[size_t(i)].component_a == 2 && rows[size_t(i)].component_b == 3) idx_23 = i;
    }
    REQUIRE(idx_13 >= 0);
    REQUIRE(idx_23 >= 0);

    // Simulate cleanup marking the middle (1,3) row deleted.
    rows[size_t(idx_13)].deleted = true;
    rows[size_t(idx_13)].enabled = false;
    CHECK(mgr.total_filaments(num_physical) == 5); // 3 physical + 2 enabled mixed

    // virt_5 — the deleted (1,3) row's former id — now maps to the surviving (2,3)
    // row (it shifted up into the freed enabled slot), not to -1 and not to (1,3).
    // Painting holding virt_5 would silently resolve to (2,3): the R3 hazard.
    const int mapped = mgr.mixed_index_from_filament_id(5u, num_physical);
    REQUIRE(mapped >= 0);
    CHECK(mapped == idx_23);
}

TEST_CASE("RES-REGRESS-02: resolve with gradient multi-color after component removal", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {
        "#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF"
    };
    MixedFilamentManager mgr;

    // Add a custom mixed filament whose gradient references physical filament #6
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &mf = mgr.mixed_filaments().front();
    mf.gradient_enabled = true;
    mf.gradient_component_ids = "6"; // References physical filament #6

    const size_t count_before = mgr.mixed_filaments().size();
    CHECK(count_before == 1);

    // Delete physical #6 — the gradient-dependent mixed should be removed
    mgr.remove_physical_filament(6);

    const size_t count_after = mgr.mixed_filaments().size();
    CHECK(count_after == 0);
}

TEST_CASE("RES-REGRESS-03: effective_painted_region_filament_id with Grouped mode", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF", "#FF0000"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,21");
    REQUIRE(row.manual_pattern == "12,21");

    const size_t num_physical = 3;
    const unsigned int virtual_id = 4;

    // Grouped pattern (contains comma) preserves virtual ID for paint identity
    unsigned int eff = mgr.effective_painted_region_filament_id(virtual_id, num_physical, 0);
    CHECK(eff == virtual_id);
}

// ============================================================================
// [MixedFilament][Gradient] — GRAD-REGRESS regression tests
// ============================================================================

TEST_CASE("GRAD-REGRESS-01: encode/decode gradient IDs >= 10", "[MixedFilament][Gradient]")
{
    // Multi-ID with values >= 10 uses extended slash-separated format
    const std::vector<unsigned int> ids_1_2_12 = {1, 2, 12};
    std::string encoded = MixedFilamentManager::encode_gradient_component_ids(ids_1_2_12);
    CHECK(encoded == "1/2/12");

    // Round-trip: decode back to original IDs
    std::vector<unsigned int> decoded = MixedFilamentManager::decode_gradient_component_ids(encoded, 0);
    REQUIRE(decoded.size() == 3);
    CHECK(decoded[0] == 1);
    CHECK(decoded[1] == 2);
    CHECK(decoded[2] == 12);

    // Single-ID >= 10 uses leading slash for disambiguation from legacy
    const std::vector<unsigned int> single_id_12 = {12};
    std::string encoded_single = MixedFilamentManager::encode_gradient_component_ids(single_id_12);
    CHECK(encoded_single == "/12");

    // Round-trip single
    std::vector<unsigned int> decoded_single =
        MixedFilamentManager::decode_gradient_component_ids(encoded_single, 0);
    REQUIRE(decoded_single.size() == 1);
    CHECK(decoded_single[0] == 12);

    // All legacy IDs (<= 9) still use compact single-char format
    const std::vector<unsigned int> legacy_ids = {1, 2, 3};
    std::string legacy_encoded = MixedFilamentManager::encode_gradient_component_ids(legacy_ids);
    CHECK(legacy_encoded == "123");

    std::vector<unsigned int> legacy_decoded =
        MixedFilamentManager::decode_gradient_component_ids(legacy_encoded, 0);
    REQUIRE(legacy_decoded.size() == 3);
    CHECK(legacy_decoded[0] == 1);
    CHECK(legacy_decoded[1] == 2);
    CHECK(legacy_decoded[2] == 3);

    // Mixed extended: some IDs < 10, some >= 10
    const std::vector<unsigned int> mixed_ids = {3, 12, 5};
    std::string mixed_encoded = MixedFilamentManager::encode_gradient_component_ids(mixed_ids);
    CHECK(mixed_encoded == "3/12/5");

    std::vector<unsigned int> mixed_decoded =
        MixedFilamentManager::decode_gradient_component_ids(mixed_encoded, 0);
    REQUIRE(mixed_decoded.size() == 3);
    CHECK(mixed_decoded[0] == 3);
    CHECK(mixed_decoded[1] == 12);
    CHECK(mixed_decoded[2] == 5);
}

TEST_CASE("GRAD-REGRESS-02: is_simple_gradient false with manual_pattern", "[MixedFilament][Gradient]")
{
    MixedFilament mf;
    mf.component_a = 1;
    mf.component_b = 2;
    mf.gradient_enabled = true;
    mf.manual_pattern = "1212";

    // Non-empty manual_pattern -> is_simple_gradient returns false
    CHECK_FALSE(is_simple_gradient(mf));

    // Clear manual_pattern, set 2 gradient components -> is_simple_gradient true
    mf.manual_pattern.clear();
    mf.gradient_component_ids = "12";
    CHECK(is_simple_gradient(mf));

    // Empty gradient_component_ids with valid pair also satisfies the check
    mf.gradient_component_ids.clear();
    CHECK(is_simple_gradient(mf));
}

// ============================================================================
// [MixedFilament][Delete]
// ============================================================================

TEST_CASE("remove_physical_filament detects manual_pattern dependency and removes dependents", "[MixedFilament][Delete]")
{
    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors); // C(6,2)=15 auto entries (all custom=false)

    // Add 4 custom entries as per the regression scenario:
    // A: no pattern, component_a=1, component_b=2 (no dependency on #6)
    mgr.add_custom_filament(1, 2, 50, colors);

    // B: manual_pattern="1266666" — token "6" maps to physical #6
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = "1266666";
    uint64_t idB = mgr.mixed_filaments().back().stable_id;

    // C: no pattern, component_a=2, component_b=5 (no dependency on #6)
    mgr.add_custom_filament(2, 5, 50, colors);

    // D: manual_pattern="1221412465" — token "6" maps to physical #6
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = "1221412465";
    uint64_t idD = mgr.mixed_filaments().back().stable_id;

    // Delete physical filament #6 (1-based)
    mgr.remove_physical_filament(6);

    // Check: Entry B and D should be REMOVED (they depend on physical #6 via pattern)
    bool foundB = false, foundD = false;
    bool foundA = false, foundC = false;
    for (const auto &mf : mgr.mixed_filaments()) {
        if (mf.stable_id == idB) foundB = true;
        if (mf.stable_id == idD) foundD = true;
        if (mf.component_a == 1 && mf.component_b == 2 && mf.manual_pattern.empty()) foundA = true;
        if (mf.component_a == 2 && mf.component_b == 5) foundC = true;
    }
    CHECK_FALSE(foundB);
    CHECK_FALSE(foundD);
    CHECK(foundA);
    CHECK(foundC);

    // Surviving entries should have component IDs adjusted (but #6 was the max, so no shift needed)
}

TEST_CASE("build_filament_id_remap zeros dependents via mixed_filament_depends_on_physical", "[MixedFilament][Delete]")
{
    PresetBundle bundle;
    bundle.filament_presets = {
        "PLA Red", "PLA Green", "PLA Blue", "PLA Yellow", "PLA White", "PLA Black"
    };
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FFFFFF", "#000000"
    };
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;

    // Add B: manual_pattern="1266666" — token "6" maps to physical #6
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = MixedFilamentManager::normalize_manual_pattern("1266666");
    const uint64_t idB = mgr.mixed_filaments().back().stable_id;

    // Add D: manual_pattern="1221412465" — token "6" maps to physical #6
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = MixedFilamentManager::normalize_manual_pattern("1221412465");
    const uint64_t idD = mgr.mixed_filaments().back().stable_id;

    // Record old virtual IDs for B and D before the deletion call
    const unsigned int old_virtual_id_B = virtual_id_for_stable_id(mgr.mixed_filaments(), 6, idB);
    const unsigned int old_virtual_id_D = virtual_id_for_stable_id(mgr.mixed_filaments(), 6, idD);
    REQUIRE(old_virtual_id_B > 6);
    REQUIRE(old_virtual_id_D > 6);

    // Delete physical #6 (0-based index = 5)
    const size_t physical_before = 6;
    bundle.filament_presets.pop_back();
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.pop_back();
    bundle.update_multi_material_filament_presets(5, physical_before);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() > old_virtual_id_D);
    REQUIRE(remap.size() > old_virtual_id_B);

    // Pattern-dependent entries B and D should map to 0 (deleted) in the remap
    CHECK(remap[old_virtual_id_B] == 0);
    CHECK(remap[old_virtual_id_D] == 0);

    // Verify B and D are NOT in the post-deletion mixed filaments
    bool foundB = false, foundD = false;
    for (const auto &mf : mgr.mixed_filaments()) {
        if (mf.stable_id == idB) foundB = true;
        if (mf.stable_id == idD) foundD = true;
    }
    CHECK_FALSE(foundB);
    CHECK_FALSE(foundD);
}

TEST_CASE("mixed_filament_depends_on_physical indirectly exercised via deletion remap", "[MixedFilament][Delete]")
{
    // mixed_filament_depends_on_physical is a static function in PresetBundle.cpp
    // and is exercised indirectly through update_multi_material_filament_presets.
    // This test validates that the remap correctly marks pattern-dependents as
    // removed (mapped to 0) even when component_a/component_b do not directly
    // reference the deleted physical filament.

    PresetBundle bundle;
    bundle.filament_presets = {"Filament 1", "Filament 2", "Filament 3"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF"
    };
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;

    // Add a custom entry whose manual_pattern references physical #3
    // but whose component_a/component_b are 1 and 2 (not 3).
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = MixedFilamentManager::normalize_manual_pattern("123");
    const uint64_t sid = mgr.mixed_filaments().back().stable_id;

    const unsigned int old_virtual_id = virtual_id_for_stable_id(mgr.mixed_filaments(), 3, sid);
    REQUIRE(old_virtual_id > 3);

    // Delete physical #3 (0-based index = 2)
    const size_t physical_before = 3;
    bundle.filament_presets.pop_back();
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.pop_back();
    bundle.update_multi_material_filament_presets(2, physical_before);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() > old_virtual_id);

    // The pattern-dependent entry should be mapped to 0 (deleted)
    CHECK(remap[old_virtual_id] == 0);

    // Verify the entry is gone from post-deletion state
    bool found = false;
    for (const auto &mf : mgr.mixed_filaments()) {
        if (mf.stable_id == sid) found = true;
    }
    CHECK_FALSE(found);
}

TEST_CASE("remove_physical_filament adjusts manual_pattern tokens above deleted ID", "[MixedFilament][Delete]")
{
    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    // Add a custom entry with pattern "15353"
    // "1" maps to component_a, "3" and "5" are numeric physical IDs
    mgr.add_custom_filament(1, 2, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("15353");
    const uint64_t sid = row.stable_id;

    // Delete physical filament #4 (1-based)
    mgr.remove_physical_filament(4);

    // The entry should survive (no token resolves to physical #4)
    // but tokens > 4 should be decremented: "5" → "4"
    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) {
            found = true;
            // "1" stays (<=4), "5"→"4", "3" stays (<=4), "5"→"4", "3" stays (<=4)
            CHECK(cur.manual_pattern == "14343");
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("mixed_filaments_using_physical finds multiple pattern dependents for same physical", "[MixedFilament][Delete]")
{
    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    // Add B: manual_pattern="1266666" — tokens include numeric "6"
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = MixedFilamentManager::normalize_manual_pattern("1266666");
    const uint64_t idB = mgr.mixed_filaments().back().stable_id;

    // Add D: manual_pattern="1221412465" — tokens include numeric "6"
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = MixedFilamentManager::normalize_manual_pattern("1221412465");
    const uint64_t idD = mgr.mixed_filaments().back().stable_id;

    // mixed_filaments_using_physical(6) should return BOTH entries
    const auto deps = mgr.mixed_filaments_using_physical(6);
    CHECK(deps.size() >= 2);

    // Verify both stable_ids are in the returned indices
    bool foundB_in_deps = false, foundD_in_deps = false;
    for (size_t idx : deps) {
        if (mgr.mixed_filaments()[idx].stable_id == idB) foundB_in_deps = true;
        if (mgr.mixed_filaments()[idx].stable_id == idD) foundD_in_deps = true;
    }
    CHECK(foundB_in_deps);
    CHECK(foundD_in_deps);

    // After remove_physical_filament(6), BOTH should be removed
    mgr.remove_physical_filament(6);

    bool foundB = false, foundD = false;
    for (const auto &mf : mgr.mixed_filaments()) {
        if (mf.stable_id == idB) foundB = true;
        if (mf.stable_id == idD) foundD = true;
    }
    CHECK_FALSE(foundB);
    CHECK_FALSE(foundD);
}

// ============================================================================
// [MixedFilament][Merge] — MERGE-REGRESS regression tests
// ============================================================================

TEST_CASE("MERGE-REGRESS-01: build_merge_filament_remap 3-arg merges mixed into physical", "[MixedFilament][Merge]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"PLA Red", "PLA Green", "PLA Blue", "PLA Yellow"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF", "#FFFF00"
    };
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    // C(4,2) = 6 auto entries with auto_generate enabled
    REQUIRE(mgr.mixed_filaments().size() == 6);

    // Capture old_mixed state for reference
    const std::vector<MixedFilament> old_mixed = mgr.mixed_filaments();
    const size_t total_filaments = 10; // 4 physical + 6 mixed

    // Merge mixed at 0-based ID 5 (first auto mixed in 1-based: physical 1..4, mixed 5..10)
    // into physical at 0-based ID 0 (physical #1)
    bundle.build_merge_filament_remap(/*from_id=*/5, /*to_id=*/0, total_filaments);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() >= 11); // total_filaments + 1

    // from_id(5) > to_id(0) → else branch: remap[from_id+1] = to_id+1 = 1
    CHECK(remap[6] == 1);

    // IDs before from_id+1 (i < 6) stay the same
    CHECK(remap[0] == 0);
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 2);
    CHECK(remap[3] == 3);
    CHECK(remap[4] == 4);
    CHECK(remap[5] == 5);

    // IDs after from_id+1 (i > 6) shift down by 1
    CHECK(remap[7] == 6);
    CHECK(remap[8] == 7);
    CHECK(remap[9] == 8);
    CHECK(remap[10] == 9);
}

TEST_CASE("MERGE-REGRESS-02: build_merge_filament_remap 4-arg handles dependent mixed", "[MixedFilament][Merge]")
{
    // Disable auto_generate so we control exactly which entries exist
    MixedAutoGenerateGuard guard(false);

    PresetBundle bundle;
    bundle.filament_presets = {"PLA Red", "PLA Green", "PLA Blue", "PLA Yellow"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF", "#FFFF00"
    };
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    // No auto entries (auto_generate disabled), total = 4 physical

    // Add 3 custom entries:
    // Entry 0 (0-based virtual=4): component_a=2, component_b=3 — NOT dependent on physical #1
    mgr.add_custom_filament(2, 3, 50, colors);
    // Entry 1 (0-based virtual=5): component_a=1, component_b=2 — DEPENDENT on physical #1
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = MixedFilamentManager::normalize_manual_pattern("1212");
    const uint64_t dependent_sid = mgr.mixed_filaments().back().stable_id;
    // Entry 2 (0-based virtual=6): component_a=3, component_b=4 — NOT dependent on physical #1
    mgr.add_custom_filament(3, 4, 50, colors);

    REQUIRE(mgr.mixed_filaments().size() == 3);

    const size_t total_filaments = 7; // 4 physical + 3 mixed
    const size_t num_physical    = 4;

    // Merge physical #1 (from_id=0) into first mixed entry (to_id=4, 0-based virtual ID)
    // The dependent entry at old 1-based virtual ID 6 should be zeroed out in the remap.
    bundle.build_merge_filament_remap(/*from_id=*/0, /*to_id=*/4, total_filaments, num_physical);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() >= 8); // total_filaments + 1

    // Source physical #1 (1-based=1) maps to target mixed's recalculated 1-based virtual ID.
    // target_old_virtual_id=4, target_old_mixed_idx=0.  No deleted mixed before it.
    // new_num_physical=3, target_new_virtual_id=3 → 1-based = 4.
    CHECK(remap[1] == 4);

    // Physical #2..#4 shift down by 1
    CHECK(remap[2] == 1);
    CHECK(remap[3] == 2);
    CHECK(remap[4] == 3);

    // First mixed (old 1-based=5, i=5): NOT dependent, survives.
    // old_virtual_id=4, old_mixed_idx=0, deleted_before=0
    // new_virtual_id = 3 + 0 - 0 = 3 → 1-based = 4
    CHECK(remap[5] == 4);

    // Second mixed (old 1-based=6, i=6): DEPENDENT → mapped to 0.
    const unsigned int old_virtual_id_dep = num_physical + 1; // 4 + 1 = 5, i=6
    CHECK(remap[6] == 0);

    // Third mixed (old 1-based=7, i=7): NOT dependent, survives.
    // old_virtual_id=6, old_mixed_idx=2, deleted_before=1 (dependent at vid 5)
    // new_virtual_id = 3 + 2 - 1 = 4 → 1-based = 5
    CHECK(remap[7] == 5);
}

TEST_CASE("MERGE-REGRESS-03: merge remap handles multiple dependents on same physical", "[MixedFilament][Merge]")
{
    // Disable auto_generate so we control the exact entry set
    MixedAutoGenerateGuard guard(false);

    PresetBundle bundle;
    bundle.filament_presets = {
        "PLA 1", "PLA 2", "PLA 3", "PLA 4", "PLA 5", "PLA 6"
    };
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF"
    };
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    // No auto entries, 6 physical

    // Add 4 custom entries:
    // Entry 0 (0-based virt=6): comp_a=2, comp_b=3 — NOT dependent on physical #1
    mgr.add_custom_filament(2, 3, 50, colors);

    // Entry 1 (0-based virt=7): comp_a=1, comp_b=2 — DEPENDENT on physical #1
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = MixedFilamentManager::normalize_manual_pattern("1266666");
    const uint64_t dep1_sid = mgr.mixed_filaments().back().stable_id;

    // Entry 2 (0-based virt=8): comp_a=4, comp_b=5 — NOT dependent on physical #1
    mgr.add_custom_filament(4, 5, 50, colors);

    // Entry 3 (0-based virt=9): comp_a=1, comp_b=3 — DEPENDENT on physical #1
    mgr.add_custom_filament(1, 3, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = MixedFilamentManager::normalize_manual_pattern("1221412465");
    const uint64_t dep2_sid = mgr.mixed_filaments().back().stable_id;

    REQUIRE(mgr.mixed_filaments().size() == 4);

    const size_t total_filaments = 10; // 6 physical + 4 mixed
    const size_t num_physical    = 6;

    // Merge physical #1 (from_id=0) into first mixed entry (to_id=6, 0-based).
    // The first mixed entry (comp_a=2, comp_b=3) does NOT depend on physical #1.
    bundle.build_merge_filament_remap(/*from_id=*/0, /*to_id=*/6, total_filaments, num_physical);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() >= 11); // total_filaments + 1

    // Source physical #1 (1-based=1) maps to target mixed's recalculated 1-based virtual ID.
    // target_old_virtual_id=6, target_old_mixed_idx=0, deleted_before=0
    // new_num_physical=5, target_new_virtual_id=5 → 1-based = 6
    CHECK(remap[1] == 6);

    // Physical #2..#6 shift down by 1
    CHECK(remap[2] == 1);
    CHECK(remap[3] == 2);
    CHECK(remap[4] == 3);
    CHECK(remap[5] == 4);
    CHECK(remap[6] == 5);

    // First mixed (old 1-based=7, i=7): NOT dependent, survives.
    // old_virtual_id=6, old_mixed_idx=0, deleted_before=0
    // new_virtual_id = 5 + 0 - 0 = 5 → 1-based = 6
    CHECK(remap[7] == 6);

    // Second mixed (old 1-based=8, i=8): DEPENDENT → 0
    CHECK(remap[8] == 0);

    // Third mixed (old 1-based=9, i=9): NOT dependent, survives.
    // old_virtual_id=8, old_mixed_idx=2, deleted_before=1 (dependent at vid 7)
    // new_virtual_id = 5 + 2 - 1 = 6 → 1-based = 7
    CHECK(remap[9] == 7);

    // Fourth mixed (old 1-based=10, i=10): DEPENDENT → 0
    CHECK(remap[10] == 0);

    // Verify the surviving non-dependent entry at i=9 has correct deleted_before_target adjustment
    // deleted_before for vid 8 = 1 (one dependent, vid 7, appears before it)
    // new_virtual_id = 5 + 2 - 1 = 6, so 1-based = 7. This accounts for one deleted entry
    // that was before it in the virtual ID space.
    CHECK(remap[9] == 7);
}

TEST_CASE("MERGE-REGRESS-04: merge_mixed_filament marks source deleted and serializes d1", "[MixedFilament][Merge]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"PLA Red", "PLA Green", "PLA Blue"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF"
    };
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    // C(3,2) = 3 auto entries
    REQUIRE(mgr.mixed_filaments().size() >= 3);

    // Simulate merge_mixed_filament logic: mark an auto entry as deleted
    auto &source_mf = mgr.mixed_filaments()[0];
    const uint64_t source_sid = source_mf.stable_id;
    source_mf.deleted  = true;
    source_mf.enabled  = false;

    // Assert source flags
    CHECK(source_mf.deleted);
    CHECK_FALSE(source_mf.enabled);

    // Serialize and check for "d1" token
    const std::string serialized = mgr.serialize_custom_entries();
    CHECK(serialized.find("d1") != std::string::npos);

    // Round-trip: load into a new manager and verify deleted state
    MixedFilamentManager loaded;
    loaded.auto_generate(bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values);
    loaded.clear_custom_entries();
    loaded.load_custom_entries(serialized,
        bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values);

    // Find the entry by stable_id in the loaded list
    bool found_in_loaded = false;
    for (const auto &mf : loaded.mixed_filaments()) {
        if (mf.stable_id == source_sid) {
            found_in_loaded = true;
            CHECK(mf.deleted);
            CHECK_FALSE(mf.enabled);
            break;
        }
    }
    CHECK(found_in_loaded);
}

// ============================================================================
// [MixedFilament][Delete] — DELETE-PRIORITY resolution order tests
// ============================================================================

TEST_CASE("DELETE-PRIORITY-01: resolve-order — manual_pattern blocks pair/gradient check", "[MixedFilament][Delete]")
{
    // Verify that when a mixed filament has a non-empty manual_pattern, the
    // component_a/component_b pair check and gradient check are skipped (both
    // guarded by norm.empty()), so an entry can survive even if its component_a
    // or component_b references the deleted physical filament.
    //
    // Setup: pattern "12" where both tokens are non-symbolic direct physical IDs
    // (they do not map to component_a/b).  Delete physical #5 which is not
    // referenced by the pattern but IS referenced by component_a.
    // Result: manual_pattern check passes (no token resolves to #5), gradient
    // and pair checks are skipped (norm is non-empty), so the entry SURVIVES.

    constexpr int NUM_PHYSICAL = 6;
    const std::vector<std::string> colors(NUM_PHYSICAL, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    // Add a custom entry.  We use component_a=5, component_b=2 so that
    // component_a references the soon-to-be-deleted physical #5.  Then we
    // override the manual_pattern to tokens that do NOT resolve to #5.
    mgr.add_custom_filament(5, 2, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    // Use a pattern whose tokens are all direct physical IDs ("3", "4") that
    // do not include "5".  Because the norm is non-empty the pair check
    // (which would flag component_a=5) is never reached.
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("34");
    const uint64_t sid = row.stable_id;

    mgr.remove_physical_filament(5);

    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("DELETE-PRIORITY-02: bracket notation token adjustment after deletion", "[MixedFilament][Delete]")
{
    // Setup: 6 physical.  Create a custom entry with manual_pattern="1[12]3"
    // containing a bracket-wrapped two‑digit token [12].  Delete physical #5.
    // The entry survives (no token resolves to #5), and the [12] token is
    // decremented to [11] because 12 > 5.
    //
    // Normalization preserves multi‑digit bracket tokens: "1[12]3" → "1[12]3".

    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    mgr.add_custom_filament(1, 2, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("1[12]3");
    const uint64_t sid = row.stable_id;

    mgr.remove_physical_filament(5);

    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) {
            found = true;
            // Token "1" stays (1 <= 5, not decremented)
            // Token "[12]" → 12 > 5 → decremented to 11, re‑encoded as "[11]"
            // Token "3" stays (3 <= 5, not decremented)
            CHECK(cur.manual_pattern == "1[11]3");
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("DELETE-PRIORITY-03: deleting physical #1 when manual_pattern uses symbolic 1", "[MixedFilament][Delete]")
{
    // Setup: 3 physical.  Create a custom entry with manual_pattern="121".
    // The "1" tokens are symbolic — they map to component_a via
    // physical_filament_from_token.  Delete physical #1 which matches
    // component_a, so every "1" token flags uses_deleted_in_pattern.
    // The entry IS removed because it depends on physical #1 via component_a.

    const std::vector<std::string> colors(3, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    mgr.add_custom_filament(1, 2, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("121");
    const uint64_t sid = row.stable_id;

    mgr.remove_physical_filament(1);

    // Entry removed: token "1" → component_a=1 == deleted(1)
    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) {
            found = true;
            break;
        }
    }
    CHECK_FALSE(found);
}

TEST_CASE("DELETE-BRACKET-01: bracket token crosses single-digit boundary after adjustment", "[MixedFilament][Delete]")
{
    // Setup: 6 physical.  Create a custom entry with manual_pattern="1[10]3".
    // Delete physical #1.  Token "1" resolves to component_a=1 == deleted → entry removed.
    // Instead use a setup where the bracket token crosses the boundary safely:
    // Pattern "3[10]4", delete physical #2.  None resolves to deleted.
    // Token [10] (literal 10) > 2 → decremented to 9, which is < 10 → bare "9".

    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    mgr.add_custom_filament(1, 2, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("3[10]4");
    const uint64_t sid = row.stable_id;

    mgr.remove_physical_filament(2);

    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) {
            found = true;
            // 3 > 2 → 2, 10 > 2 → 9 → bare "9" (< 10), 4 > 2 → 3
            CHECK(cur.manual_pattern == "293");
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("DELETE-GROUP-01: comma-separated group adjustment after deletion", "[MixedFilament][Delete]")
{
    // Setup: 6 physical.  Create a custom entry with multi-group pattern "34,78".
    // Delete physical #2.  Neither group resolves to deleted.
    // Group 0: "34" → 3>2→2, 4>2→3 = "23"
    // Group 1: "78" → 7>2→6, 8>2→7 = "67"
    // Result: "23,67" with comma preserved.

    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    mgr.add_custom_filament(1, 2, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("34,78");
    const uint64_t sid = row.stable_id;

    mgr.remove_physical_filament(2);

    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) {
            found = true;
            CHECK(cur.manual_pattern == "23,67");
            break;
        }
    }
    CHECK(found);
}

// ============================================================================
// [MixedFilament][Gradient] — GRAD-DEL regression tests
// ============================================================================

TEST_CASE("GRAD-DEL-01: gradient with partial component survival — matching ID removes entry", "[MixedFilament][Gradient]")
{
    // Setup: 6 physical.  Create a custom entry with gradient_component_ids="123"
    // (IDs 1, 2, 3).  Delete physical #2.  The gradient check (step 2 in
    // remove_physical_filament) finds that ID 2 matches the deleted physical,
    // so the entire mixed filament is removed — even though IDs 1 and 3 survive.

    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    mgr.add_custom_filament(1, 2, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    row.gradient_enabled       = true;
    row.gradient_component_ids = "123";
    const uint64_t sid = row.stable_id;

    mgr.remove_physical_filament(2);

    // The gradient entry should be gone
    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) found = true;
    }
    CHECK_FALSE(found);

    // No surviving entry should have gradient_component_ids set
    int gradient_entries = 0;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (!cur.gradient_component_ids.empty()) ++gradient_entries;
    }
    CHECK(gradient_entries == 0);
}

TEST_CASE("GRAD-DEL-02: gradient partial survival — no matching component, IDs adjusted", "[MixedFilament][Gradient]")
{
    // Setup: 6 physical.  Create a custom entry with gradient_component_ids="345"
    // (IDs 3, 4, 5).  Delete physical #2.  None of the gradient IDs match #2,
    // so the entry SURVIVES and all IDs > 2 are decremented: "345" → "234".

    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    mgr.add_custom_filament(3, 4, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    row.gradient_enabled       = true;
    row.gradient_component_ids = "345";
    const uint64_t sid = row.stable_id;

    mgr.remove_physical_filament(2);

    // Entry survives — none of the gradient IDs (3, 4, 5) match deleted #2
    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) {
            found = true;
            // 3 > 2 → 2, 4 > 2 → 3, 5 > 2 → 4  (all single‑digit → compact "234")
            CHECK(cur.gradient_component_ids == "234");
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("GRAD-DEL-03: stale gradient ID removed when manual_pattern survives deletion", "[MixedFilament][Gradient]")
{
    // When a mixed filament has both manual_pattern and gradient_component_ids,
    // and the pattern does not reference the deleted physical but the gradient
    // does, the entry survives (pattern is the active resolution source) but
    // the stale gradient ID must be removed from gradient_component_ids.

    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    mgr.add_custom_filament(1, 2, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("34");
    row.gradient_component_ids = "5";
    const uint64_t sid = row.stable_id;

    // Delete physical #5. Pattern "34" does not reference #5, so entry survives.
    // Gradient "5" references #5, so it must be erased (stale reference).
    mgr.remove_physical_filament(5);

    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) {
            found = true;
            CHECK(cur.gradient_component_ids.empty());
            break;
        }
    }
    CHECK(found);
}

// ============================================================================
// [MixedFilament][Config] — Dirty-check and config-resolution regression tests
// ============================================================================

// Regression test for Preset.cpp skipped_in_dirty fix:
// mixed_filament_definitions is auto-generated (PresetBundle.cpp) and must not
// cause the print preset to be marked dirty after loading a 3MF.
TEST_CASE("mixed_filament_definitions skipped in dirty check", "[MixedFilament][Config]")
{
    Preset edited(Preset::TYPE_PRINT, "test_edited", false);
    Preset reference(Preset::TYPE_PRINT, "test_reference", false);

    // Simulate what happens after 3MF load: mixed_filament_definitions exists
    // in edited (from the loaded config) but not in the system preset reference.
    edited.config.set_key_value("mixed_filament_definitions", new ConfigOptionString("0,1;1,0"));

    CHECK_FALSE(PresetCollection::is_dirty(&edited, &reference));
}

// Sanity check: the dirty mechanism still catches real user-facing changes.
// A key that is NOT in skipped_in_dirty, exists only in edited, and differs
// from its ConfigDef default MUST trigger dirty.
TEST_CASE("non-skipped missing key triggers dirty", "[MixedFilament][Config]")
{
    Preset edited(Preset::TYPE_PRINT, "test_edited", false);
    Preset reference(Preset::TYPE_PRINT, "test_reference", false);

    // layer_height default is 0.2; 0.3 differs from default
    edited.config.set_key_value("layer_height", new ConfigOptionFloat(0.3));

    CHECK(PresetCollection::is_dirty(&edited, &reference));
}

// Verify that dithering_local_z_mode properly participates in dirty tracking
// through the real PresetCollection::is_dirty path (not a mock lambda).
// When both edited and reference have the same value, the preset is clean.
TEST_CASE("dithering_local_z_mode dirty tracking via is_dirty", "[MixedFilament][Config]")
{
    Preset edited(Preset::TYPE_PRINT, "test_edited", false);
    Preset reference(Preset::TYPE_PRINT, "test_reference", false);

    // Both have dlzm=true → should be clean (no difference)
    edited.config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(true));
    reference.config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(true));
    CHECK_FALSE(PresetCollection::is_dirty(&edited, &reference));

    // Change edited to false → should be dirty
    edited.config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(false));
    CHECK(PresetCollection::is_dirty(&edited, &reference));
}

TEST_CASE("Local Z whole object setting is available for 3MF project config", "[MixedFilament][Config]")
{
    const auto &print_options = Preset::print_options();
    CHECK(std::find(print_options.begin(), print_options.end(), "dithering_local_z_whole_objects") != print_options.end());

    PresetBundle bundle;
    REQUIRE(bundle.project_config.has("dithering_local_z_whole_objects"));

    bundle.project_config.set_key_value("dithering_local_z_whole_objects", new ConfigOptionBool(true));
    DynamicPrintConfig full_config = DynamicPrintConfig::full_print_config();
    full_config.apply(bundle.project_config);
    REQUIRE(full_config.has("dithering_local_z_whole_objects"));
    CHECK(full_config.opt_bool("dithering_local_z_whole_objects"));
}

TEST_CASE("Local Z infill subdivision defaults inactive when Subdivide Mix Layer is off", "[MixedFilament][Config]")
{
    PresetBundle bundle;
    REQUIRE(bundle.project_config.has("dithering_local_z_mode"));
    REQUIRE(bundle.project_config.has("dithering_local_z_infill"));
    CHECK_FALSE(bundle.project_config.opt_bool("dithering_local_z_mode"));
    CHECK_FALSE(bundle.project_config.opt_bool("dithering_local_z_infill"));

    DynamicPrintConfig full_config = DynamicPrintConfig::full_print_config();
    full_config.apply(bundle.project_config);
    REQUIRE(full_config.has("dithering_local_z_infill"));
    CHECK_FALSE(full_config.opt_bool("dithering_local_z_infill"));
}

// --------------------------------------------------------------------------
// compute_redundant_filaments
// --------------------------------------------------------------------------

namespace {

/// Build a MixedFilamentManager seeded with `n` physical colours and an
/// optional list of custom mixed rows.  Returns the manager so callers can
/// inspect its mixed_filaments() vector.
static MixedFilamentManager build_manager(size_t n,
                                          const std::vector<MixedFilament> &extra_rows = {})
{
    std::vector<std::string> colours(n, "#FF0000");
    for (size_t i = 1; i < n; ++i)
        colours[i] = "#0000FF"; // placeholder
    // Disable auto-generate so m_mixed stays exactly what we build.
    MixedAutoGenerateGuard guard(false);
    MixedFilamentManager    mgr;
    // The constructor doesn't need colours; add_custom adds mixed rows.
    // We don't need add_batch — just push rows directly via the mutable accessor.
    for (const MixedFilament &row : extra_rows) {
        MixedFilament mf = row;
        mf.enabled       = true;
        mf.deleted       = false;
        mf.custom        = true;
        mf.origin_auto   = false;
        // stable_id is left 0: these tests never serialize (load_custom_entries
        // is what assigns/validates it), and compute_redundant_filaments ignores
        // it.  Tests that need stable_id (e.g. the differential oracle) set it
        // explicitly after building.
        mgr.mixed_filaments().push_back(std::move(mf));
    }
    return mgr;
}

/// Helper to build a minimal enabled mixed row for cascade tests.
static MixedFilament make_row(unsigned int a, unsigned int b,
                              const std::string &gradient_ids = {},
                              const std::string &manual_pattern_str = {})
{
    MixedFilament mf;
    mf.component_a            = a;
    mf.component_b            = b;
    mf.enabled                = true;
    mf.deleted                = false;
    mf.gradient_component_ids = gradient_ids;
    mf.manual_pattern         = manual_pattern_str;
    mf.stable_id              = 0;
    mf.custom                 = true;
    return mf;
}

} // namespace

TEST_CASE("compute_redundant_filaments physical-only", "[MixedFilament][redundant_set]")
{
    // 4 physicals, keep {1,3} → redundant {4,2}
    auto mgr = build_manager(4);
    auto red = compute_redundant_filaments(4, {1, 3}, {}, mgr.mixed_filaments());
    REQUIRE(red.redundant_mixed.empty());
    REQUIRE(red.redundant_physical.size() == 2);
    CHECK(red.redundant_physical[0] == 4); // descending
    CHECK(red.redundant_physical[1] == 2);
    CHECK(red.new_num_physical == 2);
}

TEST_CASE("compute_redundant_filaments mixed-only", "[MixedFilament][redundant_set]")
{
    // 4 physicals, 1 mixed row (v5), keep all physicals but not the mixed
    auto mgr = build_manager(4, {make_row(1, 2)});
    auto red = compute_redundant_filaments(4, {1,2,3,4}, {}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.empty());
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 5); // virtual id = 4+1
    CHECK(red.cascade_mixed_count == 0);
}

TEST_CASE("remove_physical_filament preserves stable_id on survivors", "[MixedFilament][redundant_set]")
{
    // Regression guard for batch-match cleanup (C1 fix): cleanup marks
    // redundant mixed rows by stable_id AFTER physical deletion rebuilds
    // m_mixed, so it relies on remove_physical_filament keeping each
    // survivor's stable_id unchanged (it edits components/pattern only).
    //   row A (1,2) stable_id=100 — references physical 2 -> DROPPED
    //   row B (1,3) stable_id=200 — survives; component 3 > 2 -> renumbered to 2
    auto mgr = build_manager(4, {make_row(1, 2), make_row(1, 3)});
    auto &rows = mgr.mixed_filaments();
    REQUIRE(rows.size() == 2);
    rows[0].stable_id = 100;
    rows[1].stable_id = 200;

    mgr.remove_physical_filament(2); // delete physical 2 (1-based)

    REQUIRE(rows.size() == 1);           // A dropped; B survives the rebuild
    CHECK(rows[0].stable_id == 200);     // identity preserved through rebuild
    CHECK(rows[0].component_a == 1);
    CHECK(rows[0].component_b == 2);     // 3 > deleted 2 -> decremented to 2
}

TEST_CASE("compute_redundant_filaments cascade component-a", "[MixedFilament][redundant_set]")
{
    // 4 physical, keep {1,2,3} (drop 4). Mixed row component_a=4 → cascade
    auto mgr = build_manager(4, {make_row(4, 1)});
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 4);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 5);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments cascade component-b", "[MixedFilament][redundant_set]")
{
    // 4 physical, keep {1,2,3}. Mixed row component_b=4 → cascade
    auto mgr = build_manager(4, {make_row(1, 4)});
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 5);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments cascade gradient-ids", "[MixedFilament][redundant_set]")
{
    // 4 physical, keep {1,2,3}. Mixed row gradient_component_ids="14" → cascade
    auto mgr = build_manager(4, {make_row(1, 2, "14")});
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 5);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments survivor-floor", "[MixedFilament][redundant_set]")
{
    // Empty kept set → survivor floor keeps only filament 1
    auto mgr = build_manager(4);
    auto red = compute_redundant_filaments(4, {}, {}, mgr.mixed_filaments());
    CHECK(red.new_num_physical == 1);
    REQUIRE(red.redundant_physical.size() == 3);
    CHECK(red.redundant_physical[0] == 4); // descending
    CHECK(red.redundant_physical[1] == 3);
    CHECK(red.redundant_physical[2] == 2);
}

TEST_CASE("compute_redundant_filaments keep-all", "[MixedFilament][redundant_set]")
{
    // 3 physical + 1 mixed, all kept
    auto mgr = build_manager(3, {make_row(1, 2)});
    auto red = compute_redundant_filaments(3, {1,2,3}, {4}, mgr.mixed_filaments());
    CHECK(red.redundant_physical.empty());
    CHECK(red.redundant_mixed.empty());
}

TEST_CASE("compute_redundant_filaments empty-mixed", "[MixedFilament][redundant_set]")
{
    // No mixed rows → no crash
    auto mgr = build_manager(2);
    auto red = compute_redundant_filaments(2, {1}, {}, mgr.mixed_filaments());
    CHECK(red.redundant_physical.size() == 1);
    CHECK(red.redundant_mixed.empty());
}

TEST_CASE("compute_redundant_filaments out-of-range ids", "[MixedFilament][redundant_set]")
{
    // kept_physical_ids={99} → filtered to empty → floor → filament 1 only
    auto mgr = build_manager(4);
    auto red = compute_redundant_filaments(4, {99}, {999}, mgr.mixed_filaments());
    CHECK(red.new_num_physical == 1);
    REQUIRE(red.redundant_physical.size() == 3);
    CHECK(red.redundant_physical[0] == 4); // descending
    CHECK(red.redundant_physical[1] == 3);
    CHECK(red.redundant_physical[2] == 2);
    CHECK(red.redundant_mixed.empty());
}

TEST_CASE("compute_redundant_filaments modern gradient-id format", "[MixedFilament][redundant_set]")
{
    // "/" separated multi-digit IDs
    auto mgr = build_manager(12, {make_row(1, 2, "1/12/3")});
    auto red = compute_redundant_filaments(12, {1,2,3,4,5,6,7,8,9,10,11}, {13}, mgr.mixed_filaments());
    // Physical 12 is not kept → gradient contains 12 → cascade
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 12);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("add_batch assigned_ids matches virtual-id enumeration", "[MixedFilament][batch_match]")
{
    // 4 physicals, no pre-existing mixed rows.
    std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    MixedAutoGenerateGuard    guard(false);
    MixedFilamentManager      mgr;

    std::vector<MixedFilamentBatchEntry> entries(2);
    entries[0].component_a   = 1;
    entries[0].component_b   = 2;
    entries[0].mix_b_percent = 50;
    entries[1].component_a   = 3;
    entries[1].component_b   = 4;
    entries[1].mix_b_percent = 30;

    std::vector<unsigned int> assigned_ids;
    mgr.add_batch_custom_filaments(entries, colors, &assigned_ids);

    REQUIRE(assigned_ids.size() == 2);
    CHECK(assigned_ids[0] == 5);
    CHECK(assigned_ids[1] == 6);

    // kept_mixed = assigned_ids → both rows kept
    auto red = compute_redundant_filaments(4, {1,2,3,4}, assigned_ids, mgr.mixed_filaments());
    CHECK(red.redundant_physical.empty());
    CHECK(red.redundant_mixed.empty());

    // With only one in kept_mixed, the other is redundant
    auto red2 = compute_redundant_filaments(4, {1,2,3,4}, {assigned_ids[0]}, mgr.mixed_filaments());
    CHECK(red2.redundant_mixed.size() == 1);
    CHECK(red2.redundant_mixed[0] == assigned_ids[1]);
}

TEST_CASE("auto_generate shifts virtual ids before add_batch", "[MixedFilament][batch_match]")
{
    // Reproduces the second-match paint-loss bug:
    //   T0: dialog computes target_filament_id assuming 4 phys + 0 mixed → v5.
    //   T1: set_num_filaments → auto_generate inserts C(4,2)=6 auto rows at v5-v10.
    //   T2: add_batch_custom_filaments assigns actual ids starting from v11.
    //   kept_mixed uses the stale dialog target (v5) → batch row (v11) becomes
    //   redundant → deleted → painting lost.
    std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};

    // T0: dialog-time state
    size_t num_phys_dialog    = 4;
    size_t existing_mixed_cnt = 0;
    unsigned int dialog_target = unsigned(num_phys_dialog + existing_mixed_cnt + 1);
    CHECK(dialog_target == 5);

    // T1: auto_generate
    MixedAutoGenerateGuard guard(true);
    MixedFilamentManager   mgr;
    mgr.auto_generate(colors);
    CHECK(mgr.enabled_count() == 6);

    // T2: add_batch
    std::vector<MixedFilamentBatchEntry> entries(1);
    entries[0].component_a   = 1;
    entries[0].component_b   = 2;
    entries[0].mix_b_percent = 50;

    std::vector<unsigned int> assigned_ids;
    mgr.add_batch_custom_filaments(entries, colors, &assigned_ids);

    REQUIRE(assigned_ids.size() == 1);
    unsigned int actual_assigned = assigned_ids[0];
    CHECK(actual_assigned == 11);

    // Dialog target ≠ actual assigned
    CHECK(dialog_target != actual_assigned);

    // BUG: kept_mixed with stale dialog_target ({5}) marks the actual batch row
    // (v11) as redundant, along with the other 5 auto rows not in the kept set.
    auto red = compute_redundant_filaments(4, {1,2,3,4}, {dialog_target}, mgr.mixed_filaments());
    REQUIRE(red.redundant_mixed.size() == 6);                // v6-v11 all redundant
    CHECK(red.redundant_mixed[5] == actual_assigned);        // last one = the batch row

    // Fix: kept_mixed with assigned_ids ({11}) keeps the batch row.
    // The 6 auto rows (v5-v10) not in kept_mixed remain redundant.
    auto red_ok = compute_redundant_filaments(4, {1,2,3,4}, assigned_ids, mgr.mixed_filaments());
    REQUIRE(red_ok.redundant_mixed.size() == 6);
    // v5-v10 are auto rows, none are the batch row
    for (unsigned int id : red_ok.redundant_mixed)
        CHECK(id != actual_assigned);
}

TEST_CASE("compute_redundant_filaments manual-pattern cascade legacy", "[MixedFilament][redundant_set]")
{
    // mixed a=1,b=2, manual_pattern="13". Token '3'=literal physical 3.
    // Keep {1,2,4}, delete 3 → manual_pattern refs deleted physical → cascade.
    auto mgr = build_manager(4, {make_row(1, 2, {}, "13")});
    auto red = compute_redundant_filaments(4, {1,2,4}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 3);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 5);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments manual-pattern cascade multi-digit-token", "[MixedFilament][redundant_set]")
{
    // Mixed row with manual_pattern containing a literal direct physical id
    // that is NOT kept.  Same semantics as the modern "/" format but using
    // the legacy encoding path which is what normalize_manual_pattern produces
    // (it drops '/' characters, converting "1/11/2" → "1[11]2" style; the
    // multi-digit normalization is covered by the existing remove_physical_filament
    // test in the source).  Here we test: "14" pattern where token '4' is a
    // literal physical id not in the kept set.
    auto mgr = build_manager(4, {make_row(1, 2, {}, "14")});
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 4);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments manual-pattern token-1-resolves-to-component-a", "[MixedFilament][redundant_set]")
{
    // mixed a=3,b=4, manual_pattern="1". Token '1'→component_a(=3).
    // Keep {1,2,3,4} (3 kept) → no cascade.
    auto mgr = build_manager(4, {make_row(3, 4, {}, "1")});
    auto red = compute_redundant_filaments(4, {1,2,3,4}, {5}, mgr.mixed_filaments());
    CHECK(red.redundant_mixed.empty());
    CHECK(red.cascade_mixed_count == 0);
}

TEST_CASE("compute_redundant_filaments manual-pattern token-2-resolves-to-component-b", "[MixedFilament][redundant_set]")
{
    // mixed a=1,b=4, manual_pattern="2". Token '2'→component_b(=4).
    // Keep {1,2,3}, delete 4 → cascade via component_b, not just via pattern.
    auto mgr = build_manager(4, {make_row(1, 4, {}, "2")});
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments both-components-cascade", "[MixedFilament][redundant_set]")
{
    // mixed a=3,b=4. Keep {1,2}, delete 3 AND 4 → both cascade.
    auto mgr = build_manager(4, {make_row(3, 4)});
    auto red = compute_redundant_filaments(4, {1,2}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 2);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments all-mixed-deleted", "[MixedFilament][redundant_set]")
{
    // 4 phys + 2 mixed. keep_phys={1,2,3,4}, kept_mixed empty.
    // Both mixed are explicit redundant (not in kept set).
    auto mgr = build_manager(4, {make_row(1, 2), make_row(3, 4)});
    auto red = compute_redundant_filaments(4, {1,2,3,4}, {}, mgr.mixed_filaments());
    CHECK(red.redundant_physical.empty());
    REQUIRE(red.redundant_mixed.size() == 2);
    CHECK(red.redundant_mixed[0] == 5);
    CHECK(red.redundant_mixed[1] == 6);
    CHECK(red.cascade_mixed_count == 0);
}

TEST_CASE("compute_redundant_filaments num-physical-2", "[MixedFilament][redundant_set]")
{
    // Smallest meaningful physical count. 2 phys + 1 mixed.
    auto mgr = build_manager(2, {make_row(1, 2)});
    auto red = compute_redundant_filaments(2, {1}, {3}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 2);
    // mixed row component_b=2 → deleted physical → cascade (not just kept)
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 3);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments num-physical-0", "[MixedFilament][redundant_set]")
{
    // No physical filaments is a degenerate input: the survivor floor does not
    // apply (nothing to survive) and nothing can be redundant.  Production code
    // never reaches this state (cleanup_unused_filaments_after_batch_match
    // guards on filament_presets.empty()), but this pure helper is exported and
    // test-visible, so the contract must hold.  Before the early-return, the
    // mixed-enumeration loop below would run with virtual_id = num_physical + 1
    // = 1 and emit bogus redundant_mixed entries for any stray rows — this test
    // pins the fixed, well-defined behaviour (empty result, no floor).
    std::vector<MixedFilament> stray{make_row(1, 2)};
    auto red = compute_redundant_filaments(0, {}, {}, stray);
    REQUIRE(red.redundant_physical.empty());
    REQUIRE(red.redundant_mixed.empty());
    CHECK(red.cascade_mixed_count == 0);
}

TEST_CASE("compute_redundant_filaments deleted-rows-skipped", "[MixedFilament][redundant_set]")
{
    // 4 phys. Push a deleted mixed row — compute must skip it.
    // NOTE: build_manager overrides enabled/deleted on extra_rows, so we push
    // the deleted row directly into the manager's mutable list after creation.
    MixedFilament del_row = make_row(1, 2);
    del_row.enabled = false;
    del_row.deleted = true;
    auto mgr = build_manager(4, {make_row(3, 4)});   // v5 = enabled row (3,4)
    mgr.mixed_filaments().insert(mgr.mixed_filaments().begin(), del_row); // pushed as-is at front
    // Now: [deleted(1,2), enabled(3,4)].  virtual_id=5 for the deleted row is
    // skipped (continue); the enabled row also gets virtual_id=5, which is in
    // kept_mixed={5} → kept.  deleted/disabled rows do NOT consume a virtual-ID slot.
    auto red = compute_redundant_filaments(4, {1,2,3,4}, {5}, mgr.mixed_filaments());
    CHECK(red.redundant_mixed.empty());
}

// ===========================================================================
// Differential oracle: compute_redundant_filaments  vs  remove_physical_filament
// ---------------------------------------------------------------------------
// compute_redundant_filaments is a PREDICTION of which mixed rows the batch-match
// cleanup should drop. The cleanup then deletes physicals via delete_filament,
// which routes through remove_physical_filament -- the AUTHORITATIVE runtime
// cascade. If the two disagree, cleanup marks the wrong rows deleted (by
// stable_id): a live, correctly-resolving row is killed, its virtual id is freed
// and re-aliased to the next survivor, and painted regions silently render as a
// different valid color (the R3 hazard). B1 is exactly such a disagreement.
//
// These tests encode the invariant as a machine-checked differential assertion:
//   for every mixed row the match decided to KEEP (kept_mixed),
//     compute flags it redundant  <=>  it does NOT survive the cascade of
//     remove_physical_filament calls.
// The oracle is remove_physical_filament itself -- no hand-written expected
// values -- so the tests cannot pass-for-the-wrong-reason by mirroring the
// author's mental model (which is how B1 slipped through: every existing
// manual_pattern test kept component_a in the kept set, so the disagree shape
// was never constructed, and the expected ids were written from the same flawed
// model that produced the bug).
// ===========================================================================

namespace {

// Asserts the differential invariant (see comment above) for the kept_mixed rows.
// `rows` must be enabled & non-deleted; each is tagged with a unique 1-based
// stable_id so survival can be tracked across remove_physical_filament's rebuild.
void expect_kept_mixed_matches_runtime(
    size_t                           num_physical,
    const std::vector<unsigned int> &kept_physical,
    const std::vector<unsigned int> &kept_mixed,
    std::vector<MixedFilament>       rows)
{
    for (size_t i = 0; i < rows.size(); ++i)
        rows[i].stable_id = static_cast<uint64_t>(i + 1);

    // (1) compute's prediction.
    auto red = compute_redundant_filaments(num_physical, kept_physical, kept_mixed, rows);
    const std::set<unsigned int> compute_redundant(red.redundant_mixed.begin(),
                                                    red.redundant_mixed.end());

    // (2) ground truth: replay the cleanup's physical-deletion cascade.
    //     Descending order is required -- cleanup iterates red.redundant_physical
    //     (descending) and remove_physical_filament renumbers components > id,
    //     so deleting in ascending order would invalidate the higher ids still
    //     pending deletion.
    // Iterate red.redundant_physical directly: it is the exact descending, survivor-
    // floor-respecting set the real cleanup deletes (compute force-keeps phys 1 when
    // nothing is kept, so phys 1 is never in redundant_physical and the cleanup never
    // deletes it). Recomputing the delete set from kept_physical instead would delete
    // phys 1 when the floor fires, diverging from the real cleanup and making the
    // oracle compare compute against the wrong ground truth.
    auto mgr = build_manager(num_physical, rows);
    for (unsigned int pid : red.redundant_physical)
        mgr.remove_physical_filament(pid);

    std::set<uint64_t> survivor_sids;
    for (const MixedFilament &mf : mgr.mixed_filaments())
        if (mf.stable_id != 0)
            survivor_sids.insert(mf.stable_id);

    // (3) invariant: a KEPT row is flagged redundant by compute  <=>  it did NOT
    //     survive the runtime cascade. Checking only kept_mixed rows is intentional:
    //     rows outside the kept set are dropped by match policy, not by physical
    //     dependency, so remove_physical_filament has no opinion on them.
    for (unsigned int vid : kept_mixed) {
        if (vid <= num_physical) continue;           // not a virtual id
        const size_t k = vid - num_physical - 1;      // 0-based enabled-row index
        if (k >= rows.size()) continue;
        const uint64_t sid = rows[k].stable_id;
        if (sid == 0) continue;

        const bool compute_says_redundant = compute_redundant.count(vid) > 0;
        const bool survived               = survivor_sids.count(sid) > 0;
        INFO("vid=" << vid << " stable_id=" << sid
             << " compute_redundant=" << (compute_says_redundant ? "yes" : "no")
             << " survived=" << (survived ? "yes" : "no")
             << " (invariant expects: redundant == !survived)");
        CHECK(compute_says_redundant != survived);
    }
}

} // namespace

TEST_CASE("B1: kept manual_pattern row whose component_a is dropped must survive (differential)", "[MixedFilament][redundant_set][differential]")
{
    // THE B1 ROOT. a=3 (dropped), b=2, manual_pattern "2" -> component_b=2 (kept).
    // The pattern does NOT reference component_a. At runtime resolve() uses the
    // pattern, so the row works; remove_physical_filament gates component_a behind
    // norm.empty() and PRESERVES the row. But compute checks component_a
    // unconditionally -> cascade -> the cleanup would mark this live row deleted.
    // RED until the component_a check is gated behind norm.empty() (aligned with
    // remove_physical_filament), at which point it turns GREEN.
    std::vector<MixedFilament> rows{ make_row(3, 2, {}, "2") };
    expect_kept_mixed_matches_runtime(4, {1, 2, 4}, {5}, rows);  // drop physical 3
}

TEST_CASE("B1 control: pattern references dropped component_b -> both drop (differential)", "[MixedFilament][redundant_set][differential]")
{
    // Mirror of B1 via component_b: a=1, b=4 (dropped), pattern "2" -> component_b=4.
    // The pattern genuinely references the dropped physical, so both compute
    // (token resolves to 4, not kept) and remove_physical (token "2"->4==deleted)
    // drop the row. GREEN -- documents that the b-path is consistent and that the
    // divergence is component_a only.
    std::vector<MixedFilament> rows{ make_row(1, 4, {}, "2") };
    expect_kept_mixed_matches_runtime(4, {1, 2, 3}, {5}, rows);  // drop physical 4
}

TEST_CASE("B1 control: pattern '1' references dropped component_a -> both drop (differential)", "[MixedFilament][redundant_set][differential]")
{
    // a=3 (dropped), pattern "1" -> component_a=3 (genuinely references the dropped
    // physical). Both compute (token resolves to 3, not kept) and remove_physical
    // (physical_filament_from_token("1")->3==deleted) drop the row. GREEN.
    // Distinguishes "pattern USES deleted component_a" (agree) from B1 "pattern
    // does NOT use component_a but it is dropped" (disagree).
    std::vector<MixedFilament> rows{ make_row(3, 2, {}, "1") };
    expect_kept_mixed_matches_runtime(4, {1, 2, 4}, {5}, rows);  // drop physical 3
}

TEST_CASE("B1 variant: multi-token pattern not touching component_a, a dropped (differential)", "[MixedFilament][redundant_set][differential]")
{
    // a=3 (dropped). pattern "2,4": token '2'->component_b=2 (kept), token '4'->literal 4 (kept).
    // Neither token references physical 3, so remove_physical PRESERVES; compute
    // over-cascades on component_a=3. RED (B1). Confirms B1 is not specific to a
    // single-token pattern.
    std::vector<MixedFilament> rows{ make_row(3, 2, {}, "2,4") };
    expect_kept_mixed_matches_runtime(4, {1, 2, 4}, {5}, rows);  // drop physical 3
}

TEST_CASE("B1 with two kept rows: only the over-cascaded one disagrees (differential)", "[MixedFilament][redundant_set][differential]")
{
    // v5: a=3 dropped, pattern "2"->b=2 kept   -> B1 over-cascade (compute drops, runtime keeps).
    // v6: a=3 dropped, pattern "1"->a=3 dropped -> both agree (drop).
    // The oracle flags exactly the v5 disagreement. RED (B1). Shows B1 can coexist
    // with a correctly-cascaded sibling row.
    std::vector<MixedFilament> rows{ make_row(3, 2, {}, "2"), make_row(3, 2, {}, "1") };
    expect_kept_mixed_matches_runtime(4, {1, 2, 4}, {5, 6}, rows);  // drop physical 3
}

TEST_CASE("B1 control: no pattern, component_a dropped -> both drop via norm-empty branch (differential)", "[MixedFilament][redundant_set][differential]")
{
    // No manual_pattern (norm empty). a=4 (dropped). Both compute (component_a check
    // in the norm-empty branch) and remove_physical (pair check, norm empty) drop
    // the row. GREEN -- confirms the divergence lives ONLY in the norm-non-empty
    // branch (i.e. only when a manual_pattern is present).
    std::vector<MixedFilament> rows{ make_row(4, 1) };
    expect_kept_mixed_matches_runtime(4, {1, 2, 3}, {5}, rows);  // drop physical 4
}

TEST_CASE("m1: compute uses num_physical bound, remove_physical uses kMax=64 (differential)", "[MixedFilament][redundant_set][differential][!shouldfail]")
{
    // KNOWN DIVERGENCE (m1) -- tagged [!shouldfail]: this test is EXPECTED to FAIL.
    // It documents that compute_redundant_filaments bounds pattern/gradient literal
    // tokens by `num_physical`, while remove_physical_filament bounds them by
    // kMaxPhysicalFilaments (64) and uses an ==deleted criterion. For an
    // out-of-range literal token they disagree:
    //   pattern "[12]" (literal), num_physical=4, drop physical 4:
    //     compute:         slashified -> pid 12 > num_physical 4 -> cascade.
    //     remove_physical: physical_filament_from_token("12", mf, 64) -> 12 != 4 -> PRESERVE.
    // UNREACHABLE in product flows: a literal token > num_physical cannot exist in
    // the live mixed list -- references_exceed_physical rejects it at EVERY write
    // path: load_custom_entries AND add_batch_custom_filaments (the batch-match
    // confirm flow's own entry point) both call it, and add_custom_filament /
    // auto_generate never set a manual_pattern/gradient at all.  The
    // clear_custom_entries + load_custom_entries cycle inside
    // update_multi_material_filament_presets (run on every filament-count change,
    // incl. set_num_filaments -> to_delete=-1, which skips remove_physical_filament)
    // re-validates with the new colour count and drops it. So this guards no
    // user-hittable bug today; it is a regression guard for the validation perimeter
    // (if that perimeter is ever weakened, this state becomes reachable and m1 turns
    // into a live wrong-delete). If m1 is ever fixed (bounds unified) this test will
    // UNEXPECTEDLY SUCCEED and the [!shouldfail] tag will flag it red -- at that point
    // drop the tag. Do NOT relax the oracle instead.
    std::vector<MixedFilament> rows{ make_row(1, 2, {}, "[12]") };
    expect_kept_mixed_matches_runtime(4, {1, 2, 3}, {5}, rows);  // drop physical 4
}


TEST_CASE("compute_redundant_filaments gradient-and-manual-pattern-both-kept", "[MixedFilament][redundant_set]")
{
    // mixed with gradient="13" and manual_pattern="4". Keep {1,2,3} → delete 4.
    // manual_pattern refs deleted id 4 → cascade, even though gradient ids are kept.
    auto mgr = build_manager(4, {make_row(1, 2, "13", "4")});
    // gradient "13": physicals 1 and 3 → both kept {1,3} ✓
    // manual_pattern "4": literal physical 4 → not in {1,2,3} → cascade
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 4);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.cascade_mixed_count == 1);
}

// ===========================================================================
// Batch-match confirm-flow mechanism guards (RV1 / RV2 in the gap register)
// ===========================================================================

TEST_CASE("display_color follows the physical palette: color-keyed matching across a palette change misses", "[MixedFilament][batch_match]")
{
    // RV1 mechanism. WHY the confirm handler matches existing custom mixed rows
    // by IDENTITY (dialog-time vid -> stable_id -> current row) instead of by
    // display color: in recommended mode the handler rewrites filament_colour
    // and calls set_num_filaments BEFORE the in-place block, and
    // update_multi_material_filament_presets -> auto_generate ends in
    // refresh_display_colors on BOTH exits (auto-generate pref on or off), so
    // display_color is already NEW-palette based when that block runs. This
    // test pins the underlying reason: the same recipe yields a different
    // display_color under a different palette, so an equality lookup keyed on
    // the dialog-time color (the rejected design) would silently never find the
    // row. The identity lookup the code actually uses is immune to this.
    MixedAutoGenerateGuard guard(false);
    auto mgr = build_manager(2, {make_row(1, 2)});
    REQUIRE(mgr.mixed_filaments().size() == 1);

    const std::vector<std::string> palette_old = {"#FF0000", "#0000FF"}; // dialog-time
    const std::vector<std::string> palette_new = {"#FFFF00", "#00FF00"}; // post-rewrite

    mgr.refresh_display_colors(palette_old);
    const std::string dialog_time_color = mgr.mixed_filaments()[0].display_color;
    REQUIRE(!dialog_time_color.empty());

    // Control: refresh is deterministic — same palette, same display_color.
    mgr.refresh_display_colors(palette_old);
    CHECK(mgr.mixed_filaments()[0].display_color == dialog_time_color);

    // Palette change (what recommended mode does before the in-place block).
    mgr.refresh_display_colors(palette_new);
    const std::string confirm_time_color = mgr.mixed_filaments()[0].display_color;
    REQUIRE(!confirm_time_color.empty());

    // The equality key the in-place block relies on no longer holds.
    CHECK(confirm_time_color != dialog_time_color);
}

TEST_CASE("physical growth shifts mixed virtual ids in the remap: dialog-time source ids go stale", "[MixedFilament][batch_match]")
{
    // RV2 mechanism. The batch dialog captures source_extruder_ids = {slot+1}
    // at dialog-open. In recommended mode with < 4 physicals the confirm
    // handler then grows the physical count (set_num_filaments) and Plater::
    // on_filaments_change consumes the remap built there and applies it to
    // painted facets BEFORE apply_batch_match_to_model runs. This test pins
    // the shift itself: after 3 -> 4 growth the mixed row's old virtual id (4)
    // maps to 5, so no facet carries the dialog-time id anymore and a facet
    // remap keyed on it (apply's extruder_remap) targets what is now physical
    // slot 4 instead of the moved mixed painting.
    MixedAutoGenerateGuard guard(false);

    PresetBundle bundle;
    bundle.filament_presets = {"Filament 1", "Filament 2", "Filament 3"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF"
    };
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    mgr.add_custom_filament(1, 2, 50, colors);
    const uint64_t sid = mgr.mixed_filaments().back().stable_id;
    REQUIRE(sid != 0);

    // Persist the custom row the way the product does after every mixed edit —
    // the non-deleting umfp path below reloads customs from this config string.
    bundle.project_config.option<ConfigOptionString>("mixed_filament_definitions", true)->value =
        mgr.serialize_custom_entries();

    const unsigned int dialog_time_vid = virtual_id_for_stable_id(mgr.mixed_filaments(), 3, sid);
    REQUIRE(dialog_time_vid == 4);
    (void)bundle.consume_last_filament_id_remap(); // discard setup remap

    // Grow 3 -> 4 exactly like the confirm handler: write the new palette
    // first, then set_num_filaments (passes the true old count to the remap).
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#00FFFF", "#FF00FF", "#FFFF00", "#00FF00"
    };
    bundle.set_num_filaments(4u, std::vector<std::string>{});

    const unsigned int post_growth_vid = virtual_id_for_stable_id(mgr.mixed_filaments(), 4, sid);
    REQUIRE(post_growth_vid == 5);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() > dialog_time_vid);
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 2);
    CHECK(remap[3] == 3);
    CHECK(remap[dialog_time_vid] == post_growth_vid); // painted facets move 4 -> 5
    CHECK(dialog_time_vid != post_growth_vid);        // the dialog-time key is stale
}

// ===========================================================================
// add_batch_custom_filaments branch coverage (out_assigned_ids contract)
// ===========================================================================

TEST_CASE("add_batch at the filament cap emits per-entry zero ids instead of truncating", "[MixedFilament][batch_match]")
{
    // The confirm handler aligns assigned_ids to mappings by construction
    // order, so the contract is STRICT: one id per input entry, 0 = dropped.
    // The cap path must therefore continue (pushing 0s), never break early.
    MixedAutoGenerateGuard guard(false);
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    MixedFilamentManager mgr;
    // Fill to one slot below the cap: 4 physical + (kMax - 5) mixed = kMax - 1.
    const size_t kMax = MAXIMUM_FILAMENT_NUMBER;
    for (size_t i = 0; i < kMax - 5; ++i)
        mgr.mixed_filaments().push_back(make_row(1, 2));
    REQUIRE(mgr.total_filaments(4) == kMax - 1);

    std::vector<MixedFilamentBatchEntry> entries(3);
    for (auto &e : entries) {
        e.component_a   = 1;
        e.component_b   = 3;
        e.mix_b_percent = 40;
    }

    std::vector<unsigned int> assigned;
    mgr.add_batch_custom_filaments(entries, colors, &assigned);

    REQUIRE(assigned.size() == 3);          // strict 1:1 with entries
    CHECK(assigned[0] == kMax);             // last free slot
    CHECK(assigned[1] == 0);                // dropped at cap
    CHECK(assigned[2] == 0);                // still one zero PER entry
    CHECK(mgr.total_filaments(4) == kMax);
}

TEST_CASE("add_batch clamps out-of-range components and falls back on a==b instead of dropping", "[MixedFilament][batch_match]")
{
    MixedAutoGenerateGuard guard(false);
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;

    std::vector<MixedFilamentBatchEntry> entries(3);
    entries[0].component_a = 2;  entries[0].component_b = 2;  // a==b       -> b = 1
    entries[1].component_a = 1;  entries[1].component_b = 1;  // a==b, a==1 -> b = 2
    entries[2].component_a = 99; entries[2].component_b = 1;  // a clamped to n=3

    std::vector<unsigned int> assigned;
    mgr.add_batch_custom_filaments(entries, colors, &assigned);

    REQUIRE(assigned.size() == 3);
    CHECK(assigned[0] == 4);
    CHECK(assigned[1] == 5);
    CHECK(assigned[2] == 6);
    REQUIRE(mgr.mixed_filaments().size() == 3);
    CHECK(mgr.mixed_filaments()[0].component_a == 2);
    CHECK(mgr.mixed_filaments()[0].component_b == 1);
    CHECK(mgr.mixed_filaments()[1].component_a == 1);
    CHECK(mgr.mixed_filaments()[1].component_b == 2);
    CHECK(mgr.mixed_filaments()[2].component_a == 3);
    CHECK(mgr.mixed_filaments()[2].component_b == 1);
}

TEST_CASE("add_batch guards: <2 colours yields all-zero ids, empty entries yield empty ids", "[MixedFilament][batch_match]")
{
    MixedAutoGenerateGuard guard(false);
    MixedFilamentManager mgr;

    std::vector<MixedFilamentBatchEntry> entries(2);
    entries[0].component_a = 1; entries[0].component_b = 2;
    entries[1].component_a = 1; entries[1].component_b = 2;

    std::vector<unsigned int> assigned = {77u}; // stale content must be overwritten
    mgr.add_batch_custom_filaments(entries, {"#FF0000"}, &assigned); // n = 1
    REQUIRE(assigned.size() == 2);              // still 1:1 with entries
    CHECK(assigned[0] == 0);
    CHECK(assigned[1] == 0);
    CHECK(mgr.mixed_filaments().empty());

    std::vector<unsigned int> assigned2 = {77u};
    mgr.add_batch_custom_filaments({}, {"#FF0000", "#00FF00"}, &assigned2);
    CHECK(assigned2.empty());                   // 0 entries -> 0 ids
    CHECK(mgr.mixed_filaments().empty());
}

TEST_CASE("serialize/load round-trip preserves custom-row stable_id", "[MixedFilament][Serialization]")
{
    // The batch-match cleanup marks redundant mixed rows by stable_id AFTER
    // physical deletions that round-trip the list through serialize -> clear ->
    // load (update_multi_material_filament_presets).  Identity surviving that
    // round-trip is the load-bearing assumption; pin it explicitly.
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedAutoGenerateGuard guard(false);
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.add_custom_filament(2, 3, 30, colors);
    REQUIRE(mgr.mixed_filaments().size() == 2);
    const uint64_t sid0 = mgr.mixed_filaments()[0].stable_id;
    const uint64_t sid1 = mgr.mixed_filaments()[1].stable_id;
    REQUIRE(sid0 != 0);
    REQUIRE(sid1 != 0);
    REQUIRE(sid0 != sid1);

    const std::string serialized = mgr.serialize_custom_entries();
    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);

    bool found0 = false;
    bool found1 = false;
    for (const MixedFilament &mf : loaded.mixed_filaments()) {
        if (mf.stable_id == sid0) {
            found0 = true;
            CHECK(mf.component_a == 1);
            CHECK(mf.component_b == 2);
        }
        if (mf.stable_id == sid1) {
            found1 = true;
            CHECK(mf.component_a == 2);
            CHECK(mf.component_b == 3);
        }
    }
    CHECK(found0);
    CHECK(found1);
}

TEST_CASE("add_batch rejects entries whose gradient/pattern reference a filament > n (m1 perimeter)", "[MixedFilament][batch_match]")
{
    // Regression guard for the major-1 fix. compute_redundant_filaments bounds
    // pattern/gradient literal tokens by num_physical, while remove_physical_
    // filament bounds them by kMaxPhysicalFilaments=64 (the m1 divergence,
    // documented in MixedFilament.hpp and pinned by the [!shouldfail] "m1"
    // differential test). For m1 to stay UNREACHABLE the invariant "no live row
    // references a literal physical id > n" must hold at EVERY write path, not
    // just load_custom_entries. add_batch_custom_filaments is the batch-match
    // confirm flow's own entry point, so it must reject out-of-range references
    // the same way load_custom_entries does. Without this guard a recipe
    // generator change that ever emitted a literal token > n would turn m1 into a
    // live silent-wrong-color bug (cleanup marks the wrong rows deleted).
    MixedAutoGenerateGuard guard(false);
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}; // n = 4
    MixedFilamentManager mgr;

    std::vector<MixedFilamentBatchEntry> entries(3);
    // e0: valid (a,b in range, no gradient) -> assigned a real id
    entries[0].component_a = 1;
    entries[0].component_b = 2;
    // e1: gradient references physical 12 > n=4 -> REJECTED (assigned 0)
    entries[1].component_a = 1;
    entries[1].component_b = 2;
    entries[1].gradient_component_ids = "1/12/3";
    // e2: manual_pattern literal token '5' > n=4 -> REJECTED (assigned 0)
    entries[2].component_a = 1;
    entries[2].component_b = 2;
    entries[2].manual_pattern = "15"; // '1' symbolic, '5' literal > 4

    std::vector<unsigned int> assigned;
    mgr.add_batch_custom_filaments(entries, colors, &assigned);

    REQUIRE(assigned.size() == 3);              // strict 1:1 with entries
    CHECK(assigned[0] == 5);                    // only e0 created (4 phys + 1)
    CHECK(assigned[1] == 0u);                   // e1 rejected
    CHECK(assigned[2] == 0u);                   // e2 rejected
    REQUIRE(mgr.mixed_filaments().size() == 1); // only e0's row stored
    CHECK(mgr.mixed_filaments()[0].component_a == 1);
    CHECK(mgr.mixed_filaments()[0].component_b == 2);

    // The one stored row is clean (no out-of-range references) and, since it is
    // the only enabled mixed row at v5, compute_redundant_filaments sees a list
    // whose every token is in range -- the m1 state never enters it.
    auto red = compute_redundant_filaments(4, {1, 2, 3, 4}, {5}, mgr.mixed_filaments());
    CHECK(red.redundant_mixed.empty());
}

// --------------------------------------------------------------------------
// [shrink] — set_num_filaments tail-truncation path
//
// These tests pin the remap behaviour when the physical filament count is
// REDUCED via set_num_filaments(N) with N < current. This is the "compact"
// path the batch-match confirm flow uses (mirroring recommended mode's
// set_num_filaments call): it truncates the tail rather than deleting an
// arbitrary middle slot, so surviving physical ids keep their identity and
// the truncated tail maps to 0 (NONE). auto_generate is disabled via
// MixedAutoGenerateGuard to isolate the pure remap-table behaviour from any
// mixed-list rebuild side effects.
// --------------------------------------------------------------------------

TEST_CASE("shrink: set_num_filaments remap physical tail to 0", "[MixedFilament][shrink]")
{
    MixedAutoGenerateGuard guard(false);
    PresetBundle bundle;
    bundle.filament_presets = {"Default Filament", "Default Filament", "Default Filament",
                               "Default Filament", "Default Filament", "Default Filament"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values =
        {"#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF"};
    bundle.update_multi_material_filament_presets();

    // Shrink 6 -> 2 (tail truncation, same call shape as recommended mode).
    bundle.set_num_filaments(2, std::vector<std::string>{});
    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

    // Remap covers old_total + 1 (6 physical + 0 mixed + 1). Index 0 is the
    // NONE sink and is always present.
    REQUIRE(remap.size() == 7);

    // Surviving head keeps identity.
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 2);
    // Truncated tail maps to 0 (NONE) — NOT identity. This is what makes
    // tail-truncation safe for the compact flow: painting referencing a
    // deleted physical is cleanly redirected to NONE.
    CHECK(remap[3] == 0);
    CHECK(remap[4] == 0);
    CHECK(remap[5] == 0);
    CHECK(remap[6] == 0);
}

TEST_CASE("shrink: tail-truncate keeps surviving ids identity, mid-delete shifts", "[MixedFilament][shrink]")
{
    MixedAutoGenerateGuard guard(false);

    // --- Scenario A: tail-truncate 6 -> 4 --------------------------------
    {
        PresetBundle bundle;
        bundle.filament_presets = {"Default Filament", "Default Filament", "Default Filament",
                                   "Default Filament", "Default Filament", "Default Filament"};
        bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values =
            {"#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF"};
        bundle.update_multi_material_filament_presets();

        bundle.set_num_filaments(4, std::vector<std::string>{});
        const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

        REQUIRE(remap.size() >= 5);
        // All survivors keep identity — no shifting because nothing was
        // removed from the middle.
        CHECK(remap[1] == 1);
        CHECK(remap[2] == 2);
        CHECK(remap[3] == 3);
        CHECK(remap[4] == 4);
    }

    // --- Scenario B: mid-delete via update_num_filaments(index 2) --------
    // Deleting the 3rd physical (0-based index 2) shifts every higher id
    // down by one — the opposite of tail-truncation.
    {
        PresetBundle bundle;
        bundle.filament_presets = {"Default Filament", "Default Filament", "Default Filament",
                                   "Default Filament", "Default Filament", "Default Filament"};
        bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values =
            {"#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF"};
        bundle.update_multi_material_filament_presets();

        bundle.update_num_filaments(2); // remove 0-based index 2 → 1-based id 3
        const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

        REQUIRE(remap.size() >= 6);
        // Deleted slot maps to 0.
        CHECK(remap[3] == 0);
        // Every id above the deleted slot shifts down by one.
        CHECK(remap[4] == 3);
        CHECK(remap[5] == 4);
        CHECK(remap[6] == 5);
        // Ids below the deleted slot are unaffected.
        CHECK(remap[1] == 1);
        CHECK(remap[2] == 2);
    }
}

TEST_CASE("shrink: redundant_physical empty after tail-truncate to kept count", "[MixedFilament][shrink]")
{
    MixedAutoGenerateGuard guard(false);
    PresetBundle bundle;
    bundle.filament_presets = {"Default Filament", "Default Filament", "Default Filament", "Default Filament"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values =
        {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    bundle.update_multi_material_filament_presets();

    // Compact: keep only the first 2 physicals.
    bundle.set_num_filaments(2, std::vector<std::string>{});
    bundle.consume_last_filament_id_remap(); // drain, not needed here

    // After compaction the physical count is already the kept count, so
    // compute_redundant_filaments must report NO redundant physicals.
    // This is the precondition for the cleanup loop being a no-op in the
    // compact flow — if this fails, the deletion loop would still run and
    // the optimisation would not hold.
    auto red = compute_redundant_filaments(2, {1, 2}, {}, bundle.mixed_filaments.mixed_filaments());
    CHECK(red.redundant_physical.empty());
    CHECK(red.new_num_physical == 2);
}

// ===========================================================================
// Non-contiguous manual-mode subset — composite remap divergence
//
// Characterization tests for the "partial colour corruption" bug that fires
// when a manual-mode batch match selects a NON-contiguous subset of the
// physical palette, e.g. [2,6,8,10] out of 10 physical filaments.
//
// Root cause (verified by reading the production code, not inferred):
//   cleanup_unused_filaments_after_batch_match (Plater.cpp:8354-8446) deletes
//   the unselected physical slots via middle-deletion repacking, which compacts
//   the survivors into the low ids (C2->1, C6->2, C8->3, C10->4). It then builds
//   ONE composite painting remap via update_mixed_filament_id_remap(old, 10, 4)
//   (Plater.cpp:8425). That call routes into build_filament_id_remap with
//   deleting_filament=false (PresetBundle.cpp:3774), whose PHYSICAL branch
//   (PresetBundle.cpp:3824-3834) only does tail-truncation when
//   deleting_filament=false:
//       old_id <= new_num -> mapped = old_id (identity)
//       else              -> mapped = 0       (truncated tail)
//   Tail-truncation is only correct when the surviving set is exactly {1..N}
//   (the documented invariant at Plater.cpp:8360-8366). recommended mode
//   satisfies it because set_num_filaments rewrites the palette head-first;
//   manual mode with an arbitrary subset like [2,6,8,10] does NOT, and the
//   invariant is silently violated with no runtime check on the kept-set shape.
//
// Result for [2,6,8,10]:
//   - paint on C6/C8/C10 (old ids 6/8/10, SURVIVORS) -> remapped to 0 (LOST)
//   - paint on C1/C3/C4 (old ids 1/3/4, DELETED)     -> remapped to 1/3/4
//     which now hold C2/C8/C10 (WRONG COLOUR)
//   mixed virtual ids survive correctly (stable_id path), so the corruption
//   is PARTIAL — only unmigrated physical-source painting is affected, which
//   matches the reported "部分颜色错乱" symptom.
//
// These tests pin the behaviour at TWO layers:
//   Layer 1: compute_redundant_filaments (pure fn, upstream input — CORRECT,
//            just pinned so the remap tests have deterministic inputs).
//   Layer 2: update_mixed_filament_id_remap batch path (the bug itself).
//
// For the bug layer we use the "double test" pattern (consistent with the m1
// test above): one TEST_CASE pins the CURRENT (wrong) output (green), and a
// sibling tagged [!shouldfail] pins the EXPECTED (correct) output (red). When
// the bug is fixed the [!shouldfail] case will "unexpectedly succeed" and CI
// will flag it — at that point drop the tag. Do NOT relax the oracle instead.
// ===========================================================================

TEST_CASE("compute_redundant_filaments non-contiguous kept subset [2,6,8,10]", "[MixedFilament][redundant_set]")
{
    // Layer 1: pin the upstream deterministic input that the cleanup loop feeds
    // into the composite remap. This function's output is CORRECT for a
    // non-contiguous kept set; it just produces the {9,7,5,4,3,1} descending
    // deletion list that the (buggy) batch remap then misinterprets.
    auto mgr = build_manager(10, {});
    auto red = compute_redundant_filaments(10, {2, 6, 8, 10}, {}, mgr.mixed_filaments());

    REQUIRE(red.redundant_physical.size() == 6);
    // Descending order — cleanup's batched-delete path requires this (and
    // asserts it at runtime, Plater.cpp:8372-8383).
    CHECK(red.redundant_physical[0] == 9);
    CHECK(red.redundant_physical[1] == 7);
    CHECK(red.redundant_physical[2] == 5);
    CHECK(red.redundant_physical[3] == 4);
    CHECK(red.redundant_physical[4] == 3);
    CHECK(red.redundant_physical[5] == 1);
    CHECK(red.new_num_physical == 4);
}

// Helper for the batch-remap layer: build a 10-physical PresetBundle with
// auto-generate disabled (so m_mixed stays empty and the remap under test is
// the PURE physical branch of build_filament_id_remap), snapshot it, and run
// update_mixed_filament_id_remap(old, 10, 4) — the exact call shape
// cleanup_unused_filaments_after_batch_match makes at Plater.cpp:8442 for a
// 4-physical manual selection out of 10.
//
// `kept_physical_ids` (default empty) mirrors the cleanup call site's new
// parameter: empty = original tail-truncation behaviour (backward compat),
// non-empty = kept-aware mapping (the fix for non-contiguous manual subsets).
static std::vector<unsigned int> build_batch_remap_for_kept(size_t num_physical, size_t new_num_physical,
                                                            const std::vector<unsigned int> &kept_physical_ids = {})
{
    MixedAutoGenerateGuard guard(false);
    PresetBundle bundle;
    bundle.filament_presets.assign(num_physical, "Default Filament");
    {
        std::vector<std::string> colours;
        colours.reserve(num_physical);
        for (size_t i = 0; i < num_physical; ++i)
            colours.emplace_back("#FF0000");
        bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = std::move(colours);
    }
    bundle.update_multi_material_filament_presets();

    const std::vector<MixedFilament> old_mixed = bundle.mixed_filaments.mixed_filaments();
    // Direct batch call — mirrors Plater.cpp:8442 (deleting_filament=false),
    // forwarding kept_physical_ids as the cleanup call site now does.
    bundle.update_mixed_filament_id_remap(old_mixed, num_physical, new_num_physical,
                                          size_t(-1), kept_physical_ids);
    return bundle.consume_last_filament_id_remap();
}

TEST_CASE("batch_remap contiguous head [1,2,3,4] keeps identity (recommended-mode parity)", "[MixedFilament][batch_remap]")
{
    // Parity / non-regression guard: when the surviving physical set IS the
    // contiguous head {1..new_num}, the tail-truncation branch is correct and
    // survivors keep identity. This is exactly the recommended-mode situation
    // (set_num_filaments rewrites the palette head-first), so a future fix to
    // the [2,6,8,10] bug must NOT break this case.
    const auto remap = build_batch_remap_for_kept(10, 4);

    // size == old_total + 1 == 10 physical + 0 mixed + 1 (NONE sink at [0]).
    REQUIRE(remap.size() == 11);
    CHECK(remap[0] == 0); // NONE sink, untouched.
    // Surviving head keeps identity.
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 2);
    CHECK(remap[3] == 3);
    CHECK(remap[4] == 4);
    // Truncated tail maps to 0 (NONE) — correct for the contiguous-head case.
    CHECK(remap[5] == 0);
    CHECK(remap[6] == 0);
    CHECK(remap[7] == 0);
    CHECK(remap[8] == 0);
    CHECK(remap[9] == 0);
    CHECK(remap[10] == 0);
}

TEST_CASE("batch_remap non-contiguous kept subset [2,6,8,10] produces tail-truncation (CURRENT bug)", "[MixedFilament][batch_remap]")
{
    // Pins the CURRENT (incorrect) output of build_filament_id_remap's physical
    // branch for a non-contiguous kept subset. The batch path only does
    // tail-truncation when deleting_filament=false (PresetBundle.cpp:3830-3831),
    // so it ignores WHICH physicals survived and maps solely by id <= new_num.
    // For kept={2,6,8,10} -> new_num=4 that yields the table below, which is
    // WRONG (survivors 6/8/10 are lost; deleted 1/3/4 are kept as identity).
    // Kept green so a refactor that changes this surface is caught; the
    // expected-correct oracle lives in the [!shouldfail] sibling below.
    const auto remap = build_batch_remap_for_kept(10, 4);
    REQUIRE(remap.size() == 11);

    // --- DELETED slots wrongly kept as identity (the "wrong colour" half) ---
    CHECK(remap[1] == 1); // C1 deleted, yet maps to id 1 (now C2)
    CHECK(remap[3] == 3); // C3 deleted, yet maps to id 3 (now C8)
    CHECK(remap[4] == 4); // C4 deleted, yet maps to id 4 (now C10)
    // --- SURVIVOR C2 maps to id 2 (now C6) instead of new id 1 ---
    CHECK(remap[2] == 2);
    // --- SURVIVORS C6/C8/C10 (old ids 6/8/10) wrongly truncated to NONE ---
    CHECK(remap[5] == 0); // C5 deleted -> 0 (correct by accident)
    CHECK(remap[6] == 0); // C6 SURVIVED -> 0 (LOST)  <- bug
    CHECK(remap[7] == 0); // C7 deleted -> 0 (correct by accident)
    CHECK(remap[8] == 0); // C8 SURVIVED -> 0 (LOST)  <- bug
    CHECK(remap[9] == 0); // C9 deleted -> 0 (correct by accident)
    CHECK(remap[10] == 0); // C10 SURVIVED -> 0 (LOST) <- bug
}

TEST_CASE("batch_remap non-contiguous kept subset [2,6,8,10] maps survivors correctly when kept is supplied (FIXED)", "[MixedFilament][batch_remap]")
{
    // Kept-aware fix: when the caller supplies kept_physical_ids, the batch
    // remap maps each survivor by its position in the kept set instead of
    // tail-truncation. For [2,6,8,10] -> new ids 1/2/3/4 (sorted survivors):
    //   old id 1 (C1, deleted)   -> 0
    //   old id 2 (C2, survivor)  -> 1
    //   old id 3 (C3, deleted)   -> 0
    //   old id 4 (C4, deleted)   -> 0
    //   old id 5 (C5, deleted)   -> 0
    //   old id 6 (C6, survivor)  -> 2
    //   old id 7 (C7, deleted)   -> 0
    //   old id 8 (C8, survivor)  -> 3
    //   old id 9 (C9, deleted)   -> 0
    //   old id 10 (C10, survivor)-> 4
    // This was the [!shouldfail] oracle for the tail-truncation bug; the
    // kept-aware branch in build_filament_id_remap now makes it pass, so the
    // tag is dropped and this becomes the regression guard for the fix.
    const auto remap = build_batch_remap_for_kept(10, 4, {2, 6, 8, 10});
    REQUIRE(remap.size() == 11);

    CHECK(remap[1] == 0);  // C1 deleted
    CHECK(remap[2] == 1);  // C2 -> new id 1
    CHECK(remap[3] == 0);  // C3 deleted
    CHECK(remap[4] == 0);  // C4 deleted
    CHECK(remap[5] == 0);  // C5 deleted
    CHECK(remap[6] == 2);  // C6 -> new id 2
    CHECK(remap[7] == 0);  // C7 deleted
    CHECK(remap[8] == 3);  // C8 -> new id 3
    CHECK(remap[9] == 0);  // C9 deleted
    CHECK(remap[10] == 4); // C10 -> new id 4
}

// ---------------------------------------------------------------------------
// Kept-set SHAPE matrix for the batch physical branch. The branch
// (PresetBundle.cpp:3824-3834, deleting_filament=false) emits
//   old_id <= new_num -> old_id (identity)
//   old_id >  new_num -> 0
// REGARDLESS of which physicals survived — it only sees `new_num`. So the
// output is correct iff the kept set happens to be {1..new_num} (case A/F) and
// is the SAME bug for every other shape. The three cases below cover the
// distinct FAILURE SIGNATURES:
//   C  {5,6,7,8}  — contiguous but NOT head: every survivor > new_num, so all
//                   survivors are truncated to 0 (LOST) while deleted head ids
//                   1..4 are kept as identity (WRONG COLOUR).
//   D  {1,3,5}    — interspersed: survivors straddle new_num, so id 1 happens
//                   to be right, id 3 is wrongly kept (a deleted id held as
//                   identity), and id 2 (deleted) is wrongly held as 2. Most
//                   insidious shape because PART of the output is coincidentally
//                   correct.
//   F  {1..10}    — keep-all: the only other correct shape besides the
//                   contiguous head. Non-regression guard that a fix must not
//                   break.
// ---------------------------------------------------------------------------

TEST_CASE("batch_remap contiguous-non-head {5,6,7,8} truncates survivors (CURRENT bug)", "[MixedFilament][batch_remap]")
{
    // kept = {5,6,7,8}, new_num = 4. Physical branch output is the SAME as the
    // [2,6,8,10] case — [1,2,3,4,0,0,0,0,0,0] — proving the output does not
    // depend on WHICH ids survived, only on new_num. Here ids 5-8 (survivors)
    // all exceed new_num=4 and are truncated to 0; ids 1-4 (deleted) are held
    // as identity.
    const auto remap = build_batch_remap_for_kept(10, 4);
    REQUIRE(remap.size() == 11);
    // Deleted head wrongly held as identity (would point at C5/C6/C7/C8 after repack).
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 2);
    CHECK(remap[3] == 3);
    CHECK(remap[4] == 4);
    // Survivors 5/6/7/8 all truncated to NONE.
    CHECK(remap[5] == 0);
    CHECK(remap[6] == 0);
    CHECK(remap[7] == 0);
    CHECK(remap[8] == 0);
    // Deleted tail, correct by accident.
    CHECK(remap[9] == 0);
    CHECK(remap[10] == 0);
}

TEST_CASE("batch_remap contiguous-non-head {5,6,7,8} maps survivors correctly when kept is supplied (FIXED)", "[MixedFilament][batch_remap]")
{
    // Kept-aware fix: {5,6,7,8} survivors -> new ids 1/2/3/4.
    const auto remap = build_batch_remap_for_kept(10, 4, {5, 6, 7, 8});
    REQUIRE(remap.size() == 11);
    CHECK(remap[1] == 0); // C1 deleted
    CHECK(remap[2] == 0); // C2 deleted
    CHECK(remap[3] == 0); // C3 deleted
    CHECK(remap[4] == 0); // C4 deleted
    CHECK(remap[5] == 1); // C5 -> new id 1
    CHECK(remap[6] == 2); // C6 -> new id 2
    CHECK(remap[7] == 3); // C7 -> new id 3
    CHECK(remap[8] == 4); // C8 -> new id 4
    CHECK(remap[9] == 0); // C9 deleted
    CHECK(remap[10] == 0); // C10 deleted
}

TEST_CASE("batch_remap interspersed {1,3,5} mixes coincidentally-correct and wrong (CURRENT bug)", "[MixedFilament][batch_remap]")
{
    // kept = {1,3,5}, new_num = 3. Output [1,2,3,0,...]: id 1 is correct by
    // coincidence (kept AND == new_num range); id 3 (kept) is wrongly held as
    // 3 (should shift to 2); id 2 (deleted) is wrongly held as 2 (should be 0).
    const auto remap = build_batch_remap_for_kept(10, 3);
    REQUIRE(remap.size() == 11);
    CHECK(remap[1] == 1);  // C1 survivor, coincidentally correct (new id 1)
    CHECK(remap[2] == 2);  // C2 DELETED, wrongly held as 2 (would be C3 after repack)
    CHECK(remap[3] == 3);  // C3 survivor, wrongly held as 3 (should be new id 2)
    CHECK(remap[4] == 0);  // C4 deleted
    CHECK(remap[5] == 0);  // C5 SURVIVOR, truncated to NONE (should be new id 3)
    CHECK(remap[6] == 0);  // C6 deleted
    CHECK(remap[7] == 0);  // C7 deleted
    CHECK(remap[8] == 0);  // C8 deleted
    CHECK(remap[9] == 0);  // C9 deleted
    CHECK(remap[10] == 0); // C10 deleted
}

TEST_CASE("batch_remap interspersed {1,3,5} maps survivors correctly when kept is supplied (FIXED)", "[MixedFilament][batch_remap]")
{
    // Kept-aware fix: {1,3,5} survivors -> new ids 1/2/3.
    const auto remap = build_batch_remap_for_kept(10, 3, {1, 3, 5});
    REQUIRE(remap.size() == 11);
    CHECK(remap[1] == 1);  // C1 -> new id 1
    CHECK(remap[2] == 0);  // C2 deleted
    CHECK(remap[3] == 2);  // C3 -> new id 2
    CHECK(remap[4] == 0);  // C4 deleted
    CHECK(remap[5] == 3);  // C5 -> new id 3
    CHECK(remap[6] == 0);  // C6 deleted
    CHECK(remap[7] == 0);  // C7 deleted
    CHECK(remap[8] == 0);  // C8 deleted
    CHECK(remap[9] == 0);  // C9 deleted
    CHECK(remap[10] == 0); // C10 deleted
}

TEST_CASE("batch_remap keep-all {1..10} is identity (non-regression)", "[MixedFilament][batch_remap]")
{
    // The other correct shape besides the contiguous head: nothing deleted, so
    // new_num == old_num and every id keeps identity. A fix to the non-contiguous
    // bug must leave this case untouched.
    const auto remap = build_batch_remap_for_kept(10, 10);
    REQUIRE(remap.size() == 11);
    CHECK(remap[0] == 0); // NONE sink
    for (unsigned int i = 1; i <= 10; ++i)
        CHECK(remap[i] == i);
}

// ===========================================================================
// Batch-remap MIXED branch (deleting_filament=false)
//
// In the batch path the mixed branch (PresetBundle.cpp:3856-3918) skips the
// deletion-specific zeroing (3874/3877, both gated on deleting_filament or
// deleted_1based) and the component shift (3894-3898, gated on
// deleting_filament). It relies on EITHER:
//   (a) stable_id match (3884-3892) — new side's mixed_filaments() carries
//       the same stable_id -> old virtual id maps to the new virtual id; OR
//   (b) canonical_pair fallback (3893-3914) — old side's (component_a,component_b)
//       looked up in a map keyed by the NEW side's (component_a,component_b).
//
// In real cleanup (Plater.cpp:8425) the call is made AFTER the delete_filament
// loop, so the bundle's live mixed_filaments() has already been renumbered by
// remove_physical_filament (component_a/b decremented past each deleted id,
// MixedFilament.cpp:1864-1870), while old_mixed passed in is the PRE-deletion
// snapshot. This means:
//   - stable_id path: correct — stable_id is an identity key, renumber-proof.
//   - pair fallback path: the OLD key uses pre-deletion component ids while
//     the NEW map uses post-deletion renumbered ids -> the keys NEVER match
//     for any pair that straddled a deleted id -> fallback returns 0 (NONE),
//     silently dropping the mixed row's painting.
//
// The pair-fallback bug is effectively UNREACHABLE in product flows today:
// every mixed row gets a non-zero stable_id at creation (add_custom_filament)
// and survives serialize/load, so path (a) always fires first. The pair
// fallback only runs for stable_id==0 rows, which cannot exist in a live
// bundle (load_custom_entries re-validates and assigns). This mirrors the m1
// test's "UNREACHABLE but guards the validation perimeter" rationale: if the
// stable_id allocation is ever weakened, this state becomes reachable and
// turns into silent painting loss. Pinned as a known bug.
// ===========================================================================

TEST_CASE("batch_remap mixed stable_id survives non-contiguous physical delete (correct)", "[MixedFilament][batch_remap]")
{
    // 4 physicals, one mixed row {component 1,3, stable_id=S}. Simulate the
    // cleanup sequence for kept={1,3,4} (delete physical 2):
    //   - snapshot old_mixed = [{1,3,S}]
    //   - remove_physical_filament(2) renumbers the live row to {1,2,S}
    //     (component_b 3 -> 2 because 3 > deleted 2; component_a 1 unchanged)
    //   - update_mixed_filament_id_remap(old_mixed, 4, 3)
    // stable_id matches -> old virtual id 5 maps to new virtual id 4. CORRECT.
    MixedAutoGenerateGuard guard(false);
    PresetBundle bundle;
    bundle.filament_presets = {"F1", "F2", "F3", "F4"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values =
        {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    mgr.add_custom_filament(1, 3, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    REQUIRE(row.stable_id != 0);
    const uint64_t sid = row.stable_id;

    // Snapshot BEFORE simulating the physical deletion.
    const std::vector<MixedFilament> old_mixed = mgr.mixed_filaments();

    // Simulate remove_physical_filament(2): renumber the live row's component_b
    // (3 -> 2). We mutate the live bundle directly rather than calling
    // remove_physical_filament so the test isolates the remap-table behaviour
    // from the manager's cascade/erase logic (mirrors how the [shrink] tests
    // call set_num_filaments to isolate the pure remap path).
    mgr.mixed_filaments().back().component_b = 2;

    bundle.update_mixed_filament_id_remap(old_mixed, 4, 3);
    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

    // old_total = 4 physical + 1 mixed = 5, +1 for [0] sink -> size 6.
    REQUIRE(remap.size() == 6);
    // Physical branch (tail-truncation): old ids 1..3 identity, id 4 -> 0.
    // (Physical-branch bug for non-head kept sets is covered by the matrix
    //  above; here we focus on the mixed slot at index 5.)
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 2);
    CHECK(remap[3] == 3);
    CHECK(remap[4] == 0);
    // Mixed virtual id 5 -> 4 via stable_id match (the new side's single row
    // sits at virtual id 4 = new_num 3 + 1). This is the CORRECT outcome.
    const unsigned int new_vid = virtual_id_for_stable_id(mgr.mixed_filaments(), 3, sid);
    REQUIRE(new_vid == 4);
    CHECK(remap[5] == 4);
}

TEST_CASE("batch_remap mixed pair-fallback (stable_id=0) straddles a deleted physical (CURRENT bug)", "[MixedFilament][batch_remap]")
{
    // Same setup as the stable_id test, but the mixed row has stable_id=0,
    // forcing the pair-fallback path. old_mixed key = canonical(1,3); the
    // renumbered live row's key = canonical(1,2). The keys do not match, so
    // the fallback returns 0 (NONE) — the mixed row's painting is silently
    // dropped. UNREACHABLE in product flows (every live row has a non-zero
    // stable_id), but pinned as a boundary guard for the validation perimeter.
    MixedAutoGenerateGuard guard(false);
    PresetBundle bundle;
    bundle.filament_presets = {"F1", "F2", "F3", "F4"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values =
        {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    mgr.add_custom_filament(1, 3, 50, colors);
    // Force the fallback path by zeroing the stable_id (simulates a row that
    // bypassed allocation — cannot exist in a live bundle today).
    mgr.mixed_filaments().back().stable_id = 0;

    const std::vector<MixedFilament> old_mixed = mgr.mixed_filaments();
    // Simulate remove_physical_filament(2): component_b 3 -> 2.
    mgr.mixed_filaments().back().component_b = 2;

    bundle.update_mixed_filament_id_remap(old_mixed, 4, 3);
    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

    REQUIRE(remap.size() == 6);
    // Mixed virtual id 5 -> 0 (NONE): pair key canonical(1,3) not found in the
    // new map keyed by canonical(1,2) because the old side's components are NOT
    // shifted in the batch path (3894 is gated on deleting_filament). This is
    // the bug: painting on this mixed row is dropped.
    CHECK(remap[5] == 0);
}

TEST_CASE("batch_remap mixed pair-fallback (stable_id=0) should match renumbered pair (KNOWN bug)", "[MixedFilament][batch_remap][!shouldfail]")
{
    // Expected-correct oracle for the case above: the pair fallback ought to
    // find the renumbered row. Since old (1,3) and new (1,2) describe the same
    // physical spools after the id-2 deletion, a fallback that applied the same
    // shift the batch path skips (3894-3898) would compute key canonical(1,2)
    // and hit the new map. It currently does not. When the batch mixed branch
    // is taught to honour the actual deletion set (or cleanup stops relying on
    // this path), this test will unexpectedly succeed — drop the tag then.
    MixedAutoGenerateGuard guard(false);
    PresetBundle bundle;
    bundle.filament_presets = {"F1", "F2", "F3", "F4"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values =
        {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    mgr.add_custom_filament(1, 3, 50, colors);
    mgr.mixed_filaments().back().stable_id = 0;

    const std::vector<MixedFilament> old_mixed = mgr.mixed_filaments();
    mgr.mixed_filaments().back().component_b = 2;

    bundle.update_mixed_filament_id_remap(old_mixed, 4, 3);
    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

    REQUIRE(remap.size() == 6);
    // The single live mixed row sits at new virtual id 4 (= new_num 3 + 1).
    CHECK(remap[5] == 4);
}

// ===========================================================================
// Manual-mode painting-loss reproduction (apply "target==src skip" + cleanup
// tail-truncation)
//
// Root-cause chain for the "partial colour corruption" symptom in manual mode
// when the user selects a non-contiguous physical subset like [2,6,8,10]:
//
//   (1) apply_batch_match_to_model (MixedColorMatchHelpers.cpp:1829-1835) builds
//       extruder_remap[src] = target ONLY when target != src. After
//       need_manual_remap the target for a pure recipe pointing at a SELECTED
//       physical (e.g. C6) is the SAME global id as the source (6), so the
//       entry is skipped -> the painting is NOT migrated, it stays on the
//       original global slot (extruder 6).
//   (2) cleanup_unused_filaments_after_batch_match then deletes the unselected
//       physical slots {1,3,4,5,7,9} and builds ONE composite painting remap
//       via build_filament_id_remap(deleting_filament=false) (PresetBundle.cpp:
//       3824-3834), whose physical branch only does tail-truncation
//       (old_id <= new_num -> identity, else -> 0). For survivors packed into
//       low ids {2->1,6->2,8->3,10->4} it maps the ORIGINAL global id 6 -> 0
//       (6 > new_num 4), so the painting still sitting on slot 6 is dropped.
//
// The two tests below reproduce each step with the PURE, test-visible pieces:
//   A. The "target==src skip" rule (inline re-implementation of the
//      extruder_remap build loop) — pure data, no wxGetApp.
//   B. The painting end-state: construct a ModelVolume painted on extruder 6,
//      apply the cleanup batch state_map (tail-truncation), call the pure
//      ModelVolume::remap_extruder_ids, and assert the painting is lost.
//      This uses only libslic3r APIs (Model/TriangleSelector), no wxGetApp.
//
// If either assertion EVER fails to reproduce the loss, the root-cause chain
// above is wrong and must be re-investigated — do NOT relax these oracles.
// ===========================================================================

// Minimal POD mirror of ColorMappingEntry's two fields used by apply's
// extruder_remap build. The real ColorMappingEntry lives in the GUI header
// (MixedColorMatchHelpers.hpp, with wxColour members) which the test binary
// cannot link, so we reproduce only the two fields the build loop reads.
struct ApplyMappingStub {
    std::vector<unsigned int> source_extruder_ids;
    unsigned int              target_filament_id = 0;
};

// Inline re-implementation of apply_batch_match_to_model's extruder_remap build
// (MixedColorMatchHelpers.cpp:1829-1835). Kept byte-faithful to the production
// loop so a change there surfaces here.
static std::unordered_map<int, unsigned int> build_apply_extruder_remap(
    const std::vector<ApplyMappingStub> &mappings)
{
    std::unordered_map<int, unsigned int> extruder_remap;
    for (const auto &mapping : mappings) {
        for (unsigned int src_eid : mapping.source_extruder_ids) {
            if (mapping.target_filament_id != src_eid)
                extruder_remap[static_cast<int>(src_eid)] = mapping.target_filament_id;
        }
    }
    return extruder_remap;
}

TEST_CASE("manual-mode apply skips selected-physical painting (target==src)", "[MixedFilament][batch_apply]")
{
    // Manual subset [2,6,8,10]. A model color painted on extruder 6 (C6, which
    // the user selected) is matched as a pure recipe -> after need_manual_remap
    // target_filament_id == 6 (same global id as the source). apply's
    // extruder_remap build SKIPS it (target==src), so the painting is NOT
    // migrated. Compare with recommended, where the target is a subset id
    // (CMYG 1-4) that differs from the global source -> the painting IS moved.
    ApplyMappingStub manual_pure;
    manual_pure.source_extruder_ids = {6};   // painting on global C6
    manual_pure.target_filament_id  = 6;     // pure recipe -> global C6 after remap
    const auto manual_remap = build_apply_extruder_remap({manual_pure});
    // Manual: painting stays put — NOT in the apply remap table.
    CHECK(manual_remap.find(6) == manual_remap.end());
    CHECK(manual_remap.empty());

    // Recommended: same source 6, but target is a subset id (3) that differs.
    ApplyMappingStub recom_target;
    recom_target.source_extruder_ids = {6};
    recom_target.target_filament_id  = 3;     // CMYG subset id
    const auto recom_remap = build_apply_extruder_remap({recom_target});
    // Recommended: painting IS migrated (6 -> 3).
    REQUIRE(recom_remap.count(6) == 1);
    CHECK(recom_remap.at(6) == 3);
}

TEST_CASE("manual-mode painting on a selected physical is lost after cleanup tail-truncation", "[MixedFilament][batch_apply]")
{
    // Reproduce the consequence of the two-step chain above, using only the
    // pure libslic3r painting API (ModelVolume::remap_extruder_ids). A facet
    // painted on extruder 6 (a selected physical that survived apply unmoved)
    // is then run through cleanup's batch state_map (tail-truncation for
    // new_num=4: old ids 1..4 keep identity, ids > 4 -> 0/NONE), which drops it.
    Model model;
    ModelObject *object = model.add_object();
    object->name = "manual-painting-loss.stl";
    ModelVolume *volume = object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();

    // Paint facet 0 on extruder 6 (simulating C6, a user-selected physical).
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType(6));
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    // Sanity: facet 0 is painted on extruder 6 before remap.
    REQUIRE(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(6)));

    // cleanup's batch state_map: tail-truncation for new_num_physical=4.
    // Mirrors build_filament_id_remap(deleting_filament=false) at
    // PresetBundle.cpp:3824-3834 (old_id <= new_num -> identity, else -> 0),
    // then Plater.cpp:8429-8437 (mapped==0 -> NONE).
    EnforcerBlockerStateMap state_map;
    for (size_t i = 0; i < state_map.size(); ++i)
        state_map[i] = EnforcerBlockerType(i);
    constexpr size_t new_num_physical = 4;
    for (size_t i = 1; i < state_map.size(); ++i)
        if (i > new_num_physical)
            state_map[i] = EnforcerBlockerType::NONE;

    // Apply the composite remap exactly as cleanup does (Plater.cpp:8445).
    // total_filaments = new_num_physical + 0 mixed (no mixed in this scenario).
    volume->remap_extruder_ids(new_num_physical, state_map);

    // CURRENT (buggy) outcome: the painting on extruder 6 is LOST.
    // build_filament_id_remap's tail-truncation maps old id 6 -> NONE (6 >
    // new_num 4), and FacetsAnnotation::deserialize treats NONE as "unpainted",
    // so the facet data is dropped entirely (mmu_segmentation_facets becomes
    // empty). The correct outcome would be that the painting follows its
    // physical (C6 -> new survivor slot 2), but the batch remap has no notion
    // of which physicals survived — it only knows new_num, not the kept set.
    CHECK(volume->mmu_segmentation_facets.empty());                                       // painting data dropped (the bug)
    CHECK(!volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(2)));  // NOT remapped to survivor slot 2
}

TEST_CASE("manual-mode painting on a selected physical survives with kept-aware state_map (FIXED)", "[MixedFilament][batch_apply]")
{
    // The kept-aware fix's end-to-end painting evidence: with the state_map the
    // fix produces (6 -> 2, the survivor's new slot, instead of 6 -> NONE),
    // the painting on extruder 6 is PRESERVED on survivor slot 2. This pairs
    // with the bug-reproduction test above (which used the old tail-truncation
    // state_map and showed the painting lost) to give before/after evidence
    // per the fix-verification harness. The state_map here mirrors what
    // build_filament_id_remap now emits for kept_physical_ids={2,6,8,10}
    // (see the "batch_remap ... FIXED" tests for the remap table itself).
    Model model;
    ModelObject *object = model.add_object();
    object->name = "manual-painting-survives.stl";
    ModelVolume *volume = object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();

    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType(6)); // painting on selected C6
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    REQUIRE(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(6)));

    // kept-aware state_map: survivors map to their new packed slots.
    // For kept={2,6,8,10}: 2->1, 6->2, 8->3, 10->4; others (incl. 1,3,4,5,7,9) -> NONE.
    EnforcerBlockerStateMap state_map;
    for (size_t i = 0; i < state_map.size(); ++i)
        state_map[i] = EnforcerBlockerType::NONE;
    state_map[2]  = EnforcerBlockerType(1);
    state_map[6]  = EnforcerBlockerType(2);
    state_map[8]  = EnforcerBlockerType(3);
    state_map[10] = EnforcerBlockerType(4);

    constexpr size_t new_num_physical = 4;
    volume->remap_extruder_ids(new_num_physical, state_map);

    // FIXED outcome: painting migrated from old slot 6 to new survivor slot 2.
    CHECK(!volume->mmu_segmentation_facets.empty());                                      // painting preserved
    CHECK(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(2)));   // now on survivor slot 2
    CHECK(!volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(6)));  // gone from old slot 6
}

// ===========================================================================
// Modifier & layer-config remap (apply_batch_match_to_model Level-2 + Level-3)
//
// apply_batch_match_to_model lives in the GUI layer (depends on wxGetApp()) so
// the test binary cannot link it. These tests mirror its Level-2 (volume/object
// config extruder) and Level-3 (layer_config_ranges) passes over the REAL
// libslic3r Model API — the same approach the manual-mode tests above use for
// the triangle-painting pass. Keep the mirror byte-faithful to the production
// loop (MixedColorMatchHelpers.cpp); a change there must surface here.
//
// Level-2 reads the object's extruder ONCE before iterating volumes (a
// snapshot). ModelVolume::extruder_id() otherwise falls back to the OBJECT
// config, which this loop rewrites mid-iteration — so re-reading it for a later
// inheriting volume would resolve a different id than the first, making two
// volumes that share one source diverge. The first test below pins that real
// libslic3r contract; the rest pin the apply end-state.
// ===========================================================================

// Mirror of apply_batch_match_to_model's Level-2 (config) + Level-3 (layer)
// passes (MixedColorMatchHelpers.cpp). Covers ONLY those two passes — the
// Level-1 triangle-painting pass is exercised by the manual-mode tests above.
static void apply_config_layer_remap_mirror(
    Model&                                        model,
    const std::unordered_map<int, unsigned int>&  extruder_remap)
{
    for (ModelObject* mo : model.objects) {
        const ConfigOption* obj_opt      = mo->config.option("extruder");
        const int           orig_obj_eid = (obj_opt ? obj_opt->getInt() : 0);
        auto                obj_it       = extruder_remap.find(orig_obj_eid);
        const bool          obj_remap    = (orig_obj_eid > 0 && obj_it != extruder_remap.end());
        bool                object_extruder_written = false;
        for (ModelVolume* mv : mo->volumes) {
            const ModelVolumeType vt = mv->type();
            const bool is_part      = (vt == ModelVolumeType::MODEL_PART);
            const bool is_modifier  = (vt == ModelVolumeType::PARAMETER_MODIFIER);
            if (!is_part && !is_modifier) continue;

            const ConfigOption* vol_opt = mv->config.option("extruder");
            if (vol_opt && vol_opt->getInt() > 0) {
                auto it = extruder_remap.find(vol_opt->getInt());
                if (it != extruder_remap.end())
                    mv->config.set_key_value("extruder", new ConfigOptionInt(static_cast<int>(it->second)));
            } else if (obj_remap && !object_extruder_written) {
                mo->config.set_key_value("extruder", new ConfigOptionInt(static_cast<int>(obj_it->second)));
                object_extruder_written = true;
            }
        }

        for (auto& lr : mo->layer_config_ranges) {
            ModelConfig&         lcfg = lr.second;
            const ConfigOption*  lopt = lcfg.option("extruder");
            if (!lopt) continue;
            const int old_eid = lopt->getInt();
            if (old_eid <= 0) continue;
            auto lit = extruder_remap.find(old_eid);
            if (lit == extruder_remap.end()) continue;
            lcfg.set_key_value("extruder", new ConfigOptionInt(static_cast<int>(lit->second)));
        }
    }
}

TEST_CASE("ModelVolume::extruder_id falls back to live object config (snapshot rationale)", "[MixedFilament][batch_apply]")
{
    // Pins the libslic3r contract that makes apply_batch_match_to_model pre-read
    // the object extruder: a volume with no own "extruder" resolves
    // extruder_id() through the OBJECT config, and that resolution is LIVE —
    // rewriting the object config changes what a later inheriting volume
    // resolves. Without a pre-loop snapshot, two inheriting volumes that share
    // one source would resolve different targets once the loop writes the object
    // config mid-iteration. Pure libslic3r — no mirror.
    Model model;
    ModelObject *obj  = model.add_object();
    ModelVolume  *part = obj->add_volume(make_cube(20., 20., 20.));
    ModelVolume  *mod  = obj->add_volume(make_cube(20., 20., 20.), ModelVolumeType::PARAMETER_MODIFIER);
    obj->config.set_key_value("extruder", new ConfigOptionInt(5));

    // Neither volume owns an extruder → both inherit the object's 5.
    REQUIRE(part->extruder_id() == 5);
    REQUIRE(mod->extruder_id()  == 5);

    // Simulate the apply loop writing the object extruder for the FIRST
    // inheriting volume (5 -> 7). The second inheriting volume now resolves 7
    // — proving extruder_id() reads the CURRENT object config, not a snapshot.
    obj->config.set_key_value("extruder", new ConfigOptionInt(7));
    REQUIRE(part->extruder_id() == 7);
    REQUIRE(mod->extruder_id()  == 7);
}

TEST_CASE("apply config remap: inheriting modifier follows object consistently", "[MixedFilament][batch_apply]")
{
    // The modifier-inclusion fix's core contract: a modifier that inherits the
    // object's extruder must follow the SAME target as the part that inherits
    // it, so their colours stay the same hue. remap {5 -> 7}; both volumes
    // inherit object=5, so both must end on 7 — no divergence, regardless of
    // mo->volumes ordering.
    Model model;
    ModelObject *obj  = model.add_object();
    ModelVolume  *part = obj->add_volume(make_cube(20., 20., 20.));
    ModelVolume  *mod  = obj->add_volume(make_cube(20., 20., 20.), ModelVolumeType::PARAMETER_MODIFIER);
    obj->config.set_key_value("extruder", new ConfigOptionInt(5));

    std::unordered_map<int, unsigned int> remap{{5, 7u}};
    apply_config_layer_remap_mirror(model, remap);

    // Object written once to 7; both inheriting volumes resolve 7.
    REQUIRE(obj->config.opt_int("extruder") == 7);
    REQUIRE(part->extruder_id() == 7);
    REQUIRE(mod->extruder_id()  == 7);
    // Neither volume gained its own config entry — they still inherit.
    REQUIRE_FALSE(part->config.has("extruder"));
    REQUIRE_FALSE(mod->config.has("extruder"));
}

TEST_CASE("apply config remap: modifier with own extruder is remapped alone", "[MixedFilament][batch_apply]")
{
    // A modifier that carries its own extruder (user picked a colour in the
    // object list) is remapped on its OWN config only; the object and any other
    // volume are untouched.
    Model model;
    ModelObject *obj  = model.add_object();
    ModelVolume  *part = obj->add_volume(make_cube(20., 20., 20.));
    ModelVolume  *mod  = obj->add_volume(make_cube(20., 20., 20.), ModelVolumeType::PARAMETER_MODIFIER);
    obj->config.set_key_value("extruder",  new ConfigOptionInt(1)); // object default (not remapped)
    part->config.set_key_value("extruder", new ConfigOptionInt(3));
    mod->config.set_key_value("extruder",  new ConfigOptionInt(4));

    std::unordered_map<int, unsigned int> remap{{3, 7u}, {4, 8u}};
    apply_config_layer_remap_mirror(model, remap);

    REQUIRE(part->extruder_id() == 7);
    REQUIRE(mod->extruder_id()  == 8);
    REQUIRE(obj->config.opt_int("extruder") == 1); // object untouched
}

TEST_CASE("apply layer remap: hit is remapped, miss is left untouched", "[MixedFilament][batch_apply]")
{
    // Level-3 (layer_config_ranges): a height range whose extruder is in the
    // remap follows the match; one whose extruder is NOT in the remap stays put
    // (so a layer on an unchanged slot is not reset to default by cleanup).
    Model model;
    ModelObject *obj = model.add_object();
    obj->add_volume(make_cube(20., 20., 20.));
    obj->layer_config_ranges[t_layer_height_range{0.0, 1.0}].set_key_value("extruder", new ConfigOptionInt(5));
    obj->layer_config_ranges[t_layer_height_range{1.0, 2.0}].set_key_value("extruder", new ConfigOptionInt(9));

    std::unordered_map<int, unsigned int> remap{{5, 7u}}; // 9 absent → miss
    apply_config_layer_remap_mirror(model, remap);

    REQUIRE(obj->layer_config_ranges.at(t_layer_height_range{0.0, 1.0}).opt_int("extruder") == 7);
    REQUIRE(obj->layer_config_ranges.at(t_layer_height_range{1.0, 2.0}).opt_int("extruder") == 9);
}

TEST_CASE("manual-mode apply migrates unselected-physical painting off its slot", "[MixedFilament][batch_apply]")
{
    // Validates the precondition for the kept-aware cleanup fix (fix-verification
    // side-effects item). The fix maps unselected physical ids -> 0 (NONE) in
    // cleanup's batch state_map. That is only safe if apply has ALREADY moved
    // the painting off those ids — otherwise mapping to 0 drops residual data.
    //
    // Setup mirrors manual subset [2,6,8,10]: facet A painted on extruder 6
    // (SELECTED, apply skips it — target==src), facet B painted on extruder 3
    // (UNSELECTED, matched to C6 so target=6 != src=3, apply migrates it).
    // After apply's state_map (6->6 identity, 3->6 migrate), extruder 3 must be
    // EMPTY (its painting moved to 6). This is what makes kept-aware mapping
    // 3 -> 0 safe: nothing is left on 3 to lose.
    Model model;
    ModelObject *object = model.add_object();
    object->name = "apply-migrate-unselected.stl";
    ModelVolume *volume = object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();

    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType(6)); // facet A: selected physical C6
    selector.set_facet(1, EnforcerBlockerType(3)); // facet B: unselected physical C3
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    REQUIRE(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(6)));
    REQUIRE(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(3)));

    // apply_batch_match_to_model's state_map for this scenario:
    //   C6 selected, pure recipe, target==src -> identity (6->6)
    //   C3 unselected, matched to C6, target=6 != src=3 -> migrate (3->6)
    // (Mirrors MixedColorMatchHelpers.cpp:1849-1861 state_map construction.)
    EnforcerBlockerStateMap apply_state_map;
    for (size_t i = 0; i < apply_state_map.size(); ++i)
        apply_state_map[i] = EnforcerBlockerType(i);
    apply_state_map[3] = EnforcerBlockerType(6); // C3 painting migrated to C6
    // total_filaments: pre-cleanup palette still has all 10 physicals here.
    constexpr size_t pre_cleanup_total = 10;
    volume->remap_extruder_ids(pre_cleanup_total, apply_state_map);

    // After apply: C3 is EMPTY (its painting moved to 6). C6 holds both facets.
    CHECK(!volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(3))); // migrated away
    CHECK(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(6)));  // both facets now on 6

    // THEN cleanup's kept-aware state_map is safe to map 3 -> 0 (nothing left on
    // 3 to drop) and 6 -> 2 (survivor's new slot). This is the fix's guarantee.
}

// ===========================================================================
// rebuild_match_thumb_cache color-substitution rule (the "After Match" preview).
//
// Production function: MixedFilamentBatchDialog::rebuild_match_thumb_cache
// (MixedFilamentBatchDialog.cpp:567-601). It builds the preview color vector
// m_match_colors that the After-Match thumbnail renders against, by applying
// each match mapping onto a base vector of [physical filament_colour ...][mixed
// display_color ...] indexed by extruder_id-1.
//
// Why this is pinned separately from apply_batch_match_to_model:
//   The preview runs BEFORE the model is modified (no confirm yet), so the
//   render pipeline still indexes m_match_colors by the ORIGINAL extruder id
//   (GLVolume::simple_render, 3DScene.cpp:573 reads extruder_colors[idx-1]
//   for painted facets; render_match_thumb_for_plate:636 sets
//   vol->color = m_match_colors[vol->extruder_id-1] for unpainted geometry).
//   Therefore the preview must substitute the SOURCE slots with the matched
//   color so that, visually, it matches what apply_batch_match_to_model
//   (MixedColorMatchHelpers.cpp:1829-1835, src->target id remap) produces once
//   confirmed. The bug this pins: the production loop used to match slots by
//   wxColour == source_color and `break` on the first hit, so when two extruder
//   slots share one source color (e.g. two cubes both painted red but on
//   different extruder ids) only the first slot was substituted -> the second
//   cube rendered as the original model. The fix iterates ALL
//   source_extruder_ids per mapping with no break.
//
// Same POD-stub discipline as the [batch_apply] block above (the real
// ColorMappingEntry lives in the GUI header with wxColour members the test
// binary cannot link), byte-faithful to the fixed production loop so a
// regression surfaces here. Colors use packed RGB (0xRRGGBB) to avoid wx.
// ===========================================================================

struct PreviewMappingStub {
    std::vector<unsigned int> source_extruder_ids; // mirrors ColorMappingEntry::source_extruder_ids
    unsigned int              target_filament_id = 0;
    uint32_t                  matched_rgb = 0;      // packed RGB (mirrors matched_color)
};

// Inline re-implementation of rebuild_match_thumb_cache's color-substitution
// loop (MixedFilamentBatchDialog.cpp:567-601), FIXED rule. For each mapping,
// substitute EVERY source extruder slot (index = src-1) with the matched color,
// no break. An empty source_extruder_ids is a natural no-op (the loop body never
// runs); a defensive target==0 skip is kept but is a dead branch in practice —
// assign_batch_virtual_filament_ids (MixedColorMatchHelpers.cpp:1510-1524)
// always assigns a non-zero target. Kept byte-faithful to the (fixed) production
// loop so a regression surfaces here.
static std::vector<uint32_t> build_match_preview_colors(
    const std::vector<uint32_t>&           base_colors, // initial [physical...][virtual...], index=extruder_id-1
    const std::vector<PreviewMappingStub>& mappings)
{
    std::vector<uint32_t> out = base_colors;
    for (const auto& mapping : mappings) {
        if (mapping.target_filament_id == 0) continue; // dead branch (target always non-zero); kept defensively
        for (unsigned int src_eid : mapping.source_extruder_ids) {
            if (src_eid == 0) continue;
            const size_t idx = static_cast<size_t>(src_eid - 1);
            if (idx >= out.size()) out.resize(idx + 1, 0x80808080u); // pad gray (ensure_slot)
            out[idx] = mapping.matched_rgb;
        }
    }
    return out;
}

TEST_CASE("preview colors: same color on multiple extruder slots all get substituted", "[MixedFilament][batch_preview]")
{
    // Root-cause reproduction for the reported "two cubes, one rendered matched,
    // one rendered as the original model" bug. Two cubes are both painted red,
    // but red occupies extruder slots 2 and 5 (different extruder ids sharing one
    // source color). The match maps red -> purple and carries
    // source_extruder_ids = {2, 5}. The preview MUST substitute BOTH source
    // slots; the old production loop matched by wxColour == source_color and
    // `break`-ed on the first hit (slot 2), leaving slot 5 as red -> cube B kept
    // its original color. (MixedFilamentBatchDialog.cpp:567-601.)
    constexpr uint32_t RED    = 0xFF0000;
    constexpr uint32_t PURPLE = 0x800080;
    constexpr uint32_t OTHER  = 0x123456; // unrelated slot color, must be untouched
    // base_colors indexed by extruder_id-1: slot1=OTHER, slot2=RED, slot3=OTHER,
    // slot4=OTHER, slot5=RED.
    const std::vector<uint32_t> base = {OTHER, RED, OTHER, OTHER, RED};

    PreviewMappingStub m;
    m.source_extruder_ids = {2, 5}; // both cubes' red
    m.target_filament_id  = 6;      // virtual slot for the purple mix
    m.matched_rgb         = PURPLE;

    const auto out = build_match_preview_colors(base, {m});

    // BOTH source slots substituted (the fix). Old code would leave out[4]==RED.
    REQUIRE(out.size() >= 5);
    CHECK(out[1] == PURPLE); // slot 2 (cube A) -> matched
    CHECK(out[4] == PURPLE); // slot 5 (cube B) -> matched (was the bug: stayed RED)
    // Unrelated slots untouched.
    CHECK(out[0] == OTHER);
    CHECK(out[2] == OTHER);
    CHECK(out[3] == OTHER);
}

TEST_CASE("preview colors: single source extruder substitutes exactly one slot", "[MixedFilament][batch_preview]")
{
    // Regression baseline: the common single-extruder case substitutes exactly
    // one slot and leaves everything else untouched. Guards against an over-broad
    // substitution fix that would repaint unrelated slots.
    constexpr uint32_t RED  = 0xFF0000;
    constexpr uint32_t BLUE = 0x0000FF;
    constexpr uint32_t GRN  = 0x00FF00;
    const std::vector<uint32_t> base = {RED, BLUE, GRN};

    PreviewMappingStub m;
    m.source_extruder_ids = {1};
    m.target_filament_id  = 4;
    m.matched_rgb         = 0x111111;

    const auto out = build_match_preview_colors(base, {m});
    REQUIRE(out.size() == 3);
    CHECK(out[0] == 0x111111); // slot 1 substituted
    CHECK(out[1] == BLUE);     // untouched
    CHECK(out[2] == GRN);      // untouched
}

TEST_CASE("preview colors: empty source_extruder_ids substitutes nothing", "[MixedFilament][batch_preview]")
{
    // A mapping with empty source_extruder_ids must leave every slot untouched
    // (the loop body never runs). This is the real no-op condition the loop
    // relies on: assign_batch_virtual_filament_ids always assigns a non-zero
    // target_filament_id, so the target==0 skip is a dead branch in practice and
    // cannot guard an empty-source mapping. Guards against a regression that
    // would substitute a stale matched_rgb onto an unrelated slot when the source
    // list is empty (defensive: shouldn't happen in normal match output).
    constexpr uint32_t RED = 0xFF0000;
    const std::vector<uint32_t> base = {RED, 0x00FF00};

    PreviewMappingStub m;
    m.source_extruder_ids = {};      // empty -> no-op
    m.target_filament_id  = 4;       // non-zero (the normal case)
    m.matched_rgb         = 0x222222;

    const auto out = build_match_preview_colors(base, {m});
    REQUIRE(out.size() == 2);
    CHECK(out[0] == RED);       // untouched
    CHECK(out[1] == 0x00FF00);  // untouched
}

TEST_CASE("preview colors: virtual slot beyond base vector is padded gray", "[MixedFilament][batch_preview]")
{
    // A source extruder id pointing past the base vector (a mixed-filament
    // virtual slot whose display_color isn't in the initial m_match_colors) must
    // resize-and-pad so the render pipeline's extruder_colors[idx-1] read stays
    // in range. Mirrors the ensure_slot resize in the production loop.
    const std::vector<uint32_t> base = {0xFF0000}; // only 1 physical slot

    PreviewMappingStub m;
    m.source_extruder_ids = {4}; // virtual slot 4, beyond base size
    m.target_filament_id  = 4;
    m.matched_rgb         = 0x333333;

    const auto out = build_match_preview_colors(base, {m});
    REQUIRE(out.size() == 4);              // grown to hold index 3
    CHECK(out[0] == 0xFF0000);             // existing slot untouched
    CHECK(out[3] == 0x333333);             // new slot substituted
    // Padded holes (indices 1,2) are the gray fill, not 0.
    CHECK(out[1] == 0x80808080u);
    CHECK(out[2] == 0x80808080u);
}

// ============================================================================
// [MixedFilament][deletion_remap] — build_mixed_deletion_painting_remap
//
// Regression coverage for the batch-match cleanup bug where deleting redundant
// mixed rows (after duplicate-recipe merge) re-enumerates the virtual IDs of
// the remaining rows, but the model's painted facets kept the old (pre-deletion)
// IDs, so they resolved to the wrong mixed filament — the last merged slot
// appeared to fall back to color #1.
//
// The fix extracts the T2(pre-delete)→T3(post-delete) painting remap into a
// pure, library-testable function. These are SPEC tests (the math is defined
// independently of any runtime), not characterization tests, so a hand-written
// expected value is a valid oracle here (differential-oracle harness §5).
//
// Mapping rule under test, for each old_vid in T2 space:
//   old_vid ∈ deleted_vids               -> 0   (NONE; the row itself)
//   old_vid <= num_physical              -> old_vid (physical slots are identity)
//   else                                 -> old_vid - count(deleted_vids < old_vid)
// Return vector is sized t2_total_filaments + 1 (1-based; index 0 is unused/0).
// ===========================================================================

TEST_CASE("build_mixed_deletion_painting_remap: single middle delete shifts tail down by one", "[MixedFilament][deletion_remap]")
{
    // The reported scenario: 4 physicals, mixed rows v5..v14. After duplicate-
    // recipe merge, v9 is redundant and gets deleted. Every vid > 9 must shift
    // down by one; v9 itself maps to NONE.
    const size_t num_physical = 4;
    const size_t t2_total     = 14; // 4 physical + 10 mixed (v5..v14)
    const std::vector<unsigned int> deleted = {9};

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[0] == 0u); // unused index
    // Physical slots: identity.
    CHECK(remap[1] == 1u);
    CHECK(remap[2] == 2u);
    CHECK(remap[3] == 3u);
    CHECK(remap[4] == 4u);
    // Mixed slots before the deleted one: identity.
    CHECK(remap[5] == 5u);
    CHECK(remap[6] == 6u);
    CHECK(remap[7] == 7u);
    CHECK(remap[8] == 8u);
    // Deleted slot itself -> NONE.
    CHECK(remap[9] == 0u);
    // Mixed slots after the deleted one: shift down by one.
    CHECK(remap[10] == 9u);
    CHECK(remap[11] == 10u);
    CHECK(remap[12] == 11u);
    CHECK(remap[13] == 12u);
    CHECK(remap[14] == 13u);
}

TEST_CASE("build_mixed_deletion_painting_remap: multiple deletes accumulate offset", "[MixedFilament][deletion_remap]")
{
    // Delete v7 and v9 out of v5..v14. Each survivor's new id subtracts the
    // number of deleted vids strictly less than it.
    const size_t num_physical = 4;
    const size_t t2_total     = 14;
    const std::vector<unsigned int> deleted = {7, 9};

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[0] == 0u);
    // Physical + pre-first-delete mixed: identity.
    CHECK(remap[5] == 5u);
    CHECK(remap[6] == 6u);
    // Deleted.
    CHECK(remap[7] == 0u);
    // v8: one deleted vid (7) below it -> 8-1 = 7.
    CHECK(remap[8] == 7u);
    // Deleted.
    CHECK(remap[9] == 0u);
    // v10..v14: two deleted vids (7,9) below each -> subtract 2.
    CHECK(remap[10] == 8u);
    CHECK(remap[11] == 9u);
    CHECK(remap[12] == 10u);
    CHECK(remap[13] == 11u);
    CHECK(remap[14] == 12u);
}

TEST_CASE("build_mixed_deletion_painting_remap: physical slots never move", "[MixedFilament][deletion_remap]")
{
    // Even with mixed deletes, the physical range [1..num_physical] is identity.
    const size_t num_physical = 4;
    const size_t t2_total     = 8;
    const std::vector<unsigned int> deleted = {5, 6};

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[1] == 1u);
    CHECK(remap[2] == 2u);
    CHECK(remap[3] == 3u);
    CHECK(remap[4] == 4u);
}

TEST_CASE("build_mixed_deletion_painting_remap: deleted vids map to NONE", "[MixedFilament][deletion_remap]")
{
    const size_t num_physical = 4;
    const size_t t2_total     = 10;
    const std::vector<unsigned int> deleted = {6, 8};

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    CHECK(remap[6] == 0u);
    CHECK(remap[8] == 0u);
}

TEST_CASE("build_mixed_deletion_painting_remap: delete all mixed leaves only physicals", "[MixedFilament][deletion_remap]")
{
    // Deleting every mixed row: the survivors are the physicals alone, all
    // unchanged; every mixed vid maps to NONE.
    const size_t num_physical = 4;
    const size_t t2_total     = 7; // 4 physical + 3 mixed (v5,v6,v7)
    const std::vector<unsigned int> deleted = {5, 6, 7};

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[1] == 1u);
    CHECK(remap[2] == 2u);
    CHECK(remap[3] == 3u);
    CHECK(remap[4] == 4u);
    CHECK(remap[5] == 0u);
    CHECK(remap[6] == 0u);
    CHECK(remap[7] == 0u);
}

TEST_CASE("build_mixed_deletion_painting_remap: empty delete list is identity (short-circuit)", "[MixedFilament][deletion_remap]")
{
    // No deletes -> every vid maps to itself. This is the no-op path cleanup
    // must take without running a remap pass over the whole model.
    const size_t num_physical = 4;
    const size_t t2_total     = 10;
    const std::vector<unsigned int> deleted;

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    for (size_t i = 1; i <= t2_total; ++i)
        CHECK(remap[i] == static_cast<unsigned int>(i));
}

TEST_CASE("build_mixed_deletion_painting_remap: unsorted delete input is tolerated", "[MixedFilament][deletion_remap]")
{
    // Robustness: caller may supply vids in any order; the function must sort
    // internally so the offset count is correct. Same expectation as the
    // ordered {7,9} case.
    const size_t num_physical = 4;
    const size_t t2_total     = 14;
    const std::vector<unsigned int> deleted = {9, 7}; // descending input

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[7] == 0u);
    CHECK(remap[8] == 7u); // one delete below
    CHECK(remap[9] == 0u);
    CHECK(remap[10] == 8u); // two deletes below
    CHECK(remap[14] == 12u);
}

TEST_CASE("build_mixed_deletion_painting_remap: duplicate ids in delete list dedupe (offset not inflated)", "[MixedFilament][deletion_remap]")
{
    // Robustness: a caller that collects the same vid twice (e.g. from
    // overlapping reference scans) must not double-count it — the function
    // sorts + uniques internally, so {9,9,7} is equivalent to {7,9}.
    // The batch-match cleanup reuses this table to drive both painting and
    // config-level extruder remaps, so an inflated offset would shift every
    // survivor below the duplicate onto the wrong row.
    const size_t num_physical = 4;
    const size_t t2_total     = 14;
    const std::vector<unsigned int> deleted = {9, 9, 7}; // duplicate 9

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[7] == 0u);
    CHECK(remap[8] == 7u); // one delete below (7 only; 9 is above)
    CHECK(remap[9] == 0u);
    CHECK(remap[10] == 8u); // two deletes below (7, 9) — NOT three
    CHECK(remap[14] == 12u);
}

// ============================================================================
// [MixedFilament][config_extruder_remap] — cascade gap: config "extruder"
// references are adjusted per-deletion (naive), while the virtual-ID space also
// contracts by the cascade-removed mixed rows. This test pins the CORRECT
// cascade-aware result and currently FAILS (CURRENT BUG) — it reproduces the
// batch-match physical-deletion cascade under-shift in pure libslic3r terms.
//
// Scenario: 4 physicals {1,2,3,4}, mixed rows A(1,2)=v5, B(2,3)=v6, C(3,4)=v7.
// Batch match keeps {1,3,4} → physical 2 is deleted → A and B cascade-removed
// (both reference physical 2); C survives and its recipe (3,4) renumbers to
// (2,3). An object pinned to C holds config "extruder" = 7.
//
//   CORRECT: C's new virtual id = new_num_physical(3) + position(1) = 4.
//   ACTUAL : GUI_ObjectList.cpp:857-969 (driven per deletion by
//            Plater::on_filaments_delete, Plater.cpp:21253) subtracts exactly 1
//            per deleted physical ("if extruder > deleted_id then -1") → 7 → 6.
//            The cascade-removed rows ahead of C are never accounted for, so
//            the config lands on a renumbered survivor (6) or — when out of
//            range — is reset to default by update_objects_list_filament_column
//            (GUI_ObjectList.cpp:694-700). The painting path IS cascade-aware
//            (kept-aware composite remap, PresetBundle.cpp:3850-3854); the
//            config path is not.
//
//   HIDDEN ([.]): this test intentionally FAILS (6 != 4). The `actual` side is
//   a hand-rolled simulation of the naive decrement INSIDE the test, not a call
//   into production — so fixing the production cascade config remap will NOT
//   flip this test green, and [!shouldfail] would never fire its "unexpectedly
//   succeeded" signal. It is documentation, not a regression sentinel.
//
//   PRE-EXISTING: the naive per-deletion config decrement (GUI_ObjectList.cpp:
//   857-969) predates this PR; the cascade-aware config remap is tracked as a
//   follow-up (see Plater.cpp remap_config_extruder — it currently skips
//   out-of-range config references silently). When the follow-up lands, rewrite
//   the `actual` side to assert the production result == 4 and remove the [.]
//   tag.
// ============================================================================
TEST_CASE("config_extruder cascade: per-deletion decrement under-counts cascade rows (CURRENT BUG)",
          "[MixedFilament][config_extruder_remap][.]")
{
    // --- Correct side: real libslic3r cascade + production kept-aware remap ---
    MixedAutoGenerateGuard guard(false); // keep add_custom_filament from auto-generating gradient rows
    PresetBundle bundle;
    bundle.filament_presets = {"F1", "F2", "F3", "F4"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values =
        {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    bundle.update_multi_material_filament_presets();
    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    mgr.add_custom_filament(1, 2, 50, colors); // A → v5
    mgr.add_custom_filament(2, 3, 50, colors); // B → v6
    mgr.add_custom_filament(3, 4, 50, colors); // C → v7
    REQUIRE(mgr.enabled_count() == 3);

    const std::vector<MixedFilament> old_mixed = mgr.mixed_filaments(); // T2 snapshot

    // Delete physical 2 (kept {1,3,4} → new_num_physical 3). Real cascade.
    mgr.remove_physical_filament(2);
    REQUIRE(mgr.enabled_count() == 1); // A, B cascaded away; C survives

    // Production kept-aware remap — the oracle the PAINTING path trusts:
    // old vid 7 (C) must map to 4.
    bundle.update_mixed_filament_id_remap(old_mixed, 4, 3, size_t(-1), {1, 3, 4});
    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() > 7);
    CHECK(remap[7] == 4u); // oracle sanity: C → 4

    // --- Actual side: GUI_ObjectList naive decrement for the single deletion ---
    const int config_ref_before = 7; // object pinned to C (old vid)
    int actual = config_ref_before;
    if (actual == 2)
        actual = -1; // == deleted id → replace (batch cleanup passes -1)
    else if (actual > 2)
        actual -= 1; // per-deletion "> deleted_id → -1"

    // The naive adjustment must land on the row the config actually points at.
    // It does not (6 != 4) — the config keeps a stale virtual id. This CHECK
    // FAILS until the config path is made cascade-aware (currently only
    // painting gets the kept-aware composite remap).
    CHECK(actual == static_cast<int>(remap[7]));
}

// ============================================================================
// [MixedFilament][FilamentColor] — dual-color (multi-colour) slot primary
// derivation. The batch-match dialog derives a dual-color slot's effective
// match colour as the first valid token of filament_multi_colors (the app-wide
// PrimaryColor rule used by PresetBundle's sync), falling back to the raw
// filament_colour value when no valid token exists. These cases pin the
// libslic3r primitives the dialog's slot_match_color wrapper relies on.
// ============================================================================
TEST_CASE("Dual-color multi string primary is the first valid colour", "[MixedFilament][FilamentColor]")
{
    const auto parts = SplitFilamentMultiColors("#AABBCC|#112233");
    REQUIRE(parts.size() == 2);
    CHECK(FilamentColor::FromColors(parts, FilamentColorMode::Segment).PrimaryColor("#26A69A") == "#AABBCC");
}

TEST_CASE("Dual-color primary drops invalid tokens and falls back on empty", "[MixedFilament][FilamentColor]")
{
    const auto parts = SplitFilamentMultiColors("#AABBCC|not-a-color|#112233");
    REQUIRE(parts.size() == 2); // invalid token whitelisted away
    CHECK(FilamentColor::FromColors(parts, FilamentColorMode::Segment).PrimaryColor() == "#AABBCC");
    CHECK(FilamentColor::FromColors({}, FilamentColorMode::Segment).PrimaryColor("#26A69A") == "#26A69A");
}
