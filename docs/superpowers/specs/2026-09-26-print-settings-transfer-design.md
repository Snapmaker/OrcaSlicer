# Design: Carry process settings across printer switches as revertable modifications

Date: 2026-09-26, revised 2026-09-28 (review of PR #176, then the owner's first hand test)
Status: Approved (owner); filaments explicitly out of scope

## Problem

Switching printers auto-selects the nearest compatible process (print) profile -
alias match, then the printer's preferred default, then nearest layer height (and,
with "Remember printer configuration", the process last used on that printer).
The selection lands via `PresetCollection::select_preset()`, which discards the
previously edited config (`m_edited_preset = m_presets[idx]`). Any values the user
had dialed in - unsaved tweaks, or simply the previous printer's profile values -
are silently lost, and switching back does not restore them. A user who wants the
same result from one object on two printers needs two identical process profiles.

## Goal (owner's intent)

On a printer switch the slicer still auto-selects the nearest matching process
profile, but the previous process settings (walls, speeds, supports, infill etc. -
the whole profile, not just unsaved edits) are applied on top of it as
modifications:

- carried values show as changes vs the matched profile (orange revert arrow /
  "(modified)" suffix), each individually revertable to the matched profile's value;
- "discard all changes" returns to the matched profile exactly;
- switching back and forth between two printers no longer loses settings;
- a short notification says what was kept.

## Rules

A single rule set, `carry_print_settings()` in `PresetBundle.cpp`:

1. **Printer-coupled keys never carry** (`print_carry_is_printer_coupled`). They
   describe the machine, its firmware or the preset itself, and the matched
   profile's value is kept on every switch:

   | Group | Keys | Why |
   |---|---|---|
   | Preset identity | `inherits`, `compatible_printers(_condition)`, `compatible_prints(_condition)`, `notes` | which printers the profile belongs to; the profile's own description |
   | Per-machine vector layout | `print_extruder_id`, `print_extruder_variant`, `process_flow_support` | H2C and H2D Pro both have 5 variant entries in a different order; the flow-mode list is the machine's |
   | Idle-tool temperatures | `ooze_prevention`, `standby_temperature_delta`, `preheat_time`, `preheat_steps`, `delta_temperature` | tuned per machine (Bambu profiles -5 standby delta, the U1 toolchanger ooze prevention with -150) |
   | Machine calibration | `elefant_foot_compensation(_layers)`, `xy_contour_compensation`, `xy_hole_compensation` | dimensional compensation measured on one machine |
   | Prime tower / purge | `enable_prime_tower`, `prime_volume`, `preload_all_filaments`, `enable_tower_interface_features`, `local_z_wipe_tower_purge_lines`, `wiping_volumes_extruders`, `single_extruder_multi_material_priming`, `flush_into_infill/objects/support`, every `wipe_tower_*` and `prime_tower_*` key | depends on the filament-change hardware (AMS single nozzle, toolchanger, dual nozzle) |
   | Toolchange ordering | `toolchange_ordering`, `toolchange_cyclic_order`, `toolchange_cyclic_first_layer` | toolchanger strategy |
   | Firmware / G-code output | `timelapse_type`, `exclude_object`, `gcode_label_objects`, `enable_arc_fitting`, `gcode_add_line_number`, `gcode_comments`, `filename_format`, `post_process` | camera moves, object labelling, G2/G3 support, output naming and scripts |
   | Firmware motion semantics | `default_jerk`, `outer_wall_jerk`, `inner_wall_jerk`, `infill_jerk`, `top_surface_jerk`, `initial_layer_jerk`, `travel_jerk`, `default_junction_deviation`, `accel_to_decel_enable`, `accel_to_decel_factor` | Marlin jerk / junction deviation vs Klipper square-corner velocity; accelerations and speeds DO carry |
   | Calibration internal | `calib_flowrate_topinfill_special_order` | set by the flow-rate calibration, not a user setting |

