# Mixed-filament and Local-Z requirements

The development has seven requirements covering mixed-filament editing, project compatibility, Local-Z slicing, CPU performance, and previews. See [Mixed-Filament-Refactor.md](Mixed-Filament-Refactor.md) for architecture and implementation details.

## 1. Keep one coherent mixed-filament model

The manager must own canonical typed definitions and publish edits through explicit operations.

- Preserve pair recipes, weighted multi-component blends, grouped manual patterns, gradients, direct multicolor, and independent Local-Z heights, including multi-digit physical-filament references.
- Keep stable mix identity when physical or virtual filament numbering changes, and remap object, part, feature, layer-range, and painted-facet assignments accordingly.
- Reading definitions must not change them. Editing a snapshot takes effect only when published; legacy-row access remains a compatibility boundary.

## 2. Preserve existing 3MF projects

Saving and reopening a project must retain its recipe meaning and filament assignments. See [Mixed-Filament-3MF.md](Mixed-Filament-3MF.md) for the storage contract.

- Store authoritative recipes in `Metadata/mixed_filaments.json` and also derive compact legacy rows in `mixed_filament_definitions` inside `Metadata/project_settings.config` for older versions. Load legacy rows only when the JSON is absent; reject invalid or unsupported JSON rather than silently falling back.
- Preserve stable IDs, component references and weights, manual patterns, gradient order/stops/widths, UI mode, enabled state, deletion tombstones, and object/paint assignments. Normalization may change formatting but must stabilize and must not silently re-enable unavailable recipes.
- Preserve legacy A/B percentages independently of inactive weighted-recipe data. Explicit weight edits must clear stale compatibility percentages.

## 3. Build valid Local-Z plans from current geometry

Each object must own a validated Local-Z plan derived from its final printable geometry and current settings.

- Plan after XY compensation, slice repair, and polyhole conversion; clip painted masks to that final domain. Relevant assignment, recipe, and setting edits must rebuild the plan so reused prints behave like fresh slices.
- Own interval indexing and pass storage together. Validate layer ordering and pass ranges before publication, and use actual layer IDs, including raft offsets.
- Cover the eligible domain within the applicable height bounds, without undersized terminal passes. Reject infeasible bounds, retain the existing first-layer policy, and limit terminal ratio approximation to one minimum-height pass.

## 4. Keep Local-Z execution and purging consistent

Purge planning and G-code emission must share mask preparation and deterministic scheduling.

- Preserve fixed-paint precedence, fixed-path boundaries, and the intended overlap between base paths and mixed passes without mutating source masks.
- Use the same tool/pass order and phase heights, respecting height order and predecessor dependencies while ignoring passes with no tools.
- Preserve Local-Z ownership even for independent intervals with zero or one pass. Ordinary object/support flushing must not replace managed extrusion, and skipping a prime-tower action must not suppress a required tool change.

## 5. Keep slicing maintainable and resource use bounded

Give recipe resolution, planning, geometry preparation, scheduling, and preview calculations clear owners, with shared behavior implemented once.

- Parallelize independent geometry preparation using immutable inputs while keeping cadence and tool-order decisions sequential.
- Bound geometry batches and cache admission, retain a progress path for oversized layers, and make borrowed-data lifetimes explicit. These limits do not imply a hard cap on total slicing memory.
- Preserve clipping geometry and path order through shortcuts, caching, and buffer reuse. Keep diagnostic-only geometry and expensive developer checks opt-in, and remove obsolete implementations when replaced.

## 6. Preserve consistent mixed-filament previews

The GUI and color-context builder must share core preview settings and recipe-height calculations.

- Resolve mixed-preview options from project settings, then print settings, then defaults. Keep nominal layer height and wall count sourced from print settings, and honor explicit component-height preferences.
- Preserve existing height calculations, calibrated color behavior, gradient traversal order, stops, independent solid widths, the 3% default width, and pure endpoints.
- Treat the design preview as a recipe visualization; independent Local-Z deposition can span nominal layers.

## 7. Show the correct physical filaments on each prime tower

Prepare must color each tower using the physical filaments relevant to that tower's own plate.

- Before slicing, derive colors from that plate's configured assignments, expanding mixed recipes into physical components. Virtual mixed-filament IDs must not create extra bands; missing color data must have a safe fallback.
- After slicing, use actual tower consumption and omit configured but unused filaments.
- Preserve tower taper, brim, placement, rotation, and selection. Color bands indicate filament membership; Preview shows the actual deposition order and toolpaths.
