# Upstream Orca profile audit, 2026-10-05

Upstream: OrcaSlicer/OrcaSlicer `main` (eb5b9a77b9, 2026-10-05). Shared base commit with our tree: `737948be1f` (2025-10-02, "Fix bed_exclude_area excluding the whole bed on Anycubic Kobra 3 (#10914)").
Our tree: origin/main 211891fa59. Branch: `feat/upstream-profile-refresh`.

## Summary

- 67 vendors compared (57 ours, 10 only upstream). Every vendor we ship is behind upstream's `main` (ours 02.03.x, upstream 02.04.x); BBL is at 02.00.00.76 vs 02.08.00.12.
- Our own edits since the shared base commit are few and concentrated: the Snapmaker/Qidi/Anycubic/Creality/Flashforge/BBL work (out of scope), the Elegoo CC2 import, Prusa CORE One L, and a handful of 2026-09 repair commits (default materials, bed/texture/cover references). Everything else is upstream drift.
- 49 of the 51 in-scope vendors were refreshed on `feat/upstream-profile-refresh` (one commit each); Elegoo and Custom were deliberately not (section 5). OrcaFilamentLibrary and Prusa are partial (section 5).
- Sanity check (centred 30 mm cube; 888 machine presets on main, 937 on the branch): 98 failures on main, 77 on the branch, none of them new. The refresh fixed 11 (custom G-code variables, a bed exclusion, default process names); four separate commits fix 10 more (Dremel 2, Raise3D 3, Wanhao 1, Elegoo 4); a fifth fixes the Cubicon fan range that the refresh would otherwise have broken. Of the 77 remaining, 46 are `-17` (the default process preset is rejected by the CLI's compatibility check, so the machine itself is not exercised: Prusa 28, Snapmaker 16, Flashforge 2) and 31 are real failures in out-of-scope vendors (Anycubic 12, Creality 11, BBL 4, Qidi 4).
- Application bugs found on the way (not profile data): upstream's Custom toolchanger presets crash the CLI with runaway memory; `skin_infill_line_width` / `skeleton_infill_line_width` default to 100% of the nozzle, which trips `Too small line width` for every 0.2 nozzle + 0.20 mm layer preset that does not set them (Elegoo Neptune 4 family fixed in profiles; Creality 0.2 nozzle machines still fail).

## Method

- Presets are matched by (type, `name`), not by path, because upstream moved and reformatted many files (tabs instead of spaces, new subfolders).
- Three-way classification per preset using the shared base commit: ours == base means pure upstream drift (safe to take); ours != base means an EdgeSlicer-local edit (preserved: upstream content is taken and our changed keys are re-applied on top, or our file is kept as is when upstream did not touch it); preset only in ours and in base means upstream removed or renamed it (kept, user presets inherit by name).
- Upstream renames are recognised by `renamed_from`, by inheritance alignment (a renamed child's parent) and by content similarity >= 0.95; the upstream copy is skipped and our name kept, with every reference in imported content rewritten back to our name.
- Local edits since the base commit (all vendors): 2026-09-12 default-material fixes (`abda6f18e4`), 2026-09-12 bed/texture/cover reference fixes (`2b2a94f31f`), 2026-09-22 case-mismatched asset filenames (`42137fc045`), Prusa CORE One L/L HF (`94e2d6a6c2`), the Elegoo CC2 import, FLSun/Raise3D/Sovol/Wanhao additions, and the big Snapmaker/Qidi/Anycubic/Creality/Flashforge/BBL work (out of scope).

## 1. Vendor table

`new` = presets upstream has that we lack (excluding renames); `renamed` = upstream renamed, we keep our name; `drift` = same-name presets that differ from upstream with no local edit; `local` = same-name presets carrying an EdgeSlicer-local edit; `ours-only` = presets we have that upstream removed/renamed (kept) or that we added.

| Vendor | Scope | Ours | Upstream | new | new models | renamed | drift | local | ours-only | Action |
|---|---|---|---|---|---|---|---|---|---|---|
| Afinia | in | 02.03.01.10 | 02.04.00.06 | 0 | 0 | 0 | 24 | 1 | 6 | refreshed 02.03.01.10 -> 02.04.00.07 |
| Anker | in | 02.03.01.10 | 02.04.00.05 | 0 | 0 | 42 | 38 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.06 |
| Anycubic | out-scope | 02.04.00.02 | 02.04.00.08 | 48 | 1 | 10 | 181 | 108 | 0 | none (out of scope) |
| Artillery | in | 02.03.01.10 | 02.04.00.06 | 5 | 0 | 6 | 138 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.07 |
| BBL | out-scope | 02.00.00.76 | 02.08.00.12 | 207 | 0 | 0 | 1305 | 4 | 495 | none (out of scope) |
| BIQU | in | 02.03.01.10 | 02.04.00.03 | 0 | 0 | 0 | 21 | 3 | 0 | refreshed 02.03.01.10 -> 02.04.00.04 |
| Blocks | in | 02.03.01.10 | 02.04.00.14 | 25 | 0 | 27 | 49 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.15 |
| Chuanying | in | 02.03.01.10 | 02.04.00.06 | 0 | 0 | 17 | 17 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.07 |
| Co Print | in | 02.03.01.10 | 02.04.00.04 | 0 | 0 | 4 | 12 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.05 |
| CoLiDo | in | 02.03.01.10 | 02.04.00.07 | 0 | 0 | 8 | 49 | 1 | 0 | refreshed 02.03.01.10 -> 02.04.00.08 |
| Comgrow | in | 02.03.01.10 | 02.04.00.06 | 0 | 0 | 4 | 27 | 0 | 1 | refreshed 02.03.01.10 -> 02.04.00.07 |
| CONSTRUCT3D | in | 02.03.01.10 | 02.04.00.04 | 0 | 0 | 3 | 15 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.05 |
| Creality | out-scope | 02.03.02.00 | 02.03.02.86 | 504 | 16 | 41 | 794 | 4 | 17 | none (out of scope) |
| Cubicon | in | 02.03.01.10 | 02.04.00.08 | 33 | 2 | 2 | 17 | 1 | 0 | refreshed 02.03.01.10 -> 02.04.00.09 |
| Custom | in | 02.03.01.10 | 02.04.00.07 | 5 | 0 | 0 | 28 | 0 | 0 | NOT refreshed (upstream Custom data crashes our build, see notes) |
| DeltaMaker | in | 02.03.01.10 | 02.04.00.04 | 0 | 0 | 3 | 16 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.05 |
| Dremel | in | 02.03.01.10 | 02.04.00.04 | 0 | 0 | 3 | 24 | 0 | 1 | refreshed 02.03.01.10 -> 02.04.00.05 |
| Elegoo | in | 02.03.01.11 | 02.04.00.12 | 220 | 0 | 106 | 184 | 0 | 111 | NOT refreshed (restructured upstream, shared bases retuned); only 0.2-nozzle line-width fix + version bump |
| Eryone | in | 02.03.01.10 | 02.04.00.07 | 138 | 2 | 0 | 25 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.08 |
| Flashforge | out-scope | 02.03.01.10 | 02.04.00.11 | 72 | 2 | 60 | 564 | 18 | 109 | none (out of scope) |
| FLSun | in | 02.03.01.11 | 02.04.00.05 | 0 | 0 | 24 | 38 | 3 | 21 | refreshed 02.03.01.11 -> 02.04.00.06 |
| FlyingBear | in | 02.03.01.10 | 02.04.00.06 | 12 | 0 | 7 | 53 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.07 |
| Folgertech | in | 02.03.01.10 | 02.04.00.03 | 0 | 0 | 0 | 23 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.04 |
| Geeetech | in | 02.03.01.10 | 02.04.00.02 | 0 | 0 | 0 | 114 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.03 |
| Ginger Additive | in | 02.03.01.10 | 02.04.00.05 | 0 | 0 | 2 | 14 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.06 |
| InfiMech | in | 02.03.01.10 | 02.04.00.07 | 46 | 2 | 6 | 42 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.08 |
| iQ | in | 02.03.01.10 | 02.04.00.05 | 17 | 0 | 0 | 13 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.06 |
| Kingroon | in | 02.03.01.10 | 02.04.00.03 | 0 | 0 | 0 | 15 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.04 |
| LH | new vendor | - | 02.04.00.07 | 36 presets / 2 machines | 2 models | - | - | - | - | not added (left for a follow-up) |
| LONGER | new vendor | - | 02.04.00.05 | 67 presets / 8 machines | 2 models | - | - | - | - | not added (left for a follow-up) |
| Lulzbot | in | 02.03.01.10 | 02.04.00.06 | 7 | 1 | 3 | 18 | 1 | 0 | refreshed 02.03.01.10 -> 02.04.00.07 |
| M3D | new vendor | - | 02.04.00.05 | 18 presets / 2 machines | 2 models | - | - | - | - | not added (left for a follow-up) |
| MagicMaker | in | 02.03.01.10 | 02.04.00.06 | 0 | 0 | 1 | 37 | 1 | 0 | refreshed 02.03.01.10 -> 02.04.00.07 |
| Mellow | in | 02.03.01.10 | 02.04.00.02 | 0 | 0 | 0 | 17 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.03 |
| Meltingplot | new vendor | - | 02.04.00.01 | 105 presets / 25 machines | 5 models | - | - | - | - | not added (left for a follow-up) |
| OpenEYE | new vendor | - | 02.04.00.03 | 46 presets / 4 machines | 1 models | - | - | - | - | not added (left for a follow-up) |
| OrcaArena | in | 02.03.01.10 | 02.04.00.08 | 0 | 0 | 27 | 94 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.09 |
| OrcaFilamentLibrary | in | 02.03.01.10 | 02.04.00.19 | 677 | 0 | 2 | 267 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.20; partial: 346 name-clash presets not imported |
| Peopoly | in | 02.03.01.10 | 02.04.00.04 | 0 | 0 | 3 | 23 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.05 |
| Phrozen | in | 02.03.01.10 | 02.04.00.06 | 0 | 0 | 1 | 6 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.07 |
| Positron3D | in | 02.03.01.10 | 02.04.00.02 | 0 | 0 | 0 | 16 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.03 |
| Pragostroj | new vendor | - | 01.00.01.00 | 45 presets / 14 machines | 2 models | - | - | - | - | not added (left for a follow-up) |
| Prusa | in | 02.03.02.00 | 02.04.00.15 | 147 | 3 | 204 | 396 | 59 | 15 | refreshed 02.03.02.00 -> 02.04.00.16; INDX 4T/8T not imported |
| Qidi | out-scope | 02.04.00.11 | 02.04.00.16 | 0 | 0 | 101 | 217 | 647 | 0 | none (out of scope) |
| Raise3D | in | 02.03.01.12 | 02.04.00.03 | 0 | 0 | 0 | 14 | 2 | 14 | refreshed 02.03.01.12 -> 02.04.00.04 |
| Ratrig | in | 02.03.01.10 | 02.04.00.06 | 1 | 0 | 17 | 132 | 6 | 9 | refreshed 02.03.01.10 -> 02.04.00.07 |
| re3D | new vendor | - | 03.00.12 | 46 presets / 12 machines | 6 models | - | - | - | - | not added (left for a follow-up) |
| RH3D | new vendor | - | 02.04.00.05 | 65 presets / 10 machines | 2 models | - | - | - | - | not added (left for a follow-up) |
| RolohaunDesign | in | 02.03.01.10 | 02.04.00.03 | 0 | 0 | 0 | 26 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.04 |
| SecKit | in | 02.03.01.10 | 02.04.00.04 | 0 | 0 | 10 | 15 | 1 | 0 | refreshed 02.03.01.10 -> 02.04.00.05 |
| SeeMeCNC | new vendor | - | 2.4.0.06 | 174 presets / 28 machines | 7 models | - | - | - | - | not added (left for a follow-up) |
| Snapmaker | out-scope | 02.03.03.04 | 02.04.00.17 | 278 | 0 | 15 | 221 | 63 | 210 | none (out of scope) |
| Sovol | in | 02.03.02.00 | 02.04.00.08 | 26 | 0 | 23 | 76 | 3 | 14 | refreshed 02.03.02.00 -> 02.04.00.09 |
| Tiertime | in | 02.03.01.10 | 02.04.00.05 | 0 | 0 | 56 | 76 | 1 | 0 | refreshed 02.03.01.10 -> 02.04.00.06 |
| Tronxy | in | 02.03.01.10 | 02.04.00.02 | 0 | 0 | 0 | 11 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.03 |
| TwoTrees | in | 02.03.01.10 | 02.04.00.05 | 0 | 0 | 2 | 19 | 3 | 0 | refreshed 02.03.01.10 -> 02.04.00.06 |
| UltiMaker | in | 02.03.01.10 | 02.04.00.05 | 54 | 3 | 3 | 4 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.06 |
| Vivedino | in | 02.03.01.10 | 02.04.00.02 | 0 | 0 | 0 | 13 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.03 |
| Volumic | in | 02.03.01.10 | 02.04.00.05 | 21 | 1 | 0 | 132 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.06 |
| Voron | in | 02.03.01.10 | 02.04.00.03 | 1 | 0 | 0 | 116 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.04 |
| Voxelab | in | 02.03.01.10 | 02.04.00.02 | 0 | 0 | 0 | 6 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.03 |
| Vzbot | in | 02.03.01.10 | 02.04.00.04 | 0 | 0 | 10 | 35 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.05 |
| Wanhao | in | 02.03.01.10 | 02.04.00.04 | 34 | 0 | 0 | 9 | 1 | 0 | refreshed 02.03.01.10 -> 02.04.00.05; D9 family not imported (no cover art) |
| Wanhao France | in | 02.03.01.10 | 02.04.00.06 | 0 | 0 | 2 | 34 | 23 | 0 | refreshed 02.03.01.10 -> 02.04.00.07 |
| WEMAKE3D | new vendor | - | 02.04.00.03 | 54 presets / 8 machines | 2 models | - | - | - | - | not added (left for a follow-up) |
| WonderMaker | in | 02.03.01.10 | 02.04.00.04 | 1 | 0 | 0 | 73 | 0 | 0 | refreshed 02.03.01.10 -> 02.04.00.05 |
| Z-Bolt | in | 02.03.01.10 | 02.04.00.04 | 13 | 0 | 30 | 76 | 8 | 31 | refreshed 02.03.01.10 -> 02.04.00.05 |

## 2. EdgeSlicer-local edits found (preserved)

Same-name presets that carry an EdgeSlicer edit relative to the shared base commit. Out-of-scope vendors are counted only. `merged` = upstream content taken with our changed keys re-applied; `kept_local` = upstream did not touch it, our file kept; `added_by_us_same_name_as_upstream` = we added it after the base and upstream later added the same name, ours kept.

### Afinia (1)

- `fdm_afinia_common` (machine): merged(local keys default_filament_profile; conflicting default_filament_profile) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)

### Anycubic (108)

(out of scope, not listed)

### BBL (4)

(out of scope, not listed)

### BIQU (3)

- `BIQU B1` (machine_model): merged(local keys bed_model,bed_texture; conflicting bed_texture) -- last change: 42137fc045 2026-09-22 profiles: fix case-mismatched bed model, texture and cover filenames
- `BIQU BX` (machine_model): merged(local keys bed_model,bed_texture; conflicting bed_texture) -- last change: 42137fc045 2026-09-22 profiles: fix case-mismatched bed model, texture and cover filenames
- `BIQU Hurakan` (machine_model): merged(local keys bed_model,bed_texture; conflicting bed_texture) -- last change: 42137fc045 2026-09-22 profiles: fix case-mismatched bed model, texture and cover filenames

### CoLiDo (1)

- `fdm_klipper_common` (machine): merged(local keys default_filament_profile; conflicting default_filament_profile) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)

