#!/bin/bash

set -e
set -o pipefail

while getopts ":dpa:snt:xbc:1h" opt; do
  case "${opt}" in
    d )
        export BUILD_TARGET="deps"
        ;;
    p )
        export PACK_DEPS="1"
        ;;
    a )
        export ARCH="$OPTARG"
        ;;
    s )
        export BUILD_TARGET="slicer"
        ;;
    n )
        export NIGHTLY_BUILD="1"
        ;;
    t )
        export OSX_DEPLOYMENT_TARGET="$OPTARG"
        ;;
    x )
        export SLICER_CMAKE_GENERATOR="Ninja Multi-Config"
        export SLICER_BUILD_TARGET="all"
        export DEPS_CMAKE_GENERATOR="Ninja"
        ;;
    b )
        export BUILD_ONLY="1"
        ;;
    c )
        export BUILD_CONFIG="$OPTARG"
        ;;
    1 )
        export CMAKE_BUILD_PARALLEL_LEVEL=1
        ;;
    h ) echo "Usage: ./gitlab_build_release_macos.sh [-d]"
        echo "   -d: Build deps only"
        echo "   -a: Set ARCHITECTURE (arm64 or x86_64 or universal)"
        echo "   -s: Build slicer only"
        echo "   -n: Nightly build"
        echo "   -t: Specify minimum version of the target platform, default is 12.0"
        echo "   -x: Use Ninja Multi-Config CMake generator, default is Xcode"
        echo "   -b: Build without reconfiguring CMake"
        echo "   -c: Set CMake build configuration, default is Release"
        echo "   -1: Use single job for building"
        exit 0
        ;;
    * )
        ;;
  esac
done

# ============================================================
# GitLab repository configuration
# ============================================================
if [ -z "$GITLAB_BASE_URL" ]; then
    export GITLAB_BASE_URL="gitlab.s.com"
fi
if [ -z "$GITLAB_GROUP" ]; then
    export GITLAB_GROUP="snapmaker_orca"
fi
if [ -z "$LOCAL_REF_BASE" ]; then
    export LOCAL_REF_BASE="/Users/snapmaker/Documents/gitlab_pro/OrcaSlicer"
fi

GITLAB_VENDOR="git@${GITLAB_BASE_URL}:${GITLAB_GROUP}/snapmaker_orca_vendor.git"
GITLAB_WXWIDGETS="git@${GITLAB_BASE_URL}:${GITLAB_GROUP}/Orca-deps-wxWidgets.git"
GITLAB_SENTRY="git@${GITLAB_BASE_URL}:${GITLAB_GROUP}/sentry-native.git"

ORCA_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${ORCA_ROOT}"
VENDOR_DIR="${ORCA_ROOT}/gitlab_vendor"
WXWIDGETS_DIR="${ORCA_ROOT}/Orca-deps-wxWidgets"
SENTRY_DIR="${ORCA_ROOT}/sentry-native"

