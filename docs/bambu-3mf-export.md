# 3MF Application tag and "Export Bambu 3MF"

Two changes that belong together (target release 2.4.0.0):

1. A normal project save now names the application that wrote it: `EdgeSlicer-<version>`
   (`SLIC3R_APP_NAME` + `Snapmaker_VERSION`) in the 3MF `Application` metadata, instead of the
   `BambuStudio-<our version>` it used to borrow.
2. **File > Export > Export Bambu 3MF** writes a project Bambu Studio opens with its settings.

Code: `src/libslic3r/Format/BambuExport.{hpp,cpp}` (conversion), `BambuKnownKeys.cpp`
(generated), `BambuKeyAliases.{hpp,cpp}` (renamed keys and enum spellings, shared with the
import direction), `bbs_3mf.cpp` (`StoreParams::bambu_compat`), `Plater::export_bambu_3mf`, the CLI
option `--export-bambu-3mf` (with `--export-3mf`). Tests: `tests/libslic3r/test_bambu_3mf_export.cpp`
(tag `[BambuExport]`).

## What each slicer does with the Application tag

Read from the reader code (Bambu Studio: the reference clone `C:/Dev/BambuStudio`, 02.08.02.61,
commit 66e4054776; OrcaSlicer: SoftFever/OrcaSlicer main e71497738f; Snapmaker Orca:
Snapmaker/OrcaSlicer main da53bc5ffe, all 2026-09-22).

| reader | tag it recognises | unknown tag (e.g. `EdgeSlicer-2.4.0.0`) |
|---|---|---|
| **Bambu Studio** | `BambuStudio-` only (`bbs_3mf.cpp:4237`) | no generator version, so `dont_load_config = true` (`bbs_3mf.cpp:1972-1975`): project settings, embedded presets, custom G-code per layer and slice info are not read. The plater then finds an empty config, which fails its vendor/`check_project_config` test (`Plater.cpp:8449-8452`, "The 3mf file has invalid config, load geometry data only"), sets `load_config = false` and resets every object's and part's settings except the extruder (`Plater.cpp:8544-8566`). Geometry, colour paint, modifiers' shapes and extruder assignments survive |
| **OrcaSlicer** | `BambuStudio-`, `OrcaSlicer-`, or an `OrcaSlicer` metadata item (`bbs_3mf.cpp:4004-4017`) | project config and embedded presets **still load**: `dont_load_config = !m_load_config` (`bbs_3mf.cpp:1890`), the version gate is commented out; the file is classified `From_Other` (`Model.cpp:485-489`) and imported silently (`Plater.cpp:8850-8858`). But `m_is_bbl_3mf` is false, so the importer takes its third-party geometry path (`bbs_3mf.cpp:2037`, `3601`, `5324`): an object with several instances is split into one object per instance, a lone object is renamed after the file, and a single instance's transform is baked into the mesh |
| **Snapmaker Orca** | `BambuStudio-`, `Snapmaker_Orca-` (`bbs_3mf.cpp:3792-3799`) | project config always loads (not even gated on `m_load_config`, `bbs_3mf.cpp:1851`), embedded presets load, no warning (`Plater.cpp:11776` ff.: only `From_Prusa` is geometry-only). Same third-party geometry path as OrcaSlicer (`bbs_3mf.cpp:1921`, `3390`, `4970`) |
| **EdgeSlicer** (this fork) | `BambuStudio-`, `EdgeSlicer-` (`SLIC3R_APP_NAME`), `Snapmaker_Orca-` | - |

So the honest tag costs Bambu Studio users the settings (accepted: that is what the Bambu export
is for) and costs OrcaSlicer / Snapmaker Orca users nothing on the settings side, but it does move
them onto the geometry path those readers use for files from other applications (instances split
into separate objects, transforms baked, single object renamed). EdgeSlicer releases before this
change only knew `BambuStudio-` and (with a broken version parse, fixed here) `Snapmaker_Orca-`,
so they take the same third-party geometry path for new saves, with settings intact.

## Export Bambu 3MF

### Application tag and version

`BambuStudio-02.08.00.00`: the major.minor line of the Bambu Studio the key table was generated
from, patch 0 (`BambuExport::export_version()`). Chosen against Bambu Studio's version checks:

