# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Snapmaker Orca is an open-source FDM 3D slicer forked from OrcaSlicer (which forked from Bambu Studio → PrusaSlicer → Slic3r). Built in C++17 with wxWidgets for the GUI and CMake as the build system. The codebase is 500k+ lines — use search tools extensively rather than browsing.

See also: `AGENTS.md` (repo guidelines), `doc/developer-reference/` (slicing hierarchy, build details, setting docs), `README.md` (end-user build/install), `SECURITY.md` (vulnerability reporting).

## Build Commands

### Building on Windows
```bash
# Build everything
build_release_vs2022.bat

# Build with debug symbols
build_release_vs2022.bat debug

# Build only dependencies
build_release_vs2022.bat deps

# Build only slicer (after deps are built)
build_release_vs2022.bat slicer


```

### Building on macOS
```bash
# Build everything (dependencies and slicer)
./build_release_macos.sh

# Build only dependencies
./build_release_macos.sh -d

# Build only slicer (after deps are built)
./build_release_macos.sh -s

# Use Ninja generator for faster builds
./build_release_macos.sh -x

# Build for specific architecture
./build_release_macos.sh -a arm64    # or x86_64 or universal

# Build for specific macOS version target
./build_release_macos.sh -t 11.3
```

### Building on Linux
```bash
# First time setup - install system dependencies
./build_linux.sh -u

# Build dependencies and slicer
./build_linux.sh -dsi

# Build everything (alternative)
./build_linux.sh -dsi

# Individual options:
./build_linux.sh -d    # dependencies only
./build_linux.sh -s    # slicer only  
./build_linux.sh -i    # build AppImage

# Performance and debug options:
./build_linux.sh -j N  # limit to N cores
./build_linux.sh -1    # single core build
./build_linux.sh -b    # debug build
./build_linux.sh -c    # clean build
./build_linux.sh -r    # skip RAM/disk checks
./build_linux.sh -l    # use Clang instead of GCC
```

### Build System
- Uses CMake. **Windows requires CMake 3.31.x exactly** (mandatory); macOS/Linux minimum 3.13.
- Primary build directory: `build/`
- Dependencies are built in `deps/build/` (vendored snapshots in `deps/`, `deps_src/` — do not modify without recording upstream tag in the PR).
- The build process is split into dependency building and main application building.
- Windows builds use Visual Studio generators; macOS uses Xcode by default (Ninja with -x); Linux uses Ninja.