2. **Nozzle-size change: geometry keys keep the matched profile's values**
   (`print_carry_is_nozzle_geometry`): every key ending in `line_width`,
   `layer_height`, `initial_layer_print_height`, `support_top_z_distance`,
   `support_bottom_z_distance`, `raft_contact_distance`,
   `infill_combination_max_layer_height`, `zaa_min_z`, `ironing_spacing`,
   `support_ironing_spacing`, `seam_gap`, `wipe_inward_distance`,
   `tree_support_tip_diameter`. Everything else (speeds, walls, infill, supports ...)
   still carries: the filament's max volumetric speed caps the flow on a smaller
   nozzle. Without this, X1C 0.4 -> X1C 0.2 left 0.42 line widths / 0.2 layers under
   "0.10mm Standard @BBL X1C 0.2 nozzle", and going up in size raised "Too small
   line width".

   *Same nozzle size* = every `nozzle_diameter` of the old printer and every
   `nozzle_diameter` of the new printer are equal within 1e-6 (X1C 0.4 -> H2D 2x0.4
   and -> U1 4x0.4 count as same; any mixed-size machine on either side does not).
   The old printer's diameters are the edited values at the moment of the switch.

3. **Per-variant vectors**. Bambu's H2-series process profiles store speeds,
   accelerations etc. once per extruder variant: `print_extruder_id` /
   `print_extruder_variant` (H2S: 3 entries of extruder 1 - Standard, High Flow,
   E3D High Flow; H2C: 5; H2D: 7 over two extruders), while an X1C profile has one
   entry. The first version only knew the flow-mode layout and skipped every such
   key on a length mismatch - initial layer, top surface, overhang speeds and most
   other speeds did not carry X1C -> H2S. Now each entry is labelled
   "extruder|variant" (from the Bambu layout, or from `process_flow_support` for the
   per-flow-mode options, "standard" = "Direct Drive Standard", "high_flow" =
   "Direct Drive High Flow") and matched: same extruder + variant first, then the
   same variant on any extruder. A one-variant printer therefore fills every
   standard entry of the new printer; entries without a counterpart (High Flow,
   E3D, TPU when the old printer had only a standard nozzle) keep the matched
   profile's value. Vectors without any variant layout whose length differs keep the
   matched value (extruder-count-dependent).
4. Options whose type differs, or that are missing on either side, are skipped
   (none do between the bundled X1C, H2S, H2D, H2C and U1 profiles, see the audit).

5. **Settings conflicts**. The process page rewrites (or asks about) some
   combinations every time it updates (`ConfigManipulation::update_print_fff_config`,
   `toggle_print_fff_options`). A carry must never create one: a rewritten value is a
   modification the user did not make, and a dialog would pop on a printer switch.
   When a rule is broken after the carry but holds in the matched profile, carried
   keys are put back to the matched value in order until it holds
   (`print_carry_conflicts()` in PresetBundle.cpp):

   | Rule | Restored first |
   |---|---|
   | support style valid for the support type (supports on) | `support_style`, `support_type` |
   | spiral vase prerequisites (walls 1, no top shells/infill/support, traditional timelapse ...) | `spiral_mode` |
   | alternate extra wall vs ensure vertical shell thickness = All | `alternate_extra_wall`, `ensure_vertical_shell_thickness` |
   | scarf seam start height below the layer height | `seam_slope_start_height`, `seam_slope_type` |
   | infill lock depth not above skin depth | `infill_lock_depth`, `skin_infill_depth` |
   | offset layers prerequisites | `offset_layers` |
   | Extrusion/Combined fuzzy skin needs Arachne | `fuzzy_skin_mode`, `wall_generator` |
   | layer height not above the printer's `max_layer_height` | `layer_height` |
   | multiline infill only with a multiline pattern | `fill_multiline`, `sparse_infill_pattern` |
   | reverse internal overhangs only needs a 0% threshold | `overhang_reverse_threshold`, `overhang_reverse_internal_only` |
   | extrusion rate smoothing turns arc fitting off (arc fitting is printer-coupled) | `max_volumetric_extrusion_rate_slope` |

   A value dropped this way is remembered and offered again on the next switch (if
   the user has not changed the value kept in its place), so A -> B -> A still
   returns it. Tree-support keys, `ironing_type` and the Locked Zag sub-patterns are
   only enabled/disabled by the page, never rewritten, so they need no rule.

## Where it runs

Only the GUI printer switch, `Tab::select_preset()` on the Printer tab, FFF -> FFF:

