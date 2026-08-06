#!/usr/bin/env bash
#
# gitlab_build_linux.sh — Build Snapmaker_Orca on Linux from GitLab sources
#
# All source code comes from GitLab. No GitHub dependency at build time.
#
# GitLab repos:
#   1. snapmaker_orca/OrcaSlicer             — main repo (this script)
#   2. snapmaker_orca/snapmaker_orca_vendor   — deps build system + DL_CACHE
#   3. snapmaker_orca/Orca-deps-wxWidgets     — wxWidgets fork
#   4. snapmaker_orca/sentry-native           — Sentry SDK (crashpad backend)
#
# Directory layout (after script runs):
#   %ORCA_ROOT%/
#   |-- gitlab_build_linux.sh
#   |-- gitlab_vendor/                — cloned vendor repo
#   |   `-- gitlab_deps/
#   |       `-- build/                — deps build output
#   |-- Orca-deps-wxWidgets/          — cloned wxWidgets (source)
#   |-- sentry-native/                — cloned sentry (source + crashpad)
#   |-- gitlab_build/                 — slicer build output
#   |-- gitlab_deps_src/              — copied from vendor/gitlab_deps_src
#   `-- deps_src/                     — original repo version (untouched)
#
# Usage:
#   ./gitlab_build_linux.sh -dsi       — build deps + slicer + AppImage
#   ./gitlab_build_linux.sh -u         — install system dependencies
#   ./gitlab_build_linux.sh -ds        — build deps + slicer
#   ./gitlab_build_linux.sh -d         — build deps only
#   ./gitlab_build_linux.sh -s         — build slicer only
#
# Environment variables (optional):
#   GITLAB_BASE_URL  — GitLab host (default: gitlab.s.com)
#   GITLAB_GROUP     — GitLab group (default: snapmaker_orca)
#

set -e

SCRIPT_PATH=$(dirname "$(readlink -f "${0}")")
pushd "${SCRIPT_PATH}" > /dev/null

# ============================================================
# 1. GitLab repository configuration
# ============================================================
if [ -z "$GITLAB_BASE_URL" ]; then
    export GITLAB_BASE_URL="gitlab.s.com"
fi
if [ -z "$GITLAB_GROUP" ]; then
    export GITLAB_GROUP="snapmaker_orca"
fi

GITLAB_VENDOR="http://${GITLAB_BASE_URL}/${GITLAB_GROUP}/snapmaker_orca_vendor.git"
GITLAB_WXWIDGETS="http://${GITLAB_BASE_URL}/${GITLAB_GROUP}/Orca-deps-wxWidgets.git"
GITLAB_SENTRY="http://${GITLAB_BASE_URL}/${GITLAB_GROUP}/sentry-native.git"

ORCA_ROOT="${SCRIPT_PATH}"
VENDOR_DIR="${ORCA_ROOT}/gitlab_vendor"
WXWIDGETS_DIR="${ORCA_ROOT}/Orca-deps-wxWidgets"
SENTRY_DIR="${ORCA_ROOT}/sentry-native"

# Build directories
DEPS_DIR="${VENDOR_DIR}/gitlab_deps"
GITLAB_DEPS_SRC_DIR="${ORCA_ROOT}/gitlab_deps_src"
GITLAB_BUILD_DIR="${ORCA_ROOT}/gitlab_build"

# ============================================================
# 2. Parse arguments
# ============================================================

SLIC3R_PRECOMPILED_HEADERS="ON"