# ============================================================
# Restore patched cmake files on script exit
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
# Clone / sync GitLab sub-repositories
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
        git clone --depth 50 --reference-if-able "${LOCAL_REF_BASE}/gitlab_vendor" "${GITLAB_VENDOR}" "${VENDOR_DIR}"
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
        git clone --depth 50 --reference-if-able "${LOCAL_REF_BASE}/Orca-deps-wxWidgets" "${GITLAB_WXWIDGETS}" "${WXWIDGETS_DIR}"
    fi

    # --- sentry-native ---
    if [ -d "${SENTRY_DIR}/.git" ]; then
        echo "  sentry: pulling latest..."
        pushd "${SENTRY_DIR}" > /dev/null
        git pull --ff-only
        popd > /dev/null
    else
        rm -rf "${SENTRY_DIR}"
        echo "  sentry: cloning ${GITLAB_SENTRY} ..."
        git clone --depth 50 --reference-if-able "${LOCAL_REF_BASE}/sentry-native" "${GITLAB_SENTRY}" "${SENTRY_DIR}"
    fi

    # --- sentry-native submodule (crashpad) ---
    echo "  sentry: fixing submodule URLs (HTTP -> SSH) and initializing..."
    pushd "${SENTRY_DIR}" > /dev/null
    # Use git global url.insteadOf to redirect ALL HTTP URLs to SSH.
    # This handles nested submodules (crashpad has 3 sub-submodules)
    # that also use HTTP in their own .gitmodules files.
    # Keep --global (git -c does not reach nested submodule git processes).
    # Always unset on the way out so a failed submodule update cannot leak
    # the redirect into the user's global git config. Restore a pre-existing
    # value if the machine already had the same insteadOf key.
    INSTEADOF_OLD="$(git config --global --get "url.git@${GITLAB_BASE_URL}:.insteadOf" 2>/dev/null || true)"
    restore_insteadOf() {
        if [ -n "${INSTEADOF_OLD}" ]; then
            git config --global "url.git@${GITLAB_BASE_URL}:.insteadOf" "${INSTEADOF_OLD}"
        else
            git config --global --unset "url.git@${GITLAB_BASE_URL}:.insteadOf" 2>/dev/null || true
        fi
    }
    git config --global url."git@${GITLAB_BASE_URL}:".insteadOf "http://${GITLAB_BASE_URL}/"
    submodule_failed=0
    git submodule update --init --recursive --depth 1 || submodule_failed=1
    restore_insteadOf
    if [ "${submodule_failed}" -ne 0 ]; then
        popd > /dev/null
        echo "  sentry: submodule update failed"
        exit 1
    fi
    popd > /dev/null

    echo "  All repos synced successfully."
    echo
}

# ============================================================
# Patch vendor cmake files to use local source dirs
# Instead of letting ExternalProject git-clone from GitHub,
# we point it to the already-cloned local repos from GitLab.
# ============================================================
function patch_cmake_for_local_sources() {
    echo "Patching cmake files for local sources..."

    local WX_CMAKE="${VENDOR_DIR}/gitlab_deps/wxWidgets/wxWidgets.cmake"
    local SENTRY_CMAKE="${VENDOR_DIR}/gitlab_deps/Sentry/Sentry.cmake"

    # wxWidgets: replace GIT_REPOSITORY with SOURCE_DIR, remove GIT_SHALLOW
    if [ -f "$WX_CMAKE" ]; then
        sed -i '' \
            -e 's|GIT_REPOSITORY "https://github\.com/SoftFever/Orca-deps-wxWidgets"|SOURCE_DIR "'"${WXWIDGETS_DIR}"'"|' \
            -e '/GIT_SHALLOW ON/d' \
            "$WX_CMAKE"
        echo "  wxWidgets -> SOURCE_DIR"
    fi

    # Sentry: replace GIT_REPOSITORY with SOURCE_DIR, remove GIT_TAG + GIT_SHALLOW
    if [ -f "$SENTRY_CMAKE" ]; then
        sed -i '' -E \
            -e 's|GIT_REPOSITORY[[:space:]]+https://github\.com/getsentry/sentry-native\.git|SOURCE_DIR "'"${SENTRY_DIR}"'"|' \
            -e '/GIT_TAG[[:space:]]+[0-9.]+/d' \
            -e '/GIT_SHALLOW[[:space:]]+ON/d' \
            "$SENTRY_CMAKE"
        echo "  Sentry    -> SOURCE_DIR"
    fi

    # OpenVDB: fix Apple Clang 17+ compatibility (-Wmissing-template-arg-list-after-template-kw)
    local OPENVDB_CMAKE="${VENDOR_DIR}/gitlab_deps/OpenVDB/OpenVDB.cmake"
    if [ -f "$OPENVDB_CMAKE" ]; then
        if grep -q "Wno-missing-template-arg-list-after-template-kw" "$OPENVDB_CMAKE"; then
            echo "  OpenVDB   -> already patched"
        else
            sed -i '' 's|-DDISABLE_DEPENDENCY_VERSION_CHECKS=ON|-DDISABLE_DEPENDENCY_VERSION_CHECKS=ON\n        -DCMAKE_CXX_FLAGS=-Wno-missing-template-arg-list-after-template-kw|' "$OPENVDB_CMAKE"
            echo "  OpenVDB   -> add Clang 17 compat flag"
        fi
    fi

    echo
}