### Creality (4)

(out of scope, not listed)

### Cubicon (1)

- `Cubicon xCeler-I 0.4 nozzle` (machine): merged(local keys default_filament_profile; conflicting -) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)

### Flashforge (18)

(out of scope, not listed)

### FLSun (3)

- `FLSun S1` (machine_model): merged(local keys bed_model; conflicting -) -- last change: 42137fc045 2026-09-22 profiles: fix case-mismatched bed model, texture and cover filenames
- `FLSun T1` (machine_model): merged(local keys bed_model; conflicting -) -- last change: 42137fc045 2026-09-22 profiles: fix case-mismatched bed model, texture and cover filenames
- `0.24mm Draft @FLSun S1` (process): merged(local keys layer_height; conflicting -) -- last change: ff2a694be8 2026-09-30 FLSun S1: 0.24mm Draft now actually sets layer_height 0.24

### Lulzbot (1)

- `Lulzbot Taz Pro S` (machine_model): merged(local keys default_materials; conflicting -) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)

### MagicMaker (1)

- `MM BoneKing 0.4 nozzle` (machine): merged(local keys default_filament_profile; conflicting default_filament_profile) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)

### Prusa (59)

- `Prusament PLA @CORE One` (filament): merged(local keys compatible_printers; conflicting compatible_printers) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `Prusament PLA @CORE One HF 0.4` (filament): merged(local keys compatible_printers; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `Prusa MK3S 0.25 nozzle` (machine): merged(local keys nozzle_diameter; conflicting -) -- last change: 4283086c36 2026-09-12 Profiles: upstream validator 26 errors -> 10, and the 10 left are a pre-existing name clash
- `0.05mm DETAIL @CORE One 0.25` (process): merged(local keys compatible_printers_condition; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.07mm DETAIL @CORE One 0.25` (process): merged(local keys compatible_printers_condition; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.10mm FAST DETAIL @CORE One 0.4` (process): merged(local keys compatible_printers_condition,inner_wall_speed,outer_wall_speed,small_perimeter_speed; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.10mm STRUCTURAL @CORE One 0.5` (process): merged(local keys compatible_printers_condition; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.12mm SPEED @CORE One 0.25` (process): merged(local keys compatible_printers_condition; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.12mm STRUCTURAL @CORE One 0.25` (process): merged(local keys compatible_printers_condition; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.12mm STRUCTURAL @CORE One 0.3` (process): merged(local keys compatible_printers_condition; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.15mm SPEED @CORE One 0.25` (process): merged(local keys compatible_printers_condition; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.15mm SPEED @CORE One 0.4` (process): merged(local keys compatible_printers_condition,overhang_2_4_speed,overhang_3_4_speed,support_threshold_angle; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.15mm SPEED @CORE One HF 0.4` (process): merged(local keys compatible_printers_condition; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.15mm STRUCTURAL @CORE One 0.25` (process): merged(local keys compatible_printers_condition; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.15mm STRUCTURAL @CORE One 0.4` (process): merged(local keys compatible_printers_condition,overhang_2_4_speed,overhang_3_4_speed; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.15mm STRUCTURAL @CORE One 0.5` (process): merged(local keys compatible_printers_condition; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.15mm STRUCTURAL @CORE One 0.6` (process): merged(local keys compatible_printers_condition; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.16mm SPEED @CORE One 0.3` (process): merged(local keys compatible_printers_condition; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.16mm STRUCTURAL @CORE One 0.3` (process): merged(local keys compatible_printers_condition; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.20mm SOLUBLE FULL @CORE One 0.4` (process): merged(local keys compatible_printers_condition,inner_wall_speed,outer_wall_speed,overhang_2_4_speed,overhang_3_4_speed,small_perimeter_speed; conflic -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.20mm SOLUBLE INTERFACE @CORE One 0.4` (process): merged(local keys compatible_printers_condition,inner_wall_speed,outer_wall_speed,overhang_2_4_speed,overhang_3_4_speed,small_perimeter_speed; conflic -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.20mm SPEED @CORE One 0.3` (process): merged(local keys compatible_printers_condition; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.20mm SPEED @CORE One 0.4` (process): merged(local keys compatible_printers_condition,overhang_2_4_speed,overhang_3_4_speed,support_threshold_angle; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.20mm SPEED @CORE One 0.5` (process): merged(local keys compatible_printers_condition; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- `0.20mm SPEED @CORE One 0.6` (process): merged(local keys compatible_printers_condition; conflicting -) -- last change: 94e2d6a6c2 2026-09-12 Prusa CORE One L / L HF profiles, and PrusaLink status on the phone hub
- ... and 34 more

### Qidi (647)

(out of scope, not listed)

### Raise3D (2)

- `Raise3D Pro3` (machine_model): merged(local keys default_materials; conflicting -) -- last change: ca2134405c 2026-10-02 Add Raise3D-branded filament presets for N2, N2 Plus, Pro3, Pro3 Plus
- `Raise3D Pro3 Plus` (machine_model): merged(local keys default_materials; conflicting -) -- last change: ca2134405c 2026-10-02 Add Raise3D-branded filament presets for N2, N2 Plus, Pro3, Pro3 Plus

### Ratrig (6)

- `RatRig V-Core 4 HYBRID 500 0.5 nozzle` (machine): merged(local keys nozzle_diameter; conflicting -) -- last change: 4283086c36 2026-09-12 Profiles: upstream validator 26 errors -> 10, and the 10 left are a pre-existing name clash
- `RatRig V-Core 4 HYBRID 500 0.8 nozzle` (machine): merged(local keys nozzle_diameter; conflicting -) -- last change: 4283086c36 2026-09-12 Profiles: upstream validator 26 errors -> 10, and the 10 left are a pre-existing name clash
- `RatRig V-Core 4 IDEX 300 COPY MODE` (machine_model): merged(local keys bed_model; conflicting bed_model) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `RatRig V-Core 4 IDEX 300 MIRROR MODE` (machine_model): merged(local keys bed_model; conflicting bed_model) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `RatRig V-Core 4 IDEX 500 COPY MODE` (machine_model): merged(local keys bed_model; conflicting bed_model) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `RatRig V-Core 4 IDEX 500 MIRROR MODE` (machine_model): merged(local keys bed_model; conflicting bed_model) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing

### SecKit (1)

- `Seckit Go3` (machine_model): merged(local keys bed_texture; conflicting -) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing

### Snapmaker (63)

(out of scope, not listed)

### Sovol (3)

- `Sovol SV07` (machine_model): merged(local keys bed_model,nozzle_diameter; conflicting -) -- last change: 4283086c36 2026-09-12 Profiles: upstream validator 26 errors -> 10, and the 10 left are a pre-existing name clash
- `Sovol SV07 Plus` (machine_model): merged(local keys bed_model; conflicting -) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `Sovol SV08 MAX` (machine_model): merged(local keys default_materials; conflicting -) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)

### Tiertime (1)

- `fdm_tiertime_common` (machine): merged(local keys default_filament_profile; conflicting default_filament_profile) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)

### TwoTrees (3)

- `fdm_klipper_common` (machine): merged(local keys default_filament_profile; conflicting default_filament_profile) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)
- `TwoTrees SK1` (machine_model): merged(local keys default_materials; conflicting default_materials) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)
- `TwoTrees SP-5 Klipper` (machine_model): merged(local keys default_materials; conflicting default_materials) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)

### Wanhao (1)

- `Wanhao D12-300` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing

### Wanhao France (23)

- `D12 230 PRO SMARTPAD DIRECT 0.4 nozzle` (machine): merged(local keys default_filament_profile; conflicting -) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)
- `D12 300 PRO M2 DIRECT 0.4 nozzle` (machine): merged(local keys default_filament_profile; conflicting -) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)
- `D12 300 PRO SMARTPAD DIRECT 0.4 nozzle` (machine): merged(local keys default_filament_profile; conflicting -) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)
- `D12 500 PRO M2 DIRECT 0.4 nozzle` (machine): merged(local keys default_filament_profile; conflicting -) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)
- `D12 500 PRO SMARTPAD DIRECT 0.4 nozzle` (machine): merged(local keys default_filament_profile; conflicting -) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)
- `D12 230 PRO M2 DIRECT` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `D12 230 PRO M2 MONO DUAL` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `D12 230 PRO M2 MONO DUAL PoopTool` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `D12 230 PRO SMARTPAD DIRECT` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `D12 230 PRO SMARTPAD MONO DUAL` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `D12 230 PRO SMARTPAD MONO DUAL PoopTool` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `D12 300 PRO M2 DIRECT` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `D12 300 PRO M2 MONO DUAL` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `D12 300 PRO M2 MONO DUAL PoopTool` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `D12 300 PRO SMARTPAD DIRECT` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `D12 300 PRO SMARTPAD MONO DUAL` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `D12 300 PRO SMARTPAD MONO DUAL PoopTool` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `D12 500 PRO M2 DIRECT` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `D12 500 PRO M2 MONO DUAL` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `D12 500 PRO M2 MONO DUAL PoopTool` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `D12 500 PRO SMARTPAD DIRECT` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `D12 500 PRO SMARTPAD MONO DUAL` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing
- `D12 500 PRO SMARTPAD MONO DUAL PoopTool` (machine_model): merged(local keys bed_texture; conflicting bed_texture) -- last change: 2b2a94f31f 2026-09-12 Profiles: the 31 bed, texture and cover references that pointed at nothing

### Z-Bolt (8)

- `Z-Bolt S1000 0.4 nozzle` (machine): merged(local keys default_filament_profile; conflicting default_filament_profile) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)
- `Z-Bolt S300 0.4 nozzle` (machine): merged(local keys default_filament_profile; conflicting default_filament_profile) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)
- `Z-Bolt S400 0.4 nozzle` (machine): merged(local keys default_filament_profile; conflicting default_filament_profile) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)
- `Z-Bolt S600 0.4 nozzle` (machine): merged(local keys default_filament_profile; conflicting default_filament_profile) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)
- `Z-Bolt S800 Dual 0.4 nozzle` (machine): merged(local keys default_filament_profile; conflicting default_filament_profile) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)
- `fdm_zbolt_common` (machine): merged(local keys default_filament_profile; conflicting default_filament_profile) -- last change: abda6f18e4 2026-09-12 Profiles: every default material now names a filament that exists (112 --check-materials errors -> 0)
- `Z-Bolt S1000` (machine_model): merged(local keys bed_model; conflicting -) -- last change: 42137fc045 2026-09-22 profiles: fix case-mismatched bed model, texture and cover filenames
- `Z-Bolt S1000 Dual` (machine_model): merged(local keys bed_model; conflicting -) -- last change: 42137fc045 2026-09-22 profiles: fix case-mismatched bed model, texture and cover filenames


