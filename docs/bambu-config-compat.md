# Bambu Studio config compatibility

Cross-compatibility with Bambu Studio presets is an explicit goal of this project. This document
records where the two config schemas disagree, what the loader does about it, and what is
deliberately left alone. Keep it updated as upstream drifts.

Reference clone used for the comparison: `C:/Dev/BambuStudio`.
Generated from `this->add("<key>", <coType>)` declarations in `src/libslic3r/PrintConfig.cpp` on
both sides, plus the loop-generated `machine_max_*` and `add_nullable` families.

## The problem

Bambu Studio stores many print settings as **per-extruder nullable vectors** and writes the
literal string `"nil"` into the slots of extruders the setting does not apply to:

```json
"top_surface_acceleration": ["100", "nil", "100", "nil", "nil"]
```

This fork types a few of the same options as **scalars**, and the rest as **plain,
non-nullable vectors** (since the High-Flow work, c3c82dbf64, that includes most of the speed and
acceleration family). Either way `ConfigOption*::deserialize()` throws

```
Deserializing nil into a non-nullable object
```

(`src/libslic3r/Config.hpp`, the `deserialize()` overloads around lines 857 and 1181), and
`PresetCollection::load_presets` (`src/libslic3r/Preset.cpp` ~1334-1344, with the same pattern in
the catch handlers ~1432-1452) answers a parse failure by **deleting the `.json` and `.info`**.
On 2026-09-21 that destroyed 208 of the owner's presets.

### The cardinal rule

**A `nil` slot means "not applicable to this extruder". It does not mean zero.**
Substituting `0` for a speed or an acceleration is what emits `G1 F0` and stalls a printer - the
same class of bug as the internal_bridge_speed outage this week. No translation path may invent a
zero. (A zero Bambu genuinely wrote is preserved as-is; several options use `0` as a documented
"unset / use default" sentinel.)

## Measured impact on the owner's data

Scan of 275 presets copied from `%APPDATA%/BambuStudio/user/<id>/` (2026-09-21, before the
High-Flow retype; the totals still hold, but most of what was then a scalar collapse is now a
vector backfill - see table A):

| measure | count |
|---|---|
| presets carrying at least one `nil` array | 211 |
| presets that needed translation to load | 207 |
| presets where a **lossy** choice had to be made | 19 |
| total `nil`-bearing arrays | 1400 |
| &nbsp;&nbsp;non-nil slots all identical (clean collapse) | 1346 |
| &nbsp;&nbsp;non-nil slots genuinely differ (lossy) | 50 |
| &nbsp;&nbsp;every slot `nil` | 4 |
| distinct options implicated | 50 |
| of those, options that actually reach `deserialize()` and throw | 35 |
| options dropped earlier as unknown keys (no fix needed) | 15 |

The 15 dropped options (`vertical_shell_speed`, `overhang_totally_speed`,
`filament_retraction_length`, `filament_z_hop`, `filament_wipe`, ...) are cleared by
`PrintConfigDef::handle_legacy`'s ignore set or its `!print_config_def.has(opt_key)` fallback
before any deserialize happens. They are not part of the problem and need no translation.

## Inventory

### A. Fork SCALAR / Bambu VECTOR (6)

These are the per-extruder vectorization gap. Bambu can write a `nil` in any of them.
Translation: strip `nil`, then collapse to a single value.

| option | fork type | Bambu type |
|---|---|---|
| `filament_extruder_id` | `coInt` | `coInts` |
| `flush_multiplier` | `coFloat` | `coFloats` |
| `nozzle_type` | `coEnum` | `coEnums` |
| `nozzle_volume` | `coFloat` | `coFloats` |
| `top_solid_infill_flow_ratio` | `coFloat` | `coFloats` |
| `travel_speed_z` | `coFloat` | `coFloats` |

Until 2026-09-21 this table had 32 rows. The High-Flow work (c3c82dbf64) retyped the other 26 to
per-flow-variant vectors, and 22 of them now have **exactly** Bambu's type, so they left the
mismatch list altogether: `bridge_speed`, `default_acceleration`, `enable_overhang_speed`,
`gap_infill_speed`, `initial_layer_acceleration`, `initial_layer_infill_speed`,
`initial_layer_speed`, `inner_wall_acceleration`, `inner_wall_speed`,
`internal_solid_infill_speed`, `outer_wall_acceleration`, `outer_wall_speed`,
`small_perimeter_speed`, `small_perimeter_threshold`, `sparse_infill_acceleration`,
`sparse_infill_speed`, `support_interface_speed`, `support_speed`, `top_surface_acceleration`,
`top_surface_speed`, `travel_acceleration`, `travel_speed`. The four `overhang_N_4_speed` rows
moved to table C (arity now matches, base type does not). A `nil` in any of the 26 takes the
backfill path - see table D2.

### A2. Fork VECTOR / Bambu SCALAR (12)

| option | fork type | Bambu type | note |
|---|---|---|---|
| `filament_notes` | `coStrings` | `coString` | harmless: a scalar string deserializes into a 1-element vector |
| `scale` | `coStrings` | `coFloat` | artefact of a **duplicate declaration** in this fork, see below |
| `ironing_speed` | `coFloats` | `coFloat` | High-Flow retype; harmless, see below |
| `default_jerk`, `outer_wall_jerk`, `inner_wall_jerk`, `infill_jerk`, `top_surface_jerk`, `initial_layer_jerk`, `travel_jerk` | `coFloats` | `coFloat` | High-Flow retype; harmless |
| `accel_to_decel_enable` | `coBools` | `coBool` | High-Flow retype; harmless |
| `accel_to_decel_factor` | `coPercents` | `coPercent` | High-Flow retype; harmless |

