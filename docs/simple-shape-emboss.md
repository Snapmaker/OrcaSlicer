# Simple shape emboss

Right click an object → **Add part / Negative part / Modifier → Shape...**
(or on empty bed → **Add Primitive → Shape...**) embosses a simple shape without drawing an SVG file:
circle, square, triangle, pentagon, hexagon, octagon, star (3–24 points, inner size) and ring (hole size).
Polygons and stars can have rounded corners; the width always matches the entered size.

The shape is an ordinary embossed SVG volume, so the SVG tool provides size (also without keeping
the ratio, e.g. an ellipse from a circle), depth, rotation, distance from surface and
**Use surface** projection. Typical uses:

- **Fill a hole:** add a circle (or matching polygon) as a part over the hole and set its depth.
- **Cut a shaped hole or recess:** add it as a negative part.
- **Change settings in a region:** add it as a modifier.

**Edit shape...** in the SVG tool changes the shape type and its parameters later. Parameters are stored
in `<metadata id="edgeslicer-shape">` of the generated SVG, which is saved into the 3MF
(`3D/shape_<type>_<id>.svg`). Geometry is in `src/libslic3r/SimpleShape.*`, tests in
`tests/libslic3r/test_simple_shape.cpp`.
