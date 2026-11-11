# FullSpectrum product port

Target branch: `product_proposal_merge`. Source: FullSpectrum 0.9.13. This is a selective port into Snorca's existing mixed-filament model and painting pipeline.

Source snapshot: `ba00f1d661`. Target baseline: `5970fea62d`.

## Included behavior

- Calibrated KM/K-S predictions for Full Spectrum CMYK materials, including recipe matching, recommendations, mixed-material badges, and gradient previews. The picker identifies the supported bundle automatically; the process checkbox is removed. The legacy checkbox cannot disable pack calibration.
- Stable per-filament material IDs and transmission distances, saved with project settings and realigned when filament slots change.
- Automatic subdivision for gradients and automatic full-domain handling for mixed materials assigned to an object or volume. Ordinary objects retain their normal layers.
- Independent gradient cycle height, minimum sublayer height, achievable-ratio clamping, and first-layer protection.
- Direct multicolor SML for static recipes with three or more components. Enabling SML also enables independent static Local-Z heights; the separate switch is hidden.
- Two-, three-, and four-filament gradients with ordered color stops, transition midpoints, middle-color windows, layered/blended editor previews, and Prepare previews for whole objects and painted regions.
- Surface-only painting, extra painted-zone walls, brush precision, rectangle painting, and polygon painting. Snorca's existing same-material region deduplication is retained.

The port does not add component bias, image mapping, perimeter modulation, the swatch generator, or a second infill-subdivision option.

## Defaults and material mapping

| Setting | Default |
| --- | --- |
| Calibrated KM/K-S colors | On |
| Gradient Local-Z cycle height | 0.20 mm |
| Minimum Local-Z sublayer height | 0.06 mm |
| Protect the first layer | Always on; separate switch hidden |
| Direct multicolor SML | On |
| Independent static Local-Z heights | On with SML; separate switch hidden |
| Interior solid-color width | 3% per color for new gradients; edited in the gradient selector |
| Surface-only painting / extra painted walls | Off / 0 |

| Bundle material | SKU | TD | Picker color |
| --- | --- | --- | --- |
| Cyan | 34267 | 5.5 mm | `#008BB3` |
| Magenta | 34268 | 5.5 mm | `#AD4A76` |
| Yellow | 34269 | 9.5 mm | `#EBBE00` |
| Gray | 34265 | 6.5 mm | `#7B7F80` |

These stable SKU aliases select the corresponding measured FS cyan, magenta, yellow, and gray calibration records with the supplied TD values. The source calibration coefficients are retained; they have not been refitted from a new shipping-bundle measurement dataset. White and unsupported materials do not receive those aliases. Picker colors use `primary_colors` from the existing Panchroma CMYG measurement manifest in `D:/SwatchDatas`. Older downloaded catalogs receive the same correction; the previous swatches remain recognized when loading the matching Full Spectrum preset.

## Validation

Build the Windows launcher as well as the application DLL:

```powershell
cmake --build build-dev-release --config Release --target libslic3r_tests Snapmaker_Orca_app_gui --parallel 4
```

Run the port tests and existing relevant regressions:

```powershell
& build-dev-release/tests/libslic3r/Release/libslic3r_tests.exe '[ProductPort],[FullSpectrumKS]~[.],[MixedFilament]~[.],[mixed_filament]~[.],[TriangleSelector]~[.],[Config]~[.]' --reporter compact
```

The port tests cover calibration identity through slot reordering, stop serialization, gradient endpoints/midpoints, minimum-height clamping, projected painting selections, and slicing/G-code export. Slicing fixtures exercise automatic gradients, painted surface-only gradients, default direct static SML, and independent static heights alongside an ordinary control object.

The final settings regression run covers 221 cases: 219 pass normally and two existing cases fail as expected (72,684 assertions, including the two expected failures). Export checks verify that all four physical tools extrude and that movement coordinates and extrusion heights are finite. The application starts successfully with an isolated data directory. The gradient selector was visually checked in an isolated native window using the actual product widget. A full application walkthrough of the remaining editor and painting interactions is still needed; the first-run embedded browser blocked that earlier automation.

