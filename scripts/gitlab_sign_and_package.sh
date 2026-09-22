#!/bin/bash

# Sign, package, and notarize the macOS app (GitLab / Jenkins path).
# Usage: ./scripts/gitlab_sign_and_package.sh [arm64|x86_64] [app_path]

set -e

# Detect architecture
ARCH="${1:-$(uname -m)}"

# Normalize architecture name
case "$ARCH" in
    arm64|aarch64)
        ARCH="arm64"
        ;;
    x86_64|x86-64|amd64)
        ARCH="x86_64"
        ;;
    *)
        echo "Error: unsupported architecture $ARCH"
        echo "Usage: $0 [arm64|x86_64] [app_path]"
        exit 1
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/gitlab_build/$ARCH"
APP_NAME="Snapmaker_Orca"
APP_NAME_EX="Snapmaker Orca"
DMG_NAME="Snapmaker_Orca_${ARCH}.dmg"

ENTITLEMENTS="$PROJECT_DIR/scripts/disable_validation.entitlements"


CERTIFICATE_ID="${CERTIFICATE_ID:-}"
NOTARY_APPLE_ID="${NOTARY_APPLE_ID:-}"
NOTARY_TEAM_ID="${NOTARY_TEAM_ID:-}"
NOTARY_PASSWORD="${NOTARY_PASSWORD:-}"
NOTARY_KEYCHAIN_PROFILE="${NOTARY_KEYCHAIN_PROFILE:-}"

# Ad-hoc re-sign is still required without a Developer ID: install_name_tool
# invalidates the existing signature, and skipping re-sign causes SIGKILL on
# macOS (Code Signature Invalid).
if [ -n "$CERTIFICATE_ID" ]; then
    SIGN_IDENTITY="$CERTIFICATE_ID"
    SIGN_MODE="Developer ID"
else
    SIGN_IDENTITY="-"
    SIGN_MODE="ad-hoc"
fi

ENABLE_NOTARY=0
if [ "$SIGN_IDENTITY" != "-" ]; then
    if [ -n "$NOTARY_KEYCHAIN_PROFILE" ]; then
        ENABLE_NOTARY=1
    elif [ -n "$NOTARY_APPLE_ID" ] && [ -n "$NOTARY_TEAM_ID" ] && [ -n "$NOTARY_PASSWORD" ]; then
        ENABLE_NOTARY=1
    fi
fi

echo "=========================================="
echo "macOS sign, package, and notarize"
echo "=========================================="
echo "Arch: $ARCH"
echo "Certificate: ${CERTIFICATE_ID:-not set, using ad-hoc signing}"
echo "TEAM_ID: ${NOTARY_TEAM_ID:-not set}"
echo "Signing: $SIGN_MODE"
echo "Notarization: $([ "$ENABLE_NOTARY" -eq 1 ] && echo enabled || echo skipped)"
echo "Project dir: $PROJECT_DIR"
echo

# codesign wrapper: Developer ID includes --timestamp; ad-hoc must not.
codesign_item() {
    if [ "$SIGN_IDENTITY" = "-" ]; then
        codesign --force --verbose --options runtime --sign "$SIGN_IDENTITY" "$@"
    else
        codesign --force --verbose --options runtime --timestamp --sign "$SIGN_IDENTITY" "$@"
    fi
}

# ============================================
# Locate the app
# ============================================

# App path provided as the second argument
if [ -n "$2" ]; then
    SOURCE_APP="$2"
    if [ ! -d "$SOURCE_APP" ]; then
        echo "Error: app not found: $SOURCE_APP"
        exit 1
    fi
    echo "Using specified app: $SOURCE_APP"
