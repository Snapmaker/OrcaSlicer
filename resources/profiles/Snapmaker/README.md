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

## Process speeds on mixed nozzle sizes

A U1 process preset is made for one nozzle size. When the tool heads carry different sizes, a head
of another size than the printer preset prints with the speed, acceleration and jerk columns of a
system process preset of its own size (`src/libslic3r/PerHeadProcess.hpp`). The preset is chosen by
intent: among the process presets compatible with the machine preset of the head's size, the one
whose layer height equals the head's preferred layer height (else the selected preset's layer height)
and whose quality class (the text between `mm ` and ` @`, "Standard", "High Quality") is the selected
preset's; without an exact height the nearest one of the same class; else the machine preset's
`default_print_profile`. So a 0.2 mm head at a preferred 0.12 mm under `0.20mm Standard` prints with
`0.12mm Standard @Snapmaker U1 (0.2 nozzle)`, at 0.10 mm with `0.10mm High Quality`.

Composed are 32 of the 42 variant keys: the 17 role speeds, the 8 accelerations and the 7 jerk /
junction deviation keys. The 7 travel keys, `enable_overhang_speed`, `slowdown_for_curled_perimeters`
and `small_perimeter_threshold` stay the selected preset's on every head. Line widths, walls, shells,
infill, support, layer heights and the prime tower stay the selected preset's as well (widths given
in percent already follow each nozzle). A value the user changed in the selected preset against its
system parent keeps the user's value on every head. An override on a part is an absolute value and
applies on every tool head that prints the part, also on one that otherwise prints with the speeds of
its own nozzle size. A head switched to High Flow takes its source's High Flow column when the source
has one, else the selected preset's High Flow column, never the source's Standard column.

The project records the preset of every head in `extruder_process_preset` (one name per tool head,
empty for a head that prints with the selected preset); a load compares the record with the current
choice and reports a difference, it never switches. Older readers ignore the key. The command line
does not compose the table yet and logs a warning per recorded head. The option lives under
Preferences > Preset ("Process speeds follow the nozzle size"), on by default.

Pre-existing and unchanged: `PerimeterGenerator.cpp` resolves the wall / infill overlap spacing
against the outer-wall filament's nozzle, so a region whose walls and infill print on heads of
different sizes uses the wall head's spacing.