unset name
while getopts ":1j:bcCdhiprstulL" opt ; do
  case ${opt} in
    1 )
        export CMAKE_BUILD_PARALLEL_LEVEL=1
        ;;
    j )
        export CMAKE_BUILD_PARALLEL_LEVEL=$OPTARG
        ;;
    b )
        BUILD_DEBUG="1"
        ;;
    c )
        CLEAN_BUILD=1
        ;;
    C )
        COLORED_OUTPUT="-DCOLORED_OUTPUT=ON"
        ;;
    d )
        BUILD_DEPS="1"
        ;;
    h )
        echo "Usage: $0 [-1][-b][-c][-d][-h][-i][-j N][-p][-r][-s][-t][-u][-l][-L]"
        echo "   -1: limit builds to one core"
        echo "   -j N: limit builds to N cores"
        echo "   -b: build in debug mode"
        echo "   -c: force a clean build"
        echo "   -C: enable ANSI-colored compile output (GNU/Clang only)"
        echo "   -d: build dependencies from vendor/gitlab_deps"
        echo "   -h: prints this help text"
        echo "   -i: build the Orca Slicer AppImage (optional)"
        echo "   -p: boost ccache hit rate by disabling precompiled headers"
        echo "   -r: skip RAM and disk checks"
        echo "   -s: build the Orca Slicer"
        echo "   -t: build tests"
        echo "   -u: install system dependencies (requires sudo)"
        echo "   -l: use Clang instead of GCC (default: GCC)"
        echo "   -L: use ld.lld as linker (if available)"
        echo ""
        echo "For a first use: ./$0 -u && ./$0 -dsi"
        exit 0
        ;;
    i )
        BUILD_IMAGE="1"
        ;;
    p )
        SLIC3R_PRECOMPILED_HEADERS="OFF"
        ;;
    r )
        SKIP_RAM_CHECK="1"
        ;;
    s )
        BUILD_ORCA="1"
        ;;
    t )
        BUILD_TESTS="1"
        ;;
    u )
        export UPDATE_LIB="1"
        ;;
    l )
        USE_CLANG="1"
        ;;
    L )
        USE_LLD="1"
        ;;
    * )
        echo "Unknown argument '${opt}', aborting."
        exit 1
        ;;
  esac
done

if [ ${OPTIND} -eq 1 ] ; then
    echo "Usage: $0 [-1][-b][-c][-d][-h][-i][-j N][-p][-r][-s][-t][-u][-l][-L]"
    echo "For a first use: ./$0 -u && ./$0 -dsi"
    exit 1
fi

# ============================================================
# 3. Restore patched cmake files on script exit
# ============================================================
function restore_vendor_cmake() {
    if [ -d "${VENDOR_DIR}/.git" ]; then
        echo "Restoring patched vendor cmake files..."
        pushd "${VENDOR_DIR}" > /dev/null
        git checkout -- gitlab_deps/wxWidgets/wxWidgets.cmake 2>/dev/null || true
        git checkout -- gitlab_deps/Sentry/Sentry.cmake 2>/dev/null || true
        git checkout -- gitlab_deps/OpenVDB/OpenVDB.cmake 2>/dev/null || true
        popd > /dev/null
    fi
}
trap restore_vendor_cmake EXIT

# ============================================================
# 4. Clone / sync GitLab sub-repositories
# ============================================================
function sync_gitlab_repos() {
    echo "============================================================"
    echo "  Syncing GitLab sub-repositories..."
    echo "============================================================"
    echo "  ORCA_ROOT : ${ORCA_ROOT}"
    echo "  VENDOR    : ${VENDOR_DIR}"
    echo "  WXWIDGETS : ${WXWIDGETS_DIR}"
    echo "  SENTRY    : ${SENTRY_DIR}"
    echo "============================================================"
    echo

    # --- gitlab_vendor ---
    if [ -d "${VENDOR_DIR}/.git" ]; then
        echo "  vendor: fetching latest..."
        pushd "${VENDOR_DIR}" > /dev/null
        git fetch origin
        git reset --hard FETCH_HEAD
        popd > /dev/null
    else
        rm -rf "${VENDOR_DIR}"
        echo "  vendor: cloning ${GITLAB_VENDOR} ..."
        git clone "${GITLAB_VENDOR}" "${VENDOR_DIR}"
    fi

    # --- Orca-deps-wxWidgets ---
    if [ -d "${WXWIDGETS_DIR}/.git" ]; then
        echo "  wxWidgets: pulling latest..."
        pushd "${WXWIDGETS_DIR}" > /dev/null
        git pull --ff-only
        popd > /dev/null
    else
        rm -rf "${WXWIDGETS_DIR}"
        echo "  wxWidgets: cloning ${GITLAB_WXWIDGETS} ..."
        git clone "${GITLAB_WXWIDGETS}" "${WXWIDGETS_DIR}"
    fi

    # --- sentry-native ---
    # Sentry is OFF by default on Linux, but we sync it for consistency
    # and in case it's enabled via cmake flags
    if [ -d "${SENTRY_DIR}/.git" ]; then
        echo "  sentry: pulling latest..."
        pushd "${SENTRY_DIR}" > /dev/null
        git pull --ff-only
        popd > /dev/null
    else
        rm -rf "${SENTRY_DIR}"
        echo "  sentry: cloning ${GITLAB_SENTRY} ..."
        git clone "${GITLAB_SENTRY}" "${SENTRY_DIR}"
    fi

    # --- sentry-native submodule (crashpad) ---
    if [ -d "${SENTRY_DIR}/.git" ]; then
        echo "  sentry: initializing submodule (crashpad)..."
        pushd "${SENTRY_DIR}" > /dev/null
        git submodule update --init --recursive
        popd > /dev/null
    fi

    echo "  All repos synced successfully."
    echo
}

