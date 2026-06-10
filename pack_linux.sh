#!/usr/bin/env bash
#
# pack_linux.sh — Build Snapmaker_Orca Linux packages (AppImage + Flatpak)
#
# Default with no args: -F all -c -dsir -j 3 -I
#
# Branch and version:
#   ./pack_linux.sh -v "2.3.3" -b "dev_2.3.3_alves_packaged"
#   → fetch branch + force -F all -c -dsir -j 3 -I
#   → write version into source (version.inc / metainfo) and artifact names
#   → copy artifacts to $SCRIPT_DIR
#
# AppImage: host build/ compile + appimagetool
# Flatpak: full compile inside flatpak-builder sandbox
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

REPO_URL="https://github.com/Snapmaker/OrcaSlicer.git"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------- Defaults (no args = -F all -c -dsir -j 3 -I) ----------------

FORMAT="all"                # appimage | flatpak | all
ARCH="$(uname -m)"
[[ "$ARCH" == "amd64" ]] && ARCH="x86_64"

VERSION_OVERRIDE=""         # -v: artifact naming and version.inc override
GIT_BRANCH=""               # -b: GitHub branch name
JOBS_EXPLICIT=false

HOST_BUILD_DIR="build"
OUTPUT_DIR=""               # artifact output dir (-b defaults to SCRIPT_DIR)
DO_BUILD=false              # --build-if-missing
FULL_DSR=true
CLEAN_BUILD=true
JOBS="3"
KEEP_PACKAGE=false
APPIMAGETOOL_OVERRIDE=""
NO_NETWORK=false

FLATPAK_BUILD_DIR="build_flatpak_pack"
FLATPAK_CACHE_DIR=".flatpak-builder-pack"
FLATPAK_CLEANUP=false
FLATPAK_FORCE_CLEAN=false
FLATPAK_ENABLE_CCACHE=false
FLATPAK_INSTALL_RUNTIME=true
FLATPAK_USE_STUBS=true

REPO_DIR=""                 # build root (-b: $SCRIPT_DIR/$BRANCH/OrcaSlicer)

# ---------------- Help ----------------

show_help() {
    cat <<EOF
Usage: $0 [OPTIONS]

Package Snapmaker_Orca as AppImage and/or Flatpak.
Default with no args: -F all -c -dsir -j 3 -I

Version / branch (aligned with build_and_package_macos.sh):
  -v, --version VER        Release version, e.g. 2.3.3 (written to source and used
                           for artifact names; required with -b unless parsed from branch)
  -b, --branch NAME        Fetch GitHub branch, then force full pack:
                           -F all -c -dsir -j 3 -I
                           Source dir:  \$SCRIPT_DIR/\$BRANCH/OrcaSlicer
                           Output dir:  \$SCRIPT_DIR

General:
  -F, --format FORMAT      Output: appimage | flatpak | all (default: all)
  -a, --arch ARCH          Architecture: x86_64 | aarch64 (default: host)
  -j, --jobs N             Parallel jobs (default: 3)
  -h, --help               Show this help

Build:
  -c, --clean              Pass -c to build_linux.sh (clean host build cache)
  -dsir, --dsir            One-shot compile: build_linux.sh -dsr (default on)
      --no-dsir            Skip host compile; package existing binary (AppImage)
  -D, --build-dir DIR      Host build directory [default: build]
      --build-if-missing   Run build_linux.sh -s if binary is missing

AppImage:
  -k, --keep-package       Keep build/package working directory
  -A, --appimagetool PATH  Local appimagetool path
      --no-network         Do not download appimagetool/type2 runtime

Flatpak:
      --flatpak-build-dir DIR   flatpak-builder output directory
      --flatpak-cache-dir DIR   flatpak-builder state cache
      --flatpak-cleanup         Clean flatpak output dir before build
      --flatpak-force-clean     flatpak-builder --force-clean
      --flatpak-ccache          flatpak-builder --ccache
  -I, --install-runtime         Install GNOME Platform/SDK (default on)
      --no-install-runtime      Do not auto-install Flatpak runtime
      --flatpak-no-stubs        Do not inject local zstd / FindDBus stubs

Artifact names (-v or version.inc):
  Snapmaker_Orca_Linux_V<ver>.AppImage
  Snapmaker_Orca-Linux-flatpak_V<ver>_<arch>.flatpak

Examples:
  $0
  $0 -v "2.3.3" -b "dev_2.3.3_alves_packaged"
  $0 -F appimage --no-dsir
  $0 -F flatpak --flatpak-force-clean
EOF
}

# ---------------- Git repo setup (aligned with build_and_package_macos.sh) ----------------