1. **Snapshot** before anything changes: `PresetBundle::capture_print_settings_carry()`
   (edited process preset name + config, modifications included, and the edited
   printer's `nozzle_diameter`). The process preset is exempt from the
   transfer/discard dialog and from the discard that follows it - the carry owns it.
2. The printer is selected and `update_compatible()` re-matches the process preset
   exactly as before; with "Remember printer configuration" `update_selections()`
   may then pick the process last used on that printer.
3. **Pick the target**: `PresetBundle::select_print_carry_target()` prefers the
   preset the carried values came from, then the process last used on the new
   printer, when compatible (see Round trip).
4. **Apply** last, onto that process preset:
   `PresetBundle::apply_print_settings_carry()`. The edited preset now differs from
   the selected one exactly in the carried keys, so the existing dirty machinery
   renders the orange arrows and "(modified)" suffix.

The carry used to live inside `PresetBundle::update_compatible()`. It moved out
because (a) with "Remember printer configuration" (the default) `update_selections()`
re-selected the remembered process right after and wiped the carried values, and
(b) `update_compatible()` also runs on temporary bundles (preset export, diff
dialog) and other non-switch paths. `update_compatible()` is back to its original
behaviour.

### Not affected

3MF project load, `load_presets`, startup `load_selections`, config-file import and
the CLI never call `Tab::select_preset()` for a printer switch, so they never carry.
The "keep my printer on open" option does switch printers through
`Tab::select_preset()` after a project loads, so it carries the project's process
settings like a manual switch (and then re-applies the project's modifications as
before).

## Same-slot preset switches (auto-transfer)

Picking another preset in the same slot (e.g. another process preset in the Print
tab, or another filament of the same type) with unsaved modifications no longer
shows the "Transfer or discard changes" dialog: `UnsavedChangesDialog::ShowModal()`
answers Transfer when a Transfer button was actually offered
(`m_buttons & TRANSFER && m_transfer_btn != nullptr`; the bit stays set when
`build()` withheld the button on a printer-technology mismatch, which must still
show the dialog). The modifications stay dirty on the new preset.

## Notification

After a switch that kept at least one setting, one short, fading notification
(`NotificationType::PresetSettingsCarried`, the next switch replaces it):

- printer switch: "Kept 37 process settings from "0.20mm Standard @BBL X1C"
  (revertable)." plus "Line widths and layer heights follow the new nozzle size."
  when the nozzle size changed;
- auto-transfer: "Kept 3 modified process settings from "..." (revertable)." (or
  filament settings). A printer switch that also auto-transfers filament edits
  shows both lines in the same notification.

## Round trip

The first version re-matched the process preset from scratch on the way back, so
X1C -> H2S -> X1C landed on the X1C preset nearest to the H2S's, not the one the user
started from. Now the carry records its origin: `capture_print_settings_carry()`
sets `origin_preset` to the current process preset, or - while still on the preset
a previous carry was applied onto - to that carry's origin. The bundle also records,
per printer, the process preset last used on it (this session). Before applying,
`select_print_carry_target()` selects the origin if it is compatible with the new
printer, else the printer's last used process if compatible, else keeps the
auto-matched (or, with "Remember printer configuration", the remembered) preset.
So A -> B -> A and A -> B -> C -> A land on A's original preset with only the
user's own edits dirty, with the remember option on or off. Picking another process
preset by hand starts a new origin.

## Presets that store an inconsistent support style

Found in the owner's hand test: a user preset ("0.24mm @X1C - HexBase") stored
`support_style = tree_hybrid` under the inherited `support_type = normal(auto)` with
supports on. `update_print_fff_config` reset the style to Default on every page
update, so the preset was dirty as soon as it was loaded and "revert" could never
clear it (revert wrote tree_hybrid back, the update reset it again). The reset now
skips a style the saved process preset itself stores (`TabPrint::update` passes the
selected preset as reference). The slicer resolves such a style exactly like
Default (a tree style under normal support prints grid, a normal style under tree
support prints organic), and the combo box already shows Default for it.

## Testing

- `tests/libslic3r/test_print_settings_carry.cpp`: key classification, the same
  nozzle size rule, same-nozzle full carry minus printer-coupled keys, nozzle-change
  carry minus geometry keys, vector length skip, flow-mode matching, per-variant
  matching (X1C/H2S/H2D/U1 layouts), settings conflicts, a `PresetBundle` switch with
  dirty state, single revert, discard-all, and round trips A -> B -> A and
  A -> B -> C -> A (origin, last used, deferred conflict values, manual re-pick).
- Audit (`[Audit]` case) with the bundled profiles: X1C 0.4 -> H2S, H2D, H2C and U1
  0.4, listing every process key not (fully) carried with its reason and failing on
  any reason outside the deliberate ones.
- Manual GUI matrix (owner): X1C -> H2D and X1C -> U1 (same nozzle), X1C 0.4 ->
  0.2 (nozzle change), A -> B -> A, single revert, discard all, Print-tab preset
  switch notification.