## 3. Sanity check: centred 30 mm cube, `--arrange 1 --slice 0`, every instantiable machine preset

Harness: copy of the installed EdgeSlicer (211891fa59) in a scratch folder, `--datadir` scratch, `--printer-preset <machine> --process-preset <default process> --filament-presets <default filament>`, one CLI process per machine, 120 s timeout. If the machine's `default_print_profile` / `default_filament_profile` does not exist or is not compatible, the run is repeated with a compatible process/filament preset (so the machine itself is still exercised) and the defect is counted separately in 3.3.

| Tree | machines sliced | failing |
|---|---|---|
| main (origin/main 211891fa59) | 888 | 98 |
| branch (feat/upstream-profile-refresh) | 937 | 77 |
| upstream profiles in our build (failing machines only) | 99 | 70 |

### 3.1 Failures on main

`Same in upstream?`: the same machine preset fails the same way with upstream's own profile tree loaded into our build; `n/a` = the preset does not exist upstream (EdgeSlicer-only or renamed). `Branch`: result after this PR.

| Vendor | Preset(s) | Symptom | Root cause | Same in upstream? | Branch | Disposition |
|---|---|---|---|---|---|---|
| Anycubic | Anycubic Kobra Max 0.4 nozzle, Anycubic Kobra Plus 0.4 nozzle, Anycubic Vyper 0.4 nozzle | -51: relative extruder addressing without 'G92 E0' per layer | layer gcode lacks G92 E0 | y | FAIL | out of scope, not fixed (route to owner) |
| Anycubic | Anycubic Kobra 3 0.2 nozzle, Anycubic Kobra 3 0.4 nozzle, Anycubic Kobra 3 0.6 nozzle, Anycubic Kobra 3 0.8 nozzle | -50: no object fully inside the print volume | bed_exclude_area is the whole bed outline plus a 2 mm inset ring, i.e. the whole bed is excluded (see 3.3) | y | FAIL | out of scope, not fixed (route to owner) |
| Anycubic | Anycubic Kobra S1 Max 0.25 nozzle, Anycubic Kobra S1 Max 0.4 nozzle, Anycubic Kobra S1 Max 0.6 nozzle, Anycubic Kobra S1 Max 0.8 nozzle, Anycubic Kobra X 0.4 nozzle | -18: retraction_distances_when_cut = 0 not in [10,18] | value outside the range this build accepts | y | FAIL | out of scope, not fixed (route to owner) |
| BBL | Bambu Lab A2L 0.2 nozzle, Bambu Lab A2L 0.4 nozzle, Bambu Lab A2L 0.6 nozzle, Bambu Lab A2L 0.8 nozzle | -100: custom G-code fails to parse: M201 N1 Y[curr_y_acceleration_limit] // ^ | G-code references a variable that does not exist | y | FAIL | out of scope, not fixed (route to owner) |
| Creality | Creality CR-10 SE 0.2 nozzle, Creality Ender-3 0.2 nozzle, Creality Ender-3 Pro 0.2 nozzle, Creality Ender-3 S1 Plus 0.2 nozzle, Creality Ender-3 V3 KE 0.2 nozzle, Creality Ender-3 V3 SE 0.2 nozzle | -51: Too small line width (width <= layer height) | 0.2 nozzle with 0.20 layer: skin/skeleton infill width defaults to 100% of the nozzle (0.2) and the process widths are 0.2, all <= layer height | y | FAIL | out of scope, not fixed (route to owner) |
| Creality | Creality K2 0.4 nozzle | -18: retraction_distances_when_cut = 30 not in [10,18] | value outside the range this build accepts | y | FAIL | out of scope, not fixed (route to owner) |
| Creality | Creality SPARKX i7 0.2 nozzle, Creality SPARKX i7 0.4 nozzle, Creality SPARKX i7 0.6 nozzle, Creality SPARKX i7 0.8 nozzle | -18: retraction_distances_when_cut = 28 not in [10,18] | value outside the range this build accepts | y | FAIL | out of scope, not fixed (route to owner) |
| Dremel | Dremel 3D40 0.4 nozzle, Dremel 3D45 0.4 nozzle | -51: 'G92 E0' in before_layer_gcode with absolute extruder addressing | G92 E0 vs absolute E | y | ok | fixed (separate commit) |
| Elegoo | Elegoo Neptune 4 (0.2 nozzle), Elegoo Neptune 4 Max (0.2 nozzle), Elegoo Neptune 4 Plus (0.2 nozzle), Elegoo Neptune 4 Pro (0.2 nozzle) | -51: Too small line width (width <= layer height) | 0.2 nozzle with 0.20 layer: skin/skeleton infill width defaults to 100% of the nozzle (0.2) and the process widths are 0.2, all <= layer height | n/a | ok | fixed (separate commit) |
| Flashforge | Flashforge Creator 5 0.4 nozzle, Flashforge Creator 5 0.6 nozzle | -17: default process preset not compatible with the machine (CLI check) | default_print_profile / compat condition | n | FAIL | out of scope, not fixed (route to owner) |
| FLSun | FLSun QQ-S Pro 0.4 nozzle | -100: custom G-code fails to parse: M117 Print [output_filename_format]; Display: Printing started... // ^ | G-code references a variable that does not exist | n | ok | fixed by upstream refresh |
| Folgertech | Folgertech i3 0.6 nozzle | -50: no object fully inside the print volume | bed_exclude_area is the whole bed outline plus a 2 mm inset ring, i.e. the whole bed is excluded (see 3.3) | n | ok | fixed by upstream refresh |
| Ginger Additive | Ginger G1 1.2 nozzle, Ginger G1 3.0 nozzle, Ginger G1 5.0 nozzle, Ginger G1 8.0 nozzle | -100: custom G-code fails to parse: START_PRINT BED_TEMPERATURE=[bed_temperature_initial_layer] KAMP_LEVELING=1 EXTRUDER_ROTATION_VOLUME={extruder_rotation_volume[0]} MIXING_ST | G-code references a variable that does not exist | n | ok | fixed by upstream refresh |
| Lulzbot | Lulzbot Taz Pro S 0.5 nozzle | -100: custom G-code fails to parse: {if enable_pressure_advance == 1}M900 K{pressure_advance[0]}; set pressure advance // ^ | G-code references a variable that does not exist | n | ok | fixed by upstream refresh |
| Prusa | 28 presets: Prusa CORE One 0.25 nozzle ... Prusa MK4S HF0.8 nozzle | -17: default process preset not compatible with the machine (CLI check) | default_print_profile / compat condition | y | FAIL | not fixed: default process rejected by the CLI compatibility check, machine geometry not exercised (same on main) |
| QIDI | Qidi Q1 Pro 0.2 nozzle, Qidi Q1 Pro 0.4 nozzle, Qidi Q1 Pro 0.6 nozzle, Qidi Q1 Pro 0.8 nozzle | -64: Prime tower collides with bed_exclude_area | prime tower vs exclusion area | n/a | FAIL | out of scope, not fixed (route to owner) |
| Raise3D | Raise3D Pro3 Plus 0.4 nozzle (Dual), Raise3D Pro3 Plus 0.4 nozzle (Left), Raise3D Pro3 Plus 0.4 nozzle (Right) | -51: relative extruder addressing without 'G92 E0' per layer | layer gcode lacks G92 E0 | y | ok | fixed (separate commit) |
| Ratrig | RatRig V-Core 4 300 0.8 nozzle, RatRig V-Core 4 400 0.8 nozzle, RatRig V-Core 4 500 0.8 nozzle | -17: default process preset not compatible with the machine (CLI check) | default_print_profile / compat condition | n | ok | fixed by upstream refresh |
| RolohaunDesign | Rolohaun Delta Flyer Refit 0.4 nozzle | -17: default process preset not compatible with the machine (CLI check) | default_print_profile / compat condition | n | ok | fixed by upstream refresh |
| Snapmaker | 16 presets: Snapmaker A250 (0.8 nozzle) ... Snapmaker A350 QSKit (0.8 nozzle) | -17: default process preset not compatible with the machine (CLI check) | default_print_profile / compat condition | n | FAIL | out of scope, not fixed (route to owner) |
| Wanhao | Wanhao D12-300 0.4 nozzle | -51: relative extruder addressing without 'G92 E0' per layer | layer gcode lacks G92 E0 | y | ok | fixed (separate commit) |