# ============================================================
# Prepare gitlab_deps_src from vendor
# The slicer build uses deps_src/ via add_subdirectory().
# We copy the vendor's version to gitlab_deps_src/ and
# pass -DDEPS_SRC_DIR=gitlab_deps_src to CMake.
# The original deps_src/ is NOT touched.
# ============================================================
function prepare_deps_src() {
    echo "Preparing vendored library sources..."

    if [ -d "${VENDOR_DIR}/gitlab_deps_src" ]; then
        # Remove stale copy from previous run
        if [ -d "${ORCA_ROOT}/gitlab_deps_src" ]; then
            echo "  Removing stale gitlab_deps_src/..."
            rm -rf "${ORCA_ROOT}/gitlab_deps_src"
        fi

        # Copy vendor version to root as gitlab_deps_src/
        echo "  Copying vendor/gitlab_deps_src/ to gitlab_deps_src/..."
        cp -R "${VENDOR_DIR}/gitlab_deps_src" "${ORCA_ROOT}/gitlab_deps_src"
    else
        echo "  [WARNING] Vendor repo has no gitlab_deps_src/, using original deps_src/"
    fi

    echo "  deps_src ready."
    echo
}

# ============================================================
# Set build defaults
# ============================================================

if [ -z "$ARCH" ]; then
    ARCH="$(uname -m)"
    export ARCH
fi

if [ -z "$BUILD_CONFIG" ]; then
  export BUILD_CONFIG="Release"
fi

if [ -z "$BUILD_TARGET" ]; then
  export BUILD_TARGET="all"
fi

if [ -z "$SLICER_CMAKE_GENERATOR" ]; then
  export SLICER_CMAKE_GENERATOR="Xcode"
fi

if [ -z "$SLICER_BUILD_TARGET" ]; then
  export SLICER_BUILD_TARGET="ALL_BUILD"
fi

if [ -z "$DEPS_CMAKE_GENERATOR" ]; then
  export DEPS_CMAKE_GENERATOR="Unix Makefiles"
fi

if [ -z "$OSX_DEPLOYMENT_TARGET" ]; then
  export OSX_DEPLOYMENT_TARGET="12.0"
fi

echo "Build params:"
echo " - ARCH: $ARCH"
echo " - BUILD_CONFIG: $BUILD_CONFIG"
echo " - BUILD_TARGET: $BUILD_TARGET"
echo " - CMAKE_GENERATOR: $SLICER_CMAKE_GENERATOR for Slicer, $DEPS_CMAKE_GENERATOR for deps"
echo " - OSX_DEPLOYMENT_TARGET: $OSX_DEPLOYMENT_TARGET"
echo

# ============================================================
# Paths for GitLab build
# deps source: gitlab_vendor/gitlab_deps (patched cmake files)
# deps build:  gitlab_vendor/gitlab_deps/build/$ARCH
# deps output: gitlab_vendor/gitlab_deps/build/$ARCH/OrcaSlicer_dep
# slicer build: gitlab_build/$ARCH
# slicer output: gitlab_build/$ARCH/Snapmaker_Orca/
# ============================================================
DEPS_DIR="${ORCA_ROOT}/gitlab_vendor/gitlab_deps"
DEPS_BUILD_DIR="$DEPS_DIR/build/$ARCH"
DEPS="$DEPS_BUILD_DIR/OrcaSlicer_dep"
PROJECT_BUILD_DIR="$ORCA_ROOT/gitlab_build/$ARCH"

# For Multi-config generators like Ninja and Xcode
export BUILD_DIR_CONFIG_SUBDIR="/$BUILD_CONFIG"

