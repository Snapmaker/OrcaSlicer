# Design: Carry process settings across printer switches as revertable modifications

Date: 2026-09-26
Status: Approved (approach A, user confirmed; filaments explicitly out of scope)

## Problem

Switching printers auto-selects the nearest compatible process (print) profile —
alias match, then the printer's preferred default, then nearest layer height.
The selection lands via `PresetCollection::select_preset()`, which discards the
previously edited config (`m_edited_preset = m_presets[idx]` — its own comment:
"This resets all the edits done to the currently selected preset"). Any values the
user had dialed in — whether unsaved tweaks or simply the previous printer's
profile values — are silently lost, and switching back does not restore them.

## Goal

When a printer switch forces the process preset to change (the previous one is
incompatible with the new printer), carry the previous *edited* process config
onto the auto-matched preset as modifications:

- carried values show as changes vs the matched profile (orange revert arrow /
  "(modified)" suffix), each individually revertable;
- reverting all returns the user to the exact matched profile;
- switching back and forth between two printers no longer loses settings.

Filament presets are out of scope (separate, riskier question).

## Current mechanism (as found)

- `PresetBundle::update_compatible()` (PresetBundle.cpp ~4665) recomputes
  compatibility after a printer change and calls
  `prints.update_compatible(active_printer, nullptr, select_other, PreferedPrintProfileMatch(...))`.
- `PresetCollection::update_compatible_internal()` (Preset.cpp ~2965) deselects
  the current print preset when it is incompatible with the new printer
  (`m_idx_selected = -1`), and the template wrapper (Preset.hpp ~701) then selects
  `first_compatible_idx(match)`.
- `PresetCollection::select_preset()` (Preset.cpp ~3315) resets the edited preset
  to the selected one, discarding prior values.
- The dirty/modified UI (orange arrows, undo) is driven by the existing diff
  between the edited preset and the selected preset (`deep_diff`,
  `current_is_dirty()`), so no new UI work is needed if the edited preset simply
  holds the carried values.

## Design (approach A)

Single insertion point: `PresetBundle::update_compatible()`, FFF branch, around
the `prints.update_compatible(...)` call.

1. **Capture before**: copy the print collection's edited preset name and config
   (only if a preset is currently selected).
2. **Reselect as today** (matching logic unchanged).
3. **Transfer after**: if the selected preset changed to a different name,
   apply the captured config onto the new edited preset:
   - skip identity/compatibility keys (`inherits`,
     `compatible_printers_condition`, `compatible_prints_condition`,
     `compatible_printers`, `compatible_prints`);
   - skip options whose type differs between old and new config;
   - skip vector options whose length differs (extruder-count-dependent values
     fall back to the matched profile's value for that key);
   - everything else is cloned over.
4. **Dirty state**: after the transfer, the edited preset differs from the
   selected preset exactly in the carried/changed keys, so the existing dirty
   machinery renders orange arrows and the "(modified)" suffix automatically.

### Scope gate

The transfer only fires when the previous selection was deselected due to
incompatibility and a *different* preset got selected — i.e., a genuine printer
switch. Startup restoration and project load select compatible presets by name
(no deselection), so they are unaffected. Manual preset switching keeps its
existing unsaved-changes dialog.

### Round-trip property

A (clean PA) -> B (matched PB + A-values as dirty) -> A again: the second switch
carries the (A-valued) edited config back onto PA, restoring the original A
settings almost exactly.

## Error handling / edge cases

- Nothing selected before the switch (idx == -1): skip transfer.
- New match is the same preset (still compatible): no transfer.
- No compatible profile at all (lands on the default profile): transfer still
  applies; everything shows as modified, which faithfully represents "no matching
  profile, here are your settings".
- Edits made to the default profile are carried too (edited config is the source
  of truth), even though the matcher never *selects* default/external profiles.

## Testing

- Manual matrix in the GUI: A->B shows orange arrows on carried options;
  single-option revert works; "discard all changes" restores the matched profile;
  A->B->A round-trips.
- Build verification: `libslic3r` Release build in the isolated worktree
  (`C:\Dev\wt_kimi\build`, under `wt_lock.sh` with `-m:4`).
