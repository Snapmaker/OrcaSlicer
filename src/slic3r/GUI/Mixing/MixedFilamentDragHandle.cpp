#include "MixedFilamentDragHandle.hpp"
#include "../I18N.hpp"

#include <wx/settings.h>

namespace Slic3r {
namespace GUI {

std::vector<size_t> build_mixed_filament_ui_indices(const std::vector<MixedFilament> &mixed,
                                                           const std::vector<uint64_t>      &preferred_order)
{
    std::vector<size_t> ordered_indices;
    std::vector<bool>   used(mixed.size(), false);

    for (const uint64_t stable_id : preferred_order) {
        for (size_t idx = 0; idx < mixed.size(); ++idx) {
            const MixedFilament &entry = mixed[idx];
            if (used[idx] || entry.deleted || entry.stable_id != stable_id)
                continue;
            used[idx] = true;
            ordered_indices.emplace_back(idx);
            break;
        }
    }

    for (size_t idx = 0; idx < mixed.size(); ++idx) {
        if (used[idx] || mixed[idx].deleted)
            continue;
        ordered_indices.emplace_back(idx);
    }

    return ordered_indices;
}

} // namespace GUI
} // namespace Slic3r
