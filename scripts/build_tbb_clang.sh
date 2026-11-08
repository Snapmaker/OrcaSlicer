#!/usr/bin/env bash
# Build oneTBB 2021.5.0 as static libraries with Clang.
#
# Used by the weekly sanitizer smoke job. oneTBB must be rebuilt with the same
# Clang toolchain as the sanitized OrcaSlicer build (the prebuilt GCC TBB in the
# deps cache is ABI-incompatible under ASan/UBSan), and LTO must be disabled so
# sanitizer instrumentation is preserved through static linking.
#
# Environment variables:
#   TBB_CACHE_ROOT    install root (default: /data/orca-cache/orcaslicer/tbb-clang)
#   TBB_CLANG_KEY     cache subdirectory key (default: clang-<major>-<arch>-static)
#   TBB_CC            C compiler   (default: clang)
#   TBB_CXX           C++ compiler (default: clang++)
#   TBB_BUILD_TYPE    build type   (default: RelWithDebInfo)
#   TBB_JOBS          parallel jobs (default: nproc)
#   TBB_DOWNLOAD_DIR  where to cache the source archive
#   TBB_SRC_DIR       existing oneTBB source tree (skips download/extract)
#   TBB_FORCE_REBUILD set to 1 to rebuild even when the install already exists
#
# This script never touches /data/orca-cache/orcaslicer/deps.

set -euo pipefail

TBB_VERSION=2021.5.0
TBB_URL="https://github.com/oneapi-src/oneTBB/archive/refs/tags/v${TBB_VERSION}.zip"
TBB_SHA256=83ea786c964a384dd72534f9854b419716f412f9d43c0be88d41874763e7bb47

log()  { printf '[build_tbb_clang] %s\n' "$*"; }
fail() { printf '[build_tbb_clang] ERROR: %s\n' "$*" >&2; exit 1; }

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    sed -n '2,20p' "$0"
    exit 0
fi

CC=${TBB_CC:-clang}
CXX=${TBB_CXX:-clang++}
BUILD_TYPE=${TBB_BUILD_TYPE:-RelWithDebInfo}
NJOBS=${TBB_JOBS:-$(nproc)}
CACHE_ROOT=${TBB_CACHE_ROOT:-/data/orca-cache/orcaslicer/tbb-clang}
FORCE_REBUILD=${TBB_FORCE_REBUILD:-0}

# Parameter checks.
for tool in cmake ninja curl unzip sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool not found: $tool"
done
command -v "$CC" >/dev/null 2>&1 || fail "C compiler not found: $CC (set TBB_CC)"
command -v "$CXX" >/dev/null 2>&1 || fail "C++ compiler not found: $CXX (set TBB_CXX)"
[[ -n "$CACHE_ROOT" && "$CACHE_ROOT" != "/" ]] || fail "TBB_CACHE_ROOT must be a non-empty path other than '/'"

clang_major=$("$CXX" --version | sed -n '1s/.*version \([0-9]*\).*/\1/p')
[[ -n "$clang_major" ]] || fail "could not determine Clang major version from: $CXX"
arch=$(uname -m)
KEY=${TBB_CLANG_KEY:-tbb-${TBB_VERSION}-clang-${clang_major}-${arch}-static}
[[ "$KEY" =~ ^[A-Za-z0-9._-]+$ ]] || fail "TBB_CLANG_KEY contains unsupported characters: $KEY"

INSTALL_PREFIX="${CACHE_ROOT}/${KEY}"
BUILD_DIR="${CACHE_ROOT}/.build/${KEY}"
DOWNLOAD_DIR=${TBB_DOWNLOAD_DIR:-${CACHE_ROOT}/downloads}
DEFAULT_SRC_DIR="${CACHE_ROOT}/src/oneTBB-${TBB_VERSION}"
SRC_DIR=${TBB_SRC_DIR:-${DEFAULT_SRC_DIR}}

# An externally supplied source tree must already look like oneTBB. This keeps
# a typo such as TBB_SRC_DIR=/home/snapmaker from being replaced by rm -rf below.
if [[ -n "${TBB_SRC_DIR:-}" ]] &&
   { [[ ! -f "${SRC_DIR}/CMakeLists.txt" ]] || [[ ! -f "${SRC_DIR}/include/tbb/tbb.h" ]]; }; then
    fail "TBB_SRC_DIR must be an existing oneTBB source tree: ${SRC_DIR}"
fi

# Idempotent short-circuit: reuse a complete previous install unless a rebuild
# is explicitly requested.
if [[ "$FORCE_REBUILD" != "1" \
      && -f "${INSTALL_PREFIX}/lib/cmake/TBB/TBBConfig.cmake" \
      && -f "${INSTALL_PREFIX}/lib/libtbb.a" \
      && -f "${INSTALL_PREFIX}/lib/libtbbmalloc.a" ]]; then
    log "oneTBB already installed at ${INSTALL_PREFIX}; skipping (set TBB_FORCE_REBUILD=1 to rebuild)"
    echo "TBB_PREFIX::${INSTALL_PREFIX}"
    exit 0
fi

log "oneTBB ${TBB_VERSION} -> ${INSTALL_PREFIX} (CC=${CC}, CXX=${CXX}, key=${KEY})"
mkdir -p "${CACHE_ROOT}" "${DOWNLOAD_DIR}"

# Fetch and verify the source only when it is not already present.
if [[ ! -f "${SRC_DIR}/CMakeLists.txt" ]]; then
    zip_path="${DOWNLOAD_DIR}/oneTBB-${TBB_VERSION}.zip"
    if [[ ! -f "${zip_path}" ]]; then
        log "downloading ${TBB_URL}"
        curl -fL --retry 3 --retry-delay 5 -o "${zip_path}" "${TBB_URL}"
    fi
    log "verifying source archive"
    echo "${TBB_SHA256}  ${zip_path}" | sha256sum -c -
    # Replace only this script's cache-owned source path. Never delete a caller-supplied tree.
    [[ "${SRC_DIR}" == "${DEFAULT_SRC_DIR}" ]] || fail "refusing to replace caller-supplied TBB_SRC_DIR: ${SRC_DIR}"
    [[ "${SRC_DIR}" == "${CACHE_ROOT}"/* ]] || fail "refusing to remove outside TBB_CACHE_ROOT: ${SRC_DIR}"
    [[ "${SRC_DIR}" != "${CACHE_ROOT}" ]] || fail "refusing to remove TBB_CACHE_ROOT itself"
    rm -rf "${SRC_DIR}"
    mkdir -p "$(dirname "${SRC_DIR}")"
    log "extracting ${zip_path}"
    unzip -q "${zip_path}" -d "$(dirname "${SRC_DIR}")"
    extracted="$(dirname "${SRC_DIR}")/oneTBB-${TBB_VERSION}"
    if [[ -d "${extracted}" && "${extracted}" != "${SRC_DIR}" ]]; then
        mv "${extracted}" "${SRC_DIR}"
    fi
fi

log "configuring (BUILD_SHARED_LIBS=OFF, CMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF)"
cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DTBB_TEST=OFF \
    -DTBB_EXAMPLES=OFF \
    -DTBB4PY_BUILD=OFF

log "building and installing"
cmake --build "${BUILD_DIR}" --target install -- -j "${NJOBS}"

log "done: ${INSTALL_PREFIX}"
echo "TBB_PREFIX::${INSTALL_PREFIX}"
