# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Snapmaker_Orca is a C++17 3D slicer forked from Bambu Studio, using wxWidgets for the GUI and CMake as the build system. Sources live in `src/`, split between the platform-independent core (`src/libslic3r/`) and the GUI application (`src/slic3r/`). User assets and printer presets are in `resources/`; translations in `localization/`. Treat everything in `deps/` and `deps_src/` as vendored snapshots — do not modify without mirroring upstream tags.

## Build Commands

### Windows
```bash
build_release_vs2022.bat           # full build
build_release_vs2022.bat debug     # debug build
build_release_vs2022.bat debuginfo # release with debug symbols
build_release_vs2022.bat deps      # dependencies only
build_release_vs2022.bat slicer    # app only (after deps built)
```
Uses Visual Studio 17 2022 generator, x64. CMake minimum 3.13, maximum 3.31.x.

### macOS
```bash
./build_release_macos.sh           # full build
./build_release_macos.sh -d        # dependencies only
./build_release_macos.sh -s        # slicer only
./build_release_macos.sh -sx       # slicer with Ninja (faster, use for reproducing build issues)
./build_release_macos.sh -a arm64  # specific arch (arm64, x86_64, universal)
./build_release_macos.sh -t 11.3   # target macOS version
```

### Linux
```bash
./build_linux.sh -u    # first-time: install system dependencies
./build_linux.sh -dsi  # full build (deps + slicer + AppImage)
./build_linux.sh -d    # dependencies only
./build_linux.sh -s    # slicer only
./build_linux.sh -b    # debug build
./build_linux.sh -c    # clean build
./build_linux.sh -j N  # limit to N cores
./build_linux.sh -l    # use Clang instead of GCC
scripts/DockerBuild.sh # reproducible container build
```

### Direct CMake
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Snapmaker_Orca --config Release --parallel
```

Build output goes to `build/`; dependencies to `deps/build/`.

## Testing

Tests use Catch2 (`tests/catch2/`). Name test files after the component: `tests/libslic3r/TestPlanarHole.cpp`. Fixtures and sample G-code go in `tests/data/`. Tag long-running cases so `ctest -L fast` stays useful.

```bash
# All tests
cmake --build build --target tests
ctest --test-dir build --output-on-failure

# Fast subset
ctest --test-dir build -L fast

# Individual suites (from build/)
./tests/libslic3r/libslic3r_tests
./tests/fff_print/fff_print_tests
./tests/sla_print/sla_print_tests
```

Test domains: `tests/libslic3r/` (geometry, file formats), `tests/fff_print/` (FDM slicing, G-code), `tests/sla_print/` (SLA), `tests/libnest2d/` (2D nesting), `tests/slic3rutils/` (utilities).

## Code Style

`.clang-format` enforces 4-space indents, 140-column limit, aligned initializers, and brace wrapping for classes and functions. Run `clang-format -i <file>` before committing; the CMake `clang-format` target is available when LLVM tools are on PATH.

- Classes: `CamelCase`
- Functions and locals: `snake_case`
- Constants: `SCREAMING_CASE`
- Headers: `#pragma once`, self-contained, IWYU-aligned include order

## Architecture

### Slicing Pipeline

Entry point: `src/Snapmaker_Orca.cpp` — handles platform-specific init, GPU optimization exports (NVIDIA/AMD), Sentry error reporting, and wxWidgets bootstrap via `GUI_Init.hpp`.

The slicing pipeline is orchestrated by `src/libslic3r/Print.cpp`. Key classes: `Print`, `PrintObject`, `Layer`, `GCode`. All print/printer/material settings are defined in `src/libslic3r/PrintConfig.cpp` (~8700 lines) — this is the central settings registry.

### Core Library (`src/libslic3r/`)

Platform-independent slicing engine. Key subdirectories:
- `GCode/` — G-code generation, cooling, pressure advance, thumbnails
- `Fill/` — infill patterns (gyroid, honeycomb, lightning, etc.)
- `Support/` — tree supports and traditional support generation
- `Geometry/` — Voronoi diagrams, medial axis, advanced geometry
- `Format/` — file I/O for 3MF, AMF, STL, OBJ, STEP
- `SLA/` — SLA-specific processing and support generation
- `Arachne/` — variable-width perimeter generation via skeletal trapezoidation

### GUI Layer (`src/slic3r/GUI/`)

wxWidgets application. Key classes bridging core and UI:
- `BackgroundSlicingProcess` — runs slicing on a background thread
- `GLCanvas3D` — main 3D viewport and interaction
- `Plater` — central workspace coordinating objects, settings, and preview
- `GCodeViewer` — G-code visualization and toolpath preview

Subdirectories:
- `Gizmos/` — interactive 3D tools (cut, rotate, scale, emboss, supports, seam, MMU segmentation, etc.), all inheriting from `GLGizmoBase`
- `Jobs/` — background task workers (arrange, emboss, fill bed, etc.)
- `Widgets/` — custom wxWidgets controls (buttons, checkboxes, dropdowns, AMS controls)

### Adding Print Settings

1. Define in `PrintConfig.cpp` with bounds and defaults
2. Add UI controls in the relevant GUI component
3. Update config serialization (save/load)
4. Add tooltip/help text

### Adding Printer Profiles

Create `resources/profiles/[manufacturer].json` following existing profile structure. Start/end G-code templates go in `resources/printers/`.

## Key External Dependencies

- **Clipper2** — 2D polygon clipping and offsetting
- **TBB** — parallelization (used throughout performance-critical paths)
- **wxWidgets** — cross-platform GUI
- **Eigen** — linear algebra
- **CGAL / libigl** — computational geometry (selective use)
- **OpenVDB** — volumetric operations

## Commit & PR Guidelines

Sentence-style subject lines with issue references: `Fix grid lines origin for multiple plates (#10724)`. Squash fixups before opening a PR. Fill out `.github/pull_request_template.md`; include screenshots for UI changes; note impacted presets or translations. Use `Closes #NNNN` for linked issues. Call out dependency bumps or profile migrations explicitly.

## Developer Reference

Longer documentation lives in `doc/developer-reference/`:
- `How-to-build.md`, `How-to-create-profiles.md`
- `slicing-hierarchy.md` — slicing pipeline walkthrough
- `Built-in-placeholders-variables.md` — G-code placeholder reference
- `Preset-and-bundle.md` — preset/bundle system
- `Localization_guide.md` — translation workflow
- `Print_Time_Estimation_Analysis.md`, `Acceleration_Correction.md`, `Klipper_vs_OrcaSlicer_Velocity_Limits.md`