function build_deps() {
    # iterate over two architectures: x86_64 and arm64
    for _ARCH in x86_64 arm64; do
        # if ARCH is universal or equal to _ARCH
        if [ "$ARCH" == "universal" ] || [ "$ARCH" == "$_ARCH" ]; then

            DEPS_BUILD_DIR="$DEPS_DIR/build/$_ARCH"
            DEPS="$DEPS_BUILD_DIR/OrcaSlicer_dep"

            echo "Building deps for $_ARCH..."
            (
                set -x
                mkdir -p "$DEPS"
                cd "$DEPS_BUILD_DIR"
                if [ "1." != "$BUILD_ONLY". ]; then
                    cmake "${DEPS_DIR}" \
                        -G "${DEPS_CMAKE_GENERATOR}" \
                        -DDESTDIR="$DEPS" \
                        -DOPENSSL_ARCH="darwin64-${_ARCH}-cc" \
                        -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" \
                        -DCMAKE_OSX_ARCHITECTURES:STRING="${_ARCH}" \
                        -DCMAKE_OSX_DEPLOYMENT_TARGET="${OSX_DEPLOYMENT_TARGET}"
                fi
                cmake --build . --config "$BUILD_CONFIG" --target deps
            )
        fi
    done
}

function pack_deps() {
    echo "Packing deps..."
    (
        set -x
        cd "$DEPS_DIR"
        tar -zcvf "OrcaSlicer_dep_mac_${ARCH}_$(date +"%Y%m%d").tar.gz" "build"
    )
}

