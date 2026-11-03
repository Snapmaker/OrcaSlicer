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
