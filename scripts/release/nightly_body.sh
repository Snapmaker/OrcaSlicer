#!/usr/bin/env bash
# Writes the body of the `nightly` pre-release to stdout (.github/workflows/nightly.yml, docs/nightly.md).
#
#   scripts/release/nightly_body.sh <dist_dir>
#
# Environment:
#   FULL_VERSION     2.4.1.0-nightly.20260926+abc1234
#   NIGHTLY_SHA      the full commit the build was made from
#   STABLE_TAG       the latest stable release's tag (e.g. v2.4.1.0-edge); empty if there is none
#   REPO             owner/name (default aceRage/EdgeSlicer)
#   ULTRANET_WINDOWS yes/no  the Windows package carries the network plug-in
#   ULTRANET_POSIX   yes/no  the macOS / Linux packages carry it
#   MAC_SIGNING      none / signed / notarized
#
# The two HTML comments at the end are read by machines: nightly-version by the app's update check
# (src/slic3r/Utils/AppUpdateCheck.cpp), nightly-commit by the workflow's "has main changed" test.
# Keep their spelling.
set -euo pipefail

DIST=${1:?usage: nightly_body.sh <dist_dir>}
REPO=${REPO:-aceRage/EdgeSlicer}
: "${FULL_VERSION:?}" "${NIGHTLY_SHA:?}"
STABLE_TAG=${STABLE_TAG:-}
short=${NIGHTLY_SHA:0:7}
date_part=${FULL_VERSION#*-nightly.}
date_part=${date_part%%[+-]*}
date_iso="${date_part:0:4}-${date_part:4:2}-${date_part:6:2}"
url="https://github.com/$REPO"

cat <<EOF
> [!WARNING]
> **Untested nightly build.** Made automatically from \`main\` at $short on $date_iso and published without any
> manual testing. It may be broken, may change your settings in ways a release would not, and is not supported.
> For everyday printing use the [latest stable release]($url/releases/latest).

<!-- update-notice -->
Untested nightly build of EdgeSlicer, made automatically from main at $short on $date_iso. It may be broken: keep your projects backed up. To go back, set Preferences > General > Update channel to Stable and install the latest release over this one.
<!-- /update-notice -->

# EdgeSlicer nightly $FULL_VERSION

| | |
|---|---|
| Version | \`$FULL_VERSION\` |
| Commit | [\`$short\`]($url/commit/$NIGHTLY_SHA) |
| Built | $date_iso (UTC) |
EOF
if [ -n "$STABLE_TAG" ]; then
    echo "| Latest stable | [$STABLE_TAG]($url/releases/tag/$STABLE_TAG) |"
fi

echo
if [ -n "$STABLE_TAG" ]; then
    echo "## Changes since $STABLE_TAG"
    echo
    # One line per pull request merged into main since the stable tag: GitHub's merge commits say
    # "Merge pull request #N from ..." and carry the PR title as the first line of their body.
    n=0
    while IFS= read -r c; do
        subject=$(git log -1 --format=%s "$c")
        pr=$(printf '%s' "$subject" | sed -n 's/^Merge pull request #\([0-9][0-9]*\) .*/\1/p')
        [ -n "$pr" ] || continue
        title=$(git log -1 --format=%b "$c" | sed -n '/[^[:space:]]/{p;q;}' | tr -d '\r')
        [ -n "$title" ] || title=$subject
        # Titles are text: keep a '<' from opening an HTML tag.
        title=$(printf '%s' "$title" | sed 's/</\&lt;/g')
        echo "- $title (#$pr)"
        n=$((n + 1))
    done < <(git rev-list --first-parent --merges "$STABLE_TAG..$NIGHTLY_SHA" 2>/dev/null | head -150)
    if [ "$n" = 0 ]; then
        echo "No pull requests merged since $STABLE_TAG ([compare]($url/compare/$STABLE_TAG...$NIGHTLY_SHA))."
    else
        echo
        echo "[Full comparison]($url/compare/$STABLE_TAG...$NIGHTLY_SHA)"
    fi
    echo
fi

cat <<'EOF'
## Downloads

EOF
echo "| Platform | File |"
echo "|---|---|"
row() { # <label> <file name pattern>
    local f
    while IFS= read -r f; do
        echo "| $1 | [$(basename "$f")]($url/releases/download/nightly/$(basename "$f")) |"
    done < <(find "$DIST" -maxdepth 1 -type f -name "$2" | sort)
}
row "Windows installer" "EdgeSlicer_Windows_Installer_*.exe"
row "Windows portable" "EdgeSlicer_Windows_*_portable.zip"
row "macOS (Apple silicon and Intel)" "EdgeSlicer_Mac_universal_*.dmg"
row "Linux AppImage (Ubuntu 24.04)" "EdgeSlicer_Linux_AppImage_*.AppImage"
row "Linux Flatpak x86_64" "EdgeSlicer_Linux_flatpak_*_x86_64.flatpak"
row "Linux Flatpak aarch64" "EdgeSlicer_Linux_flatpak_*_aarch64.flatpak"
row "Checksums" "SHA256SUMS"
echo

echo "## What this build does not have"
echo
if ! compgen -G "$DIST/EdgeSlicer_Linux_flatpak_*" >/dev/null; then
    echo "- **No Flatpak tonight**: the Flatpak build failed or was skipped; the other packages are complete."
fi
if [ "${ULTRANET_WINDOWS:-no}" != yes ]; then
    echo "- **Windows: no Bambu network plug-in.** Bambu LAN printing needs it; install a stable release for that."
fi
if [ "${ULTRANET_POSIX:-no}" != yes ]; then
    echo "- **macOS / Linux: no Bambu network plug-in.** Bambu LAN printing needs it; install a stable release for that."
fi
# FlashNetwork and the bundled ffmpeg are third-party binaries the release builds take from the
# owner's build machine; the hosted CI that makes the nightly has no copy (docs/nightly.md).
echo "- **Windows: no FlashForge network library** (FlashNetwork.dll), so FlashForge printers cannot be driven from the Device tab. Stable releases include it."
echo "- **Windows: no bundled ffmpeg**, so the phone camera Quality steps other than the Bambu frame rate are unavailable. Stable releases include it."
case "${MAC_SIGNING:-none}" in
    notarized) ;;
    signed) echo "- **macOS: signed but not notarized.** Gatekeeper may refuse it; right-click > Open, or use a stable release." ;;
    *) echo "- **macOS: not signed.** Gatekeeper refuses it; right-click > Open (or \`xattr -cr /Applications/EdgeSlicer.app\`), or use a stable release." ;;
esac
echo

cat <<EOF
## Going back to stable

1. Preferences > General > **Update channel**: set it back to **Stable**, so EdgeSlicer stops offering nightlies.
2. Download the [latest stable release]($url/releases/latest) and install it over this build (Windows installer, or
   replace the app on macOS / the AppImage on Linux). Your settings, presets and printers stay where they are.

Nightly builds use the same settings folder as the stable release, and a nightly may save settings an older
release does not understand. Before trying one, copy that folder somewhere safe (Help > Show Configuration Folder)
so you can put it back. More: [docs/nightly.md]($url/blob/main/docs/nightly.md).

<!-- nightly-version: $FULL_VERSION -->
<!-- nightly-commit: $NIGHTLY_SHA -->
EOF