The High-Flow rows are harmless for the same reason as `filament_notes`: Bambu writes a scalar,
which deserializes into a 1-element vector, and `Preset::normalize` grows it to the length of
`process_flow_support`. Bambu never writes `nil` into a scalar, so none of them reaches the
translation.

### B. Nullable-axis mismatches on shared keys: **none**

No key is a plain vector on one side and a `*Nullable` vector on the other. All 15 shared
`add_nullable` `filament_*` options have identical types on both sides. This matters: it means the
nullable axis needs no translation for keys both sides know about, and the only nullable-related
work is the `nil`-in-a-non-nullable-type problem handled here.

Asymmetries land in the missing-key counts instead:
- Bambu-only nullable options the fork does not declare (7): `filament_bridge_speed`,
  `filament_enable_overhang_speed`, `filament_overhang_{1,2,3,4}_4_speed`,
  `filament_overhang_totally_speed`. These are dropped as unknown keys - no crash, but the
  per-filament overhang overrides a Bambu user set are silently lost. Worth adding if
  cross-compatibility is to be complete.
- Fork-only nullable options (3): `filament_retract_length_toolchange`,
  `filament_retract_restart_extra_toolchange`, `filament_retract_lift_enforce`.

### C. Same arity, different base type (16)

None of these can carry a `nil`, so none of them breaks loading today. They are recorded because
they are silent precision/semantics differences.

| option | fork type | Bambu type |
|---|---|---|
| `fan_cooling_layer_time` | `coFloats` | `coInts` |
| `fan_max_speed` | `coFloats` | `coInts` |
| `fan_min_speed` | `coFloats` | `coInts` |
| `slow_down_layer_time` | `coFloats` | `coInts` |
| `initial_layer_line_width` | `coFloatOrPercent` | `coFloat` |
| `inner_wall_line_width` | `coFloatOrPercent` | `coFloat` |
| `internal_solid_infill_line_width` | `coFloatOrPercent` | `coFloat` |
| `line_width` | `coFloatOrPercent` | `coFloat` |
| `outer_wall_line_width` | `coFloatOrPercent` | `coFloat` |
| `skeleton_infill_line_width` | `coFloatOrPercent` | `coFloat` |
| `skin_infill_line_width` | `coFloatOrPercent` | `coFloat` |
| `sparse_infill_line_width` | `coFloatOrPercent` | `coFloat` |
| `support_line_width` | `coFloatOrPercent` | `coFloat` |
| `top_surface_line_width` | `coFloatOrPercent` | `coFloat` |
| `seam_gap` | `coFloatOrPercent` | `coPercent` |
| `wipe_speed` | `coFloatOrPercent` | `coPercent` |
| `overhang_1_4_speed` | `coFloatsOrPercents` | `coFloats` |
| `overhang_2_4_speed` | `coFloatsOrPercents` | `coFloats` |
| `overhang_3_4_speed` | `coFloatsOrPercents` | `coFloats` |
| `overhang_4_4_speed` | `coFloatsOrPercents` | `coFloats` |

`coFloats` reading a Bambu `coInts` value is safe (widening). The `coFloatOrPercent` rows are a
fork superset: the fork accepts everything Bambu writes plus a percent form.

The four `overhang_N_4_speed` rows used to be a double mismatch (scalar-vs-vector as well). Since
the High-Flow retype only the base type differs: a Bambu value of `"50"` means 50 mm/s on both
sides, and a fork-side percent has no Bambu equivalent (a fork-only extension). Unlike the rest of
this table they **can** carry a `nil` - they are vectors - and take the backfill path (table D2).

### D. Non-nullable vectors that can still receive a `nil`

Not a *type* mismatch - both sides call these vectors - but the fork's is not nullable, so a Bambu
`nil` still throws. Observed in the owner's data:

| option | fork type |
|---|---|
| `filament_flow_ratio` | `coFloats` |
| `filament_max_volumetric_speed` | `coFloats` |
| `nozzle_temperature` | `coInts` |
| `nozzle_temperature_initial_layer` | `coInts` |
| `long_retractions_when_ec` | `coBools` |
| `retraction_distances_when_ec` | `coFloats` |
| `machine_max_jerk_x` / `_y` / `_e` | `coFloats` |

These must **not** be collapsed to a scalar - that would destroy per-extruder indexing. They are
backfilled instead, preserving the slot count.

### D2. Per-flow-variant vectors (the High-Flow retype)

The 26 options that left table A are now non-nullable vectors listed in
`process_flow_variant_options()` (`PrintConfig.cpp`), so a Bambu `nil` in them is **backfilled**,
not collapsed: `["200","nil","200","nil","160"]` loads as `200,200,200,200,160`, where it used to
load as the scalar `200`.

This was checked against how the slicer reads them, because the two sides index these vectors
differently:

- **Bambu** writes one slot per *extruder variant* - `print_extruder_variant`, 4 entries in
  `fdm_process_dual_common.json`, 5-7 in the H2D/H2C process presets
  (`Direct Drive Standard`, `Direct Drive High Flow`, ... per extruder).
- **This fork** reads one slot per *flow variant*: `get_config_idx(config, ConfigFlowDomain::Process, ...)`
  looks the filament's flow type up in `process_flow_support` (default `["standard"]`; an unknown
  type resolves to 0). A Bambu preset carries no `process_flow_support`, so every read resolves to
  **slot 0**, for standard and high-flow filaments alike.
- `print_extruder_variant` / `print_extruder_id` are declared here but nothing indexes by them.