function build_slicer() {
    # iterate over two architectures: x86_64 and arm64
    for _ARCH in x86_64 arm64; do
        # if ARCH is universal or equal to _ARCH
        if [ "$ARCH" == "universal" ] || [ "$ARCH" == "$_ARCH" ]; then

            PROJECT_BUILD_DIR="$ORCA_ROOT/gitlab_build/$_ARCH"
            DEPS_BUILD_DIR="$DEPS_DIR/build/$_ARCH"
            DEPS="$DEPS_BUILD_DIR/OrcaSlicer_dep"

            echo "Building slicer for $_ARCH..."
            (
                set -x
            mkdir -p "$PROJECT_BUILD_DIR"
            cd "$PROJECT_BUILD_DIR"
            if [ "1." != "$BUILD_ONLY". ]; then
                cmake "${ORCA_ROOT}" \
                    -G "${SLICER_CMAKE_GENERATOR}" \
                    -DBBL_RELEASE_TO_PUBLIC=1 \
                    -DORCA_TOOLS=ON \
                    ${ORCA_UPDATER_SIG_KEY:+-DORCA_UPDATER_SIG_KEY="$ORCA_UPDATER_SIG_KEY"} \
                    -DDEPS_SRC_DIR=gitlab_deps_src \
                    -DCMAKE_PREFIX_PATH="$DEPS/usr/local" \
                    -DCMAKE_INSTALL_PREFIX="$PWD/Snapmaker_Orca" \
                    -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" \
                    -DCMAKE_MACOSX_RPATH=ON \
                    -DCMAKE_INSTALL_RPATH="${DEPS}/usr/local" \
                    -DCMAKE_MACOSX_BUNDLE=ON \
                    -DCMAKE_OSX_ARCHITECTURES="${_ARCH}" \
                    -DCMAKE_OSX_DEPLOYMENT_TARGET="${OSX_DEPLOYMENT_TARGET}"
            fi
            cmake --build . --config "$BUILD_CONFIG" --target "$SLICER_BUILD_TARGET"
            # Explicitly build profile_validator if ORCA_TOOLS is enabled
            if [ "$SLICER_BUILD_TARGET" = "all" ] || [ "$SLICER_BUILD_TARGET" = "ALL_BUILD" ]; then
                cmake --build . --config "$BUILD_CONFIG" --target Snapmaker_Orca_profile_validator || echo "Warning: Snapmaker_Orca_profile_validator build failed or not available"
            fi
        )

        echo "Verify localization with gettext..."
        (
            cd "$PROJECT_DIR"
            ./scripts/run_gettext.sh
        )

    echo "Fix macOS app package..."
    (
        cd "$PROJECT_BUILD_DIR"
        mkdir -p Snapmaker_Orca
        cd Snapmaker_Orca
        # remove previously built app
        rm -rf "./Snapmaker Orca.app"
        # determine source app path (handle both space and underscore names)
        APP_SOURCE_PATH="../src$BUILD_DIR_CONFIG_SUBDIR/Snapmaker Orca.app"
        if [ ! -d "$APP_SOURCE_PATH" ]; then
            APP_SOURCE_PATH="../src$BUILD_DIR_CONFIG_SUBDIR/Snapmaker_Orca.app"
        fi
        if [ ! -d "$APP_SOURCE_PATH" ]; then
            echo "Error: cannot find built app bundle at $APP_SOURCE_PATH"
            exit 1
        fi
        # fully copy newly built app (rename to canonical name with space)
        cp -pR "$APP_SOURCE_PATH" "./Snapmaker Orca.app"
        # fix resources
        resources_path=$(readlink "./Snapmaker Orca.app/Contents/Resources")
        rm "./Snapmaker Orca.app/Contents/Resources"
        cp -R "$resources_path" "./Snapmaker Orca.app/Contents/Resources"
        # delete .DS_Store file
        find "./Snapmaker Orca.app/" -name '.DS_Store' -delete

        # Copy Sentry crashpad_handler and libsentry.dylib for crash reporting
        CRASHPAD_HANDLER="${DEPS}/usr/local/bin/crashpad_handler"
        LIBSENTRY="${DEPS}/usr/local/lib/libsentry.dylib"
        APP_MACOS_DIR='./Snapmaker Orca.app/Contents/MacOS'
        APP_FRAMEWORKS_DIR='./Snapmaker Orca.app/Contents/Frameworks'
        EXECUTABLE="${APP_MACOS_DIR}/Snapmaker_Orca"

        if [ -f "${CRASHPAD_HANDLER}" ]; then
            echo "Copying crashpad_handler to app bundle..."
            cp -f "${CRASHPAD_HANDLER}" "${APP_MACOS_DIR}/crashpad_handler"
            # Sign crashpad_handler
            codesign --force --sign - "${APP_MACOS_DIR}/crashpad_handler" 2>/dev/null || true
        else
            echo "Warning: crashpad_handler not found at ${CRASHPAD_HANDLER}"
        fi

        if [ -f "${LIBSENTRY}" ]; then
            echo "Copying libsentry.dylib to Frameworks..."
            mkdir -p "${APP_FRAMEWORKS_DIR}"
            cp -f "${LIBSENTRY}" "${APP_FRAMEWORKS_DIR}/libsentry.dylib"
            # Sign libsentry.dylib
            codesign --force --sign - "${APP_FRAMEWORKS_DIR}/libsentry.dylib" 2>/dev/null || true

            # Update rpath in Snapmaker_Orca to use @executable_path relative path
            if [ -f "${EXECUTABLE}" ]; then
                echo "Updating libsentry.dylib rpath in Snapmaker_Orca..."
                install_name_tool -change "@rpath/libsentry.dylib" "@executable_path/../Frameworks/libsentry.dylib" "${EXECUTABLE}" 2>/dev/null || true
                # Re-sign the executable after modification
                codesign --force --sign - "${EXECUTABLE}" 2>/dev/null || true
            fi
        else
            echo "Warning: libsentry.dylib not found at ${LIBSENTRY}"
        fi

        # Copy Snapmaker_Orca_profile_validator.app if it exists
        if [ -f "../src$BUILD_DIR_CONFIG_SUBDIR/Snapmaker_Orca_profile_validator.app/Contents/MacOS/Snapmaker_Orca_profile_validator" ]; then
            echo "Copying Snapmaker_Orca_profile_validator.app..."
            rm -rf ./Snapmaker_Orca_profile_validator.app
            cp -pR "../src$BUILD_DIR_CONFIG_SUBDIR/Snapmaker_Orca_profile_validator.app" ./Snapmaker_Orca_profile_validator.app
            # delete .DS_Store file
            find ./Snapmaker_Orca_profile_validator.app/ -name '.DS_Store' -delete
        fi

        # Generate dSYM debug symbols for debugging and Sentry crash reporting
        # Always generate dSYM files - they are useful for crash analysis even without Sentry
        echo "Generating dSYM debug symbols..."
        DSYM_DIR="./dSYM"
        mkdir -p "${DSYM_DIR}"

        # Generate dSYM for main app
        if [ -f "${APP_MACOS_DIR}/Snapmaker_Orca" ]; then
            echo "Generating dSYM for Snapmaker_Orca..."
            dsymutil "${APP_MACOS_DIR}/Snapmaker_Orca" -o "${DSYM_DIR}/Snapmaker_Orca.dSYM" 2>/dev/null || echo "Warning: Failed to generate dSYM for Snapmaker_Orca (no debug symbols?)"
        fi

        # Generate dSYM for crashpad_handler if it exists
        if [ -f "${APP_MACOS_DIR}/crashpad_handler" ]; then
            echo "Generating dSYM for crashpad_handler..."
            dsymutil "${APP_MACOS_DIR}/crashpad_handler" -o "${DSYM_DIR}/crashpad_handler.dSYM" 2>/dev/null || true
        fi

        # Generate dSYM for libsentry.dylib if it exists
        if [ -f "${APP_FRAMEWORKS_DIR}/libsentry.dylib" ]; then
            echo "Generating dSYM for libsentry.dylib..."
            dsymutil "${APP_FRAMEWORKS_DIR}/libsentry.dylib" -o "${DSYM_DIR}/libsentry.dSYM" 2>/dev/null || true
        fi

        # Generate dSYM for profile_validator if it exists
        if [ -f "./Snapmaker_Orca_profile_validator.app/Contents/MacOS/Snapmaker_Orca_profile_validator" ]; then
            echo "Generating dSYM for Snapmaker_Orca_profile_validator..."
            dsymutil "./Snapmaker_Orca_profile_validator.app/Contents/MacOS/Snapmaker_Orca_profile_validator" -o "${DSYM_DIR}/Snapmaker_Orca_profile_validator.dSYM" 2>/dev/null || true
        fi

        echo "dSYM files generated in ${DSYM_DIR}"
        ls -la "${DSYM_DIR}" 2>/dev/null || echo "No dSYM files generated"
    )

    fi
    done
}

