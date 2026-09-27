# Snapmaker profiles: High Flow nozzles on the U1

Snapmaker Orca describes the Standard and the High Flow nozzle of a U1 tool head with the
extruder-variant columns of the preset system, not with the `[standard, high_flow]` value pairs the
Snapmaker 2.4 profiles use. The `*_flow_support` keys are kept in the converted files as a marker
for later syncs with the 2.4 profiles; the slicer ignores them.

## Which presets carry columns

| Preset | Columns | Column keys |
|---|---|---|
| `machine/Snapmaker U1 (0.4 nozzle).json` | 8: Standard and High Flow for each of the four tool heads | `extruder_variant_list`, `printer_extruder_id` (1,1,2,2,3,3,4,4), `printer_extruder_variant` |
| `process/0.20mm Standard @Snapmaker U1 (0.4 nozzle).json` | 2: Standard, High Flow, shared by all tool heads | `print_extruder_id` (1,1), `print_extruder_variant` |
| the 15 Snapmaker-brand U1 0.4 filaments whose `filament_flow_support` names `high_flow` | 2: Standard, High Flow | `filament_extruder_variant` |
| every other preset | 1 | none, or a single `Direct Drive Standard` |

`default_nozzle_volume_type` is `Standard` for every tool head, so nothing changes until a tool
head is switched to High Flow. A preset with one column is used for both nozzle types.

In the machine preset every key of `printer_options_with_variant_1` (`src/libslic3r/PrintConfig.cpp`)
holds 8 values and every key of `printer_options_with_variant_2` (normal, silent pairs) holds 16.
They are written out in full, including the values that come from `fdm_U1` and its parents: the
0.2 / 0.6 / 0.8 presets inherit the same parents without declaring the variant list, so the wide
vectors cannot move there. **When such a key changes in a parent, repeat the change in the 0.4
preset.**

## Invariants of the machine preset

- **M1** - no machine key differs between the Standard and the High Flow column of a tool head.
  High Flow values belong to filament and process presets only.
- **M2** - every (normal, silent) pair is identical on all tool heads. The preset composition of
  the application (`PresetBundle::full_config`) resolves these pairs after the single-value keys
  and would read another head's pair if they differed; the slicing path is not affected.

## Filament keys with a value per flow type

In a filament preset with two columns every key of `filament_options_with_variant` holds a Standard
and a High Flow value. Beyond mainline's keys that set contains the nine values the Snapmaker
filaments tune per nozzle: `pressure_advance`, `enable_pressure_advance`, `fan_min_speed`,
`fan_max_speed`, `additional_cooling_fan_speed`, `filament_multitool_ramming`,
`filament_multitool_ramming_volume`, `filament_multitool_ramming_flow` and
`filament_minimal_purge_on_wipe_tower`. A preset with one column states them once, as before.

A nullable filament pair such as `"filament_retraction_length": ["nil", "0.8"]` is intended: the
Standard nozzle keeps the printer's value, the High Flow nozzle overrides it.

`tests/libslic3r/test_snapmaker_hf_profiles.cpp` checks the column counts, M1, M2 and the values
copied from the parents.
