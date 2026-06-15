#include "FilamentColorUtils.hpp"

#include "libslic3r/PrintConfig.hpp"
#include "MixedFilamentBadge.hpp"

#include <algorithm>

namespace Slic3r
{
namespace GUI
{
namespace FilamentColorUtils
{
namespace
{

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

void AddFallbackToNormalizedColors(std::vector<std::string>& colors, const std::string& fallbackColor)
{
    if (!colors.empty())
        return;

    const std::string fallback = NormalizeHexColor(fallbackColor, "#FFFFFF");
    colors.emplace_back(fallback.empty() ? "#FFFFFF" : fallback);
}

wxColour WxColorFromHex(const std::string& color)
{
    wxColour parsed(color);
    return parsed.IsOk() ? parsed : wxColour("#FFFFFF");
}

wxBitmap* GetFilamentColorIconFromNormalized(const std::vector<std::string>& normalizedColors, FilamentColorMode mode,
                                             const std::string& label, int iconWidth, int iconHeight,
                                             const wxColour& lightBorderColor)
{
    iconWidth = std::max(1, iconWidth);
    iconHeight = std::max(1, iconHeight);

    std::vector<wxColour> wx_colors;
    wx_colors.reserve(normalizedColors.size());
    for (const std::string& color : normalizedColors)
        wx_colors.emplace_back(WxColorFromHex(color));

    ColorBlockParams params;
    params.width = iconWidth;
    params.height = iconHeight;
    params.label = wxString::FromUTF8(label.c_str());

    if (wx_colors.size() <= 1)
    {
        params.mode = ColorBlockParams::Solid;
        params.solid_color = wx_colors.empty() ? wxColour("#FFFFFF") : wx_colors.front();
    }
    else if (mode == FilamentColorMode::Gradient)
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

    return get_color_block_bitmap_cached(params, lightBorderColor);
}

} // namespace

std::string NormalizeHexColor(const std::string& color)
{
    return NormalizeFilamentHexColor(color);
}

std::string NormalizeHexColor(const std::string& color, const std::string& fallback_color)
{
    return NormalizeFilamentHexColor(color, fallback_color);
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
    return SplitFilamentMultiColors(value);
}

std::string JoinMultiColors(const std::vector<std::string>& colors)
{
    return JoinFilamentMultiColors(colors);
}

std::string GetPrimaryColor(const std::vector<std::string>& colors, const std::string& fallback_color)
{
    const FilamentColor color = FilamentColor::FromColors(colors, FilamentColorMode::Segment, fallback_color);
    return color.PrimaryColor(fallback_color);
}

std::string GetFilamentMatchName(const std::string& name)
{
    return Slic3r::GetFilamentMatchName(name);
}

FilamentColor GetFilamentColorFromConfig(const DynamicPrintConfig* config, size_t colorIndex, const std::string& fallbackColor)
{
    std::vector<std::string> colors;
    FilamentColorMode mode = FilamentColorMode::Segment;

    if (config != nullptr && config->has("filament_multi_colors"))
    {
        const ConfigOptionStrings* option = config->option<ConfigOptionStrings>("filament_multi_colors");
        if (option != nullptr && option->values.size() > colorIndex)
            colors = SplitMultiColors(option->values[colorIndex]);
    }

    if (config != nullptr && config->has("filament_colour_mode"))
    {
        const ConfigOptionInts* option = config->option<ConfigOptionInts>("filament_colour_mode");
        if (option != nullptr && option->values.size() > colorIndex)
            mode = FilamentColorModeFromConfig(option->values[colorIndex]);
    }

    return FilamentColor::FromColors(colors, mode, fallbackColor);
}

nlohmann::json BuildPreprintColorMultiItem(const std::string& multiColors, FilamentColorMode mode,
                                           const std::string& fallbackColor)
{
    const FilamentColor color = FilamentColor::FromMultiColors(multiColors, mode, fallbackColor);

    nlohmann::json out_colors = nlohmann::json::array();
    for (const std::string& item : color.colors)
        out_colors.push_back(item.substr(1));

    nlohmann::json item;
    item["mode"] = FilamentColorModeToConfig(color.NormalizedMode());
    item["nums"] = color.colors.size();
    item["colors"] = out_colors;
    return item;
}

wxBitmap* GetFilamentColorIcon(const std::vector<std::string>& colors, FilamentColorMode mode, const std::string& label,
                               int iconWidth, int iconHeight, const wxColour& lightBorderColor)
{
    const std::vector<std::string> normalized = NormalizedColorsOrFallback(colors, "#FFFFFF");
    return GetFilamentColorIconFromNormalized(normalized, mode, label, iconWidth, iconHeight, lightBorderColor);
}

wxBitmap* GetFilamentColorIcon(const std::string& multiColors, FilamentColorMode mode, const std::string& fallbackColor,
                               const std::string& label, int iconWidth, int iconHeight, const wxColour& lightBorderColor)
{
    std::vector<std::string> normalized = SplitMultiColors(multiColors);
    AddFallbackToNormalizedColors(normalized, fallbackColor);
    return GetFilamentColorIconFromNormalized(normalized, mode, label, iconWidth, iconHeight, lightBorderColor);
}

ImU32 ToImGuiColor(const std::string& color)
{
    const wxColour parsed = WxColorFromHex(NormalizeHexColor(color, "#FFFFFF"));
    return IM_COL32(parsed.Red(), parsed.Green(), parsed.Blue(), 255);
}

float ImGuiAverageLuminance(const FilamentColor& color)
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

ImVec4 ImGuiTextColorFor(const FilamentColor& color)
{
    if (ImGuiAverageLuminance(color) < 80.0f / 255.0f)
        return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    return ImVec4(0, 0, 0, 1.0f);
}

void DrawImGuiFilamentColorBlock(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max,
                                 const FilamentColor& color, float inset)
{
    if (draw_list == nullptr || color.colors.size() <= 1)
        return;

    const ImVec2 fill_min(min.x + inset, min.y + inset);
    const ImVec2 fill_max(max.x - inset, max.y - inset);
    const float width = fill_max.x - fill_min.x;
    const float height = fill_max.y - fill_min.y;
    if (width <= 1.0f || height <= 1.0f)
        return;

    if (color.IsGradient())
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