else
    # Auto-detect a built app
    for possible_path in \
        "$BUILD_DIR/src/Release/$APP_NAME.app" \
        "$BUILD_DIR/src/RelWithDebInfo/$APP_NAME.app" \
        "$BUILD_DIR/src/$APP_NAME.app" \
        "$BUILD_DIR/src/Debug/$APP_NAME.app"
    do
        if [ -d "$possible_path" ]; then
            SOURCE_APP="$possible_path"
            echo "Found app: $SOURCE_APP"
            break
        fi
    done

    # Also check the display name with a space
    if [ -z "$SOURCE_APP" ]; then
        for possible_path in \
            "$BUILD_DIR/src/Release/Snapmaker Orca.app" \
            "$BUILD_DIR/Snapmaker_Orca/Snapmaker Orca.app"
        do
            if [ -d "$possible_path" ]; then
                SOURCE_APP="$possible_path"
                echo "Found app: $SOURCE_APP"
                break
            fi
        done
    fi

    if [ -z "$SOURCE_APP" ]; then
        echo "Error: built $APP_NAME.app not found in $BUILD_DIR"
        echo "Build the $ARCH variant first: ./build_release_macos.sh -s -a $ARCH"
        exit 1
    fi
fi

# Temporary working directory
WORK_DIR="$BUILD_DIR/sign_package"
STAGING_DIR="$WORK_DIR/staging"
rm -rf "$WORK_DIR"
mkdir -p "$STAGING_DIR"

# ============================================
# Remove leftover Snapmaker volumes/directories.
# Covers three sources: hdiutil, diskutil, and leftover filesystem dirs.
# ============================================
cleanup_volumes() {
    local pattern="${1:-Snapmaker}"

    # Path 1: disk images managed by hdiutil
    hdiutil info 2>/dev/null | grep -oE "/Volumes/${pattern}[^\"]+" | sort -u | while read -r mp; do
        [ -d "$mp" ] || continue
        echo "  [hdiutil] Detach: $mp"
        hdiutil detach "$mp" -force 2>/dev/null || true
    done

    # Path 2: any leftover directory under /Volumes (including non-hdiutil / interrupted mounts)
    local glob_pattern="/Volumes/${pattern}*"
    # Suppress "No such file" errors
    shopt -s nullglob 2>/dev/null || true
    for mp in $glob_pattern; do
        [ -d "$mp" ] || continue
        echo "  [fs] Cleaning: $mp"
        diskutil unmount force "$mp" 2>/dev/null || true
        hdiutil detach "$mp" -force 2>/dev/null || true
        rmdir "$mp" 2>/dev/null || true
    done
    shopt -u nullglob 2>/dev/null || true

    sleep 2
    # Clear mount cache
    rm -rf ~/Library/Caches/com.apple.dt.Xcode/Volumes 2>/dev/null || true
}

# Detach leftover volumes before work starts
echo "Cleaning leftover mount points..."
cleanup_volumes

# Copy the app into the working directory
echo
echo "=========================================="
echo "Step 1/6: Copy app"
echo "=========================================="
echo "Copying app to the working directory..."
cp -R "$SOURCE_APP" "$STAGING_DIR/$APP_NAME.app"
FINAL_APP="$STAGING_DIR/$APP_NAME.app"

# Remove .DS_Store files
find "$FINAL_APP" -name '.DS_Store' -delete
# Remove redundant PkgInfo
rm -f "$FINAL_APP/Contents/PkgInfo" 2>/dev/null || true

# Strip extended attributes (including com.apple.quarantine) to avoid Gatekeeper issues
echo "Clearing extended attributes..."
xattr -cr "$FINAL_APP" 2>/dev/null || {
    echo "  Some xattrs could not be cleared (safe to ignore)"
    find "$FINAL_APP" -type f -exec sh -c 'xattr -c "$1" 2>/dev/null || true' _ {} \;
}

# ============================================
# Bundle external dylibs
# ============================================

APP_MACOS_DIR="$FINAL_APP/Contents/MacOS"
APP_FRAMEWORKS_DIR="$FINAL_APP/Contents/Frameworks"
EXECUTABLE="$APP_MACOS_DIR/$APP_NAME"

# Ensure Frameworks exists
mkdir -p "$APP_FRAMEWORKS_DIR"

echo
echo "Checking and bundling external libraries..."

