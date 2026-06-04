# Repository Guidelines

## Project Structure & Module Organization
Snapmaker_Orca’s C++17 sources live in `src/`, split by feature modules and platform adapters. User assets, icons, and printer presets are in `resources/`; translations stay in `localization/`. Tests sit in `tests/`, grouped by domain (`libslic3r/`, `sla_print/`, etc.) with fixtures under `tests/data/`. CMake helpers reside in `cmake/`, and longer references in `doc/` and `SoftFever_doc/`. Automation scripts belong in `scripts/` and `tools/`. Treat everything in `deps/` and `deps_src/` as vendored snapshots—do not modify without mirroring upstream tags.

## Build, Test, and Development Commands
Use out-of-source builds:
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` configures dependencies and generates build files.
- `cmake --build build --target Snapmaker_Orca --config Release` compiles the app; add `--parallel` to speed up.
- `cmake --build build --target tests` then `ctest --test-dir build --output-on-failure` runs automated suites.
Platform helpers such as `build_linux.sh`, `build_release_macos.sh`, and `build_release_vs2022.bat` wrap the same flow with toolchain flags. Use `build_release_macos.sh -sx` when reproducing macOS build issues, and `scripts/DockerBuild.sh` for reproducible container builds.

## Coding Style & Naming Conventions
`.clang-format` enforces 4-space indents, a 140-column limit, aligned initializers, and brace wrapping for classes and functions.
Run `clang-format -i <file>` before committing; the CMake `clang-format` target is available when LLVM tools are on your PATH.
Prefer `CamelCase` for classes, `snake_case` for functions and locals, and `SCREAMING_CASE` for constants, matching conventions in `src/`.
Use alignment sparingly: avoid padding with long runs of spaces solely to vertically align declarations, assignments, or arguments.
Keep headers self-contained and align include order with the IWYU pragmas.

## Testing Guidelines
Unit tests rely on Catch2 (`tests/catch2/`). Name specs after the component under test—for example `tests/libslic3r/TestPlanarHole.cpp`—and tag long-running cases so `ctest -L fast` remains useful. Cover new algorithms with deterministic fixtures or sample G-code stored in `tests/data/`. Document manual printer validation or regression slicer checks in your PR when automated coverage is insufficient.

## Commit & Pull Request Guidelines
The history favors concise, sentence-style subject lines with optional issue references, e.g., `Fix grid lines origin for multiple plates (#10724)`. Squash fixups locally before opening a PR. Complete `.github/pull_request_template.md`, include reproduction steps or screenshots for UI changes, and mention impacted presets or translations. Link issues via `Closes #NNNN` when applicable, and call out dependency bumps or profile migrations for maintainer review.

## Security & Configuration Tips
Follow `SECURITY.md` for vulnerability reporting. Keep API tokens and printer credentials out of tracked configs; use `sandboxes/` for experimental settings. When touching third-party code in `deps_src/`, record the upstream commit or release in your PR description and run the relevant platform build script to confirm integration.


## Overview

Snapmaker_Orca is an open-source 3D slicer application forked from Bambu Studio, built using C++ with wxWidgets for the GUI and CMake as the build system. The project uses a modular architecture with separate libraries for core slicing functionality, GUI components, and platform-specific code.


## Architecture

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
- **Naming conventions**: PascalCase for classes, snake_case for functions/variables
- **Header guards**: Use `#pragma once`
- **Memory management**: Prefer smart pointers, RAII patterns
- **Thread safety**: Use TBB for parallelization, be mindful of shared state

### Common Development Tasks

#### Adding New Print Settings
1. Define setting in `PrintConfig.cpp` with proper bounds and defaults
2. Add UI controls in appropriate GUI components
3. Update serialization in config save/load
4. Add tooltips and help text for user guidance
5. Test with different printer profiles

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
