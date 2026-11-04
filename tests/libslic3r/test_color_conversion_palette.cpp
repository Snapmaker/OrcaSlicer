#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ColorConversionPalette.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r;

TEST_CASE("normalize_palette_hex accepts the usual spellings", "[ColorConversionPalette]")
{
    CHECK(normalize_palette_hex("#00ffff") == "#00FFFF");
    CHECK(normalize_palette_hex("00FFFF") == "#00FFFF");
    CHECK(normalize_palette_hex("  #0Ff ") == "#00FFFF");
    CHECK(normalize_palette_hex("#12345").empty());
    CHECK(normalize_palette_hex("#GGGGGG").empty());
    CHECK(normalize_palette_hex("").empty());
}

TEST_CASE("color conversion palette round-trips through its serialized form", "[ColorConversionPalette]")
{
    const std::vector<ColorConversionPaletteEntry> entries = {
        {"#00FFFF", "Snapmaker PLA Full Spectrum @U1 0.4 nozzle"},
        {"#FF00FF", "Some Preset"},
        {"#000000", ""},
    };
    const std::vector<std::string> serialized = serialize_color_conversion_palette(entries);
    REQUIRE(serialized.size() == 3);
    CHECK(serialized[0] == "#00FFFF|Snapmaker PLA Full Spectrum @U1 0.4 nozzle");
    CHECK(serialized[2] == "#000000|");
    CHECK(parse_color_conversion_palette(serialized) == entries);
}

TEST_CASE("parse_color_conversion_palette drops invalid rows and caps the size", "[ColorConversionPalette]")
{
    std::vector<std::string> rows = {"#FFFFFF|A", "garbage|B", "", "#000", "#123456"};
    const auto parsed = parse_color_conversion_palette(rows);
    REQUIRE(parsed.size() == 3);
    CHECK(parsed[0].hex == "#FFFFFF");
    CHECK(parsed[1].hex == "#000000");
    CHECK(parsed[1].preset_name.empty());
    CHECK(parsed[2].hex == "#123456");

    std::vector<std::string> many(kMaxColorConversionPalette + 5, "#ABCDEF|x");
    CHECK(parse_color_conversion_palette(many).size() == kMaxColorConversionPalette);
}

TEST_CASE("shift_gradient_component_ids moves every id by the offset", "[ColorConversionPalette]")
{
    CHECK(shift_gradient_component_ids("123", 4) == "567");
    // Crossing 9 switches to the extended '/' encoding.
    CHECK(MixedFilamentManager::decode_gradient_component_ids(shift_gradient_component_ids("89", 3)) ==
          std::vector<unsigned int>({11, 12}));
    CHECK(shift_gradient_component_ids("", 4).empty());
}

TEST_CASE("color_conversion_palette is a project option", "[ColorConversionPalette]")
{
    PresetBundle bundle;
    REQUIRE(bundle.project_config.has("color_conversion_palette"));
    CHECK(bundle.project_config.option<ConfigOptionStrings>("color_conversion_palette")->values.empty());

    bundle.project_config.option<ConfigOptionStrings>("color_conversion_palette")->values = {"#FF0000|X", "#0000FF|Y"};
    DynamicPrintConfig full_config = DynamicPrintConfig::full_print_config();
    full_config.apply(bundle.project_config);
    REQUIRE(full_config.has("color_conversion_palette"));
    CHECK(full_config.option<ConfigOptionStrings>("color_conversion_palette")->values.size() == 2);
}

