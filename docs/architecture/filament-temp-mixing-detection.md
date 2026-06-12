# Filament Temperature Mixing Detection Mechanism

> Explored 2026-06-11. Covers the end-to-end mechanism that detects and handles mixing of high-temperature and low-temperature filaments on the same build plate.

## Overview

The mechanism prevents users from accidentally mixing incompatible filaments (e.g., PLA + ABS) on the same plate. It uses a **boolean flag** (`filament_is_high_temperature`) rather than comparing actual nozzle temperatures. Detection is per-plate, real-time (event-driven), and blocks slicing when mixing is detected unless the user has explicitly opted in via a preference.

## Three-State Model

Defined in `src/slic3r/GUI/Plater.hpp:235-239`:

```cpp
enum class FilamentTempMixingState {
    Compatible,     // No mixing, or plate is empty/invalid
    AllowedWarning, // Mixing detected but user enabled the preference -> warning only
    BlockedError    // Mixing detected and user has NOT enabled the preference -> blocked
};
```

## Key Config Flag

**`filament_is_high_temperature`** (ConfigOptionBools) — set in filament presets.

- **Definition**: `PrintConfig.cpp:2394`, `PrintConfig.hpp:1183`
- **UI**: `Tab.cpp:3694` — checkbox in Filament settings tab labeled "Is high-temperature filament"
- **In JSON presets**: e.g., ABS has `"1"`, PETG has `"0"`, PLA omits it (defaults false)

## Core Detection Algorithm

**`check_filament_temp_mixing(int plate_index)`** — `Plater.cpp:20215-20306`

1. Validates plate index (empty/invalid → Compatible)
2. Reads `filament_type` from global config to determine number of extruder slots
3. Skips if plate has no objects
4. Collects used filament slots from two sources:
   - **Config-based** (`collect_filament_slots_from_config`): checks `wall_filament`, `sparse_infill_filament`, `solid_infill_filament`, `support_filament`, `support_interface_filament`, `wipe_tower_filament`, default `extruder`
   - **Model-based** (`collect_filament_slots_from_model_config`): checks per-object and per-volume `extruder` config
5. Resolves global default extruder (`extruder=0`) for objects that use it
6. Looks up each used slot's preset and reads `filament_is_high_temperature` directly from the preset config
7. Returns `true` if compatible (no mixing), `false` if mixing detected

**`get_filament_temp_mixing_state(int plate_index)`** — `Plater.cpp:20313-20321`:
- If compatible → `Compatible`
- If mixing + user allowed → `AllowedWarning`
- If mixing + user not allowed → `BlockedError`

## End-User Preference

**`allow_filament_temp_mixing`** in Preferences dialog — `Preferences.cpp:1241`

- Toggling ON shows a risk warning dialog before enabling
- Toggling OFF directly triggers `notify_filament_usage_changed()`

## Notification Display

**`sync_filament_temp_mixing_notification()`** — `Plater.cpp:20354-20396`

| State | UI Behavior |
|-------|-------------|
| Compatible | Closes any existing error/warning notifications, clears plate invalid flag |
| AllowedWarning | Closes error notifications, shows **ValidateWarning**: "WARNING: Detected both high and low temperature materials..." |
| BlockedError | Shows **ValidateError** notification (persistent, no fade-out), calls `update_apply_result_invalid(true)` to block slicing |

## Slicing Block Layers

Four layers prevent slicing when `BlockedError`:

1. **Slice button**: `MainFrame.cpp:1907` — `get_enable_slice_status()` checks `is_plate_blocked_by_filament_temp_mixing()`
2. **`start_next_slice()`**: `Plater.cpp:19432` — early return `-1` if blocked
3. **`update_background_process()`**: `Plater.cpp:11843` — validates after `Print::validate()` passes, sets `UPDATE_BACKGROUND_PROCESS_INVALID`
4. **PartPlate state**: `PartPlate.hpp:405` — `can_slice()` returns `false` when `m_apply_invalid=true`

Pre-slice confirmation dialogs:
- `guard_before_slice_plate()` — `Plater.cpp:20398`
- `guard_before_slice_all()` — `Plater.cpp:20417`
- For `AllowedWarning`: shows "This material combination may cause risks. Continue?" [Confirm]/[Cancel]

