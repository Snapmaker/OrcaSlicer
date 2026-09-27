#pragma once

// How a GUI Choice combo for an enum option (coEnum / coEnums) maps between its row index and the
// stored enum value. Kept in libslic3r, free of wxWidgets, so tests can exercise the exact mapping
// Field.cpp (Choice::set_value / Choice::get_value) uses.
//
// The combo lists ConfigOptionDef::enum_values in order. For most options enum_values[i] is the key
// whose value is i, so the row index IS the stored value and Field uses it directly. The options
// named in enum_choice_maps_by_key() do not line up that way - they list a subset of the enum, or
// the enum has a gap - so Field maps them through the key instead: value -> key -> row on display,
// row -> key -> value on pick. The test "Enum options list their keys in the order of their values"
// (tests/libslic3r/test_config.cpp) fails for any other option whose list is out of order.

#include "Config.hpp"

#include <string>

namespace Slic3r {

inline bool enum_choice_maps_by_key(const std::string &opt_key)
{
    return
        // The infill pattern menus each list a subset of InfillPattern.
        opt_key == "top_surface_pattern" || opt_key == "undertop_surface_pattern" || opt_key == "bottom_surface_pattern" ||
        opt_key == "internal_solid_infill_pattern" || opt_key == "sparse_infill_pattern" ||
        opt_key == "support_base_pattern" || opt_key == "support_interface_pattern" ||
        opt_key == "ironing_pattern" || opt_key == "support_ironing_pattern" ||
        // Locked Zag per-band patterns: a subset that starts with "default" (ipCount = 30).
        opt_key == "locked_skin_infill_pattern" || opt_key == "locked_skeleton_infill_pattern" ||
        opt_key == "support_style" || opt_key == "curr_bed_type" ||
        // NozzleVolumeType skips 4: "E3D High Flow" is value 5 at row 4.
        opt_key == "nozzle_volume_type" || opt_key == "default_nozzle_volume_type" || opt_key == "extruder_nozzle_volume_type";
}

// Row of the combo that shows the stored enum `value`: the position in def.enum_values of a key whose
// value is `value`. -1 when no listed key has that value (or the option has no key map).
inline int enum_choice_index_of_value(const ConfigOptionDef &def, int value)
{
    if (def.enum_keys_map == nullptr)
        return -1;
    for (size_t i = 0; i < def.enum_values.size(); ++i) {
        const auto it = def.enum_keys_map->find(def.enum_values[i]);
        if (it != def.enum_keys_map->end() && it->second == value)
            return int(i);
    }
    return -1;
}

// Enum value stored when row `index` of the combo is picked. -1 when the row is out of range or its
// key is not in the option's key map.
inline int enum_choice_value_at_index(const ConfigOptionDef &def, int index)
{
    if (def.enum_keys_map == nullptr || index < 0 || index >= int(def.enum_values.size()))
        return -1;
    const auto it = def.enum_keys_map->find(def.enum_values[size_t(index)]);
    return it == def.enum_keys_map->end() ? -1 : it->second;
}

} // namespace Slic3r