### 3.2 Failures that exist only on the branch (new machines or regressions)

None in the final state. During the work the refresh itself introduced three (Cubicon xCeler-I, xCeler-Mini, xCeler-Plus: `during/complete_print_exhaust_fan_speed` 255 not in [0,100], upstream's value); the Cubicon fan commit fixes them.

### 3.3 Static flags (all machine presets, `instantiation: true`)

Checks: `bed_exclude_area` bounding box or even-odd/non-zero fill covering the bed centre or > 50% of the bed, `printable_area` smaller than 30 mm / not parsable / not containing its own centre, `printable_height` <= 0 or < 30, `nozzle_diameter` or `printer_variant` disagreeing with the preset name, missing `bed_model` / `bed_texture` / `hotend_model` / `_cover.png`, `default_print_profile` / `default_filament_profile` that do not exist or (for the process) do not list the machine in `compatible_printers`.

| Flag | main | upstream (as shipped) | branch |
|---|---|---|---|
| bed_exclude_area ring/covers bed | 4 | 4 | 4 |
| default_filament_profile does not exist | 1 | 2 | 2 |
| default_print_profile does not exist | 67 | 75 | 69 |
| default_print_profile not compatible | 114 | 110 | 110 |
| missing bed_model 'Z-Bolt_S1000_buildplate_model.s | 0 | 6 | 0 |
| missing bed_model 'elegoo_neptune_pro_buildplate_m | 0 | 5 | 0 |
| missing bed_texture 'elegoo_centuri_carbon_buildpl | 0 | 4 | 0 |
| missing cover 'Wanhao D9-300 MK1 BLTouch kit_cover | 0 | 1 | 0 |
| missing cover 'Wanhao D9-300 MK1_cover.png' | 0 | 1 | 0 |
| missing cover 'Wanhao D9-300 MK2_cover.png' | 0 | 1 | 0 |
| missing cover 'Wanhao D9-300 MK3_cover.png' | 0 | 1 | 0 |
| missing cover 'Wanhao D9-400 MK1 BLTouch kit_cover | 0 | 1 | 0 |
| missing cover 'Wanhao D9-400 MK1_cover.png' | 0 | 1 | 0 |
| missing cover 'Wanhao D9-400 MK2_cover.png' | 0 | 1 | 0 |
| missing cover 'Wanhao D9-400 MK3_cover.png' | 0 | 1 | 0 |
| missing cover 'Wanhao D9-500 MK1 BLTouch kit_cover | 0 | 1 | 0 |
| missing cover 'Wanhao D9-500 MK1_cover.png' | 0 | 1 | 0 |
| missing cover 'Wanhao D9-500 MK2_cover.png' | 0 | 1 | 0 |
| missing cover 'Wanhao D9-500 MK3_cover.png' | 0 | 1 | 0 |
| missing hotend_model file | 52 | 55 | 51 |
| nozzle_diameter unparsable ['0.4,0.4'] | 0 | 1 | 0 |
| printable_area not a list of XxY points | 11 | 37 | 11 |
| printable_height not a plain number | 4 | 0 | 4 |
| printer_variant '0.4+0.6' != name nozzle 0.6 | 0 | 1 | 0 |

Flagged machines that matter (not the default-reference or hotend noise):

| Vendor | Preset | Flag | Root cause | Same in upstream? | Scope |
|---|---|---|---|---|---|
| Anycubic | Anycubic Kobra 3 0.2 nozzle, Anycubic Kobra 3 0.4 nozzle, Anycubic Kobra 3 0.6 nozzle, Anycubic Kobra 3 0.8 nozzle | bed_exclude_area ring/covers bed | bed_exclude_area is the whole bed outline (0x0, 255x0, 255x255, 0x255, 0x0) followed by a 2 mm inset outline (2x2 ... 2x2), meant as a 2 mm border; the polygon is treated as ONE excluded region, so every position on the bed is excluded and the arranged cube is never 'fully inside' (CLI -50). Upstream #10914 replaced a different bad polygon with this one. Fix: empty bed_exclude_area (or four thin border rectangles) | y | out of scope |
| Creality | 11 presets (Creality Ender-3 V4 0.4 nozzle ... Creality SPARKX i7 0.8 nozzle) | printable_area not a list of XxY points | printable_area stored as one comma-joined string, e.g. "0x0,400x0,400x400,0x400"; parses and slices, the check just cannot read it | y | out of scope |
| Elegoo | Elegoo OrangeStorm Giga 0.4 nozzle, Elegoo OrangeStorm Giga 0.6 nozzle, Elegoo OrangeStorm Giga 0.8 nozzle, Elegoo OrangeStorm Giga 1.0 nozzle | printable_height not a plain number | printable_height stored as a one-element list (['1010']) instead of a string; slices fine | n | in scope |

`missing hotend_model file`: 52 machines on main, 55 upstream, 51 on the branch reference a `*_hotend.stl` that is not shipped (BIQU, Chuanying, Creality, Flashforge, MagicMaker, Qidi, SecKit, Vzbot, Wanhao ...). The viewer falls back to the default hotend; nothing breaks. Not fixed.

`default_print_profile` / `default_filament_profile` defects: 181 / 1 machines on main (179 / 2 on the branch) name a default that does not exist or does not list the machine; the GUI then falls back to the first compatible preset, so users see a different default than the profile author intended. Biggest groups on the branch: Ratrig 57, Flashforge 19, Volumic 14, Creality 12, Anker 9, Artillery 8.


## 4. Validation (main vs branch)

| Check | main | branch |
|---|---|---|
| `OrcaSlicer_profile_validator -l 2`, whole tree (upstream binary, SoftFever/Orca_tools release 1) | 0 errors, 58 vendors loaded | 0 errors, 58 vendors loaded |
| `scripts/orca_extra_profile_check.py` (bare, as CI runs it) | 0 files with errors, 360 warnings | 0 files with errors, 374 warnings |
| `scripts/check_preset_name_clashes.py` | OK (9650 names) | OK (10581 names) |
| per-vendor `--check-filaments --check-materials`, 51 vendors (50 refreshed + Elegoo): files with errors | 148 (all in OrcaFilamentLibrary, see below) | 270 (all in OrcaFilamentLibrary) |
| same, warnings | 239 | 253 |
| per-vendor upstream validator `-v <vendor>`, 51 vendors | 50 ok / 50 | 50 ok / 50 |

The upstream validator aborts at the first bad vendor, so a whole-tree count is a floor; the per-vendor runs enumerate. Both are 0 errors on the branch.

`OrcaFilamentLibrary` under `--check-filaments`: 148 errors on main, 270 on the branch. The check demands `compatible_printers` on every filament file, which the library deliberately never sets (it is printer-agnostic); the extra 122 are the 677 newly imported presets of the same kind. CI does not run that flag, and the bare whole-tree run is 0 errors.

Per-vendor warning deltas (`--check-filaments --check-materials`): Cubicon 1 -> 3, Eryone 1 -> 3, InfiMech 2 -> 4, Lulzbot 4 -> 5, Prusa 13 -> 16, UltiMaker 1 -> 4, Volumic 20 -> 21. The increases are new presets whose `compatible_printers` / default materials follow upstream's conventions.

## 5. What the branch does and does not do

### Imported (per vendor, one commit each)

Same-name presets that are pure upstream drift are replaced with upstream's content (upstream's tab indentation comes with it, hence the large whitespace-only diffs; `git diff -w` is the real change). New upstream presets are added with their vendor-JSON list entries; lists are re-sorted so every `inherits` parent precedes its child (the loader resolves in list order). Assets (bed models, textures, covers) that upstream added or changed are copied when our copy was unmodified. Each vendor JSON `version` is bumped to the larger of ours/upstream's with the last component +1.

