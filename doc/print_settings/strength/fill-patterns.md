# Fill Patterns

This page covers the pattern options for the solid surfaces of a print. For infill patterns (sparse interior fill), see [Infill Patterns](../strength/strength_settings_patterns.md).

## Infill of the top surface and bottom surface

The pattern used for the solid top, bottom and just-under-top surfaces of the object.

- **Top surface pattern** (`top_surface_pattern`): the pattern of the very top skin. Monotonic patterns travel in one direction for a cleaner look.
- **Bottom surface pattern** (`bottom_surface_pattern`): the pattern of the first solid layers sitting on the bed (or on support/raft).
- **Undertop surface pattern** (`undertop_surface_pattern`): the pattern of the solid layer directly underneath the top skin. It only applies when the top surface density is below 100% and there are more than 2 top shell layers, because only then does the layer below show through the top skin. **Solid default** keeps the internal solid infill pattern.

Typical choices:

- **Rectilinear**: the default; fast and strong, slight diagonal ridges may show on the top.
- **Monotonic / Monotonic line**: all lines run in a consistent order and direction, hiding the start/stop points and giving the top surface a uniform sheen — the usual pick for visible top surfaces.
- **Concentric**: follows the shape of the outline; nice for round tops, but can leave gaps where rings don't meet.
- **Hilbert curve, Archimedean chords, Octagram spiral**: decorative fills that avoid long straight travel moves; slower but distinctive.

Pick monotonic variants when surface finish matters, and rectilinear when strength or speed matters more.

## Top and bottom surface density

- **Top surface density** (`top_surface_density`): how densely the top skin is filled. 100% gives a fully solid, smooth top. Lower values space the top surface lines apart, giving a textured top in the chosen top surface pattern. 0% prints only the walls on the top layer.
- **Bottom surface density** (`bottom_surface_density`): the same for the bottom skin, from 10% to 100%. Lowering it can hurt bed adhesion.

These settings are for looks or function, such as a textured top, a see-through lattice or a grippy bottom. They are not a fix for over-extrusion; use the flow ratio for that.

With a sparse top skin, the solid layer below shows through. Set **Undertop surface pattern** to control how it looks, for example Concentric under a Rectilinear top.
