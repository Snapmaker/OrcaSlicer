#pragma once

#include <set>
#include <vector>

namespace Slic3r { namespace GUI {

// Every plate owns one Print and one GCodeResult, which PartPlateList files under a "print index".
// Two plates must never resolve to the same index. If they do, they share one Print, and that Print
// carries the plate origin of whichever plate bound to it last. Slicing the other plate then writes
// all of its G-code one plate stride off the bed (plate 1 comes out at negative X) and draws its
// prime tower on the neighbouring plate. The slice fails, and only a restart clears it.

// The first index at or after `next` that neither map files anything under. `next` moves past it.
template<class PrintMap, class ResultMap>
int claim_free_print_index(const PrintMap &prints, const ResultMap &results, int &next)
{
    if (next < 0)
        next = 0;
    while (prints.count(next) > 0 || results.count(next) > 0)
        ++next;
    return next++;
}

// After an undo/redo restore, decide for each plate (in plate order) whether it may bind to the
// Print filed under its saved index. It may when that index is still registered and no earlier
// plate has taken it already. A plate that may not needs a Print of its own.
template<class IsRegistered>
std::vector<bool> restored_plates_keep_print(const std::vector<int> &saved_indices, IsRegistered is_registered)
{
    std::vector<bool> keep(saved_indices.size(), false);
    std::set<int>     taken;
    for (size_t i = 0; i < saved_indices.size(); ++i) {
        const int idx = saved_indices[i];
        keep[i]       = idx >= 0 && is_registered(idx) && taken.insert(idx).second;
    }
    return keep;
}

}} // namespace Slic3r::GUI