### Direct CMake (alternative to platform scripts)
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Snapmaker_Orca --config Release --parallel
cmake --build build --target tests
ctest --test-dir build --output-on-failure
```

### Testing
Tests are located in the `tests/` directory and use the Catch2 testing framework. Test structure:
- `tests/libslic3r/` - Core library tests (21 test files)
  - Geometry processing, algorithms, file formats (STL, 3MF, AMF)
  - Polygon operations, clipper utilities, Voronoi diagrams
- `tests/fff_print/` - Fused Filament Fabrication tests (12 test files)
  - Slicing algorithms, G-code generation, print mechanics
  - Fill patterns, extrusion, support material
- `tests/sla_print/` - Stereolithography tests (4 test files)
  - SLA-specific printing algorithms, support generation
- `tests/libnest2d/` - 2D nesting algorithm tests
- `tests/slic3rutils/` - Utility function tests
- `tests/sandboxes/` - Experimental/sandbox test code

Run all tests after building:
```bash
cd build && ctest
```

Run tests with verbose output:
```bash
cd build && ctest --output-on-failure
```

Run individual test suites:
```bash
# From build directory
./tests/libslic3r/libslic3r_tests
./tests/fff_print/fff_print_tests
./tests/sla_print/sla_print_tests
```

Run a single test case (Catch2 syntax):
```bash
./tests/libslic3r/libslic3r_tests "TestName"           # by name
./tests/libslic3r/libslic3r_tests -L fast              # only fast-tagged specs
```
Tag long-running cases so `ctest -L fast` stays useful. Test fixtures and sample G-code live in `tests/data/`.

## Architecture

### Slicing pipeline (start here for navigation)
Slicing logic is hard to locate by browsing. The full call flow from UI click to algorithm is diagrammed in `doc/developer-reference/slicing-hierarchy.md`. The spine is:

`Plater::priv::on_action_slice_plate` → `Plater::reslice` → `BackgroundSlicingProcess::thread_proc` → `Print::process` → `PrintObject::slice` / `PrintObject::make_perimeters`

Most slicing runs on background threads — start from `BackgroundSlicingProcess::start()` when tracing. `libslic3r/Print.cpp` orchestrates the FFF pipeline; `PrintConfig.cpp` defines every print/printer/material setting (the source of truth for option names, defaults, bounds).

### Core Libraries
- **libslic3r/**: Core slicing engine and algorithms (platform-independent)
  - Main slicing logic, geometry processing, G-code generation
  - Key classes: Print, PrintObject, Layer, GCode, Config
  - Modular design with specialized subdirectories:
    - `GCode/` - G-code generation, cooling, pressure equalization, thumbnails
    - `Fill/` - Infill pattern implementations (gyroid, honeycomb, lightning, etc.)
    - `Support/` - Tree supports and traditional support generation
    - `Geometry/` - Advanced geometry operations, Voronoi diagrams, medial axis
    - `Format/` - File I/O for 3MF, AMF, STL, OBJ, STEP formats
    - `SLA/` - SLA-specific print processing and support generation
    - `Arachne/` - Advanced wall generation using skeletal trapezoidation

- **src/slic3r/**: Main application framework and GUI
  - GUI application built with wxWidgets
  - Integration between libslic3r core and user interface
  - Located in `src/slic3r/GUI/` (not shown in this directory but exists)

### Key Algorithmic Components
- **Arachne Wall Generation**: Variable-width perimeter generation using skeletal trapezoidation
- **Tree Supports**: Organic support generation algorithm  
- **Lightning Infill**: Sparse infill optimization for internal structures
- **Adaptive Slicing**: Variable layer height based on geometry
- **Multi-material**: Multi-extruder and soluble support processing
- **G-code Post-processing**: Cooling, fan control, pressure advance, conflict checking

### File Format Support
- **3MF/BBS_3MF**: Native format with extensions for multi-material and metadata
- **STL**: Standard tessellation language for 3D models
- **AMF**: Additive Manufacturing Format with color/material support  
- **OBJ**: Wavefront OBJ with material definitions
- **STEP**: CAD format support for precise geometry
- **G-code**: Output format with extensive post-processing capabilities

### External Dependencies
- **Clipper2**: Advanced 2D polygon clipping and offsetting
- **libigl**: Computational geometry library for mesh operations
- **TBB**: Intel Threading Building Blocks for parallelization
- **wxWidgets**: Cross-platform GUI framework
- **OpenGL**: 3D graphics rendering and visualization
- **CGAL**: Computational Geometry Algorithms Library (selective use)
- **OpenVDB**: Volumetric data structures for advanced operations
- **Eigen**: Linear algebra library for mathematical operations

## File Organization

### Resources and Configuration
- `resources/profiles/` - Printer and material profiles organized by manufacturer
- `resources/printers/` - Printer-specific configurations and G-code templates  
- `resources/images/` - UI icons, logos, calibration images
- `resources/calib/` - Calibration test patterns and data
- `resources/handy_models/` - Built-in test models (benchy, calibration cubes)

### Internationalization and Localization  
- `localization/i18n/` - Source translation files (.pot, .po)
- `resources/i18n/` - Runtime language resources
- Translation managed via `scripts/run_gettext.sh` / `scripts/run_gettext.bat`

### Platform-Specific Code
- `src/libslic3r/Platform.cpp` - Platform abstractions and utilities
- `src/libslic3r/MacUtils.mm` - macOS-specific utilities (Objective-C++)
- Windows-specific build scripts and configurations
- Linux distribution support scripts in `scripts/linux.d/`

### Build and Development Tools
- `cmake/modules/` - Custom CMake find modules and utilities
- `scripts/` - Python utilities for profile generation and validation  
- `tools/` - Windows build tools (gettext utilities)
- `deps/` - External dependency build configurations

## Development Workflow

### Code Style and Standards
- **C++17 standard** with selective C++20 features
- **`.clang-format` is enforced**: 4-space indent, 140-column limit, aligned initializers, brace wrapping. Run `clang-format -i <file>` before committing (CMake `clang-format` target available when LLVM is on PATH).
- **Naming**: `CamelCase` classes, `snake_case` functions/locals, `SCREAMING_CASE` constants (matches AGENTS.md; note existing code mixes this with PascalCase classes — follow nearby code).
- **Header guards**: `#pragma once`
- **Memory**: smart pointers, RAII
- **Threading**: TBB for parallelization; be mindful of shared state across background slicing threads.