# ============================================================
# 5. Patch vendor cmake files to use local source dirs
#    Instead of letting ExternalProject git-clone from GitHub,
#    we point it to the already-cloned local repos from GitLab.
# ============================================================
function patch_cmake_for_local_sources() {
    echo "Patching cmake files for local sources..."

    local WX_CMAKE="${DEPS_DIR}/wxWidgets/wxWidgets.cmake"
    local SENTRY_CMAKE="${DEPS_DIR}/Sentry/Sentry.cmake"
    local OPENVDB_CMAKE="${DEPS_DIR}/OpenVDB/OpenVDB.cmake"

    # wxWidgets: replace GIT_REPOSITORY with SOURCE_DIR, remove GIT_SHALLOW
    if [ -f "$WX_CMAKE" ]; then
        sed -i \
            -e 's|GIT_REPOSITORY "https://github\.com/SoftFever/Orca-deps-wxWidgets"|SOURCE_DIR "'"${WXWIDGETS_DIR}"'"|' \
            -e '/GIT_SHALLOW ON/d' \
            "$WX_CMAKE"
        echo "  wxWidgets -> SOURCE_DIR"
    fi

    # Sentry: replace GIT_REPOSITORY with SOURCE_DIR, remove GIT_TAG + GIT_SHALLOW
    if [ -f "$SENTRY_CMAKE" ]; then
        sed -i -E \
            -e 's|GIT_REPOSITORY[[:space:]]+https://github\.com/getsentry/sentry-native\.git|SOURCE_DIR "'"${SENTRY_DIR}"'"|' \
            -e '/GIT_TAG[[:space:]]+[0-9.]+/d' \
            -e '/GIT_SHALLOW[[:space:]]+ON/d' \
            "$SENTRY_CMAKE"
        echo "  Sentry    -> SOURCE_DIR"
    fi

    # OpenVDB: disable vdb_print tool (links tbb which may cause issues)
    if [ -f "$OPENVDB_CMAKE" ]; then
        if grep -q "OPENVDB_BUILD_VDB_PRINT=ON" "$OPENVDB_CMAKE"; then
            sed -i 's|-DOPENVDB_BUILD_VDB_PRINT=ON|-DOPENVDB_BUILD_VDB_PRINT=OFF|' "$OPENVDB_CMAKE"
            echo "  OpenVDB   -> VDB_PRINT=OFF"
        else
            echo "  OpenVDB   -> already patched"
        fi
    fi

    echo
}

# ============================================================
# 6. Prepare gitlab_deps_src from vendor
#    The slicer build uses deps_src/ via add_subdirectory().
#    We copy the vendor's version to gitlab_deps_src/ and
#    pass -DDEPS_SRC_DIR=gitlab_deps_src to CMake.
#    The original deps_src/ is NOT touched.
# ============================================================
function prepare_deps_src() {
    echo "Preparing vendored library sources..."

    if [ -d "${VENDOR_DIR}/gitlab_deps_src" ]; then
        # Remove stale copy from previous run
        if [ -d "${GITLAB_DEPS_SRC_DIR}" ]; then
            echo "  Removing stale gitlab_deps_src/..."
            rm -rf "${GITLAB_DEPS_SRC_DIR}"
        fi

        # Copy vendor version to root as gitlab_deps_src/
        echo "  Copying vendor/gitlab_deps_src/ to gitlab_deps_src/..."
        cp -R "${VENDOR_DIR}/gitlab_deps_src" "${GITLAB_DEPS_SRC_DIR}"
    else
        echo "  [WARNING] Vendor repo has no gitlab_deps_src/, using original deps_src/"
    fi

    echo "  deps_src ready."
    echo
}