function build_universal() {
    echo "Building universal binary..."

    PROJECT_BUILD_DIR="$ORCA_ROOT/gitlab_build/$ARCH"

    # Create universal binary
    echo "Creating universal binary..."
    mkdir -p "$PROJECT_BUILD_DIR/Snapmaker_Orca"
    UNIVERSAL_APP="$PROJECT_BUILD_DIR/Snapmaker_Orca/Snapmaker Orca.app"
    rm -rf "$UNIVERSAL_APP"
    cp -R "$ORCA_ROOT/gitlab_build/arm64/Snapmaker_Orca/Snapmaker Orca.app" "$UNIVERSAL_APP"

    # Get the binary path inside the .app bundle
    BINARY_PATH="Contents/MacOS/Snapmaker_Orca"

    # Create universal binary using lipo
    lipo -create \
        "$ORCA_ROOT/gitlab_build/x86_64/Snapmaker_Orca/Snapmaker Orca.app/$BINARY_PATH" \
        "$ORCA_ROOT/gitlab_build/arm64/Snapmaker_Orca/Snapmaker Orca.app/$BINARY_PATH" \
        -output "$UNIVERSAL_APP/$BINARY_PATH"

    echo "Universal binary created at $UNIVERSAL_APP"

    # Create universal crashpad_handler if both architectures have it
    CRASHPAD_ARM64="${ORCA_ROOT}/gitlab_build/arm64/Snapmaker_Orca/Snapmaker Orca.app/Contents/MacOS/crashpad_handler"
    CRASHPAD_X86="${ORCA_ROOT}/gitlab_build/x86_64/Snapmaker_Orca/Snapmaker Orca.app/Contents/MacOS/crashpad_handler"
    CRASHPAD_UNIVERSAL="${UNIVERSAL_APP}/Contents/MacOS/crashpad_handler"
    if [ -f "${CRASHPAD_ARM64}" ] && [ -f "${CRASHPAD_X86}" ]; then
        echo "Creating universal crashpad_handler..."
        lipo -create "${CRASHPAD_X86}" "${CRASHPAD_ARM64}" -output "${CRASHPAD_UNIVERSAL}"
        codesign --force --sign - "${CRASHPAD_UNIVERSAL}" 2>/dev/null || true
    elif [ -f "${CRASHPAD_ARM64}" ]; then
        cp -f "${CRASHPAD_ARM64}" "${CRASHPAD_UNIVERSAL}"
        codesign --force --sign - "${CRASHPAD_UNIVERSAL}" 2>/dev/null || true
    fi

    # Create universal libsentry.dylib if both architectures have it
    LIBSENTRY_ARM64="${ORCA_ROOT}/gitlab_build/arm64/Snapmaker_Orca/Snapmaker Orca.app/Contents/Frameworks/libsentry.dylib"
    LIBSENTRY_X86="${ORCA_ROOT}/gitlab_build/x86_64/Snapmaker_Orca/Snapmaker Orca.app/Contents/Frameworks/libsentry.dylib"
    LIBSENTRY_UNIVERSAL="${UNIVERSAL_APP}/Contents/Frameworks/libsentry.dylib"
    if [ -f "${LIBSENTRY_ARM64}" ] && [ -f "${LIBSENTRY_X86}" ]; then
        echo "Creating universal libsentry.dylib..."
        mkdir -p "${UNIVERSAL_APP}/Contents/Frameworks"
        lipo -create "${LIBSENTRY_X86}" "${LIBSENTRY_ARM64}" -output "${LIBSENTRY_UNIVERSAL}"
        codesign --force --sign - "${LIBSENTRY_UNIVERSAL}" 2>/dev/null || true
    elif [ -f "${LIBSENTRY_ARM64}" ]; then
        mkdir -p "${UNIVERSAL_APP}/Contents/Frameworks"
        cp -f "${LIBSENTRY_ARM64}" "${LIBSENTRY_UNIVERSAL}"
        codesign --force --sign - "${LIBSENTRY_UNIVERSAL}" 2>/dev/null || true
    fi

    # Update rpath in universal Snapmaker_Orca
    if [ -f "${LIBSENTRY_UNIVERSAL}" ]; then
        echo "Updating libsentry.dylib rpath in universal Snapmaker_Orca..."
        install_name_tool -change "@rpath/libsentry.dylib" "@executable_path/../Frameworks/libsentry.dylib" "${UNIVERSAL_APP}/${BINARY_PATH}" 2>/dev/null || true
        codesign --force --sign - "${UNIVERSAL_APP}/${BINARY_PATH}" 2>/dev/null || true
    fi

    # Create universal binary for profile validator if it exists
    if [ -f "$ORCA_ROOT/gitlab_build/arm64/Snapmaker_Orca/Snapmaker_Orca_profile_validator.app/Contents/MacOS/Snapmaker_Orca_profile_validator" ] && \
       [ -f "$ORCA_ROOT/gitlab_build/x86_64/Snapmaker_Orca/Snapmaker_Orca_profile_validator.app/Contents/MacOS/Snapmaker_Orca_profile_validator" ]; then
        echo "Creating universal binary for Snapmaker_Orca_profile_validator..."
        UNIVERSAL_VALIDATOR_APP="$PROJECT_BUILD_DIR/Snapmaker_Orca/Snapmaker_Orca_profile_validator.app"
        rm -rf "$UNIVERSAL_VALIDATOR_APP"
        cp -R "$ORCA_ROOT/gitlab_build/arm64/Snapmaker_Orca/Snapmaker_Orca_profile_validator.app" "$UNIVERSAL_VALIDATOR_APP"

        # Get the binary path inside the profile validator .app bundle
        VALIDATOR_BINARY_PATH="Contents/MacOS/Snapmaker_Orca_profile_validator"

        # Create universal binary using lipo
        lipo -create \
            "$ORCA_ROOT/gitlab_build/x86_64/Snapmaker_Orca/Snapmaker_Orca_profile_validator.app/$VALIDATOR_BINARY_PATH" \
            "$ORCA_ROOT/gitlab_build/arm64/Snapmaker_Orca/Snapmaker_Orca_profile_validator.app/$VALIDATOR_BINARY_PATH" \
            -output "$UNIVERSAL_VALIDATOR_APP/$VALIDATOR_BINARY_PATH"

        echo "Universal binary for Snapmaker_Orca_profile_validator created at $UNIVERSAL_VALIDATOR_APP"
    fi

    # Generate dSYM for universal binary - always generate for debugging and Sentry crash reporting
    echo "Generating dSYM for universal binary..."
    DSYM_DIR="$PROJECT_BUILD_DIR/Snapmaker_Orca/dSYM"
    mkdir -p "${DSYM_DIR}"

    # Generate dSYM for universal main app
    if [ -f "$UNIVERSAL_APP/$BINARY_PATH" ]; then
        echo "Generating dSYM for universal Snapmaker_Orca..."
        dsymutil "$UNIVERSAL_APP/$BINARY_PATH" -o "${DSYM_DIR}/Snapmaker_Orca.dSYM" 2>/dev/null || echo "Warning: Failed to generate dSYM for universal Snapmaker_Orca"
    fi

    # Generate dSYM for universal crashpad_handler if it exists
    if [ -f "$UNIVERSAL_APP/Contents/MacOS/crashpad_handler" ]; then
        echo "Generating dSYM for universal crashpad_handler..."
        dsymutil "$UNIVERSAL_APP/Contents/MacOS/crashpad_handler" -o "${DSYM_DIR}/crashpad_handler.dSYM" 2>/dev/null || true
    fi

    # Generate dSYM for universal libsentry.dylib if it exists
    if [ -f "$UNIVERSAL_APP/Contents/Frameworks/libsentry.dylib" ]; then
        echo "Generating dSYM for universal libsentry.dylib..."
        dsymutil "$UNIVERSAL_APP/Contents/Frameworks/libsentry.dylib" -o "${DSYM_DIR}/libsentry.dSYM" 2>/dev/null || true
    fi

    # Generate dSYM for universal profile_validator if it exists
    if [ -f "$UNIVERSAL_VALIDATOR_APP/$VALIDATOR_BINARY_PATH" ]; then
        echo "Generating dSYM for universal Snapmaker_Orca_profile_validator..."
        dsymutil "$UNIVERSAL_VALIDATOR_APP/$VALIDATOR_BINARY_PATH" -o "${DSYM_DIR}/Snapmaker_Orca_profile_validator.dSYM" 2>/dev/null || true
    fi

    echo "Universal dSYM files generated in ${DSYM_DIR}"
    ls -la "${DSYM_DIR}" 2>/dev/null || echo "No dSYM files generated"
}

# ============================================================
# GitLab build: sync repos, patch cmake, prepare sources
# ============================================================
sync_gitlab_repos
patch_cmake_for_local_sources
prepare_deps_src

case "${BUILD_TARGET}" in
    all)
        build_deps
        build_slicer
        ;;
    deps)
        build_deps
        ;;
    slicer)
        build_slicer
        ;;
    *)
        echo "Unknown target: $BUILD_TARGET. Available targets: deps, slicer, all."
        exit 1
        ;;
esac

if [ "$ARCH" = "universal" ] && [ "$BUILD_TARGET" != "deps" ]; then
    build_universal
fi

if [ "1." == "$PACK_DEPS". ]; then
    pack_deps
fi
