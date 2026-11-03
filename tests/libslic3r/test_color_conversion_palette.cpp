#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ColorConversionPalette.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/PresetBundle.hpp"

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
