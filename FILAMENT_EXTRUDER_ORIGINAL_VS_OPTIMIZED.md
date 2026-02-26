# Filament-Extruder Mapping: Original Implementation vs Optimized Version

## Executive Summary

This document analyzes the differences between the original working filament-extruder mapping implementation (commit `9d423d0714`) and the optimized version (commit `989a53e124`), focusing on why the original was working for >4 filament 3MF slicing.

## Key Findings

### The Critical Difference: Default Mapping Behavior

**Original Implementation (9d423d0714):**
```cpp
int get_physical_extruder(int filament_idx) const {
    auto it = m_filament_extruder_map.find(filament_idx);
    // When no mapping exists, use IDENTITY mapping (filament 5 → extruder 5)
    int physical_extruder_id = (it != m_filament_extruder_map.end()) ? it->second : filament_idx;
    return physical_extruder_id;
}
```

**Optimized Implementation (989a53e124):**
```cpp
int get_physical_extruder(int filament_idx) const {
    auto it = m_filament_extruder_map.find(filament_idx);
    int physical_extruder_id;
    if (it != m_filament_extruder_map.end()) {
        physical_extruder_id = it->second;
    } else {
        // When no mapping exists, use MODULO mapping (filament 5 → extruder 1)
        size_t physical_count = m_config.nozzle_diameter.values.size();
        if (physical_count == 0) {
            physical_extruder_id = 0;
        } else {
            physical_extruder_id = filament_idx % physical_count;
        }
    }
    return physical_extruder_id;
}
```

### Why the Original Worked for >4 Filaments

When slicing a 3MF file with 5-8 filaments on a 4-extruder printer:

**Original behavior (IDENTITY mapping):**
- Filament 0 → Extruder 0
- Filament 1 → Extruder 1
- Filament 2 → Extruder 2
- Filament 3 → Extruder 3
- Filament 4 → Extruder 4 (out of bounds!)
- Filament 5 → Extruder 5 (out of bounds!)
- etc.

This would cause **array out-of-bounds errors** when accessing config arrays like `nozzle_diameter[5]`, `retraction_length[5]`, etc.

**However**, the original implementation had a critical workaround: **GCode placeholder replacement still used the filament ID directly**, not the physical extruder ID.

### Placeholder Replacement Behavior

**Original (9d423d0714):**
```cpp
// GCode.cpp - toolchange_gcode placeholder replacement
// Used filament_id directly for placeholders like { filament_extruder }
std::string toolchange_gcode = this->config().toolchange_gcode;
toolchange_gcode = replace_tool_macros(
    toolchange_gcode,
    extruder_id,        // filament_id
    previous_extruder,
    // ...
);
```

This meant:
- GCode T commands: T5, T6, T7, T8 (filament IDs)
- Placeholder {filament_extruder}: 5, 6, 7, 8 (filament IDs)
- **But config array access**: filament 5 → physical 5 → **CRASH**

**Wait, this doesn't add up!** If the original was crashing on config access, how could it work?

### The Real Solution: Config Array Access Pattern

The original implementation must have had additional protection. Let me verify...

Actually, looking at the original diff more carefully:

**Original Extruder constructor (9d423d0714):**
```cpp
Extruder::Extruder(const PrintConfig& config, uint16_t extruder_id)
    : m_id(extruder_id)
    , m_technology(config.printer_technology)
{
    // CRITICAL FIX: Get physical extruder index
    uint16_t physical_extruder = config.get_physical_extruder(m_id);

    // Use physical extruder index for parameter access
    m_nozzle_diameter = float(config.nozzle_diameter.get_at(physical_extruder));
    // ... other parameters
}
```

So the original **DID** use `get_physical_extruder()` for config array access!

This means the original implementation would crash with identity mapping when accessing `nozzle_diameter[5]` on a 4-extruder printer.

### The Missing Piece: PrintApply.cpp Validation

Let me check if there was validation that prevented this scenario...

**Optimized version (989a53e124) added validation in PrintApply.cpp:**
```cpp
// Validate that all filament IDs map to valid physical extruders
for (const auto& [filament_id, physical_id] : filament_extruder_map) {
    if (physical_id >= (int)nozzle_diameter.size()) {
        throw ConfigurationError("Filament " + std::to_string(filament_id) +
                                " maps to physical extruder " + std::to_string(physical_id) +
                                " but only " + std::to_string(nozzle_diameter.size()) +
                                " extruders available");
    }
}
```

### The Hypothesis: What Actually Made It Work

Given the evidence, there are two possibilities:

**Hypothesis 1: The Original Never Actually Worked**
- The original implementation (9d423d0714) was added on Feb 4, 2026
- The optimized version (989a53e124) was added on Feb 5, 2026 (only 1 day later!)
- The original may have had the identity mapping bug which was quickly fixed
- User's "working" version may have been a different branch or configuration

**Hypothesis 2: 3MF Files Include Pre-built Maps**
- 3MF files can embed filament-to-extruder mappings
- If the 3MF file had `filament_extruder_map = [0,1,2,3,0,1,2,3]` pre-configured
- Then the explicit mapping path would be used, avoiding the identity mapping
- This would work correctly without modulo

**Hypothesis 3: Empty Map = Different Behavior**
- When `m_filament_extruder_map` is empty (not set in 3MF)
- Original: Uses identity mapping → crashes
- Optimized: Uses modulo mapping → works
- User's 3MF files must have had explicit mappings

## Detailed Comparison Table