// Palette conversion appends the palette as slots N+1..N+K and keeps only those (plus the new
// mixed rows). Everything that referenced an original filament must be reported redundant.
TEST_CASE("compute_redundant_filaments with an appended palette keeps only the palette", "[ColorConversionPalette][redundant_set]")
{
    constexpr size_t N = 4; // original filaments
    constexpr size_t K = 3; // palette filaments appended as slots 5..7

    MixedFilamentManager mgr;
    MixedFilament fresh; // new mixed colour made from palette slots 5 and 6  -> virtual id 8
    fresh.component_a = 5;
    fresh.component_b = 6;
    fresh.custom      = true;
    MixedFilament old_row; // pre-existing mixed colour made from original slots 1 and 2 -> virtual id 9
    old_row.component_a = 1;
    old_row.component_b = 2;
    old_row.custom      = true;
    mgr.mixed_filaments().push_back(fresh);
    mgr.mixed_filaments().push_back(old_row);

    const RedundantFilamentSet red = compute_redundant_filaments(N + K, {5, 6, 7}, {8}, mgr.mixed_filaments());

    CHECK(red.redundant_physical == std::vector<unsigned int>({4, 3, 2, 1}));
    CHECK(red.redundant_mixed == std::vector<unsigned int>({9}));
    CHECK(red.new_num_physical == K);
}

// ---------------------------------------------------------------------------
// Palette conversion is the first batch flow whose KEPT physical slots sit above the
// deleted ones (kept N+1..N+K, deleted 1..N), so every survivor moves down by N.
// These tests pin the three remaps cleanup_unused_filaments_after_batch_match relies on:
//   1. mixed-row components   -> remove_physical_filament, called once per deleted slot
//   2. painted physical ids   -> kept-aware composite remap (update_mixed_filament_id_remap)
//   3. painted mixed (virtual) ids -> same composite remap, by stable_id
// Config-level "extruder" ids go through Plater::on_filaments_delete per deletion
// (remap_model_config_filament_ids), which is already covered in test_mixed_filament.cpp.
// ---------------------------------------------------------------------------

namespace {
struct AutoGenerateOff
{
    bool prev;
    AutoGenerateOff() : prev(MixedFilamentManager::auto_generate_enabled()) { MixedFilamentManager::set_auto_generate_enabled(false); }
    ~AutoGenerateOff() { MixedFilamentManager::set_auto_generate_enabled(prev); }
};
} // namespace

TEST_CASE("palette conversion: mixed-row components shift down by N", "[ColorConversionPalette][remap]")
{
    AutoGenerateOff guard;
    constexpr unsigned int N = 4; // originals 1..4, palette 5..9 (C, M, Y, K, W)

    MixedFilamentManager mgr;
    MixedFilament pair; // 60% slot 5 + 40% slot 7
    pair.component_a   = 5;
    pair.component_b   = 7;
    pair.mix_b_percent = 40;
    pair.custom        = true;
    pair.stable_id     = 101;
    MixedFilament quad; // 4-component recipe over slots 5,6,8,9
    quad.component_a                = 5;
    quad.component_b                = 6;
    quad.gradient_component_ids     = MixedFilamentManager::encode_gradient_component_ids({5, 6, 8, 9});
    quad.gradient_component_weights = "40/30/20/10";
    quad.distribution_mode          = int(MixedFilament::LayerCycle);
    quad.custom                     = true;
    quad.stable_id                  = 102;
    MixedFilament legacy; // pre-existing row on original slots 1+2 -> must disappear
    legacy.component_a = 1;
    legacy.component_b = 2;
    legacy.custom      = true;
    legacy.stable_id   = 103;
    mgr.mixed_filaments() = {pair, quad, legacy};

    // cleanup deletes the redundant physical slots in DESCENDING order (4,3,2,1).
    for (unsigned int id = N; id >= 1; --id)
        mgr.remove_physical_filament(id);

    const auto& rows = mgr.mixed_filaments();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].stable_id == 101);
    CHECK(rows[0].component_a == 1);
    CHECK(rows[0].component_b == 3);
    CHECK(rows[0].mix_b_percent == 40);
    CHECK(rows[1].stable_id == 102);
    CHECK(MixedFilamentManager::decode_gradient_component_ids(rows[1].gradient_component_ids) == std::vector<unsigned int>({1, 2, 4, 5}));
    CHECK(rows[1].gradient_component_weights == "40/30/20/10");
}