# GitHub release packages do not ship Homebrew libzstd*.dylib.
# Local builds that link /opt/homebrew/.../libzstd.1.5.7.dylib must not copy
# it into Frameworks either: that is a builder-specific version, and user
# machines will crash if the load path still points at Homebrew.
should_skip_bundle_lib() {
    case "$1" in
        libzstd*) return 0 ;;
        *) return 1 ;;
    esac
}

# Find non-system external dependencies
EXTERNAL_LIBS=$(otool -L "$EXECUTABLE" | grep -E "opt/homebrew|usr/local|opt/local" | awk '{print $1}')

if [ -n "$EXTERNAL_LIBS" ]; then
    echo "External dependencies found:"
    echo "$EXTERNAL_LIBS"
    echo

    for LIB_PATH in $EXTERNAL_LIBS; do
        if [ -f "$LIB_PATH" ]; then
            LIB_NAME=$(basename "$LIB_PATH")
            if should_skip_bundle_lib "$LIB_NAME"; then
                echo "Skip: $LIB_NAME (not bundled, matches GitHub releases)"
                continue
            fi
            echo "Processing: $LIB_NAME"

            # Resolve the real library path (follow symlinks) in a macOS-compatible way
            if command -v realpath &> /dev/null; then
                REAL_LIB=$(realpath "$LIB_PATH" 2>/dev/null || echo "$LIB_PATH")
            else
                # macOS has no realpath / readlink -f; use perl
                REAL_LIB=$(perl -MCwd=abs_path -e 'print abs_path(shift)' "$LIB_PATH" 2>/dev/null || echo "$LIB_PATH")
            fi
            REAL_NAME=$(basename "$REAL_LIB")

            # Copy the real library file
            if [ ! -f "$APP_FRAMEWORKS_DIR/$REAL_NAME" ]; then
                cp "$REAL_LIB" "$APP_FRAMEWORKS_DIR/$REAL_NAME"

                # Set the library ID to the filename (no path)
                install_name_tool -id "$REAL_NAME" "$APP_FRAMEWORKS_DIR/$REAL_NAME"

                # Drop leftover rpaths that can break loading
                install_name_tool -delete_rpath "@loader_path/../lib" "$APP_FRAMEWORKS_DIR/$REAL_NAME" 2>/dev/null || true
                install_name_tool -delete_rpath "@loader_path/lib" "$APP_FRAMEWORKS_DIR/$REAL_NAME" 2>/dev/null || true
            fi

            # Intentionally not creating intermediate symlinks
            # if [ "$LIB_NAME" != "$REAL_NAME" ]; then
            #     (cd "$APP_FRAMEWORKS_DIR" && ln -sf "$REAL_NAME" "$LIB_NAME")
            # fi

            # Rewrite dependency references in the main executable
            install_name_tool -change "$LIB_PATH" "@executable_path/../Frameworks/$REAL_NAME" "$EXECUTABLE" 2>/dev/null || true
            install_name_tool -change "$REAL_LIB" "@executable_path/../Frameworks/$REAL_NAME" "$EXECUTABLE" 2>/dev/null || true
        fi
    done

    echo
    echo "Bundled libraries:"
    ls -la "$APP_FRAMEWORKS_DIR/"
else
    echo "No external libraries to bundle"
fi

# Remove leftover libzstd*.dylib from previous packaging runs
rm -f "$APP_FRAMEWORKS_DIR"/libzstd*.dylib

# Remove unused rpaths
echo
echo "Cleaning rpaths..."
install_name_tool -delete_rpath "/opt/homebrew/lib" "$EXECUTABLE" 2>/dev/null || true
install_name_tool -delete_rpath "/usr/local/lib" "$EXECUTABLE" 2>/dev/null || true
install_name_tool -delete_rpath "/opt/local/lib" "$EXECUTABLE" 2>/dev/null || true

# Replace a Resources symlink with a real copy if needed
RESOURCES_LINK="$FINAL_APP/Contents/Resources"
if [ -L "$RESOURCES_LINK" ]; then
    echo "Replacing Resources symlink with a real copy..."
    RESOURCES_TARGET=$(readlink "$RESOURCES_LINK")
    rm "$RESOURCES_LINK"
    cp -R "$RESOURCES_TARGET" "$RESOURCES_LINK"
