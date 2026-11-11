#pragma once

#include "libslic3r/MixedFilament.hpp"

#include <string>
#include <tuple>
#include <vector>

namespace Slic3r {

// PRD 2.0 color-family order, then lightness descending, hue ascending,
// saturation descending and normalized HEX. Independent of recipe order.
using MixedColorSortKey = std::tuple<int, double, double, double, std::string>;
MixedColorSortKey mixed_color_sort_key(const std::string& hex);

struct SavedMixedFilament
{
    MixedFilamentDefinition definition;
    unsigned int            filament_id = 0;
    bool                    removed     = false;
};

struct MixedFilamentRecommendation
{
    MixedFilamentDefinition definition;
    std::vector<size_t>     saved_indices;
    bool                    selected = false;

    bool registered() const { return !saved_indices.empty(); }
};

// A dialog-local snapshot. Selecting, removing and cancelling never mutate the
// project. All potentially failing preparation happens on a manager copy.
class MixedFilamentRecommendations
{
public:
    static constexpr size_t MAX_PHYSICAL_FILAMENTS = 16;

    MixedFilamentRecommendations(const MixedFilamentDisplayContext&          context,
                                 const std::vector<std::string>&             material_types,
                                 const std::vector<MixedFilamentDefinition>& definitions);

    std::vector<SavedMixedFilament>          saved;
    std::vector<MixedFilamentRecommendation> recommendations;
    size_t                                   representative_count  = 0;
    bool                                     excluded_incompatible = false;

    size_t additions(size_t component_count = 0) const;
    size_t removals() const;
    // 0 = none, 1 = some, 2 = all unregistered candidates selected.
    int                  selection_state(size_t component_count = 0) const;
    void                 select_all(bool selected, size_t component_count = 0);
    bool                 exceeds_limit() const;
    MixedFilamentManager prepare(const MixedFilamentManager& current) const;

private:
    MixedFilamentDisplayContext          m_context;
    std::vector<MixedFilamentDefinition> m_definitions;
};

} // namespace Slic3r
