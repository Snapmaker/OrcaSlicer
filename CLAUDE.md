# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Snapmaker_Orca is an open-source 3D slicer application forked from OrcaSlicer → Bambu Studio → PrusaSlicer → Slic3r. Built with C++17, wxWidgets GUI, and CMake. Licensed under AGPL v3.

## Build Commands

### Prerequisites
- **Windows**: Visual Studio 2019 or 2022, CMake 3.13–3.31.x, git, git-lfs, Strawberry Perl
  - Strawberry Perl's `c/bin` must appear **after** CMake in PATH
- **macOS**: Xcode, CMake, git, gettext, libtool, automake, autoconf, texinfo (`brew install cmake gettext libtool automake autoconf texinfo`)
- **Linux (Ubuntu)**: System deps auto-installed by `BuildLinux.sh -u`

### Windows
```bash
# After cloning: git lfs pull

# VS 2019 (x64 Native Tools Command Prompt)
build_release.bat              # Build everything
build_release.bat debug        # Debug build
build_release.bat deps         # Dependencies only
build_release.bat slicer       # Slicer only (deps already built)

# VS 2022
build_release_vs2022.bat       # Build everything
build_release_vs2022.bat pack  # Pack deps into zip
```

### macOS
```bash
./build_release_macos.sh           # Build everything
./build_release_macos.sh -d        # Dependencies only
./build_release_macos.sh -s        # Slicer only
./build_release_macos.sh -x        # Use Ninja (faster)
./build_release_macos.sh -a arm64  # Architecture: arm64/x86_64/universal
./build_release_macos.sh -t 11.3   # macOS deployment target
```

### Linux
```bash
sudo ./BuildLinux.sh -u            # Install system dependencies (first time)
./BuildLinux.sh -dsir              # Build deps + slicer + AppImage
./BuildLinux.sh -d                 # Dependencies only
./BuildLinux.sh -s                 # Slicer only
./BuildLinux.sh -i                 # AppImage only
./BuildLinux.sh -b                 # Debug build
./BuildLinux.sh -j N               # Limit to N cores
./BuildLinux.sh -l                 # Use Clang instead of GCC
```

### Build System Notes
- Dependencies built first in `deps/build/`, then linked to main application
- Windows uses Visual Studio generators; macOS uses Xcode by default (Ninja with `-x`); Linux uses Ninja
- Windows builds produce `Snapmaker_Orca_app_gui.exe` (output name: `snapmaker-orca.exe`) via a shim that loads `Snapmaker_Orca.dll`
- macOS builds produce `Snapmaker_Orca.app` bundle; Linux produces an AppImage
- Version is set in `version.inc` (currently 2.3.3); the build defines `GIT_COMMIT_HASH` as a compile definition

### Key CMake Options

| Option | Default | Purpose |
|---|---|---|
| `SLIC3R_GUI` | ON | Build with wxWidgets/OpenGL GUI |
| `SLIC3R_STATIC` | ON (Win/Mac), OFF (Linux) | Static linking of Boost, TBB, GLEW |
| `SLIC3R_PCH` | ON | Precompiled headers |
| `SLIC3R_SENTRY` | ON (Win/Mac), OFF (Linux) | Sentry crash reporting |
| `SLIC3R_PROFILE` | OFF | Invasive Shiny profiler |
| `SLIC3R_ASAN` | OFF | Address Sanitizer |
| `BUILD_TESTS` | OFF | Build unit tests |
| `SLIC3R_BUILD_SANDBOXES` | OFF | Build dev sandboxes |
| `ORCA_TOOLS` | OFF | Build profile validator tool |

## Testing

Tests use the Catch2 v2 framework. See `tests/CLAUDE.md` for the comprehensive testing guide.

From the `build/` directory:

```bash
ctest                              # Run all tests
ctest --output-on-failure          # Verbose failures

# Individual test binaries (run with --order rand --warn NoAssertions for best results)
./tests/libslic3r/libslic3r_tests
./tests/fff_print/fff_print_tests
./tests/sla_print/sla_print_tests
./tests/libnest2d/libnest2d_tests
./tests/slic3rutils/slic3rutils_tests

# Filter by tags or name patterns
./tests/libslic3r/libslic3r_tests "[Geometry]" --order rand
./tests/libslic3r/libslic3r_tests "*perimeter*" --order rand
```

Test fixtures live in `tests/fff_print/test_data.hpp` (`Slic3r::Test` namespace):
- `mesh(TestMesh::cube_20x20x20)` — load standard test meshes
- `init_print({meshes...}, print, config)` — set up a print with test objects
- `init_and_process_print(...)` — set up and run the full slicing pipeline
- `gcode(print)` — extract G-code output as string

