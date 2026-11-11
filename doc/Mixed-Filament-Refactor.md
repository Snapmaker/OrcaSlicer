# Mixed-filament refactor in the product port

Branch: `product_proposal_merge_with_mf_refactor`. Product-port baseline: `687192c8ce` on `product_proposal_merge`. The implementation is adapted from FullSpectrum snapshot `ba00f1d661`, including its typed weighted-blend model and separation of mixed-filament responsibilities.

## Model and ownership

`MixedFilamentManager` owns `MixedFilamentDefinition` objects. Each definition separates persistent identity, source, visibility, recipe, print behavior, and presentation. Pair and multi-material recipes share `MixedFilamentWeightedBlend`; manual patterns retain exact sequences of physical filament references grouped by perimeter.

The first two weighted components provide the legacy A/B projection. Gradient traversal order is stored separately with its stops and solid-color widths, so normalizing a weighted blend cannot reorder an existing gradient.

The compact project-setting rows remain supported through `MixedFilamentLegacyRow` adapters. The `MixedFilament` alias and existing row accessors remain compatibility interfaces for Snorca callers, as in the FS source snapshot. New code can use typed getters and setters. Edits through retained legacy row references are synchronized before typed queries or mutations; unchanged queries do not invalidate active definition references.

Unlike the FS visibility simplification, this port retains Snorca's saved `enabled` state as well as deletion tombstones. A structural refactor must not silently re-enable an unavailable mix. Stable IDs, UI mode, pure gradient endpoints, and the `p`/`v` stop and width tokens are preserved.

## Implementation layout

Code lives in `src/libslic3r/MixedFilament/`:

| File | Responsibility |
| --- | --- |
| `Common.cpp` | Numeric, cadence, and shared helpers |
| `Definition.cpp` | Typed recipes and legacy adapters |
| `LegacyRow.cpp` | Existing compact-row parsing and normalization |
| `Pattern.cpp` | Grouped manual filament sequences |
| `Gradient.cpp` | Weighted component parsing and normalization |
| `GradientPreview.cpp` | Product gradient stops, widths, sampling, and blended preview |
| `Display.cpp` | Product color prediction and display behavior |
| `Preview.cpp` | Layered preview and Local-Z pass calculations |
| `Resolver.cpp` | Typed virtual-to-physical filament resolution |
| `Manager.cpp` | Definition ownership, edits, IDs, and persistence |
| `Internal.hpp` | Private contracts between modules |

The public types and manager API remain in `MixedFilament.hpp`.

## Local-Z planning contract

Local-Z captures painted material channels during segmentation, then builds its plan after XY compensation, slicing repair, and polyhole conversion. Whole-object masks are derived from these final slices; captured painted masks are clipped to the final domain. The plan belongs to the `posSlice` stage. Filament edits affecting an existing plan invalidate that producing stage, including automatic gradients and implicit whole-object Local-Z.

`LocalZInterval` and `SubLayerPlan` live in `LocalZ/LocalZPlan.hpp`. Their `layer_id` is the actual `Layer::id()`, including the raft offset, rather than an index into `PrintObject::layers()`. Consumers use `local_z_interval_for_layer()` for lookup and `LocalZInterval::is_managed()` for geometry ownership. Independent cadence owns its masks even when a nominal interval emits zero or one pass; these intervals must also exclude flushing into model/support paths.

Independent cadence tracks the end of each contiguous eligible mixed domain. Before emitting a pass, terminal height fitting reserves a minimum-height final pass where possible or absorbs a shorter remainder into the current component without exceeding its printer limit. This may approximate the terminal color ratio by up to one minimum-height pass. An infeasible minimum/maximum height combination is reported as a slicing error instead of emitting an undersized pass. Regression coverage is in `tests/libslic3r/test_local_z_plan.cpp`.

`LocalZPlan` owns the interval index and pass storage together and validates layer ordering, pass ranges, and layer references before publication.

`LocalZMaskPreparation` owns fixed-paint precedence and boundary expansion for both purge planning and G-code emission. `LocalZSchedule` produces ordered tool/pass phases from geometry-independent descriptors; both consumers use that scheduler, including its height-order fallback.

The planner now lives in `LocalZ/LocalZPlanner.cpp`. Effective settings, row recipes/cadence state, and immutable geometry preparation have separate contracts. Base-mask geometry used only by JSON/SVG diagnostics is captured only when `ORCA_LOCAL_Z_DEBUG=1` and is not retained in the execution plan. The disabled dense-zone shared-plane fallback has been removed.

Production callers read `mixed_filament_legacy_rows()` and publish edits through explicit setters. ID lookup returns a const row. Mutable `mixed_filaments()` borrowing remains only as a compatibility interface, covered by compatibility tests; production code no longer activates that synchronization path. The disabled old sidebar editor has been removed.

## Product behavior retained

This port retains Snorca's multi-digit filament and manual-pattern encoding, filament-count limits, deletion remapping, and the product's calibrated color selection. Gradient previews retain the FilamentMixer endpoint correction and 3% default solid-color width. Direct multicolor, independent Local-Z heights, first-layer protection, painting controls, and the prime-tower return-to-object-filament fix remain part of the product baseline.

Legacy compatibility is tested through repeated string loading/saving and the production Snorca 3MF exporter/importer. Static component IDs use the original compact encoding for IDs 1?9 and the existing slash-delimited encoding for higher IDs; manual patterns retain bracketed IDs. The tested 3MF round trip preserves stable IDs, component weights, gradient order/stops/widths, unavailable rows, object assignments, and painted triangles.

Simple-mode rows retain their A/B cadence even when an old project also contains inactive multi-component weights. Legacy rows in either distribution mode keep their independent A/B percentage when the weighted recipe is unchanged. An explicit typed weight edit clears a stale compatibility value, so subsequent edits cannot resurrect it. The exporter uses the existing percentage field; no new serialized token or 3MF schema is introduced.

The refactor does not introduce FS image mapping, perimeter modulation, component-bias controls, descriptive color names, or the FS vNext 3MF package format. Existing project persistence is retained.

## Validation

The core Release build and selected regression suite pass: 243 cases, 241 passed and the same two pre-existing expected failures (114,800 assertions). Coverage includes the imported FS typed-model cases plus product checks for gradient order and widths, multi-digit manual sequences, synchronization between typed and legacy manager APIs, configuration, color prediction, painting, and 3MF persistence.

A slicing harness reconstructed the geometry, painted triangles, and configuration from `CubeTest123.3mf`. At both 0.20 mm and 0.12 mm, the refactored build produced exactly the same layer plans and recorded extrusion moves as the committed product port:

| Nominal layer height | Layer-plan rows | Extrusion moves | Green-only moves above the painted regions |
| --- | ---: | ---: | ---: |
| 0.20 mm | 135 | 12,179 | 1,239 |
| 0.12 mm | 224 | 20,901 | 2,183 |

A separate production-loader check opened the existing `CubeTest123.3mf`, saved a copy, and reopened it. Both mixed recipes and all object/painted assignments were unchanged. The complete mixed-filament string inside the original and resaved archives was also identical. The original project was left untouched.

This verifies that the reported yellow-band fix and Local-Z planning survive the refactor. It is a software regression check, not a new physical print validation.

The final Release builds of the core, tests, GUI library, DLL, and launcher passed. The rebuilt executable started with `--help` and exited with code 0. No manual GUI walkthrough or new physical print validation was performed for this refactor.
