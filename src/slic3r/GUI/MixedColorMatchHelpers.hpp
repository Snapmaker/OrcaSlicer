#pragma once

// Shared declarations for color-match helpers used by both Plater.cpp and
// MixedColorMatchPanel.cpp. Implementations live in Plater.cpp.

#include "libslic3r/MixedFilament.hpp"

#include <wx/wx.h>
#include <wx/bitmap.h>

#include <limits>
#include <optional>
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

std::vector<unsigned int> decode_color_match_gradient_ids(const std::string& value);
std::vector<int>          decode_color_match_gradient_weights(const std::string& value, size_t expected_components);
MixedColorMatchRecipeResult build_pair_color_match_candidate(const std::vector<wxColour>& palette,
                                                             unsigned int                 component_a,
                                                             unsigned int                 component_b,
                                                             int                          mix_b_percent,
                                                             int                          min_component_percent = 0);
MixedColorMatchRecipeResult build_multi_color_match_candidate(const std::vector<wxColour>&     palette,
                                                              const std::vector<unsigned int>& ids,
                                                              const std::vector<int>&          weights,
                                                              int                              min_component_percent = 0);
bool color_match_weights_within_range(const std::vector<int>& weights, int min_component_percent);
std::vector<unsigned int>   build_color_match_sequence(const std::vector<unsigned int>& ids, const std::vector<int>& weights);
wxColour                    blend_sequence_filament_mixer(const std::vector<wxColour>& palette, const std::vector<unsigned int>& sequence);

bool is_filament_compatible(const std::vector<unsigned int>& filament_ids);
bool is_filament_compatible(const MixedFilament& mf);
// Returns std::nullopt if all compatible (or insufficient data),
// otherwise the 1-based filament IDs of the first incompatible pair.
std::optional<std::pair<unsigned int, unsigned int>> find_incompatible_filament_pair(const std::vector<unsigned int>& filament_ids);

// Result of parsing a normalized cycle-mode manual pattern.
struct CyclePatternParseResult {
    std::string              invalid_token; // first token that failed strtoul
    unsigned int             invalid_id = 0; // first out-of-range 1-based ID (0 = none)
    std::vector<unsigned int> ids;           // valid 1-based filament IDs in parse order
    int                      total_tokens = 0;
};

// One-shot parse of a normalized cycle pattern: split→token→strtoul→validate.
CyclePatternParseResult parse_cycle_pattern(const std::string& normalized_pattern, int num_physical);

// Parse a normalized cycle pattern and produce a human-readable percentage summary.
// Format: "F1 60%+F2 40%". Returns empty string on invalid input.
std::string summarize_cycle_pattern_text(const std::string& normalized_pattern,
                                         const MixedFilament& entry,
                                         int num_physical);

}} // namespace Slic3r::GUI
