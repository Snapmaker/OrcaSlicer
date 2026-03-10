# OrcaSlicer Cloud Slicing Engine: Technical Guide

**Version:** 01.10.01.50
**Branch:** engine-build
**Last Updated:** 2026-03-10

---

## Table of Contents

1. [Overview](#1-overview)
2. [Quick Start](#2-quick-start)
3. [Command-Line Interface](#3-command-line-interface)
4. [Core Slicing Pipeline](#4-core-slicing-pipeline)
5. [Plate Layout System](#5-plate-layout-system)
6. [Build System](#6-build-system)
7. [Engine vs GUI Comparison](#7-engine-vs-gui-comparison)
8. [API Integration Guide](#8-api-integration-guide)
9. [Troubleshooting](#9-troubleshooting)
10. [Development Guide](#10-development-guide)

---

## 1. Overview

### 1.1 Introduction

`orca-slice-engine` is a headless command-line slicing tool extracted from the OrcaSlicer project. It accepts a 3MF file containing 3D models and embedded slicing configuration, runs the full OrcaSlicer slicing pipeline, and produces G-code output without requiring any graphical environment.

The engine was designed for server-side and cloud deployment scenarios where:
- No display or desktop environment is available
- Slicing must be triggered programmatically via REST API or job queue
- Reproducible, deterministic G-code output is required
- Multiple slicing jobs must run concurrently in containerized environments

### 1.2 Design Goals

| Goal | Description |
|------|-------------|
| Headless operation | No GUI, no display server, no OpenGL context required |
| Configuration-embedded | All slicing settings are read from the input 3MF file |
| Multi-plate support | Slice a single plate by ID or all plates in one pass |
| GUI parity | G-code output is mathematically identical to GUI output |
| Cross-platform | Builds and runs on Linux x64, macOS, and Windows |
| Minimal footprint | No wxWidgets, no OpenGL; only libslic3r and Boost |

### 1.3 Relationship with the GUI Application

The engine and GUI application share the same core slicing library (`libslic3r`). All slicing algorithms, G-code generation logic, and configuration handling are identical. The engine is not a reimplementation — it is a thin command-line wrapper around the same `Print::process()` and `Print::export_gcode()` calls that the GUI uses.

```
┌─────────────────────────────────────────────────────────────┐
│                    OrcaSlicer Project                       │
│                                                             │
│  ┌─────────────────────┐    ┌──────────────────────────┐   │
│  │  GUI Application    │    │  Cloud Slicing Engine     │   │
│  │  (Snapmaker_Orca)   │    │  (orca-slice-engine)      │   │
│  │                     │    │                           │   │
│  │  wxWidgets          │    │  CLI argument parser      │   │
│  │  OpenGL rendering   │    │  Progress reporter        │   │
│  │  Interactive UI     │    │  3MF packager             │   │
│  └──────────┬──────────┘    └────────────┬──────────────┘   │
│             │                            │                  │
│             └──────────┬─────────────────┘                  │
│                        ▼                                    │
│           ┌────────────────────────┐                        │
│           │       libslic3r        │                        │
│           │  (shared core library) │                        │
│           │                        │                        │
│           │  Print::process()      │                        │
│           │  Print::export_gcode() │                        │
│           │  Model::read_from_file │                        │
│           │  store_bbs_3mf()       │                        │
│           └────────────────────────┘                        │
└─────────────────────────────────────────────────────────────┘
```

**Key point:** Any slicing algorithm change or bug fix in `libslic3r` automatically applies to both the GUI and the engine.

---

## 2. Quick Start

### 2.1 Prerequisites

| Component | Requirement |
|-----------|-------------|
| CMake | 3.13 or later (max 3.31.x on Windows) |
| C++ Compiler | C++17 capable (GCC 7+, Clang 5+, MSVC 2019+) |
| Build tool | Ninja (recommended) or Make |
| OS | Linux (Ubuntu 20.04+), macOS 12+, Windows 10+ |

### 2.2 Building from Source

#### Step 1: Clone the repository

```bash
git clone https://github.com/Snapmaker/OrcaSlicer.git
cd OrcaSlicer
git checkout engine-build
```

#### Step 2: Build dependencies (without GUI)

**Linux:**
```bash
mkdir -p deps/build
cmake -S deps -B deps/build -G Ninja \
  -DFLATPAK=ON \
  -DDESTDIR="$(pwd)/deps/build/destdir"
cmake --build deps/build
```

**macOS:**
```bash
mkdir -p deps/build
cmake -S deps -B deps/build \
  -DFLATPAK=ON \
  -DDESTDIR="$(pwd)/deps/build/destdir"
cmake --build deps/build
```

**Windows (Visual Studio 2022):**
```bash
mkdir deps\build
cmake -S deps -B deps\build -G "Visual Studio 17 2022" -A x64 ^
  -DDESTDIR="%CD%\deps\build\destdir"
cmake --build deps\build --config Release
```

#### Step 3: Configure and build the engine

```bash
# Linux / macOS
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$(pwd)/deps/build/destdir" \
  -DSLIC3R_STATIC=1 \
  -DSLIC3R_GUI=OFF \
  -DORCA_TOOLS=OFF \
  -DBUILD_TESTS=OFF

cmake --build build --target orca-slice-engine -j$(nproc)
```

```bash
# Windows
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_PREFIX_PATH="%CD%\deps\build\destdir" ^
  -DSLIC3R_STATIC=1 ^
  -DSLIC3R_GUI=OFF
cmake --build build --target orca-slice-engine --config Release
```

The compiled binary is placed at:
- Linux/macOS: `build/src/orca-slice-engine/orca-slice-engine`
- Windows: `build\src\orca-slice-engine\Release\orca-slice-engine.exe`

### 2.3 Basic Usage Examples

```bash
# Slice all plates in a project → outputs model.gcode.3mf
./orca-slice-engine model.3mf

# Slice plate 1 only → outputs model-p1.gcode.3mf
./orca-slice-engine model.3mf -p 1

# Slice plate 1 as plain G-code → outputs model-p1.gcode
./orca-slice-engine model.3mf -p 1 -f gcode

# Specify custom output path → outputs /tmp/result.gcode.3mf
./orca-slice-engine model.3mf -o /tmp/result

# Use a specific resources directory
./orca-slice-engine model.3mf -r /opt/orca/resources

# Enable verbose logging for debugging
./orca-slice-engine model.3mf -v
```

### 2.4 Docker Deployment Example

**Dockerfile:**
```dockerfile
FROM ubuntu:22.04

# Install minimum runtime dependencies
RUN apt-get update && apt-get install -y \
    libgl1-mesa-glx \
    libglib2.0-0 \
    && rm -rf /var/lib/apt/lists/*

# Copy engine and its bundled libraries
COPY orca-slice-engine /usr/local/bin/
COPY lib/ /usr/local/lib/
COPY resources/ /opt/orca/resources/

# Set library path and resources
ENV ORCA_RESOURCES=/opt/orca/resources
ENV LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH

ENTRYPOINT ["orca-slice-engine"]
```

**Build and run:**
```bash
# Build the Docker image
docker build -t orca-slice-engine .

# Slice all plates
docker run --rm \
  -v "$(pwd)/models:/data" \
  orca-slice-engine /data/model.3mf -o /data/output

# Slice a single plate
docker run --rm \
  -v "$(pwd)/models:/data" \
  orca-slice-engine /data/model.3mf -p 1 -o /data/output
```

---

## 3. Command-Line Interface

### 3.1 Synopsis

```
orca-slice-engine input.3mf [OPTIONS]
```

### 3.2 Options Reference

| Option | Long Form | Argument | Default | Description |
|--------|-----------|----------|---------|-------------|
| `-o` | `--output` | `<path>` | (derived from input) | Output file base path, without extension |
| `-p` | `--plate` | `<id\|all>` | `all` | Plate ID to slice (1, 2, 3...) or `all` |
| `-f` | `--format` | `<fmt>` | `gcode.3mf` | Output format: `gcode` or `gcode.3mf` |
| `-r` | `--resources` | `<dir>` | Auto-detected | Path to resources directory |
| `-t` | `--timestamp` | `<ts>` | System time | Fixed timestamp for reproducible output |
| `-v` | `--verbose` | — | off | Enable trace-level logging |
| `-h` | `--help` | — | — | Show help message and exit |

### 3.3 Option Details

#### `-o, --output <path>`

Specifies the output file path base. The appropriate extension (`.gcode` or `.gcode.3mf`) is appended automatically based on the format and plate selection.

When omitted, the output file is placed in the same directory as the input file, using the input file's stem as the base name.

```bash
# Input: /data/my_model.3mf
# Default output for plate 1: /data/my_model-p1.gcode.3mf
./orca-slice-engine /data/my_model.3mf -p 1

# Explicit output: /output/result.gcode.3mf
./orca-slice-engine /data/my_model.3mf -p 1 -o /output/result
```

#### `-p, --plate <id>`

Selects which plate to slice. Use a positive integer (1, 2, 3...) for a specific plate, or `all` (or omit the option entirely) to slice all plates.

The engine validates that the requested plate ID exists in the 3MF file. If it does not exist, it lists all available plate IDs and exits with code 1.

```bash
./orca-slice-engine model.3mf -p 1      # Plate 1 only
./orca-slice-engine model.3mf -p all    # All plates (explicit)
./orca-slice-engine model.3mf           # All plates (default)
```

#### `-f, --format <fmt>`

Two formats are supported:

| Format | Description | Use Case |
|--------|-------------|----------|
| `gcode` | Plain-text G-code file | Single plate; direct firmware upload |
| `gcode.3mf` | 3MF container with embedded G-code | Multi-plate; metadata preservation; Snapmaker workflow |

**Constraint:** When slicing all plates (`-p all` or omitting `-p`), the format is forced to `gcode.3mf` regardless of `-f`. Specifying `-f gcode` with all-plate slicing is an error.

#### `-r, --resources <dir>`

The engine requires printer and material profiles from the `resources/` directory. Resolution order:

1. `-r` command-line argument (highest priority)
2. `ORCA_RESOURCES` environment variable
3. `resources/` subdirectory next to the executable (auto-detect)

The engine checks for `profiles/` and `printers/` subdirectories when validating the resources path.

#### `-t, --timestamp <ts>`

Sets a fixed timestamp embedded in the G-code header, overriding the system clock. This enables reproducible, deterministic G-code output useful for testing and CI validation.

Format: `YYYY-MM-DD HH:MM:SS`

```bash
./orca-slice-engine model.3mf -t "2024-01-01 12:00:00"
```

### 3.4 Output File Naming Rules

| Command | Output File |
|---------|-------------|
| `engine model.3mf` | `model.gcode.3mf` |
| `engine model.3mf -p 1` | `model-p1.gcode.3mf` |
| `engine model.3mf -p 1 -f gcode` | `model-p1.gcode` |
| `engine model.3mf -p 1 -o /out/r` | `/out/r.gcode.3mf` |
| `engine model.3mf -p 1 -o /out/r -f gcode` | `/out/r.gcode` |
| `engine model.3mf -o /out/r` | `/out/r.gcode.3mf` |

### 3.5 Exit Codes

| Code | Constant | Meaning |
|------|----------|---------|
| `0` | `EXIT_OK` | Success — output file(s) written |
| `1` | `EXIT_INVALID_ARGS` | Invalid arguments (bad plate ID, conflicting format, unknown option) |
| `2` | `EXIT_FILE_NOT_FOUND` | Input file does not exist |
| `3` | `EXIT_LOAD_ERROR` | Failed to parse or load the 3MF file |
| `4` | `EXIT_SLICING_ERROR` | Slicing failed for at least one plate |
| `5` | `EXIT_EXPORT_ERROR` | Failed to write the output G-code or package |

Exit code `4` aborts immediately on the first plate that fails. Temporary files for successfully processed plates are deleted before exit.

### 3.6 Progress Output

The engine writes structured progress messages to standard output during slicing:

```
[Progress] 25% - Processing layers
[Status] Generating support material
[Progress] 50% - Generating G-code
[Progress] 100% - Done
```

Format:
- `[Progress] <N>% - <text>` — quantified progress with percentage
- `[Status] <text>` — qualitative status update without percentage

When slicing multiple plates, each plate is preceded by a separator:

```
=== Processing plate 1 ===
[Progress] 10% - ...
=== Processing plate 2 ===
[Progress] 10% - ...
```

Log messages (from Boost.Log) go to standard output with severity prefix: `[info]`, `[warning]`, `[error]`, `[debug]` (verbose mode only).

---

## 4. Core Slicing Pipeline

The engine executes a linear pipeline of seven stages for each invocation. For multi-plate jobs, stages 4-6 repeat for each plate.

```
┌─────────────────────────────────────────────────────────────────┐
│  Stage 1: Argument Parsing                                      │
│  Validate CLI args, resolve paths, select output format         │
└───────────────────────────────┬─────────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  Stage 2: 3MF Loading                                           │
│  Model::read_from_file() — load geometry + embedded config      │
└───────────────────────────────┬─────────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  Stage 3: Plate Preparation                                     │
│  Identify plates, validate plate IDs, compute plate origins     │
└───────────────────────────────┬─────────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  Stage 4: Slicing  [per plate]                                  │
│  Set printable flags, Print::apply(), Print::process()          │
└───────────────────────────────┬─────────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  Stage 5: G-code Export  [per plate]                            │
│  Print::export_gcode() — generate .gcode to temp file           │
└───────────────────────────────┬─────────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  Stage 6: 3MF Packaging  (gcode.3mf format only)               │
│  store_bbs_3mf() — pack all G-codes into output container       │
└───────────────────────────────┬─────────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  Stage 7: Cleanup                                               │
│  Delete temp .gcode files, exit with quick_exit(0)             │
└─────────────────────────────────────────────────────────────────┘
```

### 4.1 Stage 1: Argument Parsing (main.cpp:133-234)

All command-line arguments are parsed in a single pass. Validation checks include:

- Input file presence and existence on disk
- Plate ID: must be a positive integer or `"all"`
- Format/plate combination: `gcode` format is incompatible with all-plate slicing
- Unknown options: trigger usage message and exit code 1

After argument parsing, the format is auto-corrected: if no plate is specified and the format was not explicitly set, `gcode.3mf` is selected.

### 4.2 Stage 2: 3MF Loading (main.cpp:299-373)

The engine calls `Model::read_from_file()` with the following strategy flags:

```cpp
LoadStrategy strategy = LoadStrategy::LoadModel    // Load 3D mesh geometry
                      | LoadStrategy::LoadConfig   // Load embedded slicing config
                      | LoadStrategy::AddDefaultInstances  // Ensure instances exist
                      | LoadStrategy::LoadAuxiliary;       // Load auxiliary data
```

This call populates:
- `model` — all `ModelObject` instances with mesh data
- `config` — `DynamicPrintConfig` with all slicing settings
- `plate_data` — vector of `PlateData*` describing per-plate object assignments
- `project_presets` — presets embedded in the 3MF
- `file_version` — `Semver` of the originating OrcaSlicer version

**Fallback behavior:** If the 3MF contains no plate data (e.g., a simple non-BBL 3MF), the engine creates a single synthetic `PlateData` entry with `plate_index = 1` to represent the entire model as one plate.

### 4.3 Stage 3: Plate Preparation (main.cpp:375-519)

This stage handles plate selection and origin computation.

**Plate selection:** A vector `plates_to_process` is populated with the IDs of plates to slice. For single-plate mode, it contains one element. For all-plates mode, it is populated from every entry in `plate_data`.

**Plate origin calculation:** For each plate to be processed, the engine computes a 3D origin vector using the same formula as the GUI's `PartPlateList::compute_shape_position()`. See [Section 5](#5-plate-layout-system) for the full algorithm.

**Printable instance filtering:** For each plate, the engine marks only the instances belonging to that plate as printable. This is done by matching `ModelInstance::loaded_id` against the `identify_id` values stored in `PlateData::obj_inst_map`:

```cpp
inst->printable = on_current_plate;
inst->print_volume_state = on_current_plate
    ? ModelInstancePVS_Inside
    : ModelInstancePVS_Fully_Outside;
```

### 4.4 Stage 4: Slicing (main.cpp:444-556)

A new `Print` object is created per plate. It is configured with:
- `print.set_plate_index(plate_index)` — 0-based plate index
- `print.set_plate_origin(plate_origin)` — computed 3D origin
- `print.set_gcode_timestamp(custom_timestamp)` — optional fixed timestamp

Then the slicing pipeline runs:

```cpp
// Apply configuration and model to print
auto apply_status = print.apply(model, config);

// Execute full slicing pipeline
print.process();
```

`Print::process()` in `libslic3r/Print.cpp` orchestrates all slicing steps: layer generation, perimeter generation, infill, support material, cooling, and G-code sequence planning.

**Error handling:** Any `std::exception` thrown by `process()` causes immediate cleanup of all temporary files and exit with code `EXIT_SLICING_ERROR` (4).

### 4.5 Stage 5: G-code Export (main.cpp:560-598)

After slicing, G-code is exported via:

```cpp
std::string result = print.export_gcode(gcode_output, &slice_result.gcode_result, nullptr);
```

For `gcode.3mf` format (or multi-plate), the G-code is written to a temporary file in the system's temp directory:

```
/tmp/plate_<id>.gcode
```

For single-plate `gcode` format, the G-code is written directly to the final output path.

After export, the engine collects from the `Print` object:
- `PrintStatistics::total_weight` — filament weight in grams
- `Print::is_support_used()` — whether any supports were generated
- `GCodeProcessorResult` — print time estimates, thumbnail data, timelapse flags

### 4.6 Stage 6: 3MF Packaging (main.cpp:601-791)

When output format is `gcode.3mf`, all per-plate G-code files are assembled into a single 3MF container using `store_bbs_3mf()`.

The engine prepopulates each `PlateData` entry with slicing metadata before packaging:

| Field | Source |
|-------|--------|
| `gcode_file` | Path to temp .gcode file |
| `is_sliced_valid` | `true` |
| `printer_model_id` | From config `printer_model` |
| `nozzle_diameters` | From config `nozzle_diameter` |
| `gcode_prediction` | Print time in seconds from `GCodeProcessorResult` |
| `gcode_weight` | Filament weight from `PrintStatistics` |
| `toolpath_outside` | From `GCodeProcessorResult` |
| `timelapse_warning_code` | From `GCodeProcessorResult` |
| `is_support_used` | From `Print::is_support_used()` |
| `is_label_object_enabled` | From `GCodeProcessorResult` |
| `slice_filaments_info` | Parsed via `pd->parse_filament_info()` |

The save strategy flags match GUI behavior:

```cpp
params.strategy = SaveStrategy::Silence      // Suppress verbose output
                | SaveStrategy::SplitModel   // Match GUI export behavior
                | SaveStrategy::WithGcode    // Include G-code files
                | SaveStrategy::SkipModel    // Reduce size (no mesh data)
                | SaveStrategy::Zip64;       // Support large files (>4 GB)
```

**Thumbnail handling:** The engine passes empty thumbnail data vectors. The GUI generates thumbnails via OpenGL rendering, which is not available in headless mode. The resulting `gcode.3mf` has all metadata but no preview images.

### 4.7 Stage 7: Cleanup (main.cpp:796-809)

All temporary `.gcode` files tracked in the `temp_files` vector are deleted. The process then terminates via `std::quick_exit(EXIT_OK)`, bypassing potentially unstable global destructors in third-party libraries.

---

## 5. Plate Layout System

### 5.1 Concept

The OrcaSlicer plate system arranges multiple print beds in a virtual 2D grid. Each plate occupies a rectangular region, with 20% gaps between adjacent plates. Every model instance's world coordinates are defined relative to its plate's origin within this grid.

The G-code coordinate system is absolute, so the engine must compute each plate's origin and apply it during G-code generation. The engine's plate layout logic is mathematically identical to the GUI's `PartPlateList`.

### 5.2 Grid Layout Algorithm

Plates are arranged in a near-square grid. The column count is computed from the total number of plates in the project:

```cpp
inline int compute_colum_count(int count) {
    float value = sqrt((float)count);
    float round_value = round(value);
    return (value > round_value) ? (round_value + 1) : round_value;
}
```

Column count examples:

| Total Plates | Column Count | Layout |
|--------------|--------------|--------|
| 1 | 1 | 1×1 |
| 2 | 2 | 1×2 |
| 3 | 2 | 2×2 (last cell empty) |
| 4 | 2 | 2×2 |
| 5 | 3 | 2×3 (last cell empty) |
| 6 | 3 | 2×3 |
| 9 | 3 | 3×3 |

Visual layout for a 3-plate project:

```
┌──────────────┬──────────────┐
│              │              │
│   Plate 0   │   Plate 1   │  Row 0
│  (index=0)  │  (index=1)  │
│              │              │
├──────────────┼──────────────┤
│              │              │
│   Plate 2   │   (empty)   │  Row 1
│  (index=2)  │             │
│              │              │
└──────────────┴──────────────┘
    Col 0            Col 1

  plate_stride_x ─────────────▶
```

### 5.3 Plate Origin Calculation

Given a plate's 0-based index, the origin is computed as follows:

**Step 1: Determine plate dimensions from config**

```cpp
// Read bounding box from printable_area config
BoundingBoxf bbox;
for (const Vec2d& pt : printable_area_opt->values) {
    bbox.merge(pt);
}

// Subtract bed axis tip radius to match GUI
const double BED_AXES_TIP_RADIUS = 2.5 * 0.5;  // = 1.25 mm
double plate_width = bbox.size().x() - BED_AXES_TIP_RADIUS;
double plate_depth = bbox.size().y() - BED_AXES_TIP_RADIUS;
```

**Step 2: Compute stride with 20% gap**

```cpp
const double LOGICAL_PART_PLATE_GAP = 0.2;
double plate_stride_x = plate_width  * (1.0 + LOGICAL_PART_PLATE_GAP);
double plate_stride_y = plate_depth  * (1.0 + LOGICAL_PART_PLATE_GAP);
```

**Step 3: Map index to row/column**

```cpp
// CRITICAL: Use total plate count from 3MF, not plates being processed
const int plate_cols = compute_colum_count(static_cast<int>(plate_data.size()));
int row = plate_index / plate_cols;
int col = plate_index % plate_cols;
```

**Step 4: Compute origin vector**

```cpp
Vec3d plate_origin(
    col * plate_stride_x,    // X: positive for rightward columns
    -row * plate_stride_y,   // Y: negative for downward rows
    0                        // Z: always zero
);
```

**Example (3 plates, 270×270 mm bed):**

| Plate Index | Row | Col | Origin X | Origin Y |
|-------------|-----|-----|----------|----------|
| 0 | 0 | 0 | 0.0 | 0.0 |
| 1 | 0 | 1 | 322.5 | 0.0 |
| 2 | 1 | 0 | 0.0 | -322.5 |

Where: `plate_size = 270 - 1.25 = 268.75 mm`, `stride = 268.75 × 1.2 = 322.5 mm`

### 5.4 GUI Reference Implementation

The engine implementation directly mirrors the GUI's `PartPlateList::compute_shape_position()`:

```cpp
// GUI: src/slic3r/GUI/PartPlate.cpp:3297-3309
Vec2d PartPlateList::compute_shape_position(int index, int cols)
{
    int row = index / cols;
    int col = index % cols;
    pos(0) = col * plate_stride_x();
    pos(1) = -row * plate_stride_y();
    return pos;
}
```

### 5.5 The Plate Alignment Fix (2026-03-10)

A critical bug was fixed in `main.cpp:493`. The bug caused G-code coordinate offsets of approximately ±324 mm when slicing individual plates from multi-plate projects.

**Root cause:** The engine incorrectly used `plates_to_process.size()` (number of plates being sliced) instead of `plate_data.size()` (total plates in the project) to compute `plate_cols`.

**Incorrect code:**
```cpp
const int plate_cols = compute_colum_count(
    static_cast<int>(plates_to_process.size())  // WRONG
);
```

**Corrected code:**
```cpp
// CRITICAL: Use total plate count from source 3MF, not plates_to_process.size()
const int plate_cols = compute_colum_count(
    static_cast<int>(plate_data.size())  // CORRECT
);
```

**Why this matters:** Consider a 3-plate project where only plate 1 (index 0) is being sliced:

| Variable | Incorrect | Correct |
|----------|-----------|---------|
| `plate_cols` | `compute_colum_count(1)` = **1** | `compute_colum_count(3)` = **2** |
| Plate 1 row | `0 / 1 = 0` | `0 / 2 = 0` |
| Plate 1 col | `0 % 1 = 0` | `0 % 2 = 0` |
| Plate 1 origin | (0, 0) ✅ (coincidentally correct) | (0, 0) ✅ |
| Plate 2 row | `1 / 1 = 1` ❌ | `1 / 2 = 0` ✅ |
| Plate 2 col | `1 % 1 = 0` ❌ | `1 % 2 = 1` ✅ |
| Plate 2 origin | **(0, -322.5)** ❌ | **(322.5, 0)** ✅ |

For full details and test scripts, see [plate-alignment-fix.md](./plate-alignment-fix.md).

---

## 6. Build System

### 6.1 CMake Configuration

The engine is enabled by setting `SLIC3R_GUI=OFF`. This is enforced in `src/CMakeLists.txt`:

```cmake
# src/CMakeLists.txt:22-24
if (NOT SLIC3R_GUI)
    add_subdirectory(orca-slice-engine)
endif()
```

When `SLIC3R_GUI=ON` (the GUI build), the engine target is not compiled.

**Key CMake options for engine builds:**

| Option | Recommended Value | Effect |
|--------|-------------------|--------|
| `SLIC3R_GUI` | `OFF` | Excludes wxWidgets, OpenGL, GUI code |
| `SLIC3R_STATIC` | `1` (Linux/macOS) | Links dependencies statically for portability |
| `ORCA_TOOLS` | `OFF` | Excludes profile validator tool |
| `BUILD_TESTS` | `OFF` | Excludes test suite |
| `FLATPAK` | `ON` (deps build) | Excludes wxWidgets from dependency build |

### 6.2 Engine CMakeLists.txt

The engine target (`src/orca-slice-engine/CMakeLists.txt`) is minimal:

**Source files:**
- `main.cpp` — entire engine logic (~810 lines)
- `nanosvg.cpp` — SVG rasterization implementation (required by libslic3r in headless builds)

**Required link libraries:**

| Library | Purpose |
|---------|---------|
| `libslic3r` | Core slicing engine |
| `cereal::cereal` | Serialization framework |
| `boost_libs` | Filesystem, logging, nowide (Unicode args on Windows) |
| `TBB::tbb` | Parallel slicing (Intel Threading Building Blocks) |
| `TBB::tbbmalloc` | TBB-optimized memory allocator |
| `nanosvg` | SVG support for thumbnails and bed shape |

**Platform-specific libraries:**

| Platform | Additional Libraries |
|----------|---------------------|
| Windows | `Psapi.lib`, `bcrypt.lib` |
| macOS | `libiconv`, IOKit, CoreFoundation frameworks |
| Linux | `libdl`, `pthreads` |

**No GUI dependencies:**
- No `wxWidgets`
- No `OpenGL` / `GLEW` / `GLFW`
- No `libcurl` (no network required)
- No `OpenCV`

### 6.3 Dependency Build (Headless Mode)

The dependency build must use `-DFLATPAK=ON` to skip wxWidgets, which is not needed for the engine:

```bash
cmake -S deps -B deps/build -G Ninja \
  -DFLATPAK=ON \
  -DDESTDIR="$(pwd)/deps/build/destdir"
```

Required dependency libraries built:
- Boost 1.83.0 (system, filesystem, thread, log, log_setup, locale, regex, chrono, atomic, date_time, iostreams, program_options, nowide)
- Intel TBB
- Eigen3 (header-only)
- Clipper2
- OpenVDB + Blosc
- NLopt
- CGAL (GMP + MPFR)
- OpenSSL
- Cereal
- nlohmann_json
- libpng, freetype, ZLIB
- OpenCASCADE (STEP format support)

### 6.4 CI/CD: GitHub Actions

The workflow at `.github/workflows/build-cloud-engine.yml` automates Linux x64 builds.

**Trigger conditions:**

| Event | Branches / Conditions |
|-------|----------------------|
| `push` | `main`, `release/*`, `engine-build` — only when engine-related paths change |
| `pull_request` | Targeting `main` — only when engine-related paths change |
| `workflow_dispatch` | Manual trigger, any branch |

**Monitored paths that trigger the workflow:**
- `src/orca-slice-engine/**`
- `src/libslic3r/**`
- `src/libnest2d/**`
- `deps/**`
- `CMakeLists.txt`
- `cmake/**`
- `.github/workflows/build-cloud-engine.yml`

**Build environment:** Ubuntu 22.04 (chosen for glibc 2.35 compatibility, maximizing deployment compatibility).

**Dependency caching:** The workflow caches `deps/build/destdir` and `deps/DL_CACHE` using a hash of all dependency CMakeLists.txt files. Cache key: `orca-deps-headless-Linux-<hash>`.

**Artifact structure:**

```
orca-slice-engine-linux-x64-V<version>.tar.gz
└── orca-slice-engine/
    ├── orca-slice-engine          # ELF binary (statically linked core)
    ├── run.sh                     # Wrapper script setting LD_LIBRARY_PATH
    ├── lib/
    │   ├── libstdc++.so.6         # Bundled for compatibility
    │   ├── libjpeg.so.8           # libjpeg-turbo (version specific)
    │   └── ...                    # Other non-system shared libraries
    └── resources/
        ├── profiles/              # Printer/material presets (all vendors)
        └── info/                  # Nozzle size information
```

**Artifact retention:** 30 days.

**Downloading artifacts:**
1. Navigate to Actions → Build Cloud Slice Engine
2. Select a completed run
3. Download `orca-slice-engine-linux-x64-*` from the Artifacts section

---

## 7. Engine vs GUI Comparison

| Aspect | GUI Application | Slicing Engine |
|--------|-----------------|----------------|
| **Entry point** | `src/Snapmaker_Orca.cpp` | `src/orca-slice-engine/main.cpp` |
| **Interface** | Interactive wxWidgets window | Command-line arguments |
| **Input** | Open file dialog, drag-and-drop | `-` argument: path to 3MF |
| **Configuration** | Interactive preset selection | Embedded in 3MF file |
| **Display** | OpenGL 3D viewport | None |
| **Thumbnails** | Generated via OpenGL rendering | Empty (not generated) |
| **Plate origin** | `PartPlateList::compute_shape_position()` | Same formula in `main.cpp:501-515` |
| **Plate size** | `bed.extended_bounding_box()` | `bbox(printable_area) - BED_AXES_TIP_RADIUS` |
| **Plate count for layout** | Total plates in project | `plate_data.size()` (total) |
| **Slicing call** | `Print::process()` | `Print::process()` (identical) |
| **G-code export** | `Print::export_gcode()` | `Print::export_gcode()` (identical) |
| **3MF packaging** | `store_bbs_3mf()` | `store_bbs_3mf()` (identical) |
| **SaveStrategy** | `Silence\|SplitModel\|WithGcode\|SkipModel\|Zip64` | Same flags |
| **Timestamp** | System clock | System clock or `-t` override |
| **Dependencies** | wxWidgets, OpenGL, GLEW, GLFW, OpenCV, libcurl | libslic3r, Boost, TBB, Cereal only |
| **Binary size** | ~100+ MB | ~20-30 MB |
| **Memory footprint** | High (rendering buffers, UI state) | Low (model + slicing data only) |
| **Parallelism** | TBB (slicing) + GPU (rendering) | TBB (slicing only) |
| **Platform** | Windows, macOS, Linux | Linux x64, macOS, Windows |
| **Exit mechanism** | `wxApp::OnExit()` | `std::quick_exit()` |

---

## 8. API Integration Guide

### 8.1 REST API Example (Python/Flask)

```python
from flask import Flask, request, send_file, jsonify
import subprocess
import tempfile
import os
import uuid

app = Flask(__name__)

ENGINE_PATH = "/opt/orca/orca-slice-engine"
RESOURCES_PATH = "/opt/orca/resources"


@app.route("/slice", methods=["POST"])
def slice_all_plates():
    """Slice all plates in the uploaded 3MF file."""
    if "model" not in request.files:
        return jsonify({"error": "No model file provided"}), 400

    job_id = str(uuid.uuid4())
    input_path = f"/tmp/{job_id}.3mf"
    output_base = f"/tmp/{job_id}_output"

    try:
        request.files["model"].save(input_path)

        result = subprocess.run(
            [
                ENGINE_PATH,
                input_path,
                "-o", output_base,
                "-r", RESOURCES_PATH,
            ],
            capture_output=True,
            text=True,
            timeout=300,
        )

        if result.returncode != 0:
            return jsonify({
                "error": "Slicing failed",
                "stderr": result.stderr,
                "exit_code": result.returncode,
            }), 500

        output_file = f"{output_base}.gcode.3mf"
        return send_file(output_file, mimetype="application/octet-stream",
                         as_attachment=True,
                         download_name=f"{job_id}.gcode.3mf")

    finally:
        # Cleanup input file
        if os.path.exists(input_path):
            os.unlink(input_path)


@app.route("/slice/plate/<int:plate_id>", methods=["POST"])
def slice_single_plate(plate_id: int):
    """Slice a specific plate and return G-code."""
    fmt = request.args.get("format", "gcode.3mf")
    if fmt not in ("gcode", "gcode.3mf"):
        return jsonify({"error": "Invalid format. Use 'gcode' or 'gcode.3mf'"}), 400

    job_id = str(uuid.uuid4())
    input_path = f"/tmp/{job_id}.3mf"
    output_base = f"/tmp/{job_id}"

    try:
        request.files["model"].save(input_path)

        result = subprocess.run(
            [
                ENGINE_PATH,
                input_path,
                "-p", str(plate_id),
                "-f", fmt,
                "-o", output_base,
                "-r", RESOURCES_PATH,
            ],
            capture_output=True,
            text=True,
            timeout=300,
        )

        if result.returncode == 1:
            return jsonify({"error": "Invalid plate ID or arguments",
                            "stderr": result.stderr}), 400
        if result.returncode == 2:
            return jsonify({"error": "Input file not found"}), 404
        if result.returncode != 0:
            return jsonify({"error": "Slicing failed",
                            "stderr": result.stderr,
                            "exit_code": result.returncode}), 500

        ext = ".gcode" if fmt == "gcode" else ".gcode.3mf"
        output_file = f"{output_base}{ext}"
        mimetype = "text/plain" if fmt == "gcode" else "application/octet-stream"

        return send_file(output_file, mimetype=mimetype,
                         as_attachment=True,
                         download_name=f"plate_{plate_id}{ext}")
    finally:
        if os.path.exists(input_path):
            os.unlink(input_path)


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
```

### 8.2 Async Job Queue Example (Python/Celery)

```python
from celery import Celery
import subprocess
import os

celery_app = Celery("slicer", broker="redis://localhost:6379/0")
ENGINE_PATH = "/opt/orca/orca-slice-engine"


@celery_app.task(bind=True)
def slice_model(self, input_path: str, output_base: str,
                plate_id: int = 0, fmt: str = "gcode.3mf"):
    """Async slicing task."""
    cmd = [ENGINE_PATH, input_path, "-o", output_base]
    if plate_id > 0:
        cmd.extend(["-p", str(plate_id)])
    cmd.extend(["-f", fmt])

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        if result.returncode != 0:
            raise Exception(f"Slicing failed (exit {result.returncode}): {result.stderr}")

        return {"status": "success", "output": output_base}
    except subprocess.TimeoutExpired:
        self.retry(countdown=0, max_retries=0)
        raise Exception("Slicing timed out after 10 minutes")
```

### 8.3 Error Handling Best Practices

Always map the exit code to a meaningful response:

```python
EXIT_CODE_MESSAGES = {
    0: "Success",
    1: "Invalid arguments (check plate ID and format)",
    2: "Input file not found",
    3: "Failed to load 3MF file (corrupt or incompatible version)",
    4: "Slicing failed (model may have geometric issues)",
    5: "Failed to write output file (check disk space and permissions)",
}

def run_engine(cmd: list) -> dict:
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    return {
        "success": result.returncode == 0,
        "exit_code": result.returncode,
        "message": EXIT_CODE_MESSAGES.get(result.returncode, "Unknown error"),
        "stdout": result.stdout,
        "stderr": result.stderr,
    }
```

---

## 9. Troubleshooting

### 9.1 Common Error Messages

**"No objects found in 3MF file"**

The input 3MF contains no 3D mesh objects, or the file is corrupt.

Resolution: Open the file in OrcaSlicer GUI to verify it contains printable models. Re-export if necessary.

---

**"Plate X not found in 3MF file"**

The requested plate ID does not exist in the project. The engine prints a list of available plate IDs.

Resolution: Run without `-p` to see what plates are available, then re-run with a valid ID.

---

**"All-plate slicing requires gcode.3mf format"**

`-f gcode` was specified without `-p`, which is not allowed.

Resolution: Either add `-p <id>` to slice a specific plate, or remove `-f gcode` to use the default format.

---

**"Failed to load 3MF file"**

The 3MF file uses a format version or features not supported by this engine version, or the file is corrupt.

Resolution: Check that the 3MF was exported from OrcaSlicer (not from Bambu Studio or PrusaSlicer without conversion). Verify file integrity.

---

**"libjpeg.so.8: cannot open shared object file"**

A bundled shared library is missing from the deployment.

Resolution: Use the `run.sh` wrapper script from the release artifact instead of calling `orca-slice-engine` directly. The wrapper sets `LD_LIBRARY_PATH` to include the bundled `lib/` directory.

---

**"Resources directory may be incomplete"**

The resources directory exists but lacks `profiles/` or `printers/` subdirectories.

Resolution: Ensure the full `resources/` directory from the release artifact is present. The engine needs `resources/profiles/` to load printer/material configurations.

### 9.2 Log Levels

| Level | Flag | Content |
|-------|------|---------|
| `info` | Default | Key milestones, plate origins, output paths |
| `warning` | Default | Non-fatal issues (missing resources, empty plates) |
| `error` | Default | Fatal errors with details |
| `debug` | `-v` | Temp file operations, config values |
| `trace` | `-v` | Full slicing trace from libslic3r |

Enable verbose logging and redirect to a log file:

```bash
./orca-slice-engine model.3mf -v 2>&1 | tee /tmp/slice.log
```

### 9.3 Performance Notes

| Resource | Guidance |
|----------|----------|
| RAM | 2-4 GB for typical models; 6+ GB for complex multi-material models |
| CPU | All available cores used via TBB; no way to limit without `taskset`/`numactl` |
| Disk | Temp files written to system temp dir; multi-plate jobs create N temp `.gcode` files |
| Concurrency | Multiple engine instances can run simultaneously; no shared state between processes |

Limit CPU usage for concurrent jobs:

```bash
# Limit to 4 cores
taskset -c 0-3 ./orca-slice-engine model.3mf

# Use only 4 cores via TBB environment variable
TBB_THREAD_NUM=4 ./orca-slice-engine model.3mf
```

---

## 10. Development Guide

### 10.1 Source Code Structure

```
src/orca-slice-engine/
├── main.cpp          # All engine logic (~810 lines)
└── nanosvg.cpp       # nanosvg implementation unit (required by libslic3r)
```

The engine is intentionally a single-file implementation. All logic is in `main.cpp`. This minimizes complexity and makes the engine easy to understand, audit, and modify.

**Key sections in main.cpp:**

| Lines | Content |
|-------|---------|
| 1-43 | Includes and `using namespace Slic3r` |
| 44-64 | Exit codes, `OutputFormat` enum, `compute_colum_count()` |
| 66-131 | `print_usage()` and `generate_output_path()` |
| 133-209 | `main()`: argument parsing loop |
| 210-234 | Argument validation and format auto-selection |
| 236-289 | Logging setup and resources directory resolution |
| 291-373 | 3MF loading and plate data validation |
| 375-519 | Plate preparation loop: printable flags, origin calculation |
| 520-599 | Slicing and G-code export per plate |
| 601-791 | 3MF packaging with metadata |
| 793-810 | Summary output and cleanup |

### 10.2 Related Source Files

To understand or modify the engine, these files in `libslic3r` are the primary references:

| File | Relevance |
|------|-----------|
| `src/libslic3r/Print.cpp` | `Print::process()` and `Print::export_gcode()` |
| `src/libslic3r/PrintApply.cpp` | How plate origin shifts are applied to model |
| `src/libslic3r/GCode.cpp` | G-code generation with `set_gcode_offset()` |
| `src/libslic3r/Format/bbs_3mf.cpp` | 3MF load/save; `plate_index` 0-based conversion at line 1485 |
| `src/slic3r/GUI/PartPlate.cpp` | GUI plate layout reference (lines 3260-3309) |
| `src/slic3r/GUI/3DBed.cpp` | `extended_bounding_box()` and `DefaultTipRadius` |

### 10.3 Adding a New CLI Option

1. Add the new option constant or enum in the declarations section (around line 44-64)
2. Add a case in the argument parsing loop (lines 148-209)
3. Add validation after the loop if needed (lines 211-234)
4. Pass the value to the appropriate `Print::` method or packager
5. Update `print_usage()` (line 66-91) to document the new option
6. Update this technical guide and the README

### 10.4 Testing Slicing Output

To verify that engine output matches GUI output:

```python
#!/usr/bin/env python3
"""Compare G-code coordinates from engine and GUI output."""
import zipfile, re, sys

def first_xy(gcode_3mf, plate=1):
    with zipfile.ZipFile(gcode_3mf) as z:
        gcode = z.read(f"Metadata/plate_{plate}.gcode").decode()
    for line in gcode.splitlines():
        m = re.search(r"G1 .*X([-\d.]+).*Y([-\d.]+)", line)
        if m:
            return float(m.group(1)), float(m.group(2))
    return None, None

engine_x, engine_y = first_xy(sys.argv[1])
gui_x,    gui_y    = first_xy(sys.argv[2])

diff = ((engine_x - gui_x)**2 + (engine_y - gui_y)**2) ** 0.5
print(f"Engine: ({engine_x:.3f}, {engine_y:.3f})")
print(f"GUI:    ({gui_x:.3f}, {gui_y:.3f})")
print(f"Delta:  {diff:.3f} mm  {'PASS' if diff < 0.1 else 'FAIL'}")
```

Usage:
```bash
python compare.py engine_output.gcode.3mf gui_output.gcode.3mf
```

### 10.5 Adding Support for a New 3MF Feature

The engine uses the same `Model::read_from_file()` as the GUI. If the GUI adds support for a new 3MF feature (e.g., a new per-plate metadata field), the engine inherits it automatically via `libslic3r` — no changes to `main.cpp` are needed unless the feature requires a new CLI option or changes the packaging step.

For new per-plate metadata that should appear in the output `gcode.3mf`, add population code to the `PlateData` preparation loop (lines 643-713 in `main.cpp`).

---

## Appendix A: Mathematical Formulas

```
# Plate dimensions
plate_width = bbox_width - BED_AXES_TIP_RADIUS    (BED_AXES_TIP_RADIUS = 1.25 mm)
plate_depth = bbox_depth - BED_AXES_TIP_RADIUS

# Plate stride
plate_stride_x = plate_width * (1 + LOGICAL_PART_PLATE_GAP)   (GAP = 0.2)
plate_stride_y = plate_depth * (1 + LOGICAL_PART_PLATE_GAP)

# Grid layout
plate_cols = compute_colum_count(total_plate_count)
row = plate_index / plate_cols
col = plate_index % plate_cols

# Origin
origin_x = col * plate_stride_x
origin_y = -row * plate_stride_y

# Column count function
compute_colum_count(n):
    v = sqrt(n)
    r = round(v)
    return r + 1 if v > r else r
```

---

## Appendix B: File Structure of a gcode.3mf Output

A `gcode.3mf` output produced by the engine has this internal structure:

```
output.gcode.3mf  (ZIP archive)
├── _rels/
│   └── .rels
├── 3D/
│   └── 3dmodel.model          # Model geometry (present with SplitModel flag)
├── Metadata/
│   ├── model_settings.config  # Slicing configuration
│   ├── plate_1.gcode          # G-code for plate 1
│   ├── plate_2.gcode          # G-code for plate 2 (if multi-plate)
│   ├── slice_info.config      # Per-plate slicing metadata (time, weight, etc.)
│   └── project_settings.config
└── [Content_Types].xml
```

---

**Document Version:** 1.0
**Author:** Technical Writer
**Status:** Complete
