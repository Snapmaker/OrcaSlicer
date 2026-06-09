#include "FilamentColorUtils.hpp"

#include "libslic3r/PrintConfig.hpp"
#include "MixedFilamentBadge.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace Slic3r
{
namespace GUI
{
namespace FilamentColorUtils
{
namespace
{

std::string TrimCopy(const std::string& value)
{
    std::string::const_iterator begin = std::find_if_not(value.begin(), value.end(),
                                        [](unsigned char ch)
                                        {
                                            return std::isspace(ch);
                                        });
    std::string::const_iterator end = std::find_if_not(value.rbegin(), value.rend(),
                                        [](unsigned char ch)
                                        {
                                            return std::isspace(ch);
                                        }).base();
    return begin < end ? std::string(begin, end) : std::string();
}

std::string NormalizeHexColorNoFallback(const std::string& color)
{
    std::string value = TrimCopy(color);
    if (!value.empty() && value.front() == '#')
        value.erase(value.begin());
    if (value.size() == 8)
        value = value.substr(0, 6);
    if (value.size() != 6)
        return {};

    for (char& ch : value)
    {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (!std::isxdigit(uch))
            return {};
        ch = static_cast<char>(std::toupper(uch));
    }

    return "#" + value;
}

std::vector<std::string> NormalizedColorsOrFallback(const std::vector<std::string>& colors,
                                                    const std::string& fallback_color)
{
    std::vector<std::string> normalized;
    normalized.reserve(colors.size());
    for (const std::string& color : colors)
    {
        const std::string normalized_color = NormalizeHexColor(color);
        if (!normalized_color.empty())
            normalized.emplace_back(normalized_color);
    }

    if (normalized.empty())
    {
        const std::string fallback = NormalizeHexColor(fallback_color, "#FFFFFF");
        normalized.emplace_back(fallback.empty() ? "#FFFFFF" : fallback);
    }

    return normalized;
}

wxColour WxColorFromHex(const std::string& color)
{
    wxColour parsed(color);
    return parsed.IsOk() ? parsed : wxColour("#FFFFFF");
}

} // namespace

std::string NormalizeHexColor(const std::string& color)
{
    return NormalizeHexColorNoFallback(color);
}

std::string NormalizeHexColor(const std::string& color, const std::string& fallback_color)
{
    const std::string normalized = NormalizeHexColorNoFallback(color);
    if (!normalized.empty())
        return normalized;

    if (color == fallback_color)
        return {};
    return NormalizeHexColorNoFallback(fallback_color);
}

std::string StripHashForPreprint(const std::string& color)
{
    std::string normalized = NormalizeHexColor(color);
    if (!normalized.empty() && normalized.front() == '#')
        normalized.erase(normalized.begin());
    return normalized;
}

std::string StripHashForPreprint(const std::string& color, const std::string& fallback_color)
{
    std::string normalized = NormalizeHexColor(color, fallback_color);
    if (!normalized.empty() && normalized.front() == '#')
        normalized.erase(normalized.begin());
    return normalized;
}

std::vector<std::string> SplitMultiColors(const std::string& value)
{
    std::vector<std::string> colors;
    std::stringstream stream(value);
    std::string token;

    while (std::getline(stream, token, '|'))
    {
        const std::string normalized = NormalizeHexColor(token);
        if (!normalized.empty())
            colors.emplace_back(normalized);
    }

    return colors;
}

std::string JoinMultiColors(const std::vector<std::string>& colors)
{
    std::ostringstream out;
    bool first = true;
    for (const std::string& color : colors)
    {
        const std::string normalized = NormalizeHexColor(color);
        if (normalized.empty())
            continue;
        if (!first)
            out << '|';
        out << normalized;
        first = false;
    }
    return out.str();
}

std::string GetPrimaryColor(const std::vector<std::string>& colors, const std::string& fallback_color)
{
    for (const std::string& color : colors)
    {
        const std::string normalized = NormalizeHexColor(color);
        if (!normalized.empty())
            return normalized;
    }
    return NormalizeHexColor(fallback_color, "#FFFFFF");
}

int NormalizeColourMode(int mode)
{
    return mode == 1 ? 1 : 0;
}

FilamentColorDisplay GetFilamentColorDisplay(const DynamicPrintConfig* config,
                                             size_t color_index,
                                             const std::string& fallback_color)
{
    FilamentColorDisplay display;
    display.primaryColor = NormalizeHexColor(fallback_color, "#FFFFFF");

    if (config != nullptr && config->has("filament_multi_colors"))
    {
        const ConfigOptionStrings* option = config->option<ConfigOptionStrings>("filament_multi_colors");
        if (option != nullptr && option->values.size() > color_index)
            display.colors = SplitMultiColors(option->values[color_index]);
    }

    if (config != nullptr && config->has("filament_colour_mode"))
    {
        const ConfigOptionInts* option = config->option<ConfigOptionInts>("filament_colour_mode");
        if (option != nullptr && option->values.size() > color_index)
            display.mode = option->values[color_index];
    }

    if (display.colors.empty())
        display.colors.emplace_back(display.primaryColor);

    display.primaryColor = GetPrimaryColor(display.colors, display.primaryColor);
    display.mode = display.colors.size() > 1 ? NormalizeColourMode(display.mode) : 0;
    return display;
}

nlohmann::json BuildPreprintColorMultiItem(const std::string& multi_colors, int mode, const std::string& fallback_color)
{
    std::vector<std::string> colors = SplitMultiColors(multi_colors);
    if (colors.empty())
        colors.emplace_back(NormalizeHexColor(fallback_color, "#FFFFFF"));

    const int normalized_mode = colors.size() > 1 ? NormalizeColourMode(mode) : 0;

    nlohmann::json out_colors = nlohmann::json::array();
    for (const std::string& color : colors)
        out_colors.push_back(color.substr(1));

    nlohmann::json item;
    item["mode"] = normalized_mode;
    item["nums"] = colors.size();
    item["colors"] = out_colors;
    return item;
}

wxBitmap* GetFilamentColorIcon(const std::vector<std::string>& colors, int mode, const std::string& label, int icon_width, int icon_height)
{
    icon_width = std::max(1, icon_width);
    icon_height = std::max(1, icon_height);

    const std::vector<std::string> normalized = NormalizedColorsOrFallback(colors, "#FFFFFF");
    std::vector<wxColour> wx_colors;
    wx_colors.reserve(normalized.size());
    for (const std::string& color : normalized)
        wx_colors.emplace_back(WxColorFromHex(color));

    ColorBlockParams params;
    params.width = icon_width;
    params.height = icon_height;
    params.label = wxString::FromUTF8(label.c_str());

    if (wx_colors.size() <= 1)
    {
        params.mode = ColorBlockParams::Solid;
        params.solid_color = wx_colors.empty() ? wxColour("#FFFFFF") : wx_colors.front();
    }
    else if (NormalizeColourMode(mode) == 1)
    {
        params.mode = ColorBlockParams::Gradient;
        params.gradient_direction = ColorBlockParams::LeftToRight;
        params.colors = wx_colors;
    }
    else
    {
        params.mode = ColorBlockParams::Segments;
        params.colors = wx_colors;
    }

    return get_color_block_bitmap_cached(params);
}

wxBitmap* GetFilamentColorIcon(const std::string& multi_colors, int mode, const std::string& fallback_color,
                               const std::string& label, int icon_width, int icon_height)
{
    return GetFilamentColorIcon(NormalizedColorsOrFallback(SplitMultiColors(multi_colors), fallback_color), mode, label, icon_width, icon_height);
}

ImU32 ToImGuiColor(const std::string& color)
{
    const wxColour parsed = WxColorFromHex(NormalizeHexColor(color, "#FFFFFF"));
    return IM_COL32(parsed.Red(), parsed.Green(), parsed.Blue(), 255);
}

float ImGuiAverageLuminance(const FilamentColorDisplay& color)
{
    if (color.colors.empty())
        return 1.0f;

    float total_luminance = 0.0f;
    for (const std::string& item : color.colors)
    {
        const wxColour parsed = WxColorFromHex(NormalizeHexColor(item, "#FFFFFF"));
        total_luminance += (0.299f * parsed.Red() + 0.587f * parsed.Green() + 0.114f * parsed.Blue()) / 255.0f;
    }

    return total_luminance / float(color.colors.size());
}

ImVec4 ImGuiTextColorFor(const FilamentColorDisplay& color)
{
    return ImGuiAverageLuminance(color) < 80.0f / 255.0f ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0, 0, 0, 1.0f);
}

void DrawImGuiFilamentColorBlock(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max, const FilamentColorDisplay& color, float inset)
{
    if (draw_list == nullptr || color.colors.size() <= 1)
        return;

    const ImVec2 fill_min(min.x + inset, min.y + inset);
    const ImVec2 fill_max(max.x - inset, max.y - inset);
    const float width = fill_max.x - fill_min.x;
    const float height = fill_max.y - fill_min.y;
    if (width <= 1.0f || height <= 1.0f)
        return;

    if (color.mode == 1)
    {
        const size_t gradient_count = color.colors.size() - 1;
        for (size_t index = 0; index < gradient_count; ++index)
        {
            const float left = fill_min.x + width * float(index) / float(gradient_count);
            const float right = fill_min.x + width * float(index + 1) / float(gradient_count);
            const ImU32 left_color = ToImGuiColor(color.colors[index]);
            const ImU32 right_color = ToImGuiColor(color.colors[index + 1]);
            draw_list->AddRectFilledMultiColor(ImVec2(left, fill_min.y), ImVec2(right, fill_max.y), left_color,
                                                right_color, right_color, left_color);
        }
        return;
    }

    const size_t color_count = color.colors.size();
    for (size_t index = 0; index < color_count; ++index)
    {
        const float left = fill_min.x + width * float(index) / float(color_count);
        const float right = fill_min.x + width * float(index + 1) / float(color_count);
        draw_list->AddRectFilled(ImVec2(left, fill_min.y), ImVec2(right, fill_max.y), ToImGuiColor(color.colors[index]));
    }
}

} // namespace FilamentColorUtils
} // namespace GUI
} // namespace Slic3r