Key Catch2 rules for this codebase:
- Use `DYNAMIC_SECTION("name " << i)` instead of `SECTION("name")` inside loops
- Assertions are NOT thread-safe — collect results in threads, assert on main thread
- Use `WithinAbs`/`WithinRel`/`WithinULP` matchers instead of deprecated `Approx`
- Split compound assertions: `REQUIRE(a > 0); REQUIRE(b < 10);` not `REQUIRE(a > 0 && b < 10);`

## Architecture

### Source Layout (`src/`)

Each subdirectory builds as a separate CMake target:

| Directory | Purpose |
|---|---|
| `src/libslic3r/` | Core slicing engine — geometry, G-code, algorithms. Platform-independent. |
| `src/slic3r/` | wxWidgets GUI application. Links `libslic3r` and produces `libslic3r_gui`. |
| `src/mqtt/` | MQTT client (Paho C/C++) for device communication |
| `src/sentry_wrapper/` | Sentry crash-reporting integration |
| `src/bury_cfg/` | Crash/bury-point configuration |
| `src/common_func/` | Shared utility functions |
| `src/dev-utils/` | Profile validator, encoding check, StackWalker |

### Key Entry Points

- `src/Snapmaker_Orca.cpp` — Application entry point (shared lib on Windows)
- `src/Snapmaker_Orca_app_msvc.cpp` — Windows GUI shim (WinMain, DPI awareness, WebView2 setup)
- `src/libslic3r/PrintConfig.cpp` — All print/printer/material config definitions with defaults and bounds
- `src/libslic3r/Print.cpp` — Orchestrates the slicing pipeline
- `src/slic3r/GUI/GUI_App.cpp` — Main wxWidgets application class
- `src/slic3r/GUI/GLCanvas3D.cpp` — OpenGL 3D viewport rendering

### libslic3r Subdirectories

- `Format/` — File I/O: 3MF, AMF, STL, OBJ, STEP
- `GCode/` — G-code generation, cooling, pressure equalization, thumbnails, conflict checking
- `Fill/` — Infill patterns (gyroid, honeycomb, lightning, etc.)
- `Support/` — Tree supports and traditional support generation
- `Geometry/` — Voronoi diagrams, medial axis, advanced geometry operations
- `SLA/` — SLA-specific print processing and support generation
- `Arachne/` — Variable-width perimeter generation via skeletal trapezoidation

### slic3r GUI Key Components

- `GUI/Gizmos/` — Interactive 3D manipulation tools (move, rotate, scale, cut, paint supports, seam, etc.)
- `GUI/DeviceTab/` — Device management UI (AMS humidity, firmware updates)
- `GUI/Calibration*.cpp` — Calibration wizards for pressure advance, flow rate, etc.
- `GUI/BackgroundSlicingProcess.cpp` — Background slicing thread management

## Slicing Pipeline (End-to-End)

### GUI → Background Thread Bridge

`BackgroundSlicingProcess` (in `src/slic3r/GUI/`) is the critical bridge between the wxWidgets GUI and the platform-independent `libslic3r`:

```
User clicks "Slice" in Plater
  → Plater calls background_process.start()
    → boost::thread runs thread_proc() loop
      → process_fff():
          1. m_print->process()       // slicing
          2. m_print->export_gcode()  // GCode export
      → wxQueueEvent() posts results back to UI thread
```

The process has its own state machine: `INITIAL → IDLE → STARTED → RUNNING → FINISHED` (with `CANCELED` / `EXIT` / `EXITED` transitions). Synchronization uses `std::mutex`, `std::condition_variable`, and wxWidgets events.

### Print::process() Stages

`Print::process()` in `Print.cpp` executes these stages sequentially:

1. **Object deduplication** — identical objects share slicing results via `set_shared_object()`
2. **For each unique object:**
   - `make_perimeters()` — calls `slice()` (mesh → 2D layers), then generates perimeters
   - `estimate_curled_extrusions()` — detect areas that may curl
   - `infill()` — prepare infill regions, then generate infill paths
   - `ironing()` — ironing pass
   - `generate_support_material()` — support generation (parallel across objects via TBB)
   - `detect_overhangs_for_lift()` — overhang detection for Z-hop
3. **Copy layers** from shared objects to non-unique instances
4. **psWipeTower** — wipe tower geometry and tool ordering
5. **psSkirtBrim** — skirt and brim generation
6. **Path simplification** — `simplify_extrusion_path()` for each object
7. **Conflict checking** — `ConflictChecker::find_inter_of_lines_in_diff_objs()` for multi-object collision detection