prepare_git_repo() {
    local branch="$1"
    local work_dir="$SCRIPT_DIR/$branch"
    local repo_dir="$work_dir/OrcaSlicer"

    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}Prepare Git repository${NC}"
    echo -e "Remote:  ${GREEN}$REPO_URL${NC}"
    echo -e "Branch:  ${GREEN}$branch${NC}"
    echo -e "WorkDir: ${GREEN}$work_dir${NC}"
    echo -e "${BLUE}========================================${NC}"

    mkdir -p "$work_dir"
    cd "$work_dir"

    if [[ -d "OrcaSlicer/.git" ]]; then
        echo -e "${YELLOW}>> Repository exists, fetching and checking out branch...${NC}"
        cd OrcaSlicer
        git fetch origin
        git checkout "$branch" 2>/dev/null || git checkout -b "$branch" "origin/$branch" 2>/dev/null || {
            echo -e "${RED}Cannot checkout branch $branch; ensure it exists on remote${NC}"
            exit 1
        }
        git pull origin "$branch" || true
    else
        if [[ -d "OrcaSlicer" ]]; then
            echo -e "${RED}OrcaSlicer directory exists but is not a valid git repo; remove it and retry${NC}"
            exit 1
        fi
        echo -e "${YELLOW}>> Cloning repository...${NC}"
        git clone "$REPO_URL" OrcaSlicer
        cd OrcaSlicer
        git fetch origin
        git checkout "$branch" 2>/dev/null || git checkout -b "$branch" "origin/$branch" 2>/dev/null || {
            echo -e "${RED}Cannot checkout branch $branch; ensure it exists on remote${NC}"
            exit 1
        }
    fi

    REPO_DIR="$(pwd)"
    echo -e "${GREEN}Build directory: $REPO_DIR${NC}"
    echo ""
}

extract_version_from_branch() {
    local branch="$1"
    if [[ "$branch" =~ ([0-9]+\.[0-9]+\.[0-9]+) ]]; then
        echo "${BASH_REMATCH[1]}"
    fi
}

apply_version_override() {
    local ver="$1"
    local version_inc="$REPO_DIR/version.inc"
    local metainfo="$REPO_DIR/scripts/flatpak/io.github.Snapmaker.Snapmaker_Orca.metainfo.xml"

    if [[ ! -f "$version_inc" ]]; then
        echo -e "${RED}Missing version.inc: $version_inc${NC}"
        exit 1
    fi

    if ! [[ "$ver" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo -e "${RED}Invalid version format: $ver (expected x.y.z)${NC}"
        exit 1
    fi

    echo -e "${YELLOW}Writing version $ver → version.inc / Flatpak metainfo${NC}"
    sed -i -E "s/set\\(Snapmaker_VERSION \"[^\"]*\"\\)/set(Snapmaker_VERSION \"$ver\")/" "$version_inc"

    if [[ -f "$metainfo" ]]; then
        sed -i -E \
            -e "s/(<release version=\")[^\"]*(\")/\\1${ver}\\2/" \
            -e "s/(<p>Version )[0-9]+\\.[0-9]+\\.[0-9]+( release)/\\1${ver}\\2/" \
            "$metainfo"
    fi
}

apply_remote_branch_profile() {
    echo -e "${BLUE}Remote branch mode: forcing full pack -F all -c -dsir -j ${JOBS} -I${NC}"
    FORMAT="all"
    CLEAN_BUILD=true
    FULL_DSR=true
    FLATPAK_INSTALL_RUNTIME=true
    OUTPUT_DIR="$SCRIPT_DIR"
}

copy_artifacts_to_output_dir() {
    [[ "$OUTPUT_DIR" == "$REPO_DIR" ]] && return 0
    mkdir -p "$OUTPUT_DIR"
    if [[ "$FORMAT" != "flatpak" && -f "$REPO_DIR/$APPIMAGE_NAME" ]]; then
        cp -f "$REPO_DIR/$APPIMAGE_NAME" "$OUTPUT_DIR/$APPIMAGE_NAME"
        echo -e "${GREEN}AppImage → ${OUTPUT_DIR}/${APPIMAGE_NAME}${NC}"
    fi
    if [[ "$FORMAT" != "appimage" && -f "$REPO_DIR/$FLATPAK_BUNDLE_NAME" ]]; then
        cp -f "$REPO_DIR/$FLATPAK_BUNDLE_NAME" "$OUTPUT_DIR/$FLATPAK_BUNDLE_NAME"
        echo -e "${GREEN}Flatpak  → ${OUTPUT_DIR}/${FLATPAK_BUNDLE_NAME}${NC}"
    fi
}

# ---------------- Parse arguments ----------------

while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--version) VERSION_OVERRIDE="$2"; shift 2 ;;
        -b|--branch) GIT_BRANCH="$2"; shift 2 ;;

        -F|--format) FORMAT="$2"; shift 2 ;;
        -a|--arch) ARCH="$2"; shift 2 ;;
        -j|--jobs) JOBS="$2"; JOBS_EXPLICIT=true; shift 2 ;;

        -c|--clean) CLEAN_BUILD=true; shift ;;
        -dsir|--dsir) FULL_DSR=true; shift ;;
        --no-dsir) FULL_DSR=false; shift ;;
        -D|--build-dir|-d) HOST_BUILD_DIR="$2"; shift 2 ;;
        --build-if-missing) DO_BUILD=true; shift ;;
        -k|--keep-package) KEEP_PACKAGE=true; shift ;;
        -A|--appimagetool) APPIMAGETOOL_OVERRIDE="$2"; shift 2 ;;
        --no-network) NO_NETWORK=true; shift ;;

        --flatpak-build-dir) FLATPAK_BUILD_DIR="$2"; shift 2 ;;
        --flatpak-cache-dir) FLATPAK_CACHE_DIR="$2"; shift 2 ;;
        --flatpak-cleanup) FLATPAK_CLEANUP=true; shift ;;
        --flatpak-force-clean) FLATPAK_FORCE_CLEAN=true; shift ;;
        --flatpak-ccache) FLATPAK_ENABLE_CCACHE=true; shift ;;
        -I|--install-runtime) FLATPAK_INSTALL_RUNTIME=true; shift ;;
        --no-install-runtime) FLATPAK_INSTALL_RUNTIME=false; shift ;;
        --flatpak-no-stubs) FLATPAK_USE_STUBS=false; shift ;;

        -h|--help) show_help; exit 0 ;;
        *) echo -e "${RED}Unknown option: $1${NC}"; show_help; exit 1 ;;
    esac
