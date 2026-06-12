#pragma once

#include <imgui/imgui.h>
#include <nlohmann/json.hpp>
#include <wx/bitmap.h>
#include <wx/colour.h>

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r
{

class DynamicPrintConfig;

namespace GUI
{
namespace FilamentColorUtils
{

struct FilamentColorDisplay
{
    std::string primaryColor;
    std::vector<std::string> colors;
    int mode { 0 }; // 0 = solid/averaged, 1 = gradient
};

std::string NormalizeHexColor(const std::string& color);
std::string NormalizeHexColor(const std::string& color, const std::string& fallback_color);

std::string StripHashForPreprint(const std::string& color);
std::string StripHashForPreprint(const std::string& color, const std::string& fallback_color);

std::vector<std::string> SplitMultiColors(const std::string& value);
std::string JoinMultiColors(const std::vector<std::string>& colors);

std::string GetPrimaryColor(const std::vector<std::string>& colors, const std::string& fallback_color);

int NormalizeColourMode(int mode);
std::string GetFilamentMatchName(const std::string& name);

FilamentColorDisplay GetFilamentColorDisplay(const DynamicPrintConfig* config, size_t color_index, const std::string& fallback_color);

nlohmann::json BuildPreprintColorMultiItem(const std::string& multi_colors, int mode, const std::string& fallback_color);

wxBitmap* GetFilamentColorIcon(const std::vector<std::string>& colors, int mode, const std::string& label, int iconWidth, int iconHeight,
                               const wxColour& lightBorderColor = wxNullColour);

wxBitmap* GetFilamentColorIcon(const std::string& multiColors, int mode, const std::string& fallbackColor, const std::string& label,
                               int iconWidth, int iconHeight, const wxColour& lightBorderColor = wxNullColour);

ImU32 ToImGuiColor(const std::string& color);
float ImGuiAverageLuminance(const FilamentColorDisplay& color);
ImVec4 ImGuiTextColorFor(const FilamentColorDisplay& color);

void DrawImGuiFilamentColorBlock(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max, const FilamentColorDisplay& color, float inset = 0.0f);

} // namespace FilamentColorUtils
} // namespace GUI
} // namespace Slic3r
