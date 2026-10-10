# Speeds and line widths per extruder: the quality rule and the "Extruder preset" picker

A Snapmaker process preset is made for one nozzle size (`0.20mm Standard @Snapmaker U1 (0.4 nozzle)`,
`0.10mm High Quality @Snapmaker U1 (0.2 nozzle)`). On a plate whose extruders carry different nozzle
sizes, Snapmaker Orca gives every extruder of another size than the printer preset the speeds,
accelerations, jerk and line widths of a process preset made for its own size, and lets the user
choose that preset per extruder. Walls, infill and every other setting stay those of the selected
process preset. The engine lives in `src/libslic3r/PerHeadProcess.hpp`; the pages are the Speed page
(speeds, accelerations, jerk) and the Quality page (the nine line widths) of the Process tab.

## The rule in one paragraph

The process preset selected in the sidebar is the plate's **quality choice**. For an extruder whose
nozzle size differs from the printer preset, the rule takes the system presets made for the extruder's
size and picks, within the plate's quality class (the text of the preset name between the layer
height and the printer: `Standard`, `High Quality`, `Strength`, `Draft`, ...; a user preset takes its
system parent's class), the one whose layer height is nearest to the extruder's preferred layer height
(sidebar, nozzle tab) or, without one, to the layer height of the selected preset as saved; a tie
goes to the thinner preset. When the extruder's size has no preset of that class, the rule walks a
ladder: `High Quality` and `Color Mixing` ask for `High Quality`, then `Standard`; every other class
asks for itself, then `Standard`. Without any class match the machine preset's default process
preset is taken, then the first compatible preset by name. The 32 speed, acceleration and jerk keys
come from that preset; a value changed in the selected preset under "All extruders" is kept on
every extruder, and a value set for one extruder on the Speed or Quality page beats everything.

## The U1 matrix

Rows: the selected preset (its size in parentheses is the printer preset's). Columns: what an
extruder of that size prints with. `-`: the home size, the selected preset itself.

| selected | 0.2 extruder | 0.4 extruder | 0.6 extruder | 0.8 extruder |
|---|---|---|---|---|
| 0.08mm High Quality (0.2) | - | 0.20 HQ | 0.18 Std | 0.24 Std |
| 0.10mm High Quality (0.2) | - | 0.20 HQ | 0.18 Std | 0.24 Std |
| 0.12mm Standard (0.2) | - | 0.12 Std | 0.18 Std | 0.24 Std |
| 0.08mm Standard (0.4) | 0.12 Std | - | 0.18 Std | 0.24 Std |
| 0.10mm Color Mixing (0.4) | 0.10 HQ | - | 0.18 Std | 0.24 Std |
| 0.12mm Standard (0.4) | 0.12 Std | - | 0.18 Std | 0.24 Std |
| 0.16mm Standard (0.4) | 0.12 Std | - | 0.18 Std | 0.24 Std |
| 0.20mm High Quality (0.4) | 0.10 HQ | - | 0.18 Std | 0.24 Std |
| 0.20mm Standard (0.4) | 0.12 Std | - | 0.18 Std | 0.24 Std |
| 0.24mm Standard (0.4) | 0.12 Std | - | 0.24 Std | 0.24 Std |
| 0.28mm Standard (0.4) | 0.12 Std | - | 0.30 Std | 0.24 Std |
| 0.18mm Standard (0.6) | 0.12 Std | 0.16 Std | - | 0.24 Std |
| 0.24mm Standard (0.6) | 0.12 Std | 0.24 Std | - | 0.24 Std |
| 0.30mm Standard (0.6) | 0.12 Std | 0.28 Std | - | 0.32 Std |
| 0.24mm Standard (0.8) | 0.12 Std | 0.24 Std | 0.24 Std | - |
| 0.32mm Standard (0.8) | 0.12 Std | 0.28 Std | 0.30 Std | - |
| 0.40mm Standard (0.8) | 0.12 Std | 0.28 Std | 0.30 Std | - |
| 0.40mm Strength (0.8) | 0.12 Std | 0.28 Std | 0.30 Std | - |

A preferred layer height on an extruder replaces the plate's layer height for that extruder inside the
class; it never changes the class.

## The J1 matrix (a 0.4 plate, one extruder changed to another size)

| selected | 0.2 extruder | 0.6 extruder | 0.8 extruder |
|---|---|---|---|
| 0.08 Extra Fine | 0.06 Std | 0.18 Std | 0.24 Std |
| 0.12 Fine | 0.10 Std | 0.18 Std | 0.24 Std |
| 0.16 Optimal | 0.14 Std | 0.18 Std | 0.24 Std |
| 0.20 Standard | 0.14 Std | 0.18 Std | 0.24 Std |
| 0.20 Strength | 0.14 Std | 0.30 Strength | 0.24 Std |
| 0.24 Draft | 0.14 Std | 0.42 Draft | 0.48 Draft |
| 0.25 Benchy | 0.14 Std | 0.24 Std | 0.24 Std |
| 0.28 Extra Draft | 0.14 Std | 0.30 Std | 0.24 Std |

## High Flow

An extruder with a High Flow nozzle reads the High Flow column of its preset. When neither the
selected preset (for an extruder of the home size) nor the preset of the extruder's size has one, the rule
takes the preset of the extruder's size that has a High Flow column (the 0.4 mm High Flow extruder under
`0.20mm High Quality` prints the High Flow column of `0.20mm Standard`). A preset chosen by the user
that has no High Flow column prints its own Standard values.

