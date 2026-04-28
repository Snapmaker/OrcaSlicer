#!/usr/bin/env bash
# Package orca-slice-engine for Linux deployment
# Usage: ./scripts/package_slice_engine.sh [build_dir] [output_dir]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

BUILD_DIR="${1:-${PROJECT_ROOT}/build-engine}"
OUTPUT_DIR="${2:-${PROJECT_ROOT}/package-orca-slice-engine}"
VERSION=$(grep "Snapmaker_VERSION" "${PROJECT_ROOT}/version.inc" | cut -d'"' -f2)

PACKAGE_NAME="orca-slice-engine-linux-x64-${VERSION}"
PACKAGE_DIR="${OUTPUT_DIR}/${PACKAGE_NAME}"

echo "=== Packaging orca-slice-engine ==="
echo "Build dir: ${BUILD_DIR}"
echo "Output dir: ${PACKAGE_DIR}"
echo "Version: ${VERSION}"

# Check if build exists
ENGINE_PATH="${BUILD_DIR}/bin/orca-slice-engine"
if [ ! -f "${ENGINE_PATH}" ]; then
    # Try alternative location
    ENGINE_PATH="${BUILD_DIR}/src/orca-slice-engine/orca-slice-engine"
fi

if [ ! -f "${ENGINE_PATH}" ]; then
    echo "ERROR: orca-slice-engine not found in ${BUILD_DIR}"
    echo "Please build with: cmake -DSLIC3R_GUI=OFF .."
    exit 1
fi

echo "Found executable: ${ENGINE_PATH}"

# Create package directory structure
rm -rf "${PACKAGE_DIR}"
mkdir -p "${PACKAGE_DIR}/bin"
mkdir -p "${PACKAGE_DIR}/lib"
mkdir -p "${PACKAGE_DIR}/resources"

# Copy executable
cp "${ENGINE_PATH}" "${PACKAGE_DIR}/bin/"
chmod +x "${PACKAGE_DIR}/bin/orca-slice-engine"

# Copy resources
echo "Copying resources..."
cp -r "${PROJECT_ROOT}/resources/"* "${PACKAGE_DIR}/resources/" 2>/dev/null || {
    echo "WARNING: Some resources may not have been copied"
}

# Check if statically linked
IS_STATIC=$(ldd "${ENGINE_PATH}" 2>&1 | grep -c "not a dynamic executable" || true)

if [ "${IS_STATIC}" -eq 1 ]; then
    echo "Executable is statically linked - no library dependencies"
else
    # Collect shared library dependencies
    echo "Collecting shared libraries..."
    cd "${PACKAGE_DIR}/bin"
    
    # Use ldd to find dependencies, filter out system libraries
    ldd orca-slice-engine 2>/dev/null | while read -r line; do
        lib_path=$(echo "$line" | awk '{print $3}')
        lib_name=$(echo "$line" | awk '{print $1}')
        
        # Skip if not a valid path
        if [ -z "$lib_path" ] || [ ! -f "$lib_path" ]; then
            continue
        fi
        
        # Skip common system libraries (glibc, libm, libpthread, etc.)
        case "$lib_name" in
            linux-vdso.*|libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*|ld-linux*|libgcc_s.so.*|libstdc++.so.*)
                echo "  [system] $lib_name"
                ;;
            *)
                echo "  [bundle] $lib_name"
                cp -L "$lib_path" "${PACKAGE_DIR}/lib/" 2>/dev/null || true
                ;;
        esac
    done
fi

# Create wrapper script for easy execution
cat > "${PACKAGE_DIR}/orca-slice-engine" << 'WRAPPER_EOF'
#!/usr/bin/env bash
# OrcaSlicer Cloud Slicing Engine wrapper script
# Sets up library path and resources directory

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Set library path for bundled libraries
export LD_LIBRARY_PATH="${SCRIPT_DIR}/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Set resources directory
export ORCA_RESOURCES="${SCRIPT_DIR}/resources"

