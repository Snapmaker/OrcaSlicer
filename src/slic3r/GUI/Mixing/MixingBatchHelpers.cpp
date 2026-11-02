#include "MixingBatchHelpers.hpp"
#include "../I18N.hpp"

namespace Slic3r {
namespace GUI {

bool mixed_filament_uses_physical(const MixedFilament* target_mf, unsigned int source_physical_1based)
{
    if (!target_mf)
        return false;

    // Check manual_pattern tokens (resolve order #1)
    const std::string norm = MixedFilamentManager::normalize_manual_pattern(target_mf->manual_pattern);
    if (!norm.empty()) {
        const auto groups = MixedFilamentManager::split_pattern_groups(norm);
        for (const std::string &group : groups) {
            const auto tokens = MixedFilamentManager::split_pattern_group_to_tokens(group, 0);
            for (const std::string &token : tokens) {
                if (MixedFilamentManager::physical_filament_from_token(token, *target_mf, MixedFilamentManager::kMaxPhysicalFilaments) == source_physical_1based)
                    return true;
            }
        }
    }

    // Check gradient components (resolve order #2).
    // Only check when there is no manual_pattern; a pattern already resolves
    // every token, so gradient IDs would be a false positive at worst.
    if (norm.empty()) {
        const std::vector<unsigned int> ids = MixedFilamentManager::decode_gradient_component_ids(target_mf->gradient_component_ids, 0);
        for (unsigned int id : ids) {
            if (id == source_physical_1based)
                return true;
        }
    }

    // Check if target mixed filament uses source physical filament as component
    // (resolve order #3). Only reached when the mixed filament has no manual_pattern,
    // because in cycle mode pattern tokens "1"/"2" already cover component_a/b.
    if (norm.empty() && (target_mf->component_a == source_physical_1based || target_mf->component_b == source_physical_1based)) {
        return true;
    }

    return false;
}

void extract_batch_kept_sets(const BatchMatchResult &result,
                                    size_t                  num_physical,
                                    std::vector<unsigned int> &kept_physical,
                                    std::vector<unsigned int> &kept_mixed)
{
    kept_physical.clear();
    kept_mixed.clear();
    kept_physical.reserve(result.selected_physical_ids.size());
    for (unsigned int id : result.selected_physical_ids) {
        if (id >= 1 && id <= num_physical)
            kept_physical.push_back(id);
    }
    kept_mixed.reserve(result.mappings.size());
    for (const auto &m : result.mappings) {
        if (!m.is_pure_recipe && m.target_filament_id > num_physical)
            kept_mixed.push_back(m.target_filament_id);
    }
}

} // namespace GUI
} // namespace Slic3r
