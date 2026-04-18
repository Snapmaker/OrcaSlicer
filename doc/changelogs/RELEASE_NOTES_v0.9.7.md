# Snapmaker Orca FullSpectrum v0.9.7

Color-preview unification, print-preprocessing filament matching fixes, and release workflow cleanup.
Compared to `fs-0.96`.
Based on Snapmaker Orca v2.3.1, with the bundled print-preprocessing Flutter/web resources synced forward to the newer 2.3.12 line.

## Quick Overview

- Unifies mixed-filament display color computation so generated recipes, cached previews, and editor rows use the same color logic.
- Improves preview colors for grouped manual patterns, Local-Z previews, same-layer patterns, and bias-aware pair displays.
- Fixes print-preprocessing filament auto-matching so same-type trays can be matched by nearest color again instead of behaving like exact-color-only.
- Normalizes file-side filament types more consistently, including support-material IDs, to reduce false preprint type mismatches.
- Updates the bundled `flutter_web` preprocessing page and release-note lookup flow for cleaner fork releases.

## More Detailed Overview

### Mixed-Filament Color Consistency

- Added a shared mixed-filament display context/helper used by `MixedFilamentManager`, the mixed-color recipe dialog, and the plater-side previews.
- Reworked preview color generation for grouped manual patterns, effective pair ratios, same-layer modes, and Local-Z preview subdivision.
- Moved bias-aware apparent-color calculations onto shared helpers so the manager and UI no longer drift apart.
- Refreshed mixed-filament row colors from the same physical-color context used when new color-match recipes are created.
- Reduced mismatch between newly created mixed filaments, cached previews, and what is shown after reopening or editing rows.

### Print Preprocessing Filament Matching

- Synced the bundled `flutter_web` print-preprocessing resources to the newer 2.3.12 line.
- Fixed the preprint matcher so approximate same-type colors like cyan, yellow, and magenta can fall back to the nearest tray color instead of effectively requiring exact color equality.
- Relaxed the fallback nozzle check so trays are not skipped just because the file-side nozzle value is absent in the web payload.
- Disabled stale Flutter service-worker re-registration in the desktop bootstrap and added cache cleanup in `index.html` so updated preprocessing pages can replace old cached shells on user machines more reliably.
- Bumped the bundled `flutter_web` build number so newer repo-shipped resources overwrite older AppData copies on startup.
- Carries forward the upstream print-preprocessing fix from the synced Flutter bundle for projects with more than four preset filaments, so excess presets remain selectable in the matching dialog.

### Filament Type Normalization

- Fixed `DynamicPrintConfig::get_filament_type()` to use `filament_ids` instead of the singular `filament_id`.
- Updated `sw_GetFileFilamentMapping()` to build file-side filament types from `full_print_config()` and the normalized helper instead of forwarding raw config strings.
- Added regression coverage for support-material normalization and ordinary-material passthrough.
- Reduces false "filament type does not match" warnings when the visible UI label looks correct but the underlying raw strings do not.

### Workflow And Release Maintenance

- Updated the release workflow to prefer `doc/changelogs/RELEASE_NOTES_v*.md` before changelog files when assembling release notes.
- Carries forward the README warning that some older FullSpectrum `.3mf` projects may not migrate cleanly because mixed-filament serialization has evolved.

## Notes

- This release was validated on actual hardware while reproducing and fixing the print-preprocessing filament auto-match issue.
- `node --check` passed for the synced `flutter_web` JavaScript files.
- `cmake --build build --target Snapmaker_Orca --config Release --parallel 4` succeeded.
- The active build tree in this workspace had no registered `ctest` cases, so broader automated test reruns were not available from `ctest` here.
- macOS builds from this fork remain unsigned and not notarized.
- Release downloads for this fork are available here: https://github.com/ratdoux/OrcaSlicer-FullSpectrum/releases
- Use at your own risk.