# ============================================================
# 7. Verify local repos are usable
# ============================================================
function verify_repos() {
    echo "Verifying local repos..."

    if [ -f "${WXWIDGETS_DIR}/CMakeLists.txt" ]; then
        echo "  wxWidgets: CMakeLists.txt found - OK"
    else
        echo "  [ERROR] wxWidgets: CMakeLists.txt NOT found at ${WXWIDGETS_DIR}"
        exit 1
    fi

    if [ -d "${SENTRY_DIR}" ] && [ -f "${SENTRY_DIR}/CMakeLists.txt" ]; then
        echo "  Sentry:    CMakeLists.txt found - OK"
    else
        echo "  Sentry:    not present or no CMakeLists.txt (not required on Linux) - OK"
    fi

    if [ -f "${DEPS_DIR}/CMakeLists.txt" ]; then
        echo "  Deps:      CMakeLists.txt found - OK"
    else
        echo "  [ERROR] Deps: CMakeLists.txt NOT found at ${DEPS_DIR}"
        exit 1
    fi

    echo "  All repos verified."
    echo
}

# ============================================================
# 8. System checks (RAM, disk, cmake version, distribution)
# ============================================================

function check_available_memory_and_disk() {
    FREE_MEM_GB=$(free --gibi --total | grep 'Mem' | rev | cut --delimiter=" " --fields=1 | rev)
    MIN_MEM_GB=10

    FREE_DISK_KB=$(df --block-size=1K . | tail -1 | awk '{print $4}')
    MIN_DISK_KB=$((10 * 1024 * 1024))

    if [[ ${FREE_MEM_GB} -le ${MIN_MEM_GB} ]] ; then
        echo -e "\nERROR: Orca Slicer Builder requires at least ${MIN_MEM_GB}G of 'available' mem (system has only ${FREE_MEM_GB}G available)"
        echo && free --human && echo
        echo "Invoke with -r to skip RAM and disk checks."
        exit 2
    fi

    if [[ ${FREE_DISK_KB} -le ${MIN_DISK_KB} ]] ; then
        echo -e "\nERROR: Orca Slicer Builder requires at least $(echo "${MIN_DISK_KB}" |awk '{ printf "%.1fG\n", $1/1024/1024; }') (system has only $(echo "${FREE_DISK_KB}" | awk '{ printf "%.1fG\n", $1/1024/1024; }') disk free)"
        echo && df --human-readable . && echo
        echo "Invoke with -r to skip ram and disk checks."
        exit 1
    fi
}

# cmake 4.x compatibility workaround
export CMAKE_POLICY_VERSION_MINIMUM=3.5

CMAKE_VER=$("${CMAKE:-cmake}" --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || echo "0.0.0")
CMAKE_MAJOR=$(echo "$CMAKE_VER" | cut -d. -f1)
CMAKE_MINOR=$(echo "$CMAKE_VER" | cut -d. -f2)
echo "Detected CMake version: ${CMAKE_VER}"
if [ "$CMAKE_MAJOR" -ge 4 ] 2>/dev/null; then
    echo ""
    echo "================================================================"
    echo " ERROR: CMake ${CMAKE_VER} is not supported!"
    echo ""
    echo " CMake 4.x has breaking changes that cause dependency builds"
    echo " (curl, OCCT, etc.) to fail during CMake configure."
    echo ""
    echo " Install CMake 3.28.x – 3.30.x instead."
    echo "================================================================"
    echo ""
    exit 1
fi
if [ "$CMAKE_MAJOR" -eq 3 ] 2>/dev/null && [ "$CMAKE_MINOR" -ge 32 ] 2>/dev/null; then
    echo ""
    echo "================================================================"
    echo " WARNING: CMake ${CMAKE_VER} may have compatibility issues."
    echo " Recommended: CMake 3.28.x – 3.30.x"
    echo " Proceeding, but dependency builds may fail."
    echo "================================================================"
    echo ""
fi

DISTRIBUTION=$(awk -F= '/^ID=/ {print $2}' /etc/os-release | tr -d '"')
DISTRIBUTION_LIKE=$(awk -F= '/^ID_LIKE=/ {print $2}' /etc/os-release | tr -d '"')
if [ "${DISTRIBUTION}" == "ubuntu" ] || [ "${DISTRIBUTION}" == "linuxmint" ] ; then
    DISTRIBUTION="debian"
elif [[ "${DISTRIBUTION_LIKE}" == *"debian"* ]] || [[ "${DISTRIBUTION_LIKE}" == *"ubuntu"* ]] ; then
    DISTRIBUTION="debian"
elif [[ "${DISTRIBUTION_LIKE}" == *"arch"* ]] ; then
    DISTRIBUTION="arch"
fi

if [ ! -f "./scripts/linux.d/${DISTRIBUTION}" ] ; then
    echo "Your distribution \"${DISTRIBUTION}\" is not supported by system-dependency scripts in ./scripts/linux.d/"
    echo "Please resolve dependencies manually and contribute a script for your distribution to upstream."
    exit 1
