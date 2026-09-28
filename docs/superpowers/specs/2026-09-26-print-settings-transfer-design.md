# Design: Carry process settings across printer switches as revertable modifications

Date: 2026-09-26, revised 2026-09-28 (review of PR #176)
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

3. **Vectors**: values whose length differs between the two profiles keep the
   matched profile's value (extruder-count-dependent). Exception: the per-flow-mode
   process options (`process_flow_variant_options()`, e.g. `outer_wall_speed`) are
   ordered by each profile's own `process_flow_support`; when the two mode lists
   differ, entries are matched up by mode name ("standard" -> "standard"), and a
   mode the previous profile did not have keeps the matched profile's value. This is
   what lets speeds carry from a standard-only X1C profile onto an H2C profile with
   standard + high-flow entries.
4. Options whose type differs, or that are missing on either side, are skipped.

## Where it runs

Only the GUI printer switch, `Tab::select_preset()` on the Printer tab, FFF -> FFF:

1. **Snapshot** before anything changes: `PresetBundle::capture_print_settings_carry()`
   (edited process preset name + config, modifications included, and the edited
   printer's `nozzle_diameter`). The process preset is exempt from the
   transfer/discard dialog and from the discard that follows it - the carry owns it.
2. The printer is selected and `update_compatible()` re-matches the process preset
   exactly as before; with "Remember printer configuration" `update_selections()`
   may then pick the process last used on that printer.
3. **Apply** last, onto whatever process preset was finally selected:
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

A (clean PA) -> B (matched PB + A's values as modifications) -> A again: the second
switch carries B's edited config (A's values, except B's printer-coupled keys) back
onto PA, leaving only the user's own earlier modifications dirty. Across a nozzle
size change the geometry keys follow each printer's matched profile.

## Testing

- `tests/libslic3r/test_print_settings_carry.cpp`: key classification, the same
  nozzle size rule, same-nozzle full carry minus printer-coupled keys, nozzle-change
  carry minus geometry keys, vector length skip, flow-mode matching, and a
  `PresetBundle` switch with dirty state, single revert, discard-all and A -> B -> A.
- Manual GUI matrix (owner): X1C -> H2D and X1C -> U1 (same nozzle), X1C 0.4 ->
  0.2 (nozzle change), A -> B -> A, single revert, discard all, Print-tab preset
  switch notification.