### GCode Generation Pipeline

`Print::export_gcode()` → `GCode::do_export()` → `GCode::_do_export()` uses a **TBB parallel_pipeline** with filter stages:

```
Generator (serial)    → calls process_layer() per layer to emit raw GCode
  ↓
SpiralVase (optional) → spiral vase mode filter
  ↓
PressureEqualizer     → smooths pressure advance changes
  ↓
CoolingBuffer (serial)→ manages fan speed, material cooling
  ↓
FanMover (serial)     → adjusts fan on/off timing
  ↓
PAProcessor (serial)  → applies adaptive pressure advance
  ↓
Output (serial)       → writes final GCode string to file
```

Pipeline depth is 12. Different assemblies are used depending on whether spiral vase and/or pressure equalizer are active.

### Cancelation Protocol

- `PrintBase` provides `canceled()` and `throw_if_canceled()` checked at many points during slicing
- When the UI requests cancelation, `stop_internal()` sets a cancel flag and waits for the thread
- The slicing thread periodically calls `throw_if_canceled()` which throws `CanceledException`

## Config System

### Option Types

Defined in `PrintConfig.hpp`. Options use a type+vector encoding: base types (coFloat, coInt, coString, coBool, coEnum, coPercent, coPoint, coPoint3) each have a corresponding vector variant (coFloats, coInts, etc.) at `base_type + 0x4000`.

### Adding a New Config Option

In `PrintConfig.cpp`, options are registered in `PrintConfigDef` constructor methods (`init_common_params()`, `init_fff_params()`, `init_sla_params()`):

```cpp
ConfigOptionDef* def;
def = this->add("option_name", coFloat);
def->label = L("Human-readable name");
def->category = L("Category");
def->tooltip = L("Description shown in tooltip");
def->sidetext = "mm";
def->min = 0;
def->max = 100;
def->mode = comAdvanced;  // comSimple | comAdvanced | comDevelop
def->set_default_value(new ConfigOptionFloat(0.2));
```

For enums, use the `CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS` macro with a `t_config_enum_values` map.

### Config Class Hierarchy

```
DynamicPrintConfig      — runtime-extensible, used at the UI level
  StaticPrintConfig     — compile-time typed configs for slicing
    GCodeConfig         — GCode generation settings
      MachineEnvelopeConfig — machine limits
        PrintConfig     — full print settings
    PrintObjectConfig   — per-object settings
    PrintRegionConfig   — per-region settings
      FullPrintConfig   — combines all three (passed during slicing)
```

### Validation and Invalidation

- `PrintConfigDef::validate()` checks cross-option constraints, returns `StringObjectException` with per-object error messages
- Config changes call `invalidate_state_by_config_options()` which marks affected print steps stale, triggering recomputation

## External Dependencies

Clipper2 (polygon clipping), libigl (mesh ops), TBB (parallelization), wxWidgets (GUI), OpenGL (rendering), CGAL (computational geometry, selective), OpenVDB (volumetric), Eigen (linear algebra).

**Dependency management:** Prebuilt binaries for Windows live in `deps/` (GMP, MPFR, OCCT, etc.). Source-built dependencies live in `deps_src/` and are compiled via CMake `ExternalProject_Add` using the `Snapmaker_Orca_add_cmake_project` wrapper. Key source-built deps: admesh, agg, clipper, eigen, imgui/imguizmo, libigl, libnest2d, mcut, miniz, nanosvg, nlohmann, qhull.

## Snapmaker-Specific Additions vs Upstream

- **MQTT** (`src/mqtt/`): Printer device communication over MQTT
- **Sentry** (`src/sentry_wrapper/`): Crash reporting with `SLIC3R_SENTRY` compile flag
- **Bury CFG** (`src/bury_cfg/`): Crash/bury-point tracking
- **Common func** (`src/common_func/`): Shared helpers
- **Profile validator** (`src/dev-utils/`): Profile validation tool

## Internationalization

- Source translations in `localization/i18n/` (.pot/.po files)
- Runtime language resources in `resources/i18n/`
- Update translations: `scripts/run_gettext.sh` (Unix) or `scripts/run_gettext.bat` (Windows)
- `scripts/HintsToPot.py` extracts translatable strings from `resources/data/hints.ini`

## Code Style

- C++17 standard (C++20 features selectively)
- PascalCase for classes, snake_case for functions/variables
- `#pragma once` for header guards
- Smart pointers / RAII for memory management
- TBB for parallelization
