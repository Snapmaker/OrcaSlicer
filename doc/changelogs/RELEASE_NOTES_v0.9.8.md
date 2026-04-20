# Snapmaker Orca FullSpectrum v0.9.8

Local-Z multicolor improvements, duplicate gap-fill cleanup, and mixed-filament workflow fixes.
Compared to the previous FullSpectrum v0.9.7 release.
Based on Snapmaker Orca v2.3.1, with the bundled print-preprocessing Flutter/web resources kept on the newer 2.3.12 line.

## Quick Overview

- Adds optional whole-object Local-Z support for mixed wall regions instead of limiting Local-Z strictly to painted mixed zones.
- Adds an experimental direct multicolor Local-Z solver with carry-over error for 3+ color mixed rows, reducing pair-cadence banding.
- Fixes several Local-Z boundary issues so painted colors, fixed-color islands, and whole-object mixed masks cooperate more cleanly.
- Fixes a duplicated inner-loop gap-fill regression that could overprint narrow collapsed regions.
- Fixes mixed-filament bias-state and print-preprocessing remap issues that required restarting or blocked otherwise valid remaps.

## More Detailed Overview

### Local-Z Dithering

- Restores and relabels the Local-Z lower and upper height bounds in the Others tab so they are clearly presented as Local-Z controls.
- Adds `Apply Local-Z to whole mixed objects` as an experimental opt-in, allowing default mixed wall regions to keep Local-Z active outside painted islands.
- Adds `Use direct multicolor Local-Z solver` as an experimental opt-in for 3+ color rows, using direct component allocation with carry-over error across layers instead of collapsing everything into pair cadence first.
- Keeps the older pair-based Local-Z path as the default for compatibility, while letting the new direct solver be tested independently.
- Fixes several planner/runtime issues around painted zones inside whole-object Local-Z bodies, including lost painted Local-Z treatment, phase resets at zone boundaries, and overlap/regrowth artifacts at fixed-color painted caps.
- Improves Local-Z summary text in the mixed-filament panel so the UI reports when the direct multicolor solver is active instead of describing the older pair-cadence model.

### Gap Fill And Toolpath Generation

- Removes the collapsed infill gap-fill fallback that was appending duplicate gap-fill passes in narrow looped regions.
- Resolves the regression where certain capsule-like inner loops could receive a doubled gap-fill contour and get visibly over-squished.
- Keeps the safer older path that avoided the duplicate loop in the reproduced meshes.

### Mixed-Filament Workflow Fixes

- Fixes mixed-filament bias controls getting stuck disabled after loading certain `.3mf` projects and then creating a new project without restarting.
- Keeps the mixed-filament project config mirrored back into the sidebar state when presets or projects are reloaded programmatically.
- Fixes print-preprocessing filament remap for the newer web dialog by sending file-side nozzle diameter data expected by the 2.3.12 frontend.
- Reduces the mismatch between automatic tray selection and manual remap eligibility in the preprocessing flow.

### UI Cleanup

- Retires the old height-weighted cadence and pointillisme-facing controls from the Others tab while keeping compatibility for older stored configs and projects.
- Keeps the Local-Z-related controls that are still in active use visible and synchronized with the current mixed-filament workflow.

## Notes

- `cmake --build build --target Snapmaker_Orca --config Release --parallel 4` succeeded.
- Much of this release was validated through repeated slice-preview and hardware-oriented regression checks for mixed-filament and Local-Z behavior.
- The active build tree in this workspace still had no registered `ctest` cases, so broader automated test reruns were not available from `ctest` here.
- macOS builds from this fork remain unsigned and not notarized.
- Release downloads for this fork are available here: https://github.com/ratdoux/OrcaSlicer-FullSpectrum/releases
- Use at your own risk.
