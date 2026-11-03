#pragma once
#ifndef slic3r_ColorConversionPalette_hpp_
#define slic3r_ColorConversionPalette_hpp_

#include <string>
#include <vector>

namespace Slic3r {

struct FullSpectrumPaletteEntry;

// One filament of the user-defined "color conversion" palette (e.g. C, M, Y, K, W).
// Stored in the project as strings "#RRGGBB|<filament preset name or library family name>".
struct ColorConversionPaletteEntry
{
    std::string hex;          // normalized "#RRGGBB" (upper-case)
    std::string preset_name;  // filament preset name, or library family name for defaults; may be empty

    bool operator==(const ColorConversionPaletteEntry &rhs) const { return hex == rhs.hex && preset_name == rhs.preset_name; }
    bool operator!=(const ColorConversionPaletteEntry &rhs) const { return !(*this == rhs); }
};

// A palette needs at least two filaments to mix anything; the upper bound keeps
// total_filaments (physical + mixed) well below MAXIMUM_FILAMENT_NUMBER.
constexpr size_t kMinColorConversionPalette = 2;
constexpr size_t kMaxColorConversionPalette = 16;

// Returns "#RRGGBB" (upper-case) for "#RGB"/"#RRGGBB"/"RRGGBB" input, or "" when invalid.
std::string normalize_palette_hex(const std::string &value);

// Invalid rows are dropped; the result is capped at kMaxColorConversionPalette.
std::vector<ColorConversionPaletteEntry> parse_color_conversion_palette(const std::vector<std::string> &serialized);
std::vector<std::string> serialize_color_conversion_palette(const std::vector<ColorConversionPaletteEntry> &entries);

// Default C/M/Y/W palette from the Full Spectrum library palette (see DefaultFullSpectrumSelections).
std::vector<ColorConversionPaletteEntry> default_color_conversion_palette(const std::vector<FullSpectrumPaletteEntry> &library_palette,
                                                                          const std::string                            &default_family);

// Shift every filament ID of an encoded gradient_component_ids string by `offset`
// (used when palette filaments are appended after N existing physical filaments).
// Returns "" for an empty/undecodable input.
std::string shift_gradient_component_ids(const std::string &encoded_ids, unsigned int offset);

} // namespace Slic3r

#endif
