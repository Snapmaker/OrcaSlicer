# Snapmaker Orca FullSpectrum v0.9.9

Local-Z toolchange reduction, prime tower planning, and physical-color geometry fixes.

## Quick Overview

- Greatly reduces Local-Z toolchanges by batching same-plane work across objects and scheduling Local-Z passes with dependency chains.
- Reworks Local-Z prime tower behavior so Local-Z swaps are planned ahead of time instead of using oversized reserve slots.
- Fixes Local-Z prime tower correctness issues, including missing plan lookups, final-purge sentinel handling, and empty nominal-toolchange layers.
- Fixes classic Local-Z physical-color regions so ordinary physical filament zones stay at nominal layer height instead of inheriting mixed-row micro-heights.
- Keeps whole-object Local-Z behavior available, while giving physical-color zones a clean two-pass split instead of arbitrary mixed-row pass heights.

## More Detailed Overview

### Local-Z Toolchange Scheduling

- Groups Local-Z phase-b perimeter work by shared micro-pass `print_z` across objects, instead of restarting optimization for each object layer.
- Adds dependency-chain metadata to Local-Z sublayer plans so independent swatches or mixed regions can be scheduled in a tool-friendly order without violating vertical pass dependencies.
- Extends dependency-chain tagging to single-row split intervals, which lets common calibration swatches use the optimized Local-Z scheduler instead of falling back to raw pass order.
- Uses the dependency scheduler in both G-code emission and Local-Z wipe-tower preplanning so the tower and object path agree on the same toolchange sequence.
- Carries the actual ending Local-Z tool into the nominal layer loop instead of forcing a restore to the pre-pass tool, removing a recurring unnecessary toolchange pattern.
- Improves Local-Z island assignment with more robust interior sampling so boundary-starting paths are less likely to be assigned to the wrong island bucket.

### Local-Z Prime Tower

- Preplans Local-Z wipe-tower swaps as real toolchange events instead of treating them as late, unplanned reserve-slot users.
- Replaces the old padded Local-Z reserve-band behavior with exact planned Local-Z tower toolchanges, making the tower denser and less wasteful.
- Keeps the old reserve-slot path as a fallback if runtime ever diverges from the preplanned sequence.
- Fixes Local-Z tower layers that contain only Local-Z toolchanges and no nominal tower toolchanges.
- Fixes final-purge sentinel handling so the tower does not mistake the final unload for a normal planned toolchange.
- Makes nominal wipe-tower generation consume exact planned toolchange entries directly, reducing fragile lookup behavior when the current tool state drifts.

### Local-Z Geometry Correctness

- Fixes classic Local-Z mode so pure physical-filament regions remain in the normal nominal layer instead of being pulled into mixed Local-Z micro-passes.
- Fixes overlapping thick/thin layer bands in physical-color zones caused by fixed physical masks inheriting mixed-row variable pass heights.
- Keeps Local-Z micro-height splitting focused on actual mixed-filament painted regions in classic mode.
- Updates whole-object Local-Z handling so physical-color regions can still participate when requested, but use a clean two-pass half-layer split rather than arbitrary mixed-row heights.
- Preserves fixed-color guard behavior around whole-object Local-Z mixed masks so painted mixed regions do not grow into neighboring physical-color caps.

## Notes

- macOS builds from this fork remain unsigned and not notarized.
- Release downloads for this fork are available here: https://github.com/ratdoux/OrcaSlicer-FullSpectrum/releases
- Use at your own risk.
