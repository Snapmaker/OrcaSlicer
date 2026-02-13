# Filament-Extruder Mapping Fix - Test Plan

## Prerequisites
- 8 filaments configured
- 4 physical extruders
- Mapping: 5→1, 6→2, 7→3, 8→4 (0-indexed: 4→0, 5→1, 6→2, 7→3)

## Test Case 1: z_hop_types Override

### Setup
1. Set different z_hop_types for each extruder:
   - Extruder 1: Normal
   - Extruder 2: Spiral
   - Extruder 3: Normal
   - Extruder 4: Spiral

2. For filaments 1-4: Enable override with different values
3. For filaments 5-8: Disable override (uncheck)

### Expected Result
| Filament | Override | Expected z_hop_type |
|----------|----------|---------------------|
| 1 | Enabled | Filament 1's value |
| 2 | Enabled | Filament 2's value |
| 3 | Enabled | Filament 3's value |
| 4 | Enabled | Filament 4's value |
| 5 | Disabled | Extruder 1's value (Normal) |
| 6 | Disabled | Extruder 2's value (Spiral) |
| 7 | Disabled | Extruder 3's value (Normal) |
| 8 | Disabled | Extruder 4's value (Spiral) |

## Test Case 2: retraction_length Override

### Setup
1. Set different retraction_length for each extruder:
   - Extruder 1: 0.8mm
   - Extruder 2: 1.0mm
   - Extruder 3: 1.2mm
   - Extruder 4: 1.4mm

2. For filaments 5-8: Disable override

### Expected Result
| Filament | Expected retraction_length |
|----------|---------------------------|
| 5 | 0.8mm (from Extruder 1) |
| 6 | 1.0mm (from Extruder 2) |
| 7 | 1.2mm (from Extruder 3) |
| 8 | 1.4mm (from Extruder 4) |

## Test Case 3: Modifying Extruder Settings

### Steps
1. Uncheck override for filament 5 (maps to extruder 1)
2. Change extruder 1's z_hop_type from Normal to Spiral
3. Re-slice

### Expected Result
- Filament 5 should now use Spiral (inherited from extruder 1)
- Filaments 1-4 with override enabled should NOT change

## Debug Log Verification

After slicing, check logs for these patterns:

### Expected log output:
```
DEBUG_MAP_INDICES_RESULT: z_hop_types [0->0 1->1 2->2 3->3 4->0 5->1 6->2 7->3 ]
DEBUG_APPLY_OVERRIDE_IDX: i=4 nil=Y map_idx=0 inherited=<extruder 1 value>
DEBUG_APPLY_OVERRIDE_IDX: i=5 nil=Y map_idx=1 inherited=<extruder 2 value>
DEBUG_APPLY_OVERRIDE_IDX: i=6 nil=Y map_idx=2 inherited=<extruder 3 value>
DEBUG_APPLY_OVERRIDE_IDX: i=7 nil=Y map_idx=3 inherited=<extruder 4 value>
```

### G-code Verification
In the generated G-code, verify that retraction commands for each filament use the correct parameters.

## Build Command

```bash
build_release_vs2022.bat slicer
```

## Success Criteria

1. ✅ Compilation succeeds without errors
2. ✅ Application starts without crashes
3. ✅ Filaments 5-8 inherit from correct mapped extruders
4. ✅ Filaments 1-4 with override enabled keep their own values
5. ✅ Modifying extruder settings updates inherited values for non-overridden filaments