Physical print validation on the shipping bundle remains necessary, especially for independent static heights, changes of gradient component, and surface-only painted boundaries. The software checks do not establish adhesion, finish, or measured color accuracy on a printer.

The gradient selector reuses the FS theme helpers, bar outline and center line, contrasting circular color stops, white midpoint ticks, and drag-limit markers. Position entry uses the FS text-field style (relative between adjacent colors for midpoints), with the FS minimum-stop-gap slider and percentage field. Preview labels support dark mode and the two-panel preview retains its width after a DPI change. The Windows application rebuild passes. Gradient selector interaction checks are recorded below.

## Settings simplification

Additional walls in painted zones are visible and applied only when surface-only painting is enabled. Their stored count is retained when disabled. Calibrated colors, independent heights, and the global middle-window setting are removed from the process panel.

Each interior gradient color has two square width handles below the gradient bar and a numeric solid-color width field when selected. Drag either edge to resize the centered solid span, bounded by the adjacent transition midpoints. Widths are independent per color and saved in the mixed-material recipe (`v` token), used by both preview and slicing. Older recipes without widths inherit their saved process window; new gradients start at 3%. The old global value remains readable for compatibility.

The final Windows application and test builds pass. All 221 regression cases have the expected outcome: 219 pass and two existing cases fail as expected. Checks include hidden extra walls being inactive without surface-only painting, automatic SML independent heights despite an old disabled flag, and the sliced solid-color span honoring recipe widths over the legacy global value. The actual gradient widget was visually checked in an isolated native window: width dragging, precise numeric entry, independent widths, and clamping at adjacent transitions all behaved correctly.


## Gradient preview correction

The gradient slider, blended editor preview, and Prepare gradient colors follow the requested mixing ratio across the full transition. Minimum printable pass heights no longer clamp these design previews into broad constant-color bands. Slicing and the layered preview still enforce physical minimum heights; the blended design preview is not a guarantee of an achievable printed ratio at every point.

Automatic gradient and color-matching previews use calibrated KM/K-S when all active components match measured materials, including the supported Snapmaker pack. Unsupported color combinations use FilamentMixer so generic RGB selections retain their transitions. Explicit low-level KM/K-S prediction remains available unchanged.

The Local-Z minimum sublayer-height field is visible only while Subdivide mix layer is enabled; hiding it retains the stored value.

The reported green-white-yellow case was visually verified with the actual native gradient widget and blended rendering helper. Regression coverage checks distinct transition colors, pure endpoints, the solid middle span, independence from preview cycle height, retained calibrated-pack predictions, and unchanged physical minimum-height constraints. The Windows launcher and DLL build succeeded; the final regression run has 219 passing cases and two existing expected failures (72,684 assertions).

## FilamentMixer endpoint correction

The shared FilamentMixer wrapper now anchors its polynomial predictions to the actual endpoint colors before gamut clipping. This removes the pink fringe at the green-white stop and equivalent endpoint jumps for other color pairs, while retaining pigment-style curvature. Identical colors remain unchanged when mixed. The measured KM/K-S engine is unchanged.

The native gradient widget was visually checked. The application and test builds pass; the expanded regression run has 226 cases (224 pass, two existing expected failures), with 112,983 assertions. See [the investigation](FilamentMixer-Endpoint-Correction.md) for the general color sweep, examples, correction formula, and remaining interior hue errors.

## First-layer protection and setting order

Subdivide all mix layers is the first setting in the Color Mixing group. Keep first layer unsplit is hidden, and print configuration normalization forces first-layer protection on even when an older project saved it as false. The first object layer retains its configured initial layer height. The legacy key remains readable for compatibility.

