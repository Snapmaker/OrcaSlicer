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

High Flow follows the nozzle size of the tool head, not the size of the printer preset: a 0.4 mm
tool head on the 0.2 / 0.6 / 0.8 mm preset may run High Flow because the 0.4 mm machine preset
declares it. Such a head reads its own Standard machine column, which holds the values of the 0.4 mm
preset once the sidebar set its size (M1: they equal the High Flow ones), and the High Flow columns
of its filament and process presets, as it would on the 0.4 mm preset.

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

## Speeds per tool head

The Speed page of the Process tab carries a selector `All tool heads | Head 1 | ... | Head N` on
every printer with more than one extruder, except a Bambu two-head printer, whose process presets
name their tool heads in `print_extruder_id` and keep mainline's row. Under All the fields edit the
shared columns of the preset (the Standard / High Flow toggle picks the column when the preset has
two); under a tool head they show the value that head prints with (its own, else the composed
source's, else the shared value) and an edit sets the value for that head alone. 38 of the 42
variant keys can be set per tool head (the 39 value keys other than `enable_overhang_speed`,
`slowdown_for_curled_perimeters` and `small_perimeter_threshold`, which stay uniform; `travel_speed_z`
has no field). An edit under All skips a tool head that has a value of its own for the key; the line
under the selector says so and offers the clear of the values set.

Storage (`src/libslic3r/PerHeadProcess.hpp`): while at least one value is set per tool head the
preset is laid out as the parent's flow columns with id 0 (the shared columns) followed by the
printer's columns: the U1 child `0.20mm Standard` becomes `print_extruder_id` 0,0,1,1,2,2,3,3,4,4
with Standard / High Flow per head, a one-column preset 0,1,1,2,2,3,3,4,4. The key
`print_extruder_override` names, per column, the keys set for the column's tool head; it is empty on
a shared column. Without a value set the preset keeps its vendor layout byte for byte. A value set
for a tool head lives in both of its flow columns, so a flow change never strands it. It is bound to
the nozzle size it was set for (`print_extruder_value_nozzle`): kept, not printed, while the head
carries another size. Precedence at slice time: an
override of an object, part or layer range (as wide as the shared columns, read by the flow of each
head) > the value set for the tool head > a value changed under All > the composed source of an
off-size head > the vendor column. The preference "Process speeds follow the nozzle size" gates the
composition only: values set per tool head apply with it off as well.

Compatibility: no head lookup matches id 0, so `Print::apply`, the composer and mainline OrcaSlicer
read the head columns exactly and drop the shared ones; Snapmaker Orca 2.4 reads column 0, the
shared Standard column, so every head prints the shared values after its schema notice (the same
as a project of the two-column preset today). A user preset saves its columns and marker as diffs
against the parent laid out like the preset and reloads with every column; a project lists every
widened key in `different_settings_to_system` so that a load keeps them; the command line honours the
stored columns and lays the newest system preset out like the project before restoring the kept
keys. The `.opc` preset cache refuses a cache written without `print_extruder_override`. The Type 1
prime tower (Bambu printers) reads the travel and first-layer speeds of the initial tool for every
head, as in mainline; the Type 2 tower and the G-code writer read them in the slot of the tool head.