- The CLI refuses a file whose major.minor is newer than its own (`BambuStudio.cpp:1909`); the
  GUI shows its "newer version" dialog when `file > app` and the minor or `patch / 100` differ
  (`Plater.cpp:8488-8526`), and loads the settings anyway. A same-line file loads silently in any
  2.8.x; an older Bambu Studio warns (truthfully: the settings target 2.8) but still loads.
- Anything older than 2.7.0 would switch on Bambu's old-file migrations: prime-tower parameter
  rewrites below 2.0.0 (`Plater.cpp:8685`), `enable_wrapping_detection` forced off below 2.2.0 and
  `skirt_per_object` reset below 2.7.0 (`BambuStudio.cpp:1914-1948`), plate translation below 1.5.9.
- A major version newer than Bambu's own takes the "lower version of Bambu Studio" path.

The same version goes into `project_settings.config`, the embedded presets and
`slice_info.config`'s `X-BBL-Client-Version`. Regenerating the key table against a newer Bambu
Studio moves all of them.

### Which keys, and in which shape

`scripts/gen_bambu_known_keys.py --bambu <Bambu Studio checkout> --ours src/libslic3r/PrintConfig.cpp --out src/libslic3r/Format/BambuKnownKeys.cpp`
parses Bambu's `PrintConfig.cpp` (`init_common_params` / `init_fff_params`, the loop-generated
`machine_max_*` and `filament_*` override families) for every key, its type, nullability, the enum
strings its keys map accepts, and its per-extruder-variant class; it removes the keys Bambu's own
`handle_legacy()` discards on load (`prime_volume`, `only_one_wall_top`, ...), and derives our-enum
-> Bambu-enum translations by matching enumerator names (`rectilinear` = `ipRectilinear` =
Bambu's `zig-zag`, `organic` -> `tree_organic`). `--compare` prints the key/type/enum diff.
Regenerate when targeting a newer Bambu Studio, or when one of our enum options gains a value
(the `[BambuExport]` test "Every value of our enum options has a known Bambu fate" fails until you do).

For every key of every config written (project settings, embedded print/filament/printer presets,
per-object, per-part and per-height-range settings):

1. **Rename** where Bambu spells the same setting differently, from the one table in
   `Format/BambuKeyAliases.cpp` that our loader also reads backwards (see
   `bambu-config-compat.md`, "Renamed keys", for the full table and the import rules), so every
   rename round-trips: `infill_anchor(_max)` -> `sparse_infill_anchor(_max)`,
   `chamber_temperature` -> `chamber_temperatures`, `bottom_solid_infill_flow_ratio` ->
   `initial_layer_flow_ratio`, `ironing_angle` -> `ironing_direction` (our `-1` = "default" is
   left out), `only_one_wall_top` -> `top_one_wall_type` (`all top` / `not apply`),
   `support_ironing` -> `enable_support_ironing`, `extruder_clearance_radius` ->
   `extruder_clearance_max_radius`, `dont_slow_down_outer_wall` ->
   `no_slow_down_for_cooling_on_outwalls`, `role_based_wipe_speed` -> `role_base_wipe_speed`,
   `reduce_infill_retraction` -> `reduce_infill_retraction_mode` (`Enabled` / `Disabled`),
   `wipe_tower_rib_width` / `_extra_rib_length` / `_fillet_wall` / `_max_purge_speed` ->
   `prime_tower_*`, `wipe_tower_wall_type` -> `prime_tower_rib_wall` (`rib` = on; Bambu has no
   cone tower, a cone goes out as off), and since 2026-09-22 `lateral_lattice_angle_1/2` ->
   `sparse_infill_lattice_angle_1/2`, `notes` -> `process_notes`, `filament_colour_mode` ->
   `filament_colour_type` (0/1 swapped: Bambu's 0 is gradient) and `filament_multi_colors` ->
   `filament_multi_colour` (`|`-separated -> space-separated).
2. **Drop** keys Bambu does not know (about 240 of ours: Orca/EdgeSlicer features).
3. **Reshape** to Bambu's type (the doc `bambu-config-compat.md` tables, in reverse):
   our vector -> Bambu scalar keeps slot 0 (`*_jerk`, `ironing_speed`, `accel_to_decel_*`,
   `filament_notes`); our scalar -> Bambu vector (`travel_speed_z`, `top_solid_infill_flow_ratio`,
   `nozzle_type`, `nozzle_volume`, `flush_multiplier`); floats -> ints are rounded (`fan_*_speed`,
   `fan_cooling_layer_time`, `slow_down_layer_time`); "mm or %" -> mm resolves the percentage
   against the option's `ratio_over` (line widths over `nozzle_diameter`, overhang speeds over
   `outer_wall_speed`); mm -> % for Bambu's percent-only `seam_gap` (of the nozzle diameter) and
   `wipe_speed` (of `travel_speed`).
4. **Translate enum values** (generated table plus the manual rows in `BambuKeyAliases.cpp`:
   `ensure_vertical_shell_thickness` `none` -> `disabled`, `ensure_critical_only` /
   `ensure_moderate` -> `partial`, `ensure_all` -> `enabled`; `lateral-lattice` -> `2dlattice`,
   Bambu's name for the same pattern, in every pattern option).
   A value Bambu does not have (`seam_position = aligned_back`, `brim_type = painted`,
   `lateral-honeycomb`, the `tpms*` / `quartercubic` infills, `Textured Cool Plate`, ...) drops the key, so
   Bambu keeps its own default; a plate whose bed type Bambu lacks omits its plate bed type.
5. **Validate** the result against the Bambu type (numbers, bools, enum strings, `nil` only in a
   nullable vector); anything that does not fit is dropped rather than written.
6. **Lay out per-extruder-variant vectors** the way Bambu indexes them
   (`get_index_for_extruder`): `printer_extruder_variant/_id` get one entry per extruder named
   `<extruder_type> <nozzle_volume_type>` when ours does not cover every extruder (a single-variant
   list on a multi-extruder machine leaves Bambu reading the wrong slot); `print_extruder_*`
   follow the printer layout; filament options get one slot per (filament, printer variant) with
   `filament_self_index` = the filament (our own `[1,1,1,...]` is not trusted); machine limits
   stay (normal, silent) pairs per variant; `extruder_type` / `nozzle_volume_type` have one entry
   per nozzle (Bambu's `check_project_config` loads a multi-extruder project geometry-only
   otherwise). Values move by variant name and extruder id; per-flow slots (`process_flow_support`)
   map `high_flow` onto Bambu's `High Flow` variants.

7. **Bambu-specific constraints**, each found by slicing exports with Bambu Studio's CLI:
   - `extruder_nozzle_stats` is left out when empty (ours never fills it). Present-but-empty,
     Bambu's CLI takes it as "no nozzles" and every multi-extruder slice fails ("Failed slicing
     the model"); absent, it derives the stats from `extruder_max_nozzle_count` +
     `nozzle_volume_type` (`BambuStudio.cpp:4101-4156`).
   - `flush_multiplier` gets one value per extruder and `flush_volumes_matrix` one
     filament x filament matrix per extruder (extruder-major), as in Bambu's own H2D projects; ours
     is one matrix / multiplier used for every extruder, so it is repeated.
   - `ooze_prevention` is left out when the prime tower is enabled: Bambu's `Print::validate`
     refuses the combination ("Ooze prevention is currently not supported with the prime tower
     enabled."), this fork supports it for tool changers.
   - `top_color_penetration_layers` / `bottom_color_penetration_layers` (paint penetration): same
     keys and type as Bambu, but ours has an extra value, 0 = "follow the shell" (the default), and
     Bambu's minimum is 1 (default 4 / 3). A value >= 1 goes out as it is. A 0 goes out as the depth
     our shell settings give, the number `MultiMaterialSegmentation.cpp` uses for a flat painted
     top or bottom: `max(shell layers, ceil(shell thickness / layer_height))`, at least 1 (a
     0-layer shell claims nothing here; the closest Bambu can do is the surface layer). An object
     that overrides its shells or layer height, under a project that follows the shell, gets its
     own converted depth in `model_settings.config`, since Bambu would otherwise apply the
     project's number to it. Bambu's defaults (4 / 3) are not written: they would change how deep
     the colour goes whenever our shells differ from them. On import a Bambu value (always >= 1)
     is taken as it is, so a Bambu project keeps its penetration.

No value is invented: a key that cannot be represented is left out and Bambu uses its default.

`different_settings_to_system` is rewritten in Bambu's vocabulary. Geometry, plates, paint
(colour, supports, seams, fuzzy skin), modifiers, height ranges, cut data and everything else in
the archive are written exactly as for a normal save.

### Report

Every export logs one line per scope ("N setting(s) left out: ...") plus one line per rename or
non-trivial conversion, and a summary; the GUI shows "Exported for Bambu Studio: N settings not
supported by Bambu Studio were left out." (N = distinct keys dropped across the project, presets
and objects). The CLI prints the summary.

### Round trip

EdgeSlicer reads both kinds of file as full projects. Reading a Bambu export back restores every
renamed key (the loader reads the same `BambuKeyAliases` table backwards, with the inverse value
conversion), every translated enum value (`zig-zag` -> `rectilinear`, `2dlattice` ->
`lateral-lattice`, `tree_organic` -> `organic`, `ensure_vertical_shell_thickness`
`enabled/partial/disabled`), and `top_one_wall_type = not apply` as `only_one_wall_top = 0`. The
`[BambuAliases]` test "Our config survives Export Bambu 3MF and our own import, key by key" pins
this for every row of the table. What cannot come back: `wipe_tower_wall_type = cone` returns as
`rectangle`, `ironing_angle = -1` stays at our default (it was left out), `ensure_critical_only`
returns as `ensure_moderate`; settings Bambu does not have come back at their defaults: the export
is lossy by design.

## Sliced-plate files keep the old tag

A `.gcode.3mf` (strategy `SkipModel`: print jobs sent to a printer, "Export plate sliced file",
calibration jobs) is a Bambu printer / Bambu Studio preview artifact rather than a project, and
keeps writing `BambuStudio-<our version>` as before. Nobody has checked a Bambu printer or Bambu
Handy against another tag, and this change was not allowed to touch printers. One line in
`_add_model_file_to_archive` switches it if wanted.

## Verification (2026-09-22)

`[BambuExport]` (7 cases): honest tag on a normal save and full-config reload; the Bambu export's
tag, version, key set and shapes checked against the generated table, every conversion above,
embedded print/filament presets, per-object / per-part / height-range filtering, plate bed type,
the report, a dual-extruder layout, and EdgeSlicer reloading the Bambu export with geometry, two
instances, modifier, paint, plates, per-object settings and embedded presets intact.

Live, with the installed Bambu Studio 02.08.02.61 CLI in a scratch `--datadir`
(`--slice 0 --no-check`), on a painted three-filament cube whose project config came from this
tree's BBL system presets plus distinctive values (`wall_loops 5`, `sparse_infill_density 37%`,
`layer_height 0.16`, `sparse_infill_pattern rectilinear`, `line_width 110%`, `seam_gap 0.1`,
`only_one_wall_top`, `ensure_all`, `outer_wall_speed 123`, `fan_max_speed 95.6`):

| printer | Bambu export | normal save (`EdgeSlicer-` tag) |
|---|---|---|
| P1S | Success; G-code config block has all the values above, as `zig-zag`, `0.44`, `25%`, `all top`, `enabled`, `96` | Success with Bambu's defaults (walls 2, 20%, 0.20 mm): geometry only |
| H2D | Success, same values, per-extruder flush data | same as P1S |
| H2C | Success, same values | same as P1S |

Snapmaker U1 projects (4 tool heads, not a Bambu machine) exported through
`EdgeSlicer --export-bambu-3mf --export-3mf`: the 4-plate, 5-filament one loads with its settings
(Bambu reports its layer height, density and walls) and stops at Bambu's own layout check
("Assembly is too close to others"); the 3-plate, 7-filament one crashes Bambu's CLI during
slicing whatever project config it is given (its geometry with the other U1 project's config
crashes too; the other project's geometry with its config does not). Saved the old way, the same
file is rejected by Bambu with "Invalid parameter value(s) included in the 3mf file".

The script used for the live runs is the hidden test `[.bambu_live]` (see its comment).
