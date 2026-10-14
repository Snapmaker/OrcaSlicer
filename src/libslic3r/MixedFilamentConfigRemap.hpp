#pragma once

#include "PrintConfig.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

/// Feature-filament option keys that can be overridden at object or volume level.
inline const std::vector<std::string> &mixed_filament_feature_keys()
{
    static const std::vector<std::string> keys = {
        "wall_filament",
        "sparse_infill_filament",
        "solid_infill_filament",
        "support_filament",
        "support_interface_filament",
    };
    return keys;
}

/// Map a 1-based filament ID through a deletion remap table.
///
/// The table is indexed by the old 1-based ID and contains the new 1-based ID.
/// Zero means that the old filament no longer exists and the caller should use
/// its default behavior. `total_filaments` is a defensive upper bound.
inline unsigned int remap_filament_config_id(int old_id,
                                             const std::vector<unsigned int> &id_remap,
                                             size_t total_filaments)
{
    if (old_id <= 0 || static_cast<size_t>(old_id) >= id_remap.size())
        return 0;

    const unsigned int mapped_id = id_remap[static_cast<size_t>(old_id)];
    return static_cast<size_t>(mapped_id) > total_filaments ? 0 : mapped_id;
}

/// Remap object/volume config-level filament references after a deletion.
///
/// `extruder` uses explicit zero for the default filament, matching the existing
/// mixed-filament cleanup convention. Feature-filament overrides are erased when
/// their old filament is gone so the corresponding global default takes effect.
inline void remap_model_config_filament_ids(ModelConfig &config,
                                             const std::vector<unsigned int> &id_remap,
                                             size_t total_filaments)
{
    if (config.has("extruder")) {
        const int old_id = config.extruder();
        const unsigned int mapped_id = remap_filament_config_id(old_id, id_remap, total_filaments);
        if (mapped_id == 0) {
            if (old_id > 0)
                config.set("extruder", 0);
        } else {
            config.set("extruder", static_cast<int>(mapped_id));
        }
    }

    for (const std::string &key : mixed_filament_feature_keys()) {
        if (!config.has(key))
            continue;

        const int old_id = config.opt_int(key);
        const unsigned int mapped_id = remap_filament_config_id(old_id, id_remap, total_filaments);
        if (mapped_id == 0) {
            if (old_id > 0)
                config.erase(key);
        } else {
            config.set(key, static_cast<int>(mapped_id));
        }
    }
}

/// Remap feature-filament keys in a global DynamicPrintConfig after a deletion.
inline void remap_dynamic_config_feature_filament_ids(DynamicPrintConfig &config,
                                                       const std::vector<unsigned int> &id_remap,
                                                       size_t total_filaments)
{
    for (const std::string &key : mixed_filament_feature_keys()) {
        if (!config.has(key))
            continue;

        const int old_id = config.opt_int(key);
        const unsigned int mapped_id = remap_filament_config_id(old_id, id_remap, total_filaments);
        if (mapped_id == 0) {
            if (old_id > 0)
                config.erase(key);
        } else {
            config.set(key, static_cast<int>(mapped_id));
        }
    }
}

} // namespace Slic3r