done

case "$FORMAT" in
    appimage|flatpak|all) ;;
    *) echo -e "${RED}Invalid --format: $FORMAT (expected appimage|flatpak|all)${NC}"; exit 1 ;;
esac

if [[ "$ARCH" != "x86_64" && "$ARCH" != "aarch64" ]]; then
    echo -e "${RED}Unsupported architecture: $ARCH (x86_64 / aarch64 only)${NC}"
    exit 1
fi

if [[ -n "$JOBS" ]] && ! [[ "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo -e "${RED}-j/--jobs must be a positive integer: $JOBS${NC}"
    exit 1
fi

# -b mode: force full pack; default jobs to 3 unless -j was explicit
if [[ -n "$GIT_BRANCH" ]]; then
    [[ "$JOBS_EXPLICIT" == false ]] && JOBS="3"
    apply_remote_branch_profile
fi

if [[ -n "$GIT_BRANCH" && -z "$VERSION_OVERRIDE" ]]; then
    VERSION_OVERRIDE="$(extract_version_from_branch "$GIT_BRANCH" || true)"
    if [[ -z "$VERSION_OVERRIDE" ]]; then
        echo -e "${RED}-b requires -v <x.y.z> (could not parse version from branch name)${NC}"
        exit 1
    fi
    echo -e "${YELLOW}-v not set; parsed version from branch name: ${VERSION_OVERRIDE}${NC}"
fi

# ---------------- Prepare build directory ----------------

if [[ -n "$GIT_BRANCH" ]]; then
    prepare_git_repo "$GIT_BRANCH"
else
    REPO_DIR="$SCRIPT_DIR"
    OUTPUT_DIR="${OUTPUT_DIR:-$REPO_DIR}"
fi

cd "$REPO_DIR"

if [[ -n "$VERSION_OVERRIDE" ]]; then
    apply_version_override "$VERSION_OVERRIDE"
fi

# ---------------- Version and artifact names ----------------

if [[ ! -f "$REPO_DIR/version.inc" ]]; then
    echo -e "${RED}Missing version.inc${NC}"
    exit 1
fi

APP_KEY="$(grep 'set(SLIC3R_APP_KEY' "$REPO_DIR/version.inc" | cut -d '"' -f2)"
if [[ -n "$VERSION_OVERRIDE" ]]; then
    VER_PURE="$VERSION_OVERRIDE"
else
    VER_PURE="$(grep 'set(Snapmaker_VERSION' "$REPO_DIR/version.inc" | cut -d '"' -f2)"
fi
if [[ -z "$APP_KEY" || -z "$VER_PURE" ]]; then
    echo -e "${RED}Cannot parse SLIC3R_APP_KEY / Snapmaker_VERSION${NC}"
    exit 1
fi

OUTPUT_DIR="${OUTPUT_DIR:-$REPO_DIR}"

VER="V${VER_PURE}"
APP_CMD="snapmaker-orca"

APPIMAGE_NAME="Snapmaker_Orca_Linux_${VER}.AppImage"
FLATPAK_BUNDLE_NAME="Snapmaker_Orca-Linux-flatpak_${VER}_${ARCH}.flatpak"

echo -e "${BLUE}Snapmaker_Orca — Linux packaging${NC}"
echo -e "Repo:           ${GREEN}${REPO_DIR}${NC}"
[[ -n "$GIT_BRANCH" ]] && echo -e "Branch:         ${GREEN}${GIT_BRANCH}${NC}"
echo -e "Output:         ${GREEN}${OUTPUT_DIR}${NC}"
echo -e "Format:         ${GREEN}${FORMAT}${NC}"
echo -e "Version:        ${GREEN}${VER_PURE}${NC}"
echo -e "Arch:           ${GREEN}${ARCH}${NC}"
echo -e "Jobs:           ${GREEN}${JOBS}${NC}"
echo -e "Host compile:   ${GREEN}$([ "$FULL_DSR" = true ] && echo "dsir$( [ "$CLEAN_BUILD" = true ] && echo '+clean' )" || echo "skip")${NC}"
[[ "$FORMAT" != "flatpak" ]] && echo -e "AppImage:       ${GREEN}${APPIMAGE_NAME}${NC}"
[[ "$FORMAT" != "appimage" ]] && echo -e "Flatpak bundle: ${GREEN}${FLATPAK_BUNDLE_NAME}${NC}"
echo ""

# ===============================================================
# AppImage
# ===============================================================

locate_host_binary() {
    for cand in \
        "${HOST_BUILD_DIR}/src/Release/${APP_CMD}" \
        "${HOST_BUILD_DIR}/src/RelWithDebInfo/${APP_CMD}" \
        "${HOST_BUILD_DIR}/src/Debug/${APP_CMD}" \
        "${HOST_BUILD_DIR}/src/${APP_CMD}"; do
        if [[ -f "$cand" ]]; then
            echo "$cand"
            return 0
        fi
    done
    return 1
}

fetch_with_wget() {
    local url="$1"
    local out="$2"
    if [[ "$NO_NETWORK" == true ]]; then
        echo -e "${RED}Missing ${out} and --no-network is set${NC}"
        return 1
    fi
    if ! command -v wget >/dev/null 2>&1; then
        echo -e "${RED}wget not installed; cannot download ${url}${NC}"
        return 1
    fi
    echo -e "${YELLOW}Downloading: ${url}${NC}"
    wget --tries=3 --timeout=60 "$url" -O "$out"
}

pack_appimage() {
    echo -e "${BLUE}===== AppImage =====${NC}"

    if [[ "$FULL_DSR" == true ]]; then
        echo -e "${BLUE}One-shot compile: ./build_linux.sh -dsr$( [[ "$CLEAN_BUILD" == true ]] && echo ' -c' )${NC}"
        local BL_ARGS=(./build_linux.sh -d -s -r)
        [[ "$CLEAN_BUILD" == true ]] && BL_ARGS+=(-c)
        [[ -n "$JOBS" ]] && BL_ARGS+=(-j "$JOBS")
        "${BL_ARGS[@]}"
        echo ""
    fi

    local BIN_PATH
    BIN_PATH="$(locate_host_binary || true)"
    if [[ -z "$BIN_PATH" && "$DO_BUILD" == true ]]; then
        echo -e "${YELLOW}Binary not found; running ./build_linux.sh -s ...${NC}"
        local BL_ARGS=(./build_linux.sh -s)
        [[ "$CLEAN_BUILD" == true ]] && BL_ARGS+=(-c)
        [[ -n "$JOBS" ]] && BL_ARGS+=(-j "$JOBS")
        "${BL_ARGS[@]}"
        BIN_PATH="$(locate_host_binary || true)"
    fi
    if [[ -z "$BIN_PATH" ]]; then
        echo -e "${RED}Binary ${APP_CMD} not found (${HOST_BUILD_DIR}/src/{Release,RelWithDebInfo,Debug,.}/${APP_CMD})${NC}"
        echo -e "${YELLOW}Default is -dsir auto-build; use --build-if-missing or run build_linux.sh -dsi manually${NC}"
        exit 1
    fi
    echo -e "Using binary: ${GREEN}${BIN_PATH}${NC}"

    local APPIMAGETOOL_CACHE="${HOST_BUILD_DIR}/appimagetool.AppImage"
    local APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-${ARCH}.AppImage"
    local APPIMAGE_RUNTIME_CACHE="${HOST_BUILD_DIR}/appimage-runtime-${ARCH}"
    local APPIMAGE_RUNTIME_URL="https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-${ARCH}"

    mkdir -p "$HOST_BUILD_DIR"

    if [[ -n "$APPIMAGETOOL_OVERRIDE" ]]; then
        if [[ ! -f "$APPIMAGETOOL_OVERRIDE" ]]; then
            echo -e "${RED}appimagetool not found: $APPIMAGETOOL_OVERRIDE${NC}"
            exit 1
        fi
        cp -f "$APPIMAGETOOL_OVERRIDE" "$APPIMAGETOOL_CACHE"
    fi
    if [[ ! -f "$APPIMAGETOOL_CACHE" ]]; then
        fetch_with_wget "$APPIMAGETOOL_URL" "$APPIMAGETOOL_CACHE" || exit 1
    else
        echo -e "Reusing appimagetool: ${GREEN}${APPIMAGETOOL_CACHE}${NC}"
    fi
    chmod +x "$APPIMAGETOOL_CACHE"

    if [[ ! -s "$APPIMAGE_RUNTIME_CACHE" ]]; then
        fetch_with_wget "$APPIMAGE_RUNTIME_URL" "$APPIMAGE_RUNTIME_CACHE" || exit 1
    else
        echo -e "Reusing type2 runtime: ${GREEN}${APPIMAGE_RUNTIME_CACHE}${NC}"
    fi

    local APPIMAGETOOL_USE_EXTRACT=false
    if [[ -f /.dockerenv ]] || [[ ! -e /dev/fuse ]]; then
        APPIMAGETOOL_USE_EXTRACT=true
    fi

    local PACKAGE_DIR="${HOST_BUILD_DIR}/package"
    echo -e "${YELLOW}Preparing ${PACKAGE_DIR} ...${NC}"
    rm -rf "$PACKAGE_DIR"
    mkdir -p "$PACKAGE_DIR/bin"

    echo -e "${YELLOW}Copying resources/ ...${NC}"
    cp -Rf resources "$PACKAGE_DIR/resources"
    cp -f "$BIN_PATH" "$PACKAGE_DIR/bin/${APP_CMD}"
    chmod +x "$PACKAGE_DIR/bin/${APP_CMD}"

    cat > "$PACKAGE_DIR/${APP_CMD}" <<EOF
#!/bin/bash
DIR=\$(readlink -f "\$0" | xargs dirname)
export LD_LIBRARY_PATH="\$DIR/bin:\$LD_LIBRARY_PATH"
export LC_ALL=C

if [ "\$XDG_SESSION_TYPE" = "wayland" ] && [ "\$ZINK_DISABLE_OVERRIDE" != "1" ]; then
    if command -v glxinfo >/dev/null 2>&1; then
        RENDERER=\$(glxinfo | grep "OpenGL renderer string:" | sed 's/.*: //')
        if echo "\$RENDERER" | grep -qi "NVIDIA"; then
            if command -v nvidia-smi >/dev/null 2>&1; then
                DRIVER_VERSION=\$(nvidia-smi --query-gpu=driver_version --format=csv,noheader | head -n1)
                DRIVER_MAJOR=\$(echo "\$DRIVER_VERSION" | cut -d. -f1)
                [ "\$DRIVER_MAJOR" -gt 555 ] && ZINK_FORCE_OVERRIDE=1
            fi
            if [ "\$ZINK_FORCE_OVERRIDE" = "1" ]; then
                export __GLX_VENDOR_LIBRARY_NAME=mesa
                export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
                export MESA_LOADER_DRIVER_OVERRIDE=zink
                export GALLIUM_DRIVER=zink
                export WEBKIT_DISABLE_DMABUF_RENDERER=1
            fi
        fi
    fi
fi
exec "\$DIR/bin/${APP_CMD}" "\$@"
EOF
    chmod +x "$PACKAGE_DIR/${APP_CMD}"

    local ICON_SRC="resources/images/${APP_KEY}_192px.png"
    if [[ ! -f "$ICON_SRC" ]]; then
        echo -e "${RED}Icon not found: $ICON_SRC${NC}"
        exit 1
    fi

    pushd "$PACKAGE_DIR" >/dev/null
    sed -i -e 's#/usr#././#g' "bin/${APP_CMD}"
    mv "${APP_CMD}" AppRun
    chmod +x AppRun

    cp "resources/images/${APP_KEY}_192px.png" "${APP_KEY}.png"
    mkdir -p "usr/share/icons/hicolor/192x192/apps"
    cp "resources/images/${APP_KEY}_192px.png" "usr/share/icons/hicolor/192x192/apps/${APP_KEY}.png"

    cat > "${APP_KEY}.desktop" <<EOF
[Desktop Entry]
Name=${APP_KEY}
Exec=AppRun %F
Icon=${APP_KEY}
Type=Application
Categories=Utility;
MimeType=model/stl;application/vnd.ms-3mfdocument;application/prs.wavefront-obj;application/x-amf;
EOF

    local APPIMAGETOOL_ABS APPIMAGE_RUNTIME_ABS
    APPIMAGETOOL_ABS="$(cd .. && pwd)/$(basename "$APPIMAGETOOL_CACHE")"
    APPIMAGE_RUNTIME_ABS="$(cd .. && pwd)/$(basename "$APPIMAGE_RUNTIME_CACHE")"

    echo -e "${BLUE}Running appimagetool ...${NC}"
    local TOOL_ARGS=(--runtime-file "$APPIMAGE_RUNTIME_ABS" .)
    if [[ "$APPIMAGETOOL_USE_EXTRACT" == true ]]; then
        "$APPIMAGETOOL_ABS" --appimage-extract-and-run "${TOOL_ARGS[@]}"
    else
        "$APPIMAGETOOL_ABS" "${TOOL_ARGS[@]}"
    fi

    local GENERATED="${APP_KEY}-${ARCH}.AppImage"
    if [[ ! -f "$GENERATED" ]]; then
        echo -e "${RED}appimagetool did not produce $GENERATED${NC}"
        popd >/dev/null
        exit 1
    fi
    mv -f "$GENERATED" "${APPIMAGE_NAME}"
    chmod +x "${APPIMAGE_NAME}"
    popd >/dev/null

    mv -f "${PACKAGE_DIR}/${APPIMAGE_NAME}" "${REPO_DIR}/${APPIMAGE_NAME}"
    [[ "$KEEP_PACKAGE" == false ]] && rm -rf "$PACKAGE_DIR"

    echo -e "${GREEN}AppImage done: $(du -h "${REPO_DIR}/${APPIMAGE_NAME}" | cut -f1)  ${APPIMAGE_NAME}${NC}"
    echo ""
}

# ===============================================================
# Flatpak
# ===============================================================

pack_flatpak() {
    echo -e "${BLUE}===== Flatpak =====${NC}"

    local MANIFEST_REL="scripts/flatpak/io.github.Snapmaker.Snapmaker_Orca.yml"
    local MANIFEST="$REPO_DIR/$MANIFEST_REL"
    if [[ ! -f "$MANIFEST" ]]; then
        echo -e "${RED}Manifest not found: $MANIFEST_REL${NC}"
        exit 1
    fi

    if ! command -v flatpak &>/dev/null; then
        echo -e "${RED}flatpak is not installed${NC}"; exit 1
    fi
    if ! command -v flatpak-builder &>/dev/null; then
        echo -e "${RED}flatpak-builder is not installed${NC}"; exit 1
    fi

    local APP_ID RUNTIME_VER
    APP_ID="$(grep -m1 '^app-id:' "$MANIFEST" | sed -E 's/^app-id:[[:space:]]+//')"
    RUNTIME_VER="$(grep -m1 '^runtime-version:' "$MANIFEST" | sed -E 's/^runtime-version:[[:space:]]+//; s/^"//; s/"$//')"
    if [[ -z "$APP_ID" || -z "$RUNTIME_VER" ]]; then
        echo -e "${RED}Cannot parse app-id or runtime-version from manifest${NC}"
        exit 1
    fi
    echo -e "App ID:         ${GREEN}$APP_ID${NC}"
    echo -e "GNOME runtime:  ${GREEN}$RUNTIME_VER${NC}"

    local FLATPAK_JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
    echo -e "Parallel jobs:  ${GREEN}$FLATPAK_JOBS${NC}"

    local AVAILABLE_SPACE REQUIRED_KB
    AVAILABLE_SPACE="$(df "$REPO_DIR" | awk 'NR==2 {print $4}')"
    REQUIRED_KB=$((5 * 1024 * 1024))
    if [[ "${AVAILABLE_SPACE:-0}" -lt "$REQUIRED_KB" ]]; then
        echo -e "${YELLOW}Disk space may be insufficient (recommend 5GB+ free)${NC}"
        if [[ -t 0 ]]; then
            read -r -p "Continue? [y/N] " response || true
            [[ ! "${response:-}" =~ ^[Yy]$ ]] && exit 1
        fi
    fi

    if [[ "$FLATPAK_INSTALL_RUNTIME" == true ]]; then
        echo -e "${YELLOW}Installing org.gnome.Platform/Sdk //$RUNTIME_VER ...${NC}"
        flatpak install --user -y flathub "org.gnome.Platform//$RUNTIME_VER"
        flatpak install --user -y flathub "org.gnome.Sdk//$RUNTIME_VER"
    fi
    if ! flatpak info --user "org.gnome.Platform//$RUNTIME_VER" &>/dev/null; then
        echo -e "${RED}org.gnome.Platform//$RUNTIME_VER is not installed${NC}"
        echo "Use -I to auto-install, or: flatpak install --user flathub org.gnome.Platform//$RUNTIME_VER"
        exit 1
    fi
    if ! flatpak info --user "org.gnome.Sdk//$RUNTIME_VER" &>/dev/null; then
        echo -e "${RED}org.gnome.Sdk//$RUNTIME_VER is not installed${NC}"
        echo "Use -I to auto-install"
        exit 1
    fi

    if [[ "$FLATPAK_CLEANUP" == true ]]; then
        echo -e "${YELLOW}Cleaning $FLATPAK_BUILD_DIR ...${NC}"
        rm -rf "$FLATPAK_BUILD_DIR"
        if [[ "$FLATPAK_FORCE_CLEAN" == true ]]; then
            rm -rf "$FLATPAK_CACHE_DIR"
        fi
    fi
    mkdir -p "$FLATPAK_BUILD_DIR/cache" "$FLATPAK_BUILD_DIR/flatpak-builder"
    rm -rf "$FLATPAK_BUILD_DIR/build-dir"
    rm -f "$REPO_DIR/$FLATPAK_BUNDLE_NAME"

    export FLATPAK_BUILDER_N_JOBS="$FLATPAK_JOBS"

    FLATPAK_LOCAL_MANIFEST_REL="scripts/flatpak/io.github.Snapmaker.Snapmaker_Orca.local.yml"
    FLATPAK_LOCAL_MANIFEST="${REPO_DIR}/${FLATPAK_LOCAL_MANIFEST_REL}"
    FLATPAK_LOCAL_STUBS_DIR="${REPO_DIR}/scripts/flatpak/pack-local-stubs"

    cleanup_flatpak_artifacts() {
        rm -f "${FLATPAK_LOCAL_MANIFEST}"
        rm -rf "${FLATPAK_LOCAL_STUBS_DIR}"
    }

    local MANIFEST_TO_USE="$MANIFEST_REL"

    if [[ "$FLATPAK_USE_STUBS" == true ]]; then
        trap cleanup_flatpak_artifacts EXIT

        rm -f "$FLATPAK_LOCAL_MANIFEST"
        awk '
        /^  - name:/ {
            mod = $0
            sub(/^  - name:[[:space:]]*/, "", mod)
            gsub(/[[:space:]]+$/, "", mod)
            in_snapmaker = (mod == "Snapmaker_Orca")
        }
        {
            if (in_snapmaker && $0 ~ /^[[:space:]]+CXXFLAGS=-std=gnu\+\+20 cmake \. -B build_flatpak \\$/) {
                print $0
                print "          -DCMAKE_MODULE_PATH=\"/run/build/Snapmaker_Orca/scripts/flatpak/pack-local-stubs\" \\"
                next
            }
            if (in_snapmaker && $0 ~ /^[[:space:]]+-DCMAKE_PREFIX_PATH=\/app \\$/) {
                print "          -DCMAKE_PREFIX_PATH=\"/run/build/Snapmaker_Orca/scripts/flatpak/pack-local-stubs/zstd-stub;/app\" \\"
                next
            }
            if (in_snapmaker && $0 ~ /^[[:space:]]+-DCMAKE_INSTALL_PREFIX=\/app$/) {
                print "          -DCMAKE_INSTALL_PREFIX=/app \\"
                print "          -DCMAKE_POLICY_DEFAULT_CMP0167=OLD \\"
                print "          -DBoost_NO_BOOST_CMAKE=ON"
                next
            }
            print $0
        }
        ' "$MANIFEST_REL" > "$FLATPAK_LOCAL_MANIFEST"
        MANIFEST_TO_USE="$FLATPAK_LOCAL_MANIFEST_REL"

        local LOCAL_ZSTD_STUB_DIR="$FLATPAK_LOCAL_STUBS_DIR/zstd-stub"
        local LOCAL_FIND_DBUS_FILE="$FLATPAK_LOCAL_STUBS_DIR/FindDBus.cmake"
        mkdir -p "$FLATPAK_LOCAL_STUBS_DIR" "$LOCAL_ZSTD_STUB_DIR"

        cat > "$LOCAL_ZSTD_STUB_DIR/zstdConfig.cmake" <<'EOF'
# Minimal zstd CMake package for environments where libzstd exists but zstdConfig.cmake is missing.
include_guard(GLOBAL)

find_path(ZSTD_INCLUDE_DIR NAMES zstd.h
  PATHS /usr/include
)

find_library(ZSTD_LIBRARY
  NAMES zstd libzstd
  PATHS /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu /usr/lib /usr/lib64
)

if(NOT ZSTD_INCLUDE_DIR OR NOT ZSTD_LIBRARY)
  set(zstd_FOUND FALSE)
  if(${CMAKE_FIND_PACKAGE_NAME}_FIND_REQUIRED)
    message(FATAL_ERROR "zstd stub: could not find libzstd (install libzstd in SDK)")
  endif()
  return()
endif()

if(NOT TARGET zstd::libzstd_shared)
  add_library(zstd::libzstd_shared UNKNOWN IMPORTED)
  set_target_properties(zstd::libzstd_shared PROPERTIES
    IMPORTED_LOCATION "${ZSTD_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIR}")
endif()

if(NOT TARGET zstd::libzstd_static)
  add_library(zstd::libzstd_static UNKNOWN IMPORTED)
  set_target_properties(zstd::libzstd_static PROPERTIES
    IMPORTED_LOCATION "${ZSTD_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIR}")
endif()

set(zstd_FOUND TRUE)
set(zstd_VERSION "1.5.6")
EOF

        cat > "$LOCAL_FIND_DBUS_FILE" <<'EOF'
include(FindPackageHandleStandardArgs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PC_DBUS QUIET dbus-1)

find_path(DBUS_INCLUDE_DIR
    NAMES dbus/dbus.h
    HINTS ${PC_DBUS_INCLUDEDIR} ${PC_DBUS_INCLUDE_DIRS}
    PATH_SUFFIXES dbus-1.0
)

find_path(DBUS_ARCH_INCLUDE_DIR
    NAMES dbus/dbus-arch-deps.h
    HINTS ${PC_DBUS_LIBDIR} ${PC_DBUS_LIBRARY_DIRS}
    PATHS /usr/lib/x86_64-linux-gnu/dbus-1.0/include /usr/lib/aarch64-linux-gnu/dbus-1.0/include
)

find_library(DBUS_LIBRARIES
    NAMES dbus-1
    HINTS ${PC_DBUS_LIBDIR} ${PC_DBUS_LIBRARY_DIRS}
)

set(DBUS_INCLUDE_DIRS ${DBUS_INCLUDE_DIR} ${DBUS_ARCH_INCLUDE_DIR})

find_package_handle_standard_args(DBus
    REQUIRED_VARS DBUS_LIBRARIES DBUS_INCLUDE_DIRS
)

mark_as_advanced(DBUS_INCLUDE_DIR DBUS_ARCH_INCLUDE_DIR DBUS_LIBRARIES)
EOF
        echo -e "${BLUE}Using local temporary manifest + stubs (zstd / FindDBus)${NC}"
    else
        echo -e "${BLUE}Using original manifest (no stubs injected)${NC}"
    fi

    local BUILDER_ARGS=(
        --arch="$ARCH"
        --user
        --install-deps-from=flathub
        --repo="$FLATPAK_BUILD_DIR/repo"
        --verbose
        --state-dir="$FLATPAK_CACHE_DIR"
        --jobs="$FLATPAK_JOBS"
    )
    [[ "$FLATPAK_FORCE_CLEAN" == true ]] && BUILDER_ARGS+=(--force-clean)
    [[ "$FLATPAK_ENABLE_CCACHE" == true ]] && BUILDER_ARGS+=(--ccache)

    echo -e "${BLUE}Running flatpak-builder (sandbox compile; often 30+ minutes)...${NC}"
    flatpak-builder "${BUILDER_ARGS[@]}" "$FLATPAK_BUILD_DIR/build-dir" "$MANIFEST_TO_USE"

    if [[ ! -d "$FLATPAK_BUILD_DIR/repo" ]]; then
        echo -e "${YELLOW}No $FLATPAK_BUILD_DIR/repo found; running flatpak build-export...${NC}"
        mkdir -p "$FLATPAK_BUILD_DIR/repo"
        flatpak build-export "$FLATPAK_BUILD_DIR/repo" "$FLATPAK_BUILD_DIR/build-dir"
    fi

    echo -e "${YELLOW}Building Flatpak bundle: $FLATPAK_BUNDLE_NAME${NC}"
    flatpak build-bundle "$FLATPAK_BUILD_DIR/repo" "$FLATPAK_BUNDLE_NAME" "$APP_ID" --arch="$ARCH"

    echo -e "${GREEN}Flatpak done: $(du -h "$FLATPAK_BUNDLE_NAME" | cut -f1)  $FLATPAK_BUNDLE_NAME${NC}"
    echo ""
}

# ===============================================================
# Dispatch
# ===============================================================

if [[ "$FORMAT" == "appimage" || "$FORMAT" == "all" ]]; then
    pack_appimage
fi
if [[ "$FORMAT" == "flatpak" || "$FORMAT" == "all" ]]; then
    pack_flatpak
fi

copy_artifacts_to_output_dir

echo -e "${GREEN}=========== All done ===========${NC}"
if [[ "$FORMAT" != "flatpak" ]]; then
    echo -e "AppImage:  ${GREEN}$(du -h "${OUTPUT_DIR}/${APPIMAGE_NAME}" 2>/dev/null | cut -f1 || echo '-')${NC}  ${OUTPUT_DIR}/${APPIMAGE_NAME}"
    echo -e "Run:       ${BLUE}chmod +x ./${APPIMAGE_NAME} && ./${APPIMAGE_NAME}${NC}"
fi
if [[ "$FORMAT" != "appimage" ]]; then
    echo -e "Flatpak:   ${GREEN}$(du -h "${OUTPUT_DIR}/${FLATPAK_BUNDLE_NAME}" 2>/dev/null | cut -f1 || echo '-')${NC}  ${OUTPUT_DIR}/${FLATPAK_BUNDLE_NAME}"
    echo -e "Install:   ${BLUE}flatpak install --user ./${FLATPAK_BUNDLE_NAME}${NC}"
    echo -e "Run:       ${BLUE}flatpak run io.github.Snapmaker.Snapmaker_Orca${NC}"
fi
