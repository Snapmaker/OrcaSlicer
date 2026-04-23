#pragma once

// Shared declarations for color-match helpers used by both Plater.cpp and
// MixedColorMatchPanel.cpp. Implementations live in Plater.cpp.

#include "libslic3r/MixedFilament.hpp"

#include <wx/wx.h>
#include <wx/bitmap.h>

#include <limits>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

struct MixedColorMatchRecipeResult
{
    bool         cancelled     = false;
    bool         valid         = false;
    unsigned int component_a   = 1;
    unsigned int component_b   = 2;
    int          mix_b_percent = 50;
    std::string  manual_pattern;
    std::string  gradient_component_ids;
    std::string  gradient_component_weights;
    wxColour     preview_color = wxColour("#26A69A");
    double       delta_e       = std::numeric_limits<double>::infinity();
};

// ---- small pure helpers (defined here, used everywhere) ----

wxColour parse_mixed_color(const std::string &value);
wxString normalize_color_match_hex(const wxString &value);
bool     try_parse_color_match_hex(const wxString &value, wxColour &color_out);

std::vector<int> normalize_color_match_weights(const std::vector<int> &weights, size_t count);
std::vector<int> expand_color_match_recipe_weights(const MixedColorMatchRecipeResult &recipe, size_t num_physical);
std::string      summarize_color_match_recipe(const MixedColorMatchRecipeResult &recipe);
wxBitmap         make_color_match_swatch_bitmap(const wxColour &color, const wxSize &size);

std::vector<MixedColorMatchRecipeResult> build_color_match_presets(
    const std::vector<std::string> &physical_colors,
    int                             min_component_percent = 0);

double color_delta_e00(const wxColour &lhs, const wxColour &rhs);

MixedColorMatchRecipeResult build_best_color_match_recipe(
    const std::vector<std::string> &physical_colors,
    const wxColour                 &target_color,
    int                             min_component_percent = 0);

// ---- display context helpers ----

MixedFilamentDisplayContext build_mixed_filament_display_context(
    const std::vector<std::string> &physical_colors);

wxColour compute_color_match_recipe_display_color(
    const MixedColorMatchRecipeResult  &recipe,
    const MixedFilamentDisplayContext  &context);

MixedColorMatchRecipeResult prompt_best_color_match_recipe(wxWindow *parent,
                                                           const std::vector<std::string> &physical_colors,
                                                           const wxColour &initial_color);

}} // namespace Slic3r::GUI