else
    echo "resolving system dependencies for distribution \"${DISTRIBUTION}\" ..."
    # shellcheck source=/dev/null
    source "./scripts/linux.d/${DISTRIBUTION}"
fi

echo "FOUND_GTK3=${FOUND_GTK3}"
if [[ -z "${FOUND_GTK3_DEV}" ]] ; then
    echo "Error, you must install the dependencies before."
    echo "Use option -u with sudo"
    exit 1
fi

echo "Changing date in version..."
{
    sed --in-place "s/+UNKNOWN/_$(date '+%F')/" version.inc
}
echo "done"

if [[ -z "${SKIP_RAM_CHECK}" ]] ; then
    check_available_memory_and_disk
fi

# Compiler setup
export CMAKE_C_CXX_COMPILER_CLANG=()
if [[ -n "${USE_CLANG}" ]] ; then
    export CMAKE_C_CXX_COMPILER_CLANG=(-DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++)
fi

export CMAKE_LLD_LINKER_ARGS=()
if [[ -n "${USE_LLD}" ]] ; then
    if command -v ld.lld >/dev/null 2>&1 ; then
        LLD_BIN=$(command -v ld.lld)
        export CMAKE_LLD_LINKER_ARGS=(-DCMAKE_LINKER="${LLD_BIN}" -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=lld)
    else
        echo "Error: ld.lld not found. Please install the 'lld' package (e.g., sudo apt install lld) or omit -L."
        exit 1
    fi
fi

# ============================================================
# 9. Sync repos, patch, prepare sources
# ============================================================
sync_gitlab_repos
patch_cmake_for_local_sources
prepare_deps_src
verify_repos

# ============================================================
# 10. Build dependencies (from vendor/gitlab_deps)
#     wxWidgets and Sentry use SOURCE_DIR (patched above) so
#     ExternalProject skips git-clone and builds from local repos.
#     Other deps (Boost, OpenCV, etc.) use DL_CACHE for offline build.
# ============================================================
if [[ -n "${BUILD_DEPS}" ]] ; then
    echo "============================================================"
    echo "  Building dependencies from vendor/gitlab_deps..."
    echo "  Source: ${DEPS_DIR}"
    echo "============================================================"

    DEPS_BUILD_DIR="${DEPS_DIR}/build"
    DEPS="${DEPS_BUILD_DIR}/OrcaSlicer_dep"

    read -r -a BUILD_ARGS <<< "${DEPS_EXTRA_BUILD_ARGS}"
    BUILD_ARGS+=(-DDEP_WX_GTK3=ON)

    if [[ -n "${CLEAN_BUILD}" ]] ; then
        rm -fr "${DEPS_BUILD_DIR}"
    fi
    mkdir -p "${DEPS_BUILD_DIR}"

    if [[ -n "${BUILD_DEBUG}" ]] ; then
        # build deps with release first else cmake will not find required sources
        mkdir -p "${DEPS_BUILD_DIR}/release"
        set -x
        cmake "${DEPS_DIR}" -B "${DEPS_BUILD_DIR}/release" \
            "${CMAKE_C_CXX_COMPILER_CLANG[@]}" "${CMAKE_LLD_LINKER_ARGS[@]}" \
            -G Ninja \
            -DSLIC3R_PCH="${SLIC3R_PRECOMPILED_HEADERS}" \
            -DDESTDIR="${DEPS}" \
            "${COLORED_OUTPUT}" \
            "${BUILD_ARGS[@]}"
        set +x
        cmake --build "${DEPS_BUILD_DIR}/release"
        BUILD_ARGS+=(-DCMAKE_BUILD_TYPE=Debug)
    fi

    set -x
    cmake "${DEPS_DIR}" -B "${DEPS_BUILD_DIR}" \
        "${CMAKE_C_CXX_COMPILER_CLANG[@]}" "${CMAKE_LLD_LINKER_ARGS[@]}" \
        -G Ninja \
        -DSLIC3R_PCH="${SLIC3R_PRECOMPILED_HEADERS}" \
        -DDESTDIR="${DEPS}" \
        "${COLORED_OUTPUT}" \
        "${BUILD_ARGS[@]}"
    set +x
    cmake --build "${DEPS_BUILD_DIR}"

    echo "  Deps build complete."
    echo
fi