fi

# Verify dependencies
echo
echo "Final dependencies:"
otool -L "$EXECUTABLE" | grep -E "@executable|libzstd|libsentry" || echo "No special dependencies"

if otool -L "$EXECUTABLE" | grep -qE "/opt/homebrew/.*/libzstd|/usr/local/.*/libzstd|/opt/local/.*/libzstd"; then
    echo ""
    echo "Warning: the executable still links Homebrew/MacPorts libzstd."
    echo "         This DMG will crash on machines without that library (Library not loaded)."
    echo "         GitHub packages work because CI does not link Homebrew zstd."
    echo "         Rebuild without brew zstd installed, or drop /opt/homebrew from the link path."
    echo ""
fi

# ============================================
# Step 2/6: Sign the app
# ============================================

echo
echo "=========================================="
echo "Step 2/6: Sign app"
echo "=========================================="

APP_FRAMEWORKS_DIR="$FINAL_APP/Contents/Frameworks"
APP_MACOS_DIR="$FINAL_APP/Contents/MacOS"
EXECUTABLE="$APP_MACOS_DIR/$APP_NAME"

echo "Signing mode: $SIGN_MODE ($SIGN_IDENTITY)"

# 2.1 Remove existing signature
echo "2.1 Removing existing signature..."
codesign --remove-signature "$FINAL_APP" 2>/dev/null || true

