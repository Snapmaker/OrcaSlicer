#!/usr/bin/env bash
# Regenerate the macOS app icon artifacts under resources/:
#   - Assets.car   layered icon used by macOS 26+ (Tahoe); wired via CFBundleIconName
#   - Icon.icns    legacy fallback used on macOS 12-15 and older paths; wired via CFBundleIconFile
#
# Requires Xcode 26+ (actool for Assets.car). iconutil ships with any Xcode.
# The 1024px icon master is NOT committed to the repo (design source); the
# Icon.icns step is documented below and must be run from the design export.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "Compiling layered icon (Assets.car) from resources/AppIcon.icon..."
xcrun actool "$ROOT/resources/AppIcon.icon" --compile "$ROOT/resources" \
    --platform macosx --minimum-deployment-target 10.15 \
    --target-device mac --app-icon AppIcon

# Icon.icns regeneration (manual step):
# 1. Export 16/32/64/128/256/512/1024 px frames from the 1024px master
#    (Apple 824/1024 icon grid, 0.805 tile ratio, centered) into an iconset
#    directory: icon_16x16.png, icon_16x16@2x.png, ..., icon_512x512@2x.png.
#    Keeping the @2x pairs is mandatory - dropping them (as happened once
#    before) makes the icon change size between Dock, Cmd-Tab and Finder.
# 2. Then run:
#    iconutil -c icns "<path-to-iconset>" -o "$ROOT/resources/Icon.icns"
# 3. Verify: tests/libslic3r/test_icns.cpp asserts the container structure.