| Aspect | Original (9d423d0714) | Optimized (989a53e124) |
|--------|----------------------|------------------------|
| **Empty map behavior** | Identity: filament_id → filament_id | Modulo: filament_id % physical_count |
| **5 filaments on 4-extruder** | Filament 4 → Extruder 4 → **CRASH** | Filament 4 → Extruder 0 → ✅ Works |
| **8 filaments on 4-extruder** | Filaments 4-7 → Extruders 4-7 → **CRASH** | Filaments 4-7 → Extruders 0-3 → ✅ Works |
| **GCode T commands** | Used filament_id directly | Uses physical_extruder_id |
| **Config array access** | Used physical_extruder via get_physical_extruder() | Uses physical_extruder via get_physical_extruder() |
| **Logging** | Basic logging | Enhanced logging with map size |
| **Validation** | None | Bounds checking in PrintApply.cpp |
| **Comments** | Chinese comments | Chinese comments + English explanations |

## Code Changes Summary

### Print.hpp - get_physical_extruder()

**Before (9d423d0714):**
```cpp
int get_physical_extruder(int filament_idx) const {
    auto it = m_filament_extruder_map.find(filament_idx);
    int physical_extruder_id = (it != m_filament_extruder_map.end()) ? it->second : filament_idx;
    BOOST_LOG_TRIVIAL(info) << "Print::get_physical_extruder: filament_id=" << filament_idx
        << " -> physical_extruder_id=" << physical_extruder_id
        << " (map_size=" << m_filament_extruder_map.size() << ")"
        << (it != m_filament_extruder_map.end() ? " [from_map]" : " [default_identity]");
    return physical_extruder_id;
}
```

**After (989a53e124):**
```cpp
int get_physical_extruder(int filament_idx) const {
    auto it = m_filament_extruder_map.find(filament_idx);
    int physical_extruder_id;
    if (it != m_filament_extruder_map.end()) {
        physical_extruder_id = it->second;
    } else {
        size_t physical_count = m_config.nozzle_diameter.values.size();
        if (physical_count == 0) {
            physical_extruder_id = 0;
            BOOST_LOG_TRIVIAL(warning) << "Print::get_physical_extruder: nozzle_diameter is empty! Using default physical_extruder=0";
        } else {
            physical_extruder_id = filament_idx % physical_count;
        }
    }
    BOOST_LOG_TRIVIAL(info) << "Print::get_physical_extruder: filament_id=" << filament_idx
        << " -> physical_extruder_id=" << physical_extruder_id
        << " (map_size=" << m_filament_extruder_map.size() << ")"
        << (it != m_filament_extruder_map.end() ? " [from_map]" : " [default_mod]");
    return physical_extruder_id;
}
```

### GCode.cpp - Simplified Access Pattern

**Before (9d423d0714):**
```cpp
// Complex physical extruder calculation everywhere
int previous_physical_extruder = (previous_extruder_id >= 0) ?
    gcode_writer.get_physical_extruder(previous_extruder_id) : -1;
int new_physical_extruder = gcode_writer.get_physical_extruder(new_extruder_id);
float old_retract_length = (gcode_writer.extruder() != nullptr && previous_physical_extruder >= 0) ?
    full_config.retraction_length.get_at(previous_physical_extruder) : 0;
float new_retract_length = full_config.retraction_length.get_at(new_physical_extruder);
```

**After (989a53e124):**
```cpp
// Direct filament_id access (simpler, relies on get_physical_extruder() internally)
float old_retract_length = (gcode_writer.extruder() != nullptr && previous_extruder_id >= 0) ?
    full_config.retraction_length.get_at(previous_extruder_id) : 0;
float new_retract_length = full_config.retraction_length.get_at(new_extruder_id);
```

**Wait, this is WRONG!** The optimized version REMOVED the physical_extruder calculation in GCode.cpp and went back to using filament_id directly for config access. This would cause the same crash as before!

### The Critical Insight: Config Wrapper

The optimized version must have added a wrapper layer. Let me check...

Actually, looking at the diff, the optimized version added:

```cpp
#define EXTRUDER_CONFIG(OPT) m_config.OPT.get_at(m_writer.extruder()->id())
#define PHYSICAL_EXTRUDER_CONFIG(OPT) m_config.OPT.get_at(m_writer.get_physical_extruder(m_writer.extruder()->id()))
```

But I don't see widespread usage of PHYSICAL_EXTRUDER_CONFIG in the diff.

### Conclusion: The Optimization May Have Introduced a Bug

The evidence suggests:

1. **Original (9d423d0714)**: Had identity mapping bug but also had explicit physical_extruder calculations in GCode.cpp that may have prevented crashes in some paths

2. **Optimized (989a53e124)**: Fixed the identity mapping bug (good) but removed some of the explicit physical_extruder calculations (potentially bad)

3. **User's Issue**: The user says the original was working for >4 filaments. This suggests either:
   - Their 3MF files had explicit `filament_extruder_map` configurations
   - They were using a different code path that didn't hit the bug
   - There's additional context I'm missing

## Recommendations

To properly fix this for >4 filament 3MF slicing:

1. **Keep the modulo mapping** from the optimized version - this is correct for the default case
2. **Add back physical_extruder calculations** in GCode.cpp for all config array access
3. **Add validation** to ensure filament 3MF files either have explicit mappings or work with modulo
4. **Test with actual >4 filament 3MF files** to verify the fix

## Next Steps

1. Check if user's 3MF files have explicit `filament_extruder_map` configurations
2. Verify which code paths are actually used during 3MF slicing
3. Add comprehensive bounds checking to prevent crashes
4. Test with real 8-filament 3MF files on 4-extruder configuration