# 2.2 Sign frameworks and dylibs with the runtime option
echo "2.2 Signing frameworks and dylibs (runtime)..."
if [ -d "$APP_FRAMEWORKS_DIR" ]; then
    # Sign all .framework bundles
    for framework in "$APP_FRAMEWORKS_DIR"/*.framework; do
        if [ -d "$framework" ]; then
            echo "  - Signing: $(basename "$framework")"
            codesign_item "$framework"
        fi
    done

    # Sign all .dylib files
    for dylib in "$APP_FRAMEWORKS_DIR"/*.dylib; do
        if [ -f "$dylib" ]; then
            echo "  - Signing: $(basename "$dylib")"
            codesign_item "$dylib"
        fi
    done

    # Sign any other library files (e.g. .so)
    for lib in "$APP_FRAMEWORKS_DIR"/*.*; do
        if [ -f "$lib" ]; then
            case "$lib" in
                *.dylib) ;;  # already handled
                *)
                    echo "  - Signing: $(basename "$lib")"
                    codesign_item "$lib"
                    ;;
            esac
        fi
    done
fi

# 2.3 Sign helper tools
echo "2.3 Signing helper tools (runtime)..."
if [ -f "$APP_MACOS_DIR/crashpad_handler" ]; then
    echo "  - Signing: crashpad_handler"
    codesign_item "$APP_MACOS_DIR/crashpad_handler"
fi

# 2.4 Sign the whole app bundle (apply entitlements)
echo "2.4 Signing the app bundle (applying entitlements)..."
echo "  This signs all components and applies entitlements to the main executable"
codesign_item --entitlements "$ENTITLEMENTS" "$FINAL_APP"

# 2.5 Verify signature and entitlements
echo "2.5 Verifying signature and entitlements..."
echo "  Checking signature..."
codesign -vvv "$FINAL_APP" 2>&1 | grep -E "valid on disk|Authority|TeamIdentifier|adhoc" | head -5
echo ""
echo "  Checking entitlements..."
if codesign -d --entitlements - "$FINAL_APP" 2>&1 | grep -q "com.apple.security.cs.disable-library-validation"; then
    echo "  Entitlements embedded correctly"
else
    echo "Warning: expected entitlements not found"
fi

# ============================================
# Step 3/6: Create and sign the DMG
# Two-step flow: create a blank image -> mount at a custom path -> copy
# contents -> detach -> convert to UDZO.
# Avoid -srcfolder: macOS 26.2+ blocks file operations under
# /Volumes/Snapmaker_Orca/Snapmaker Orca.app when the volume name and
# the app name share a prefix. Mounting outside /Volumes/ bypasses that.
# ============================================

echo
echo "=========================================="
echo "Step 3/6: Create and sign DMG"
echo "=========================================="

DMG_CONTENT_DIR="$WORK_DIR/dmg_content"
rm -rf "$DMG_CONTENT_DIR"
mkdir -p "$DMG_CONTENT_DIR"
rm -rf "$DMG_CONTENT_DIR/.fseventsd" 2>/dev/null || true

# Copy the app (display name Snapmaker Orca.app) and add an Applications symlink
echo "Preparing DMG contents..."
cp -R "$FINAL_APP" "$DMG_CONTENT_DIR/$APP_NAME_EX.app"
xattr -cr "$DMG_CONTENT_DIR/$APP_NAME_EX.app" 2>/dev/null || {
    echo "  Some xattrs could not be cleared (safe to ignore)"
    find "$DMG_CONTENT_DIR/$APP_NAME_EX.app" -type f -exec sh -c 'xattr -c "$1" 2>/dev/null || true' _ {} \;
}
ln -sfn /Applications "$DMG_CONTENT_DIR/Applications"

DMG_VOLNAME="Snapmaker_Orca"
FINAL_DMG_PATH="$BUILD_DIR/$DMG_NAME"
rm -f "$FINAL_DMG_PATH"

# Size the scratch image
APP_SIZE_KB=$(du -sk "$DMG_CONTENT_DIR" | cut -f1)
IMAGE_SIZE_MB=$((APP_SIZE_KB / 1024 + 200))
echo "DMG contents: ${APP_SIZE_KB}KB, image size: ${IMAGE_SIZE_MB}MB"

# Step 1: create a blank writable image (UDIF, JHFS+)
echo "Creating blank image..."
TMP_IMG="$WORK_DIR/tmp_dmg.dmg"
hdiutil create \
    -size ${IMAGE_SIZE_MB}m \
    -volname "$DMG_VOLNAME" \
    -layout SPUD \
    -fs JHFS+ \
    -type UDIF \
    -ov \
    -o "$TMP_IMG" || {
    echo "Error: failed to create blank image"
    exit 1
}

# Step 2: mount at a custom path (not /Volumes/) to bypass the macOS 26.2 restriction
echo "Mounting image at a custom path..."
CUSTOM_MOUNT="$WORK_DIR/dmg_mount"
rm -rf "$CUSTOM_MOUNT"
mkdir -p "$CUSTOM_MOUNT"
hdiutil attach "$TMP_IMG" \
    -noverify \
    -noautoopen \
    -mountpoint "$CUSTOM_MOUNT" || {
    echo "Error: failed to mount image"
    rm -f "$TMP_IMG"
    exit 1
}

# Step 3: copy contents onto the image
echo "Copying contents onto the image..."
cp -Rp "$DMG_CONTENT_DIR"/* "$CUSTOM_MOUNT/" || {
    echo "Error: failed to copy contents"
    hdiutil detach "$CUSTOM_MOUNT" -force 2>/dev/null || true
    rm -f "$TMP_IMG"
    exit 1
}
echo "  Copied: $(du -sh "$CUSTOM_MOUNT" | cut -f1)"

# Step 4: detach
echo "Detaching image..."
hdiutil detach "$CUSTOM_MOUNT" || {
    echo "Warning: normal detach failed, force-detaching"
    hdiutil detach "$CUSTOM_MOUNT" -force 2>/dev/null || true
}
rmdir "$CUSTOM_MOUNT" 2>/dev/null || true

# Step 5: convert to compressed UDZO
echo "Compressing DMG (UDZO)..."
hdiutil convert "$TMP_IMG" \
    -format UDZO \
    -imagekey zlib-level=9 \
    -o "$FINAL_DMG_PATH" || {
    echo "Error: DMG conversion failed"
    rm -f "$TMP_IMG" "$TMP_IMG."*.dmg 2>/dev/null || true
    exit 1
}

# Remove the scratch image
rm -f "$TMP_IMG" "$TMP_IMG."*.dmg 2>/dev/null || true

[ ! -f "$FINAL_DMG_PATH" ] && echo "Error: DMG was not created" && exit 1

# Sign the DMG
echo "Signing DMG..."
if [ "$SIGN_IDENTITY" = "-" ]; then
    codesign --force --sign "$SIGN_IDENTITY" "$FINAL_DMG_PATH"
else
    codesign --force --timestamp --sign "$SIGN_IDENTITY" "$FINAL_DMG_PATH"
fi

echo "Verifying DMG signature..."
codesign -vvv "$FINAL_DMG_PATH" 2>&1 | head -3

rm -rf "$DMG_CONTENT_DIR"

echo ""
echo "=========================================="
echo "DMG created"
echo "=========================================="
echo "DMG: $FINAL_DMG_PATH"
echo "Size: $(du -h "$FINAL_DMG_PATH" | cut -f1)"

# ============================================
# Step 4/6: Notarize the DMG
# ============================================

echo ""
echo "=========================================="
echo "Step 4/6: Notarize DMG"
echo "=========================================="

echo "Checking notarization credentials..."
echo "  Apple ID: ${NOTARY_APPLE_ID:-not set}"
echo "  Team ID: ${NOTARY_TEAM_ID:-not set}"
echo "  Keychain Profile: ${NOTARY_KEYCHAIN_PROFILE:-not set}"

if [ "$ENABLE_NOTARY" -eq 0 ]; then
    echo ""
    if [ "$SIGN_IDENTITY" = "-" ]; then
        echo "CERTIFICATE_ID is not set; used ad-hoc signing. Ad-hoc cannot be notarized, skipping notarization"
    else
        echo "Notarization credentials are incomplete, skipping notarization"
        echo "Set one of the following on this machine or in CI:"
        echo "  1) export NOTARY_KEYCHAIN_PROFILE=\"snapmaker\""
        echo "  2) or set NOTARY_APPLE_ID, NOTARY_TEAM_ID, and NOTARY_PASSWORD"
    fi
else
    echo "Notarization credentials configured"
    echo ""
    echo "=========================================="
    echo "Step 5/6: Submit for notarization"
    echo "=========================================="

    echo "Submitting DMG to Apple notarization..."
    if [ -n "$NOTARY_KEYCHAIN_PROFILE" ]; then
        xcrun notarytool submit "$FINAL_DMG_PATH" \
            --keychain-profile "$NOTARY_KEYCHAIN_PROFILE" \
            --wait \
            --progress
    else
        xcrun notarytool submit "$FINAL_DMG_PATH" \
            --apple-id "$NOTARY_APPLE_ID" \
            --team-id "$NOTARY_TEAM_ID" \
            --password "$NOTARY_PASSWORD" \
            --wait \
            --progress
    fi

    echo ""
    echo "=========================================="
    echo "Step 6/6: Staple notarization ticket"
    echo "=========================================="

    echo "Stapling the notarization ticket onto the DMG..."
    xcrun stapler staple "$FINAL_DMG_PATH"

    echo ""
    echo "Validating notarization..."
    xcrun stapler validate -v "$FINAL_DMG_PATH"

    echo ""
    echo "=========================================="
    echo "Notarization complete"
    echo "=========================================="
    echo "This DMG is signed and notarized and can run on any Mac"
fi

echo ""
echo "=========================================="
echo "Done"
echo "=========================================="
echo "Arch: $ARCH"
echo "App: $FINAL_APP"
echo "DMG: $FINAL_DMG_PATH"
echo "Certificate: ${CERTIFICATE_ID:-not set}"
echo "TEAM_ID: ${NOTARY_TEAM_ID:-not set}"
echo "Signing: $SIGN_MODE"
echo "Notarization: $([ "$ENABLE_NOTARY" -eq 1 ] && echo enabled || echo skipped)"
echo ""
echo "How to use:"
echo "  1. Open the DMG: open $FINAL_DMG_PATH"
echo "  2. Drag $APP_NAME_EX.app into Applications"
echo "  3. Launch the app from Applications"
echo "=========================================="