### Preserved EdgeSlicer-local deviations

Re-applied on top of upstream's content or kept as ours (see section 2 for the full list): the 2026-09-12 `default_filament_profile` / `default_materials` repairs (Afinia, Z-Bolt, ...), the 2026-09-12 and 2026-09-22 bed model / texture / cover reference fixes (BIQU, Ratrig, Wanhao France, Raise3D, Sovol, Lulzbot, ...; exact files in section 2), the Prusa CORE One L / L HF presets, and every preset EdgeSlicer added (Raise3D, FLSun, Sovol, Prusa).

### Upstream renames (our names kept)

658 upstream renames were not followed (user presets inherit by name). The upstream copy is skipped, its content refreshes our preset under our name, and references to the new name in imported files are rewritten back. Per vendor: Anker 42, Artillery 6, Blocks 27, Chuanying 17, Co Print 4, CoLiDo 8, Comgrow 4, CONSTRUCT3D 3, Cubicon 2, DeltaMaker 3, Dremel 3, Elegoo 106, FLSun 24, FlyingBear 7, Ginger Additive 2, InfiMech 6, Lulzbot 3, MagicMaker 1, OrcaArena 27, OrcaFilamentLibrary 2, Peopoly 3, Phrozen 1, Prusa 204, Ratrig 17, SecKit 10, Sovol 23, Tiertime 56, TwoTrees 2, UltiMaker 3, Vzbot 10, Wanhao France 2, Z-Bolt 30. The upstream renames are mostly the generic filament family (`Anker Generic PLA` -> `Generic PLA @Anker`) and `X @Vendor` filament bases.