Full domain for all mixes is hidden from the settings panel. The legacy configuration key remains readable for existing projects. An eligible mixed wall assigned to an object or volume already activates full-domain planning automatically, including when painted overrides are present. In the current painted-only path, physical single-color paint is excluded from the Local-Z planner. The remaining override can synchronize a newly appearing mixed region's layer-sequence position with the dominant mixed region in the older pair-cadence path. Independent gradient/direct-multicolor paths maintain their own cadence state. The option is therefore largely redundant for the new default workflows. Hiding it does not change the automatic full-domain behavior or the interpretation of saved legacy values.

Validation: the Windows launcher/DLL and tests build successfully. The settings regression run has 226 cases (224 pass, two existing expected failures), with 114,298 assertions. Gradient, painted-gradient and static-SML slicing fixtures supply the legacy protection flag as false and verify that the effective setting is true and first-layer passes remain unsplit at the initial layer height.

## Subdivision setting grouping and precision

The Color Mixing panel now lists Subdivide all mix layers, Apply subdivision to infill, and Local-Z minimum sublayer height before Surface paint only and Extra perimeters in painted zones. Conditional visibility is retained.

The mixed-filament sidebar preserves the double precision used by ConfigOptionFloat when reading and writing process/project settings. Previously a refresh narrowed 0.06 to a float and wrote approximately 0.05999999865889549 back into the preset. Both values display as 0.06, but exact preset comparison marked the refreshed value as modified, so resetting immediately recreated the reset icon. Keeping the configuration round trip in double precision prevents this false modification without suppressing reset controls for real edits. The same fix covers other float-valued mixed-filament settings synchronized by this sidebar.

## Restoring direct multicolor when SML is enabled

The former SML-off handler wrote false to the project's hidden direct-multicolor flag. Enabling SML again restored infill subdivision but left that flag disabled, allowing the older solver to remain active despite the visible SML checkbox.

Enabling SML now enables direct multicolor in both process and project settings. Disabling SML no longer clears the hidden solver preference. Print configuration normalization also enables direct multicolor whenever SML is on, repairing previously saved false values; GUI preview contexts follow the same rule. Explicit A/B height overrides retain their existing precedence.

A regression paints a single object with both a 35/65 two-filament mix and a 13/18/69 three-filament mix, using a 0.12 mm nominal layer height and 0.06 mm minimum. It starts with SML off and both hidden direct/independent flags false, then enables SML and checks that both regions receive independent passes. The pair uses 0.060 / 0.11143 mm. With a 0.30 mm printer maximum, the three-component cycle uses 0.060 / 0.08308 / 0.15923 / 0.15923 mm; the two final passes belong to the 69% component.

The regression run passes: 227 cases, 225 passed and two existing expected failures, with 114,428 assertions. This verifies the reproduced configuration state and generated slicing plan; the user's screenshot alone does not expose the saved hidden flag or prove that it was the only issue in that project.

## Wrong filament above a painted Local-Z region

The saved CubeTest123 project reproduced a yellow band above the painted regions at a 0.20 mm nominal layer height. The slicing masks ended correctly at Z=22.85 mm, but the prime-tower integration skipped the switch back to the object's green filament once its planned tower layers ran out. With the tower disabled, the same object paths already used green. Local-Z tool changes can leave a different filament active than the nominal tower plan assumes.

Tower integration now honors an actual required tool change even above the last planned tower layer or when skipping a sparse tower layer. It uses the normal extruder-switch routine when no tower transition is available; requesting the already active filament remains a no-op.

The project mesh, original triangle paint annotations and saved settings were reproduced in a headless slicing harness, with the original 3MF left untouched. With the tower enabled, all 1,239 model extrusion moves above the painted region now use green at 0.20 mm; previously 1,139 used yellow. The 0.12 mm comparison also uses green for all 2,183 model extrusion moves above its painted region. These are generated-G-code checks, not physical print validation.

A focused regression checks that exhausted and skipped sparse tower layers still request a needed tool change, while an already active filament requires no tower output. The Windows DLL, launcher and tests build successfully. The regression run has 228 cases: 226 passed and two existing expected failures, with 114,436 assertions.
