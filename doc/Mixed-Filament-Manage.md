# Manage mixed filaments

The Color Mixing header now has a **Manage** button. The dialog ports the actual
accordion, swatch, checkbox and collapsible subsection widgets from FullSpectrum
`main` at `cc152a84dde21edde575b6cd2266168acc5f401c`. It preserves the original
500 × 600 DIP dialog, card spacing, hover behavior and footer layout, adapted to
this branch and the mixed-color recommendation PRD 2.0 (2026-11-02).
The existing Color Mixing Match tool remains a separate dialog.

## Source attribution

The FullSpectrum UI architecture and controls were created by **Hanno Witzleb
(Xipit)**, using the author identity `Hanno Witzleb <hannowitzleb@gmail.com>`.
This is a selective port; the original commits are retained in the FullSpectrum
repository, not in this branch's ancestry. Relevant source commits are:

- `523fd40c637e8f315043124fb96ca24c8ec1efbc`: accordion and separate content panels.
- `09bbcb2daefcafcf3f223dee1a2256b3af0b205a`: Manage dialog and batch recommendations.
- `36b8e2fdff3cf6a76b84180aff42dac86d57717a`: consistent numbering in Manage.
- `2a2dca5cd68b5cc13563e70c130f51225b83e440`: swatch rendering and Manage UI refinements.

The port preserves his accordion/subsection organization, drawing style, spacing,
checkboxes and footer. Recipe generation, staged persistence, compatibility and
prediction use this branch's model. Large palettes share drawing surfaces within
those panels instead of allocating a native child window for every swatch.

## Agreed changes to the PRD

- Manage accepts **2–16 physical filaments**. The physical count controls entry,
  independently of how many mixed recipes are already saved.
- A project may contain **256 mixed recipes**, including disabled recipes but
  excluding deletion tombstones. Removing recipes in the same confirmation
  frees capacity for additions.
- Predicted colors use the calibrated KM/K-S predictor when every participating
  material is identified by the calibration data. Other recipes use FilamentMixer.
  Manage does not change the application's color-engine preference. The PRD's
  RGB-average appendix is superseded by this choice.

## Behavior

The saved section shows existing recipes in their stored order. Swatches keep
their opening filament numbers throughout the dialog. Gradients retain their
gradient swatches and direction tooltips; ordinary recipes show component slots
and percentages. Clicking a saved swatch toggles a pending removal.
Saved weighted recipes and recommendations use the same requested-ratio color
prediction, so registration does not change the color shown inside Manage.

Physical swatches are display-only. Recommendation generation first deduplicates
physical slots by normalized HEX **and material type**, retaining the lowest
original slot number. It checks each remaining combination against the existing
material compatibility matrix before computing a color. Duplicate physical slots
and existing saved recipes are never modified by this filtering.

The two collapsible recommendation sections contain:

| Section | Recipes per physical combination |
| --- | --- |
| Two-color Mixing | 50/50, 33/67, 67/33 |
| Three-color Mixing | 33/33/34 and all three placements of 50/25/25 |

Each section sorts by HSL family: red, orange, yellow, green, cyan, blue, purple,
magenta/pink, then achromatic colors. The PRD hue boundaries are 15, 45, 75, 165,
195, 255, 285 and 345 degrees; saturation below 10% is achromatic. Within a family,
sort by lightness descending, hue ascending, saturation descending, then HEX
ascending. Equal color keys retain deterministic recipe order. Selection does
not reorder the lists. Different recipes with identical predicted colors remain
distinct; the PRD's deduplication rule applies to physical inputs.

Already registered recommendations display their existing number and can only
be removed in the saved section. They do not contribute to Select All, partial
selection or pending-addition counts. A pending removal is reflected in both
sections. Cancel, Escape and the window close button discard all pending edits.

Confirm validates the complete staged result before publishing it. Exceeding
256 recipes leaves the dialog open with its selections intact. Preparation or
serialization failure also leaves the dialog open for retry. Successful edits
remap object, part, layer-range and feature assignments as well as painted facets,
then refresh the sidebar and dirty state. Deletion of an edited generated pair
also remembers its base-pair tombstone so regeneration cannot resurrect it.

