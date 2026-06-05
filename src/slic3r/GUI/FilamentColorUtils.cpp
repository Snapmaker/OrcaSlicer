#include "FilamentColorUtils.hpp"

#include "MixedFilamentBadge.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace Slic3r { namespace GUI { namespace FilamentColorUtils {
namespace {

std::string trim_copy(const std::string& value)
{
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch); });
    const auto end   = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) { return std::isspace(ch); }).base();
    return begin < end ? std::string(begin, end) : std::string();
}

std::string normalize_hex_color_no_fallback(const std::string& color)
{
    std::string value = trim_copy(color);
    if (!value.empty() && value.front() == '#')
        value.erase(value.begin());
    if (value.size() == 8)
        value = value.substr(0, 6);
    if (value.size() != 6)
        return {};

    for (char& ch : value) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (!std::isxdigit(uch))
            return {};
        ch = static_cast<char>(std::toupper(uch));
    }

    return "#" + value;
}

std::vector<std::string> normalized_colors_or_fallback(const std::vector<std::string>& colors, const std::string& fallback_color)
{
    std::vector<std::string> normalized;
    normalized.reserve(colors.size());
    for (const std::string& color : colors) {
        const std::string normalized_color = normalize_hex_color(color);
        if (!normalized_color.empty())
            normalized.emplace_back(normalized_color);
    }

    if (normalized.empty()) {
        const std::string fallback = normalize_hex_color(fallback_color, "#FFFFFF");
        normalized.emplace_back(fallback.empty() ? "#FFFFFF" : fallback);
    }

    return normalized;
}

wxColour wx_color_from_hex(const std::string& color)
{
    wxColour parsed(color);
    return parsed.IsOk() ? parsed : wxColour("#FFFFFF");
}

} // namespace

std::string normalize_hex_color(const std::string& color)
{
    return normalize_hex_color_no_fallback(color);
}

std::string normalize_hex_color(const std::string& color, const std::string& fallback_color)
{
    const std::string normalized = normalize_hex_color_no_fallback(color);
    if (!normalized.empty())
        return normalized;

    if (color == fallback_color)
        return {};
    return normalize_hex_color_no_fallback(fallback_color);
}

std::string strip_hash_for_preprint(const std::string& color)
{
    std::string normalized = normalize_hex_color(color);
    if (!normalized.empty() && normalized.front() == '#')
        normalized.erase(normalized.begin());
    return normalized;
}

std::string strip_hash_for_preprint(const std::string& color, const std::string& fallback_color)
{
    std::string normalized = normalize_hex_color(color, fallback_color);
    if (!normalized.empty() && normalized.front() == '#')
        normalized.erase(normalized.begin());
    return normalized;
}

std::vector<std::string> split_multi_colors(const std::string& value)
{
    std::vector<std::string> colors;
    std::stringstream        stream(value);
    std::string              token;

    while (std::getline(stream, token, '|')) {
        const std::string normalized = normalize_hex_color(token);
        if (!normalized.empty())
            colors.emplace_back(normalized);
    }

    return colors;
}

std::string join_multi_colors(const std::vector<std::string>& colors)
{
    std::ostringstream out;
    bool first = true;
    for (const std::string& color : colors) {
        const std::string normalized = normalize_hex_color(color);
        if (normalized.empty())
            continue;
        if (!first)
            out << '|';
        out << normalized;
        first = false;
    }
    return out.str();
}

std::string get_primary_color(const std::vector<std::string>& colors, const std::string& fallback_color)
{
    for (const std::string& color : colors) {
        const std::string normalized = normalize_hex_color(color);
        if (!normalized.empty())
            return normalized;
    }
    return normalize_hex_color(fallback_color, "#FFFFFF");
}

int normalize_colour_mode(int mode)
{
    return mode == 1 ? 1 : 0;
}

nlohmann::json build_preprint_color_multi_item(const std::string& multi_colors, int mode, const std::string& fallback_color)
{
    std::vector<std::string> colors = split_multi_colors(multi_colors);
    if (colors.empty())
        colors.emplace_back(normalize_hex_color(fallback_color, "#FFFFFF"));

    const int normalized_mode = colors.size() > 1 ? normalize_colour_mode(mode) : 0;

    nlohmann::json out_colors = nlohmann::json::array();
    for (const std::string& color : colors)
        out_colors.push_back(color.substr(1));

    nlohmann::json item; 
    item["mode"]   = normalized_mode;
    item["nums"]   = colors.size();
    item["colors"] = out_colors;
    return item;
}

wxBitmap* get_filament_color_icon(const std::vector<std::string>& colors,
                                  int                             mode,
                                  const std::string&              label,
                                  int                             icon_width,
                                  int                             icon_height)
{
    icon_width  = std::max(1, icon_width);
    icon_height = std::max(1, icon_height);

    const std::vector<std::string> normalized = normalized_colors_or_fallback(colors, "#FFFFFF");
    std::vector<wxColour> wx_colors;
    wx_colors.reserve(normalized.size());
    for (const std::string& color : normalized)
        wx_colors.emplace_back(wx_color_from_hex(color));

    ColorBlockParams params;
    params.width  = icon_width;
    params.height = icon_height;
    params.label  = wxString::FromUTF8(label.c_str());

    if (wx_colors.size() <= 1) {
        params.mode        = ColorBlockParams::Solid;
        params.solid_color = wx_colors.empty() ? wxColour("#FFFFFF") : wx_colors.front();
    } else if (normalize_colour_mode(mode) == 1) {
        params.mode               = ColorBlockParams::Gradient;
        params.gradient_direction = ColorBlockParams::LeftToRight;
        params.colors             = wx_colors;
    } else {
        params.mode   = ColorBlockParams::Segments;
        params.colors = wx_colors;
    }

    return get_color_block_bitmap_cached(params);
}

wxBitmap* get_filament_color_icon(const std::string& multi_colors,
                                  int                mode,
                                  const std::string& fallback_color,
                                  const std::string& label,
                                  int                icon_width,
                                  int                icon_height)
{
    return get_filament_color_icon(normalized_colors_or_fallback(split_multi_colors(multi_colors), fallback_color),
                                   mode,
                                   label,
                                   icon_width,
                                   icon_height);
}

}}} // namespace Slic3r::GUI::FilamentColorUtils