### Adapted to this build

- `OrcaFilamentLibrary`: 346 upstream presets (and everything inheriting from them) are NOT imported because their names already exist in other vendors that this branch does not refresh (BBL, Elegoo, Qidi, Snapmaker ...); importing them fails the upstream validator with `Found duplicated preset` (263 errors). 5 presets whose upstream version inherits one of them keep our version. 5 children are pinned to their old `filament_id` because upstream moved it out of the parent. They can be imported once BBL/Elegoo are refreshed.
- `Prusa`: `Prusa CORE One INDX 4T/8T` (2 models, 2 machines, 4 process, 6 filament presets) not imported: their start G-code uses `chamber_minimal_temperature`, which this build does not define (CLI -100 'Variable does not exist').
- `Wanhao`: the 12 new D9 models (and their machines, process and filament presets) are not imported: upstream ships them without `_cover.png` and the asset check treats that as an error.
- `Cubicon`: `during_print_exhaust_fan_speed` / `complete_print_exhaust_fan_speed` 255 -> 100 in four filament bases (range is [0,100] in this build; upstream ships 255). Separate commit.
- `retraction_distances_when_cut` ([10,18] in this build): no refreshed vendor carries an out-of-range value after the refresh; the out-of-range ones (Creality K2 30, SPARKX i7 28, Anycubic Kobra S1 Max / X 0) are in out-of-scope vendors and fail the same way on main.

