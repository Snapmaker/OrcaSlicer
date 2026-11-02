#pragma once

#include <vector>

#include "../libslic3r/MixedFilament.hpp"
#include "../MixedColorMatchHelpers.hpp"
#include "../GUI_App.hpp"

namespace Slic3r {
namespace GUI {

bool mixed_filament_uses_physical(const MixedFilament* target_mf, unsigned int source_physical_1based);

void extract_batch_kept_sets(const BatchMatchResult &result,
    size_t                  num_physical,
    std::vector<unsigned int> &kept_physical,
    std::vector<unsigned int> &kept_mixed);


} // namespace GUI
} // namespace Slic3r