## The flow toggle

With a High Flow extruder selected in the row above the settings, the toggle beside the row reads
**High Flow | Standard**, High Flow selected: the extruder prints the High Flow speeds of its source (the
rule above). Standard makes that extruder print the Standard speeds column instead - of the plate's preset
for an extruder of the home size, of the preset of its size or of the preset chosen for it otherwise - while
its nozzle and its filament settings stay High Flow. The choice is the project key
`extruder_process_flow` (one entry per extruder, `Standard`, empty for an extruder that prints its
nozzle's own column, `[]` while no extruder has an entry); it follows the table of the picker's choice
below: New Project clears it, Open and Import take the file's value, a nozzle that turns Standard
leaves the entry dormant and a nozzle that turns High Flow again applies it, the toggle back to High
Flow clears the extruder's entry. A value set for the extruder on the Speed page still wins; a chosen preset
serves its Standard column. Under a Standard extruder the toggle is hidden, and so is it on the
Quality page, whose line widths do not depend on the flow; under All extruders the same toggle picks
the shared column the fields edit and shows only when the preset has two. Notice
N4 (a High Flow extruder without High Flow speeds) is silent for an extruder that chose the Standard speeds.
Older readers drop the key; the command line logs it and keeps it.

## Line widths

The nine line widths of the Quality page (`line_width`, `initial_layer_line_width`, the outer wall,
inner wall, top surface, sparse infill and internal solid infill widths, `support_line_width`,
`bridge_line_width`) are columns per extruder like the speeds (`print_options_with_variant`,
`ConfigOptionFloatsOrPercentsNullable`); the two Locked Zag widths stay one value. Every reader of a
width takes the column of the extruder whose nozzle resolves it (`Print::width_slot`: the nozzle
index of the filament), so a percent width is a percent of the nozzle that prints it and a value
set for an extruder reaches that extruder alone.

**Where an extruder's widths come from**: the same preset as its speeds, with one exception. An extruder
of another size prints the widths of the preset the rule or the picker gave it; an extruder of the home
size prints the selected preset's. The High Flow rule, which may re-pick the *speeds* of a High Flow
extruder to the sibling preset that has a High Flow column, never moves a width: a High Flow nozzle has
the bore of its size (`Source::size_preset`, `width_source`). The flow toggle and the chosen flow
never select a width column either; a width is flow-independent (a value set for an extruder fills both
of its flow columns, an edit under All extruders fills every shared column, differing flow columns
of a file are equalised on load, the Standard column winning).

The owner plate (0.2 / 0.4 High Flow / 0.6 / 0.8 mm under `0.20mm High Quality @Snapmaker U1 (0.4
nozzle)`, `bridge_line_width` 0 under All):

| extruder | speeds source | width source | line / outer / top / internal solid / support | inner / sparse | first layer |
|---|---|---|---|---|---|
| 1, 0.2 | 0.10mm High Quality (0.2) | the same | 110 % = 0.22 mm | 0.22 | 125 % = 0.25 |
| 2, 0.4 High Flow | High Flow column of 0.20mm Standard | the selected preset | 105 % = 0.42 | 112.5 % = 0.45 | 125 % = 0.50 |
| 3, 0.6 | 0.18mm Standard (0.6) | the same | 103.33 % = 0.62 | 0.62 | 0.62 |
| 4, 0.8 | 0.24mm Standard (0.8) | the same | 102.5 % = 0.82 | 0.82 | 0.82 |

Precedence is the speeds': an override on an object, part or layer range (uniform, every extruder) >
a value set for the extruder > a value changed under All extruders or saved in the selected user
preset (every extruder) > the chosen or automatic preset > the selected preset.

**A width added to an object, a part or a layer range.** Such an override has one value and applies
on every extruder that prints the item, so "Add settings" in the object list seeds it with the value
the item prints now, not with the selected preset's: the extruder-4 cube of the owner plate gets 102.5 %
for its sparse infill (0.82 mm, unchanged until edited), where the plate's 112.5 % would have printed
0.90 mm from an override nobody changed. The item's extruders are the one of its extruder setting and those of every
role filament set on it (a part or a range without an extruder takes its object's; a painted object
counts every filament); an item printed by several extruders gets the value under All extruders, and
a notice names the extruders that printed it with another width ("extruders 1 and 4 printed it with the
line widths of their own nozzle size"). A percent stays a percent and resolves against the printing
extruder's nozzle; every other setting added this way keeps the selected preset's value as before.

**Print::validate** checks each width at the extruder that prints it (the default width for every
extruder that prints, the support width at the support and the interface extruder, each role width at its
filament's extruder, the internal solid width at the bottom surface filament's extruder, the bridge width
at every bridge role's extruder and the bottom surface filament's); the message names the extruder. The
Locked Zag widths are checked for a Locked Zag region alone. An absolute width that reached an extruder
through All extruders and lies below the extruder's nozzle or above twice it raises a warning naming
the extruder. Line widths that differ between extruders are refused on a Bambu printer.

**Files and older readers.** While every column of a width is equal (no value set per extruder)
the key is written as one value, as before: a preset, a project, a 3MF override, a G-code header
(after narrowing: one value per extruder when they differ), the published INI payload and the
`SLIC3R_*` environment are byte-identical to files written before. A width set for an extruder is
written as the full array, shared columns first and never `nil`: Snapmaker Orca 2.4 and mainline
OrcaSlicer read the first number and take the percent flag from anywhere in the array, so they read
the shared value while every column has the same unit. An absolute shared value with a percent extruder
value (`0.42,0.42,110%`) reads there as `0.42 %` and is refused as too small; the Quality page warns
when an extruder's unit differs from the All value. A Snapmaker Orca 2.5 build from before this change (the
extruder selector without line widths per extruder) opens such a preset with the value under All
extruders, drops the width from the record of the values set per extruder (a log line names the key), and
narrows the preset to one value when no speed is set per extruder either; saved there, the widths set
per extruder are gone. A preset that also has a speed set per extruder keeps its layout, the widths
reading as the shared value. Vendor caches (`.opc`) written before the type change are not served and
the vendor is parsed from JSON once (`scripts/build_preset_cache.sh` regenerates).

**The Quality page.** The Line width group sits under the same row of extruders as the Speed page,
with the same line, picker and clear link, and no flow toggle. The picker is stacked here: its label
"Extruder preset:" stands on its own line and the list takes the whole width of the page beside the
undo button (the page's wide label column would clip a name like "0.10mm High Quality (automatic)" in
the narrowest sidebar); the Speed page keeps the one-line row. The dot on an extruder entry is the same on
both pages: it marks an extruder that has anything of its own (a value on either page, a chosen preset, a
chosen flow), so on the Quality page an extruder with speeds set and no width shows the dot, and the
entry's tooltip gives the number of values set. Under All extruders the line names the extruders that
print the line widths of another preset. Under an extruder the widths show that extruder's values (its
own, else its width source's, else the shared value) and every other field of the page is greyed; the
line adds the extruder's preferred layer height when it differs from the plate's and a warning when a
width set for the extruder has the other unit than the value
under All extruders (an older version reads the first number with the percent sign of any entry:
`0.42,0.42,110%` reads as `0.42 %` there); the link "Clear the line widths set for this extruder"
clears the widths alone, the speeds set on the Speed page stay. The picker's item tooltips show the
default, outer wall and first layer widths of every preset ("110 % (0.22 mm)") and mark a preset
whose widths differ from the automatic preset's. A nozzle size change with an absolute line width
set for the extruder raises "Extruder N keeps a line width of X mm set for a Y mm nozzle; it now has Z
mm." with "Clear"; a percent width follows the new nozzle by itself. A slice warning about a width a
extruder prints ("Set for this extruder") and a refusal naming an extruder open the field with that
extruder selected. On a printer without the row (one extruder, a Bambu two-extruder printer, the plate,
object and part tabs) a line width has one value: the field shows it and a write fills every column.

**Limitations.** The brim, the skirt and the layer-0 brim of a tree support are one plate-wide flow
(the first region's outer wall extruder, the first object's support extruder): on the owner plate every
object's brim prints at extruder 1's first layer width. The tree support widths at nozzle 1, the raw
numbers of the first-layer tool order, the arrange skirt distance and the branch tips, and the
filament-diameter ratio of the outer wall volumetric speed keep today's numbers.

## The picker

On the Speed and Quality pages of the Process tab, with an extruder selected in the row above the
settings, the first line is **Extruder preset**: a list with the automatic result first ("0.24mm
Standard (automatic)"), then the system presets made for the extruder's nozzle size, the user presets
and the project presets that fit. Picking one **chooses** it as the extruder's base for its speeds and its line widths:
the fields below show its values, the extruder entry gets a dot, the nozzle tab hint in the sidebar
reads "(chosen)", the undo button beside the list returns to automatic. The tooltip of the automatic
entry says why the automatic preset was chosen; the line under the row names a choice that does not
apply. Values set for the extruder
on either page keep winning above the choice, and values changed under "All extruders" apply to a
chosen extruder too.

The plater's process combo names every extruder's preset in its tooltip. Switching the plate's
preset while an extruder has a chosen preset raises a notice: that extruder keeps its preset, the others
follow the new quality.

## Where the choice lives and what resets it

The choice is a project key, `extruder_process_choice`: one entry per extruder, the name of the
chosen preset, empty for an extruder that follows the rule, `[]` while no extruder has a choice. It is never
stored in a process preset and never in the record. Next to it the project carries the record
`extruder_process_preset`: what every extruder printed with at the last slice, which a load compares
and reports once ("Extruder 3 (0.6 mm) printed with X when this project was saved; it now prints
with Y."). Both keys are in the G-code header.

| event | choice |
|---|---|
| New Project | cleared |
| Open project | the file's value, `[]` when the file has none; a published project brings none |
| Import Configs (from a G-code or a project's config) | the file's value, `[]` when the file has none |
| a nozzle size change, a flow change, a preferred layer height, a printer or process preset switch, the preference | unchanged; the choice is applied or inactive |
| deleting a chosen user preset | unchanged, shown as "(not installed)" |
| the undo button or the automatic item | that extruder's entry cleared |

Nothing is ever dropped automatically. A chosen preset that is not made for the extruder's new nozzle
size, or that is not installed here, stays in the project as inactive (the list shows it disabled,
the nozzle hint says "(inactive)", a notice offers "Choose preset") and applies again when the size
comes back or the preset is installed. A choice applies whether the preference **Process speeds and
line widths follow the nozzle size** is on or off; with it off the other extruders print the
selected preset.
A chosen user preset travels with the project only where it is installed; elsewhere the extruder prints
the automatic preset and the load report says so.

## Other readers

- Snapmaker Orca 2.4 and mainline OrcaSlicer do not know the key and drop it on load and save; every
  extruder prints the selected preset there.
- Snapmaker Orca 2.5 builds before the picker keep the record and drop the choice; reopened here, a
  lost choice reads as the record's "printed with X; now prints with Y".
- The command line composes no per-extruder table: every extruder slices with the loaded process
  preset. It logs the recorded and the chosen presets of the project, writes the record as `[]` into
  its outputs (the truth for that G-code) and keeps the choice.
- Vendor caches (`.opc`) written by a build without the keys (`extruder_process_choice`,
  `extruder_process_flow`) are not served and the vendor is parsed from JSON once; the release
  regenerates them (`scripts/build_preset_cache.sh`).

## J1 duplication and mirror modes

On a J1 plate printed in duplication or mirror mode the firmware replays extruder 1 on extruder 2
(the mode is decided by the plate name at G-code time). A preset chosen for extruder 2 has no
effect on such a plate; the record still names what the G-code carries.

## High Flow values for any filament

A filament preset holds its High Flow values as a second column of the same preset: its
`filament_extruder_variant` list gains "Direct Drive High Flow" ("Bowden High Flow" for a Bowden
column 0), and every key of `filament_options_with_variant` (nozzle, bed and chamber temperatures,
flow ratio, max volumetric speed, pressure advance, fan speeds, retraction overrides and the like)
holds one value per column. This is the layout of the Snapmaker presets that ship with High Flow values; no second
preset is made.

- **Where it starts.** When an extruder of the project is set to High Flow and may print the preset
  (a preset pinned to 0.6 mm gets nothing from a 0.4 mm High Flow extruder), the Filament tab offers
  a High Flow entry for a preset without a High Flow column; not for a material that cannot be
  printed with a High Flow nozzle, such as TPU 85A, or a user preset made from one. Opening the preset
  from a slot on a High Flow extruder selects that entry. The fields show the Standard values and the
  preset stays unmodified; the first change to a field on a line with the extruder icon, or **Create
  High Flow values**, adds the column as a copy of the Standard column. Until then the Standard view
  names the High Flow extruder that prints the Standard values.
- **Which column a change lands in.** A change under High Flow writes the High Flow column, a change
  under Standard the Standard column. The bed temperature of every plate (first and other layers) and
  the Print chamber temperature group (temperature control, target, minimal) are per column like the
  nozzle temperature. Lines without the extruder icon (cooling layer times, overhang fan, adaptive
  pressure advance, G-code, ...) have one value for both columns and are read-only under High Flow,
  their undo arrow and lock included. When a Standard value changes while
  its saved High Flow value was still a copy, the tab offers **Apply to High Flow too**; nothing is
  carried over on its own. After the tab is closed the notice about Standard changes the High Flow
  column does not follow names such a preset.
- **Saving.** A system filament is saved as a new user preset holding both columns. When the system
  preset restricts no printer, the new preset is pinned to the machine preset of the slot's extruder
  (a 0.4 mm extruder under a 0.6 mm printer preset: "Snapmaker U1 (0.4 nozzle)"), else to the printer
  preset. Only the slots whose extruder the new preset fits switch to it; a 0.6 mm slot keeps the
  system preset. A user preset keeps its Standard values and gains the High Flow ones. In a slot on a
  High Flow extruder a preset with High Flow values reads "HF" after its size.
- **What is written.** A per-column key changed in either column is written with both values and
  without "nil"; a key changed in neither column is not written, and both of its columns follow the
  parent's Standard value. A Standard value written this way no longer follows vendor updates of the
  parent. Compare, save and revert use the parent widened by the High Flow column, so a High Flow
  value equal to the parent's Standard value shows the system lock; "Revert all to system" drops the
  added column.
- **Loading.** User, cloud and project presets keep a column their parent lacks: the parent is
  widened by it before the columns are matched by name. A High Flow column saved before the bed and
  chamber temperatures were per column loads with them as a copy of its Standard values; a project
  that stores them once per filament is spread over each filament's columns. A vendor update that later gives the parent
  a High Flow column fills the keys the user never wrote with the vendor's High Flow values; the
  notice about Standard changes the High Flow column does not follow may then appear.
- **Slicing.** A High Flow extruder prints the High Flow column of its filament, a Standard extruder
  the Standard column. The first layer bed temperature (`M140` / `M190`, the "highest" rule or the
  first filament), the bed temperature of the other layers and the chamber temperature (`M191`, the
  highest of the printed filaments) take each filament's value from the column of the extruder that
  prints it; so do the bed and chamber placeholders of custom G-code. An unsaved High Flow column
  slices at once.

Limits:

- **"Vice versa" starts with the High Flow values.** A preset without a High Flow column prints its
  Standard values on a High Flow extruder too, so a Standard change reaches both extruders until the
  preset has High Flow values.
- **An override switched off.** A retraction override switched off under High Flow only is written
  as "nil", the only "off" such a key has, and "nil" means "follow the parent": after loading, the
  override is on again when the parent has it on.

Other readers:

- Snapmaker Orca 2.4, user presets (same data folder): it reads the Standard column of every key and
  never finds "nil" in `filament_flow_ratio`, so the file loads there. Saved again in 2.4, the file
  loses `filament_extruder_variant` and this application reads the Standard values only; the High Flow
  values are gone without a message. The other way round, a preset that 2.4 gave High Flow values
  (`filament_flow_support`) is converted on load here; saved here, 2.4 no longer sees those values.
- Snapmaker Orca 2.4, projects: it reads filament i at column i of every per-column key, while this
  application lists a filament's High Flow column right after its Standard column, so from the first
  filament with High Flow values on, 2.4 prints the next filament with the previous one's High Flow
  values (a PLA at the 265 °C of a PETG, say, and its bed and chamber temperatures). The 15 Snapmaker
  presets with High Flow values already do this. 2.4 shows its notice for projects of a newer schema and goes on loading.
- Mainline OrcaSlicer: the same shift is expected for the keys this application keeps per column;
  not verified.
- The command line takes the columns of a filament file given to `--load-filaments`: a project
  filament column the file lacks (its High Flow column, say) is dropped and named in a log warning;
  the Standard values are kept. `--uptodate` refreshes the presets of any 3MF written by Snapmaker
  Orca; with `--uptodate-filaments` the High Flow column of a project filament is kept, even when the
  refreshing file has none (its High Flow values that the project did not change start as a copy of
  the file's Standard values). Without `--uptodate-filaments` the filament files are looked up among
  the Bambu Lab presets only. A user preset with a High Flow column is not verified on the command line.