Consequences:

1. **The value the slicer uses is Bambu's slot 0** - extruder 1, Direct Drive Standard - which is
   what Bambu Studio itself prints with on that nozzle. Where slot 0 differs from the majority this
   is a *change* from the old collapse, and a more faithful one: `support_interface_speed`
   `30,50,nil,50,nil` (in the 43-preset corpus) now slices at 30 (was 50), and
   `outer_wall_acceleration` `3000,2000,nil,2000,nil` (fixture `0.30mm @H2D - Custom ASA`) at 3000
   (was 2000).
2. **Keeping the full slot count is right.** `Preset::normalize` only ever *grows* a flow-variant
   vector to the length of `process_flow_support`; it never truncates, and the extra slots are
   inert. The fork's own shipped BBL H2D/H2C system profiles already load with the same 5-7 slot
   layout, so a translated Bambu user preset has the same shape as its system parent.
   Collapsing to one slot would make it differ from that parent in every such key.
3. **Slots 0 and 1 line up.** If a high-flow column is ever added to such a preset
   (`process_flow_support = ["standard","high_flow"]`), a high-flow filament reads slot 1, which is
   Bambu's `Direct Drive High Flow` on extruder 1. Slots 2+ (extruder 2, TPU) are never read.
4. A nil in slot 0 is filled from the first real value after it, so the value the slicer reads is
   never invented: it is always a value Bambu wrote for some variant.

