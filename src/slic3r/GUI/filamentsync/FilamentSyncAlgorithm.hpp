#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <vector>

#include "FilamentData.hpp"

namespace Slic3r {
namespace GUI {

// Closest-color matching using CIE76 Delta E in CIE Lab (D65).
// Returns design_index -> machine_index mapping; -1 means unmatched.
std::vector<int> compute_color_match(
    const std::list<FilamentData>& design_colors,
    const std::list<FilamentData>& machine_colors);

// Sequential 1:1 override: design[i] -> machine[i] until either list is exhausted.
std::vector<int> compute_direct_override(
    size_t design_count,
    size_t machine_count);

// CIE76 perceptual color distance between two sRGB colors.
// Lower value = perceptually closer.
float delta_e_cie76(uint8_t r1, uint8_t g1, uint8_t b1,
                    uint8_t r2, uint8_t g2, uint8_t b2);

} // namespace GUI
} // namespace Slic3r