# ============================================================
# 11. Build slicer
#     Uses gitlab_deps_src/ via -DDEPS_SRC_DIR=gitlab_deps_src
#     (copied from vendor above, original deps_src/ untouched).
# ============================================================
if [[ -n "${BUILD_ORCA}" ]] ; then
    echo "============================================================"
    echo "  Building Snapmaker_Orca slicer..."
    echo "  Build: ${GITLAB_BUILD_DIR}"
    echo "============================================================"

    # Ensure DEPS is set (for slicer-only mode)
    if [ -z "${DEPS}" ]; then
        DEPS="${DEPS_DIR}/build/OrcaSlicer_dep"
    fi

    if [[ -n "${CLEAN_BUILD}" ]] ; then
        rm -fr "${GITLAB_BUILD_DIR}"
    fi
    mkdir -p "${GITLAB_BUILD_DIR}"

    read -r -a BUILD_ARGS <<< "${ORCA_EXTRA_BUILD_ARGS}"
    if [[ -n "${FOUND_GTK3_DEV}" ]] ; then
        BUILD_ARGS+=(-DSLIC3R_GTK=3)
    fi
    if [[ -n "${BUILD_DEBUG}" ]] ; then
        BUILD_ARGS+=(-DCMAKE_BUILD_TYPE=Debug -DBBL_INTERNAL_TESTING=1)
    else
        BUILD_ARGS+=(-DBBL_RELEASE_TO_PUBLIC=1 -DBBL_INTERNAL_TESTING=0)
    fi
    if [[ -n "${BUILD_TESTS}" ]] ; then
        BUILD_ARGS+=(-DBUILD_TESTS=ON)
    fi
    if [[ -n "${ORCA_UPDATER_SIG_KEY}" ]] ; then
        BUILD_ARGS+=(-DORCA_UPDATER_SIG_KEY="${ORCA_UPDATER_SIG_KEY}")
    fi

    set -x
    cmake "${ORCA_ROOT}" -B "${GITLAB_BUILD_DIR}" \
        "${CMAKE_C_CXX_COMPILER_CLANG[@]}" "${CMAKE_LLD_LINKER_ARGS[@]}" \
        -G "Ninja Multi-Config" \
        -DSLIC3R_PCH="${SLIC3R_PRECOMPILED_HEADERS}" \
        -DDEPS_SRC_DIR=gitlab_deps_src \
        -DCMAKE_PREFIX_PATH="${DEPS}/usr/local" \
        -DSLIC3R_STATIC=1 \
        -DORCA_TOOLS=ON \
        "${COLORED_OUTPUT}" \
        "${BUILD_ARGS[@]}"
    set +x

    echo "Building Snapmaker_Orca ..."
    if [[ -n "${BUILD_DEBUG}" ]] ; then
        cmake --build "${GITLAB_BUILD_DIR}" --config Debug --target Snapmaker_Orca
    else
        cmake --build "${GITLAB_BUILD_DIR}" --config Release --target Snapmaker_Orca
    fi

    echo "Building Snapmaker_Orca_profile_validator ..."
    if [[ -n "${BUILD_DEBUG}" ]] ; then
        cmake --build "${GITLAB_BUILD_DIR}" --config Debug --target Snapmaker_Orca_profile_validator
    else
        cmake --build "${GITLAB_BUILD_DIR}" --config Release --target Snapmaker_Orca_profile_validator
    fi

    ./scripts/run_gettext.sh
    echo "  Slicer build complete."
    echo
fi

# ============================================================
# 12. Build AppImage (optional, -i flag)
# ============================================================
if [[ -n "${BUILD_IMAGE}" || -n "${BUILD_ORCA}" ]] ; then
    pushd "${GITLAB_BUILD_DIR}" > /dev/null
    echo "[9/9] Generating Linux app..."
    build_linux_image="./src/build_linux_image.sh"
    if [[ -e ${build_linux_image} ]] ; then
        extra_script_args=""
        if [[ -n "${BUILD_IMAGE}" ]] ; then
            extra_script_args="-i"
        fi
        ${build_linux_image} ${extra_script_args}
        echo "done"
    fi
    popd > /dev/null # ${GITLAB_BUILD_DIR}
fi

echo ""
echo "============================================================"
echo "  Build completed successfully!"
echo "============================================================"
echo "  Deps:     ${DEPS_DIR}/build/"
echo "  Slicer:   ${GITLAB_BUILD_DIR}/"
echo "============================================================"

popd > /dev/null # ${SCRIPT_PATH}