`tests/slic3rutils/bambu_config_compat_tests.cpp` pins all of this end-to-end ("a backfilled Bambu
vector is read at the slot the slicer uses").

### Incidental findings

The fork declares some keys **twice**; the later declaration wins at runtime and the earlier one
is dead code. Bambu declares each once.

- `scale` - `coFloat` at `PrintConfig.cpp:10115`, then `coStrings` at `:10601`. This redeclaration
  is the only reason `scale` appears in table A2 above.
- `outer_wall_acceleration` - `coFloats` at `:3037` and again at `:3069` (same type; the second
  one's default of 500 wins over the first one's 10000). Predates the High-Flow retype, which
  converted both.
- `downward_check` (formerly `coStrings`, then `coBool`) is now declared once.

Neither is caused by this change; both are worth cleaning up separately.

## The translation

Lives in `src/libslic3r/BambuConfigCompat.{hpp,cpp}`, called from two places in
`src/libslic3r/Config.cpp`:

- `ConfigBase::load_from_json_document`, for JSON arrays (presets, `project_settings.config`);
- `ConfigBase::set_deserialize_raw`, for every value that arrives as a comma-joined string
  (3MF per-object / per-part / layer-range settings, ini files, G-code config blocks, the CLI).
  JSON arrays are translated before they get here, so the two never both fire. See
  [Overrides](#overrides-per-object-per-part-and-layer-range-settings-in-a-3mf).

### Why there

`PrintConfigDef::handle_legacy(opt_key, value)` (`PrintConfig.cpp` ~8857) looks like the natural
hook - it takes both key and value by reference and already rewrites both. It is not usable for
this, because on the **array** path (`Config.cpp` ~598 scalar / ~1105 array) it is
called *before* `value_str` is assembled from the array elements (assembly happens ~30 lines
later). At that point `value` is still empty, so a value-rewriting translation there has nothing
to rewrite.

Moving or duplicating the `handle_legacy` call to after assembly was considered and rejected: by
then the elements have been joined into a single comma-separated string, so the translation would
have to re-split it, and `handle_legacy` has no access to the `ConfigOptionDef` it needs to know
whether the target is a scalar, a vector, or nullable.

The chosen site is the existing collapse block at `Config.cpp` ~1163, where `array_values` is
still a `std::vector<std::string>` and `optdef` is in hand. That block already collapsed
single-element and all-equal arrays for scalar targets; the `nil` translation runs just before it
and sets a flag so the two do not both fire.

### Rules

| input | fork option | outcome |
|---|---|---|
| no `nil` | any | untouched |
| any | nullable | **untouched** - the type handles `nil` natively; rewriting would destroy it |
| all slots `nil` | any | key dropped, option keeps its compiled-in default. No value is invented |
| non-nil slots all equal | scalar | collapse to that value (`Collapsed`) |
| non-nil slots differ | scalar | **lossy**: majority of non-nil slots wins, first non-nil slot breaks a tie (`CollapsedLossy`) |
| non-nil slots all equal | vector, non-nullable | backfill each `nil` from the nearest real neighbour, slot count preserved (`Backfilled`) |
| non-nil slots differ | vector, non-nullable | same backfill, flagged **lossy** (`BackfilledLossy`) |

The scalar tie-break takes the **first** non-nil slot because Bambu writes the primary extruder
first, so slot 0 is the value a single-extruder machine actually prints with. The majority rule
ahead of it handles the common H2D shape `["200","nil","200","nil","160"]`, where two slots agree
and a third (a different nozzle) does not - taking 200 matches what the machine's main nozzle does.

Backfill takes the nearest *preceding* non-nil value, or the first following one when the array
starts with `nil`, so an extruder that had no value of its own inherits its neighbour's rather
than a fabricated constant.

## Overrides: per-object, per-part and layer-range settings in a 3MF

A Bambu Studio project stores object, part (modifier) and layer-range settings in
`Metadata/model_settings.config` and `Metadata/layer_config_ranges.xml` as comma-joined strings,
one slot per `print_extruder_variant`, with `nil` where the override does not apply:

```xml
<metadata key="outer_wall_speed" value="80,nil,80,nil"/>
```

Those strings went straight into `ConfigOptionFloats::deserialize` (`bbs_3mf.cpp`, the
`set_deserialize` calls in the object loop and in `_generate_volumes_new`, and in
`_extract_layer_config_ranges_from_archive`), bypassing the JSON translation above, so the whole
project failed to load with *Deserializing nil into a non-nullable object*. Bambu's own
`resources/calib/pressure_advance/auto_pa_line_dual.3mf` (H2D, 02.00.02.01) was the repro.

### How Bambu Studio reads a nil in an override

Not like a preset. `PrintObject::object_config_from_model_object` and
`apply_to_print_region_config` (Bambu `PrintObject.cpp` ~3052 / ~3069) apply an object or part
config with `ConfigOptionVector::set_to_index` (Bambu `Config.hpp` ~602): for each physical
extruder it takes the override's slot for that extruder's variant **unless that slot is nil**, in
which case the value already there - the print settings, or the enclosing object's override for a
part - is kept. A nil slot in an override therefore means **inherit that slot from the parent**,
per slot. An all-nil override is no override at all.

### What this fork does

This fork applies an object / part / layer-range config as a **whole vector**
(`ConfigOption::set` in `PrintObject::object_config_from_model_object` and
`apply_to_print_region_config`); a `ModelConfig` cannot hold a per-slot "inherit" marker in a
non-nullable option. So the reader resolves the nil slots **at load time from the same parents
Bambu would use**:

- `BambuConfigCompat::OverrideScope` marks a stretch of loading as "these are overrides" and names
  the parent. `bbs_3mf.cpp` opens one with the project config around the object loop and around
  the (now deferred) layer-range read, and a nested one with the object's own config inside
  `_generate_volumes_new`, so a part looks at its object first and then at the project.
- `ConfigBase::set_deserialize_raw` sees the scope (`ConfigSubstitutionContext::bambu_override_parents`)
  and calls `translate_nil_override` instead of the preset rule.
- `layer_config_ranges.xml` is now read after the archive loop: Bambu writes it *before*
  `project_settings.config`, so reading it in archive order would find no parent.

| override (object has 4 variant slots) | parent (project) | result | reported |
|---|---|---|---|
| `80,nil,80,nil` | `200,250,200,250` | `80,250,80,250` (`Inherited`) | no - exactly what Bambu slices |
| `nil,90,nil,90` | `350,600,350,600` | `350,90,350,90` (`Inherited`) | no |
| `nil,nil,nil,nil` | any | key dropped, object inherits everything (`AllNil`) | no - that is Bambu's meaning |
| `80,nil,80,nil` | none, or a different slot count | `80,80,80,80` (`BackfilledLossy`) | **yes** |
| scalar target, `12,nil,12,nil` | scalar `12` | `12` (`Collapsed`) | no |
| scalar target, `12,nil,12,nil` | scalar `20`, or none | `12` (`CollapsedLossy`) | **yes** - the uncovered variants print at 12, not 20 |

Nil in slot 0 matters most: the slicer reads slot 0 for a standard-flow filament (table D2), and
Bambu's override did not cover that variant, so slot 0 must be the parent's value. The old preset
backfill would have spread the override's `90` into it.

### Lossy cases and caveats

- **Snapshot, not a live link.** The inherited slots are copied into the object's override. If
  the process preset is changed later, those slots keep the value the project had at load. In
  practice only slot 0 (and slot 1 with a `high_flow` column in `process_flow_support`) is ever
  read, and slot 0 is usually one the override itself set. Not reported: at load time the value
  is exactly Bambu's.
- **No parent known** (the project config lacks the key, or its slot count differs from the
  override's): the nil slots are backfilled from the override's own values and the key is
  reported through `ConfigSubstitutionContext`, so the Plater's substitution dialog lists it with
  the original `80,nil,80,nil` next to what landed.
- **Layer ranges** inherit from the project config only. Bambu layers them over the part and
  object too, but the ranges are read before objects exist; a layer range and its object both
  overriding the same key with nil slots is the one shape that can differ, silently.
- **Modifier parts** inherit from their object and then the project - not from the normal part's
  own override, which Bambu's region stacking would consult first. Rare in practice.
- **Scalars** in this fork (table A) can only hold one value; the collapse rule applies and it is
  reported unless every uncovered variant would have inherited that same value.

### Round trip

An object loaded this way is written back by this fork with full vectors and no `nil`
(`80,250,80,250`), which reloads here unchanged with nothing translated. Bambu Studio will read
the same file as an override that now covers every variant - equal values at the time of export,
but no longer tracking Bambu's process preset for the uncovered variants.

### The shared layer also covers every other string path

Outside an `OverrideScope`, `set_deserialize_raw` applies the **preset** rule (table above) to a
comma-joined value, so an ini file, a G-code config block or a CLI `--key 200,nil,200` carrying a
Bambu `nil` now loads instead of throwing. Nullable and string options are untouched on every
path.

## Renamed keys (Bambu Studio's name -> ours)

Some settings exist on both sides under different names, or with an enum value spelled
differently. Until 2026-09-22 only six of them were read back (`sparse_infill_anchor(_max)`,
`chamber_temperatures`, `initial_layer_flow_ratio`, `ironing_direction`, `top_one_wall_type`);
the rest were dropped as unknown keys, so a genuine Bambu Studio project lost its prime tower
settings, its clearance radius, its process notes and so on, even though "Export Bambu 3MF" was
already writing all of them in the other direction.

### One table, both directions

`src/libslic3r/Format/BambuKeyAliases.{hpp,cpp}` holds every pair once, with the value conversion
in each direction:

- export: `BambuExport` renames our key and applies `to_bambu` (docs/bambu-3mf-export.md);
- import: `PrintConfigDef::handle_legacy` calls `BambuKeyAliases::import_key` first, which renames
  Bambu's key and applies `from_bambu`. Every loader goes through `handle_legacy`, so this covers
  3MF project settings, embedded presets, per-object / per-part / height-range settings
  (`model_settings.config`, `layer_config_ranges.xml`), loose preset `.json` files (Import
  Configs, the Bambu Studio preset mirror, system profiles), ini files and G-code config blocks.

| Bambu Studio | ours | value on import | since |
|---|---|---|---|
| `sparse_infill_anchor` / `_max` | `infill_anchor` / `_max` | same | before |
| `chamber_temperatures` | `chamber_temperature` | same | before |
| `initial_layer_flow_ratio` | `bottom_solid_infill_flow_ratio` | same | before |
| `ironing_direction` | `ironing_angle` | same (export leaves our `-1` = "default" out) | before |
| `top_one_wall_type` | `only_one_wall_top` | `not apply` -> 0, `all top` / `topmost` -> 1 (topmost is approximate) | before |
| `enable_support_ironing` | `support_ironing` | same | **new** |
| `no_slow_down_for_cooling_on_outwalls` | `dont_slow_down_outer_wall` | same | **new** |
| `role_base_wipe_speed` | `role_based_wipe_speed` | same | **new** |
| `reduce_infill_retraction_mode` | `reduce_infill_retraction` | `Enabled` -> 1, `Disabled` -> 0, `Auto` -> not loaded | **new** |
| `prime_tower_rib_width` | `wipe_tower_rib_width` | same | **new** |
| `prime_tower_extra_rib_length` | `wipe_tower_extra_rib_length` | same | **new** |
| `prime_tower_fillet_wall` | `wipe_tower_fillet_wall` | same | **new** |
| `prime_tower_rib_wall` | `wipe_tower_wall_type` | 1 -> `rib`, 0 -> `rectangle` | **new** |
| `prime_tower_max_speed` | `wipe_tower_max_purge_speed` | same | **new** |
| `prime_tower_skip_points` | `wipe_tower_wall_gap` | same (not read from the bundled vendor presets, see below) | **new**, both directions |
| `sparse_infill_lattice_angle_1` / `_2` | `lateral_lattice_angle_1` / `_2` | same | **new**, both directions |
| `process_notes` | `notes` | same | **new**, both directions |
| `filament_colour_type` | `filament_colour_mode` | 0 (gradient) <-> 1, 1 (default) <-> 0 | **new**, both directions |
| `filament_multi_colour` | `filament_multi_colors` | colours space-separated <-> `\|`-separated | **new**, both directions |

Enum values (same key, different spelling), read back from the manual rows in
`BambuKeyAliases::enum_aliases()` and from the generated `BambuKnownKeys.cpp` table:
`2dlattice` -> `lateral-lattice` (every pattern option; it is the same pattern, `Fill2DLattice` /
`FillLateralLattice`, with the same angle options), `tree_organic` -> `organic` (`support_style`;
used to fall back to the default with a substitution warning), `zig-zag` -> `rectilinear`, and
`ensure_vertical_shell_thickness` `enabled` / `partial` / `disabled` -> `ensure_all` /
`ensure_moderate` / `none`. The `[BambuAliases]` test "Every Bambu enum value of a shared key
loads" walks every enum value Bambu can write for a key we share; the only ones left without an
equivalent are `fuzzy_skin = disabled_fuzzy` (Bambu: off and ignore paint) and
`nozzle_type = tungsten_carbide`, which fall back through the normal substitution report.

### Value conversions and what cannot be represented

- A conversion works element by element on the value as our option serializes it (whole value
  for a scalar, `,`-split for a vector, the quoted `;` form for `coStrings`), so it is the same for
  a JSON array, a JSON string and a `model_settings.config` attribute. `nil` slots are passed
  through untouched to the nil translation above, which runs after the rename, against our
  option's type.
- A key-only pass (the first `handle_legacy` call on a JSON array, `different_settings_to_system`)
  only renames. For a JSON array whose key is an alias, `load_from_json_document` deserializes the
  assembled value under the Bambu name, so `handle_legacy` sees the whole value and converts it.
- A Bambu value with no equivalent here (`reduce_infill_retraction_mode = Auto`, decided per
  filament) is not loaded: the key is dropped like any unknown key and ours keeps its own or its
  inherited value. Nothing is invented.
- Lossy on export, by design: `wipe_tower_wall_type = cone` goes out as a plain wall and comes back
  as `rectangle`; `ironing_angle = -1` is left out; `ensure_critical_only` comes back as
  `ensure_moderate`. The `[BambuAliases]` test "What Export Bambu 3MF cannot say ..." pins these.

### Both spellings in one file: ours wins

A file may carry both names (our own BBL/Qidi/Elegoo machine profiles carry
`extruder_clearance_radius` *and* `extruder_clearance_max_radius`, copied from Bambu's). Our key
wins, whatever order the two are read in:

- JSON documents: `load_from_json_document` skips a Bambu alias when the same document also has
  our key (logged at `info`). Before this, the loader's sorted key order decided, and for most
  pairs (`top_one_wall_type` after `only_one_wall_top`, `process_notes` after `notes`, ...) the
  Bambu value was applied last and won.
- Per-object / per-part settings: `ModelConfig::set_deserialize` skips a Bambu alias when the
  config already holds our key; our key read later overwrites anyway.
- Not covered: a height range (`layer_config_ranges.xml`) that lists both names keeps the one it
  lists last. Neither Bambu Studio nor this fork writes both.

Consequence for `extruder_clearance_radius`: Bambu's own machine profiles still carry a stale
`extruder_clearance_radius` (49 on the H2D) that Bambu Studio itself ignores in favour of
`extruder_clearance_max_radius` (96). A Bambu system profile loaded as a file keeps 49 - the value
our shipped copy of the same profile has always loaded as. Bambu projects and user presets only
ever carry `extruder_clearance_max_radius` (Bambu does not write keys it does not declare), so
they get Bambu's value.

Since the H2D timelapse fix (PR #240) `extruder_clearance_max_radius` is no longer a rename: it is
a printer setting of ours too (Printer settings > Extruder Clearance > Max Radius; Bambu's default
68), read by the timelapse position picker exactly as Bambu Studio's picker reads it. It still
feeds `extruder_clearance_radius` wherever the rename used to: a file (JSON document, INI, G-code
config block, string map) that sets Bambu's key but not ours loads the value into both
(`BambuKeyAliases::load_fallbacks`). A file that sets both keeps each. So the by-object clearance
(`extruder_clearance_radius`) loads exactly as before. "Export Bambu 3MF" now writes our
`extruder_clearance_max_radius` (the machine profile's Bambu value, 96 on the H2D) instead of
`extruder_clearance_radius` under Bambu's name.

### Effect on this fork's shipped profiles

`resources/profiles` (11,600 files) carries some Bambu names that used to be dropped. Where the
same file does not also set our key, they now apply:

| Bambu key | files | effect |
|---|---|---|
| `extruder_clearance_max_radius` alone | 11 | **changes** the sequential-print clearance radius: BBL A1 / A1 mini / A2L 57 -> 73, X2D 13 -> 60, Elegoo CC2 common 65 -> 68, Qidi Q2 / Q2C / X-Plus 5 65 -> 75, X-Max 4 65 -> 80, Geeetech common 40 (default) -> 47 / 65. The new values are the vendors' own (what Bambu / Qidi Studio use) |
| `prime_tower_rib_wall` = 1 | 15 | **changes** the wipe tower wall of the Anycubic Kobra S1 Max process presets from `rectangle` to `rib` |
| `no_slow_down_for_cooling_on_outwalls` = [0] | 229 | none (our default, nothing inherited) |
| `prime_tower_max_speed` 90, `_rib_width` 8, `_extra_rib_length` 0, `_fillet_wall` 1 | 15-16 | none (our defaults) |
| `enable_support_ironing` 0, `sparse_infill_lattice_angle_1/2` -45 / 45 | 2 (Qidi) | none (our defaults) |
| `extruder_clearance_max_radius` next to `extruder_clearance_radius` | 29 | none (ours wins) |
| `prime_tower_skip_points` = 0 | 9 (Flashforge Creator 5) | none: not read from vendor presets (`SystemPresetTowerKeysScope`); would turn the WipeTower2 wall gaps off |
| `enable_tower_interface_features` = 1 | 59 (50 BBL H2D / H2C / X2D, 9 Flashforge Creator 5) | none: kept as Bambu's value, not mapped for vendor presets; see "Tower interface features" |

### Tower interface features (`enable_tower_interface_features`)

Bambu Studio's one developer switch for a bundle of prime tower behaviours on its interface
layers. Three of them are separate options here, for every printer and both tower generators
(`src/libslic3r/GCode/WipeTowerInterface.hpp`): `wipe_tower_interface_temp`,
`wipe_tower_interface_run_in` (needs `wipe_tower_wall_gap`), `wipe_tower_interface_extra_prime`,
plus `wipe_tower_interface_trigger`. The per-filament values keep Bambu's names
(`filament_tower_interface_print_temp`, `_pre_extrusion_dist`, `_pre_extrusion_length`), so they
load and export as they are. Bambu's firmware purge (`M620.13`, H2 templates) is not ported.

- Import (`PrintConfigDef::handle_legacy_composite`): `true` in a file that has none of our three
  options (nor the trigger) turns on all three and the wall gaps, triggered at material changes,
  then sets the Bambu key to false so a re-save cannot re-arm it. `false` changes nothing.
- The bundled vendor presets are loaded under `SystemPresetTowerKeysScope`: the key is kept but not
  mapped, and `prime_tower_skip_points` is not read, so their shipped output is unchanged. A Bambu
  project, an imported Bambu preset or a loose preset file is mapped.
- Export ("Export Bambu 3MF"): written from our three options, `true` when any of them is on (Bambu
  can only switch on the whole bundle; a report note says when only some were on), never from the
  stored Bambu value. `wipe_tower_wall_gap` goes out as `prime_tower_skip_points`.

### Deliberately not mapped

Audited against Bambu Studio's `PrintConfig.cpp` (02.08.02.61): every Bambu-only key was checked
for an equivalent here. These look related but are not the same setting:

- `skirt_per_object` (sequential printing only, `comDevelop`, default on) vs our `skirt_type`:
  mapping it would switch every Bambu project to per-object skirts.
- `print_in_clockwise` (`comDevelop`, always off in practice) vs our `wall_direction`.
- `prime_tower_infill_gap` (150 %) vs our `wipe_tower_extra_spacing` (100 %): ours only drives the
  Type 2 tower; our Type 1 tower computes its spacing.
- `machine_hotend_change_time` (H2C hotend rack) vs our `machine_tool_change_time`;
  `machine_prepare_compensation_time` (default 260 s, `comDevelop`) vs our `machine_prepare_time`.
- `max_volumetric_extrusion_rate_slope_positive/_negative` (compiled out in Bambu) vs our single
  `max_volumetric_extrusion_rate_slope`.
- `thumbnail_size` -> `thumbnails` is an older Orca rename in `handle_legacy`, not part of this table.

### Measured on a corpus of genuine Bambu Studio files

`libslic3r_tests "[.bambu_corpus]"` with `BAMBU_CORPUS` set loads every file and counts, per file,
the distinct keys the loader drops as unknown (`ConfigSubstitutionContext::unrecogized_keys`); a
3MF counts its project settings and embedded presets plus a full project load. Corpus: Bambu
Studio's `resources/calib` 3MFs (5, three with project settings, BambuStudio-01.07 / 02.00),
the owner's Bambu user presets in `tests/data/bambu_compat` and `bambu_compat_43` (52), and Bambu
Studio's `resources/profiles/BBL` process / filament / machine presets (2,494). "Before" is
origin/main 5264e22e6a with only the counting test added; "after" is this branch merged with
main 3dca92a1bf (PR #95, which also makes `auto_pa_line_dual.3mf` load fully).

| corpus | files | unknown keys dropped, before | after |
|---|---|---|---|
| Bambu 3MF projects | 5 | 138 | 121 |
| Bambu preset .json | 2,546 | 34,284 | 34,191 |

Every aliased key left the dropped list, per file that carried it:

| Bambu key | 3MF before -> after | .json before -> after |
|---|---|---|
| `extruder_clearance_max_radius` | 3 -> 0 | 12 -> 0 |
| `prime_tower_rib_wall` | 2 -> 0 | 23 -> 0 |
| `prime_tower_fillet_wall` | 2 -> 0 | 14 -> 0 |
| `prime_tower_max_speed` | 2 -> 0 | 4 -> 0 |
| `prime_tower_rib_width`, `prime_tower_extra_rib_length` | 2 -> 0 each | - |
| `role_base_wipe_speed` | 2 -> 0 | - |
| `process_notes` | 2 -> 0 | - |
| `enable_support_ironing` | - | 20 -> 0 |
| `reduce_infill_retraction_mode` | - | 18 -> 1 (the one `Auto`: Bambu's `fdm_process_common.json`) |
| `no_slow_down_for_cooling_on_outwalls` | - | 1 -> 0 |
| `sparse_infill_lattice_angle_1` / `_2` | - | 1 -> 0 each |

What is still dropped is Bambu-only (the per-filament overhang / ramming / retraction overrides,
`pre_start_fan_time`, `first_x_layer_fan_speed`, hole and counter compensation, scarf-seam
filament overrides, ...), plus a few keys Bambu Studio 02.00 wrote that current Bambu Studio no
longer declares either (`chamber_temp_control`, `layer_time_smoothing`, `end_print_exhaust_fan_*`).
The Bambu system presets carry few aliased keys because they inherit them from a handful of
`*_common.json` files; each file is counted on its own, without resolving `inherits`.

## What the user sees

Never silent. Three layers:

1. **Log, always.** Every translated option logs at `info` (clean) or `warning` (lossy), naming
   the key, the file, the original array and the result. A per-file summary line records how many
   options were translated and how many were lossy.
2. **Substitution report, when lossy.** Lossy and all-nil cases are pushed into the existing
   `ConfigSubstitutionContext`, which `PresetCollection::load_presets` already collects and
   `show_substitutions_info` (`src/slic3r/GUI/GUI.cpp:381`) already renders as a dialog listing
   each preset and each changed value. No new mechanism was invented, and the existing
   `skip_settings_mapping_warnings` opt-out continues to apply.
3. **Clean collapses are deliberately not surfaced in the dialog.** Collapsing
   `["200","nil","200"]` to `200` loses nothing; reporting 1346 such cases would bury the 50 that
   matter. They remain in the log.

## Round-trip safety on save

The mirror from Bambu Studio is **one-way**: this fork only ever reads from Bambu's folder and
writes into its own (`%APPDATA%/EdgeSlicer`). Nothing in this change writes to
`%APPDATA%/BambuStudio`.

When the fork later saves a translated preset **into its own folder**, it serializes from its own
option types, so a collapsed scalar is written as a scalar (`"top_surface_acceleration": "100"`)
and a backfilled vector as a full vector with no `nil`. The original per-extruder structure of the
lossy cases is therefore not recoverable from the fork's copy - which is why those cases are
reported rather than applied silently.

The user's Bambu Studio copy is unaffected in all cases. If a two-way sync is ever added, this
becomes a real concern and the lossy set is exactly the list that must not be written back.

## Should any of these be retyped instead?

Retyping a scalar to a vector to match upstream is the cleaner long-term answer for the speed and
acceleration families - it would make the fork's model match the hardware (H2D/H2C really do have
per-extruder speeds) and remove the lossy class entirely.

**Update (2026-09-22):** the High-Flow work (c3c82dbf64) has since done this for 26 of the 32
options, as per-flow-variant vectors (table D2). The six left in table A are the ones that remain
scalar. The advice below still applies to them.

**It was not done as part of this change, and should not be done casually.** The High-Flow retype of
`internal_bridge_speed` from a scalar to a vector is precisely what produced this week's `G1 F0`
outage: every read site has to be audited, because a `coFloat` read as `opt->value` silently
becomes `opt->values[idx]` with a different default and a different empty-vector behaviour.

If it is attempted later, the cost per option is roughly:
- every `config.opt_float("key")` / `->value` read site retargeted with a correct extruder index
- the GUI field for the option converted to a per-extruder control
- profile migration for every shipped profile that sets the option as a scalar
- a regression test asserting the option is non-zero at the G-code writer

Of the six left, `travel_speed_z` is the best candidate (few read sites) and
`top_solid_infill_flow_ratio` the worst (widely read, rarely differs per extruder). Doing them
one at a time, each with its own gate, is the only safe route.

## Tests

`tests/slic3rutils/bambu_config_compat_tests.cpp`, tag `[BambuCompat]`. Covers each rule in the
table above on both paths - the collapse path on a key picked at run time from table A that must
still be scalar (so a future retype fails loudly instead of silently testing a vector), and the
backfill path on the retyped keys - plus an all-nil array, an untouched nullable option, a
regression asserting that no translation ever yields `0` for a speed or acceleration while a zero
Bambu really wrote is preserved, and an end-to-end check that a translated H2D preset is read at
slot 0 for standard and high-flow filaments. Fixtures in `tests/data/bambu_compat/` and
`tests/data/bambu_compat_43/` are copies of real presets from the owner's Bambu Studio data;
nothing reads the live folder at test time.

`tests/libslic3r/test_bambu_import_renames.cpp`, tag `[BambuAliases]` (renamed keys): the alias
table names real keys once each (and no Bambu name is one of ours); every Bambu name loads as our
key with its value converted, including the key-only pass and `different_settings_to_system`;
every enum value Bambu can write for a shared key loads; our config survives Export Bambu 3MF and
our own import for every row of the table; the documented lossy cases; both spellings in one
file keep ours (JSON and per-object, either order); Bambu's own `auto_pa_line_single.3mf` keeps
its prime tower, clearance radius, wipe and ironing settings; Bambu user and system presets map
theirs. Fixtures in `tests/data/bambu_import_renames/` are unmodified copies from Bambu Studio's
`resources`. The hidden `[.bambu_corpus]` case produced the corpus table above.

### Verified results

Re-verified 2026-09-22 on `fix/bambu-compat-tests` (main 0ebb94d331 plus test changes only):

`[BambuCompat]`: 27 test cases, 962 assertions, all passing.

`slic3rutils_tests` (default run, as ctest runs it): 255 test cases, 3522 assertions, all passing. The three `[Http]`
cases that talk to github.com and jigsaw.w3.org are hidden (`[.network]`): jigsaw.w3.org's
`/HTTP/Basic/` answers 403 to every client now, plain curl included, so it failed on every machine
without saying anything about this code. Run them on purpose with `slic3rutils_tests "[network]"`.

The 43 presets named in `2026-09-21-16-52-41.log.0` as `parse config ... failed`, run through the
fork's own `load_from_json`:

| outcome | count |
|---|---|
| now load successfully | **43 / 43** |
| still fail | 0 |
| load with at least one lossy value | 2 |
| total lossy values | 4 |
| nil arrays translated, all via backfill (no scalar collapse left in this corpus) | 389 |

The four lossy values, all `BackfilledLossy` on an H2D preset where the second nozzle genuinely
differs (before the High-Flow retype they were `CollapsedLossy` to 100, 100, 100 and 50):

```
0.20mm @H2D - ABS.json     inner_wall_speed            100,nil,100,nil,150 -> 100,100,100,100,150
0.20mm @H2D - ABS.json     internal_solid_infill_speed 100,nil,100,nil,180 -> 100,100,100,100,180
0.20mm @H2D - ABS.json     sparse_infill_speed         100,nil,100,nil,180 -> 100,100,100,100,180
0.30mm @H2D - Custom.json  support_interface_speed     30,50,nil,50,nil    -> 30,50,50,50,50
```

No real value is lost any more: only the `nil` slots are guesses. The slicer reads slot 0, so the
last one now slices at 30 (Bambu's extruder-1 standard value) rather than the old majority 50.

### Overrides in a 3MF

`tests/libslic3r/test_bambu_nil_override.cpp`, tag `[BambuOverride]` (also `[BambuCompat]`):
`translate_nil_override` (nil in the middle, nil in slot 0, nearest parent, slot-count mismatch,
all-nil, no parent, scalar targets, never a zero), `set_deserialize` under nested
`OverrideScope`s and outside any scope, and Bambu Studio's own
`resources/calib/pressure_advance/auto_pa_line_dual.3mf` (fixture in
`tests/data/bambu_compat_3mf/`, AGPL like this project): it loads with and without its config,
every modifier holds `80,<project slot>,80,<project slot>`, nothing is reported, and it
round-trips through this fork's own 3MF writer unchanged.

A hidden harness, `libslic3r_tests "[.corpus3mf]"` with `EDGE_3MF_CORPUS=<dir>`, loads every
`.3mf` under a directory and prints one line per file. Run 2026-09-22 on copies of every 3MF in
Bambu Studio's `resources/` (5), this fork's `resources/` (12) and `tests/` of the main checkout
(13), with and without the `set_deserialize_raw` hook:

| corpus | before | after |
|---|---|---|
| Bambu Studio resources | 4 / 5 (`auto_pa_line_dual.3mf`: *Deserializing nil*) | 5 / 5 |
| EdgeSlicer resources | 12 / 12 | 12 / 12 |
| main checkout `tests/` | 13 / 13 | 13 / 13 |

No file in the corpus needed a lossy override translation.

## Note for other work in flight

The destructive delete-on-parse-failure in `PresetCollection::load_presets` is **not** addressed
here - this change prevents the parse failure, it does not make failure non-destructive. That is
tracked separately (quarantine instead of erase). This branch does not modify `Preset.cpp` at all,
so the two changes should not conflict.
