#include <catch2/catch.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"

#include <sstream>
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
    region_config.wall_filament.value                  = 3;
    region_config.wall_loops.value                     = 2;
    region_config.enable_infill_filament_override.value = false;
    region_config.sparse_infill_density.value          = 15.;
    region_config.sparse_infill_filament.value         = 2;
    region_config.solid_infill_filament.value          = 3;

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

    CHECK(layer0.wall_filament(region) == 0);
    CHECK(layer1.wall_filament(region) == 1);
    CHECK(layer0.sparse_infill_filament(region) == 0);
    CHECK(layer1.sparse_infill_filament(region) == 0);
    CHECK(layer0.solid_infill_filament(region) == 0);
    CHECK(layer1.solid_infill_filament(region) == 0);

    region_config.enable_infill_filament_override.value = true;
    region_config.sparse_infill_filament.value          = 2;
    region_config.solid_infill_filament.value           = 2;
    PrintRegion overridden_region(region_config);

    CHECK(layer0.sparse_infill_filament(overridden_region) == 1);
    CHECK(layer1.sparse_infill_filament(overridden_region) == 1);
    CHECK(layer0.solid_infill_filament(overridden_region) == 1);
    CHECK(layer1.solid_infill_filament(overridden_region) == 1);
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