### Not refreshed (left for later)

- `Elegoo` (02.03.01.11, upstream 02.04.00.12): upstream reorganised the whole tree (BASE/EN2SERIES/..., 106 renames, 220 new presets) and retuned the shared bases (`fdm_process_elegoo_common`, `fdm_filament_common`, ...), which would silently re-tune every Neptune/Centauri we ship; our own CC2 import (`CC2BASE`) overlaps it. Needs its own pass. Only the Neptune 4 0.2 nozzle fix is applied (version bumped to 02.04.00.13 because a file changed).
- `Custom` (02.03.01.10, upstream 02.04.00.07): upstream's Klipper/toolchanger presets use per-extruder-variant arrays (`extruder_variant_list`, `print_extruder_variant`, 3- and 15-element machine limits). Importing them makes `MyToolChanger 0.2/0.4/0.6/0.8` crash the CLI (access violation / runaway memory, 4-16 GB, killed after 120 s). Keeping all `fdm_*` bases and the toolchanger family at our version makes it slice again, but leaves almost nothing to refresh, so Custom is skipped entirely. This is an application bug worth a look: a profile must never be able to crash the slicer. New `Generic Repetier Printer` also unusable (`gcode_flavor: repetier` is rejected: invalid value).
- New upstream vendors not added: LH, LONGER, M3D, Meltingplot, OpenEYE, Pragostroj, RH3D, SeeMeCNC, WEMAKE3D, re3D (10 vendors, ~650 presets, ~113 machines). Pure additions; left out because the brief was to refresh vendors that are behind.
- Out of scope, untouched: Qidi, Anycubic, Creality, BBL, Snapmaker, Flashforge. Their rows in section 1 show how far behind they are (BBL 02.00.00.76 vs upstream 02.08.00.12; 1305 same-name presets differ and 207 new).