## Event-Driven Trigger Chain

`notify_filament_usage_changed()` queues `EVT_FILAMENT_USAGE_CHANGED` via `wxQueueEvent` (deduplicated by a pending flag). The handler calls `sync_filament_temp_mixing_notification()`.

### All Trigger Points

| Trigger | File | Line |
|---|---|---|
| Filament preset changed in sidebar | Plater.cpp | 2949 |
| Object removed from plate (delete) | Plater.cpp | 11386 |
| All objects removed from current plate | Plater.cpp | 11354 |
| Object extruder assignment changed | GUI_ObjectList.cpp | 1103, 5926, 5986 |
| Object moved to different plate (drag-drop) | Plater.cpp | 11425 |
| 3MF project loaded | Plater.cpp | 17262 |
| Objects deleted from canvas | Plater.cpp | 18162 |
| Any config change via `on_config_change()` | Plater.cpp | 20553 |
| Plate selection changed | Plater.cpp | 21301 |
| Object move/rotate on canvas | GLCanvas3D.cpp | 4826 |
| Background process validation | Plater.cpp | 9142, 11847 |
| Slice initiated | Plater.cpp | 13772, 13794, 21195, 21252 |
| Preference toggle | Preferences.cpp | 765 |
| Tab selection changing | Plater.cpp | 9140-9142 |

## Notification State Caching

In `Plater::priv` (`Plater.cpp:8489-8491`):
```cpp
bool filament_temp_mixing_notification_initialized = false;
int  filament_temp_mixing_notification_plate  = -1;
FilamentTempMixingState filament_temp_mixing_notification_state = Compatible;
```

## G-Code Impact

`GCode.cpp:2711` — `filament_is_high_temperature` controls chamber cooling:
- Low-temp filaments → active cooling based on vitrification temperature
- High-temp filaments → chamber kept warm

## File Index

**GUI Layer:**
| File | Role |
|------|------|
| `src/slic3r/GUI/Plater.hpp` | Class declaration, enum, public API |
| `src/slic3r/GUI/Plater.cpp` | Core detection, notification sync, confirmation dialogs, all triggers |
| `src/slic3r/GUI/MainFrame.cpp` | Slice button enable/disable |
| `src/slic3r/GUI/Preferences.cpp` | `allow_filament_temp_mixing` checkbox |
| `src/slic3r/GUI/Tab.cpp` | `filament_is_high_temperature` checkbox in filament UI |
| `src/slic3r/GUI/NotificationManager.cpp` | push/close validate notifications |
| `src/slic3r/GUI/PartPlate.hpp` | `can_slice()`, `update_apply_result_invalid()`, `m_apply_invalid` |
| `src/slic3r/GUI/GUI_ObjectList.cpp` | Extruder assignment triggers |
| `src/slic3r/GUI/GLCanvas3D.cpp` | Object move/rotate triggers |

**Core Library:**
| File | Role |
|------|------|
| `src/libslic3r/PrintConfig.hpp` | Config declarations |
| `src/libslic3r/PrintConfig.cpp` | Config definitions |
| `src/libslic3r/GCode.cpp` | Chamber cooling logic |
| `src/libslic3r/PrintBase.hpp` | `STRING_EXCEPT_FILAMENTS_DIFFERENT_TEMP` |
| `src/libslic3r/Utils.hpp` | `CLI_FILAMENTS_DIFFERENT_TEMP` (-62) |
| `src/Snapmaker_Orca.cpp` | CLI error handling |

## Commit Timeline

```
62454d9c96 fix: unify filament temp mixing confirm dialog style and add translations
78a10be3f4 fix: per-plate filament temp mixing check using global config
2cd1e1b7d5 fix: add solid_infill_filament to Plater config for filament temp mixing detection
64b3d4fe02 Fix filament temperature mixing slice guards
0ec8144172 fix: rebuild filament mixing feature on current main base
9b2b9970d4 Make check_filament_temp_mixing per-plate instead of global
06debeb531 Add confirmation dialog with guard for allow_filament_temp_mixing preference toggle
9946414f20 Add preference option to allow high/low temperature filament mixing
fca2f3d2c4 feat: add real-time high/low temperature filament mixing detection (initial)
```
