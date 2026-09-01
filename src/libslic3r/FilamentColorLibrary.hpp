#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r
{

enum class FilamentColorMode
{
    Segment = 0, // Single colors or side-by-side segments.
    Gradient = 1 // Gradient stops ordered from bottom to top.
};

std::string NormalizeFilamentHexColor(const std::string& color);
std::string NormalizeFilamentHexColor(const std::string& color, const std::string& fallbackColor);
std::vector<std::string> SplitFilamentMultiColors(const std::string& value);
std::string JoinFilamentMultiColors(const std::vector<std::string>& colors);
std::string GetFilamentMatchName(const std::string& name);
FilamentColorMode FilamentColorModeFromConfig(int modeValue);
int FilamentColorModeToConfig(FilamentColorMode mode);

struct FilamentColor
{
    std::vector<std::string> colors;
    FilamentColorMode mode { FilamentColorMode::Segment };

    bool Empty() const;
    FilamentColorMode NormalizedMode() const;
    bool IsGradient() const;
    std::string PrimaryColor(const std::string& fallbackColor = "#26A69A") const;
    std::string ToMultiColorsString() const;
    bool Matches(const FilamentColor& other) const;

    static FilamentColor FromColors(const std::vector<std::string>& colors, FilamentColorMode mode,
                                    const std::string& fallbackColor = "#26A69A");
    static FilamentColor FromMultiColors(const std::string& multiColors, FilamentColorMode mode,
                                         const std::string& fallbackColor = "#26A69A");
};

struct FilamentColorItem
{
    std::unordered_map<std::string, std::string> colorNames;
    std::string sku;
    double tdValue = 0.0;
    FilamentColor colorData;
};

struct FilamentColorInfo
{
    std::string filamentId;
    std::string filamentName;
    std::string type;
    std::vector<FilamentColorItem> colors;
};

// Number of color slots in the recommended-mode (Full Spectrum) palette card —
// product spec constant (2x2 slot grid); also the C/M/Y/W default-selection count.
constexpr int kFullSpectrumSlotCount = 4;

// One selectable color in the recommended-mode (Full Spectrum) color-mix palette.
// Built by BuildFullSpectrumPalette from the hot-updated filaments_colours.json.
struct FullSpectrumPaletteEntry
{
    std::string hex;         // normalized "#RRGGBB"
    std::string en_name;     // canonical English color name (sort key). Empty when the SKU has no "en"
                             // entry — deliberately no arbitrary-locale fallback: this feeds sorting and
                             // slot-name matching, not display (slot matching simply skips empty names)
    std::string family_name; // owning filament name, e.g. "Snapmaker PLA Full Spectrum @U1"
    double td_value = 0.0;   // transmittance density from FilamentColorItem::tdValue; 0 = no value
    std::unordered_map<std::string, std::string> color_names; // locale -> display name (copied from the SKU entry)
};

// Recommended-mode palette for the batch color match: every library filament whose
// filament_type contains "Full Spectrum" contributes its single-color SKUs (multi-color /
// gradient SKUs are skipped, same rule as the GUI's legacy load_full_spectrum_colors filter).
// Entries are sorted case-insensitively by "<family_name> <en_name> <hex>" — grouped by
// family (families in alphabetical order), colors alphabetically within a family — so the
// dropdown order matches the phase-2 test matrix #10 and is stable across UI locales.
// Pure function over its input (no singleton access, no file IO) — unit-testable with
// constructed FilamentColorInfo inputs.
std::vector<FullSpectrumPaletteEntry> BuildFullSpectrumPalette(const std::vector<FilamentColorInfo>& library_data);

// Default dropdown selections (palette indices, one per slot 0..3) per the phase-2 spec:
// slots 1..4 prefer cyan / magenta / yellow / white, matched case-insensitively against the
// EN color name; entries of default_family win over other families (palette order breaks
// remaining ties). Slots with no name match fall back to the next unused entry —
// default_family entries first, then the rest, both in palette order. default_family is
// compared in the GetFilamentMatchName space on both sides (same convention as
// FindFilamentByName), so any of the three family-name forms is accepted: the raw library
// name ("... @U1"), the full preset name ("... @U1 0.4 nozzle"), or the stripped match
// name. Always returns distinct indices; returns fewer than 4 only when the palette itself
// has fewer than 4 entries.
std::vector<int> DefaultFullSpectrumSelections(const std::vector<FullSpectrumPaletteEntry>& palette, const std::string& default_family);

class FilamentColorLibrary
{
public:
    static FilamentColorLibrary& Instance();

    bool EnsureLoaded();
    void Reload();

    // Read-only view of the loaded entries (empty until EnsureLoaded succeeds). The view
    // is invalidated by Reload()/Clear — copy what you need beyond the call site.
    const std::vector<FilamentColorInfo>& GetAllFilamentInfos() const { return _filamentInfoVec; }

    bool FindFilamentById(const std::string& filamentId, FilamentColorInfo& outFilament);
    bool FindFilamentByName(const std::string& filamentName, FilamentColorInfo& outFilament);

private:
    bool LoadIndex();
    bool FindFilamentByIndex(size_t index, FilamentColorInfo& outFilament) const;
    void Clear();

private:
    bool _loaded { false };

    std::vector<FilamentColorInfo> _filamentInfoVec;
    std::unordered_map<std::string, size_t> _filamentIndexByIdMap;   // filament_id to index in _filamentInfoVec
    std::unordered_map<std::string, size_t> _filamentIndexByNameMap; // normalized filament name to index in _filamentInfoVec
};

} // namespace Slic3r
