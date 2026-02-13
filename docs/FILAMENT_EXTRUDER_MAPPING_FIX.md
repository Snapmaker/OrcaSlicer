# Filament-Extruder Mapping Fix

## Problem Description

In an 8-filament, 4-extruder system with mapping (5→1, 6→2, 7→3, 8→4):
- **Bug**: When unchecking parameter override for filaments 5-8, they all incorrectly inherited from extruder 1
- **Expected**: Each filament should inherit from its mapped physical extruder

## Root Cause

`ConfigOptionVector<T>::apply_override()` used `values[0]` as default for nil entries, instead of using the mapped physical extruder's value.

## Files Modified

| File | Change |
|------|--------|
| `Config.hpp` | Added `apply_override(rhs, map_indices)` overload |
| `PrintApply.cpp` | Build `map_indices`, call new overload |
| `GCodeProcessor.hpp/cpp` | Added modulo fallback for `get_physical_extruder()` |
| `CoolingBuffer.hpp/cpp` | Added modulo fallback for `get_physical_extruder()` |
| `GCode.cpp` | Added `m_cooling_buffer->set_filament_extruder_map()` |

## Technical Details

### 1. Config.hpp - New apply_override overload

```cpp
bool apply_override(const ConfigOption *rhs, const std::vector<int>& map_indices) override {
    std::vector<T> default_values = this->values;  // Save physical extruder values

    for (size_t i = 0; i < rhs_vec->size(); ++i) {
        if (!rhs_vec->is_nil(i)) {
            this->values[i] = rhs_vec->values[i];  // Use filament's own value
        } else {
            // Inherit from mapped physical extruder
            this->values[i] = default_values[map_indices[i]];  // KEY FIX
        }
    }
}
```

### 2. PrintApply.cpp - Build map_indices

```cpp
map_indices.resize(filament_count, 0);
for (size_t i = 0; i < filament_count; ++i) {
    auto it = filament_extruder_map.find((int)i);
    if (it != filament_extruder_map.end()) {
        map_indices[i] = it->second;
    } else {
        map_indices[i] = (int)(i % physical_extruder_count);  // Fallback
    }
}
```

### 3. Unified Fallback Strategy

All components now use the same modulo fallback:
```cpp
int get_physical_extruder(int filament_idx) const {
    auto it = m_filament_extruder_map.find(filament_idx);
    if (it != m_filament_extruder_map.end()) {
        return it->second;
    }
    return filament_idx % m_physical_extruder_count;  // Unified fallback
}
```

## Test Scenario

1. **Config**: 8 filaments, 4 physical extruders
2. **Setup**: Uncheck `z_hop_types` override for filaments 5-8
3. **Expected Result**:
   - Filament 5 → inherits from extruder 1
   - Filament 6 → inherits from extruder 2
   - Filament 7 → inherits from extruder 3
   - Filament 8 → inherits from extruder 4

## Build Command

```bash
build_release_vs2022.bat slicer
```

## Debug Keywords (in logs)

- `DEBUG_MAP_INDICES_RESULT` - Shows mapping indices
- `DEBUG_APPLY_OVERRIDE_IDX` - Shows each filament's inherited value
