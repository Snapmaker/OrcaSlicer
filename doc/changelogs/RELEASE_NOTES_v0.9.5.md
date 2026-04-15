# Snapmaker Orca FullSpectrum v0.95

Mixed-color workflow improvements, Snapmaker 2.3.1 sync, and release polish.
Based on Snapmaker Orca v2.3.1.

## Quick Overview

- Rebased the fork onto the newer Snapmaker Orca 2.3.x line and updated the base to 2.3.1.
- Improved mixed-color generation so it can find better color matches and manage gradients more cleanly.
- Added more control over how infill uses filaments, including first-layer and last-layer behavior.
- Reduced fragmentation in painted mixed-color areas by collapsing same-color regions where possible.
- Refreshed release branding with the Snapmaker Orca FullSpectrum name, updated About dialog information, and the new FS app icon.

## More Detailed Overview

### Base Sync And Stability

- Synced FullSpectrum with the Snapmaker 2.3.0 update path and bumped the base version to Snapmaker Orca 2.3.1.
- Pulled in a wide set of upstream Linux, macOS, packaging, Flutter-resource, and web/download-flow fixes from the Snapmaker 2.3.x branch.
- Included the OpenVDB clang patch fix from PR #66.
- Kept the fork release flow focused on the fork itself, including the fork release link in the README and Windows portable packaging for releases.

### Mixed-Color And Dithering Improvements

- Added a brute-force color mixer path that can search for closer 2-, 3-, or 4-filament combinations from the selected physical filaments.
- Added `min_component_percent` validation so suggested mixes stay within more useful and realistic component ranges.
- Added controls for auto-generated mixed filament gradients, including preference handling and guardrails for very large generated sets.
- Added an explicit infill filament override setting so infill can use a different filament without relying on implicit behavior.
- Added per-layer control to keep base filament on the first and last layers before switching infill to the override filament.
- Added support for collapsing same-color mixed filament regions across painted areas so equivalent output colors can slice as larger continuous regions instead of separate islands.
- Added additional Local-Z and mixed-filament slicing fixes plus broader regression coverage in tests.

### Workflow And Usability Improvements

- Fixed the `.gcode.3mf` filename issue.
- Fixed wipe tower brim behavior inherited from the 2.2.3 line.
- Added and expanded tests around mixed filament handling, slicing behavior, and output generation.
- Updated visible app branding to Snapmaker Orca FullSpectrum.
- Updated the About page to show both the Snapmaker Orca base version and the FullSpectrum fork version.
- Replaced the app icon set with the new FS branding assets.

## Notes

- This release has been tested on actual hardware.
- macOS builds from this fork remain unsigned and not notarized.
- Release downloads for this fork are available here: https://github.com/ratdoux/OrcaSlicer-FullSpectrum/releases
