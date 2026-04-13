# Snapmaker Orca FullSpectrum v0.9.51

Mixed-filament bias controls, preview updates, and release cleanup.
Based on Snapmaker Orca v2.3.1.

## Highlights

### Mixed Filament Bias
- Added an optional per-pair **Bias** control for mixed filaments.
- Positive bias recesses the second filament in the pair.
- Negative bias recesses the first filament in the pair.
- Added clamping and signed-bias handling so the slicer and UI use the same safe range.

### Mixed Filament Workflow Improvements
- Added a print setting to enable or disable mixed-filament bias mode.
- Moved the bias control inline with the mixed-filament preview row to keep the editor compact.
- Added apparent-color preview feedback so biased pairs can show an estimated shifted mix in the UI.
- Simplified the final bias workflow so the control behaves consistently across mixed filament pairs.

### Slicing And Preview Fixes
- Applied bias through the mixed-filament slicing path so recessed layers affect generated geometry instead of only the editor preview.
- Fixed mixed-filament panel refresh behavior when the bias setting is toggled.
- Improved preview behavior for biased mixed filaments and reduced mismatches between the row preview and sliced output.
- Fixed region handling so recessed mixed-filament space is not immediately refilled by the other component in the common painted path.

### Release And Project Maintenance
- Bumped the FullSpectrum version to `0.9.51`.
- Moved older changelogs and release notes into `doc/changelogs/` and updated the release workflow to read them from there.
- Refreshed the README mixed-filament section to document the Bias feature and added a compatibility warning for older `.3mf` projects.

## Tests

- Expanded `test_mixed_filament.cpp` coverage for signed bias, clamping, serialization, and remapping behavior.
- Rebuilt `libslic3r_gui`.
- Rebuilt `libslic3r_tests` and re-ran the `[MixedFilament]` suite successfully.

## Important Notes

- Some `.3mf` files created with older FullSpectrum builds may not load or migrate cleanly in newer versions because mixed-filament project data has evolved.
- macOS builds from this fork remain unsigned and not notarized.
- Use at your own risk.