TEST_CASE("palette conversion: composite painting remap maps kept tail slots to 1..K", "[ColorConversionPalette][remap]")
{
    AutoGenerateOff guard;
    constexpr size_t N = 4, K = 3; // originals 1..4, palette 5..7, new mixed row = virtual id 8

    PresetBundle bundle;
    bundle.filament_presets.assign(N + K, "Default Filament");
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {"#111111", "#222222", "#333333", "#444444",
                                                                                     "#00FFFF", "#FF00FF", "#FFFF00"};
    bundle.update_multi_material_filament_presets();

    MixedFilament fresh;
    fresh.component_a = 5;
    fresh.component_b = 6;
    fresh.custom      = true;
    fresh.stable_id   = 7001;
    bundle.mixed_filaments.mixed_filaments() = {fresh};
    const std::vector<MixedFilament> old_mixed = bundle.mixed_filaments.mixed_filaments();

    // Mirror the cleanup loop: delete 4,3,2,1 from the mixed list, then build the composite remap.
    for (unsigned int id = N; id >= 1; --id)
        bundle.mixed_filaments.remove_physical_filament(static_cast<unsigned int>(id));
    bundle.update_mixed_filament_id_remap(old_mixed, N + K, K, size_t(-1), {5, 6, 7});
    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

    REQUIRE(remap.size() == N + K + 1 + 1); // [0] sink + 7 physical + 1 mixed
    for (unsigned int old_id = 1; old_id <= N; ++old_id)
        CHECK(remap[old_id] == 0); // originals are gone (their painting was migrated by apply)
    CHECK(remap[5] == 1);
    CHECK(remap[6] == 2);
    CHECK(remap[7] == 3);
    CHECK(remap[8] == 4); // the new mixed colour: K physical + first mixed row
}

TEST_CASE("filament rewrite jump: undo and redo across one rewrite", "[ColorConversionPalette][undo]")
{
    const std::vector<size_t> times{5}; // snapshot 5 was taken right before the rewrite

    auto undo = resolve_filament_rewrite_jump(times, 6, 5); // topmost -> the rewrite's snapshot
    REQUIRE(undo.has_value());
    CHECK(undo->index == 0);
    CHECK(undo->restore_before);

    auto redo = resolve_filament_rewrite_jump(times, 5, 6);
    REQUIRE(redo.has_value());
    CHECK(redo->index == 0);
    CHECK_FALSE(redo->restore_before);

    // Jumps that stay on one side of the rewrite leave the filament list alone.
    CHECK_FALSE(resolve_filament_rewrite_jump(times, 9, 6).has_value());
    CHECK_FALSE(resolve_filament_rewrite_jump(times, 6, 9).has_value());
    CHECK_FALSE(resolve_filament_rewrite_jump(times, 5, 2).has_value());
    CHECK_FALSE(resolve_filament_rewrite_jump(times, 2, 5).has_value());
    CHECK_FALSE(resolve_filament_rewrite_jump(times, 6, 6).has_value());
    CHECK_FALSE(resolve_filament_rewrite_jump({}, 6, 5).has_value());
}

TEST_CASE("filament rewrite jump: a jump across several rewrites lands on the outermost", "[ColorConversionPalette][undo]")
{
    const std::vector<size_t> times{3, 7};

    // Undo past both: the state before the earliest.
    auto undo = resolve_filament_rewrite_jump(times, 9, 1);
    REQUIRE(undo.has_value());
    CHECK(undo->index == 0);
    CHECK(undo->restore_before);

    // Redo past both: the state after the latest.
    auto redo = resolve_filament_rewrite_jump(times, 1, 9);
    REQUIRE(redo.has_value());
    CHECK(redo->index == 1);
    CHECK_FALSE(redo->restore_before);

    // Between the two rewrites: only the later one is crossed.
    auto to_middle = resolve_filament_rewrite_jump(times, 9, 5);
    REQUIRE(to_middle.has_value());
    CHECK(to_middle->index == 1);
    CHECK(to_middle->restore_before);

    auto from_middle = resolve_filament_rewrite_jump(times, 5, 1);
    REQUIRE(from_middle.has_value());
    CHECK(from_middle->index == 0);
    CHECK(from_middle->restore_before);
}