### Route to the owners of the out-of-scope vendors

- Anycubic Kobra 3 (0.2/0.4/0.6/0.8): `bed_exclude_area` excludes the whole bed (see 3.3). Same in upstream. Empty the key or use border rectangles.
- Anycubic Kobra S1 Max (4), Kobra X, Creality K2, SPARKX i7 (4): `retraction_distances_when_cut` outside [10,18] (-18). Same in upstream.
- Anycubic Kobra Max / Plus / Vyper: relative extruder addressing without `G92 E0` per layer (-51).
- Creality Ender-3 / Ender-3 Pro / Ender-3 S1 Plus / Ender-3 V3 KE / V3 SE / CR-10 SE (0.2 nozzle): same line-width/layer-height failure as the Elegoo Neptune 4 family (skin/skeleton infill width defaults to the 0.2 nozzle diameter).
- Qidi Q1 Pro (4): prime tower collides with `bed_exclude_area` (-64). Not in upstream under this name.
- BBL A2L (4): `M201 N1 Y[curr_y_acceleration_limit]` references a variable this build does not define (-100). Same in upstream.
- Flashforge Creator 5 (2), Snapmaker A250/A350 0.8 nozzle (16): default process preset does not list the machine.

## 6. Tooling

Everything was run from a scratch copy of the installed build (`%TEMP%\orcarefresh\bin_*`) with scratch data dirs; nothing touched `%APPDATA%`, no printers were contacted, no builds were made. The CLI leaves one `ultra_cli_presets_<pid>` folder in `%TEMP%` per run (about 3000 small folders from this audit); they are harmless but could be swept. The refresh itself was done with a name-keyed three-way merge script that edits JSON textually (no re-serialisation).
