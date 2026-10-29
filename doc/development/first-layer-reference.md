# First-layer references in sliced 3MF

Every export using `SaveStrategy::WithGcode` generates a reference directly from
**each final G-code file** before placing that file in the archive. Both GUI and
CLI exports use the same code. Ordinary project saves are unaffected. Existing
plate, top-view and picking thumbnails keep their existing meanings.

For plate N (one-based, matching `Metadata/plate_N.gcode`):

- `Metadata/first_layer_N.png`: orthographic, white extrusion footprint on opaque
  black, antialiased edges, no travel, retraction, custom/startup calibration,
  lighting, bed decoration, or GUI overlays. It includes tagged first-layer
  model, support, raft, brim, skirt and wipe-tower extrusions. Holes stay empty.
- `Metadata/first_layer_N.json`: schema version 1, readiness status, plate ID,
  image path/dimensions, G-code path and MD5, included roles, layer tag, Z range,
  printable-bed polygon, and a 3x3 `pixel_to_bed_mm` matrix.
- `Metadata/model_settings.config` plate metadata links both resources.

The image spans the bounding rectangle of the printable bed polygon and is
clipped to that polygon. The long edge is 2048 pixels; the short edge preserves
bed aspect ratio. The rendering API accepts 16 through 4096 for callers needing
a different resolution. PNG rows run top to bottom. The matrix maps integer
pixel-center coordinates `[column, row, 1]` to printer-bed millimeters; Y therefore
increases upward in bed coordinates and downward in the image. Coordinates use
the printer's G-code coordinate system, without GUI plate-layout offsets.

Layer selection uses the smallest positive slicer layer tag with a recognized,
positive-width extrusion, rather than a fixed preview layer index or Z position.
`MoveVertex::layer_id` preserves that tag through time-estimation finalization.
Both Orca comment dialects are accepted without changing the currently selected
printer or global tag mode.
Arcs use the existing G-code processor interpolation and paths use their actual
extrusion widths. A raft's first printed layer is the reference, rather than the
first layer of the object above it.

A JSON status other than `ready` has **no PNG**. Sequential/by-object printing is
reported as `unsupported_sequential_print`: its objects reach their first-layer
completion at different times, so a simultaneous plate-wide camera reference
would be misleading. Missing layer tags/no usable extrusions and invalid beds
also do not produce a fake reference. Unrecognized firmware macros and motion
commands have the same interpretation limits as the existing G-code viewer.

References are regenerated on every sliced export, including exports of imported
sliced files. This covers archives created before this feature and prevents stale
images after slicing or external G-code post-processing. Import exposes the
resource paths in `PlateData`; existing PNG/JSON extraction handles their bytes.
The G-code MD5 ties the reference to the archived toolpath. Unexpected parsing or
archive-write errors fail the export instead of reporting success with stale data.

This feature supplies the reference input for a later computer-vision pipeline.
Camera calibration/registration, image comparison and printer pause commands are
separate responsibilities.

## Validation

Run `libslic3r_tests "[first_layer_reference]"`. The tests cover role/layer
selection, support and brim, holes, arc interpolation, non-rectangular beds,
pixel/bed transforms and PNG orientation, unsupported/empty inputs, preservation
of layer IDs after parsing either tag dialect, sparse plate numbers, final-file
checksums, full import/re-export of resources, ordinary project-save isolation,
and image regeneration after G-code changes. Before enabling an on-printer verifier, also
validate alignment using a calibrated camera on each printer model.