# Run the engine
exec "${SCRIPT_DIR}/bin/orca-slice-engine" "$@"
WRAPPER_EOF
chmod +x "${PACKAGE_DIR}/orca-slice-engine"

# Create README
cat > "${PACKAGE_DIR}/README.md" << README_EOF
# OrcaSlicer Cloud Slicing Engine v${VERSION}

A headless slicing engine for cloud deployment and automation.

## Quick Start

\`\`\`bash
# Extract the package
tar -xzf orca-slice-engine-linux-x64-${VERSION}.tar.gz
cd orca-slice-engine-linux-x64-${VERSION}

# Run slicing
./orca-slice-engine model.3mf -o output
\`\`\`

## Usage

\`\`\`
./orca-slice-engine input.3mf [OPTIONS]

Options:
  -o, --output <file>    Output file path (without extension)
  -p, --plate <id>       Plate number to slice (1, 2, 3...) or "all"
  -f, --format <fmt>     Output format: gcode | gcode.3mf (default: gcode.3mf)
  -r, --resources <dir>  Resources directory (default: ./resources)
  -v, --verbose          Enable verbose logging
  -h, --help             Show help message

Examples:
  ./orca-slice-engine model.3mf                        # All plates -> model.gcode.3mf
  ./orca-slice-engine model.3mf -p 1                   # Plate 1 only
  ./orca-slice-engine model.3mf -o output -f gcode     # Plain G-code output
  ./orca-slice-engine model.3mf -r /path/to/resources  # Custom resources path
\`\`\`

## Exit Codes

| Code | Description |
|------|-------------|
| 0    | Success |
| 1    | Invalid arguments |
| 2    | Input file not found |
| 3    | 3MF load error |
| 4    | Slicing error |
| 5    | G-code export error |
| 6    | Pre-processing validation error |
| 7    | Post-processing warning |

## System Requirements

- Linux x86_64
- glibc 2.17+ (CentOS 7+, Ubuntu 14.04+, Debian 8+)

## Directory Structure

\`\`\`
orca-slice-engine-linux-x64-${VERSION}/
├── orca-slice-engine      # Wrapper script (use this)
├── bin/
│   └── orca-slice-engine  # Main executable
├── lib/                   # Bundled libraries
└── resources/             # Printer profiles and presets
    ├── profiles/
    ├── printers/
    └── filaments/
\`\`\`

## Environment Variables

- \`ORCA_RESOURCES\`: Override resources directory location
- \`LD_LIBRARY_PATH\`: Library search path (set by wrapper)

## API Integration

For cloud service integration, call the executable directly:

\`\`\`python
import subprocess

result = subprocess.run([
    './bin/orca-slice-engine',
    'model.3mf',
    '-o', 'output',
    '-p', '1',
    '-r', '/app/resources'
], capture_output=True, text=True)

if result.returncode == 0:
    print("Slicing successful")
    print(result.stdout)
else:
    print(f"Error: {result.stderr}")
\`\`\`

## License

See LICENSE.txt for details.
README_EOF

# Create tarball
echo ""
echo "Creating tarball..."
cd "${OUTPUT_DIR}"
tar -czf "${PACKAGE_NAME}.tar.gz" "${PACKAGE_NAME}"

# Calculate checksum
sha256sum "${PACKAGE_NAME}.tar.gz" > "${PACKAGE_NAME}.tar.gz.sha256"

# Get size
SIZE=$(du -h "${PACKAGE_NAME}.tar.gz" | cut -f1)

echo ""
echo "=== Package created successfully ==="
echo "Package: ${OUTPUT_DIR}/${PACKAGE_NAME}.tar.gz"
echo "Size: ${SIZE}"
echo "Checksum: ${OUTPUT_DIR}/${PACKAGE_NAME}.tar.gz.sha256"
echo ""
echo "To deploy:"
echo "  1. Copy ${PACKAGE_NAME}.tar.gz to target server"
echo "  2. Extract: tar -xzf ${PACKAGE_NAME}.tar.gz"
echo "  3. Run: cd ${PACKAGE_NAME} && ./orca-slice-engine --help"
