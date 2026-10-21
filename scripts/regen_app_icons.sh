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
# 1. Export 16/32/64/128/256/512/1024 px frames from the 1024px master into an
#    iconset directory: icon_16x16.png, icon_16x16@2x.png, ..., icon_512x512@2x.png.
#    HARD REQUIREMENT: the tile (dark rounded rect) must measure exactly
#    824x824 px on the 1024 frame and be centered on the canvas (Apple
#    824/1024 icon grid). A previous regen drifted to a ~748px tile (73%
#    canvas, ~22px above center) because the tile was drawn around an
#    under-scaled glyph - the geometry check below fails loudly on that.
#    Keeping the @2x pairs is mandatory - dropping them (as happened once
#    before) makes the icon change size between Dock, Cmd-Tab and Finder.
# 2. Then run:
#    iconutil -c icns "<path-to-iconset>" -o "$ROOT/resources/Icon.icns"
# 3. Keep the twin in sync (document-type icons reference the same file):
cp "$ROOT/resources/Icon.icns" "$ROOT/resources/images/Snapmaker_Orca.icns"
# 4. Geometry self-check (fails the script on grid/size/centering drift).
if ! python3 -c 'import PIL' 2>/dev/null; then
    echo "WARNING: python3/PIL not available, skipping icns geometry check."
else
    python3 - "$ROOT/resources/Icon.icns" << 'PYEOF'
import struct, io, sys
from PIL import Image

data = open(sys.argv[1], 'rb').read()
assert data[:4] == b'icns', 'not an icns file'
total = struct.unpack('>I', data[4:8])[0]
assert total == len(data), 'icns length mismatch'
chunks, off = {}, 8
while off < total:
    typ = data[off:off+4].decode('latin1')
    ln = struct.unpack('>I', data[off+4:off+8])[0]
    chunks[typ] = data[off+8:off+ln]
    off += ln

errors = []
for typ, canvas in (('ic10', 1024), ('ic09', 512), ('ic08', 256)):
    if typ not in chunks:
        errors.append(f'{typ}: chunk missing')
        continue
    img = Image.open(io.BytesIO(chunks[typ])).convert('RGBA')
    w, h = img.size
    px = img.load()
    dark = [(x, y) for y in range(h) for x in range(w)
            if px[x, y][3] > 128 and px[x, y][0] < 60 and px[x, y][1] < 60 and px[x, y][2] < 60]
    xs = [p[0] for p in dark]; ys = [p[1] for p in dark]
    x0, y0, x1, y1 = min(xs), min(ys), max(xs), max(ys)
    tw, th = x1 - x0 + 1, y1 - y0 + 1
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    target = int(canvas * 824 / 1024)
    if abs(tw - target) > canvas * 0.01 or abs(th - target) > canvas * 0.01:
        errors.append(f'{typ}: tile {tw}x{th}, expected ~{target}x{target} (Apple 824/1024 grid)')
    if abs(cx - w / 2) > canvas * 0.01 or abs(cy - h / 2) > canvas * 0.01:
        errors.append(f'{typ}: tile center ({cx:.1f},{cy:.1f}) off canvas center ({w/2},{h/2})')

if errors:
    print('ICNS GEOMETRY CHECK FAILED:')
    for e in errors:
        print('  -', e)
    sys.exit(1)
print('ICNS geometry OK: tile on the Apple 824/1024 grid, centered.')
PYEOF
fi