TEST_CASE("filament project state round-trips and keeps unrelated project options", "[ColorConversionPalette][undo]")
{
    AutoGenerateOff guard;
    PresetBundle bundle;

    auto colours = [&bundle]() -> std::vector<std::string>& {
        return bundle.project_config.option<ConfigOptionStrings>("filament_colour", true)->values;
    };

    bundle.filament_presets        = {"PLA A", "PLA B"};
    bundle.ams_multi_color_filment = {{"#111111"}, {"#222222"}};
    colours()                      = {"#111111", "#222222"};
    bundle.mixed_filaments.add_custom_filament(1, 2, 50, colours());
    REQUIRE(bundle.mixed_filaments.mixed_filaments().size() == 1);
    const uint64_t stable_id = bundle.mixed_filaments.mixed_filaments().front().stable_id;

    const FilamentProjectState before = bundle.capture_filament_project_state();

    // The rewrite: new filament list, new colours, the mixed row is gone.
    bundle.filament_presets        = {"PLA C", "PLA D", "PLA E"};
    bundle.ams_multi_color_filment = {{"#333333"}, {"#444444"}, {"#555555"}};
    colours()                      = {"#333333", "#444444", "#555555"};
    bundle.mixed_filaments.mixed_filaments().clear();
    bundle.project_config.set_key_value("color_conversion_palette", new ConfigOptionStrings({"#000000|x", "#FFFFFF|y"}));
    bundle.set_filament_id_remap({0, 1, 2});

    const FilamentProjectState after = bundle.capture_filament_project_state();
    const t_config_option_keys keys  = PresetBundle::changed_filament_project_keys(before, after);
    CHECK(std::find(keys.begin(), keys.end(), "filament_colour") != keys.end());

    // An unrelated option edited after the rewrite is not in the changed keys.
    bundle.project_config.set_key_value("wipe_tower_rotation_angle", new ConfigOptionFloat(42.));
    CHECK(std::find(keys.begin(), keys.end(), "wipe_tower_rotation_angle") == keys.end());

    bundle.restore_filament_project_state(before, keys);
    CHECK(bundle.filament_presets == std::vector<std::string>{"PLA A", "PLA B"});
    CHECK(bundle.ams_multi_color_filment.size() == 2);
    CHECK(colours() == std::vector<std::string>{"#111111", "#222222"});
    REQUIRE(bundle.mixed_filaments.mixed_filaments().size() == 1);
    CHECK(bundle.mixed_filaments.mixed_filaments().front().stable_id == stable_id);
    CHECK(bundle.project_config.opt_float("wipe_tower_rotation_angle") == 42.);
    CHECK(bundle.last_filament_id_remap().empty()); // a stale remap must not hit the restored ids

    // Redo brings the rewrite back with the same keys.
    bundle.restore_filament_project_state(after, keys);
    CHECK(bundle.filament_presets == std::vector<std::string>{"PLA C", "PLA D", "PLA E"});
    CHECK(colours() == std::vector<std::string>{"#333333", "#444444", "#555555"});
    CHECK(bundle.mixed_filaments.mixed_filaments().empty());
}

TEST_CASE("changed_filament_project_keys covers keys only one state has", "[ColorConversionPalette][undo]")
{
    FilamentProjectState a, b;
    a.project_config.set_key_value("filament_colour", new ConfigOptionStrings({"#111111"}));
    b.project_config.set_key_value("filament_colour", new ConfigOptionStrings({"#111111"}));
    CHECK(PresetBundle::changed_filament_project_keys(a, b).empty());

    b.project_config.set_key_value("color_conversion_palette", new ConfigOptionStrings({"#000000|x"}));
    const t_config_option_keys only_in_b = PresetBundle::changed_filament_project_keys(a, b);
    CHECK(only_in_b == t_config_option_keys{"color_conversion_palette"});

    // Restoring a state without the key removes it.
    PresetBundle bundle;
    bundle.project_config.set_key_value("color_conversion_palette", new ConfigOptionStrings({"#000000|x"}));
    bundle.restore_filament_project_state(a, only_in_b);
    CHECK_FALSE(bundle.project_config.has("color_conversion_palette"));
}
