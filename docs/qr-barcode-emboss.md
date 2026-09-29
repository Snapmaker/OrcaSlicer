# QR code / barcode emboss

Right click an object → **Add part / Negative part / Modifier → QR code / Barcode...**
(or on empty bed → **Add Primitive → QR code / Barcode...** for a standalone tag).

Supported symbologies: QR code (UTF-8, error correction L/M/Q/H), Code 128, EAN-13, UPC-A, Code 39.

## Parts

A code is created as up to three embossed SVG volumes that share one transformation:

| Part  | Content                                            |
|-------|----------------------------------------------------|
| dark  | dark modules / bars                                 |
| light | quiet zone rectangle minus dark modules and logo    |
| logo  | optional SVG logo in the middle of a QR code        |

- **Multi-color:** same depth for dark and light, pick a different filament for each.
- **Relief (single filament):** light depth smaller than dark depth, or no light part at all
  (dark modules raised over the object surface). As a negative part the dark modules are engraved.
- **Surface projection:** "Project onto surface" wraps every part onto the curved object surface.
  Parts of one code are never projected onto each other.

## Surround shape

The light part can be a **Square**, **Rounded corners** (radius = quiet zone) or a **Circle** through the
corners of the quiet zone. For QR codes with a circle, **Circular QR pattern** fills the space between the
quiet zone and the circle with random modules (seeded by the content, so it is stable), which gives the
round QR look. Scanners ignore the pattern as long as the quiet zone keeps it apart from the code;
at least 2 modules are recommended.

## Editing

Selecting any part opens the SVG gizmo. Moving, rotating, scaling, mirroring and toggling
"Use surface" is applied to all parts of the code once the mouse is released; depth, filament and
operation stay per part. **Edit code...** regenerates all parts (content, size, logo, light part).

## Logo

The logo SVG's filled shapes are merged into one silhouette. Modules touched by the logo
(plus margin, following its outline / a square / a circle) are removed and restored by QR error
correction, so High error correction is selected with a logo. The logo is kept away from the finder
patterns, and a warning is shown when it covers more modules than the error correction can safely
restore.

## Storage

Parameters are stored in `<metadata id="edgeslicer-code">` of each generated SVG, which is saved
into the 3MF (`3D/code_<group>_<role>.svg`), so codes stay editable after reopening a project.
Geometry lives in `src/libslic3r/Barcode.*` (encoders) and `src/libslic3r/CodeEmboss.*` (parts),
tests in `tests/libslic3r/test_code_emboss.cpp`.