The maximum distinct compatible palette generates **2,600** recommendations:
360 two-color and 2,240 three-color recipes. Each section uses FullSpectrum's
28 DIP swatch drawing and wrapping spacing. A single surface per group paints
only damaged visible cells, using a buffer bounded by the visible viewport.
Selection updates invalidate affected cells without laying out the whole dialog;
off-screen marks are painted when scrolled into view. The outer cards also avoid
buffers spanning the entire scrollable content. Arrow keys navigate swatches;
Space/Enter toggles the focused editable swatch or checkbox.

The PRD adaptations to the original interface are the two recommendation groups,
read-only physical swatches, a global Select All checkbox, and the Confirm label.
The original auto-generation setting is omitted because edits are staged until
Confirm. Pending-removal slashes sit in a corner so the opening filament numbers
remain readable.

## Persistence and ID range

The existing typed manager, canonical `Metadata/mixed_filaments.json` and legacy
recipe string remain the persistence path. No new archive part or schema version
is needed for Manage.

Physical tool slots remain capped at 64 elsewhere in the application. The virtual
filament-ID range is now 320 (64 physical + 256 mixed), and painting's existing
extension-nibble encoding carries those IDs without byte truncation. OBJ color
matching IDs were widened too. Manage itself permits only 16 physical slots, so
its highest assigned ID is 272.

Older builds can read the legacy recipe fields, but their own filament limits
still apply. In particular, old readers with a 255 painted-state ceiling cannot
preserve painting that uses IDs above 255. Keeping the legacy string does not
remove that older-reader limitation.

## Integration and checks

- `MixedFilament/Recommendations` contains the UI-independent filtering,
  generation, ordering, selection and staged manager preparation.
- `MixedFilamentManageDialog` owns rendering and interaction. It uses the existing
  display context for calibration identity and compatibility information.
- `Widgets/Accordion` retains FullSpectrum's card implementation;
  `MixedFilamentManageWidgets` ports its swatch painting, checkbox and subsection
  controls, connected to the staged model through callbacks.
- The sidebar opens Manage; the normal filament-change path handles the explicit
  mixed-edit remap without regenerating or reordering recipes during that refresh.
- Interaction events use the existing `NetworkAgent::track_event` API. That API
  is currently disabled in this branch; this feature does not enable telemetry.
- Regression coverage includes the exact ratios, duplicate inputs, incompatible
  combinations, sorting, cancellation semantics, select-all semantics, calibration
  fallback, the 2,600-candidate case, capacity after removals, auto-pair deletion,
  IDs above 255 in painting/OBJ import, and a real 3MF round trip with 256 recipes.

Windows checks cover opening Manage, partial/select-all state, cancelling staged
edits, registering a recipe, recognizing it on reopening, and removing a recipe
with the remaining IDs updated. A 16-filament fixture also exercised all 2,600
recommendations, scrolling, collapse, select-all, and the capacity error retaining
the complete pending selection. The focused regression run passed 310 cases,
with one skip and two existing expected failures (343,873 assertions).
The final focused Manage/JSON/painting run passed all 20 cases (7,543 assertions).
The large-palette rendering fix was checked with 200 saved recipes and 2,600
recommendations. Temporary in-process profiling on the Windows development
machine measured a 0.86 ms median swatch paint for the 200-recipe fixture and
1.05 ms for the maximum recommendation fixture; the largest sampled paint was
7.7 ms. The 2,600-item Select All state update took 5.4 ms. These are sampled
handler timings, not whole-frame or cross-hardware guarantees. Profiling code is
not included in the feature. Scrolling, off-screen selection, collapse, keyboard
navigation to the final swatch, and capacity-error recovery were also checked.
The PRD's Mac and low-end hardware performance checks require those environments;
they are not implied by the candidate-generation tests.