### Common Development Tasks

#### Adding New Print Settings
1. Define setting in `src/libslic3r/PrintConfig.cpp` (the source of truth — option name, type, default, bounds, tooltip key).
2. Add UI controls in the appropriate `src/slic3r/GUI/` component (typically an OG_Settings or Tab subclass).
3. Serialization in config save/load is driven by the PrintConfig definition — rarely needs manual work.
4. Document the setting under `doc/print_settings/` (organized by category: quality, strength, speed, support, multimaterial, others).
5. Test with different printer profiles.

#### Modifying Slicing Algorithms  
1. Core algorithms live in `libslic3r/` subdirectories
2. Performance-critical code should be profiled and optimized
3. Consider multi-threading implications (TBB integration)
4. Validate changes don't break existing profiles
5. Add regression tests where appropriate

#### GUI Development
1. GUI code resides in `src/slic3r/GUI/` (not visible in current tree)
2. Use existing wxWidgets patterns and custom controls
3. Support both light and dark themes
4. Consider DPI scaling on high-resolution displays
5. Maintain cross-platform compatibility

#### Adding Printer Support
1. Create JSON profile in `resources/profiles/[manufacturer].json`
2. Add printer-specific start/end G-code templates
3. Configure build volume, capabilities, and material compatibility
4. Test thoroughly with actual hardware when possible
5. Follow existing profile structure and naming conventions

### Dependencies and Build System
- **CMake-based** with separate dependency building phase
- **Dependencies** built once in `deps/build/`, then linked to main application  
- **Cross-platform** considerations important for all changes
- **Resource files** embedded at build time, platform-specific handling

### Performance Considerations
- **Slicing algorithms** are CPU-intensive, profile before optimizing
- **Memory usage** can be substantial with complex models
- **Multi-threading** extensively used via TBB
- **File I/O** optimized for large 3MF files with embedded textures
- **Real-time preview** requires efficient mesh processing

## Important Development Notes

### Codebase Navigation
- Use search tools extensively - codebase has 500k+ lines
- Key entry points: `src/Snapmaker_Orca.cpp` for application startup
- Core slicing: `libslic3r/Print.cpp` orchestrates the slicing pipeline
- Configuration: `PrintConfig.cpp` defines all print/printer/material settings

### Compatibility and Stability
- **Backward compatibility** maintained for project files and profiles
- **Cross-platform** support essential (Windows/macOS/Linux)  
- **File format** changes require careful version handling
- **Profile migrations** needed when settings change significantly

### Quality and Testing
- **Regression testing** important due to algorithm complexity
- **Performance benchmarks** help catch performance regressions
- **Memory leak** detection important for long-running GUI application
- **Cross-platform** testing required before releases

### Commit & PR Conventions
- Sentence-style subject lines with optional issue ref, e.g. `Fix grid lines origin for multiple plates (#10724)`. Squash fixups locally before opening a PR.
- Complete `.github/pull_request_template.md`. Include reproduction steps or screenshots for UI changes; mention impacted presets or translations.
- Link issues via `Closes #NNNN`. Call out dependency bumps or profile migrations explicitly for maintainer review.
- Do not commit API tokens or printer credentials; use `sandboxes/` for experimental settings.