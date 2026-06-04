#pragma once

#include <nlohmann/json.hpp>
#include <wx/bitmap.h>

#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace FilamentColorUtils {

std::string normalize_hex_color(const std::string& color);
std::string normalize_hex_color(const std::string& color, const std::string& fallback_color);

std::string strip_hash_for_preprint(const std::string& color);
std::string strip_hash_for_preprint(const std::string& color, const std::string& fallback_color);

std::vector<std::string> split_multi_colors(const std::string& value);
std::string join_multi_colors(const std::vector<std::string>& colors);

std::string get_primary_color(const std::vector<std::string>& colors, const std::string& fallback_color);

int normalize_colour_mode(int mode);

nlohmann::json build_preprint_color_multi_item(const std::string& multi_colors, int mode, const std::string& fallback_color);

wxBitmap* get_filament_color_icon(const std::vector<std::string>& colors,
                                  int                             mode,
                                  const std::string&              label,
                                  int                             icon_width,
                                  int                             icon_height);

wxBitmap* get_filament_color_icon(const std::string& multi_colors,
                                  int                mode,
                                  const std::string& fallback_color,
                                  const std::string& label,
                                  int                icon_width,
                                  int                icon_height);

}}} // namespace Slic3r::GUI::FilamentColorUtils
