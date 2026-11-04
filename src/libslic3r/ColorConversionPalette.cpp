#include "ColorConversionPalette.hpp"

#include "FilamentColorLibrary.hpp"
#include "MixedFilament.hpp"

#include <algorithm>
#include <cctype>

namespace Slic3r {

std::string normalize_palette_hex(const std::string &value)
{
    std::string s = value;
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }), s.end());
    if (!s.empty() && s.front() == '#')
        s.erase(s.begin());
    if (s.size() == 3) {
        std::string expanded;
        for (char c : s) { expanded += c; expanded += c; }
        s = expanded;
    }
    if (s.size() != 6)
        return {};
    for (char &c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return {};
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return "#" + s;
}

std::vector<ColorConversionPaletteEntry> parse_color_conversion_palette(const std::vector<std::string> &serialized)
{
    std::vector<ColorConversionPaletteEntry> out;
    for (const std::string &row : serialized) {
        if (out.size() >= kMaxColorConversionPalette)
            break;
        const size_t sep = row.find('|');
        const std::string hex = normalize_palette_hex(sep == std::string::npos ? row : row.substr(0, sep));
        if (hex.empty())
            continue;
        out.push_back({hex, sep == std::string::npos ? std::string() : row.substr(sep + 1)});
    }
    return out;
}

std::vector<std::string> serialize_color_conversion_palette(const std::vector<ColorConversionPaletteEntry> &entries)
{
    std::vector<std::string> out;
    out.reserve(entries.size());
    for (const ColorConversionPaletteEntry &e : entries) {
        const std::string hex = normalize_palette_hex(e.hex);
        if (hex.empty())
            continue;
        out.push_back(hex + "|" + e.preset_name);
    }
    return out;
}

std::vector<ColorConversionPaletteEntry> default_color_conversion_palette(const std::vector<FullSpectrumPaletteEntry> &library_palette,
                                                                          const std::string                            &default_family)
{
    std::vector<ColorConversionPaletteEntry> out;
    for (int idx : DefaultFullSpectrumSelections(library_palette, default_family)) {
        if (idx < 0 || static_cast<size_t>(idx) >= library_palette.size())
            continue;
        const FullSpectrumPaletteEntry &e = library_palette[static_cast<size_t>(idx)];
        const std::string hex = normalize_palette_hex(e.hex);
        if (!hex.empty())
            out.push_back({hex, e.family_name});
    }
    return out;
}

std::string shift_gradient_component_ids(const std::string &encoded_ids, unsigned int offset)
{
    std::vector<unsigned int> ids = MixedFilamentManager::decode_gradient_component_ids(encoded_ids);
    if (ids.empty())
        return {};
    for (unsigned int &id : ids)
        id += offset;
    return MixedFilamentManager::encode_gradient_component_ids(ids);
}

std::optional<FilamentRewriteJump> resolve_filament_rewrite_jump(const std::vector<size_t> &snapshot_times, size_t time_from, size_t time_to)
{
    // First rewrite with a timestamp >= time_to. Crossed rewrites are in [time_to, time_from) when
    // undoing and in [time_from, time_to) when redoing.
    const auto it = std::lower_bound(snapshot_times.begin(), snapshot_times.end(), time_to);
    if (time_to < time_from) {
        // Undo: the state before the earliest crossed rewrite.
        if (it != snapshot_times.end() && *it < time_from)
            return FilamentRewriteJump{static_cast<size_t>(it - snapshot_times.begin()), true};
    } else if (time_to > time_from) {
        // Redo: the state after the latest crossed rewrite.
        if (it != snapshot_times.begin() && *(it - 1) >= time_from)
            return FilamentRewriteJump{static_cast<size_t>(it - snapshot_times.begin()) - 1, false};
    }
    return std::nullopt;
}

} // namespace Slic3r
